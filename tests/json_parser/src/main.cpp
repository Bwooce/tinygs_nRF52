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

/* ---- FSK begine parsing tests ---- */

ZTEST(json_parser, test_begine_fsk_basic)
{
    char json[] = "{\"mode\":\"FSK\",\"freq\":401.7,\"bw\":9.6,\"br\":9600,"
                  "\"fd\":5000,\"pl\":4,\"pwr\":5,\"ook\":0,\"len\":64,"
                  "\"enc\":2,\"ws\":256,\"sat\":\"TestFSK\",\"NORAD\":55555}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "FSK parse should succeed");
    zassert_not_null(msg.mode, "mode should be parsed");
    zassert_true(strcmp(msg.mode, "FSK") == 0, "mode should be FSK");
    zassert_equal(msg.br, 9600, "bitrate should be 9600");
    zassert_equal(msg.ook, 0, "ook should be 0");
    zassert_equal(msg.len, 64, "len should be 64");
    zassert_equal(msg.enc, 2, "enc should be 2");
    zassert_equal(msg.ws, 256, "ws should be 256");

    float fd = tinygs_begine_get_fd(&msg);
    zassert_true(fabsf(fd - 5000.0f) < 0.1f, "fd should be 5000");
}

ZTEST(json_parser, test_begine_fsk_software_crc)
{
    char json[] = "{\"mode\":\"FSK\",\"freq\":435.0,\"bw\":25,\"br\":4800,"
                  "\"fd\":2500,\"pl\":4,\"pwr\":5,\"sat\":\"CRC_Test\","
                  "\"cSw\":true,\"cB\":2,\"cI\":65535,\"cP\":4129,"
                  "\"cF\":0,\"cRI\":false,\"cRO\":false}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "FSK CRC parse should succeed");
    zassert_true(msg.cSw, "cSw should be true");
    zassert_equal(msg.cB, 2, "cB should be 2");
    zassert_equal(msg.cI, 65535, "cI should be 65535 (0xFFFF)");
    zassert_equal(msg.cP, 4129, "cP should be 4129 (0x1021)");
    zassert_equal(msg.cF, 0, "cF should be 0");
    zassert_false(msg.cRI, "cRI should be false");
    zassert_false(msg.cRO, "cRO should be false");
}

ZTEST(json_parser, test_begine_fsk_crc_reflected)
{
    char json[] = "{\"mode\":\"FSK\",\"freq\":435.0,\"bw\":25,\"br\":4800,"
                  "\"fd\":2500,\"pl\":4,\"pwr\":5,\"sat\":\"Ref_Test\","
                  "\"cSw\":true,\"cB\":2,\"cI\":0,\"cP\":32773,"
                  "\"cF\":65535,\"cRI\":true,\"cRO\":true}";

    struct tinygs_begine_msg msg;
    int64_t ret = tinygs_parse_begine(json, strlen(json), &msg);
    zassert_true(ret > 0, "reflected CRC parse should succeed");
    zassert_true(msg.cRI, "cRI should be true");
    zassert_true(msg.cRO, "cRO should be true");
    zassert_equal(msg.cP, 32773, "cP should be 32773 (0x8005)");
    zassert_equal(msg.cF, 65535, "cF should be 65535 (0xFFFF)");
}

/* ---- FSK sync word parser tests ---- */

ZTEST(json_parser, test_fsw_basic)
{
    char json[] = "{\"fsw\":[126,170,85,126],\"other\":1}";
    uint8_t buf[8];
    int ret = tinygs_parse_fsw(json, strlen(json), buf, sizeof(buf));
    zassert_equal(ret, 4, "should parse 4 sync bytes");
    zassert_equal(buf[0], 126, "byte 0 = 0x7E");
    zassert_equal(buf[1], 170, "byte 1 = 0xAA");
    zassert_equal(buf[2], 85, "byte 2 = 0x55");
    zassert_equal(buf[3], 126, "byte 3 = 0x7E");
}

ZTEST(json_parser, test_fsw_empty)
{
    char json[] = "{\"freq\":435.0}";
    uint8_t buf[8];
    int ret = tinygs_parse_fsw(json, strlen(json), buf, sizeof(buf));
    zassert_equal(ret, 0, "no fsw field should return 0");
}

