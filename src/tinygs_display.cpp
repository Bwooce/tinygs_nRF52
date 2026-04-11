/**
 * TinyGS Display — Multi-page status display for ST7789V 240x135 TFT.
 *
 * Pages cycle every 5 seconds:
 *   Page 0: Station info (name, version, uptime, vbat)
 *   Page 1: Satellite (name, freq, SF/BW/CR)
 *   Page 2: System (Thread, MQTT, heap, packets)
 *
 * Uses minimal RAM — renders text to a line buffer and pushes to display.
 * Optional — all functions are no-ops if display_dev is NULL.
 */

#include "tinygs_display.h"
#include "tinygs_protocol.h"
#include "tinygs_config.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
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
#define DISP_W           240
#define DISP_H           135
#define LINE_H           16   /* pixels per text line */
#define MAX_LINES        8    /* 135 / 16 = 8 lines */
#define CHAR_W           8    /* approximate char width for simple font */

/* RGB565 colors */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_YELLOW  0xFFE0
#define COL_CYAN    0x07FF

/* Simple 8x16 font — just use the display_write API to draw filled rectangles
 * for a minimal text renderer. For now, clear screen + write status lines
 * using snprintf. A proper font renderer can be added later. */

/* Line buffer — one text line as RGB565 pixels */
static uint16_t line_buf[DISP_W];

static const struct gpio_dt_spec backlight = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin = 15,
    .dt_flags = GPIO_ACTIVE_HIGH,
};

static void clear_screen(uint16_t color)
{
    if (!disp_dev) return;
    for (int i = 0; i < DISP_W; i++) line_buf[i] = color;

    struct display_buffer_descriptor desc = {
        .buf_size = sizeof(line_buf),
        .width = DISP_W,
        .height = 1,
        .pitch = DISP_W,
    };

    for (int y = 0; y < DISP_H; y++) {
        display_write(disp_dev, 0, y, &desc, line_buf);
    }
}

/* Extern declarations for status data */
extern int read_vbat_mv(void);

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

    display_blanking_off(disp_dev);
    clear_screen(COL_BLACK);
    display_active = true;
    last_page_switch_ms = k_uptime_get_32();

    LOG_INF("Display: ST7789V 240x135 ready (%d pages)", PAGE_COUNT);
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

    /* For now, just clear and show page info.
     * Full text rendering requires a font — placeholder until
     * we integrate a bitmap font or CFB. */
    clear_screen(COL_BLACK);

    /* TODO: Render text pages when font support is added.
     * Page 0: "TinyGS nRF52" / station name / uptime / vbat
     * Page 1: satellite name / freq / SF BW CR
     * Page 2: Thread:Child / MQTT:Connected / Heap:81820 */
}

void tinygs_display_off(void)
{
    if (!disp_dev) return;
    display_blanking_on(disp_dev);
    if (device_is_ready(backlight.port)) {
        gpio_pin_set_dt(&backlight, 0);
    }
    display_active = false;
    LOG_INF("Display off");
}

void tinygs_display_on(void)
{
    if (!disp_dev) return;
    display_blanking_off(disp_dev);
    if (device_is_ready(backlight.port)) {
        gpio_pin_set_dt(&backlight, 1);
    }
    display_active = true;
    last_page_switch_ms = k_uptime_get_32();
    LOG_INF("Display on");
}
