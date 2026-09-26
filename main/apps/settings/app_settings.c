#include "app_settings.h"
#include <stdio.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "display_gfx.h"
#include "roboeyes_display.h"

static uint32_t s_screensaver_timeout_ms = 25000; // Default: 25s
static uint16_t s_current_eye_color = COLOR_CYAN;

void app_settings_draw(void)
{
    display_fill_screen(0x0841);
    app_common_draw_header("SETTINGS");

    // 1. Color picker
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

    // 2. Screensaver timeout
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

    // 3. Touch Test button
    gfx_fill_round_rect(14, 140, 292, 34, 6, 0x243F);
    gfx_draw_string_centered(14, 151, 292, "TEST TOUCH CALIBRATION SCREEN", COLOR_WHITE, 0x243F, 1);

    // 4. Restart button
    gfx_fill_round_rect(14, 185, 292, 34, 6, 0x7800);
    gfx_draw_string_centered(14, 196, 292, "RESTART PETBOT SYSTEM", COLOR_WHITE, 0x7800, 1);
}

bool app_settings_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // Colors (y ~ 44..82)
    if (ty >= 44 && ty <= 82) {
        const uint16_t c_list[] = {COLOR_CYAN, COLOR_GREEN, COLOR_YELLOW, COLOR_RED, COLOR_MAGENTA};
        for (int i = 0; i < 5; i++) {
            int bx = 14 + i * 60;
            if (tx >= bx && tx <= bx + 56) {
                s_current_eye_color = c_list[i];
                roboeyes_set_color(s_current_eye_color);
                if (needs_redraw) *needs_redraw = true;
                return true;
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
                if (needs_redraw) *needs_redraw = true;
                return true;
            }
        }
    }
    // Touch test button (y ~ 138..176)
    else if (ty >= 138 && ty <= 176 && tx >= 14 && tx <= 306) {
        if (next_state) *next_state = STATE_CALIBRATE_VIEW;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }
    // Restart Button (y ~ 182..220)
    else if (ty >= 182 && ty <= 220 && tx >= 14 && tx <= 306) {
        display_fill_screen(0x0000);
        gfx_draw_string_centered(0, 110, 320, "Rebooting PetBot...", COLOR_RED, 0x0000, 2);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return true;
    }

    return false;
}

uint32_t app_settings_get_screensaver_timeout_ms(void)
{
    return s_screensaver_timeout_ms;
}

uint16_t app_settings_get_eye_color(void)
{
    return s_current_eye_color;
}
