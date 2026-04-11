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
/* CFB doesn't support RGB565 — using display_write() directly */
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

/* TODO: Implement text rendering for RGB565 display.
 * CFB only supports monochrome. Options:
 * - Simple 8x16 bitmap font renderer (~100 lines, minimal flash)
 * - LVGL (full featured but heavy — ~50-100KB flash)
 * For now, pages are defined but not rendered. */

static void draw_page_station(void) { (void)disp_dev; }
static void draw_page_satellite(void) { (void)disp_dev; }
static void draw_page_system(void) { (void)disp_dev; }

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

    /* CFB doesn't support RGB565 color displays. For now, just turn on
     * the backlight and blanking. Text rendering needs a custom font
     * renderer or LVGL. Display update is a no-op until then. */
    display_blanking_off(disp_dev);
    display_active = false; /* Disabled until font renderer is implemented */
    last_page_switch_ms = k_uptime_get_32();

    LOG_INF("Display: ST7789V 240x135 ready (font rendering TODO)");
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

    switch (current_page) {
    case 0: draw_page_station(); break;
    case 1: draw_page_satellite(); break;
    case 2: draw_page_system(); break;
    }
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
