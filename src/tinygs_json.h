#ifndef TINYGS_JSON_H
#define TINYGS_JSON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Structured JSON parsing for TinyGS MQTT commands.
 * Uses Zephyr's zero-allocation json.h with descriptor-based parsing.
 * Float fields use json_obj_token (raw string) + strtof() conversion.
 */

/* Parsed begine/batch_conf message */
struct tinygs_begine_msg {
    const char *mode;
    const char *sat;
    const char *tlx;   /* Passive TLE (map display only) */
    const char *tle;   /* Active-Doppler TLE (server requests freq compensation) */
    int32_t sf;
    int32_t cr;
    int32_t sw;
    int32_t pwr;
    int32_t cl;
    int32_t pl;
    int32_t gain;
    int32_t NORAD;
    int32_t fldro;
    bool crc;
    bool iIQ;
    /* FSK-specific fields */
    int32_t ook;      /* OOK mode (255 = OOK enabled) */
    int32_t enc;      /* Encoding (0=none, 1=Manchester, 2=whitening) */
    int32_t ws;       /* Whitening seed */
    int32_t fr;       /* Framing (0=raw, 1=AX.25 NRZS, 2=PN9, 3=scrambled AX.25) */
    int32_t len;      /* Packet length: LoRa implicit header (>0) or FSK fixed length. 0=explicit header. */
    /* Software CRC fields (FSK) */
    bool cSw;         /* Software CRC enable */
    int32_t cB;       /* CRC byte count */
    int32_t cI;       /* CRC initial value */
    int32_t cP;       /* CRC polynomial */
    int32_t cF;       /* CRC final XOR */
    bool cRI;         /* CRC reflect input */
    bool cRO;         /* CRC reflect output */
    /* Floats stored as raw tokens — call tinygs_begine_get_freq() etc. */
    struct {
        char *start;
        size_t length;
    } freq;
    struct {
        char *start;
        size_t length;
    } bw;
    struct {
        char *start;
        size_t length;
    } fd;  /* FSK frequency deviation */
    struct {
        char *start;
        size_t length;
    } br;  /* FSK bitrate — may be fractional (e.g. 1.2 kbps) */
    /* filter, tlx, and fsw need special handling (arrays/base64) */
};

/* Extract float values from parsed begine message */
float tinygs_begine_get_fd(const struct tinygs_begine_msg *msg);
float tinygs_begine_get_br(const struct tinygs_begine_msg *msg);

/*
 * Parse a begine/batch_conf JSON payload into structured fields.
 * Returns bitmask of successfully parsed fields (0 on total failure).
 * The json buffer is modified in-place (null terminators inserted).
 */
int64_t tinygs_parse_begine(char *json, size_t len, struct tinygs_begine_msg *msg);

/* Extract float values from parsed begine message */
float tinygs_begine_get_freq(const struct tinygs_begine_msg *msg);
float tinygs_begine_get_bw(const struct tinygs_begine_msg *msg);

/* Parsed set_pos_prm message — [lat, lon, alt] or [alt] or [null] */
struct tinygs_pos_msg {
    float values[3];
    int count;  /* 0=[null], 1=[alt], 3=[lat,lon,alt] */
};

/*
 * Parse set_pos_prm payload. Returns number of values (0, 1, or 3).
 * Returns -1 on parse error.
 */
int tinygs_parse_set_pos(const char *json, size_t len, struct tinygs_pos_msg *msg);

/* Parsed set_name message — ["MAC", "new_name"] */
struct tinygs_name_msg {
    char mac[13];
    char name[32];
};

/*
 * Parse set_name payload. Returns 0 on success, -1 on error.
 */
int tinygs_parse_set_name(const char *json, size_t len, struct tinygs_name_msg *msg);

/*
 * Parse filter array from JSON. Writes filter bytes to buf, returns count.
 * Returns -1 on error.
 */
int tinygs_parse_filter(const char *json, size_t len, uint8_t *buf, size_t buf_size);

/*
 * Parse foff command payload.
 * Can be a simple float ("1500.0") or array [offset, tolerance, refresh_ms].
 * Returns offset in Hz. If tolerance/refresh present, writes them to *tol/*refresh_ms.
 * Pass NULL for tol/refresh_ms if not needed.
 */
/*
 * Parse FSK sync word array "fsw":[b0,b1,...] from begine JSON.
 * Returns number of sync word bytes (0 if not found).
 */
int tinygs_parse_fsw(const char *json, size_t len, uint8_t *buf, size_t buf_size);

/*
 * Generic JSON value extractors — lightweight, no allocation.
 * Use strlen(key)+1 internally for offset, avoiding hardcoded magic numbers.
 */

/* Extract a float value for a given key. Returns default_val if not found.
 * key_len is the length of the key string (use sizeof(key)-1 for literals). */
float json_extract_float_n(const char *json, const char *key, size_t key_len, float default_val);

/* Extract a quoted string value for a given key into buf.
 * Returns length of extracted string, or -1 if not found. */
int json_extract_string_n(const char *json, const char *key, size_t key_len, char *buf, size_t buf_size);

/* Extract an int value for a given key. Returns default_val if not found. */
int json_extract_int_n(const char *json, const char *key, size_t key_len, int default_val);

/* Convenience macros — sizeof() on string literals is compile-time */
#define json_extract_float(json, key, def)      json_extract_float_n(json, key, sizeof(key) - 1, def)
#define json_extract_string(json, key, buf, sz) json_extract_string_n(json, key, sizeof(key) - 1, buf, sz)
#define json_extract_int(json, key, def)        json_extract_int_n(json, key, sizeof(key) - 1, def)

float tinygs_parse_foff(const char *json, size_t len, float *tol, uint32_t *refresh_ms);

#ifdef __cplusplus
}
#endif

#endif /* TINYGS_JSON_H */
