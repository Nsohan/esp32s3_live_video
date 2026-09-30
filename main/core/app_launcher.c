#include "app_launcher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Drivers
#include "display.h"
#include "display_gfx.h"
#include "touch_xpt2046.h"

// Apps
#include "app_camera.h"
#include "roboeyes_display.h"
#include "app_settings.h"
#include "app_sysinfo.h"
#include "app_moods.h"
#include "app_webstream.h"
#include "app_touch_test.h"
#include "app_music.h"
#include "app_recorder.h"
#include "audio_player.h"

static const char *TAG = "app_launcher";

static char s_wifi_ssid[32] = "Not Connected";
static char s_wifi_ip[32] = "0.0.0.0";
static AppState s_current_state = STATE_APP_MENU;
static bool s_state_needs_redraw = true;
static volatile bool s_status_bar_needs_update = false;

// ─── App Drawer Pagination & Grid Definition ─────────────
#define TOTAL_PAGES 2
static int s_current_page = 0;

typedef struct {
    int page;
    int col;
    int row;
    IconType icon;
    const char *name;
    uint16_t color;
    AppState target_state;
} AppMenuItem;

static const AppMenuItem MENU_ITEMS[] = {
    // ─── PAGE 0 (Main Page) ───────────────────────────────
    {0, 0, 0, ICON_CAMERA,     "Camera",     COLOR_MAGENTA,       STATE_CAMERA_VIEW},
    {0, 1, 0, ICON_ROBOEYES,   "RoboEyes",   COLOR_CYAN,          STATE_ROBOEYES_VIEW},
    {0, 2, 0, ICON_MUSIC,      "Music",      COLOR_ORANGE_ACCENT, STATE_MUSIC_VIEW},
    {0, 3, 0, ICON_SETTINGS,   "Settings",   0x7BEF,              STATE_SETTINGS_VIEW},
    {0, 0, 1, ICON_PET_MOODS,  "Moods",      COLOR_GREEN,         STATE_PET_MOODS_VIEW},
    {0, 1, 1, ICON_SYSINFO,    "Sys Info",   COLOR_YELLOW,        STATE_SYSINFO_VIEW},
    {0, 2, 1, ICON_RECORDER,   "Recorder",   COLOR_RED_ACCENT,    STATE_RECORDER_VIEW},
    {0, 3, 1, ICON_GALLERY,    "Touch Test", COLOR_CYAN_ACCENT,   STATE_CALIBRATE_VIEW},

    // ─── PAGE 1 (Second Page) ─────────────────────────────
    {1, 0, 0, ICON_WEB_STREAM, "WebStream",  COLOR_CYAN_ACCENT,   STATE_WEB_STREAM_VIEW}
};

#define MENU_ITEM_COUNT (sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]))

#define TILE_SIZE 52
#define GRID_START_X 16
#define GRID_START_Y 38
#define GRID_GAP_X 24
#define GRID_GAP_Y 22

static void get_tile_rect(int col, int row, int *x, int *y, int *w, int *h)
{
    *x = GRID_START_X + col * (TILE_SIZE + GRID_GAP_X);
    *y = GRID_START_Y + row * (TILE_SIZE + GRID_GAP_Y + 16);
    *w = TILE_SIZE;
    *h = TILE_SIZE;
}

static void draw_arrow_right(int cx, int cy, uint16_t color)
{
    // Shaft
    gfx_fill_rect(cx - 8, cy - 1, 14, 2, color);
    // Upper diagonal
    gfx_fill_rect(cx + 3, cy - 2, 2, 2, color);
    gfx_fill_rect(cx + 1, cy - 4, 2, 2, color);
    gfx_fill_rect(cx - 1, cy - 6, 2, 2, color);
    // Lower diagonal
    gfx_fill_rect(cx + 3, cy + 1, 2, 2, color);
    gfx_fill_rect(cx + 1, cy + 3, 2, 2, color);
    gfx_fill_rect(cx - 1, cy + 5, 2, 2, color);
    // Arrow tip
    gfx_fill_rect(cx + 5, cy - 1, 2, 2, color);
}

