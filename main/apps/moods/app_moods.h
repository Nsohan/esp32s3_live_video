#pragma once

#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render the Pet Moods Emotion triggers screen
 */
void app_moods_draw(void);

/**
 * @brief Handle touch events in the Pet Moods screen
 */
bool app_moods_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

#ifdef __cplusplus
}
#endif
