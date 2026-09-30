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
#include "wake_word_service.h"

static const char *TAG = "app_settings";
static const char *NVS_NAMESPACE = "settings";

static uint32_t s_screensaver_timeout_ms = 25000; // Default: 25s
static bool s_wake_word_enabled = true;           // Default: Enabled (Hot Word Jarvis)
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

        uint8_t saved_ww = 1;
        if (nvs_get_u8(nvs, "wake_word", &saved_ww) == ESP_OK) {
            s_wake_word_enabled = (saved_ww != 0);
        }

        nvs_close(nvs);
        ESP_LOGI(TAG, "Restored settings from NVS: Timeout=%lu ms, Volume=%d%%, WakeWord=%s",
                 (unsigned long)s_screensaver_timeout_ms, i2s_audio_get_volume(),
                 s_wake_word_enabled ? "ON" : "OFF");
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
        nvs_set_u8(nvs, "wake_word", s_wake_word_enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Settings persisted to NVS: Timeout=%lu ms, Volume=%d%%, WakeWord=%s",
                 (unsigned long)s_screensaver_timeout_ms, i2s_audio_get_volume(),
                 s_wake_word_enabled ? "ON" : "OFF");
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

bool app_settings_is_wake_word_enabled(void)
{
    return s_wake_word_enabled;
}

void app_settings_set_wake_word_enabled(bool enabled)
{
    if (s_wake_word_enabled == enabled) return;
    s_wake_word_enabled = enabled;

    if (!s_nvs_initialized) {
        app_settings_init();
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "wake_word", s_wake_word_enabled ? 1 : 0);
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "Wake word setting persisted to NVS: %s",
                 s_wake_word_enabled ? "ON" : "OFF");
    }

    // Apply live change immediately
    if (s_wake_word_enabled) {
        ESP_LOGI(TAG, "Starting Jarvis Wake Word service...");
        wake_word_service_start();
    } else {
        ESP_LOGI(TAG, "Stopping Jarvis Wake Word service (Freeing Core 1 ~30%% CPU)...");
        wake_word_service_stop();
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
        gfx_draw_round_rect(timeouts[i].x - 1, 47, 68, 28, 7, 0x0841);

        gfx_fill_round_rect(timeouts[i].x, 48, 66, 26, 6, bg);
        if (is_sel) {
            gfx_draw_round_rect(timeouts[i].x - 1, 47, 68, 28, 7, COLOR_WHITE);
        }
        gfx_draw_string_centered(timeouts[i].x, 53, 66, timeouts[i].name, fg, bg, 1);
    }
}

