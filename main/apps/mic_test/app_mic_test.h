#pragma once

#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Mic Test & Live Passthrough Application
 */
void app_mic_test_init(void);

/**
 * @brief Draw full static UI elements of the Mic Test screen
 */
void app_mic_test_draw(void);

/**
 * @brief Periodic UI update (refresh live VU meter, telemetry, and animations)
 */
void app_mic_test_update(void);

/**
 * @brief Handle touch interactions inside the Mic Test app
 * @param tx Touch X coordinate
 * @param ty Touch Y coordinate
 * @param next_state Output pointer to set next AppState
 * @param needs_redraw Output pointer to request full screen redraw
 * @return true if touch was handled
 */
bool app_mic_test_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

/**
 * @brief Stop live passthrough when exiting the app
 */
void app_mic_test_stop(void);

#ifdef __cplusplus
}
#endif
