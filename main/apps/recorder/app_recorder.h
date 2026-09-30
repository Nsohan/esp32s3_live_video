#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Sound Recorder App (starts mic level monitoring & RMS serial output)
 */
void app_recorder_init(void);

/**
 * @brief Draw full static UI elements of Sound Recorder screen
 */
void app_recorder_draw(void);

/**
 * @brief Periodic UI update (refresh live VU meter, recording timer, and status)
 */
void app_recorder_update(void);

/**
 * @brief Handle touch interactions inside the Sound Recorder app
 * @param tx Touch X coordinate
 * @param ty Touch Y coordinate
 * @param next_state Output pointer to set next AppState
 * @param needs_redraw Output pointer to request full screen redraw
 * @return true if touch was handled
 */
bool app_recorder_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

/**
 * @brief Stop recording, playback, and background mic level monitoring when leaving app
 */
void app_recorder_stop(void);

#ifdef __cplusplus
}
#endif