static void draw_volume_badge(void)
{
    uint8_t cur_vol = i2s_audio_get_volume();
    gfx_fill_round_rect(88, 94, 144, 26, 6, COLOR_CARD_BG);
    gfx_draw_round_rect(88, 94, 144, 26, 6, COLOR_CARD_BORDER);
    char vol_str[32];
    snprintf(vol_str, sizeof(vol_str), "VOLUME: %d%%", cur_vol);
    gfx_draw_string_centered(88, 99, 144, vol_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
}

static void draw_wake_word_buttons(void)
{
    // Label status badge at (176, 126)
    gfx_fill_rect(176, 126, 130, 14, 0x0841);
    if (s_wake_word_enabled) {
        gfx_draw_string(176, 126, "[Active ~30%]", COLOR_YELLOW_ACCENT, 0x0841, 1);
    } else {
        gfx_draw_string(176, 126, "[Off 0% CPU]", COLOR_GREEN_ACCENT, 0x0841, 1);
    }

    // Erase halo/borders around buttons
    gfx_draw_round_rect(13, 139, 144, 30, 7, 0x0841);
    gfx_draw_round_rect(163, 139, 144, 30, 7, 0x0841);

    if (s_wake_word_enabled) {
        // ON button: Selected (Cyan Accent, black text, white outline)
        gfx_fill_round_rect(14, 140, 142, 28, 6, COLOR_CYAN);
        gfx_draw_round_rect(13, 139, 144, 30, 7, COLOR_WHITE);
        gfx_draw_string_centered(14, 146, 142, "ON  (JARVIS)", COLOR_BLACK, COLOR_CYAN, 1);

        // OFF button: Unselected (Dark)
        gfx_fill_round_rect(164, 140, 142, 28, 6, 0x2124);
        gfx_draw_round_rect(164, 140, 142, 28, 6, COLOR_CARD_BORDER);
        gfx_draw_string_centered(164, 146, 142, "OFF (IDLE)", COLOR_WHITE, 0x2124, 1);
    } else {
        // ON button: Unselected (Dark)
        gfx_fill_round_rect(14, 140, 142, 28, 6, 0x2124);
        gfx_draw_round_rect(14, 140, 142, 28, 6, COLOR_CARD_BORDER);
        gfx_draw_string_centered(14, 146, 142, "ON  (JARVIS)", COLOR_WHITE, 0x2124, 1);

        // OFF button: Selected (Red/Maroon accent, white outline)
        gfx_fill_round_rect(164, 140, 142, 28, 6, 0x7800);
        gfx_draw_round_rect(163, 139, 144, 30, 7, COLOR_WHITE);
        gfx_draw_string_centered(164, 146, 142, "OFF (0% CPU)", COLOR_WHITE, 0x7800, 1);
    }
}

void app_settings_draw(void)
{
    display_fill_screen(0x0841);
    app_common_draw_header("SETTINGS");

    // 1. Screensaver timeout section (Y: 34 - 76)
    gfx_draw_string(14, 34, "AUTO-SCREENSAVER TIMEOUT:", COLOR_WHITE, 0x0841, 1);
    draw_timeout_buttons();

    // 2. Master Audio Volume section (Y: 80 - 122)
    gfx_draw_string(14, 80, "MASTER AUDIO VOLUME:", COLOR_WHITE, 0x0841, 1);

    // Vol Down [-] button (x: 14, w: 66, h: 26)
    gfx_fill_round_rect(14, 94, 66, 26, 6, 0x2124);
    gfx_draw_round_rect(14, 94, 66, 26, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(14, 99, 66, "- 10%", COLOR_WHITE, 0x2124, 1);

    // Current Volume Badge
    draw_volume_badge();

    // Vol Up [+] button (x: 240, w: 66, h: 26)
    gfx_fill_round_rect(240, 94, 66, 26, 6, 0x2124);
    gfx_draw_round_rect(240, 94, 66, 26, 6, COLOR_CARD_BORDER);
    gfx_draw_string_centered(240, 99, 66, "+ 10%", COLOR_CYAN_ACCENT, 0x2124, 1);

    // 3. Hot Word (Jarvis) section (Y: 126 - 170)
    gfx_draw_string(14, 126, "HOT WORD (JARVIS):", COLOR_WHITE, 0x0841, 1);
    draw_wake_word_buttons();

    // 4. Restart button (Y: 180 - 220)
    gfx_fill_round_rect(14, 180, 292, 40, 6, 0x7800);
    gfx_draw_round_rect(14, 180, 292, 40, 6, COLOR_RED_ACCENT);
    gfx_draw_string_centered(14, 192, 292, "RESTART PETBOT SYSTEM", COLOR_WHITE, 0x7800, 1);
}

bool app_settings_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // 1. Screensaver Timeouts (Y ~ 42..78)
    if (ty >= 42 && ty <= 78) {
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
    // 2. Audio Volume Controls (Y ~ 88..124)
    else if (ty >= 88 && ty <= 124) {
        uint8_t cur_vol = i2s_audio_get_volume();

        // Vol Down [-] (x: 14..80)
        if (tx >= 14 && tx <= 80) {
            uint8_t new_vol = (cur_vol >= 10) ? (cur_vol - 10) : 0;
            i2s_audio_set_volume(new_vol);
            app_settings_save_volume(new_vol);
            draw_volume_badge(); // In-place update: zero full-screen refresh/flicker!
            return true;
        }
        // Vol Up [+] (x: 240..308)
        else if (tx >= 240 && tx <= 308) {
            uint8_t new_vol = (cur_vol <= 90) ? (cur_vol + 10) : 100;
            i2s_audio_set_volume(new_vol);
            app_settings_save_volume(new_vol);
            draw_volume_badge(); // In-place update: zero full-screen refresh/flicker!
            return true;
        }
    }
    // 3. Hot Word (Jarvis) Switch (Y ~ 134..172)
    else if (ty >= 134 && ty <= 172) {
        // ON (x: 14..156)
        if (tx >= 14 && tx <= 156) {
            if (!s_wake_word_enabled) {
                app_settings_set_wake_word_enabled(true);
                draw_wake_word_buttons(); // In-place update: zero flicker
            }
            return true;
        }
        // OFF (x: 164..306)
        else if (tx >= 164 && tx <= 306) {
            if (s_wake_word_enabled) {
                app_settings_set_wake_word_enabled(false);
                draw_wake_word_buttons(); // In-place update: zero flicker
            }
            return true;
        }
    }
    // 4. Restart Button (Y ~ 176..226)
    else if (ty >= 176 && ty <= 226 && tx >= 14 && tx <= 306) {
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
