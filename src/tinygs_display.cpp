/**
 * TinyGS Display — Multi-page status display for ST7789V 240x135 TFT.
 *
 * Uses a custom 8x16 bitmap font renderer with display_write() for
 * minimal flash overhead (~1.5KB font + ~200 lines code).
 *
 * 8 pages matching ESP32 TinyGS display layout:
 *   Page 0: TinyGS logo/station name (ESP32 Frame1)
 *   Page 1: MQTT/Thread status (ESP32 Frame8)
 *   Page 2: Satellite config (ESP32 Frame3)
 *   Page 3: World map + sat position (ESP32 Frame5)
 *   Page 4: Last packet info (ESP32 Frame2 - local, not remote)
 *   Page 5: System info (ESP32 Frame4 - local, not remote)
 *   Page 6: Reserved for server frame (ESP32 Frame6)
 *   Page 7: Reserved for server frame (ESP32 Frame7)
 */

#include "tinygs_display.h"
#include "tinygs_protocol.h"
#include "tinygs_config.h"
#include "worldmap.h"
#include "font8x16.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(tinygs_display, LOG_LEVEL_INF);

static const struct device *disp_dev = NULL;
static bool display_active = false;
static int current_page = 0;
static uint32_t last_page_switch_ms = 0;
static uint32_t last_activity_ms = 0;

#define PAGE_COUNT       8  /* Match ESP32 TinyGS 8-frame display */
#define PAGE_INTERVAL_MS 5000
#define DISP_W           240
#define DISP_H           135
#define DISPLAY_TIMEOUT_DEFAULT_MS 30000
static uint32_t display_timeout_ms = DISPLAY_TIMEOUT_DEFAULT_MS;

/* RGB565 colors */
#define COL_BLACK   0x0000
#define COL_WHITE   0xFFFF
#define COL_GREEN   0x07E0
#define COL_YELLOW  0xFFE0
#define COL_CYAN    0x07FF
#define COL_RED     0xF800
#define COL_DKBLUE  0x0011  /* Dark ocean blue */
#define COL_DKGREEN 0x0320  /* Dark land green */

extern int read_vbat_mv(void);
extern char cfg_station[32];
extern volatile bool thread_attached;

/* App state enum — must match main.cpp */
enum app_state {
    STATE_WAIT_THREAD = 0,
    STATE_DNS_RESOLVE,
    STATE_MQTT_CONNECT,
    STATE_MQTT_CONNECTED,
    STATE_ERROR,
};
extern enum app_state app_state;

/* Last packet info — updated by tinygs_display_packet_rx() */
static float last_pkt_rssi = 0;
static float last_pkt_snr = 0;
static uint32_t last_pkt_time_ms = 0;

/* Remote frames — server-pushed text via frame/{num} MQTT command.
 * Each frame has up to 8 text elements with position and content.
 * ESP32 supports 4 frames x 15 elements — we use 2 frames x 8 for RAM. */
#define REMOTE_FRAME_COUNT   2
#define REMOTE_FRAME_MAX_ELEM 8
#define REMOTE_TEXT_MAX_LEN  30  /* Max chars per text element */

struct remote_text_elem {
    int16_t x;
    int16_t y;
    char text[REMOTE_TEXT_MAX_LEN + 1];
};

static struct {
    struct remote_text_elem elems[REMOTE_FRAME_MAX_ELEM];
    uint8_t count;
} remote_frames[REMOTE_FRAME_COUNT];

static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback button_cb_data;

static volatile bool weblogin_requested = false;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    uint32_t now = k_uptime_get_32();

    if (!display_active && disp_dev) {
        /* First press wakes the display */
        display_blanking_off(disp_dev);
        if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 1);
        display_active = true;
        current_page = 0;
    } else {
        /* Display already active (or no display): request weblogin */
        weblogin_requested = true;
    }

    last_activity_ms = now;
    last_page_switch_ms = now;
}

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

/* Page 4: Last received packet info */
static void draw_page_lastpkt(void)
{
    char buf[32];
    draw_string(0, 0, "Last Packet", COL_CYAN, COL_BLACK);
    if (last_pkt_time_ms > 0) {
        uint32_t ago = (k_uptime_get_32() - last_pkt_time_ms) / 1000;
        snprintf(buf, sizeof(buf), "RSSI: %.1f dBm", (double)last_pkt_rssi);
        draw_string(0, 18, buf, COL_WHITE, COL_BLACK);
        snprintf(buf, sizeof(buf), "SNR: %.2f dB", (double)last_pkt_snr);
        draw_string(0, 36, buf, COL_WHITE, COL_BLACK);
        snprintf(buf, sizeof(buf), "%us ago", (unsigned)ago);
        draw_string(0, 54, buf, COL_GREEN, COL_BLACK);
    } else {
        draw_string(0, 36, "No packets yet", COL_YELLOW, COL_BLACK);
    }
}