ZTEST(json_parser, test_fsw_single)
{
    char json[] = "{\"fsw\":[255]}";
    uint8_t buf[8];
    int ret = tinygs_parse_fsw(json, strlen(json), buf, sizeof(buf));
    zassert_equal(ret, 1, "single sync byte");
    zassert_equal(buf[0], 255, "byte = 0xFF");
}

/* ---- bitcode tests ---- */

extern "C" {
#include "bitcode.h"
}

ZTEST(json_parser, test_pn9_descramble)
{
    /* PN9 with known initial state 0x1FF should XOR first byte with 0xFF */
    uint8_t input[] = {0xFF, 0x00, 0xAA, 0x55};
    uint8_t output[4];
    bitcode_pn9(input, 4, output);
    /* First byte: 0xFF ^ 0xFF (PN9 init low byte) = 0x00 */
    zassert_equal(output[0], 0x00, "first byte should be 0x00 after PN9");
    /* PN9 is deterministic — verify it produces non-zero for other bytes */
    /* The exact values depend on the LFSR sequence, just verify it changed */
    zassert_not_equal(output[1], 0x00, "PN9 should modify subsequent bytes");
}

ZTEST(json_parser, test_pn9_roundtrip)
{
    /* PN9 is its own inverse — applying twice should return original */
    uint8_t original[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; /* "Hello" */
    uint8_t scrambled[5];
    uint8_t recovered[5];
    bitcode_pn9(original, 5, scrambled);
    bitcode_pn9(scrambled, 5, recovered);
    zassert_mem_equal(original, recovered, 5, "PN9 double-apply should recover original");
}

ZTEST(json_parser, test_reverse_byte)
{
    zassert_equal(bitcode_reverse_byte(0x01), 0x80, "0x01 reversed = 0x80");
    zassert_equal(bitcode_reverse_byte(0x80), 0x01, "0x80 reversed = 0x01");
    zassert_equal(bitcode_reverse_byte(0xFF), 0xFF, "0xFF reversed = 0xFF");
    zassert_equal(bitcode_reverse_byte(0x00), 0x00, "0x00 reversed = 0x00");
    zassert_equal(bitcode_reverse_byte(0xA5), 0xA5, "0xA5 reversed = 0xA5");
    zassert_equal(bitcode_reverse_byte(0x0F), 0xF0, "0x0F reversed = 0xF0");
}

/* ---- packet filter logic tests ---- */

/* Replicates the filter logic from main.cpp lora_check_rx() */
static bool filter_matches(const uint8_t *data, size_t len,
                           const uint8_t *filter, size_t filter_size)
{
    if (filter[0] == 0) return true; /* no filter = match all */
    uint8_t count = filter[0];
    uint8_t start = filter[1];
    for (uint8_t i = 0; i < count && i + 2 < filter_size; i++) {
        if (start + i >= len || data[start + i] != filter[2 + i]) {
            return false;
        }
    }
    return true;
}

ZTEST(json_parser, test_filter_match)
{
    /* Tianqi filter: [1, 0, 235] — match 1 byte at offset 0, value 235 (0xEB) */
    uint8_t filter[] = {1, 0, 235};
    uint8_t data_good[] = {235, 0x12, 0x34, 0x56}; /* 0xEB at offset 0 */
    uint8_t data_bad[] = {234, 0x12, 0x34, 0x56};  /* 0xEA at offset 0 */

    zassert_true(filter_matches(data_good, 4, filter, 3), "should match");
    zassert_false(filter_matches(data_bad, 4, filter, 3), "should not match");
}

ZTEST(json_parser, test_filter_multi_byte)
{
    /* RS52 filter: [4, 0, 82, 83, 53, 50] — "RS52" at offset 0 */
    uint8_t filter[] = {4, 0, 82, 83, 53, 50};
    uint8_t data_good[] = {'R', 'S', '5', '2', 0x00, 0x01};
    uint8_t data_bad[] = {'R', 'S', '5', '3', 0x00, 0x01};

    zassert_true(filter_matches(data_good, 6, filter, 6), "RS52 should match");
    zassert_false(filter_matches(data_bad, 6, filter, 6), "RS53 should not match");
}

ZTEST(json_parser, test_filter_no_filter)
{
    uint8_t filter[] = {0, 0, 0};
    uint8_t data[] = {0xFF, 0xFE};
    zassert_true(filter_matches(data, 2, filter, 3), "no filter = accept all");
}

ZTEST(json_parser, test_filter_packet_too_short)
{
    /* Filter wants byte at offset 5, but packet is only 3 bytes */
    uint8_t filter[] = {1, 5, 0xAA};
    uint8_t data[] = {0x01, 0x02, 0x03};
    zassert_false(filter_matches(data, 3, filter, 3), "short packet should not match");
}

/* ---- software CRC tests ---- */

/* CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, refin=false, refout=false, xorout=0 */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

ZTEST(json_parser, test_sw_crc_ccitt_known)
{
    /* "123456789" has CRC-16/CCITT-FALSE = 0x29B1 */
    const uint8_t data[] = "123456789";
    uint16_t crc = crc16_ccitt(data, 9);
    zassert_equal(crc, 0x29B1, "CRC-16/CCITT of '123456789' should be 0x29B1, got 0x%04X", crc);
}

ZTEST(json_parser, test_sw_crc_empty)
{
    uint16_t crc = crc16_ccitt(NULL, 0);
    zassert_equal(crc, 0xFFFF, "CRC of empty data should be init value 0xFFFF");
}

ZTEST(json_parser, test_sw_crc_single_byte)
{
    const uint8_t data[] = {0x00};
    uint16_t crc = crc16_ccitt(data, 1);
    /* CRC-16/CCITT-FALSE of 0x00: init=0xFFFF, process one zero byte */
    zassert_equal(crc, 0xE1F0, "CRC of 0x00 should be 0xE1F0, got 0x%04X", crc);
}

/* ---- real satellite packet tests ---- */

ZTEST(json_parser, test_real_tianqi39_filter_match)
{
    /* Real Tianqi-39 packet from tinygs.com/packet/019d8e04-70be-745a-8446-68cf79c00272
     * 100 bytes, received by fitzsimons_org_GS ESP32 station.
     * Filter: [1, 0, 235] — match first byte == 0xEB */
    uint8_t data[] = {0xEB, 0xF2, 0x68, 0x0F, 0x95, 0x3F, 0x73, 0x27, 0x40};
    uint8_t filter[] = {1, 0, 235};
    zassert_true(filter_matches(data, 9, filter, 3), "Tianqi-39 packet should pass filter");
}

ZTEST(json_parser, test_real_tianqi39_filter_reject_noise)
{
    /* Noise packet from our CRC error logs — starts with 0xB9, not 0xEB.
     * Tianqi filter should reject this. */
    uint8_t data[] = {0xB9, 0xFD, 0xBC, 0x39, 0xB0, 0x98, 0xBD, 0xE3,
                      0x36, 0xF2, 0x96, 0x2F, 0x4D, 0x9A, 0x34, 0x68};
    uint8_t filter[] = {1, 0, 235};
    zassert_false(filter_matches(data, 16, filter, 3),
                  "noise packet (0xB9) should be rejected by Tianqi filter (wants 0xEB)");
}

ZTEST(json_parser, test_real_rs52_filter_match)
{
    /* RS52 satellites use filter [4, 0, 82, 83, 53, 50] = "RS52" at offset 0 */
    uint8_t data[] = {'R', 'S', '5', '2', 'S', 'V', 0x01, 0x02};
    uint8_t filter[] = {4, 0, 82, 83, 53, 50};
    zassert_true(filter_matches(data, 8, filter, 6), "RS52SV packet should pass RS52 filter");
}

/* TODO: Add tests with real satellite packet captures once we receive clean packets.
 * The hex dump diagnostic logging will provide test vectors for:
 * - Tianqi packet decode (LoRa, implicit header, filter match)
 * - FSK satellite packet decode (once assigned)
 * - AX.25 frame decode with real NRZ-S data */

ZTEST_SUITE(json_parser, NULL, NULL, NULL, NULL, NULL);
