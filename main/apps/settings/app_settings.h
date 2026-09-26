#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render the Settings UI screen
 */
void app_settings_draw(void);

/**
 * @brief Handle touch inputs in the Settings screen
 * @param tx Touch X
 * @param ty Touch Y
 * @param next_state Output next state
 * @param needs_redraw Output true if UI needs refresh
 * @return true if touch was handled
 */
bool app_settings_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

/**
 * @brief Get configured auto-screensaver timeout in ms (0 = disabled)
 */
uint32_t app_settings_get_screensaver_timeout_ms(void);

/**
 * @brief Get configured RoboEyes eye color
 */
uint16_t app_settings_get_eye_color(void);

#ifdef __cplusplus
}
#endif
