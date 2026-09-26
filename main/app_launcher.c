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
#include "esp_camera.h"

#include "display.h"
#include "display_gfx.h"
#include "touch_xpt2046.h"
#include "roboeyes_display.h"

static const char *TAG = "app_launcher";

static char s_wifi_ssid[32] = "Not Connected";
static char s_wifi_ip[32] = "0.0.0.0";
static AppState s_current_state = STATE_APP_MENU;
static bool s_state_needs_redraw = true;

// Configurable settings
static uint32_t s_screensaver_timeout_ms = 25000; // 25s auto-screensaver
static uint16_t s_current_eye_color = COLOR_CYAN;

// ─── App Tile Grid Definition ─────────────────────────────
typedef struct {
    int col;
    int row;
    IconType icon;
    const char *name;
    uint16_t color;
    AppState target_state;
} AppMenuItem;

static const AppMenuItem MENU_ITEMS[] = {
    {0, 0, ICON_CAMERA,     "Camera",     COLOR_MAGENTA,     STATE_CAMERA_VIEW},
    {1, 0, ICON_ROBOEYES,   "RoboEyes",   COLOR_CYAN,        STATE_ROBOEYES_VIEW},
    {2, 0, ICON_SETTINGS,   "Settings",   0x7BEF,            STATE_SETTINGS_VIEW},
    {3, 0, ICON_SYSINFO,    "Sys Info",   COLOR_YELLOW,      STATE_SYSINFO_VIEW},
    {0, 1, ICON_PET_MOODS,  "Moods",      COLOR_GREEN,       STATE_PET_MOODS_VIEW},
    {1, 1, ICON_WEB_STREAM, "WebStream",  COLOR_CYAN_ACCENT, STATE_WEB_STREAM_VIEW},
    {2, 1, ICON_TORCH,      "Torch",      0xFD20,            STATE_TORCH_VIEW},
    {3, 1, ICON_GALLERY,    "Touch Test", COLOR_CYAN_ACCENT, STATE_CALIBRATE_VIEW}
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

static void draw_header_bar(const char *title)
{
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);
    gfx_draw_string_centered(70, 8, 180, title, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);
}

// ─── 1. App Launcher Home Menu ────────────────────────────
static void draw_menu_screen(void)
{
    display_fill_screen(0x0000);

    // Status Bar
    gfx_fill_rect(0, 0, 320, 22, 0x0861);
    gfx_draw_icon(10, 11, ICON_WIFI, COLOR_GREEN);
    char status_str[64];
    snprintf(status_str, sizeof(status_str), "%s", s_wifi_ip);
    gfx_draw_string(22, 7, status_str, COLOR_WHITE, 0x0861, 1);

    uint32_t free_ram_kb = esp_get_free_heap_size() / 1024;
    char ram_str[32];
    snprintf(ram_str, sizeof(ram_str), "%uKB", (unsigned int)free_ram_kb);
    gfx_draw_string_centered(260, 7, 56, ram_str, COLOR_CYAN_ACCENT, 0x0861, 1);

    gfx_fill_rect(0, 22, 320, 1, 0x2124);

    // Draw all 8 App Tiles
    for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
        int tx, ty, tw, th;
        get_tile_rect(MENU_ITEMS[i].col, MENU_ITEMS[i].row, &tx, &ty, &tw, &th);
        gfx_draw_app_tile(tx, ty, tw, MENU_ITEMS[i].icon, MENU_ITEMS[i].name, MENU_ITEMS[i].color, false);
    }
}

// ─── 2. Camera View ───────────────────────────────────────
static void draw_camera_screen(void)
{
    display_fill_screen(0x0000);
    draw_header_bar("LIVE CAMERA");

    gfx_draw_round_rect(30, 40, 260, 170, 8, COLOR_CYAN);
    gfx_draw_string_centered(40, 110, 240, "Live Stream Active", COLOR_WHITE, 0x0000, 1);

    char url_str[64];
    snprintf(url_str, sizeof(url_str), "http://%s", s_wifi_ip);
    gfx_draw_string_centered(40, 130, 240, url_str, COLOR_YELLOW, 0x0000, 1);

    gfx_draw_string_centered(40, 170, 240, "Tap [BACK] to return", COLOR_TEXT_DIM, 0x0000, 1);
}

