#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATE_APP_MENU,
    STATE_CAMERA_VIEW,
    STATE_ROBOEYES_VIEW,
    STATE_SETTINGS_VIEW,
    STATE_SYSINFO_VIEW,
    STATE_PET_MOODS_VIEW,
    STATE_WEB_STREAM_VIEW,
    STATE_TORCH_VIEW,
    STATE_CALIBRATE_VIEW
} AppState;

/**
 * @brief Initialize touch controller and app launcher subsystem
 */
void app_launcher_init(void);

/**
 * @brief Start the interactive App Launcher FreeRTOS task
 */
void app_launcher_start_task(void);

/**
 * @brief Update WiFi SSID and IP address shown in status bar & web view
 */
void app_launcher_set_wifi_info(const char *ssid, const char *ip);

#ifdef __cplusplus
}
#endif
