#include "app_settings.h"
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "display_gfx.h"
#include "i2s_audio.h"

static const char *TAG = "app_settings";
static const char *NVS_NAMESPACE = "settings";

static uint32_t s_screensaver_timeout_ms = 25000; // Default: 25s
static bool s_nvs_initialized = false;

void app_settings_init(void)
{
    if (s_nvs_initialized) return;

    // Initialize NVS partition if not already initialized
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    s_nvs_initialized = true;

    // Load saved settings from NVS Flash
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t saved_timeout = 0;
        if (nvs_get_u32(nvs, "timeout_ms", &saved_timeout) == ESP_OK) {
            s_screensaver_timeout_ms = saved_timeout;
        }

        uint8_t saved_vol = 0;
        if (nvs_get_u8(nvs, "volume", &saved_vol) == ESP_OK && saved_vol <= 100) {
            i2s_audio_set_volume(saved_vol);
        }

        nvs_close(nvs);
        ESP_LOGI(TAG, "Restored saved settings from NVS: Timeout=%lu ms, Volume=%d%%",
                 (unsigned long)s_screensaver_timeout_ms, i2s_audio_get_volume());
    } else {
        ESP_LOGI(TAG, "No saved settings found in NVS, defaults will be used");
    }
}

void app_settings_save(void)
{
    if (!s_nvs_initialized) {
        app_settings_init();
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        nvs_set_u32(nvs, "timeout_ms", s_screensaver_timeout_ms);
        nvs_set_u8(nvs, "volume", i2s_audio_get_volume());
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Settings persisted to NVS: Timeout=%lu ms, Volume=%d%%",
                 (unsigned long)s_screensaver_timeout_ms, i2s_audio_get_volume());
    } else {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
    }
}

void app_settings_save_volume(uint8_t volume)
{
    if (!s_nvs_initialized) {
        app_settings_init();
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "volume", volume);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Volume setting persisted to NVS: %d%%", volume);
    }
}

static void draw_timeout_buttons(void)
{
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

        // Clear outer border area to erase previous selection halo cleanly
        gfx_draw_round_rect(timeouts[i].x - 1, 55, 68, 36, 7, 0x0841);

        gfx_fill_round_rect(timeouts[i].x, 56, 66, 34, 6, bg);
        if (is_sel) {
            gfx_draw_round_rect(timeouts[i].x - 1, 55, 68, 36, 7, COLOR_WHITE);
        }
        gfx_draw_string_centered(timeouts[i].x, 67, 66, timeouts[i].name, fg, bg, 1);
    }
}

static void draw_volume_badge(void)
{
    uint8_t cur_vol = i2s_audio_get_volume();
    gfx_fill_round_rect(90, 122, 142, 36, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(90, 122, 142, 36, 6, COLOR_CARD_BORDER);
    char vol_str[32];
    snprintf(vol_str, sizeof(vol_str), "VOLUME: %d%%", cur_vol);
    gfx_draw_string_centered(90, 133, 142, vol_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
}

void app_settings_draw(void)
{
    display_fill_screen(0x0841);
    app_common_draw_header("SETTINGS");

    // 1. Screensaver timeout section (Y: 40 - 92)
    gfx_draw_string(14, 40, "AUTO-SCREENSAVER TIMEOUT:", COLOR_WHITE, 0x0841, 1);
    draw_timeout_buttons();

    // 2. Master Audio Volume section (Y: 106 - 162)
    gfx_draw_string(14, 106, "MASTER AUDIO VOLUME:", COLOR_WHITE, 0x0841, 1);

    // Vol Down [-] button (x: 14, w: 66, h: 36)
    gfx_fill_round_rect(14, 122, 66, 36, 6, 0x2124);
    gfx_draw_round_rect(14, 122, 66, 36, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(14, 133, 66, "- 10%", COLOR_WHITE, 0x2124, 1);

    // Current Volume Badge
    draw_volume_badge();

    // Vol Up [+] button (x: 242, w: 66, h: 36)
    gfx_fill_round_rect(242, 122, 66, 36, 6, 0x2124);
    gfx_draw_round_rect(242, 122, 66, 36, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(242, 133, 66, "+ 10%", COLOR_CYAN_ACCENT, 0x2124, 1);

    // 3. Restart button (Y: 182 - 224)
    gfx_fill_round_rect(14, 182, 292, 42, 6, 0x7800);
    gfx_draw_round_rect(14, 182, 292, 42, 6, COLOR_RED_ACCENT);
    gfx_draw_string_centered(14, 196, 292, "RESTART PETBOT SYSTEM", COLOR_WHITE, 0x7800, 1);
}

bool app_settings_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // 1. Screensaver Timeouts (Y ~ 50..95)
    if (ty >= 50 && ty <= 95) {
        const uint32_t t_list[] = {15000, 25000, 60000, 0};
        for (int i = 0; i < 4; i++) {
            int bx = 14 + i * 76;
            if (tx >= bx && tx <= bx + 70) {
                if (s_screensaver_timeout_ms != t_list[i]) {
                    s_screensaver_timeout_ms = t_list[i];
                    app_settings_save(); // Persist instantly to NVS Flash
                    draw_timeout_buttons(); // In-place update: zero full-screen refresh/flicker!
                }
                return true;
            }
        }
    }
    // 2. Audio Volume Controls (Y ~ 115..165)
    else if (ty >= 115 && ty <= 165) {
        uint8_t cur_vol = i2s_audio_get_volume();

        // Vol Down [-] (x: 14..80)
        if (tx >= 14 && tx <= 80) {
            uint8_t new_vol = (cur_vol >= 10) ? (cur_vol - 10) : 0;
            i2s_audio_set_volume(new_vol);
            app_settings_save_volume(new_vol);
            draw_volume_badge(); // In-place update: zero full-screen refresh/flicker!
            return true;
        }
        // Vol Up [+] (x: 242..308)
        else if (tx >= 242 && tx <= 308) {
            uint8_t new_vol = (cur_vol <= 90) ? (cur_vol + 10) : 100;
            i2s_audio_set_volume(new_vol);
            app_settings_save_volume(new_vol);
            draw_volume_badge(); // In-place update: zero full-screen refresh/flicker!
            return true;
        }
    }
    // 3. Restart Button (Y ~ 175..230)
    else if (ty >= 175 && ty <= 230 && tx >= 14 && tx <= 306) {
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
