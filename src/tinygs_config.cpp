#include "tinygs_config.h"
#include "tinygs_protocol.h"
#include "mqtt_credentials.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <string.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(tinygs_config, LOG_LEVEL_INF);

/* Runtime config variables — shared with main.cpp via tinygs_config.h externs */
char cfg_station[32] = "tinygs_nrf52_poc";
char cfg_mqtt_user[64] = MQTT_USERNAME;
char cfg_mqtt_pass[64] = MQTT_PASSWORD;
/* tinygs_station_lat/lon/alt and tinygs_radio are in tinygs_protocol.cpp */

/*
 * Settings load callback — called by settings_load_subtree("tgs") for each
 * key found in NVS. Populates runtime variables from stored values.
 */
static int tgs_settings_set(const char *name, size_t len,
                             settings_read_cb read_cb, void *cb_arg)
{
    if (!strcmp(name, "lat")) {
        float val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_station_lat = val;
        }
    } else if (!strcmp(name, "lon")) {
        float val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_station_lon = val;
        }
    } else if (!strcmp(name, "alt")) {
        float val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_station_alt = val;
        }
    } else if (!strcmp(name, "station")) {
        if (len < sizeof(cfg_station)) {
            read_cb(cb_arg, cfg_station, len);
            cfg_station[len] = '\0';
        }
    } else if (!strcmp(name, "user")) {
        if (len < sizeof(cfg_mqtt_user)) {
            read_cb(cb_arg, cfg_mqtt_user, len);
            cfg_mqtt_user[len] = '\0';
        }
    } else if (!strcmp(name, "pass")) {
        if (len < sizeof(cfg_mqtt_pass)) {
            read_cb(cb_arg, cfg_mqtt_pass, len);
            cfg_mqtt_pass[len] = '\0';
        }
    } else if (!strcmp(name, "modem")) {
        if (len < sizeof(tinygs_radio.modem_conf)) {
            read_cb(cb_arg, tinygs_radio.modem_conf, len);
            tinygs_radio.modem_conf[len] = '\0';
        }
    } else if (!strcmp(name, "freq")) {
        float val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_radio.frequency = val;
        }
    } else if (!strcmp(name, "sf")) {
        int val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_radio.sf = val;
        }
    } else if (!strcmp(name, "bw")) {
        float val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_radio.bw = val;
        }
    } else if (!strcmp(name, "cr")) {
        int val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_radio.cr = val;
        }
    } else if (!strcmp(name, "sat")) {
        if (len < sizeof(tinygs_radio.satellite)) {
            read_cb(cb_arg, tinygs_radio.satellite, len);
            tinygs_radio.satellite[len] = '\0';
        }
    } else if (!strcmp(name, "norad")) {
        uint32_t val;
        if (len == sizeof(val) && read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
            tinygs_radio.norad = val;
        }
    }

    return 0;
}

static struct settings_handler tgs_handler = {
    .name = "tgs",
    .h_set = tgs_settings_set,
};

int tinygs_config_init(void)
{
    int ret = settings_subsys_init();
    if (ret) {
        LOG_ERR("settings_subsys_init failed: %d", ret);
        return ret;
    }

    ret = settings_register(&tgs_handler);
    if (ret) {
        LOG_ERR("settings_register failed: %d", ret);
        return ret;
    }

    ret = settings_load_subtree("tgs");
    if (ret) {
        LOG_ERR("settings_load_subtree failed: %d", ret);
        return ret;
    }

    LOG_INF("Config loaded: station=%s lat=%.4f lon=%.4f alt=%.0f",
            cfg_station,
            (double)tinygs_station_lat,
            (double)tinygs_station_lon,
            (double)tinygs_station_alt);

    return 0;
}

int tinygs_config_save(const char *key, const void *data, size_t len)
{
    char path[32];
    snprintf(path, sizeof(path), "tgs/%s", key);
    int ret = settings_save_one(path, data, len);
    if (ret) {
        LOG_ERR("settings_save %s failed: %d", path, ret);
    }
    return ret;
}

int tinygs_config_save_location(float lat, float lon, float alt)
{
    int ret = 0;
    ret |= tinygs_config_save("lat", &lat, sizeof(lat));
    ret |= tinygs_config_save("lon", &lon, sizeof(lon));
    ret |= tinygs_config_save("alt", &alt, sizeof(alt));
    if (ret == 0) {
        LOG_INF("Location saved: lat=%.4f lon=%.4f alt=%.0f",
                (double)lat, (double)lon, (double)alt);
    }
    return ret;
}

int tinygs_config_save_station(const char *name)
{
    return tinygs_config_save("station", name, strlen(name));
}

int tinygs_config_save_modem_conf(const char *conf)
{
    return tinygs_config_save("modem", conf, strlen(conf));
}
