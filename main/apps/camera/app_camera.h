#pragma once

#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render the Live Camera UI view
 * @param wifi_ip Current device IP for stream URL
 */
void app_camera_draw(const char *wifi_ip);

/**
 * @brief Handle touch events in the Camera view
 * @param tx Touch X
 * @param ty Touch Y
 * @param next_state Output next AppState
 * @return true if event handled
 */
bool app_camera_handle_touch(int tx, int ty, AppState *next_state);

#ifdef __cplusplus
}
#endif
