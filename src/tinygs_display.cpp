/**
 * TinyGS Display — Multi-page status display for ST7789V 240x135 TFT.
 *
 * Uses Zephyr CFB (Character Framebuffer) for text rendering.
 * Pages cycle every 5 seconds:
 *   Page 0: Station info (name, version, uptime, vbat)
 *   Page 1: Satellite (name, freq, SF/BW/CR)
 *   Page 2: System (Thread, MQTT, heap)
 *
 * Optional — all functions are no-ops if no display detected.
 */

#include "tinygs_display.h"
#include "tinygs_protocol.h"
#include "tinygs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(tinygs_display, LOG_LEVEL_INF);

static const struct device *disp_dev = NULL;
static bool display_active = false;
static int current_page = 0;
static uint32_t last_page_switch_ms = 0;

#define PAGE_COUNT       3
#define PAGE_INTERVAL_MS 5000

extern int read_vbat_mv(void);
extern char cfg_station[32];

static const struct gpio_dt_spec backlight = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin = 15,
    .dt_flags = GPIO_ACTIVE_HIGH,
};

static void draw_page_station(void)
{
    char buf[40];
    cfb_print(disp_dev, "TinyGS nRF52", 0, 0);
    snprintf(buf, sizeof(buf), "Station: %s", cfg_station);
    cfb_print(disp_dev, buf, 0, 16);
    snprintf(buf, sizeof(buf), "Uptime: %us", (unsigned)(k_uptime_get_32() / 1000));
    cfb_print(disp_dev, buf, 0, 32);
    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    cfb_print(disp_dev, buf, 0, 48);
    snprintf(buf, sizeof(buf), "Version: %u", (unsigned)TINYGS_VERSION);
    cfb_print(disp_dev, buf, 0, 64);
}

static void draw_page_satellite(void)
{
    char buf[40];
    if (tinygs_radio.satellite[0]) {
        cfb_print(disp_dev, tinygs_radio.satellite, 0, 0);
    } else {
        cfb_print(disp_dev, "No satellite", 0, 0);
    }
    snprintf(buf, sizeof(buf), "Freq: %.4f MHz", (double)tinygs_radio.frequency);
    cfb_print(disp_dev, buf, 0, 16);
    snprintf(buf, sizeof(buf), "SF:%d BW:%.1f CR:%d",
             tinygs_radio.sf, (double)tinygs_radio.bw, tinygs_radio.cr);
    cfb_print(disp_dev, buf, 0, 32);
    snprintf(buf, sizeof(buf), "NORAD: %u", (unsigned)tinygs_radio.norad);
    cfb_print(disp_dev, buf, 0, 48);
    cfb_print(disp_dev, "Listening...", 0, 64);
}

static void draw_page_system(void)
{
    char buf[40];
    cfb_print(disp_dev, "System Status", 0, 0);
    cfb_print(disp_dev, "Thread: Child", 0, 16);
    cfb_print(disp_dev, "MQTT: Connected", 0, 32);
    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    cfb_print(disp_dev, buf, 0, 48);
    snprintf(buf, sizeof(buf), "Keepalive: %ds", CONFIG_MQTT_KEEPALIVE);
    cfb_print(disp_dev, buf, 0, 64);
}

bool tinygs_display_init(void)
{
    disp_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(st7789v));
    if (!disp_dev || !device_is_ready(disp_dev)) {
        LOG_INF("Display not found — running headless");
        disp_dev = NULL;
        return false;
    }

    if (device_is_ready(backlight.port)) {
        gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&backlight, 1);
    }

    if (cfb_framebuffer_init(disp_dev) != 0) {
        LOG_ERR("CFB init failed");
        disp_dev = NULL;
        return false;
    }

    cfb_framebuffer_set_font(disp_dev, 0);
    display_blanking_off(disp_dev);
    display_active = true;
    last_page_switch_ms = k_uptime_get_32();

    LOG_INF("Display: ST7789V ready, CFB %dx%d",
            cfb_get_display_parameter(disp_dev, CFB_DISPLAY_WIDTH),
            cfb_get_display_parameter(disp_dev, CFB_DISPLAY_HEIGH));
    return true;
}

void tinygs_display_update(void)
{
    if (!disp_dev || !display_active) return;

    uint32_t now = k_uptime_get_32();
    if ((now - last_page_switch_ms) >= PAGE_INTERVAL_MS) {
        current_page = (current_page + 1) % PAGE_COUNT;
        last_page_switch_ms = now;
    }

    cfb_framebuffer_clear(disp_dev, false);

    switch (current_page) {
    case 0: draw_page_station(); break;
    case 1: draw_page_satellite(); break;
    case 2: draw_page_system(); break;
    }

    cfb_framebuffer_finalize(disp_dev);
}

void tinygs_display_off(void)
{
    if (!disp_dev) return;
    display_blanking_on(disp_dev);
    if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 0);
    display_active = false;
}

void tinygs_display_on(void)
{
    if (!disp_dev) return;
    display_blanking_off(disp_dev);
    if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 1);
    display_active = true;
    last_page_switch_ms = k_uptime_get_32();
}
