/**
 * TinyGS JSON Parser — structured parsing for MQTT command payloads.
 *
 * Uses Zephyr's zero-allocation json.h for the begine message (most fields).
 * Filter array, TLE base64, and set_pos_prm use lightweight manual parsing
 * since Zephyr json.h doesn't handle heterogeneous arrays or base64.
 *
 * Float fields: Zephyr json.h stores floats as raw string tokens
 * (json_obj_token). We convert with strtof() via accessor functions.
 */

#include "tinygs_json.h"
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(tinygs_json, LOG_LEVEL_INF);

/**
 * Escape a JSON string: replace " with \" and \ with \\.
 * Returns bytes written (excluding null terminator). Always null-terminates
 * if dstlen >= 1 (truncates if output would overflow).
 */
extern "C" size_t tinygs_json_escape(char *dst, size_t dstlen, const char *src)
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
    if (dst && dstlen > 0) {
        dst[(n < dstlen) ? n : dstlen - 1] = '\0';
    }
    return n;
}

int tinygs_build_adv_prm(char *buf, size_t buflen, const char *adv_prm)
{
    /* ESP32 format (MQTT_Client.cpp:536-539):
     *   doc["adv_prm"].set(configManager.getAvancedConfig());
     * We always publish {"adv_prm":"<escaped-stored-string>"} even if the
     * stored blob is itself JSON — server treats it as an opaque string. */
    if (!buf || buflen < 16) {
        return -1;
    }
    static char esc[1024];
    tinygs_json_escape(esc, sizeof(esc), adv_prm ? adv_prm : "");
    int n = snprintf(buf, buflen, "{\"adv_prm\":\"%s\"}", esc);
    if (n < 0 || (size_t)n >= buflen) {
        return -1;
    }
    return n;
}

/* --- begine/batch_conf parsing via ArduinoJson ---
 *
 * Switched from Zephyr's descriptor-based json_obj_parse to ArduinoJson v7
 * because descriptor-based parsing fails the entire message on any unknown
 * key or type mismatch. Two production bugs today (missing 'tle' field,
 * fractional 'br') came from exactly that class of fragility. ArduinoJson
 * reads per-field lazily with defaults, so a surprise key at position N
 * never invalidates fields 1..N-1. Matches the ESP32 station semantically.
 *
 * Cost (measured): +~5 KB flash, +~5 KB RAM (JsonDocument pool).
 */
#include <ArduinoJson.h>
#include <errno.h>

int64_t tinygs_parse_begine(char *json, size_t len, struct tinygs_begine_msg *msg)
{
    memset(msg, 0, sizeof(*msg));

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        LOG_ERR("begine JSON parse failed: %s", err.c_str());
        return -EINVAL;
    }

    /* Strings: `| (const char *)nullptr` is the ArduinoJson default-on-miss
     * operator. Same for numeric types below. */
    msg->mode    = doc["mode"]  | (const char *)NULL;
    msg->sat     = doc["sat"]   | (const char *)NULL;
    msg->tlx     = doc["tlx"]   | (const char *)NULL;
    msg->tle     = doc["tle"]   | (const char *)NULL;

    msg->sf      = doc["sf"]    | 0;
    msg->cr      = doc["cr"]    | 0;
    msg->sw      = doc["sw"]    | 18;      /* LoRa default sync word */
    msg->pwr     = doc["pwr"]   | 5;
    msg->cl      = doc["cl"]    | 0;
    msg->pl      = doc["pl"]    | 8;
    msg->gain    = doc["gain"]  | 0;
    msg->NORAD   = doc["NORAD"] | 0;
    msg->fldro   = doc["fldro"] | 0;
    msg->crc     = doc["crc"]   | true;
    msg->iIQ     = doc["iIQ"]   | false;

    msg->freq    = doc["freq"]  | 0.0f;
    msg->bw      = doc["bw"]    | 0.0f;

    /* FSK-specific */
    msg->br      = doc["br"]    | 0.0f;
    msg->fd      = doc["fd"]    | 0.0f;
    msg->ook     = doc["ook"]   | 0;
    msg->enc     = doc["enc"]   | 0;
    msg->ws      = doc["ws"]    | 0;
    msg->fr      = doc["fr"]    | 0;
    msg->len     = doc["len"]   | 0;

    /* Software CRC block */
    msg->cSw     = doc["cSw"]   | false;
    msg->cB      = doc["cB"]    | 0;
    msg->cI      = doc["cI"]    | 0;
    msg->cP      = doc["cP"]    | 0;
    msg->cF      = doc["cF"]    | 0;
    msg->cRI     = doc["cRI"]   | false;
    msg->cRO     = doc["cRO"]   | false;

    /* Return a non-zero "parsed" indicator; ESP32-style callers treat >0 as
     * success. We pick the field count as a rough approximation, but any
     * positive number works for our existing callers. */
    return doc.size();
}

