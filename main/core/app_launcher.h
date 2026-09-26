#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "app_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize touch controller and app launcher subsystem
 */
void app_launcher_init(void);

/**
 * @brief Start the interactive App Launcher FreeRTOS task
 */
void app_launcher_start_task(void);

/**
 * @brief Update WiFi SSID and IP address shown in status bar & app views
 */
void app_launcher_set_wifi_info(const char *ssid, const char *ip);

/**
 * @brief Get current application state
 */
AppState app_launcher_get_current_state(void);

/**
 * @brief Programmatically switch application state
 */
void app_launcher_switch_state(AppState new_state);

#ifdef __cplusplus
}
#endif