// ─── 3. Settings Screen ───────────────────────────────────
static void draw_settings_screen(void)
{
    display_fill_screen(0x0841);
    draw_header_bar("SETTINGS");

    // Color picker
    gfx_draw_string(14, 34, "ROBOEYES COLOR THEME:", COLOR_WHITE, 0x0841, 1);
    const struct { const char *name; uint16_t color; int x; } colors[] = {
        {"CYAN",   COLOR_CYAN,    14},
        {"GREEN",  COLOR_GREEN,   74},
        {"GOLD",   COLOR_YELLOW, 134},
        {"RED",    COLOR_RED,    194},
        {"PINK",   COLOR_MAGENTA,254}
    };
    for (int i = 0; i < 5; i++) {
        bool is_sel = (s_current_eye_color == colors[i].color);
        gfx_fill_round_rect(colors[i].x, 48, 52, 28, 6, colors[i].color);
        if (is_sel) {
            gfx_draw_round_rect(colors[i].x - 2, 46, 56, 32, 8, COLOR_WHITE);
        }
        gfx_draw_string_centered(colors[i].x, 57, 52, colors[i].name, COLOR_BLACK, colors[i].color, 1);
    }

    // Screensaver timeout
    gfx_draw_string(14, 88, "AUTO-SCREENSAVER TIMEOUT:", COLOR_WHITE, 0x0841, 1);
    const struct { const char *name; uint32_t ms; int x; } timeouts[] = {
        {"15s",   15000, 14},
        {"25s",   25000, 90},
        {"60s",   60000, 166},
        {"OFF",   0,     242}
    };
    for (int i = 0; i < 4; i++) {
        bool is_sel = (s_screensaver_timeout_ms == timeouts[i].ms);
        uint16_t bg = is_sel ? COLOR_CYAN : 0x2124;
        uint16_t fg = is_sel ? COLOR_BLACK : COLOR_WHITE;
        gfx_fill_round_rect(timeouts[i].x, 102, 66, 26, 6, bg);
        if (is_sel) {
            gfx_draw_round_rect(timeouts[i].x - 1, 101, 68, 28, 7, COLOR_WHITE);
        }
        gfx_draw_string_centered(timeouts[i].x, 110, 66, timeouts[i].name, fg, bg, 1);
    }

    // Touch Test button
    gfx_fill_round_rect(14, 140, 292, 34, 6, 0x243F);
    gfx_draw_string_centered(14, 151, 292, "TEST TOUCH CALIBRATION SCREEN", COLOR_WHITE, 0x243F, 1);

    // Restart button
    gfx_fill_round_rect(14, 185, 292, 34, 6, 0x7800);
    gfx_draw_string_centered(14, 196, 292, "RESTART PETBOT SYSTEM", COLOR_WHITE, 0x7800, 1);
}

// ─── 4. System Info ───────────────────────────────────────
static void draw_sysinfo_screen(void)
{
    display_fill_screen(0x0841);
    draw_header_bar("SYSTEM METRICS");

    gfx_fill_round_rect(10, 36, 300, 52, 6, 0x18E3);
    gfx_draw_string(18, 44, "WiFi Network :", COLOR_CYAN, 0x18E3, 1);
    gfx_draw_string(115, 44, s_wifi_ssid, COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 64, "IP Address   :", COLOR_CYAN, 0x18E3, 1);
    gfx_draw_string(115, 64, s_wifi_ip, COLOR_GREEN, 0x18E3, 1);

    gfx_fill_round_rect(10, 96, 300, 52, 6, 0x18E3);
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char buf[64];
    snprintf(buf, sizeof(buf), "Free Heap    : %u KB", (unsigned int)(free_heap / 1024));
    gfx_draw_string(18, 104, buf, COLOR_WHITE, 0x18E3, 1);
    snprintf(buf, sizeof(buf), "Free PSRAM   : %u KB", (unsigned int)(free_psram / 1024));
    gfx_draw_string(18, 124, buf, COLOR_WHITE, 0x18E3, 1);

    gfx_fill_round_rect(10, 156, 300, 70, 6, 0x18E3);
    gfx_draw_string(18, 164, "Processor    : ESP32-S3 Dual-Core 240MHz", COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 180, "Display      : 2.4\" ILI9341 SPI (320x240)", COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 196, "Touch IC     : XPT2046 Hardware SPI", COLOR_GREEN, 0x18E3, 1);
}

