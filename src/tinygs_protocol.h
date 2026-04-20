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

#ifdef __cplusplus
extern "C" {
#endif

/* Symbolic name for Zephyr negative errno values — see main.cpp for table. */
const char *errno_name(int err);

#ifdef __cplusplus
}
#endif

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
#define TINYGS_VERSION        2604100  /* YYMMDDR — don't change without testing server reaction */
#define TINYGS_DEFAULT_FREQ   436.703f /* MHz — initial listen frequency before server assigns */
#define TINYGS_GIT_VERSION    "tinygs_nRF52"
#define TINYGS_CHIP           "nRF52840"  /* CONFIG_SOC gives "nRF52840_QIAA" — server rejects suffix */
#define TINYGS_BOARD          255  /* Custom/unknown board ID */
/* LORA_DTS_NODE must be defined BEFORE any macro that references it —
 * earlier ordering put the TCXO check first, so it evaluated against an
 * undefined node and silently fell through to 0.0V, breaking FSK begin. */
#define LORA_DTS_NODE DT_ALIAS(lora0)
/* TCXO voltage from DTS (dio3-tcxo-voltage property).
 * RadioLib needs the actual voltage as a float, not the register code. */
#if DT_NODE_HAS_PROP(LORA_DTS_NODE, dio3_tcxo_voltage)
#define LORA_TCXO_VOLTAGE     1.8f  /* SX126X_DIO3_TCXO_1V8 from DTS */
#else
#define LORA_TCXO_VOLTAGE     0.0f  /* No TCXO */
#endif
/* Radio chip ID for TinyGS welcome message (matches ESP32 Radio.h enum):
 * 0=SX1262, 1=SX1278, 2=SX1276, 5=SX1268, 6=SX1262, 10=LR1121 */
#if DT_NODE_HAS_COMPAT(LORA_DTS_NODE, semtech_sx1262)
#define TINYGS_RADIO_CHIP     6
#elif DT_NODE_HAS_COMPAT(LORA_DTS_NODE, semtech_sx1268)
#define TINYGS_RADIO_CHIP     5
#elif DT_NODE_HAS_COMPAT(LORA_DTS_NODE, semtech_sx1276)
#define TINYGS_RADIO_CHIP     2
#elif DT_NODE_HAS_COMPAT(LORA_DTS_NODE, semtech_sx1278)
#define TINYGS_RADIO_CHIP     1
#else
#define TINYGS_RADIO_CHIP     255
#endif

/* Ping interval — offset 30s before MQTT keepalive to avoid collision.
 * When both TinyGS PUBLISH ping and MQTT PINGREQ fire simultaneously,
 * the combined TCP writes cause EIO (-5) over NAT64, disconnecting.
 * Override keepalive via CONFIG_MQTT_KEEPALIVE in prj.conf. */
#define TINYGS_PING_INTERVAL_S  (CONFIG_MQTT_KEEPALIVE - 30)

/* Threshold of unanswered PINGREQs before we give up on the MQTT session
 * and reconnect. 1 is normal jitter on a slow NAT64/Thread path; 2 means
 * the other end is almost certainly gone. Zephyr's mqtt_client doesn't
 * act on client->unacked_ping — we do it ourselves in the poll loop.
 * PubSubClient on ESP32 uses a similar effective threshold. */
#define TINYGS_MQTT_UNACKED_PING_MAX  2

/* ArduinoJson JsonDocument guardrails for inbound begine payloads —
 * real messages are ~300 B, this gives 4× headroom against a malformed
 * or hostile payload exhausting the default malloc allocator.
 * NestingLimit caps how deep deserializeJson will recurse (our schema
 * is flat, 5 is plenty). */
#define TINYGS_BEGINE_MAX_LEN          2048
#define TINYGS_JSON_NESTING_LIMIT      5

/* sleep/siesta clamp — we refuse any request longer than this. Matches
 * common-sense upper bound; the server's own ESP32-path deep sleep
 * accepts arbitrary seconds. */
#define TINYGS_SLEEP_MAX_SECONDS       86400  /* 24h */

