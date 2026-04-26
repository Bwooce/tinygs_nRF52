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

/* TinyGS logo — 67x32 XBM from ESP32 source (graphics.h) */
#define LOGO_W 67
#define LOGO_H 32
static const uint8_t logo_bits[] = {
    0xE0, 0x00, 0x00, 0xC0, 0x1F, 0x1F, 0xC3, 0xDD, 0x01, 0xF8, 0x07, 0x00,
    0x40, 0x12, 0x04, 0x86, 0x88, 0x00, 0x3C, 0x0E, 0x30, 0x00, 0x02, 0x04,
    0x8A, 0x88, 0x00, 0x0E, 0x08, 0x78, 0x00, 0x02, 0x04, 0x8A, 0x50, 0x00,
    0xE7, 0x03, 0xFC, 0x00, 0x02, 0x04, 0x92, 0x20, 0x00, 0x33, 0x04, 0xFE,
    0x00, 0x02, 0x04, 0xB2, 0x20, 0x00, 0x1B, 0x00, 0xFF, 0x01, 0x02, 0x04,
    0xA2, 0x20, 0x00, 0xCB, 0x9F, 0xFF, 0x01, 0x02, 0x04, 0xC2, 0x20, 0x00,
    0xCB, 0xCF, 0xFF, 0x01, 0x07, 0x1F, 0x87, 0x70, 0x00, 0xCB, 0xE7, 0xFF,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD3, 0xF3, 0xFF, 0x03, 0xFE, 0x39,
    0x80, 0xBF, 0x03, 0xC6, 0xF9, 0xFF, 0x83, 0xFF, 0x3F, 0xE0, 0xFF, 0x03,
    0xC4, 0xFC, 0xFF, 0x83, 0xFF, 0x3F, 0xF0, 0xFF, 0x07, 0x40, 0xFE, 0xFF,
    0xC3, 0xFF, 0x3F, 0xF8, 0xFF, 0x07, 0x00, 0xFF, 0xFF, 0xE1, 0x07, 0x3F,
    0xF8, 0xE1, 0x07, 0x80, 0xFF, 0xFF, 0xF1, 0x01, 0x3C, 0x78, 0xC0, 0x07,
    0xC0, 0xFF, 0xFF, 0xF1, 0x00, 0x3C, 0x78, 0x80, 0x07, 0xE0, 0xFF, 0xFF,
    0xF1, 0x00, 0x18, 0x78, 0x80, 0x03, 0xF0, 0xFF, 0xFF, 0xF8, 0x00, 0x00,
    0xF8, 0x00, 0x00, 0xF8, 0xFF, 0x7F, 0x78, 0x00, 0x00, 0xF8, 0x1F, 0x00,
    0xFC, 0xFF, 0x7F, 0x78, 0x00, 0x00, 0xF0, 0xFF, 0x00, 0xFE, 0xFF, 0x3F,
    0x78, 0xE0, 0x7F, 0xE0, 0xFF, 0x01, 0xFF, 0xFF, 0x9F, 0x78, 0xF0, 0xFF,
    0x80, 0xFF, 0x03, 0xFE, 0xFF, 0xCF, 0x78, 0xF0, 0xFF, 0x00, 0xF0, 0x07,
    0xF8, 0xFF, 0xE7, 0xF8, 0xE0, 0x7F, 0x18, 0xC0, 0x07, 0xE0, 0xFF, 0xF9,
    0xF0, 0x00, 0x3C, 0x3C, 0x80, 0x07, 0x80, 0x3F, 0xFE, 0xF0, 0x01, 0x3C,
    0x3C, 0x80, 0x07, 0x00, 0xC0, 0xFF, 0xF0, 0x03, 0x3C, 0x7C, 0xC0, 0x07,
    0x00, 0xF0, 0xFF, 0xE0, 0xFF, 0x3F, 0xFC, 0xFF, 0x07, 0x00, 0xF8, 0xFF,
    0xC0, 0xFF, 0x3F, 0xFC, 0xFF, 0x03, 0x00, 0xFC, 0xFF, 0x80, 0xFF, 0x1F,
    0xFC, 0xFF, 0x01, 0x00, 0xFC, 0xFF, 0x00, 0xFF, 0x07, 0xD8, 0x7F, 0x00,
};

