#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <openthread/thread.h>
#include <openthread/ip6.h>

#include <zephyr/usb/usb_device.h>
#include <zephyr/fs/fs.h>
#include <ff.h>

LOG_MODULE_REGISTER(tinygs_nrf52, LOG_LEVEL_DBG);

/* The mqtt client struct */
// static struct mqtt_client client_ctx;

/* MQTT Broker details */
#define SERVER_ADDR "mqtt.tinygs.com"
#define SERVER_PORT 8883
#define APP_CONNECT_TIMEOUT_MS 2000
#define APP_SLEEP_MSECS 500

/* Buffers for MQTT client */
// static uint8_t rx_buffer[256];
// static uint8_t tx_buffer[256];
// static uint8_t payload_buf[256];

/**
 * OpenThread State Change Callback
 */
static void ot_state_changed_handler(otChangedFlags flags, struct openthread_context *ot_context, void *user_data)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        switch (otThreadGetDeviceRole(ot_context->instance)) {
        case OT_DEVICE_ROLE_CHILD:
            LOG_INF("OpenThread Role: Child (Connected)");
            break;
        case OT_DEVICE_ROLE_ROUTER:
            LOG_INF("OpenThread Role: Router (Connected)");
            break;
        case OT_DEVICE_ROLE_LEADER:
            LOG_INF("OpenThread Role: Leader (Connected)");
            break;
        case OT_DEVICE_ROLE_DETACHED:
            LOG_INF("OpenThread Role: Detached");
            break;
        case OT_DEVICE_ROLE_DISABLED:
            LOG_INF("OpenThread Role: Disabled");
            break;
        }
    }
}

static struct openthread_state_changed_cb ot_state_cb = {
    .state_changed_cb = ot_state_changed_handler,
};

/**
 * Initialize OpenThread Network
 */
static void init_openthread(void)
{
    LOG_INF("Initializing OpenThread...");
    openthread_state_changed_cb_register(openthread_get_default_context(), &ot_state_cb);
    openthread_start(openthread_get_default_context());
}

/* -------------------------------------------------------------------------- */
/* USB MSC & FATFS Configuration UI */
/* -------------------------------------------------------------------------- */

static FATFS fat_fs;
static struct fs_mount_t mp = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &fat_fs,
};

const char *html_content = 
    "<!DOCTYPE html><html><head><title>TinyGS Configurator</title>"
    "<style>body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:20px;}</style>"
    "</head><body>"
    "<h1>TinyGS Setup</h1>"
    "<p><b>MAC Address (EUI-64):</b> <span id='mac'>Loading...</span></p>"
    "<p><b>Joiner Password (PSKd):</b> <span id='pskd'>J01NME</span></p>"
    "<hr>"
    "<label>Station ID:</label><br><input type='text' id='station' value='MyTinyGS'><br><br>"
    "<label>MQTT Password:</label><br><input type='password' id='pass' value='ChangeMe'><br><br>"
    "<button onclick='saveConfig()'>Save config.txt</button>"
    "<script>"
    "function saveConfig() {"
    "  const content = 'STATION_ID=' + document.getElementById('station').value + '\\n' +"
    "                  'MQTT_PASSWORD=' + document.getElementById('pass').value + '\\n';"
    "  const blob = new Blob([content], { type: 'text/plain' });"
    "  const a = document.createElement('a');"
    "  a.href = URL.createObjectURL(blob);"
    "  a.download = 'config.txt';"
    "  a.click();"
    "}"
    "</script>"
    "</body></html>";

const char *default_config = 
    "STATION_ID=MyTinyGS\n"
    "MQTT_PASSWORD=ChangeMe\n";

static void setup_usb_storage(void) {
    struct fs_file_t file;
    struct fs_dirent entry;

    LOG_INF("Mounting FATFS /NAND: ...");
    int res = fs_mount(&mp);

    if (res != 0) {
        LOG_INF("Formatting /NAND: partition...");
        res = fs_mkfs(FS_FATFS, (uintptr_t)mp.mnt_point, &mp, 0);
        if (res == 0) {
            res = fs_mount(&mp);
        } else {
            LOG_ERR("Failed to format: %d", res);
            return;
        }
    }

    if (res == 0) {
        LOG_INF("FATFS Mounted successfully!");
        fs_file_t_init(&file);

        /* Create index.html if missing */
        if (fs_stat("/NAND:/index.html", &entry) != 0) {
            LOG_INF("Creating index.html...");
            if (fs_open(&file, "/NAND:/index.html", FS_O_CREATE | FS_O_WRITE) == 0) {
                fs_write(&file, html_content, strlen(html_content));
                fs_close(&file);
            }
        }

        /* Create config.txt if missing */
        if (fs_stat("/NAND:/config.txt", &entry) != 0) {
            LOG_INF("Creating default config.txt...");
            if (fs_open(&file, "/NAND:/config.txt", FS_O_CREATE | FS_O_WRITE) == 0) {
                fs_write(&file, default_config, strlen(default_config));
                fs_close(&file);
            }
        }
    }

    /* Enable USB subsystem */
    res = usb_enable(NULL);
    if (res != 0) {
        LOG_ERR("Failed to enable USB: %d", res);
    } else {
        LOG_INF("USB MSC enabled. Plug into PC to configure.");
    }
}

/**
 * Main Application Loop
 */
int main(void)
{
    LOG_INF("TinyGS nRF52 Port - Phase 1: TLS over Thread PoC");
    LOG_INF("Board: Heltec T114");

    setup_usb_storage();
    init_openthread();

    /* The main loop will eventually handle the state machine for MQTT keep-alives 
       and checking for LoRa packets. For now, we just sleep to let the OpenThread 
       stack connect to the Border Router in the background. */
    while (1) {
        k_msleep(APP_SLEEP_MSECS);
    }

    return 0;
}