/* Outbound UDP helper targets (DNS bootstrap + SNTP).
 *
 * ULA-source → global-IPv6-destination packets get ICMP-rejected by the
 * OT BorderRouting module in multi-BR networks, so every outbound host
 * must be reachable via the mesh's NAT64 prefix. Two paths:
 *
 *   DNS: target = <nat64_prefix_96> + DNS_SERVER_V4 synthesised at query
 *        time. Cloudflare 1.1.1.1 is an IPv4-anycast resolver and needs
 *        no DNS64 on its side because we query A records explicitly and
 *        let OT synthesise the response into the NAT64 prefix for us.
 *
 *   SNTP: target = DNS-resolved AAAA for SNTP_HOSTNAME. The resolver
 *        returns a NAT64-synthesised address from whichever IPv4 the NTP
 *        pool picks today — so no hardcoded IPv4 to go stale. Trade:
 *        SNTP depends on DNS working (acceptable — MQTT also depends on
 *        DNS, so if DNS is down the whole station is down anyway).
 */
#define TINYGS_DNS_SERVER_V4       { 1, 1, 1, 1 }
#define TINYGS_DNS_SERVER_PORT     53
#define TINYGS_SNTP_HOSTNAME       "pool.ntp.org"
#define TINYGS_SNTP_SERVER_PORT    123

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
                    float rssi, float snr, float freq_err,
                    bool crc_error);

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
    char  modem_mode[8];   /* "LoRa" or "FSK" */
    float frequency;       /* MHz */
    float freq_offset;     /* Hz — applied on top of frequency from foff command */
    float freq_doppler;    /* Hz — current Doppler correction */
    int   sf;              /* Spreading factor 5-12 (LoRa) */
    float bw;              /* Bandwidth kHz */
    int   cr;              /* Coding rate 5-8 (LoRa) */
    int   pl;              /* Preamble length */
    bool  crc_on;          /* CRC enabled */
    int   fldro;           /* FLDRO setting (0/1/2=auto) */
    bool  iIQ;             /* Inverted IQ — from begine, reported in RX payload */
    /* FSK-specific */
    float bitrate;         /* FSK bitrate bps */
    float freq_dev;        /* FSK frequency deviation Hz */
    int   ook;             /* OOK mode (255=enabled) */
    int   fsk_len;         /* FSK fixed packet length */
    int   fsk_enc;         /* FSK encoding (0=none, 1=Manchester, 2=whitening) */
    int   fsk_framing;     /* FSK framing (0=raw, 1=AX.25 NRZS, 2=PN9, 3=scrambled) */
    /* Software CRC config (FSK only) */
    bool  sw_crc_enabled;  /* Software CRC check after reception */
    uint8_t sw_crc_bytes;  /* CRC byte count (1 or 2) */
    uint16_t sw_crc_init;  /* CRC initial value */
    uint16_t sw_crc_poly;  /* CRC polynomial */
    uint16_t sw_crc_xor;   /* CRC final XOR */
    bool  sw_crc_refin;    /* CRC reflect input */
    bool  sw_crc_refout;   /* CRC reflect output */
    char  satellite[32];   /* Current satellite name */
    uint32_t norad;        /* NORAD catalog number */
    uint8_t tle[64];       /* Binary TLE from server (base64-decoded); ESP32 uses same size */
    bool  tle_valid;       /* True if TLE data was received */
    bool  doppler_enabled; /* True if Doppler compensation is active */
    float doppler_tol;     /* Hz — hysteresis threshold (default 1200) */
    uint8_t filter[8];     /* Packet filter bytes from server */
    uint8_t filter_len;    /* Number of active filter bytes (0 = no filter) */
    char  modem_conf[512]; /* Last begine/batch_conf JSON, echoed in welcome */
    /* Last packet metrics — for status payload */
    float last_rssi;
    float last_snr;
    float last_freq_err;
    bool  last_crc_error;
    float sat_pos_x;       /* Satellite map position from sat_pos_oled (128x64 coords) */
    float sat_pos_y;
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

/*
 * Request weblogin URL from server.
 * Publishes "1" to tele/get_weblogin. Server responds on cmnd/weblogin.
 */
int tinygs_send_weblogin_request(struct mqtt_client *client,
                                  const char *user, const char *station);

/*
 * Respond to get_adv_prm. Publishes {"adv_prm":"<stored_blob>"} to
 * tele/get_adv_prm.
 */
int tinygs_send_adv_prm(struct mqtt_client *client,
                         const char *user, const char *station,
                         const char *adv_prm);

/* tinygs_build_adv_prm is declared in tinygs_json.h (pure JSON builder,
 * usable from unit tests that don't link this file). */

#endif /* TINYGS_PROTOCOL_H */