static const struct device *disp_dev = NULL;
static bool display_active = false;
/* Page IDs */
#define PAGE_WORLDMAP   0
#define PAGE_STATION    1
#define PAGE_STATUS     2
#define PAGE_SATELLITE  3
#define PAGE_LASTPKT    4
#define PAGE_INFO       5
#define PAGE_REMOTE0    6
#define PAGE_REMOTE1    7

/* Rotation favours the map: it appears between every other page, so the user
 * always returns to the worldmap within ~5s. Other screens are still reachable
 * but only "occasionally interesting" per the UX call. */
static const uint8_t page_rotation[] = {
    PAGE_WORLDMAP, PAGE_STATION,
    PAGE_WORLDMAP, PAGE_STATUS,
    PAGE_WORLDMAP, PAGE_SATELLITE,
    PAGE_WORLDMAP, PAGE_LASTPKT,
    PAGE_WORLDMAP, PAGE_INFO,
    PAGE_WORLDMAP, PAGE_REMOTE0,
    PAGE_WORLDMAP, PAGE_REMOTE1,
};
#define ROTATION_LEN (sizeof(page_rotation) / sizeof(page_rotation[0]))

static int rotation_idx = 0;
static int current_page = PAGE_WORLDMAP;
static uint32_t last_page_switch_ms = 0;
static uint32_t last_activity_ms = 0;

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
#define COL_LTGRAY  0xC618  /* Light gray background */
#define COL_OCEAN   0x3B7F  /* Blue ocean for map */
#define COL_LAND    0x2E05  /* Dark green land for map */

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
        /* First press wakes the display — start on the map (the screen the
         * user actually wants to see). */
        display_blanking_off(disp_dev);
        if (device_is_ready(backlight.port)) gpio_pin_set_dt(&backlight, 1);
        display_active = true;
        rotation_idx = 0;
        current_page = page_rotation[0];
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

