#include "tinygs_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/base64.h>
#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

LOG_MODULE_REGISTER(tinygs_proto, LOG_LEVEL_INF);

/* Shared buffers for topic and payload construction */
static char topic_buf[128];
static char payload_buf[512];

int tinygs_build_welcome(char *buf, size_t buflen,
                          const char *mac, float vbat, uint32_t free_mem,
                          uint32_t uptime_s)
{
    /* ESP32 sends WiFi IPv4 here. We're on Thread (IPv6 only).
     * Send "0.0.0.0" to avoid breaking TinyGS database which expects IPv4. */
    const char *ip_str = "0.0.0.0";

    return snprintf(buf, buflen,
        "{"
        "\"station_location\":[-33.8688,151.2093],"
        "\"version\":\"%s\","
        "\"git_version\":\"%s\","
        "\"chip\":\"%s\","
        "\"board\":%d,"
        "\"mac\":\"%s\","
        "\"radioChip\":%d,"
        "\"Mem\":%u,"
        "\"seconds\":%u,"
        "\"Vbat\":%.1f,"
        "\"tx\":0,"
        "\"sat\":\"\","
        "\"ip\":\"%s\","
        "\"idfv\":\"NCS/Zephyr\","
        "\"modem_conf\":\"{}\""
        "}",
        TINYGS_VERSION,
        TINYGS_GIT_VERSION,
        TINYGS_CHIP,
        TINYGS_BOARD,
        mac,
        TINYGS_RADIO_CHIP,
        (unsigned)free_mem,
        (unsigned)uptime_s,
        (double)vbat,
        ip_str
    );
}

int tinygs_build_ping(char *buf, size_t buflen,
                       float vbat, uint32_t free_mem, uint32_t min_mem,
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
        "\"Vbat\":%.1f,"
        "\"Mem\":%u,"
        "\"MinMem\":%u,"
        "\"MaxBlk\":%u,"
        "\"RSSI\":%d,"
        "\"radio\":%d,"
        "\"InstRSSI\":%.1f"
        "}",
        (double)vbat,
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

    /* Build payload */
    int len = tinygs_build_welcome(payload_buf, sizeof(payload_buf),
                                    mac, 3.7f, 81820,
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
                                 3.7f, 81820, 81820, 0, -120.0f);

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

    /* Build RX JSON payload */
    int len = snprintf(payload_buf, sizeof(payload_buf),
        "{"
        "\"station_location\":[-33.8688,151.2093],"
        "\"mode\":1,"
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
        "\"crc_error\":0,"
        "\"data\":\"%s\","
        "\"NORAD\":0,"
        "\"noisy\":0,"
        "\"iIQ\":0"
        "}",
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
