#ifndef FONT8X16_H
#define FONT8X16_H

/**
 * @brief Minimal 8x16 bitmap font for RGB565 displays.
 *
 * Covers ASCII 32-126 (printable characters). Each character is
 * 8 pixels wide, 16 pixels tall, stored as 16 bytes (one byte per row,
 * MSB = leftmost pixel).
 *
 * Total size: 95 characters × 16 bytes = 1,520 bytes of flash.
 */

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16
#define FONT_FIRST_CHAR 32
#define FONT_LAST_CHAR 126

/* Font data — 16 bytes per character, 95 characters */
extern const uint8_t font8x16_data[];

#endif /* FONT8X16_H */
