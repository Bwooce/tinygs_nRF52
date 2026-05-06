#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of timezones in the table (parity with ESP32 IotWebConf). */
extern const size_t tinygs_tz_count;

/* Return the Olson zone name for a given index (e.g. "Australia/Sydney"),
 * or "?" if out of range. Pointer is to const flash storage. */
const char *tinygs_tz_get_name(uint16_t idx);

/* Return the POSIX TZ rule string with the leading 3-digit index prefix
 * stripped (e.g. "AEST-10AEDT,M10.1.0,M4.1.0/3"). */
const char *tinygs_tz_get_rule(uint16_t idx);

/* Apply the TZ at idx via setenv("TZ", ...) + tzset() so localtime_r()
 * picks up the rule. Out-of-range falls back to UTC. */
void tinygs_tz_apply(uint16_t idx);

#ifdef __cplusplus
}
#endif
