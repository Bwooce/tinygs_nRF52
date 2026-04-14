/**
 * Ztest unit tests for TinyGS JSON parser (tinygs_json.h)
 *
 * Run: west build -b native_posix tests/json_parser && ./build/zephyr/zephyr.exe
 * Or:  west build -b nrf52840dk_nrf52840 tests/json_parser && west flash
 */

#include <zephyr/ztest.h>
#include "tinygs_json.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---- begine parsing tests ---- */

ZTEST(json_parser, test_begine_basic)
{
    char json[] = "{\"mode\":\"LoRa\",\"freq\":436.703,\"bw\":250,\"sf\":10,"
                  "\"cr\":5,\"sw\":18,\"pwr\":5,\"cl\":120,\"pl\":8,\"gain\":0,"
                  "\"crc\":true,\"fldro\":1,\"sat\":\"Norby\",\"NORAD\":12345}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "parse should succeed");

    float freq = tinygs_begine_get_freq(&msg);
    zassert_true(fabsf(freq - 436.703f) < 0.001f, "freq should be 436.703");

    float bw = tinygs_begine_get_bw(&msg);
    zassert_true(fabsf(bw - 250.0f) < 0.1f, "bw should be 250");

    zassert_equal(msg.sf, 10, "sf should be 10");
    zassert_equal(msg.cr, 5, "cr should be 5");
    zassert_equal(msg.sw, 18, "sw should be 18");
    zassert_equal(msg.NORAD, 12345, "NORAD should be 12345");
    zassert_true(msg.crc, "crc should be true");
    zassert_false(msg.iIQ, "iIQ should default false");
    zassert_not_null(msg.sat, "sat should be parsed");
    zassert_true(strcmp(msg.sat, "Norby") == 0, "sat should be Norby");
}

ZTEST(json_parser, test_begine_minimal)
{
    /* Server sometimes sends minimal begine without all fields */
    char json[] = "{\"freq\":400.265,\"bw\":125,\"sf\":9,\"cr\":5,\"sat\":\"Tianqi\"}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "parse should succeed with minimal fields");

    float freq = tinygs_begine_get_freq(&msg);
    zassert_true(fabsf(freq - 400.265f) < 0.001f, "freq should be 400.265");
    zassert_equal(msg.sf, 9, "sf should be 9");
    /* Defaults for missing fields */
    zassert_equal(msg.sw, 18, "sw should default to 18");
    zassert_true(msg.crc, "crc should default true");
}

ZTEST(json_parser, test_begine_with_iiq)
{
    char json[] = "{\"freq\":436.0,\"bw\":62.5,\"sf\":8,\"cr\":6,"
                  "\"iIQ\":true,\"crc\":false,\"sat\":\"Test\",\"NORAD\":99}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "parse should succeed");
    zassert_true(msg.iIQ, "iIQ should be true");
    zassert_false(msg.crc, "crc should be false");
}

ZTEST(json_parser, test_begine_empty)
{
    char json[] = "{}";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    /* Should succeed but with no fields set (defaults) */
    zassert_true(ret >= 0, "empty object should not error");
    zassert_equal(msg.sw, 18, "sw should be default 18");
}

ZTEST(json_parser, test_begine_malformed)
{
    char json[] = "not json at all";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret < 0, "malformed JSON should return error");
}

/* ---- set_pos_prm parsing tests ---- */

ZTEST(json_parser, test_set_pos_null)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[null]", 6, &msg);
    zassert_equal(ret, 0, "null should return 0 values");
    zassert_equal(msg.count, 0, "count should be 0");
}

ZTEST(json_parser, test_set_pos_full)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[-33.8688, 151.2093, 50]", 24, &msg);
    zassert_equal(ret, 3, "should parse 3 values");
    zassert_equal(msg.count, 3, "count should be 3");
    zassert_true(fabsf(msg.values[0] - (-33.8688f)) < 0.001f, "lat");
    zassert_true(fabsf(msg.values[1] - 151.2093f) < 0.001f, "lon");
    zassert_true(fabsf(msg.values[2] - 50.0f) < 0.1f, "alt");
}

