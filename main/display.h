#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// ─── Display Pin Configuration (Match your wiring) ───────
#define LCD_HOST        SPI2_HOST

#define LCD_PIN_SCK     40
#define LCD_PIN_MOSI    41
#define LCD_PIN_CS      42
#define LCD_PIN_DC      2
#define LCD_PIN_RST     1
#define LCD_PIN_BCKL    -1   // -1 if connected directly to 3.3V

#define LCD_H_RES       320
#define LCD_V_RES       240

// ─── Colors (RGB565) ──────────────────────────────────────
#define COLOR_BLACK     0x0000
#define COLOR_NAVY      0x000F
#define COLOR_DARKGREEN 0x03E0
#define COLOR_DARKCYAN  0x03EF
#define COLOR_MAROON    0x7800
#define COLOR_PURPLE    0x780F
#define COLOR_OLIVE     0x7BE0
#define COLOR_LIGHTGREY 0xC618
#define COLOR_DARKGREY  0x7BEF
#define COLOR_BLUE      0x001F
#define COLOR_GREEN     0x07E0
#define COLOR_CYAN      0x07FF
#define COLOR_RED       0xF800
#define COLOR_MAGENTA   0xF81F
#define COLOR_YELLOW    0xFFE0
#define COLOR_WHITE     0xFFFF
#define COLOR_ORANGE    0xFD20

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);
void display_fill_screen(uint16_t color);
void display_draw_test_pattern(void);
void display_draw_petbot_face(void);
void display_draw_framebuffer(const uint16_t *buffer);

#ifdef __cplusplus
}
#endif

