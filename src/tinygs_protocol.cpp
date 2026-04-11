#include "tinygs_protocol.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(tinygs_proto, LOG_LEVEL_INF);

/* Shared buffers for topic and payload construction */
static char topic_buf[128];
static char payload_buf[512];

int tinygs_build_welcome(char *buf, size_t buflen,
                          const char *mac, float vbat, uint32_t free_mem,
                          uint32_t uptime_s)
{
    /* Minimal welcome — the server needs at minimum: version, mac, board, chip.
     * We omit ESP32-specific fields (ip, slot, pSize, idfv) and add what we can. */
    return snprintf(buf, buflen,
        "{"
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
        "\"station_location\":[-33.8688,151.2093]"
        "}",
        TINYGS_VERSION,
        TINYGS_GIT_VERSION,
        TINYGS_CHIP,
        TINYGS_BOARD,
        mac,
        TINYGS_RADIO_CHIP,
        (unsigned)free_mem,
        (unsigned)uptime_s,
        (double)vbat
    );
}

int tinygs_build_ping(char *buf, size_t buflen,
                       float vbat, uint32_t free_mem, uint32_t min_mem,
                       int radio_error, float inst_rssi)
{
    return snprintf(buf, buflen,
        "{"
        "\"Vbat\":%.1f,"
        "\"Mem\":%u,"
        "\"MinMem\":%u,"
        "\"MaxBlk\":%u,"
        "\"RSSI\":0,"
        "\"radio\":%d,"
        "\"InstRSSI\":%.1f"
        "}",
        (double)vbat,
        (unsigned)free_mem,
        (unsigned)min_mem,
        (unsigned)free_mem,  /* MaxBlk ≈ free mem on Zephyr */
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
