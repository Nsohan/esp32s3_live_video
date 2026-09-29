#include "app_moods.h"
#include "display.h"
#include "display_gfx.h"
#include "roboeyes_display.h"

void app_moods_draw(void)
{
    display_fill_screen(0x0841);
    app_common_draw_header("EXPRESS EMOTIONS");

    const struct { const char *label; int mood_idx; uint16_t color; int x; int y; } moods[] = {
        {"HAPPY",    ROBOEYES_MODE_HAPPY,           COLOR_GREEN,         14,  36},
        {"LAUGH",    ROBOEYES_MODE_LAUGHING,        COLOR_YELLOW,        166, 36},
        {"ANGRY",    ROBOEYES_MODE_ANGRY,           COLOR_RED,           14,  84},
        {"TIRED",    ROBOEYES_MODE_TIRED,           0x7BEF,              166, 84},
        {"CONFUSED", ROBOEYES_MODE_CONFUSED,        COLOR_MAGENTA,       14,  132},
        {"SWEAT",    ROBOEYES_MODE_SWEATING,        COLOR_CYAN_ACCENT,   166, 132},
        {"MUSIC",    ROBOEYES_MODE_MUSIC_LISTENING, COLOR_ORANGE_ACCENT, 14,  180},
        {"WINKING",  ROBOEYES_MODE_WINKING,         COLOR_CYAN,          166, 180}
    };
    for (int i = 0; i < 8; i++) {
        gfx_fill_round_rect(moods[i].x, moods[i].y, 140, 42, 7, moods[i].color);
        gfx_draw_string_centered(moods[i].x, moods[i].y + 15, 140, moods[i].label, COLOR_BLACK, moods[i].color, 1);
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
        {ROBOEYES_MODE_HAPPY,           14,  36,  154, 78},
        {ROBOEYES_MODE_LAUGHING,        166, 36,  306, 78},
        {ROBOEYES_MODE_ANGRY,           14,  84,  154, 126},
        {ROBOEYES_MODE_TIRED,           166, 84,  306, 126},
        {ROBOEYES_MODE_CONFUSED,        14,  132, 154, 174},
        {ROBOEYES_MODE_SWEATING,        166, 132, 306, 174},
        {ROBOEYES_MODE_MUSIC_LISTENING, 14,  180, 154, 222},
        {ROBOEYES_MODE_WINKING,         166, 180, 306, 222}
    };

    for (int i = 0; i < 8; i++) {
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

