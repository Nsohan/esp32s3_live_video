#include "app_webstream.h"
#include <stdio.h>
#include "display.h"
#include "display_gfx.h"

void app_webstream_draw(const char *wifi_ip)
{
    display_fill_screen(0x0841);
    app_common_draw_header("LIVE STREAM & CONTROL");

    gfx_fill_round_rect(20, 45, 280, 160, 10, 0x18E3);
    gfx_draw_icon(160, 80, ICON_WEB_STREAM, COLOR_CYAN);
    gfx_draw_string_centered(20, 110, 280, "Open in any Web Browser:", COLOR_WHITE, 0x18E3, 1);

    char url_str[64];
    snprintf(url_str, sizeof(url_str), "http://%s", wifi_ip ? wifi_ip : "0.0.0.0");
    gfx_draw_string_centered(20, 135, 280, url_str, COLOR_YELLOW, 0x18E3, 2);

    gfx_draw_string_centered(20, 175, 280, "Camera Feed & Remote Controller", COLOR_TEXT_DIM, 0x18E3, 1);
}

bool app_webstream_handle_touch(int tx, int ty, AppState *next_state)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        return true;
    }
    return false;
}
