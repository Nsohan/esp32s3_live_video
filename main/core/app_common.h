#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Application / View States
 */
typedef enum {
    STATE_APP_MENU,         // Main App Drawer Home Screen
    STATE_CAMERA_VIEW,      // Live Camera App
    STATE_ROBOEYES_VIEW,    // RoboEyes Screensaver / Eyes Display
    STATE_SETTINGS_VIEW,    // Settings App
    STATE_SYSINFO_VIEW,     // System Metrics App
    STATE_PET_MOODS_VIEW,   // Emotions / Moods App
    STATE_WEB_STREAM_VIEW,  // Web Stream Info App
    STATE_MUSIC_VIEW,       // SD Music & Sound Player App
    STATE_CALIBRATE_VIEW    // 24-Block Touch Calibration & Diagnostic App
} AppState;

/**
 * @brief Helper to draw a unified modern top header bar with a [BACK] button
 * @param title Screen title text
 */
void app_common_draw_header(const char *title);

/**
 * @brief Draw modern smartphone-style status bar (notifications on left, time in middle, wifi & battery on right)
 */
void app_common_draw_status_bar(void);

/**
 * @brief Check if the [BACK] button area was pressed (Top-Left: X <= 70, Y <= 30)
 * @param tx Touch X coordinate
 * @param ty Touch Y coordinate
 * @return true if [BACK] was pressed
 */
bool app_common_is_back_pressed(int tx, int ty);

#ifdef __cplusplus
}
#endif
