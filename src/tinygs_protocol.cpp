#include "tinygs_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/base64.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

LOG_MODULE_REGISTER(tinygs_proto, LOG_LEVEL_INF);

/* Station location — defaults to Sydney, updated by set_pos_prm */
float tinygs_station_lat = -33.8688f;
float tinygs_station_lon = 151.2093f;
float tinygs_station_alt = 50.0f;

/* Shared buffers for topic and payload construction */
static char topic_buf[128];
static char payload_buf[512];

int tinygs_build_welcome(char *buf, size_t buflen,
                          const char *mac, int vbat_mv, uint32_t free_mem,
                          uint32_t uptime_s)
{
    /* ESP32 sends WiFi IPv4 here. We're on Thread (IPv6 only).
     * Send "0.0.0.0" to avoid breaking TinyGS database which expects IPv4. */
    const char *ip_str = "0.0.0.0";

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
        "\"sat\":\"\","
        "\"ip\":\"%s\","
        "\"idfv\":\"NCS/Zephyr\","
        "\"modem_conf\":\"{}\""
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
        ip_str
    );
}

int tinygs_build_ping(char *buf, size_t buflen,
                       int vbat_mv, uint32_t free_mem, uint32_t min_mem,
                       int radio_error, float inst_rssi)
{
    /* Get Thread parent RSSI if available */
    int8_t thread_rssi = 0;
    struct openthread_context *ot_ctx = openthread_get_default_context();
    if (ot_ctx) {
        openthread_api_mutex_lock(ot_ctx);
        otThreadGetParentAverageRssi(ot_ctx->instance, &thread_rssi);
        openthread_api_mutex_unlock(ot_ctx);
    }

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

    /* Build payload — Vbat in millivolts (ESP32 convention) */
    int len = tinygs_build_welcome(payload_buf, sizeof(payload_buf),
                                    mac, 3700, 81820,
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
                                 3700, 81820, 81820, 0, -120.0f);

    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic_buf;
    param.message.topic.topic.size = strlen(topic_buf);
    param.message.payload.data = (uint8_t *)payload_buf;
    param.message.payload.len = len;
    param.message_id = 0;
    param.dup_flag = 0;
    param.retain_flag = 0;

    LOG_DBG("Ping: %s", payload_buf);

    return mqtt_publish(client, &param);
}

int tinygs_send_rx(struct mqtt_client *client,
                    const char *user, const char *station,
                    const uint8_t *data, size_t data_len,
                    float rssi, float snr, float freq_err,
                    float frequency, int sf, float bw, int cr)
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

    /* Build RX JSON payload — types matched to ESP32 ArduinoJson output */
    int len = snprintf(payload_buf, sizeof(payload_buf),
        "{"
        "\"station_location\":[%.4f,%.4f],"
        "\"mode\":\"LoRa\","
        "\"frequency\":%.4f,"
        "\"frequency_offset\":0,"
        "\"satellite\":\"\","
        "\"sf\":%d,"
        "\"cr\":%d,"
        "\"bw\":%.1f,"
        "\"rssi\":%.1f,"
        "\"snr\":%.2f,"
        "\"frequency_error\":%.1f,"
        "\"unix_GS_time\":%u,"
        "\"usec_time\":0,"
        "\"crc_error\":false,"
        "\"data\":\"%s\","
        "\"NORAD\":0,"
        "\"noisy\":false,"
        "\"iIQ\":false"
        "}",
        (double)tinygs_station_lat,
        (double)tinygs_station_lon,
        (double)frequency,
        sf, cr,
        (double)bw,
        (double)rssi,
        (double)snr,
        (double)freq_err,
        (unsigned)(k_uptime_get_32() / 1000),
        b64_buf
    );

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
        LOG_INF("Position updated: alt=%.1f", (double)tinygs_station_alt);
    } else if (count == 3) {
        tinygs_station_lat = vals[0];
        tinygs_station_lon = vals[1];
        tinygs_station_alt = vals[2];
        LOG_INF("Position updated: lat=%.4f lon=%.4f alt=%.1f",
                (double)tinygs_station_lat,
                (double)tinygs_station_lon,
                (double)tinygs_station_alt);
    } else {
        LOG_WRN("set_pos_prm: expected 1 or 3 values, got %d", count);
    }
}
