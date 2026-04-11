#ifndef TINYGS_DISPLAY_H
#define TINYGS_DISPLAY_H

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

#endif /* TINYGS_DISPLAY_H */
