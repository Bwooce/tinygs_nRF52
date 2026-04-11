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

/* --- begine/batch_conf parsing via Zephyr json descriptors --- */

static const struct json_obj_descr begine_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, mode, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, sat, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, tlx, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, sf, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, cr, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, sw, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, pwr, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, cl, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, pl, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, gain, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM_NAMED(struct tinygs_begine_msg, "NORAD",
                               NORAD, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, fldro, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, crc, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, iIQ, JSON_TOK_TRUE),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, freq, JSON_TOK_FLOAT),
    JSON_OBJ_DESCR_PRIM(struct tinygs_begine_msg, bw, JSON_TOK_FLOAT),
};

int64_t tinygs_parse_begine(char *json, size_t len, struct tinygs_begine_msg *msg)
{
    memset(msg, 0, sizeof(*msg));
    /* Set defaults for optional fields */
    msg->sw = 18;
    msg->pwr = 5;
    msg->pl = 8;
    msg->crc = true;

    int64_t ret = json_obj_parse(json, len, begine_descr,
                                  ARRAY_SIZE(begine_descr), msg);
    if (ret < 0) {
        LOG_ERR("begine JSON parse failed: %lld", ret);
    }
    return ret;
}

float tinygs_begine_get_freq(const struct tinygs_begine_msg *msg)
{
    if (msg->freq.start && msg->freq.length > 0) {
        return strtof(msg->freq.start, NULL);
    }
    return 0.0f;
}

float tinygs_begine_get_bw(const struct tinygs_begine_msg *msg)
{
    if (msg->bw.start && msg->bw.length > 0) {
        return strtof(msg->bw.start, NULL);
    }
    return 0.0f;
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

/* --- filter array parsing: [1, 0, 235] --- */

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
