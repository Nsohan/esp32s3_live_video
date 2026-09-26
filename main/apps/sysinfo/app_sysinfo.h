#pragma once

#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Render System Metrics and Hardware Info screen
 * @param wifi_ssid Current connected WiFi SSID
 * @param wifi_ip Current assigned IP
 */
void app_sysinfo_draw(const char *wifi_ssid, const char *wifi_ip);

/**
 * @brief Handle touch events in SysInfo view
 */
bool app_sysinfo_handle_touch(int tx, int ty, AppState *next_state);

#ifdef __cplusplus
}
#endif