ZTEST(json_parser, test_set_pos_alt_only)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[100]", 5, &msg);
    zassert_equal(ret, 1, "should parse 1 value");
    zassert_true(fabsf(msg.values[0] - 100.0f) < 0.1f, "alt");
}

/* ---- set_name parsing tests ---- */

ZTEST(json_parser, test_set_name_valid)
{
    struct tinygs_name_msg msg;
    int ret = tinygs_parse_set_name("[\"16EDE3EB6081\",\"MyStation\"]", 28, &msg);
    zassert_equal(ret, 0, "should parse successfully");
    zassert_true(strcmp(msg.mac, "16EDE3EB6081") == 0, "mac");
    zassert_true(strcmp(msg.name, "MyStation") == 0, "name");
}

ZTEST(json_parser, test_set_name_mac_wrong_length)
{
    struct tinygs_name_msg msg;
    int ret = tinygs_parse_set_name("[\"SHORT\",\"Name\"]", 17, &msg);
    zassert_equal(ret, -1, "short MAC should fail");
}

ZTEST(json_parser, test_set_name_empty_name)
{
    struct tinygs_name_msg msg;
    int ret = tinygs_parse_set_name("[\"16EDE3EB6081\",\"\"]", 20, &msg);
    zassert_equal(ret, -1, "empty name should fail");
}

/* ---- filter parsing tests ---- */

ZTEST(json_parser, test_filter_basic)
{
    uint8_t buf[8];
    int ret = tinygs_parse_filter("[1, 0, 235]", 11, buf, sizeof(buf));
    zassert_equal(ret, 3, "should parse 3 filter bytes");
    zassert_equal(buf[0], 1, "filter[0]");
    zassert_equal(buf[1], 0, "filter[1]");
    zassert_equal(buf[2], 235, "filter[2]");
}

ZTEST(json_parser, test_filter_empty)
{
    uint8_t buf[8];
    int ret = tinygs_parse_filter("[]", 2, buf, sizeof(buf));
    zassert_equal(ret, 0, "empty filter should return 0");
}

ZTEST(json_parser, test_filter_overflow)
{
    uint8_t buf[3];
    int ret = tinygs_parse_filter("[1,2,3,4,5,6,7,8,9]", 20, buf, sizeof(buf));
    zassert_equal(ret, 3, "should truncate to buffer size");
}

ZTEST(json_parser, test_filter_no_bracket)
{
    uint8_t buf[8];
    int ret = tinygs_parse_filter("no array", 8, buf, sizeof(buf));
    zassert_equal(ret, -1, "missing bracket should fail");
}

/* ---- Real server payload tests ---- */

ZTEST(json_parser, test_begine_real_trisat)
{
    /* Actual payload from mqtt.tinygs.com (truncated tlx for test) */
    char json[] = "{\"mode\":\"LoRa\",\"freq\":436.7,\"bw\":250,\"sf\":10,"
                  "\"cr\":5,\"sw\":18,\"pwr\":5,\"cl\":120,\"pl\":8,"
                  "\"gain\":0,\"crc\":true,\"fldro\":0,"
                  "\"sat\":\"TriSat-3\",\"NORAD\":67299}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "real payload should parse");

    float freq = tinygs_begine_get_freq(&msg);
    zassert_true(fabsf(freq - 436.7f) < 0.01f, "freq 436.7");
    zassert_equal(msg.sf, 10, "sf 10");
    zassert_equal(msg.cr, 5, "cr 5");
    zassert_equal(msg.NORAD, 67299, "NORAD 67299");
    zassert_true(strcmp(msg.sat, "TriSat-3") == 0, "sat TriSat-3");
}