/* Draw an XBM bitmap scaled 2x. XBM format: LSB first, row-major. */
static void draw_xbm_2x(int x, int y, int w, int h,
                         const uint8_t *bits, uint16_t fg, uint16_t bg)
{
    if (!disp_dev) return;
    int row_bytes = (w + 7) / 8;
    /* Render 2 display rows per XBM row (2x vertical scale) */
    uint16_t row_buf[DISP_W];
    struct display_buffer_descriptor desc = {
        .buf_size = (uint32_t)(w * 2 * sizeof(uint16_t)),
        .width = (uint32_t)(w * 2),
        .height = 1,
        .pitch = (uint32_t)(w * 2),
    };
    for (int r = 0; r < h; r++) {
        /* Build one scaled row */
        for (int c = 0; c < w; c++) {
            uint8_t byte = bits[r * row_bytes + c / 8];
            bool set = byte & (1 << (c % 8)); /* XBM is LSB first */
            uint16_t col = set ? fg : bg;
            row_buf[c * 2] = col;
            row_buf[c * 2 + 1] = col;
        }
        /* Write the same row twice for 2x vertical */
        display_write(disp_dev, x, y + r * 2, &desc, row_buf);
        display_write(disp_dev, x, y + r * 2 + 1, &desc, row_buf);
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
    for (int i = 0; i < DISP_W; i++) line_buf[i] = COL_WHITE;

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
    draw_string(0, 0, "TinyGS nRF52", 0x0010, COL_WHITE);
    snprintf(buf, sizeof(buf), "Sta: %s", cfg_station);
    draw_string(0, 18, buf, COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "Up: %us", (unsigned)(k_uptime_get_32() / 1000));
    draw_string(0, 36, buf, COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    draw_string(0, 54, buf, 0x0400, COL_WHITE);
    snprintf(buf, sizeof(buf), "Ver: %u", (unsigned)TINYGS_VERSION);
    draw_string(0, 72, buf, COL_BLACK, COL_WHITE);
}

static void draw_page_satellite(void)
{
    char buf[32];
    if (tinygs_radio.satellite[0]) {
        draw_string(0, 0, tinygs_radio.satellite, 0x8400, COL_WHITE);
    } else {
        draw_string(0, 0, "No satellite", 0x8400, COL_WHITE);
    }
    snprintf(buf, sizeof(buf), "%.4f MHz", (double)tinygs_radio.frequency);
    draw_string(0, 18, buf, COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "SF%d BW%.0f CR%d",
             tinygs_radio.sf, (double)tinygs_radio.bw, tinygs_radio.cr);
    draw_string(0, 36, buf, COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "NORAD: %u", (unsigned)tinygs_radio.norad);
    draw_string(0, 54, buf, COL_BLACK, COL_WHITE);
    draw_string(0, 72, "Listening...", 0x0400, COL_WHITE);
}

/* Page 4: Last received packet info */
static void draw_page_lastpkt(void)
{
    char buf[32];
    draw_string(0, 0, "Last Packet", 0x0010, COL_WHITE);
    if (last_pkt_time_ms > 0) {
        uint32_t ago = (k_uptime_get_32() - last_pkt_time_ms) / 1000;
        snprintf(buf, sizeof(buf), "RSSI: %.1f dBm", (double)last_pkt_rssi);
        draw_string(0, 18, buf, COL_BLACK, COL_WHITE);
        snprintf(buf, sizeof(buf), "SNR: %.2f dB", (double)last_pkt_snr);
        draw_string(0, 36, buf, COL_BLACK, COL_WHITE);
        snprintf(buf, sizeof(buf), "%us ago", (unsigned)ago);
        draw_string(0, 54, buf, 0x0400, COL_WHITE);
    } else {
        draw_string(0, 36, "No packets yet", 0x8400, COL_WHITE);
    }
}

/* Page 3: World map with station + satellite position */
/* Pulsing satellite-dot animation state. Ramps 3→10 then 10→3 on each
 * worldmap redraw (~100 ms cadence per display update tick). Mirrors the
 * ESP32 `graphVal` pattern in tinyGS-esp32 Display.cpp. */
static int sat_dot_radius = 3;
static int sat_dot_delta = 1;

static void draw_page_worldmap(void)
{
    if (!disp_dev) return;

    /* Advance the animation one frame per worldmap redraw. */
    sat_dot_radius += sat_dot_delta;
    if (sat_dot_radius >= 10)     sat_dot_delta = -1;
    else if (sat_dot_radius <= 3) sat_dot_delta = +1;

    /* Render world map: land=dark green, ocean=dark blue, one row at a time */
    struct display_buffer_descriptor desc = {
        .buf_size = DISP_W * sizeof(uint16_t),
        .width = DISP_W,
        .height = 1,
        .pitch = DISP_W,
    };

    /* Pre-compute satellite name layout once per redraw. The name is
     * blitted into line_buf inline with the map row write so map and
     * text land in the same atomic display_write per row — eliminates
     * the visible flicker the user saw with map-then-text-overlay
     * (the bottom 16 rows were briefly text-less between the map row
     * write and the subsequent draw_string overlay every ~100ms). */
    const char *sat_name = tinygs_radio.satellite[0] ? tinygs_radio.satellite : NULL;
    int text_y0 = DISP_H - 16;        /* font is 8x16, name pinned bottom */
    int text_chars = 0;
    if (sat_name) {
        for (const char *p = sat_name; *p && text_chars * FONT_W + FONT_W <= DISP_W; p++) {
            text_chars++;
        }
    }

    for (int y = 0; y < DISP_H; y++) {
        for (int x = 0; x < DISP_W; x++) {
            int bi = y * WORLDMAP_W + x;
            bool land = (worldmap_bits[bi / 8] >> (bi % 8)) & 1;
            line_buf[x] = land ? COL_LAND : COL_OCEAN;
        }

        /* Draw station location dot (3x3 yellow) */
        int sx = (int)((180.0f + tinygs_station_lon) / 360.0f * DISP_W);
        int sy = (int)((90.0f - tinygs_station_lat) / 180.0f * DISP_H);
        if (y >= sy - 1 && y <= sy + 1 && sx >= 1 && sx < DISP_W - 1) {
            for (int dx = -1; dx <= 1; dx++) {
                line_buf[sx + dx] = COL_YELLOW;
            }
        }

        /* Draw satellite dot — pulsing filled red circle with white outline,
         * matching the ESP32 reference (Display.cpp `graphVal` ramp 1↔6).
         * Animation state lives in the radius variables below; updated once
         * per worldmap redraw. The display loop calls draw_page_worldmap()
         * every ~100 ms while page 3 is active, giving a visible breathing
         * effect (~12 frames per page-show window). */
        if (tinygs_radio.sat_pos_x != 0 || tinygs_radio.sat_pos_y != 0) {
            int sat_x = (int)(tinygs_radio.sat_pos_x * DISP_W / 128);
            int sat_y = (int)(tinygs_radio.sat_pos_y * DISP_H / 64);
            int dy = y - sat_y;
            int r_fill = sat_dot_radius;            /* filled red */
            int r_outline = sat_dot_radius + 1;     /* white halo */
            if (dy >= -r_outline && dy <= r_outline) {
                for (int dx = -r_outline; dx <= r_outline; dx++) {
                    int x = sat_x + dx;
                    if (x < 0 || x >= DISP_W) continue;
                    int d2 = dx * dx + dy * dy;
                    int rf2 = r_fill * r_fill;
                    int ro2 = r_outline * r_outline;
                    if (d2 <= rf2)         line_buf[x] = COL_RED;
                    else if (d2 <= ro2)    line_buf[x] = COL_WHITE;
                }
            }
        }

        /* Inline satellite-name overlay at the bottom — paint glyph
         * pixels into line_buf so the row write is atomic. No flicker. */
        if (sat_name && y >= text_y0 && y < text_y0 + FONT_H) {
            int font_row = y - text_y0;
            for (int ci = 0; ci < text_chars; ci++) {
                char c = sat_name[ci];
                if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) continue;
                const uint8_t *glyph = &font8x16_data[(c - FONT_FIRST_CHAR) * FONT_H];
                uint8_t bits = glyph[font_row];
                int x0 = ci * FONT_W;
                for (int col = 0; col < FONT_W; col++) {
                    line_buf[x0 + col] = (bits & (0x80 >> col)) ? COL_WHITE : COL_OCEAN;
                }
            }
        }

        display_write(disp_dev, 0, y, &desc, line_buf);
    }
}

/* Page 1: Connection status (real-time state) */
static void draw_page_status(void)
{
    char buf[32];
    draw_string(0, 0, "Status", 0x0010, COL_WHITE);

    const char *thread_str = thread_attached ? "Child" : "Detached";
    uint16_t thread_col = thread_attached ? COL_GREEN : COL_YELLOW;
    snprintf(buf, sizeof(buf), "Thread: %s", thread_str);
    draw_string(0, 18, buf, thread_col, COL_WHITE);

    const char *mqtt_str;
    uint16_t mqtt_col;
    switch (app_state) {
    case STATE_MQTT_CONNECTED: mqtt_str = "Connected"; mqtt_col = COL_GREEN; break;
    case STATE_MQTT_CONNECT:   mqtt_str = "Connecting..."; mqtt_col = COL_YELLOW; break;
    case STATE_DNS_RESOLVE:    mqtt_str = "DNS..."; mqtt_col = COL_YELLOW; break;
    case STATE_ERROR:          mqtt_str = "Error"; mqtt_col = COL_RED; break;
    default:                   mqtt_str = "Waiting"; mqtt_col = COL_BLACK; break;
    }
    snprintf(buf, sizeof(buf), "MQTT: %s", mqtt_str);
    draw_string(0, 36, buf, mqtt_col, COL_WHITE);

    snprintf(buf, sizeof(buf), "Vbat: %dmV", read_vbat_mv());
    draw_string(0, 54, buf, COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "Keep: %ds", CONFIG_MQTT_KEEPALIVE);
    draw_string(0, 72, buf, COL_BLACK, COL_WHITE);
}

/* Pages 6-7: Server-pushed remote text frames */
static void draw_page_remote(int frame_idx)
{
    if (frame_idx < 0 || frame_idx >= REMOTE_FRAME_COUNT) return;
    const auto &frame = remote_frames[frame_idx];

    /* Empty frames are filtered out at rotation time, so we shouldn't reach
     * here with count==0. Belt-and-braces: just leave the cleared screen. */
    if (frame.count == 0) return;

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
        draw_string(x, y, elem.text, COL_BLACK, COL_WHITE);
    }
}