static void draw_arrow_left(int cx, int cy, uint16_t color)
{
    // Shaft
    gfx_fill_rect(cx - 6, cy - 1, 14, 2, color);
    // Upper diagonal
    gfx_fill_rect(cx - 5, cy - 2, 2, 2, color);
    gfx_fill_rect(cx - 3, cy - 4, 2, 2, color);
    gfx_fill_rect(cx - 1, cy - 6, 2, 2, color);
    // Lower diagonal
    gfx_fill_rect(cx - 5, cy + 1, 2, 2, color);
    gfx_fill_rect(cx - 3, cy + 3, 2, 2, color);
    gfx_fill_rect(cx - 1, cy + 5, 2, 2, color);
    // Arrow tip
    gfx_fill_rect(cx - 7, cy - 1, 2, 2, color);
}

// ─── Home Screen App Drawer ───────────────────────────────
static void draw_home_menu_screen(void)
{
    display_fill_screen(0x0000);

    // Modern Smartphone-Style Status Bar (force redraw on screen draw)
    app_common_draw_status_bar(true);

    // Draw all App Tiles belonging to current active page
    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        if (MENU_ITEMS[i].page == s_current_page) {
            int tx, ty, tw, th;
            get_tile_rect(MENU_ITEMS[i].col, MENU_ITEMS[i].row, &tx, &ty, &tw, &th);
            gfx_draw_app_tile(tx, ty, tw, MENU_ITEMS[i].icon, MENU_ITEMS[i].name, MENU_ITEMS[i].color, false);
        }
    }

    // Modern Smartphone Page Indicator Dots & Arrow Navigation (Y: 216 - 228)
    int dot_y = 218;
    if (s_current_page == 0) {
        // Page 0: Active pill (cyan) + Inactive circle (gray)
        gfx_fill_round_rect(144, dot_y, 14, 6, 3, COLOR_CYAN_ACCENT);
        gfx_fill_round_rect(164, dot_y, 6, 6, 3, COLOR_CARD_BORDER);
        // Right arrow → to navigate to Page 2
        draw_arrow_right(290, dot_y + 3, COLOR_CYAN_ACCENT);
    } else {
        // Page 1: Inactive circle (gray) + Active pill (cyan)
        gfx_fill_round_rect(144, dot_y, 6, 6, 3, COLOR_CARD_BORDER);
        gfx_fill_round_rect(156, dot_y, 14, 6, 3, COLOR_CYAN_ACCENT);
        // Left arrow ← to navigate to Page 1
        draw_arrow_left(30, dot_y + 3, COLOR_CYAN_ACCENT);
    }
}

