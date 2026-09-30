#include "app_common.h"
#include "display_gfx.h"
#include <stdio.h>
#include <time.h>
#include "esp_timer.h"
#include "audio_player.h"
#include "wifi_service.h"
#include "http_stream.h"
#include "battery_service.h"

void app_common_draw_header(const char *title)
{
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);
    gfx_draw_string_centered(70, 8, 180, title, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);
}

void app_common_draw_status_bar(void)
{
    // 1. Background (sleek dark status bar)
    gfx_fill_rect(0, 0, 320, 21, 0x0861);
    gfx_fill_rect(0, 21, 320, 1, 0x2124);

    // 2. Left side: Active running service badges (Audio player, Live stream, etc.)
    // Empty by default if no active service!
    int service_icon_x = 10;
    if (audio_player_get_state() == AUDIO_STATE_PLAYING) {
        gfx_draw_icon(service_icon_x, 10, ICON_MUSIC, COLOR_CYAN_ACCENT);
        service_icon_x += 16;
    }
    if (http_stream_is_active()) {
        // Red recording / live stream indicator dot
        gfx_fill_round_rect(service_icon_x - 3, 7, 7, 7, 3, COLOR_RED);
        service_icon_x += 14;
    }

    // 3. Middle: Clock / Time
    time_t now_sec;
    time(&now_sec);
    struct tm timeinfo;
    localtime_r(&now_sec, &timeinfo);

    char time_str[16];
    if (timeinfo.tm_year >= (2020 - 1900)) {
        strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
    } else {
        uint32_t total_sec = (uint32_t)(esp_timer_get_time() / 1000000ULL);
        int hours = (total_sec / 3600) % 24;
        int mins = (total_sec / 60) % 60;
        snprintf(time_str, sizeof(time_str), "%02d:%02d", hours, mins);
    }
    gfx_draw_string_centered(120, 3, 80, time_str, COLOR_WHITE, 0x0861, 1);

    // 4. Right side: WiFi icon, Battery icon with %
    bool wifi_connected = wifi_service_is_connected();
    gfx_draw_wifi_indicator(246, 11, wifi_connected);

    uint8_t bat_pct = battery_service_get_percentage();
    bool bat_charging = battery_service_is_charging();
    gfx_draw_battery(262, 6, 20, 10, bat_pct, bat_charging);

    char bat_str[10];
    snprintf(bat_str, sizeof(bat_str), "%u%%", (unsigned int)bat_pct);
    uint16_t pct_col = (bat_pct < 20) ? COLOR_RED : COLOR_WHITE;
    gfx_draw_string(286, 3, bat_str, pct_col, 0x0861, 1);
}

bool app_common_is_back_pressed(int tx, int ty)
{
    return (tx <= 70 && ty <= 30);
}
