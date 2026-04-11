#ifndef TINYGS_PROTOCOL_H
#define TINYGS_PROTOCOL_H

/*
 * TinyGS MQTT Protocol Implementation
 * Reverse-engineered from ESP32 TinyGS source (github.com/G4lile0/tinyGS)
 * See docs/TINYGS_MQTT_PROTOCOL.md for full specification.
 */

#include <zephyr/net/mqtt.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

/* Topic templates — %user% and %station% are replaced at runtime */
#define TINYGS_TOPIC_GLOBAL   "tinygs/global/#"
#define TINYGS_TOPIC_CMND     "tinygs/%s/%s/cmnd/#"
#define TINYGS_TOPIC_TELE     "tinygs/%s/%s/tele/%s"
#define TINYGS_TOPIC_STAT     "tinygs/%s/%s/stat/%s"

/* Sub-topics */
#define TINYGS_TELE_WELCOME   "welcome"
#define TINYGS_TELE_PING      "ping"
#define TINYGS_TELE_RX        "rx"
#define TINYGS_STAT_STATUS    "status"

/* Version info for welcome message */
#define TINYGS_VERSION        2604100  /* YYMMDDR: 2026-04-10, release 0 */
#define TINYGS_GIT_VERSION    "tinygs_nRF52"
#define TINYGS_CHIP           "nRF52840"
#define TINYGS_BOARD          255  /* Custom/unknown board ID */
#define TINYGS_RADIO_CHIP     6    /* SX1262 (matches ESP32 Radio.h enum) */

/* Ping interval — tied to MQTT keepalive so pings double as keepalives.
 * Override via CONFIG_MQTT_KEEPALIVE in prj.conf. */
#define TINYGS_PING_INTERVAL_S  CONFIG_MQTT_KEEPALIVE

/*
 * Build a topic string. Caller provides buffer.
 * Returns length written (excluding null terminator).
 */
static inline int tinygs_build_topic(char *buf, size_t buflen,
                                      const char *tmpl, const char *subtopic,
                                      const char *user, const char *station)
{
    return snprintf(buf, buflen, tmpl, user, station, subtopic);
}

/*
 * Build the welcome JSON payload.
 * Returns length written.
 */
int tinygs_build_welcome(char *buf, size_t buflen,
                          const char *mac, int vbat_mv, uint32_t free_mem,
                          uint32_t uptime_s);

/*
 * Build the ping JSON payload.
 * Returns length written.
 */
int tinygs_build_ping(char *buf, size_t buflen,
                       int vbat_mv, uint32_t free_mem, uint32_t min_mem,
                       int radio_error, float inst_rssi);

/*
 * Subscribe to TinyGS MQTT topics.
 * Must be called after MQTT CONNACK.
 */
int tinygs_subscribe(struct mqtt_client *client,
                      const char *user, const char *station);

/*
 * Send the welcome message.
 */
int tinygs_send_welcome(struct mqtt_client *client,
                         const char *user, const char *station,
                         const char *mac);

/*
 * Send a ping message.
 */
int tinygs_send_ping(struct mqtt_client *client,
                      const char *user, const char *station);

/*
 * Send an RX packet (LoRa received data) to the TinyGS server.
 * Data is base64-encoded in the JSON payload.
 * Radio config (freq, sf, bw, cr, satellite) taken from tinygs_radio state.
 */
int tinygs_send_rx(struct mqtt_client *client,
                    const char *user, const char *station,
                    const uint8_t *data, size_t data_len,
                    float rssi, float snr, float freq_err);

/*
 * Station location — used in welcome and RX payloads.
 * Updated by set_pos_prm server command. Defaults to Sydney.
 */
extern float tinygs_station_lat;
extern float tinygs_station_lon;
extern float tinygs_station_alt;

/*
 * Current radio configuration — updated by begine/batch_conf commands.
 * Used in send_rx and send_status to report actual radio state.
 */
struct tinygs_radio_state {
    float frequency;       /* MHz */
    int   sf;              /* Spreading factor 5-12 */
    float bw;              /* Bandwidth kHz */
    int   cr;              /* Coding rate 5-8 */
    char  satellite[32];   /* Current satellite name */
    uint32_t norad;        /* NORAD catalog number */
    char  modem_conf[384]; /* Last begine/batch_conf JSON — echoed in welcome */
};
extern struct tinygs_radio_state tinygs_radio;

/*
 * Handle set_pos_prm command from server.
 * Payload is JSON array: [lat, lon, alt] or [alt].
 */
void tinygs_handle_set_pos(const char *payload, size_t len);

/*
 * Send status response (stat/status topic).
 * Called in response to cmnd/status from server.
 * Radio config taken from tinygs_radio state.
 */
int tinygs_send_status(struct mqtt_client *client,
                        const char *user, const char *station);

#endif /* TINYGS_PROTOCOL_H */
