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
    const char *tlx;
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
    /* Floats stored as raw tokens — call tinygs_begine_get_freq() etc. */
    struct {
        char *start;
        size_t length;
    } freq;
    struct {
        char *start;
        size_t length;
    } bw;
    /* filter and tlx need special handling (array and base64) */
};

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
float tinygs_parse_foff(const char *json, size_t len, float *tol, uint32_t *refresh_ms);

#ifdef __cplusplus
}
#endif

#endif /* TINYGS_JSON_H */
