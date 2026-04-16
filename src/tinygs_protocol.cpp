#include "tinygs_protocol.h"
#include <RadioLib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinygs_config.h"
#include <zephyr/sys/heap_listener.h>

extern int read_vbat_mv(void);
extern int64_t get_utc_epoch(void);
/* Radio pointer type must match main.cpp's DTS-selected type */
#define LORA_NODE DT_ALIAS(lora0)
#if DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1262)
extern SX1262 *radio;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1268)
extern SX1268 *radio;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1276)
extern SX1276 *radio;
#elif DT_NODE_HAS_COMPAT(LORA_NODE, semtech_sx1278)
extern SX1278 *radio;
#endif

static uint32_t get_free_heap(void)
{
#ifdef CONFIG_SYS_HEAP_RUNTIME_STATS
    extern struct k_heap _system_heap;
    struct sys_memory_stats stats;
    if (sys_heap_runtime_stats_get(&_system_heap.heap, &stats) == 0) {
        return (uint32_t)stats.free_bytes;
    }
#endif
    return 0;
}
#include <zephyr/sys/base64.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

LOG_MODULE_REGISTER(tinygs_proto, LOG_LEVEL_INF);

/**
 * Escape a JSON string: replace " with \" and \ with \\.
 * Returns bytes written (excluding null), or required size if dst is NULL.
 */
static size_t json_escape(char *dst, size_t dstlen, const char *src)
{
    size_t n = 0;
    while (*src) {
        if (*src == '"' || *src == '\\') {
            if (dst && n + 2 < dstlen) { dst[n] = '\\'; dst[n+1] = *src; }
            n += 2;
        } else {
            if (dst && n + 1 < dstlen) { dst[n] = *src; }
            n += 1;
        }
        src++;
    }
    if (dst && n < dstlen) dst[n] = '\0';
    return n;
}

/* Station location — defaults to Sydney, updated by set_pos_prm */
float tinygs_station_lat = -33.8688f;
float tinygs_station_lon = 151.2093f;
float tinygs_station_alt = 50.0f;

/* Radio state — updated by begine/batch_conf, used in send_rx/send_status */
struct tinygs_radio_state tinygs_radio = {
    .modem_mode = "LoRa",
    .frequency = TINYGS_DEFAULT_FREQ,
    .freq_offset = 0.0f,
    .freq_doppler = 0.0f,
    .sf = 10,
    .bw = 250.0f,
    .cr = 5,
    .satellite = "",
    .norad = 0,
    .tle = {0},
    .tle_valid = false,
    .doppler_enabled = true,
    .doppler_tol = 1200.0f, /* Hz — retune when delta exceeds this */
    .filter = {0},
    .filter_len = 0,
    .modem_conf = "{}",
};

/* Shared buffers for topic and payload construction */
static char topic_buf[128];
static char payload_buf[768];

int tinygs_build_welcome(char *buf, size_t buflen,
                          const char *mac, int vbat_mv, uint32_t free_mem,
                          uint32_t uptime_s)
{
    const char *ip_str = "0.0.0.0";

    /* Escape modem_conf for embedding as a JSON string value */
    static char escaped_conf[384]; /* modem_conf(256) + escape overhead */
    json_escape(escaped_conf, sizeof(escaped_conf), tinygs_radio.modem_conf);

    return snprintf(buf, buflen,
        "{"
        "\"station_location\":[%.4f,%.4f],"
        "\"version\":%u,"
        "\"git_version\":\"%s\","
        "\"chip\":\"%s\","
        "\"board\":%d,"
        "\"mac\":\"%s\","
        "\"radioChip\":%d,"
        "\"Mem\":%u,"
        "\"seconds\":%u,"
        "\"Vbat\":%d,"
        "\"tx\":false,"
        "\"sat\":\"%s\","
        "\"ip\":\"%s\","
        "\"idfv\":\"NCS/Zephyr\","
        "\"modem_conf\":\"%s\""
        "}",
        (double)tinygs_station_lat,
        (double)tinygs_station_lon,
        (unsigned)TINYGS_VERSION,
        TINYGS_GIT_VERSION,
        TINYGS_CHIP,
        TINYGS_BOARD,
        mac,
        TINYGS_RADIO_CHIP,
        (unsigned)free_mem,
        (unsigned)uptime_s,
        vbat_mv,
        tinygs_radio.satellite,
        ip_str,
        escaped_conf
    );
}

