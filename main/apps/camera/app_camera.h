#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the camera viewfinder subsystem and buffers
 */
void app_camera_init(void);

/**
 * @brief Capture a live frame, decode to RGB565, render HUD, and push to LCD
 */
void app_camera_update(void);

/**
 * @brief Handle touch events in the Camera viewfinder
 */
bool app_camera_handle_touch(int tx, int ty, AppState *next_state);

#ifdef __cplusplus
}
#endif
