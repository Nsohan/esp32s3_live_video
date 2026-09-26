#include "app_common.h"
#include "display_gfx.h"

void app_common_draw_header(const char *title)
{
    gfx_fill_rect(0, 0, 320, 26, 0x10A2);
    gfx_fill_round_rect(4, 3, 58, 20, 4, 0x2124);
    gfx_draw_icon(14, 13, ICON_BACK, COLOR_WHITE);
    gfx_draw_string(24, 8, "BACK", COLOR_WHITE, 0x2124, 1);
    gfx_draw_string_centered(70, 8, 180, title, COLOR_CYAN, 0x10A2, 1);
    gfx_fill_rect(0, 26, 320, 1, 0x31A6);
}

bool app_common_is_back_pressed(int tx, int ty)
{
    return (tx <= 70 && ty <= 30);
}