/* Page 5: Version and memory info */
static void draw_page_info(void)
{
    char buf[32];
    draw_string(0, 0, "System Info", 0x0010, COL_WHITE);
    snprintf(buf, sizeof(buf), "Ver: %u", (unsigned)TINYGS_VERSION);
    draw_string(0, 18, buf, COL_BLACK, COL_WHITE);
    draw_string(0, 36, "nRF52840/SX1262", COL_BLACK, COL_WHITE);
    snprintf(buf, sizeof(buf), "Up: %us", (unsigned)(k_uptime_get_32() / 1000));
    draw_string(0, 54, buf, 0x0400, COL_WHITE);
    snprintf(buf, sizeof(buf), "Flash: %uKB", (unsigned)(462 /* approx */));
    draw_string(0, 72, buf, COL_BLACK, COL_WHITE);
}

bool tinygs_display_init(void)
{
    disp_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(st7789v));
    if (!disp_dev || !device_is_ready(disp_dev)) {
        LOG_INF("Display not found — running headless");
        disp_dev = NULL;
        return false;
    }

    /* Enable TFT power (P0.03) — must be driven before display will work */
    static const struct gpio_dt_spec tft_en = GPIO_DT_SPEC_GET_OR(DT_NODELABEL(tft_en), gpios, {0});
    if (tft_en.port && device_is_ready(tft_en.port)) {
        gpio_pin_configure_dt(&tft_en, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&tft_en, 1);
        k_msleep(10); /* Let TFT power stabilize */
    }

    if (device_is_ready(backlight.port)) {
        gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
        gpio_pin_set_dt(&backlight, 1);
    }

    display_blanking_off(disp_dev);
    clear_screen();

    /* Boot splash — TinyGS logo (2x scaled) + version, 2 seconds */
    {
        int logo_2x_w = LOGO_W * 2;  /* 134 pixels */
        int logo_2x_h = LOGO_H * 2;  /* 64 pixels */
        int logo_x = (DISP_W - logo_2x_w) / 2;  /* centered */
        int logo_y = 4;
        draw_xbm_2x(logo_x, logo_y, LOGO_W, LOGO_H,
                     logo_bits, COL_BLACK, COL_WHITE);

        /* Text below logo */
        char buf[32];
        draw_string(72, 72, "tinyGS nRF52", 0x0010, COL_WHITE);
        snprintf(buf, sizeof(buf), "v%u", (unsigned)TINYGS_VERSION);
        draw_string(96, 90, buf, COL_BLACK, COL_WHITE);
        draw_string(48, 112, "Thread / LoRa / MQTT", 0x8400, COL_WHITE);
        k_msleep(2000);
        clear_screen();
    }

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
        /* Advance through the rotation, skipping empty remote frames so the
         * user never sees a blank flash for a page with nothing to show. */
        for (int i = 0; i < (int)ROTATION_LEN; i++) {
            rotation_idx = (rotation_idx + 1) % ROTATION_LEN;
            int next = page_rotation[rotation_idx];
            if (next == PAGE_REMOTE0 && remote_frames[0].count == 0) continue;
            if (next == PAGE_REMOTE1 && remote_frames[1].count == 0) continue;
            current_page = next;
            break;
        }
        last_page_switch_ms = now;
        clear_screen();
    }

    switch (current_page) {
    case PAGE_STATION:   draw_page_station();   break;
    case PAGE_STATUS:    draw_page_status();    break;
    case PAGE_SATELLITE: draw_page_satellite(); break;
    case PAGE_WORLDMAP:  draw_page_worldmap();  break;
    case PAGE_LASTPKT:   draw_page_lastpkt();   break;
    case PAGE_INFO:      draw_page_info();      break;
    case PAGE_REMOTE0:   draw_page_remote(0);   break;
    case PAGE_REMOTE1:   draw_page_remote(1);   break;
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
