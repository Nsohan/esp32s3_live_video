#pragma once

#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render Web Stream & Control connection instructions screen
 * @param wifi_ip Current assigned IP
 */
void app_webstream_draw(const char *wifi_ip);

/**
 * @brief Handle touch events in WebStream view
 */
bool app_webstream_handle_touch(int tx, int ty, AppState *next_state);

#ifdef __cplusplus
}
#endif