/* Backwards-compat accessors — callers still use these. */
float tinygs_begine_get_freq(const struct tinygs_begine_msg *msg) { return msg->freq; }
float tinygs_begine_get_bw(const struct tinygs_begine_msg *msg)   { return msg->bw;   }
float tinygs_begine_get_fd(const struct tinygs_begine_msg *msg)   { return msg->fd;   }
float tinygs_begine_get_br(const struct tinygs_begine_msg *msg)   { return msg->br;   }

int tinygs_parse_fsw(const char *json, size_t len, uint8_t *buf, size_t buf_size)
{
    /* Find "fsw":[ in the JSON */
    const char *key = "\"fsw\":[";
    const char *p = strstr(json, key);
    if (!p) return 0;
    p += strlen(key);

    int count = 0;
    while (count < (int)buf_size) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        buf[count++] = (uint8_t)atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }
    return count;
}

/* --- set_pos_prm parsing: [lat, lon, alt] or [alt] or [null] --- */

int tinygs_parse_set_pos(const char *json, size_t len, struct tinygs_pos_msg *msg)
{
    memset(msg, 0, sizeof(*msg));

    /* Find opening bracket */
    const char *p = (const char *)memchr(json, '[', len);
    if (!p) return -1;
    p++;

    /* Check for [null] */
    while (*p == ' ') p++;
    if (strncmp(p, "null", 4) == 0) {
        msg->count = 0;
        return 0;
    }

    /* Parse up to 3 float values */
    msg->count = 0;
    while (msg->count < 3) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;

        char *end;
        float v = strtof(p, &end);
        if (end == p) break;  /* no number found */

        msg->values[msg->count++] = v;
        p = end;
    }

    return msg->count;
}

/* --- set_name parsing: ["MAC", "new_name"] --- */

int tinygs_parse_set_name(const char *json, size_t len, struct tinygs_name_msg *msg)
{
    memset(msg, 0, sizeof(*msg));

    /* Find first quoted string (MAC) */
    const char *p = (const char *)memchr(json, '"', len);
    if (!p) return -1;
    p++;

    const char *end = strchr(p, '"');
    if (!end || (end - p) != 12) return -1;

    memcpy(msg->mac, p, 12);
    msg->mac[12] = '\0';

    /* Find second quoted string (name) */
    p = strchr(end + 1, '"');
    if (!p) return -1;
    p++;

    end = strchr(p, '"');
    if (!end) return -1;

    size_t nlen = end - p;
    if (nlen == 0 || nlen >= sizeof(msg->name)) return -1;

    memcpy(msg->name, p, nlen);
    msg->name[nlen] = '\0';

    return 0;
}

/* --- generic JSON value extractors --- */

float json_extract_float_n(const char *json, const char *key, size_t key_len, float default_val)
{
    const char *p = strstr(json, key);
    if (!p) return default_val;
    p += key_len;
    return strtof(p, NULL);
}

int json_extract_string_n(const char *json, const char *key, size_t key_len, char *buf, size_t buf_size)
{
    const char *p = strstr(json, key);
    if (!p) return -1;
    p += key_len;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = end - p;
    if (len >= buf_size) len = buf_size - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';
    return (int)len;
}

int json_extract_int_n(const char *json, const char *key, size_t key_len, int default_val)
{
    const char *p = strstr(json, key);
    if (!p) return default_val;
    p += key_len;
    return atoi(p);
}

/* --- filter array parsing: [1, 0, 235] --- */

float tinygs_parse_foff(const char *json, size_t len, float *tol, uint32_t *refresh_ms)
{
    const char *bracket = (const char *)memchr(json, '[', len);
    if (bracket) {
        bracket++;
        char *end;
        float offset = strtof(bracket, &end);
        if (*end == ',') {
            float t = strtof(end + 1, &end);
            if (tol && t > 0) *tol = t;
        }
        if (*end == ',') {
            int r = atoi(end + 1);
            if (refresh_ms && r > 0) *refresh_ms = (uint32_t)r;
        }
        return offset;
    }
    return strtof(json, NULL);
}

int tinygs_parse_filter(const char *json, size_t len, uint8_t *buf, size_t buf_size)
{
    const char *p = (const char *)memchr(json, '[', len);
    if (!p) return -1;
    p++;

    int count = 0;
    while (count < (int)buf_size) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;

        buf[count++] = (uint8_t)atoi(p);
        while (*p && *p != ',' && *p != ']') p++;
    }

    return count;
}