int tinygs_build_ping(char *buf, size_t buflen,
                       int vbat_mv, uint32_t free_mem, uint32_t min_mem,
                       int radio_error, float inst_rssi)
{
    /* Get Thread parent RSSI — ESP32 uses WiFi.RSSI() here.
     * We use Thread parent RSSI for both RSSI and InstRSSI fields. */
    int8_t thread_rssi = 0;
    struct openthread_context *ot_ctx = openthread_get_default_context();
    if (ot_ctx) {
        openthread_api_mutex_lock(ot_ctx);
        otThreadGetParentAverageRssi(ot_ctx->instance, &thread_rssi);
        openthread_api_mutex_unlock(ot_ctx);
    }
    inst_rssi = (float)thread_rssi; /* Use Thread RSSI instead of LoRa radio RSSI */

    return snprintf(buf, buflen,
        "{"
        "\"Vbat\":%d,"
        "\"Mem\":%u,"
        "\"MinMem\":%u,"
        "\"MaxBlk\":%u,"
        "\"RSSI\":%d,"
        "\"radio\":%d,"
        "\"InstRSSI\":%.1f"
        "}",
        vbat_mv,
        (unsigned)free_mem,
        (unsigned)min_mem,
        (unsigned)free_mem,
        (int)thread_rssi,
        radio_error,
        (double)inst_rssi
    );
}

int tinygs_subscribe(struct mqtt_client *client,
                      const char *user, const char *station)
{
    int ret;

    /* Subscribe to global commands */
    struct mqtt_topic topics[2];
    struct mqtt_subscription_list sub_list;

    topics[0].topic.utf8 = (uint8_t *)TINYGS_TOPIC_GLOBAL;
    topics[0].topic.size = strlen(TINYGS_TOPIC_GLOBAL);
    topics[0].qos = MQTT_QOS_0_AT_MOST_ONCE;

    /* Build station command topic */
    snprintf(topic_buf, sizeof(topic_buf), TINYGS_TOPIC_CMND, user, station);
    topics[1].topic.utf8 = (uint8_t *)topic_buf;
    topics[1].topic.size = strlen(topic_buf);
    topics[1].qos = MQTT_QOS_0_AT_MOST_ONCE;

    sub_list.list = topics;
    sub_list.list_count = 2;
    sub_list.message_id = 1;

    ret = mqtt_subscribe(client, &sub_list);
    if (ret == 0) {
        LOG_INF("Subscribed: %s", TINYGS_TOPIC_GLOBAL);
        LOG_INF("Subscribed: %s", topic_buf);
    } else {
        LOG_ERR("Subscribe failed: %d", ret);
    }

    return ret;
}

int tinygs_send_welcome(struct mqtt_client *client,
                         const char *user, const char *station,
                         const char *mac)
{
    /* Build topic */
    tinygs_build_topic(topic_buf, sizeof(topic_buf),
                        TINYGS_TOPIC_TELE, TINYGS_TELE_WELCOME,
                        user, station);

    int len = tinygs_build_welcome(payload_buf, sizeof(payload_buf),
                                    mac, read_vbat_mv(), get_free_heap(),
                                    k_uptime_get_32() / 1000);

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload_buf;
    param.message.payload.len = len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    if (len >= (int)sizeof(payload_buf)) {
        LOG_WRN("Welcome payload truncated (%d >= %zu)", len, sizeof(payload_buf));
    }

    LOG_INF("Publishing welcome to %s", topic_buf);
    LOG_INF("Payload: %s", payload_buf);

    return mqtt_publish(client, &param);
}

