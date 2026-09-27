#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "display.h"

#ifdef __cplusplus
extern "C" {
#endif

// Custom Extended RGB565 Colors
#define COLOR_BG_DARK       0x0841  // Very dark slate grey #101018
#define COLOR_CARD_BG       0x18E3  // Dark card background #181c24
#define COLOR_CARD_BORDER   0x31A6  // Border highlight
#define COLOR_CYAN_ACCENT   0x07FF  // Neon cyan
#define COLOR_ORANGE_ACCENT 0xFD20  // Orange
#define COLOR_PURPLE_ACCENT 0x999F  // Violet purple
#define COLOR_GREEN_ACCENT  0x2664  // Emerald green
#define COLOR_RED_ACCENT    0xF986  // Crimson red
#define COLOR_YELLOW_ACCENT 0xFFE0  // Gold yellow
#define COLOR_BLUE_ACCENT   0x243F  // Royal blue
#define COLOR_TEXT_DIM      0x8C71  // Dim gray text

typedef enum {
    ICON_CAMERA,
    ICON_ROBOEYES,
    ICON_SETTINGS,
    ICON_SYSINFO,
    ICON_WEB_STREAM,
    ICON_TORCH,
    ICON_PET_MOODS,
    ICON_GALLERY,
    ICON_MUSIC,
    ICON_BACK,
    ICON_WIFI
} IconType;

void gfx_fill_rect(int x, int y, int w, int h, uint16_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint16_t color);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color);
void gfx_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color);

void gfx_draw_char(int x, int y, char c, uint16_t color, uint16_t bg, int scale);
void gfx_draw_string(int x, int y, const char *str, uint16_t color, uint16_t bg, int scale);
void gfx_draw_string_centered(int x, int y, int w, const char *str, uint16_t color, uint16_t bg, int scale);

void gfx_draw_icon(int center_x, int center_y, IconType icon, uint16_t color);
void gfx_draw_app_tile(int x, int y, int size, IconType icon, const char *label, uint16_t bg_color, bool selected);

extern const uint8_t font8x16[95][16];

#ifdef __cplusplus
}
#endif