// ─── 5. Pet Moods ─────────────────────────────────────────
static void draw_pet_moods_screen(void)
{
    display_fill_screen(0x0841);
    draw_header_bar("EXPRESS EMOTIONS");

    const struct { const char *label; int mood_idx; uint16_t color; int x; int y; } moods[] = {
        {"HAPPY",    1, COLOR_GREEN,   14,  40},
        {"LAUGH",    2, COLOR_YELLOW,  166, 40},
        {"ANGRY",    3, COLOR_RED,     14,  100},
        {"TIRED",    4, 0x7BEF,        166, 100},
        {"CONFUSED", 5, COLOR_MAGENTA, 14,  160},
        {"WINKING",  9, COLOR_CYAN,    166, 160}
    };
    for (int i = 0; i < 6; i++) {
        gfx_fill_round_rect(moods[i].x, moods[i].y, 140, 48, 8, moods[i].color);
        gfx_draw_string_centered(moods[i].x, moods[i].y + 18, 140, moods[i].label, COLOR_BLACK, moods[i].color, 1);
    }
}

// ─── 6. Web Stream ────────────────────────────────────────
static void draw_web_stream_screen(void)
{
    display_fill_screen(0x0841);
    draw_header_bar("LIVE STREAM & CONTROL");

    gfx_fill_round_rect(20, 45, 280, 160, 10, 0x18E3);
    gfx_draw_icon(160, 80, ICON_WEB_STREAM, COLOR_CYAN);
    gfx_draw_string_centered(20, 110, 280, "Open in any Web Browser:", COLOR_WHITE, 0x18E3, 1);

    char url_str[64];
    snprintf(url_str, sizeof(url_str), "http://%s", s_wifi_ip);
    gfx_draw_string_centered(20, 135, 280, url_str, COLOR_YELLOW, 0x18E3, 2);

    gfx_draw_string_centered(20, 175, 280, "Camera Feed & Remote Controller", COLOR_TEXT_DIM, 0x18E3, 1);
}

// ─── 7. Torch ─────────────────────────────────────────────
static bool s_torch_on = false;

static void draw_torch_screen(void)
{
    if (s_torch_on) {
        display_fill_screen(COLOR_WHITE);
        draw_header_bar("TORCH [ON]");
        gfx_fill_round_rect(60, 80, 200, 80, 12, COLOR_YELLOW);
        gfx_draw_string_centered(60, 112, 200, "TORCH ACTIVE", COLOR_BLACK, COLOR_YELLOW, 2);
    } else {
        display_fill_screen(0x0841);
        draw_header_bar("TORCH [OFF]");
        gfx_fill_round_rect(60, 80, 200, 80, 12, 0x2124);
        gfx_draw_string_centered(60, 112, 200, "TAP TO TURN ON", COLOR_WHITE, 0x2124, 1);
    }
}

// ─── 8. Full-Screen Interactive Numbered Touch Grid ───────
#define CAL_GRID_COLS 6
#define CAL_GRID_ROWS 4
#define TOTAL_CAL_BLOCKS (CAL_GRID_COLS * CAL_GRID_ROWS)

static int s_active_block = -1;
static int s_last_hit_x = -1;
static int s_last_hit_y = -1;
static uint16_t s_last_raw_rx = 0;
static uint16_t s_last_raw_ry = 0;

static void get_cal_block_rect(int col, int row, int *x, int *y, int *w, int *h)
{
    *x = 6 + col * 52;
    *y = 32 + row * 52;
    *w = 48;
    *h = 48;
}

static void draw_cal_block(int num, int col, int row, bool is_hit)
{
    int bx, by, bw, bh;
    get_cal_block_rect(col, row, &bx, &by, &bw, &bh);

    uint16_t bg = is_hit ? COLOR_YELLOW : 0x18E3;
    uint16_t fg = is_hit ? COLOR_BLACK  : COLOR_WHITE;
    uint16_t border = is_hit ? COLOR_WHITE : 0x39E7;

    gfx_fill_round_rect(bx, by, bw, bh, 5, bg);
    gfx_draw_round_rect(bx, by, bw, bh, 5, border);

    char num_str[16];
    snprintf(num_str, sizeof(num_str), "[%d]", num);
    gfx_draw_string_centered(bx, by + (is_hit ? 8 : 16), bw, num_str, fg, bg, 1);

    if (is_hit) {
        gfx_draw_string_centered(bx, by + 28, bw, "HIT", COLOR_RED, bg, 1);
    }
}