/* Page 3: World map with station + satellite position */
static void draw_page_worldmap(void)
{
    if (!disp_dev) return;

    /* Render world map: land=dark green, ocean=dark blue, one row at a time */
    struct display_buffer_descriptor desc = {
        .buf_size = DISP_W * sizeof(uint16_t),
        .width = DISP_W,
        .height = 1,
        .pitch = DISP_W,
    };

    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            int bi = y * WORLDMAP_W + x;
            bool land = (worldmap_bits[bi / 8] >> (bi % 8)) & 1;
            line_buf[x] = land ? COL_DKGREEN : COL_DKBLUE;
        }

        /* Draw station location dot (3x3 yellow) */
        int sx = (int)((180.0f + tinygs_station_lon) / 360.0f * DISP_W);
        int sy = (int)((90.0f - tinygs_station_lat) / 180.0f * DISP_H);
        if (y >= sy - 1 && y <= sy + 1 && sx >= 1 && sx < DISP_W - 1) {
            for (int dx = -1; dx <= 1; dx++) {
                line_buf[sx + dx] = COL_YELLOW;
            }
        }

        /* Draw satellite dot (3x3 red) from sat_pos_oled or P13 */
        if (tinygs_radio.sat_pos_x != 0 || tinygs_radio.sat_pos_y != 0) {
            /* Scale from ESP32 128x64 coordinates to our 240x135 */
            int sat_x = (int)(tinygs_radio.sat_pos_x * DISP_W / 128);
            int sat_y = (int)(tinygs_radio.sat_pos_y * DISP_H / 64);
            if (y >= sat_y - 1 && y <= sat_y + 1 && sat_x >= 1 && sat_x < DISP_W - 1) {
                for (int dx = -1; dx <= 1; dx++) {
                    line_buf[sat_x + dx] = COL_RED;
                }
            }
        }

        display_write(disp_dev, 0, y, &desc, line_buf);
    }

    /* Overlay satellite name at bottom */
    if (tinygs_radio.satellite[0]) {
        draw_string(0, DISP_H - 16, tinygs_radio.satellite, COL_WHITE, COL_DKBLUE);
    }
}

/* Page 1: Connection status (real-time state) */
static void draw_page_status(void)
{
    char buf[32];
    draw_string(0, 0, "Status", COL_CYAN, COL_BLACK);

    const char *thread_str = thread_attached ? "Child" : "Detached";
    uint16_t thread_col = thread_attached ? COL_GREEN : COL_YELLOW;
    snprintf(buf, sizeof(buf), "Thread: %s", thread_str);
    draw_string(0, 18, buf, thread_col, COL_BLACK);

    const char *mqtt_str;
    uint16_t mqtt_col;
    switch (app_state) {
    case STATE_MQTT_CONNECTED: mqtt_str = "Connected"; mqtt_col = COL_GREEN; break;
    case STATE_MQTT_CONNECT:   mqtt_str = "Connecting..."; mqtt_col = COL_YELLOW; break;
    case STATE_DNS_RESOLVE:    mqtt_str = "DNS..."; mqtt_col = COL_YELLOW; break;
    case STATE_ERROR:          mqtt_str = "Error"; mqtt_col = COL_RED; break;
    default:                   mqtt_str = "Waiting"; mqtt_col = COL_WHITE; break;
    }
    snprintf(buf, sizeof(buf), "MQTT: %s", mqtt_str);
    draw_string(0, 36, buf, mqtt_col, COL_BLACK);

    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    draw_string(0, 54, buf, COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "Keep: %ds", CONFIG_MQTT_KEEPALIVE);
    draw_string(0, 72, buf, COL_WHITE, COL_BLACK);
}

/* Pages 6-7: Server-pushed remote text frames */
static void draw_page_remote(int frame_idx)
{
    if (frame_idx < 0 || frame_idx >= REMOTE_FRAME_COUNT) return;
    const auto &frame = remote_frames[frame_idx];

    if (frame.count == 0) {
        /* Empty frame — skip to next page */
        current_page = (current_page + 1) % PAGE_COUNT;
        last_page_switch_ms = k_uptime_get_32();
        return;
    }

    for (int i = 0; i < frame.count; i++) {
        const auto &elem = frame.elems[i];
        /* Scale ESP32 128x64 coordinates to our 240x135 display */
        int x = (int)(elem.x * DISP_W / 128);
        int y = (int)(elem.y * DISP_H / 64);
        /* Clamp to screen bounds */
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= DISP_W) continue;
        if (y >= DISP_H - FONT_H) continue;
        draw_string(x, y, elem.text, COL_WHITE, COL_BLACK);
    }
}

/* Page 5: Version and memory info */
static void draw_page_info(void)
{
    char buf[32];
    draw_string(0, 0, "System Info", COL_CYAN, COL_BLACK);
    snprintf(buf, sizeof(buf), "Ver: %u", (unsigned)TINYGS_VERSION);
    draw_string(0, 18, buf, COL_WHITE, COL_BLACK);
    draw_string(0, 36, "nRF52840/SX1262", COL_WHITE, COL_BLACK);
    snprintf(buf, sizeof(buf), "Up: %us", (unsigned)(k_uptime_get_32() / 1000));
    draw_string(0, 54, buf, COL_GREEN, COL_BLACK);
    snprintf(buf, sizeof(buf), "Flash: %uKB", (unsigned)(462 /* approx */));
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
    last_activity_ms = last_page_switch_ms;

    /* Set up BOOT button for display wake */
    if (device_is_ready(button.port)) {
        gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP);
        gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
        gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
        gpio_add_callback(button.port, &button_cb_data);
    }

    LOG_INF("Display: ST7789V 240x135 ready (auto-off %ds)", display_timeout_ms / 1000);
    return true;
}

