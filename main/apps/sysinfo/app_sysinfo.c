#include "app_sysinfo.h"
#include <stdio.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "display_gfx.h"

void app_sysinfo_draw(const char *wifi_ssid, const char *wifi_ip)
{
    display_fill_screen(0x0841);
    app_common_draw_header("SYSTEM METRICS");

    // Network card
    gfx_fill_round_rect(10, 36, 300, 52, 6, 0x18E3);
    gfx_draw_string(18, 44, "WiFi Network :", COLOR_CYAN, 0x18E3, 1);
    gfx_draw_string(115, 44, wifi_ssid ? wifi_ssid : "N/A", COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 64, "IP Address   :", COLOR_CYAN, 0x18E3, 1);
    gfx_draw_string(115, 64, wifi_ip ? wifi_ip : "0.0.0.0", COLOR_GREEN, 0x18E3, 1);

    // Memory card
    gfx_fill_round_rect(10, 96, 300, 52, 6, 0x18E3);
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char buf[64];
    snprintf(buf, sizeof(buf), "Free Heap    : %u KB", (unsigned int)(free_heap / 1024));
    gfx_draw_string(18, 104, buf, COLOR_WHITE, 0x18E3, 1);
    snprintf(buf, sizeof(buf), "Free PSRAM   : %u KB", (unsigned int)(free_psram / 1024));
    gfx_draw_string(18, 124, buf, COLOR_WHITE, 0x18E3, 1);

    // Hardware specifications card
    gfx_fill_round_rect(10, 156, 300, 70, 6, 0x18E3);
    gfx_draw_string(18, 164, "Processor    : ESP32-S3 Dual-Core 240MHz", COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 180, "Display      : 2.4\" ILI9341 SPI (320x240)", COLOR_WHITE, 0x18E3, 1);
    gfx_draw_string(18, 196, "Touch IC     : XPT2046 Hardware SPI", COLOR_GREEN, 0x18E3, 1);
}

bool app_sysinfo_handle_touch(int tx, int ty, AppState *next_state)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        return true;
    }
    return false;
}
