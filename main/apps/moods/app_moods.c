#include "app_moods.h"
#include "display.h"
#include "display_gfx.h"
#include "roboeyes_display.h"

void app_moods_draw(void)
{
    display_fill_screen(0x0841);
    app_common_draw_header("EXPRESS EMOTIONS");

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

bool app_moods_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    if (app_common_is_back_pressed(tx, ty)) {
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

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
            if (next_state) *next_state = STATE_ROBOEYES_VIEW;
            if (needs_redraw) *needs_redraw = true;
            return true;
        }
    }

    return false;
}