int tinygs_send_ping(struct mqtt_client *client,
                      const char *user, const char *station)
{
    tinygs_build_topic(topic_buf, sizeof(topic_buf),
                        TINYGS_TOPIC_TELE, TINYGS_TELE_PING,
                        user, station);

    int len = tinygs_build_ping(payload_buf, sizeof(payload_buf),
                                 read_vbat_mv(), get_free_heap(), get_free_heap(), 0,
                                 -120.0f);  /* Last pkt RSSI — ESP32 uses WiFi.RSSI() here, not radio */

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload_buf;
    param.message.payload.len = len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    if (len >= (int)sizeof(payload_buf)) {
        LOG_WRN("Ping payload truncated (%d >= %zu)", len, sizeof(payload_buf));
    }

    LOG_DBG("Ping: %s", payload_buf);

    return mqtt_publish(client, &param);
}

int tinygs_send_rx(struct mqtt_client *client,
                    const char *user, const char *station,
                    const uint8_t *data, size_t data_len,
                    float rssi, float snr, float freq_err,
                    bool crc_error)
{
    tinygs_build_topic(topic_buf, sizeof(topic_buf),
                        TINYGS_TOPIC_TELE, TINYGS_TELE_RX,
                        user, station);

    /* Base64 encode the packet data */
    static char b64_buf[400]; /* 255 bytes → ~340 base64 chars */
    size_t b64_len = 0;
    int ret = base64_encode((uint8_t *)b64_buf, sizeof(b64_buf) - 1,
                             &b64_len, data, data_len);
    if (ret != 0) {
        LOG_ERR("base64 encode failed: %d", ret);
        return -1;
    }
    b64_buf[b64_len] = '\0';

    /* Get real epoch time (not uptime) — server expects Unix timestamp */
    uint32_t epoch = (uint32_t)get_utc_epoch();

    /* Build RX JSON payload — field set depends on LoRa vs FSK mode */
    bool is_fsk = (strcmp(tinygs_radio.modem_mode, "FSK") == 0);
    int len;

    if (!is_fsk) {
        len = snprintf(payload_buf, sizeof(payload_buf),
            "{"
            "\"station_location\":[%.4f,%.4f],"
            "\"mode\":\"LoRa\","
            "\"frequency\":%.4f,"
            "\"frequency_offset\":%.1f,"
            "\"satellite\":\"%s\","
            "\"sf\":%d,"
            "\"cr\":%d,"
            "\"bw\":%.1f,"
            "\"rssi\":%.1f,"
            "\"snr\":%.2f,"
            "\"frequency_error\":%.1f,"
            "\"unix_GS_time\":%u,"
            "\"usec_time\":%lld,"
            "\"crc_error\":%s,"
            "\"data\":\"%s\","
            "\"NORAD\":%u,"
            "\"noisy\":%s,"
            "\"iIQ\":%s",
            (double)tinygs_station_lat, (double)tinygs_station_lon,
            (double)tinygs_radio.frequency, (double)tinygs_radio.freq_offset,
            tinygs_radio.satellite,
            tinygs_radio.sf, tinygs_radio.cr, (double)tinygs_radio.bw,
            (double)rssi, (double)snr, (double)freq_err,
            (unsigned)epoch,
            (long long)(k_uptime_get() * 1000LL),
            crc_error ? "true" : "false",
            b64_buf, (unsigned)tinygs_radio.norad,
            crc_error ? "true" : "false",
            tinygs_radio.iIQ ? "true" : "false"
        );
        /* Append f_doppler when Doppler compensation is active */
        if (tinygs_radio.freq_doppler != 0.0f && len < (int)sizeof(payload_buf) - 30) {
            len += snprintf(payload_buf + len, sizeof(payload_buf) - len,
                            ",\"f_doppler\":%.1f", (double)tinygs_radio.freq_doppler);
        }
        /* Close JSON */
        if (len < (int)sizeof(payload_buf) - 1) {
            payload_buf[len++] = '}';
            payload_buf[len] = '\0';
        }
    } else {
        len = snprintf(payload_buf, sizeof(payload_buf),
            "{"
            "\"station_location\":[%.4f,%.4f],"
            "\"mode\":\"FSK\","
            "\"frequency\":%.4f,"
            "\"frequency_offset\":%.1f,"
            "\"satellite\":\"%s\","
            "\"bitrate\":%.0f,"
            "\"freqdev\":%.0f,"
            "\"rxBw\":%.1f,"
            "\"rssi\":%.1f,"
            "\"snr\":%.2f,"
            "\"frequency_error\":%.1f,"
            "\"unix_GS_time\":%u,"
            "\"usec_time\":%lld,"
            "\"crc_error\":%s,"
            "\"data\":\"%s\","
            "\"data_raw\":\"%s\","
            "\"NORAD\":%u,"
            "\"noisy\":%s"
            "}",
            (double)tinygs_station_lat, (double)tinygs_station_lon,
            (double)tinygs_radio.frequency, (double)tinygs_radio.freq_offset,
            tinygs_radio.satellite,
            (double)tinygs_radio.bitrate, (double)tinygs_radio.freq_dev,
            (double)tinygs_radio.bw,
            (double)rssi, (double)snr, (double)freq_err,
            (unsigned)epoch,
            (long long)(k_uptime_get() * 1000LL),
            crc_error ? "true" : "false",
            b64_buf, b64_buf, /* data and data_raw are the same for now */
            (unsigned)tinygs_radio.norad,
            crc_error ? "true" : "false"
        );
    }

    if (len >= (int)sizeof(payload_buf)) {
        LOG_ERR("RX payload truncated: %d >= %zu", len, sizeof(payload_buf));
        return -1;
    }

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload_buf;
    param.message.payload.len = len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    LOG_INF("Publishing RX packet (%zu bytes) to %s", data_len, topic_buf);

    return mqtt_publish(client, &param);
}