static void draw_calibrate_screen(void)
{
    display_fill_screen(0x0000);

    // Header bar
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);

    char header_msg[64];
    if (s_active_block > 0) {
        snprintf(header_msg, sizeof(header_msg), "HIT #%d  X:%d Y:%d", s_active_block, s_last_hit_x, s_last_hit_y);
    } else {
        snprintf(header_msg, sizeof(header_msg), "TAP ANY NUMBERED BLOCK");
    }
    gfx_draw_string_centered(70, 8, 240, header_msg, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);

    // Draw all 24 blocks (6 columns x 4 rows)
    for (int row = 0; row < CAL_GRID_ROWS; row++) {
        for (int col = 0; col < CAL_GRID_COLS; col++) {
            int num = row * CAL_GRID_COLS + col + 1;
            draw_cal_block(num, col, row, (num == s_active_block));
        }
    }
}

static void update_calibrate_hit(int tx, int ty, uint16_t rx, uint16_t ry)
{
    s_last_hit_x = tx;
    s_last_hit_y = ty;
    s_last_raw_rx = rx;
    s_last_raw_ry = ry;

    // Determine which of the 24 blocks was tapped
    int col = (tx - 6) / 52;
    int row = (ty - 32) / 52;

    if (col < 0) col = 0;
    if (col >= CAL_GRID_COLS) col = CAL_GRID_COLS - 1;
    if (row < 0) row = 0;
    if (row >= CAL_GRID_ROWS) row = CAL_GRID_ROWS - 1;

    int new_block = row * CAL_GRID_COLS + col + 1;

    ESP_LOGI(TAG, ">>> TOUCH CALIBRATE: Hit Block #[%d] | Screen(X=%d, Y=%d) | Raw(rx=%u, ry=%u)",
             new_block, tx, ty, rx, ry);

    // Unhighlight previous block
    if (s_active_block > 0 && s_active_block != new_block) {
        int prev_idx = s_active_block - 1;
        draw_cal_block(s_active_block, prev_idx % CAL_GRID_COLS, prev_idx / CAL_GRID_COLS, false);
    }

    // Highlight new block
    s_active_block = new_block;
    draw_cal_block(s_active_block, col, row, true);

    // Update Header Bar info
    gfx_fill_rect(70, 0, 248, 25, 0x10A2);
    char header_msg[64];
    snprintf(header_msg, sizeof(header_msg), "HIT #%d X:%d Y:%d (R:%u,%u)", s_active_block, tx, ty, rx, ry);
    gfx_draw_string_centered(65, 8, 250, header_msg, COLOR_YELLOW, 0x10A2, 1);
}

