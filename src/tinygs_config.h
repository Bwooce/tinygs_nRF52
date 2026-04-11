#ifndef TINYGS_CONFIG_H
#define TINYGS_CONFIG_H

/**
 * @file tinygs_config.h
 * @brief Persistent configuration store using Zephyr Settings (NVS backend).
 *
 * Uses Zephyr's settings subsystem with key prefix "tgs/" to coexist with
 * OpenThread's "ot/" keys on the same NVS partition. NVS provides automatic
 * flash wear-leveling.
 *
 * New config items are added by defining a new key — old firmware that doesn't
 * know about the key simply ignores it, and new firmware uses a default if the
 * key isn't present. No version number required for forward compatibility.
 *
 * A schema version is stored at "tgs/ver" for backward-incompatible changes
 * (e.g., changing a key's value format). On load, if ver < TINYGS_CONFIG_VER,
 * a migration callback can transform old values.
 */

#include <stdint.h>
#include <stddef.h>

/* Current config schema version — bump when a key's format changes */
#define TINYGS_CONFIG_VER 1

/**
 * @brief Initialize the settings subsystem and load saved config.
 *
 * Must be called after kernel init, before MQTT connect.
 * Populates the runtime config variables (tinygs_station_lat, cfg_station, etc.)
 * from NVS. Missing keys use compiled defaults.
 *
 * @return 0 on success, negative errno on failure.
 */
/* Runtime config — owned by tinygs_config.cpp, used everywhere */
extern char cfg_station[32];
extern char cfg_mqtt_user[64];
extern char cfg_mqtt_pass[64];

int tinygs_config_init(void);

/**
 * @brief Save a single config key to NVS.
 *
 * @param key  Short key name (without "tgs/" prefix), e.g. "lat", "station".
 * @param data Pointer to value.
 * @param len  Value size in bytes.
 * @return 0 on success, negative errno on failure.
 */
int tinygs_config_save(const char *key, const void *data, size_t len);

/**
 * @brief Save station location to NVS.
 */
int tinygs_config_save_location(float lat, float lon, float alt);

/**
 * @brief Save station name to NVS.
 */
int tinygs_config_save_station(const char *name);

/**
 * @brief Save modem_conf (last begine/batch_conf payload) to NVS.
 */
int tinygs_config_save_modem_conf(const char *conf);

#endif /* TINYGS_CONFIG_H */
