#include "app_torch.h"
#include "display.h"
#include "display_gfx.h"

static bool s_torch_on = false;

void app_torch_draw(void)
{
    if (s_torch_on) {
        display_fill_screen(COLOR_WHITE);
        app_common_draw_header("TORCH [ON]");
        gfx_fill_round_rect(60, 80, 200, 80, 12, COLOR_YELLOW);
        gfx_draw_string_centered(60, 112, 200, "TORCH ACTIVE", COLOR_BLACK, COLOR_YELLOW, 2);
    } else {
        display_fill_screen(0x0841);
        app_common_draw_header("TORCH [OFF]");
        gfx_fill_round_rect(60, 80, 200, 80, 12, 0x2124);
        gfx_draw_string_centered(60, 112, 200, "TAP TO TURN ON", COLOR_WHITE, 0x2124, 1);
    }
}

bool app_torch_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    if (ty >= 80 && ty <= 160) {
        s_torch_on = !s_torch_on;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    return false;
}