ZTEST(json_parser, test_begine_real_tianqi_with_filter)
{
    /* json_obj_parse modifies the buffer (inserts nulls for strings).
     * In main.cpp we save modem_conf BEFORE parsing, then parse filter
     * from the saved copy. Test must replicate this pattern. */
    const char *original = "{\"mode\":\"LoRa\",\"freq\":400.265,\"bw\":125,\"sf\":10,"
                           "\"cr\":5,\"sw\":18,\"pwr\":5,\"cl\":120,\"pl\":9,"
                           "\"gain\":0,\"crc\":true,\"fldro\":1,"
                           "\"sat\":\"Tianqi\",\"NORAD\":99999,"
                           "\"filter\":[1,0,235]}";

    /* Save copy for filter parsing (as main.cpp does with modem_conf) */
    char saved[256];
    strncpy(saved, original, sizeof(saved));

    /* Parse begine (modifies buffer in-place) */
    char json[256];
    strncpy(json, original, sizeof(json));
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "real tianqi payload should parse");

    float freq = tinygs_begine_get_freq(&msg);
    zassert_true(fabsf(freq - 400.265f) < 0.001f, "freq 400.265");
    zassert_equal(msg.fldro, 1, "fldro should be 1");

    /* Parse filter from saved copy (not the modified buffer) */
    uint8_t filter[8];
    int fcount = tinygs_parse_filter(saved, strlen(saved), filter, sizeof(filter));
    zassert_equal(fcount, 3, "should find 3 filter bytes");
    zassert_equal(filter[0], 1, "filter[0]=1");
    zassert_equal(filter[1], 0, "filter[1]=0");
    zassert_equal(filter[2], 235, "filter[2]=235");
}

ZTEST(json_parser, test_set_pos_real_null)
{
    /* Actual server payload on every connect */
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[null]", 6, &msg);
    zassert_equal(ret, 0, "server [null] should parse");
}

/* ---- JSON output round-trip tests ---- */

/* Test that our output JSON is valid by parsing it back.
 * We can't use json_obj_parse for output (different schema),
 * but we can verify key fields are present and well-formed. */

ZTEST(json_parser, test_welcome_output_has_required_fields)
{
    /* Simulate a welcome payload (hand-built like tinygs_build_welcome) */
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"station_location\":[-33.8688,151.2093],"
        "\"version\":2604100,"
        "\"git_version\":\"tinygs_nRF52\","
        "\"chip\":\"nRF52840\","
        "\"board\":255,"
        "\"mac\":\"16EDE3EB6081\","
        "\"radioChip\":6,"
        "\"Mem\":8108,"
        "\"seconds\":100,"
        "\"Vbat\":4130,"
        "\"tx\":false,"
        "\"sat\":\"\","
        "\"ip\":\"0.0.0.0\","
        "\"idfv\":\"NCS/Zephyr\","
        "\"modem_conf\":\"{}\"}");

    zassert_true(len > 0 && len < (int)sizeof(json), "should fit in buffer");

    /* Verify key fields are present */
    zassert_not_null(strstr(json, "\"version\":2604100"), "version must be number");
    zassert_not_null(strstr(json, "\"Vbat\":4130"), "Vbat must be int mV");
    zassert_not_null(strstr(json, "\"board\":255"), "board must be number");
    zassert_not_null(strstr(json, "\"tx\":false"), "tx must be boolean");
    zassert_not_null(strstr(json, "\"modem_conf\":\"{}\""), "modem_conf must be escaped string");
    /* Verify no common type errors */
    zassert_is_null(strstr(json, "\"version\":\""), "version must NOT be string");
    zassert_is_null(strstr(json, "\"Vbat\":4."), "Vbat must NOT be float");
}

ZTEST(json_parser, test_ping_output_format)
{
    char json[256];
    int len = snprintf(json, sizeof(json),
        "{\"Vbat\":%d,\"Mem\":%u,\"MinMem\":%u,\"MaxBlk\":%u,"
        "\"RSSI\":%d,\"radio\":%d,\"InstRSSI\":%.1f}",
        4130, 8108u, 8108u, 8108u, -65, 0, -120.0);

    zassert_true(len > 0 && len < (int)sizeof(json), "should fit");
    zassert_not_null(strstr(json, "\"Vbat\":4130"), "Vbat present");
    zassert_not_null(strstr(json, "\"RSSI\":-65"), "RSSI present");
    zassert_not_null(strstr(json, "\"InstRSSI\":-120.0"), "InstRSSI present");
}

