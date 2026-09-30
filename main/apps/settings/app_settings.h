#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize settings and load saved configuration from NVS flash
 */
void app_settings_init(void);

/**
 * @brief Save all current settings to NVS flash
 */
void app_settings_save(void);

/**
 * @brief Save volume setting to NVS flash
 */
void app_settings_save_volume(uint8_t volume);

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
 * @brief Check if Jarvis Wake Word detection is enabled
 */
bool app_settings_is_wake_word_enabled(void);

/**
 * @brief Enable or disable Jarvis Wake Word detection and persist to NVS
 */
void app_settings_set_wake_word_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
