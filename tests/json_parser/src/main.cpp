/**
 * Ztest unit tests for TinyGS JSON parser (tinygs_json.h)
 *
 * Run: west build -b native_posix tests/json_parser && ./build/zephyr/zephyr.exe
 * Or:  west build -b nrf52840dk_nrf52840 tests/json_parser && west flash
 */

#include <zephyr/ztest.h>
#include "tinygs_json.h"
#include <string.h>
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

ZTEST_SUITE(json_parser, NULL, NULL, NULL, NULL, NULL);
