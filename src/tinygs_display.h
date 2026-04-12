#ifndef TINYGS_DISPLAY_H
#define TINYGS_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize the display. Returns false if no display connected.
 */
bool tinygs_display_init(void);

/**
 * @brief Update the display with current status.
 * Call periodically from the main loop (~1s interval).
 * Cycles through pages automatically.
 */
void tinygs_display_update(void);

/**
 * @brief Turn display off (power saving).
 */
void tinygs_display_off(void);

/**
 * @brief Turn display on.
 */
void tinygs_display_on(void);

/**
 * @brief Set display auto-off timeout in seconds. 0 = never auto-off.
 */
void tinygs_display_set_timeout(uint32_t seconds);

/**
 * @brief Notify display of a received LoRa packet.
 * Updates last-packet page and briefly flashes the display.
 */
void tinygs_display_packet_rx(float rssi, float snr);

/**
 * @brief Set a remote text frame from server (frame/{num} command).
 * @param frame_idx Frame index (0 or 1)
 * @param json JSON payload: [[font, align, x, y, "text"], ...]
 * @param len JSON length
 */
void tinygs_display_set_remote_frame(int frame_idx, const char *json, size_t len);

/**
 * @brief Check and clear the weblogin request flag.
 * Set by BOOT button press when display is active (or no display).
 * Rate-limited to once per 10 seconds.
 */
bool tinygs_display_weblogin_requested(void);

#endif /* TINYGS_DISPLAY_H */
