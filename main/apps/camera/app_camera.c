#include "app_camera.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_camera.h"
#include "jpeg_decoder.h"
#include "display.h"
#include "display_gfx.h"

#define SWAP_BYTES(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))

static const char *TAG = "app_camera";

static uint16_t *s_cam_framebuffer = NULL;
static int s_fps_counter = 0;
static int s_current_fps = 0;
static uint32_t s_fps_timer = 0;

void app_camera_init(void)
{
    if (!s_cam_framebuffer) {
        size_t fb_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
        s_cam_framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cam_framebuffer) {
            s_cam_framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_DEFAULT);
        }
        if (s_cam_framebuffer) {
            memset(s_cam_framebuffer, 0, fb_size);
            ESP_LOGI(TAG, "Allocated %u bytes viewfinder buffer in PSRAM", (unsigned int)fb_size);
        } else {
            ESP_LOGE(TAG, "Failed to allocate viewfinder buffer!");
        }
    }
}

// ─── Direct Pixel Drawing to Viewfinder Buffer ────────────
static inline void set_pixel_be(int x, int y, uint16_t color_be)
{
    if (!s_cam_framebuffer || x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) return;
    s_cam_framebuffer[y * LCD_H_RES + x] = color_be;
}

static void draw_hud_rect_be(int x, int y, int w, int h, uint16_t color_be)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            set_pixel_be(x + dx, y + dy, color_be);
        }
    }
}

static void draw_hud_char_be(int x, int y, char c, uint16_t color_be)
{
    extern const uint8_t font8x16[95][16];
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font8x16[c - 32];

    for (int row = 0; row < 16; row++) {
        uint8_t line = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (line & (0x80 >> col)) {
                set_pixel_be(x + col, y + row, color_be);
            }
        }
    }
}

static void draw_hud_string_be(int x, int y, const char *str, uint16_t color_be)
{
    while (str && *str) {
        draw_hud_char_be(x, y, *str, color_be);
        x += 8;
        str++;
    }
}

// ─── Camera Viewfinder Render Cycle ───────────────────────
void app_camera_update(void)
{
    if (!s_cam_framebuffer) {
        app_camera_init();
        if (!s_cam_framebuffer) return;
    }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // Calculate FPS
    s_fps_counter++;
    if (now - s_fps_timer >= 1000) {
        s_current_fps = s_fps_counter;
        s_fps_counter = 0;
        s_fps_timer = now;
    }

    // 1. Grab camera frame
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGW(TAG, "Camera capture failed");
        vTaskDelay(pdMS_TO_TICKS(10));
        return;
    }

    // 2. Decode JPEG directly to RGB565 buffer (matches ILI9341 big-endian byte order)
    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = fb->buf,
        .indata_size = (uint32_t)fb->len,
        .outbuf = (uint8_t *)s_cam_framebuffer,
        .outbuf_size = (uint32_t)(LCD_H_RES * LCD_V_RES * sizeof(uint16_t)),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 1 // Swap bytes directly during SIMD decoding for SPI LCD
        }
    };

    esp_jpeg_image_output_t outimg;
    esp_err_t decode_err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    esp_camera_fb_return(fb);

    if (decode_err == ESP_OK) {
        // 3. Render Futuristic Viewfinder HUD Overlays
        uint16_t hud_cyan = SWAP_BYTES(COLOR_CYAN_ACCENT);
        uint16_t hud_white = SWAP_BYTES(COLOR_WHITE);
        uint16_t hud_black = 0x0000;
        uint16_t hud_red = SWAP_BYTES(COLOR_RED);

        // Top-Left [BACK] Button Pill (x: 6..68, y: 6..28)
        draw_hud_rect_be(6, 6, 62, 22, hud_black);
        draw_hud_rect_be(8, 8, 58, 18, SWAP_BYTES(0x2124));
        draw_hud_string_be(14, 9, "< BACK", hud_white);

        // Top-Right Live Status & FPS pill
        char fps_str[32];
        snprintf(fps_str, sizeof(fps_str), "LIVE %dFPS", s_current_fps);
        draw_hud_rect_be(218, 6, 96, 22, hud_black);
        draw_hud_rect_be(220, 8, 92, 18, SWAP_BYTES(0x2124));
        draw_hud_rect_be(226, 14, 6, 6, hud_red); // Red live recording dot
        draw_hud_string_be(238, 9, fps_str, hud_cyan);

        // Center Viewfinder Targeting Brackets
        int cx = LCD_H_RES / 2;
        int cy = LCD_V_RES / 2;
        int brk_size = 18;
        int brk_span = 35;

        // Top-Left bracket
        draw_hud_rect_be(cx - brk_span, cy - brk_span, brk_size, 2, hud_cyan);
        draw_hud_rect_be(cx - brk_span, cy - brk_span, 2, brk_size, hud_cyan);
        // Top-Right bracket
        draw_hud_rect_be(cx + brk_span - brk_size, cy - brk_span, brk_size, 2, hud_cyan);
        draw_hud_rect_be(cx + brk_span, cy - brk_span, 2, brk_size, hud_cyan);
        // Bottom-Left bracket
        draw_hud_rect_be(cx - brk_span, cy + brk_span, brk_size, 2, hud_cyan);
        draw_hud_rect_be(cx - brk_span, cy + brk_span - brk_size, 2, brk_size, hud_cyan);
        // Bottom-Right bracket
        draw_hud_rect_be(cx + brk_span - brk_size, cy + brk_span, brk_size, 2, hud_cyan);
        draw_hud_rect_be(cx + brk_span, cy + brk_span - brk_size, 2, brk_size, hud_cyan);

        // Center cross dot
        draw_hud_rect_be(cx - 2, cy - 2, 4, 4, hud_white);

        // 4. Push full viewfinder frame to LCD via high-speed DMA
        display_draw_framebuffer(s_cam_framebuffer);
    } else {
        ESP_LOGW(TAG, "JPEG decode error: %d", decode_err);
    }
}

bool app_camera_handle_touch(int tx, int ty, AppState *next_state)
{
    // Tap [BACK] button area (Top-Left)
    if (tx <= 80 && ty <= 40) {
        if (next_state) *next_state = STATE_APP_MENU;
        return true;
    }
    return false;
}