void tinygs_display_update(void)
{
    if (!disp_dev) return;

    uint32_t now = k_uptime_get_32();

    /* Auto-off after inactivity */
    if (display_active && (now - last_activity_ms) >= display_timeout_ms) {
        display_blanking_on(disp_dev);
        if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 0);
        display_active = false;
        return;
    }

    if (!display_active) return;

    if ((now - last_page_switch_ms) >= PAGE_INTERVAL_MS) {
        current_page = (current_page + 1) % PAGE_COUNT;
        last_page_switch_ms = now;
        clear_screen();
    }

    switch (current_page) {
    case 0: draw_page_station(); break;     /* Logo + station name */
    case 1: draw_page_status(); break;     /* Thread/MQTT real-time state */
    case 2: draw_page_satellite(); break;  /* Current satellite config */
    case 3: draw_page_worldmap(); break;   /* World map + station dot */
    case 4: draw_page_lastpkt(); break;    /* Last received packet */
    case 5: draw_page_info(); break;       /* Version/board/uptime */
    case 6: draw_page_remote(0); break;     /* Remote frame 0 (server-pushed) */
    case 7: draw_page_remote(1); break;    /* Remote frame 1 (server-pushed) */
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

void tinygs_display_set_remote_frame(int frame_idx, const char *json, size_t len)
{
    if (frame_idx < 0 || frame_idx >= REMOTE_FRAME_COUNT) return;
    auto &frame = remote_frames[frame_idx];
    frame.count = 0;

    /* Parse: [[font, align, x, y, "text"], ...] — we skip font/align (single font) */
    const char *p = json;
    const char *end = json + len;

    /* Find outer array */
    while (p < end && *p != '[') p++;
    if (p >= end) return;
    p++; /* skip outer '[' */

    while (p < end && frame.count < REMOTE_FRAME_MAX_ELEM) {
        /* Find inner array start */
        while (p < end && *p != '[') {
            if (*p == ']') return; /* end of outer array */
            p++;
        }
        if (p >= end) break;
        p++; /* skip inner '[' */

        /* Parse: font, align, x, y, "text" */
        /* Skip font (field 0) */
        while (p < end && *p != ',') p++;
        if (p >= end) break;
        p++; /* skip comma */

        /* Skip align (field 1) */
        while (p < end && *p != ',') p++;
        if (p >= end) break;
        p++; /* skip comma */

        /* Parse x (field 2) */
        while (p < end && (*p == ' ')) p++;
        int16_t x = (int16_t)atoi(p);
        while (p < end && *p != ',') p++;
        if (p >= end) break;
        p++;

        /* Parse y (field 3) */
        while (p < end && (*p == ' ')) p++;
        int16_t y = (int16_t)atoi(p);
        while (p < end && *p != ',') p++;
        if (p >= end) break;
        p++;

        /* Parse "text" (field 4) */
        while (p < end && *p != '"') p++;
        if (p >= end) break;
        p++; /* skip opening quote */

        const char *text_start = p;
        while (p < end && *p != '"') p++;
        if (p >= end) break;

        size_t text_len = p - text_start;
        if (text_len > REMOTE_TEXT_MAX_LEN) text_len = REMOTE_TEXT_MAX_LEN;

        frame.elems[frame.count].x = x;
        frame.elems[frame.count].y = y;
        memcpy(frame.elems[frame.count].text, text_start, text_len);
        frame.elems[frame.count].text[text_len] = '\0';
        frame.count++;

        p++; /* skip closing quote */

        /* Skip to end of inner array */
        while (p < end && *p != ']') p++;
        if (p < end) p++; /* skip ']' */
    }

    LOG_INF("Remote frame %d: %d elements", frame_idx, frame.count);
}

void tinygs_display_set_timeout(uint32_t seconds)
{
    display_timeout_ms = seconds ? (seconds * 1000) : UINT32_MAX;
}

void tinygs_display_packet_rx(float rssi, float snr)
{
    last_pkt_rssi = rssi;
    last_pkt_snr = snr;
    last_pkt_time_ms = k_uptime_get_32();

    /* Wake display briefly on packet reception */
    if (!display_active && disp_dev) {
        display_blanking_off(disp_dev);
        if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 1);
        display_active = true;
        current_page = 4;  /* Jump to last-packet page */
    }
    last_activity_ms = k_uptime_get_32();
}

bool tinygs_display_weblogin_requested(void)
{
    if (weblogin_requested) {
        weblogin_requested = false;
        return true;
    }
    return false;
}