// ─── Touch Event Dispatcher ───────────────────────────────
static void handle_touch_event(int tx, int ty, uint16_t rx, uint16_t ry)
{
    ESP_LOGI(TAG, "Touch Triggered: X=%d, Y=%d (Current State: %d)", tx, ty, s_current_state);

    // 1. If currently in RoboEyes Screensaver, touching any part wakes up to App Menu
    if (s_current_state == STATE_ROBOEYES_VIEW) {
        roboeyes_set_active(false);
        s_current_state = STATE_APP_MENU;
        s_state_needs_redraw = true;
        return;
    }

    // 2. Dispatch touch event based on current active view
    AppState next_state = s_current_state;
    bool handled = false;

    switch (s_current_state) {
        case STATE_APP_MENU: {
            // 1. Bottom Page Navigation Tap (Y >= 195)
            if (ty >= 195) {
                if (tx > 180 && s_current_page < TOTAL_PAGES - 1) {
                    s_current_page++;
                    s_state_needs_redraw = true;
                    return;
                } else if (tx < 140 && s_current_page > 0) {
                    s_current_page--;
                    s_state_needs_redraw = true;
                    return;
                } else if (tx >= 140 && tx <= 180) {
                    s_current_page = (s_current_page == 0) ? 1 : 0;
                    s_state_needs_redraw = true;
                    return;
                }
            }

            // 2. App Tile Taps on the current active page
            for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
                if (MENU_ITEMS[i].page != s_current_page) continue;

                int tile_x, tile_y, tile_w, tile_h;
                get_tile_rect(MENU_ITEMS[i].col, MENU_ITEMS[i].row, &tile_x, &tile_y, &tile_w, &tile_h);

                if (tx >= (tile_x - 10) && tx <= (tile_x + tile_w + 10) &&
                    ty >= (tile_y - 10) && ty <= (tile_y + tile_h + 24)) {

                    gfx_draw_app_tile(tile_x, tile_y, tile_w, MENU_ITEMS[i].icon, MENU_ITEMS[i].name, MENU_ITEMS[i].color, true);
                    vTaskDelay(pdMS_TO_TICKS(120));

                    s_current_state = MENU_ITEMS[i].target_state;
                    s_state_needs_redraw = true;

                    if (s_current_state == STATE_ROBOEYES_VIEW) {
                        if (audio_player_get_state() == AUDIO_STATE_PLAYING) {
                            roboeyes_trigger_mood(ROBOEYES_MODE_MUSIC_LISTENING);
                        }
                        roboeyes_set_active(true);
                    } else if (s_current_state == STATE_CALIBRATE_VIEW) {
                        app_touch_test_reset();
                    } else if (s_current_state == STATE_RECORDER_VIEW) {
                        app_recorder_init();
                    }

                    return;
                }
            }
            break;
        }

        case STATE_CAMERA_VIEW:
            handled = app_camera_handle_touch(tx, ty, &next_state);
            break;

        case STATE_SETTINGS_VIEW:
            handled = app_settings_handle_touch(tx, ty, &next_state, &s_state_needs_redraw);
            break;

        case STATE_SYSINFO_VIEW:
            handled = app_sysinfo_handle_touch(tx, ty, &next_state);
            break;

        case STATE_PET_MOODS_VIEW:
            handled = app_moods_handle_touch(tx, ty, &next_state, &s_state_needs_redraw);
            break;

        case STATE_WEB_STREAM_VIEW:
            handled = app_webstream_handle_touch(tx, ty, &next_state);
            break;

        case STATE_MUSIC_VIEW:
            handled = app_music_handle_touch(tx, ty, &next_state, &s_state_needs_redraw);
            break;

        case STATE_RECORDER_VIEW:
            handled = app_recorder_handle_touch(tx, ty, &next_state, &s_state_needs_redraw);
            break;

        case STATE_CALIBRATE_VIEW:
            handled = app_touch_test_handle_touch(tx, ty, rx, ry, &next_state);
            break;

        default:
            break;
    }

    if (handled && next_state != s_current_state) {
        if (s_current_state == STATE_RECORDER_VIEW) {
            app_recorder_stop();
        }

        s_current_state = next_state;
        s_state_needs_redraw = true;

        if (s_current_state == STATE_ROBOEYES_VIEW) {
            roboeyes_set_active(true);
        } else if (s_current_state == STATE_MUSIC_VIEW) {
            app_music_init();
        } else if (s_current_state == STATE_RECORDER_VIEW) {
            app_recorder_init();
        }
    }
}

