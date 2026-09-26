#pragma once

#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render Torch Flashlight screen
 */
void app_torch_draw(void);

/**
 * @brief Handle touch events in Torch screen (toggle on/off, back)
 */
bool app_torch_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw);

#ifdef __cplusplus
}
#endif
