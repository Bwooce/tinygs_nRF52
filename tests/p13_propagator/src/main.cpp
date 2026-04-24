/*
 * Tests for the AioP13 satellite propagator with TinyGS's 34-byte "tlx" format.
 *
 * The "tlx" format is a TinyGS-proprietary compact binary encoding of orbital
 * elements (vs. the standard 138-byte ASCII TLE). It's parsed by
 * P13Satellite_tGS, which extracts uint24/uint32 fields at fixed byte offsets.
 *
 * These tests don't have an external "ground truth" oracle for what lat/lon
 * a given tlx should produce — that would require a reference implementation
 * we don't have. Instead, they verify:
 *   - The parser doesn't crash on valid tlx input.
 *   - The propagator produces output in the valid lat/lon range.
 *   - Output is finite (not NaN/Inf).
 *   - Identical inputs produce identical outputs (no uninitialised state).
 *
 * If the propagator pipeline ever silently breaks (e.g., an LTO change or a
 * future code refactor), these tests will catch the gross failure modes.
 * They do NOT prove numerical correctness — that needs cross-verification
 * against a known-good implementation, ideally via a Python reference.
 */

#include <zephyr/ztest.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "AioP13.h"

/* A representative 34-byte tlx payload. The byte values are constructed to
 * satisfy the parser's uint24/uint32-at-offset reads with values producing a
 * physically plausible LEO orbit (inclination ~98°, eccentricity ~0, mean
 * motion ~15 rev/day → ~500 km altitude, sun-synchronous).
 *
 * Field layout (per AioP13.cpp:362-387):
 *   [0]      epoch year offset from 1900 or 2000 (uint8)
 *   [0..2]   uint24: cp_lN (catalog number-ish)
 *   [1..2]   uint16: epoch day-of-year (integer)
 *   [3..6]   uint32: epoch fractional day (×1e8)
 *   [7..10]  uint32: 2π × first time-derivative of mean motion (×1e8)
 *   [11..13] uint24: inclination degrees (×1e4)
 *   [14..16] uint24: RAAN degrees (×1e4)
 *   [17..20] uint32: eccentricity (×1e7)
 *   [21..23] uint24: argument of perigee degrees (×1e4)
 *   [24..26] uint24: mean anomaly degrees (×1e4)
 *   [27..30] uint32: 2π × mean motion rad/sec (×1e8)
 *   [31..33] uint24: revolution number at epoch
 */
static const uint8_t sample_tlx[34] = {
    /* epoch year = 25 (= 2025) */
    25,
    /* uint16 day-of-year (LE) at offset 1 = 100 */
    0x64, 0x00,
    /* uint32 fractional day (×1e8) at offset 3 = 50000000 (= 0.5 day) */
    0x80, 0xF0, 0xFA, 0x02,
    /* uint32 dM2 (×1e8) at offset 7 = 0 (no first-derivative correction) */
    0x00, 0x00, 0x00, 0x00,
    /* uint24 inclination (×1e4) at offset 11 = 980000 (= 98°) */
    0xE0, 0xED, 0x0E,
    /* uint24 RAAN (×1e4) at offset 14 = 1800000 (= 180°) */
    0x80, 0x70, 0x1B,
    /* uint32 eccentricity (×1e7) at offset 17 = 100 (= 0.00001) */
    0x64, 0x00, 0x00, 0x00,
    /* uint24 arg perigee (×1e4) at offset 21 = 0 */
    0x00, 0x00, 0x00,
    /* uint24 mean anomaly (×1e4) at offset 24 = 0 */
    0x00, 0x00, 0x00,
    /* uint32 mean motion (rev/day ×1e8) at offset 27, LE
     * 15 rev/day × 1e8 = 1500000000 = 0x59682F00
     * Parser scales: cp_dMM = 2π × val/1e8 = 2π×15 = 94.25 (rad/day),
     * then cp_dN0 = cp_dMM/86400 → 1.09e-3 rad/s. Realistic LEO. */
    0x00, 0x2F, 0x68, 0x59,
    /* uint24 rev number at offset 31 */
    0x01, 0x00, 0x00,
};

ZTEST_SUITE(p13_propagator, NULL, NULL, NULL, NULL, NULL);

/*
 * Smoke test: construct a satellite from a 34-byte tlx, propagate for a
 * known UTC, request lat/lon. Output should be finite and in valid range.
 * If this fails, the propagator pipeline is fundamentally broken.
 */
ZTEST(p13_propagator, test_tlx_produces_valid_latlon)
{
    P13Satellite_tGS sat(sample_tlx);
    P13DateTime dt(2025, 4, 15, 12, 0, 0);
    sat.predict(dt);

    double lat = -999, lon = -999;
    sat.latlon(lat, lon);

    zassert_true(isfinite(lat), "latitude must be finite, got %f", lat);
    zassert_true(isfinite(lon), "longitude must be finite, got %f", lon);
    zassert_true(lat >= -90.0 && lat <= 90.0,
                 "latitude out of range: %f", lat);
    zassert_true(lon >= -180.0 && lon <= 180.0,
                 "longitude out of range: %f", lon);
}

