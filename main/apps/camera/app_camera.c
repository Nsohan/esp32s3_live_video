#include "app_camera.h"
#include <stdio.h>
#include "display.h"
#include "display_gfx.h"

void app_camera_draw(const char *wifi_ip)
{
    display_fill_screen(0x0000);
    app_common_draw_header("LIVE CAMERA");

    gfx_draw_round_rect(30, 40, 260, 170, 8, COLOR_CYAN);
    gfx_draw_string_centered(40, 110, 240, "Live Stream Active", COLOR_WHITE, 0x0000, 1);

    char url_str[64];
    snprintf(url_str, sizeof(url_str), "http://%s", wifi_ip ? wifi_ip : "0.0.0.0");
    gfx_draw_string_centered(40, 130, 240, url_str, COLOR_YELLOW, 0x0000, 1);

    gfx_draw_string_centered(40, 170, 240, "Tap [BACK] to return", COLOR_TEXT_DIM, 0x0000, 1);
}

bool app_camera_handle_touch(int tx, int ty, AppState *next_state)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        return true;
    }
    return false;
}
