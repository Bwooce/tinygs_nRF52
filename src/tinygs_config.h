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
extern char cfg_adv_prm[256];  /* Opaque advanced-params blob from set_adv_prm */

/* Web UI admin password (Phase 4). Used for HTTP Basic auth on /config,
 * /restart, and /cs command writes. Default "tinygs" at first boot —
 * user is expected to change it via /config. The default is documented
 * to the user via the form's placeholder text; we don't enforce a
 * change, since out-of-the-box flow on Thread is already LAN-trusted. */
extern char cfg_admin_pw[32];

/* TX enable. Default 0 (RX-only). When 0, the station advertises tx:false in
 * welcome + RX payloads (server then never schedules tx commands) and the
 * device-side tx command handler short-circuits. Set via config.json
 * "tx_enable": 1. Operator is responsible for antenna, licensing, and
 * regulatory compliance before enabling. */
extern int8_t cfg_tx_enable;

/* Timezone index into tinygs_tz_values[] (0..tinygs_tz_count-1). Default 456
 * = Etc/UTC. Applied via tinygs_tz_apply() after NVS load and any /config
 * POST; localtime_r() then formats web-UI / log timestamps in the user's
 * zone with DST handled by the embedded POSIX TZ string. */
extern uint16_t cfg_tz_idx;

/* OpenThread/network console log verbosity, on the OT log-level scale:
 * 0=NONE, 1=CRIT, 2=WARN, 3=NOTE. Default 2 (WARN) — silences the steady-state
 * MeshForwarder "Dropping rx (frag) frame" flood (emitted at NOTE, benign
 * multicast reassembly timeouts). Set 3 (NOTE) via config.json "log_level" to
 * restore MLE attach/role/parent NOTES for an RF/attach debug session, without
 * a reflash. Applied at boot (and after /config edits) via tinygs_apply_log_level().
 * Compile-time max is NOTE (prj.conf), so values >3 have no effect. App/ERR
 * logs are unaffected — this only gates the high-volume OT network stack. */
extern int8_t cfg_log_level;

int tinygs_config_init(void);

/**
 * @brief Push cfg_log_level into OpenThread via otLoggingSetLevel().
 *
 * Safe to call once OpenThread is initialised. Clamps to [0,3] (the compiled
 * NOTE ceiling). No-op difference if OT rejects an out-of-range level.
 */
void tinygs_apply_log_level(void);

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

/* Snapshot of the last config.json content we wrote to FATFS. Used by the
 * boot-time sync logic to distinguish "user edited the file" (file ≠
 * snapshot) from "NVS got updated at runtime" (file = snapshot, NVS ≠
 * memory). Sized to fit our current ~200 byte config.json with headroom. */
#define TINYGS_CONFIG_SNAPSHOT_MAX 384
extern char cfg_last_snapshot[TINYGS_CONFIG_SNAPSHOT_MAX];

/**
 * @brief Save the current config.json snapshot to NVS. Call after each
 * fresh write of config.json to FATFS.
 */
int tinygs_config_save_snapshot(const char *content);

/**
 * @brief Save radio state (freq, sf, bw, cr, satellite, norad) to NVS.
 */
int tinygs_config_save_radio(void);

#endif /* TINYGS_CONFIG_H */