int tinygs_send_status(struct mqtt_client *client,
                        const char *user, const char *station)
{
    tinygs_build_topic(topic_buf, sizeof(topic_buf),
                        TINYGS_TOPIC_STAT, TINYGS_STAT_STATUS,
                        user, station);

    bool is_fsk = (strcmp(tinygs_radio.modem_mode, "FSK") == 0);
    int len;

    if (!is_fsk) {
        len = snprintf(payload_buf, sizeof(payload_buf),
            "{"
            "\"station_location\":[%.4f,%.4f],"
            "\"version\":%u,"
            "\"board\":%d,"
            "\"tx\":false,"
            "\"mode\":\"LoRa\","
            "\"frequency\":%.4f,"
            "\"frequency_offset\":%.1f,"
            "\"satellite\":\"%s\","
            "\"sf\":%d,"
            "\"cr\":%d,"
            "\"bw\":%.1f,"
            "\"pl\":%d,"
            "\"CRC\":%s,"
            "\"FLDRO\":%d,"
            "\"NORAD\":%u,"
            "\"rssi\":%.1f,"
            "\"snr\":%.2f,"
            "\"frequency_error\":%.1f,"
            "\"crc_error\":%s,"
            "\"unix_GS_time\":%u"
            "}",
            (double)tinygs_station_lat, (double)tinygs_station_lon,
            (unsigned)TINYGS_VERSION, TINYGS_BOARD,
            (double)tinygs_radio.frequency, (double)tinygs_radio.freq_offset,
            tinygs_radio.satellite,
            tinygs_radio.sf, tinygs_radio.cr, (double)tinygs_radio.bw,
            tinygs_radio.pl,
            tinygs_radio.crc_on ? "true" : "false",
            tinygs_radio.fldro,
            (unsigned)tinygs_radio.norad,
            (double)tinygs_radio.last_rssi,
            (double)tinygs_radio.last_snr,
            (double)tinygs_radio.last_freq_err,
            tinygs_radio.last_crc_error ? "true" : "false",
            (unsigned)(uint32_t)get_utc_epoch()
        );
    } else {
        len = snprintf(payload_buf, sizeof(payload_buf),
            "{"
            "\"station_location\":[%.4f,%.4f],"
            "\"version\":%u,"
            "\"board\":%d,"
            "\"tx\":false,"
            "\"mode\":\"FSK\","
            "\"frequency\":%.4f,"
            "\"frequency_offset\":%.1f,"
            "\"satellite\":\"%s\","
            "\"bitrate\":%.0f,"
            "\"freqdev\":%.0f,"
            "\"rxBw\":%.1f,"
            "\"OOK\":%d,"
            "\"NORAD\":%u,"
            "\"rssi\":%.1f,"
            "\"snr\":%.2f,"
            "\"frequency_error\":%.1f,"
            "\"crc_error\":%s,"
            "\"unix_GS_time\":%u"
            "}",
            (double)tinygs_station_lat, (double)tinygs_station_lon,
            (unsigned)TINYGS_VERSION, TINYGS_BOARD,
            (double)tinygs_radio.frequency, (double)tinygs_radio.freq_offset,
            tinygs_radio.satellite,
            (double)tinygs_radio.bitrate, (double)tinygs_radio.freq_dev,
            (double)tinygs_radio.bw,
            tinygs_radio.ook,
            (unsigned)tinygs_radio.norad,
            (double)tinygs_radio.last_rssi,
            (double)tinygs_radio.last_snr,
            (double)tinygs_radio.last_freq_err,
            tinygs_radio.last_crc_error ? "true" : "false",
            (unsigned)(uint32_t)get_utc_epoch()
        );
    }

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload_buf;
    param.message.payload.len = len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    if (len >= (int)sizeof(payload_buf)) {
        LOG_WRN("Status payload truncated (%d >= %zu)", len, sizeof(payload_buf));
    }

    LOG_INF("Publishing status to %s", topic_buf);

    return mqtt_publish(client, &param);
}