// ─── Touch Event Handler ──────────────────────────────────
static void handle_touch_event(int tx, int ty, uint16_t rx, uint16_t ry)
{
    ESP_LOGI(TAG, "Touch Triggered: X=%d, Y=%d (State: %d)", tx, ty, s_current_state);

    if (s_current_state == STATE_ROBOEYES_VIEW) {
        roboeyes_set_active(false);
        s_current_state = STATE_APP_MENU;
        s_state_needs_redraw = true;
        return;
    }

    // Check [BACK] Button ($x \in [0, 70], y \in [0, 30]$)
    if (s_current_state != STATE_APP_MENU && tx <= 70 && ty <= 30) {
        s_current_state = STATE_APP_MENU;
        s_state_needs_redraw = true;
        return;
    }

    switch (s_current_state) {
        case STATE_APP_MENU: {
            for (size_t i = 0; i < MENU_ITEM_COUNT; i++) {
                int tile_x, tile_y, tile_w, tile_h;
                get_tile_rect(MENU_ITEMS[i].col, MENU_ITEMS[i].row, &tile_x, &tile_y, &tile_w, &tile_h);

                if (tx >= (tile_x - 10) && tx <= (tile_x + tile_w + 10) &&
                    ty >= (tile_y - 10) && ty <= (tile_y + tile_h + 24)) {

                    gfx_draw_app_tile(tile_x, tile_y, tile_w, MENU_ITEMS[i].icon, MENU_ITEMS[i].name, MENU_ITEMS[i].color, true);
                    vTaskDelay(pdMS_TO_TICKS(120));

                    s_current_state = MENU_ITEMS[i].target_state;
                    s_state_needs_redraw = true;

                    if (s_current_state == STATE_ROBOEYES_VIEW) {
                        roboeyes_set_active(true);
                    }
                    return;
                }
            }
            break;
        }

        case STATE_SETTINGS_VIEW: {
            // Colors (y ~ 45..80)
            if (ty >= 44 && ty <= 82) {
                const uint16_t c_list[] = {COLOR_CYAN, COLOR_GREEN, COLOR_YELLOW, COLOR_RED, COLOR_MAGENTA};
                for (int i = 0; i < 5; i++) {
                    int bx = 14 + i * 60;
                    if (tx >= bx && tx <= bx + 56) {
                        s_current_eye_color = c_list[i];
                        roboeyes_set_color(s_current_eye_color);
                        s_state_needs_redraw = true;
                        return;
                    }
                }
            }
            // Timeouts (y ~ 98..132)
            else if (ty >= 98 && ty <= 132) {
                const uint32_t t_list[] = {15000, 25000, 60000, 0};
                for (int i = 0; i < 4; i++) {
                    int bx = 14 + i * 76;
                    if (tx >= bx && tx <= bx + 70) {
                        s_screensaver_timeout_ms = t_list[i];
                        s_state_needs_redraw = true;
                        return;
                    }
                }
            }
            // Touch test button (y ~ 138..176)
            else if (ty >= 138 && ty <= 176 && tx >= 14 && tx <= 306) {
                s_current_state = STATE_CALIBRATE_VIEW;
                s_active_block = -1;
                s_last_hit_x = -1;
                s_last_hit_y = -1;
                s_state_needs_redraw = true;
            }
            // Restart Button (y ~ 182..220)
            else if (ty >= 182 && ty <= 220 && tx >= 14 && tx <= 306) {
                display_fill_screen(0x0000);
                gfx_draw_string_centered(0, 110, 320, "Rebooting PetBot...", COLOR_RED, 0x0000, 2);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
            break;
        }

        case STATE_CALIBRATE_VIEW: {
            update_calibrate_hit(tx, ty, rx, ry);
            break;
        }

        case STATE_PET_MOODS_VIEW: {
            const struct { int mood; int x1; int y1; int x2; int y2; } m_boxes[] = {
                {1, 14, 40, 154, 88},
                {2, 166, 40, 306, 88},
                {3, 14, 100, 154, 148},
                {4, 166, 100, 306, 148},
                {5, 14, 160, 154, 208},
                {9, 166, 160, 306, 208}
            };
            for (int i = 0; i < 6; i++) {
                if (tx >= m_boxes[i].x1 && tx <= m_boxes[i].x2 &&
                    ty >= m_boxes[i].y1 && ty <= m_boxes[i].y2) {
                    roboeyes_trigger_mood(m_boxes[i].mood);
                    s_current_state = STATE_ROBOEYES_VIEW;
                    s_state_needs_redraw = true;
                    return;
                }
            }
            break;
        }

        case STATE_TORCH_VIEW: {
            if (ty >= 80 && ty <= 160) {
                s_torch_on = !s_torch_on;
                s_state_needs_redraw = true;
            }
            break;
        }

        default:
            break;
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
                    draw_menu_screen();
                    break;
                case STATE_CAMERA_VIEW:
                    draw_camera_screen();
                    break;
                case STATE_SETTINGS_VIEW:
                    draw_settings_screen();
                    break;
                case STATE_SYSINFO_VIEW:
                    draw_sysinfo_screen();
                    break;
                case STATE_PET_MOODS_VIEW:
                    draw_pet_moods_screen();
                    break;
                case STATE_WEB_STREAM_VIEW:
                    draw_web_stream_screen();
                    break;
                case STATE_TORCH_VIEW:
                    draw_torch_screen();
                    break;
                case STATE_CALIBRATE_VIEW:
                    draw_calibrate_screen();
                    break;
                case STATE_ROBOEYES_VIEW:
                    break;
            }
        }

        // Poll Touch Screen with full coordinates and raw ADC
        int tx = -1, ty = -1;
        uint16_t rx = 0, ry = 0;
        if (touch_read_all(&tx, &ty, &rx, &ry)) {
            last_touch_activity = now;
            if (!touch_held || s_current_state == STATE_CALIBRATE_VIEW) {
                touch_held = true;
                handle_touch_event(tx, ty, rx, ry);
            }
        } else {
            touch_held = false;
        }

        // Auto-Screensaver Idle Trigger
        if (s_current_state == STATE_APP_MENU && s_screensaver_timeout_ms > 0) {
            if (now - last_touch_activity >= s_screensaver_timeout_ms) {
                ESP_LOGI(TAG, "Idle timeout reached (%ums) -> Switching to RoboEyes Screen Saver",
                         (unsigned int)s_screensaver_timeout_ms);
                s_current_state = STATE_ROBOEYES_VIEW;
                s_state_needs_redraw = true;
                roboeyes_set_active(true);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void app_launcher_init(void)
{
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
    if (ssid) strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
    if (ip) strncpy(s_wifi_ip, ip, sizeof(s_wifi_ip) - 1);
    s_state_needs_redraw = true;
}
