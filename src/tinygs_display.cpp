/**
 * TinyGS Display — Multi-page status display for ST7789V 240x135 TFT.
 *
 * Uses a custom 8x16 bitmap font renderer with display_write() for
 * minimal flash overhead (~1.5KB font + ~200 lines code).
 *
 * Pages cycle every 5 seconds:
 *   Page 0: Station info (name, version, uptime, vbat)
 *   Page 1: Satellite (name, freq, SF/BW/CR)
 *   Page 2: System (Thread, MQTT, heap, keepalive)
 */

#include "tinygs_display.h"
#include "tinygs_protocol.h"
#include "tinygs_config.h"
#include "font8x16.h"
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

/* RGB565 colors */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_YELLOW  0xFFE0
#define COL_CYAN    0x07FF

extern int read_vbat_mv(void);
extern char cfg_station[32];

static const struct gpio_dt_spec backlight = {
    .port = DEVICE_DT_GET(DT_NODELABEL(gpio0)),
    .pin = 15,
    .dt_flags = GPIO_ACTIVE_HIGH,
};

/* Line buffer for rendering — one row of FONT_H pixels */
static uint16_t line_buf[DISP_W];

static void draw_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (!disp_dev || c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) return;
    if (x + FONT_W > DISP_W || y + FONT_H > DISP_H) return;

    const uint8_t *glyph = &font8x16_data[(c - FONT_FIRST_CHAR) * FONT_H];

    struct display_buffer_descriptor desc = {
        .buf_size = FONT_W * sizeof(uint16_t),
        .width = FONT_W,
        .height = 1,
        .pitch = FONT_W,
    };

    uint16_t row_buf[FONT_W];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            row_buf[col] = (bits & (0x80 >> col)) ? fg : bg;
        }
        display_write(disp_dev, x, y + row, &desc, row_buf);
    }
}

static void draw_string(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        draw_char(x, y, *str, fg, bg);
        x += FONT_W;
        if (x + FONT_W > DISP_W) break;
        str++;
    }
}

static void clear_screen(void)
{
    if (!disp_dev) return;
    for (int i = 0; i < DISP_W; i++) line_buf[i] = COL_BLACK;

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

static void draw_page_station(void)
{
    char buf[32];
    draw_string(0, 0, "TinyGS nRF52", COL_CYAN, COL_BLACK);
    snprintf(buf, sizeof(buf), "Sta: %s", cfg_station);
    draw_string(0, 18, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "Up: %us", (unsigned)(k_uptime_get_32() / 1000));
    draw_string(0, 36, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    draw_string(0, 54, buf, COL_GREEN, COL_BLACK);
    snprintf(buf, sizeof(buf), "Ver: %u", (unsigned)TINYGS_VERSION);
    draw_string(0, 72, buf, COL_WHITE, COL_BLACK);
}

static void draw_page_satellite(void)
{
    char buf[32];
    if (tinygs_radio.satellite[0]) {
        draw_string(0, 0, tinygs_radio.satellite, COL_YELLOW, COL_BLACK);
    } else {
        draw_string(0, 0, "No satellite", COL_YELLOW, COL_BLACK);
    }
    snprintf(buf, sizeof(buf), "%.4f MHz", (double)tinygs_radio.frequency);
    draw_string(0, 18, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "SF%d BW%.0f CR%d",
             tinygs_radio.sf, (double)tinygs_radio.bw, tinygs_radio.cr);
    draw_string(0, 36, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "NORAD: %u", (unsigned)tinygs_radio.norad);
    draw_string(0, 54, buf, COL_WHITE, COL_BLACK);
    draw_string(0, 72, "Listening...", COL_GREEN, COL_BLACK);
}

static void draw_page_system(void)
{
    char buf[32];
    draw_string(0, 0, "System", COL_CYAN, COL_BLACK);
    draw_string(0, 18, "Thread: Child", COL_GREEN, COL_BLACK);
    draw_string(0, 36, "MQTT: Connected", COL_GREEN, COL_BLACK);
    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    draw_string(0, 54, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "Keep: %ds", CONFIG_MQTT_KEEPALIVE);
    draw_string(0, 72, buf, COL_WHITE, COL_BLACK);
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

    display_blanking_off(disp_dev);
    clear_screen();
    display_active = true;
    last_page_switch_ms = k_uptime_get_32();

    LOG_INF("Display: ST7789V 240x135 ready");
    return true;
}

void tinygs_display_update(void)
{
    if (!disp_dev || !display_active) return;

    uint32_t now = k_uptime_get_32();
    if ((now - last_page_switch_ms) >= PAGE_INTERVAL_MS) {
        current_page = (current_page + 1) % PAGE_COUNT;
        last_page_switch_ms = now;
        clear_screen();
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