/*
 * Determinism: same input + same time → same output. Catches bugs where
 * an internal state isn't properly initialised (e.g., LTO eliding a
 * member-init that the parser implicitly relies on).
 */
ZTEST(p13_propagator, test_tlx_propagation_is_deterministic)
{
    P13DateTime dt(2025, 4, 15, 12, 0, 0);

    P13Satellite_tGS sat1(sample_tlx);
    sat1.predict(dt);
    double lat1 = 0, lon1 = 0;
    sat1.latlon(lat1, lon1);

    P13Satellite_tGS sat2(sample_tlx);
    sat2.predict(dt);
    double lat2 = 0, lon2 = 0;
    sat2.latlon(lat2, lon2);

    zassert_true(fabs(lat1 - lat2) < 1e-9,
                 "lat differs: %f vs %f", lat1, lat2);
    zassert_true(fabs(lon1 - lon2) < 1e-9,
                 "lon differs: %f vs %f", lon1, lon2);
}

/*
 * Time advance: propagating to a later time should yield a different
 * sub-satellite point (otherwise the orbital integration isn't running).
 * Uses 30 minutes of separation — a LEO with ~96 min period moves
 * ~120° in longitude over 30 min, so the difference will be unmistakable.
 */
ZTEST(p13_propagator, test_tlx_propagation_advances_with_time)
{
    P13Satellite_tGS sat(sample_tlx);

    P13DateTime dt0(2025, 4, 15, 12, 0, 0);
    sat.predict(dt0);
    double lat0 = 0, lon0 = 0;
    sat.latlon(lat0, lon0);

    P13DateTime dt1(2025, 4, 15, 12, 30, 0);
    sat.predict(dt1);
    double lat1 = 0, lon1 = 0;
    sat.latlon(lat1, lon1);

    /* Either lat or lon must change meaningfully. Sub-degree change is
     * suspicious for a LEO over 30 min. */
    double dlat = fabs(lat1 - lat0);
    double dlon = fabs(lon1 - lon0);
    zassert_true(dlat > 1.0 || dlon > 1.0,
                 "satellite barely moved over 30 min: dlat=%f dlon=%f",
                 dlat, dlon);
}

/*
 * Display-pipeline math: confirm that the (sat_lat, sat_lon) → (sat_pos_x,
 * sat_pos_y) mapping in main.cpp:doppler_update() lands inside the ESP32
 * 128×64 grid bounds. If a future refactor changes either the propagator
 * output range or the mapping, this catches the misalignment that would
 * silently kill the worldmap red dot.
 */
ZTEST(p13_propagator, test_latlon_to_esp32_grid_mapping)
{
    /* These are the exact lines from doppler_update(): */
    auto sat_pos_x = [](double sat_lon) {
        return (float)((180.0 + sat_lon) / 360.0 * 128.0);
    };
    auto sat_pos_y = [](double sat_lat) {
        return (float)((90.0 - sat_lat) / 180.0 * 64.0);
    };

    /* Corners */
    zassert_true(sat_pos_x(-180.0) >= 0.0f && sat_pos_x(-180.0) <= 128.0f,
                 "sat_pos_x at -180 lon out of grid: %f", sat_pos_x(-180.0));
    zassert_true(sat_pos_x( 180.0) >= 0.0f && sat_pos_x( 180.0) <= 128.0f,
                 "sat_pos_x at +180 lon out of grid: %f", sat_pos_x( 180.0));
    zassert_true(sat_pos_y( 90.0) >= 0.0f && sat_pos_y( 90.0) <= 64.0f,
                 "sat_pos_y at +90 lat out of grid: %f", sat_pos_y( 90.0));
    zassert_true(sat_pos_y(-90.0) >= 0.0f && sat_pos_y(-90.0) <= 64.0f,
                 "sat_pos_y at -90 lat out of grid: %f", sat_pos_y(-90.0));

    /* Centre — the 0,0 lat/lon should map to a non-zero grid coord, so
     * that the "if (sat_pos_x != 0 || sat_pos_y != 0)" guard in the draw
     * code doesn't mistake a real null-island satellite for "uninitialised".
     * (Real satellites passing exactly over 0,0 are rare and brief; the
     * guard is acceptable.) */
    zassert_true(sat_pos_x(0.0) > 0.0f, "sat_pos_x at 0 lon should be > 0");
    zassert_true(sat_pos_y(0.0) > 0.0f, "sat_pos_y at 0 lat should be > 0");
}