void tinygs_handle_set_pos(const char *payload, size_t len)
{
    /* ESP32 format: JSON array [lat, lon, alt] or [alt]
     * Simple parser — find numbers between brackets */
    const char *p = strchr(payload, '[');
    if (!p) {
        LOG_WRN("set_pos_prm: no array found");
        return;
    }
    p++;

    float vals[3];
    int count = 0;
    while (count < 3 && *p && *p != ']') {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        char *end;
        vals[count] = strtof(p, &end);
        if (end == p) break;
        p = end;
        count++;
    }

    if (count == 1) {
        tinygs_station_alt = vals[0];
        tinygs_config_save_location(tinygs_station_lat, tinygs_station_lon, tinygs_station_alt);
        LOG_INF("Position updated: alt=%.1f", (double)tinygs_station_alt);
    } else if (count == 3) {
        tinygs_station_lat = vals[0];
        tinygs_station_lon = vals[1];
        tinygs_station_alt = vals[2];
        tinygs_config_save_location(tinygs_station_lat, tinygs_station_lon, tinygs_station_alt);
        LOG_INF("Position updated: lat=%.4f lon=%.4f alt=%.1f",
                (double)tinygs_station_lat,
                (double)tinygs_station_lon,
                (double)tinygs_station_alt);
    } else if (count == 0) {
        /* Server sends [null] when no position is configured — not an error */
        LOG_DBG("set_pos_prm: no position set on server (null)");
    } else {
        LOG_WRN("set_pos_prm: expected 1 or 3 values, got %d", count);
    }
}

int tinygs_send_weblogin_request(struct mqtt_client *client,
                                  const char *user, const char *station)
{
    static uint32_t last_weblogin_ms = 0;
    uint32_t now = k_uptime_get_32();

    if ((now - last_weblogin_ms) < 10000 && last_weblogin_ms != 0) {
        LOG_DBG("Weblogin request rate-limited (10s)");
        return 0;
    }
    last_weblogin_ms = now;

    tinygs_build_topic(topic_buf, sizeof(topic_buf),
                        TINYGS_TOPIC_TELE, "get_weblogin",
                        user, station);

    const char *payload = "1";

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload;
    param.message.payload.len = 1;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    LOG_INF("Requesting weblogin URL...");

    return mqtt_publish(client, &param);
}