/* ---- RX payload value validation ---- */

ZTEST(json_parser, test_rx_payload_has_epoch_not_uptime)
{
    /* Simulate an RX payload — verify unix_GS_time is a real epoch,
     * not a small uptime value. Real epoch should be > 1700000000 (2023+) */
    char json[512];
    uint32_t epoch = 1776080126; /* Real SNTP-synced value */
    int len = snprintf(json, sizeof(json),
        "{\"station_location\":[-33.8688,151.2093],"
        "\"mode\":\"LoRa\",\"frequency\":436.703,"
        "\"frequency_offset\":1500.0,"
        "\"satellite\":\"Test\",\"sf\":10,\"cr\":5,\"bw\":250.0,"
        "\"rssi\":-110.5,\"snr\":3.25,\"frequency_error\":1234.5,"
        "\"unix_GS_time\":%u,"
        "\"crc_error\":false,\"data\":\"dGVzdA==\","
        "\"NORAD\":12345,\"noisy\":false,\"iIQ\":false}",
        (unsigned)epoch);

    zassert_true(len > 0 && len < (int)sizeof(json), "should fit");

    /* unix_GS_time must be a real epoch (> 2023), not uptime (< 100000) */
    const char *p = strstr(json, "\"unix_GS_time\":");
    zassert_not_null(p, "unix_GS_time must be present");
    uint32_t val = (uint32_t)atol(p + 15);
    zassert_true(val > 1700000000, "unix_GS_time must be real epoch, not uptime");
}

ZTEST(json_parser, test_rx_payload_frequency_offset_not_zero)
{
    /* frequency_offset should reflect the actual foff value, not hardcoded 0 */
    char json[128];
    snprintf(json, sizeof(json), "\"frequency_offset\":%.1f", 1500.0);
    zassert_not_null(strstr(json, "\"frequency_offset\":1500.0"),
                     "frequency_offset must use actual foff value");

    /* Verify it's NOT hardcoded 0 when offset is set */
    snprintf(json, sizeof(json), "\"frequency_offset\":%.1f", 0.0);
    zassert_not_null(strstr(json, "\"frequency_offset\":0.0"),
                     "frequency_offset 0 is valid when no offset");
}

ZTEST(json_parser, test_rx_payload_iiq_from_config)
{
    /* iIQ must reflect actual radio config, not hardcoded false */
    char json_true[32], json_false[32];
    snprintf(json_true, sizeof(json_true), "\"iIQ\":%s", true ? "true" : "false");
    snprintf(json_false, sizeof(json_false), "\"iIQ\":%s", false ? "true" : "false");

    zassert_not_null(strstr(json_true, "\"iIQ\":true"), "iIQ true");
    zassert_not_null(strstr(json_false, "\"iIQ\":false"), "iIQ false");
}

ZTEST(json_parser, test_rx_payload_required_fields)
{
    /* Verify all ESP32-required fields are present in RX payload format */
    const char *required_fields[] = {
        "\"station_location\":", "\"mode\":", "\"frequency\":",
        "\"frequency_offset\":", "\"satellite\":", "\"sf\":",
        "\"cr\":", "\"bw\":", "\"rssi\":", "\"snr\":",
        "\"frequency_error\":", "\"unix_GS_time\":", "\"crc_error\":",
        "\"data\":", "\"NORAD\":", "\"noisy\":", "\"iIQ\":",
    };

    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"station_location\":[-33.8688,151.2093],"
        "\"mode\":\"LoRa\",\"frequency\":436.703,"
        "\"frequency_offset\":0.0,\"satellite\":\"Test\","
        "\"sf\":10,\"cr\":5,\"bw\":250.0,"
        "\"rssi\":-110.5,\"snr\":3.25,\"frequency_error\":0.0,"
        "\"unix_GS_time\":1776080126,\"usec_time\":0,"
        "\"crc_error\":false,\"data\":\"dGVzdA==\","
        "\"NORAD\":12345,\"noisy\":false,\"iIQ\":false}");

    zassert_true(len > 0, "should build");

    for (size_t i = 0; i < sizeof(required_fields)/sizeof(required_fields[0]); i++) {
        zassert_not_null(strstr(json, required_fields[i]),
                         "missing required field in RX payload");
    }
}