// ─── Main App Launcher Task ───────────────────────────────
static void app_launcher_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Starting App Launcher & Touch Navigation Manager...");

    touch_init();

    uint32_t last_touch_activity = (uint32_t)(esp_timer_get_time() / 1000ULL);
    bool touch_held = false;

    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

        if (s_state_needs_redraw) {
            s_state_needs_redraw = false;

            switch (s_current_state) {
                case STATE_APP_MENU:
                    draw_home_menu_screen();
                    break;
                case STATE_CAMERA_VIEW:
                    // Continuously stream live frames in the viewfinder
                    break;
                case STATE_SETTINGS_VIEW:
                    app_settings_draw();
                    break;
                case STATE_SYSINFO_VIEW:
                    app_sysinfo_draw(s_wifi_ssid, s_wifi_ip);
                    break;
                case STATE_PET_MOODS_VIEW:
                    app_moods_draw();
                    break;
                case STATE_WEB_STREAM_VIEW:
                    app_webstream_draw(s_wifi_ip);
                    break;
                case STATE_MUSIC_VIEW:
                    app_music_draw();
                    break;
                case STATE_RECORDER_VIEW:
                    app_recorder_draw();
                    break;
                case STATE_CALIBRATE_VIEW:
                    app_touch_test_draw();
                    break;
                case STATE_ROBOEYES_VIEW:
                    break;
            }
        }

        // Continuous / differential app updates
        if (s_current_state == STATE_CAMERA_VIEW) {
            app_camera_update();
        } else if (s_current_state == STATE_MUSIC_VIEW) {
            app_music_update();
        } else if (s_current_state == STATE_RECORDER_VIEW) {
            app_recorder_update();
        } else if (s_current_state == STATE_APP_MENU) {
            static uint32_t last_status_bar_tick = 0;
            if (now - last_status_bar_tick >= 1000 || s_status_bar_needs_update) {
                app_common_draw_status_bar(false);
                last_status_bar_tick = now;
                s_status_bar_needs_update = false;
            }
        }

        // Poll Touch Screen
        int tx = -1, ty = -1;
        uint16_t rx = 0, ry = 0;
        if (touch_read_all(&tx, &ty, &rx, &ry)) {
            last_touch_activity = now;
            if (!touch_held || s_current_state == STATE_CALIBRATE_VIEW || s_current_state == STATE_CAMERA_VIEW) {
                touch_held = true;
                handle_touch_event(tx, ty, rx, ry);
            }
        } else {
            touch_held = false;
        }

        // Auto-Screensaver Idle Trigger
        uint32_t screensaver_timeout = app_settings_get_screensaver_timeout_ms();
        if ((s_current_state == STATE_APP_MENU || 
             s_current_state == STATE_MUSIC_VIEW ||
             s_current_state == STATE_PET_MOODS_VIEW ||
             s_current_state == STATE_SYSINFO_VIEW ||
             s_current_state == STATE_SETTINGS_VIEW) && screensaver_timeout > 0) {
            if (now - last_touch_activity >= screensaver_timeout) {
                ESP_LOGI(TAG, "Idle timeout reached (%ums) -> Switching to RoboEyes Screen Saver",
                         (unsigned int)screensaver_timeout);
                s_current_state = STATE_ROBOEYES_VIEW;
                s_state_needs_redraw = true;
                if (audio_player_get_state() == AUDIO_STATE_PLAYING) {
                    roboeyes_trigger_mood(ROBOEYES_MODE_MUSIC_LISTENING);
                }
                roboeyes_set_active(true);
            }
        }


        if (s_current_state != STATE_CAMERA_VIEW) {
            vTaskDelay(pdMS_TO_TICKS(25));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5)); // Minimal yield for high camera preview frame rate
        }
    }
}

void app_launcher_init(void)
{
    app_camera_init();
    app_music_init();
    ESP_LOGI(TAG, "App Launcher initialized");
}

void app_launcher_start_task(void)
{
    xTaskCreatePinnedToCore(
        app_launcher_task,
        "app_launcher",
        8192,
        NULL,
        4,
        NULL,
        0
    );
    ESP_LOGI(TAG, "App Launcher background task created on Core 0!");
}

void app_launcher_set_wifi_info(const char *ssid, const char *ip)
{
    bool changed = false;
    if (ssid && strcmp(s_wifi_ssid, ssid) != 0) {
        strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
        s_wifi_ssid[sizeof(s_wifi_ssid) - 1] = '\0';
        changed = true;
    }
    if (ip && strcmp(s_wifi_ip, ip) != 0) {
        strncpy(s_wifi_ip, ip, sizeof(s_wifi_ip) - 1);
        s_wifi_ip[sizeof(s_wifi_ip) - 1] = '\0';
        changed = true;
    }

    if (!changed) {
        return;
    }

    // Only views that actually render the SSID or IP text directly need a full redraw
    if (s_current_state == STATE_SYSINFO_VIEW || s_current_state == STATE_WEB_STREAM_VIEW) {
        s_state_needs_redraw = true;
    } else if (s_current_state == STATE_APP_MENU) {
        // App Menu only needs the status bar at the top updated - NEVER reload the full screen!
        s_status_bar_needs_update = true;
    }
}

AppState app_launcher_get_current_state(void)
{
    return s_current_state;
}

void app_launcher_switch_state(AppState new_state)
{
    if (s_current_state == STATE_RECORDER_VIEW && new_state != STATE_RECORDER_VIEW) {
        app_recorder_stop();
    }
    s_current_state = new_state;
    s_state_needs_redraw = true;

    if (s_current_state == STATE_ROBOEYES_VIEW) {
        roboeyes_set_active(true);
    }
}