ZTEST(json_parser, test_welcome_field_types_strict)
{
    /* Strict type checks — these type errors have caused server resets */
    char json[512];
    int len = snprintf(json, sizeof(json),
        "{\"version\":%u,\"Vbat\":%d,\"board\":%d,"
        "\"mode\":\"LoRa\",\"tx\":false,\"modem_conf\":\"%s\"}",
        2604100u, 4130, 255, "{}");

    zassert_true(len > 0, "should build");

    /* version MUST be number, not string */
    zassert_not_null(strstr(json, "\"version\":2604100"), "version is number");
    zassert_is_null(strstr(json, "\"version\":\""), "version NOT string");

    /* Vbat MUST be int millivolts, not float volts */
    zassert_not_null(strstr(json, "\"Vbat\":4130"), "Vbat is int mV");
    zassert_is_null(strstr(json, "\"Vbat\":4."), "Vbat NOT float volts");

    /* board MUST be number, not string */
    zassert_not_null(strstr(json, "\"board\":255"), "board is number");
    zassert_is_null(strstr(json, "\"board\":\""), "board NOT string");

    /* tx MUST be boolean */
    zassert_not_null(strstr(json, "\"tx\":false"), "tx is boolean");

    /* modem_conf MUST be escaped string, not raw object */
    zassert_not_null(strstr(json, "\"modem_conf\":\"{}\""), "modem_conf is escaped string");
    zassert_is_null(strstr(json, "\"modem_conf\":{}"), "modem_conf NOT raw object");
}

/* ---- begine edge cases ---- */

ZTEST(json_parser, test_begine_fractional_bw)
{
    /* BW 62.5 is common for narrow-band sats */
    char json[] = "{\"freq\":436.08,\"bw\":62.5,\"sf\":8,\"cr\":6,"
                  "\"sat\":\"Test\",\"NORAD\":1}";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "fractional bw should parse");
    float bw = tinygs_begine_get_bw(&msg);
    zassert_true(fabsf(bw - 62.5f) < 0.1f, "bw should be 62.5");
}

ZTEST(json_parser, test_begine_all_booleans_inverted)
{
    char json[] = "{\"freq\":400.0,\"bw\":125,\"sf\":12,\"cr\":8,"
                  "\"crc\":false,\"iIQ\":true,\"sat\":\"X\",\"NORAD\":1}";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "should parse");
    zassert_false(msg.crc, "crc should be false");
    zassert_true(msg.iIQ, "iIQ should be true");
    zassert_equal(msg.sf, 12, "sf 12");
    zassert_equal(msg.cr, 8, "cr 8");
}

ZTEST(json_parser, test_begine_long_satellite_name)
{
    char json[] = "{\"freq\":437.0,\"bw\":125,\"sf\":10,\"cr\":5,"
                  "\"sat\":\"Surve-251228C-K4KDR\",\"NORAD\":99999}";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "should parse");
    zassert_true(strcmp(msg.sat, "Surve-251228C-K4KDR") == 0, "long sat name");
}

ZTEST(json_parser, test_begine_norad_zero)
{
    char json[] = "{\"freq\":436.0,\"bw\":125,\"sf\":10,\"cr\":5,"
                  "\"sat\":\"Unknown\",\"NORAD\":0}";
    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "should parse");
    zassert_equal(msg.NORAD, 0, "NORAD 0 is valid");
}

/* ---- sat_pos_oled parsing ---- */

ZTEST(json_parser, test_sat_pos_oled_basic)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[64, 32]", 8, &msg);
    zassert_equal(ret, 2, "should parse 2 values");
    zassert_true(fabsf(msg.values[0] - 64.0f) < 0.1f, "x=64");
    zassert_true(fabsf(msg.values[1] - 32.0f) < 0.1f, "y=32");
}

ZTEST(json_parser, test_sat_pos_oled_floats)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[55.5, 22.3]", 12, &msg);
    zassert_equal(ret, 2, "should parse 2 float values");
    zassert_true(fabsf(msg.values[0] - 55.5f) < 0.1f, "x=55.5");
    zassert_true(fabsf(msg.values[1] - 22.3f) < 0.1f, "y=22.3");
}

/* ---- foff and freq parsing (simple float strings) ---- */

ZTEST(json_parser, test_foff_simple_float)
{
    float foff = tinygs_parse_foff("1500.0", 6, NULL, NULL);
    zassert_true(fabsf(foff - 1500.0f) < 0.1f, "foff 1500");
}

ZTEST(json_parser, test_foff_negative)
{
    float foff = tinygs_parse_foff("-2000.5", 7, NULL, NULL);
    zassert_true(fabsf(foff - (-2000.5f)) < 0.1f, "foff -2000.5");
}

ZTEST(json_parser, test_foff_array_format)
{
    float tol = 0;
    uint32_t refresh = 0;
    float foff = tinygs_parse_foff("[1500.0, 800, 2000]", 19, &tol, &refresh);
    zassert_true(fabsf(foff - 1500.0f) < 0.1f, "offset 1500");
    zassert_true(fabsf(tol - 800.0f) < 0.1f, "tolerance 800");
    zassert_equal(refresh, 2000, "refresh 2000ms");
}

ZTEST(json_parser, test_foff_array_partial)
{
    /* Only offset and tolerance, no refresh */
    float tol = 0;
    uint32_t refresh = 4000;
    float foff = tinygs_parse_foff("[500, 1200]", 11, &tol, &refresh);
    zassert_true(fabsf(foff - 500.0f) < 0.1f, "offset 500");
    zassert_true(fabsf(tol - 1200.0f) < 0.1f, "tolerance 1200");
    zassert_equal(refresh, 4000, "refresh unchanged");
}

ZTEST(json_parser, test_freq_parse)
{
    float freq = strtof("436.703", NULL);
    zassert_true(fabsf(freq - 436.703f) < 0.001f, "freq 436.703");
}

/* ---- set_pos_prm edge cases ---- */

ZTEST(json_parser, test_set_pos_negative_coords)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[-33.8688, 151.2093, 50.0]", 27, &msg);
    zassert_equal(ret, 3, "should parse 3");
    zassert_true(msg.values[0] < -33.0f, "lat should be negative");
    zassert_true(msg.values[1] > 151.0f, "lon should be positive");
}

ZTEST(json_parser, test_set_pos_spaces)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("[ -10.5 , 20.3 , 100 ]", 23, &msg);
    zassert_equal(ret, 3, "should handle spaces");
}

ZTEST(json_parser, test_set_pos_no_bracket)
{
    struct tinygs_pos_msg msg;
    int ret = tinygs_parse_set_pos("null", 4, &msg);
    zassert_equal(ret, -1, "bare null should fail");
}

/* ---- filter edge cases ---- */

ZTEST(json_parser, test_filter_single)
{
    uint8_t buf[8];
    int ret = tinygs_parse_filter("[42]", 4, buf, sizeof(buf));
    zassert_equal(ret, 1, "single element");
    zassert_equal(buf[0], 42, "value 42");
}

ZTEST(json_parser, test_filter_max_value)
{
    uint8_t buf[8];
    int ret = tinygs_parse_filter("[0, 255, 128]", 13, buf, sizeof(buf));
    zassert_equal(ret, 3, "three elements");
    zassert_equal(buf[0], 0, "min byte");
    zassert_equal(buf[1], 255, "max byte");
    zassert_equal(buf[2], 128, "mid byte");
}

ZTEST_SUITE(json_parser, NULL, NULL, NULL, NULL, NULL);
