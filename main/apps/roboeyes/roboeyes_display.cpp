#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"

#include "display.h"
#include "roboeyes_display.h"

static const char *TAG = "roboeyes";

// Byte swap for ILI9341 SPI (RGB565)
#define SWAP_BYTES(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))

// Compatibility definitions for Arduino-based FluxGarage RoboEyes
typedef uint8_t byte;

static inline uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline long random(long max_val) {
    if (max_val <= 0) return 0;
    return (long)(esp_random() % (uint32_t)max_val);
}

static inline long random(long min_val, long max_val) {
    if (min_val >= max_val) return min_val;
    return min_val + (long)(esp_random() % (uint32_t)(max_val - min_val));
}

// Include RoboEyes template class from components/RoboEyes/src
#include "FluxGarage_RoboEyes.h"

// ─── Loona Color Themes ────────────────────────────────────
enum LoonaTheme {
    THEME_AMBER_GOLD = 0, // Loona signature warm golden amber with white-hot core
    THEME_FIERY_RED  = 1, // Angry fierce red-orange glow
    THEME_TIRED_BLUE = 2, // Sleepy cool teal/blue glow
    THEME_CUSTOM     = 3  // User-defined color
};

// ─── Loona Radial Glow Precomputed LUT Table ─────────────
static uint16_t s_glow_lut[4][256];
static bool s_lut_initialized = false;

static void init_glow_lut(void) {
    if (s_lut_initialized) return;
    for (int theme = 0; theme < 4; theme++) {
        for (int i = 0; i < 256; i++) {
            float dist = (float)i / 200.0f;
            uint8_t r = 255, g = 255, b = 255;

            if (theme == THEME_AMBER_GOLD) {
                if (dist <= 0.35f) {
                    float t = dist / 0.35f;
                    r = 255;
                    g = (uint8_t)(255 - 20 * t);
                    b = (uint8_t)(245 - 165 * t);
                } else if (dist <= 0.80f) {
                    float t = (dist - 0.35f) / 0.45f;
                    r = 255;
                    g = (uint8_t)(235 - 105 * t);
                    b = (uint8_t)(80 - 80 * t);
                } else {
                    float t = (dist - 0.80f) / 0.40f;
                    if (t > 1.0f) t = 1.0f;
                    r = (uint8_t)(255 - 45 * t);
                    g = (uint8_t)(130 - 70 * t);
                    b = 0;
                }
            } else if (theme == THEME_FIERY_RED) {
                if (dist <= 0.35f) {
                    float t = dist / 0.35f;
                    r = 255;
                    g = (uint8_t)(245 - 75 * t);
                    b = (uint8_t)(200 - 180 * t);
                } else if (dist <= 0.80f) {
                    float t = (dist - 0.35f) / 0.45f;
                    r = 255;
                    g = (uint8_t)(170 - 120 * t);
                    b = (uint8_t)(20 - 20 * t);
                } else {
                    float t = (dist - 0.80f) / 0.40f;
                    if (t > 1.0f) t = 1.0f;
                    r = (uint8_t)(255 - 65 * t);
                    g = (uint8_t)(50 - 45 * t);
                    b = 0;
                }
            } else {
                if (dist <= 0.40f) {
                    float t = dist / 0.40f;
                    r = (uint8_t)(220 - 120 * t);
                    g = 255;
                    b = 255;
                } else {
                    float t = (dist - 0.40f) / 0.60f;
                    if (t > 1.0f) t = 1.0f;
                    r = (uint8_t)(100 - 100 * t);
                    g = (uint8_t)(255 - 100 * t);
                    b = 255;
                }
            }
            uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            s_glow_lut[theme][i] = SWAP_BYTES(rgb565);
        }
    }
    s_lut_initialized = true;
}

// ─── Display Canvas Adapter with Loona Shader Engine ───────
class ESP_ILI9341_Display {
public:
    int width;
    int height;
    uint16_t *framebuffer;
    uint16_t main_color_be;
    uint16_t bg_color_be;
    LoonaTheme active_theme;
    bool enable_blush;
    bool is_winking;

    // Direct reference to RoboEyes state for atomic single-pass rendering
    RoboEyes<ESP_ILI9341_Display> *eyes_ref;

    ESP_ILI9341_Display(int w = LCD_H_RES, int h = LCD_V_RES)
        : width(w), height(h), framebuffer(nullptr),
          active_theme(THEME_AMBER_GOLD), enable_blush(false), is_winking(false),
          eyes_ref(nullptr)
    {
        init_glow_lut();
        main_color_be = SWAP_BYTES(COLOR_YELLOW);
        bg_color_be   = 0x0000; // Black

        size_t fb_size = width * height * sizeof(uint16_t);
        framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!framebuffer) {
            framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_DEFAULT);
        }
        if (framebuffer) {
            memset(framebuffer, 0, fb_size);
            ESP_LOGI(TAG, "Allocated %u bytes framebuffer for Loona RoboEyes", (unsigned int)fb_size);
        } else {
            ESP_LOGE(TAG, "Failed to allocate framebuffer for RoboEyes!");
        }
    }

    ~ESP_ILI9341_Display() {
        if (framebuffer) {
            free(framebuffer);
            framebuffer = nullptr;
        }
    }

    void clearDisplay() {
        if (framebuffer) {
            memset(framebuffer, 0, width * height * sizeof(uint16_t));
        }
    }

    inline void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color_be) {
        if (!framebuffer || y < 0 || y >= height || w <= 0) return;
        int16_t x0 = (x < 0) ? 0 : x;
        int16_t x1 = x + w - 1;
        if (x1 >= width) x1 = width - 1;
        if (x0 > x1) return;

        uint16_t *row = &framebuffer[y * width];
        for (int16_t i = x0; i <= x1; i++) {
            row[i] = color_be;
        }
    }

    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
        if (!framebuffer || w <= 0 || h <= 0) return;
        uint16_t c_be = (color == 0) ? bg_color_be : main_color_be;
        for (int16_t cur_y = y; cur_y < y + h; cur_y++) {
            drawFastHLine(x, cur_y, w, c_be);
        }
    }

    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {
        if (!framebuffer || w <= 0 || h <= 0) return;
        if (r > w / 2) r = w / 2;
        if (r > h / 2) r = h / 2;

        if (color == 0) {
            // Background cutout (Black)
            for (int16_t dy = 0; dy < h; dy++) {
                int16_t cur_y = y + dy;
                if (cur_y < 0 || cur_y >= height) continue;

                int16_t x_offset = 0;
                if (r > 0) {
                    if (dy < r) {
                        int16_t d = r - 1 - dy;
                        int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                        x_offset = r - w_arc;
                    } else if (dy >= h - r) {
                        int16_t d = dy - (h - r);
                        int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                        x_offset = r - w_arc;
                    }
                }

                int16_t line_x = x + x_offset;
                int16_t line_w = w - (2 * x_offset);
                if (line_w > 0) {
                    drawFastHLine(line_x, cur_y, line_w, bg_color_be);
                }
            }
            return;
        }

        // Render Eye with Loona Radial Glow
        if (r <= 0) r = 1;
        int16_t cx = x + w / 2;
        int16_t cy = y + h / 2;
        int16_t rx = w / 2;
        int16_t ry = h / 2;
        if (rx <= 0) rx = 1;
        if (ry <= 0) ry = 1;

        for (int16_t dy = 0; dy < h; dy++) {
            int16_t cur_y = y + dy;
            if (cur_y < 0 || cur_y >= height) continue;

            int16_t x_offset = 0;
            if (dy < r) {
                int16_t d = r - 1 - dy;
                int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                x_offset = r - w_arc;
            } else if (dy >= h - r) {
                int16_t d = dy - (h - r);
                int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                x_offset = r - w_arc;
            }

            int16_t line_x0 = x + x_offset;
            int16_t line_x1 = x + w - 1 - x_offset;
            if (line_x0 < 0) line_x0 = 0;
            if (line_x1 >= width) line_x1 = width - 1;
            if (line_x0 > line_x1) continue;

            int32_t dy_diff = cur_y - cy;
            int32_t ny_num = (dy_diff * 200) / ry;
            int32_t ny2 = ny_num * ny_num;

            uint16_t *row = &framebuffer[cur_y * width];
            for (int16_t cur_x = line_x0; cur_x <= line_x1; cur_x++) {
                int32_t dx_diff = cur_x - cx;
                int32_t nx_num = (dx_diff * 200) / rx;
                int32_t dist_idx = (int32_t)sqrtf((float)(nx_num * nx_num + ny2));
                if (dist_idx > 255) dist_idx = 255;
                row[cur_x] = s_glow_lut[active_theme][dist_idx];
            }
        }
    }

    void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {
        if (!framebuffer) return;
        uint16_t c_be = (color == 0) ? bg_color_be : main_color_be;

        if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }
        if (y1 > y2) { std::swap(y1, y2); std::swap(x1, x2); }
        if (y0 > y1) { std::swap(y0, y1); std::swap(x0, x1); }

        if (y0 == y2) {
            int16_t min_x = std::min({x0, x1, x2});
            int16_t max_x = std::max({x0, x1, x2});
            drawFastHLine(min_x, y0, max_x - min_x + 1, c_be);
            return;
        }

        int16_t dx01 = x1 - x0, dy01 = y1 - y0;
        int16_t dx02 = x2 - x0, dy02 = y2 - y0;
        int16_t dx12 = x2 - x1, dy12 = y2 - y1;

        int16_t last = (y1 == y2) ? y1 : y1 - 1;
        for (int16_t y = y0; y <= last; y++) {
            int16_t a = x0 + (dy01 > 0 ? (int32_t)dx01 * (y - y0) / dy01 : 0);
            int16_t b = x0 + (dy02 > 0 ? (int32_t)dx02 * (y - y0) / dy02 : 0);
            if (a > b) std::swap(a, b);
            drawFastHLine(a, y, b - a + 1, c_be);
        }

        for (int16_t y = y1; y <= y2; y++) {
            int16_t a = x1 + (dy12 > 0 ? (int32_t)dx12 * (y - y1) / dy12 : 0);
            int16_t b = x0 + (dy02 > 0 ? (int32_t)dx02 * (y - y0) / dy02 : 0);
            if (a > b) std::swap(a, b);
            drawFastHLine(a, y, b - a + 1, c_be);
        }
    }

    // ─── Specular Catchlight Glint Dot ────────────────────
    void drawGlint(int16_t cx, int16_t cy, int16_t radius) {
        if (!framebuffer || radius <= 0) return;
        if (cx < 0 || cx >= width || cy < 0 || cy >= height) return;

        uint16_t white_be = SWAP_BYTES(0xFFFF);
        for (int16_t dy = -radius; dy <= radius; dy++) {
            int16_t py = cy + dy;
            if (py < 0 || py >= height) continue;
            int16_t w_arc = (int16_t)sqrtf((float)(radius * radius - dy * dy));
            for (int16_t dx = -w_arc; dx <= w_arc; dx++) {
                int16_t px = cx + dx;
                if (px >= 0 && px < width) {
                    if (framebuffer[py * width + px] != bg_color_be) {
                        framebuffer[py * width + px] = white_be;
                    }
                }
            }
        }
    }

    // ─── Glowing Red Blush Cheeks (Loona Happy Mode) ──────
    void drawBlushPill(int16_t bx, int16_t by, int16_t bw, int16_t bh) {
        if (!framebuffer || bw <= 0 || bh <= 0) return;
        int16_t r = bh / 2;
        int16_t cx = bx + bw / 2;
        int16_t cy = by + bh / 2;

        for (int16_t dy = 0; dy < bh; dy++) {
            int16_t py = by + dy;
            if (py < 0 || py >= height) continue;

            int16_t x_off = 0;
            if (dy < r) {
                int16_t d = r - 1 - dy;
                int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                x_off = r - w_arc;
            } else if (dy >= bh - r) {
                int16_t d = dy - (bh - r);
                int16_t w_arc = (int16_t)sqrtf((float)(r * r - d * d));
                x_off = r - w_arc;
            }

            int16_t x0 = bx + x_off;
            int16_t x1 = bx + bw - 1 - x_off;
            if (x0 < 0) x0 = 0;
            if (x1 >= width) x1 = width - 1;

            float ny = (float)(py - cy) / ((float)bh / 2.0f);
            for (int16_t px = x0; px <= x1; px++) {
                float nx = (float)(px - cx) / ((float)bw / 2.0f);
                float dist = sqrtf(nx * nx + ny * ny);
                if (dist > 1.0f) dist = 1.0f;

                // Glowing vivid red-coral blush
                uint8_t cr = (uint8_t)(255 - 40 * dist);
                uint8_t cg = (uint8_t)(45 * (1.0f - dist));
                uint8_t cb = (uint8_t)(60 * (1.0f - dist));
                uint16_t blush_c = SWAP_BYTES(((cr & 0xF8) << 8) | ((cg & 0xFC) << 3) | (cb >> 3));
                framebuffer[py * width + px] = blush_c;
            }
        }
    }

    // ─── Loona Curved Wink Arch (Image 1 Expression) ──────
    void drawWinkArch(int16_t x, int16_t y, int16_t w, int16_t h) {
        if (!framebuffer || w <= 0 || h <= 0) return;
        int16_t cx = x + w / 2;
        int16_t cy = y + h / 2;
        int16_t bar_thick = 16;
        int16_t arc_depth = 10;

        for (int16_t dx = -w / 2; dx <= w / 2; dx++) {
            int16_t px = cx + dx;
            if (px < 0 || px >= width) continue;

            float nx = (float)dx / ((float)w / 2.0f);
            if (nx < -1.0f || nx > 1.0f) continue;

            float curve_offset = (1.0f - nx * nx) * (float)arc_depth;
            int16_t mid_y = cy - (int16_t)curve_offset;

            for (int16_t ty = -bar_thick / 2; ty <= bar_thick / 2; ty++) {
                int16_t py = mid_y + ty;
                if (py >= 0 && py < height) {
                    float dist = fabsf(nx);
                    int32_t dist_idx = (int32_t)(dist * 200.0f);
                    if (dist_idx > 255) dist_idx = 255;
                    framebuffer[py * width + px] = s_glow_lut[active_theme][dist_idx];
                }
            }
        }
    }

    void display() {
        if (!framebuffer) return;

        // Apply Loona overlays before pushing single atomic frame to display
        if (eyes_ref) {
            // 1. Winking Arch Overlay
            if (is_winking) {
                drawWinkArch(eyes_ref->eyeLx, eyes_ref->eyeLy + 24, eyes_ref->eyeLwidthCurrent, 30);
            }

            // 2. Specular Catchlight Highlights (Glint) on open eyes
            if (eyes_ref->eyeLheightCurrent >= 28 && !is_winking) {
                int16_t glint_lx = eyes_ref->eyeLx + (int16_t)(eyes_ref->eyeLwidthCurrent * 0.72f);
                int16_t glint_ly = eyes_ref->eyeLy + (int16_t)(eyes_ref->eyeLheightCurrent * 0.26f);
                drawGlint(glint_lx, glint_ly, 4);
            }

            if (eyes_ref->eyeRheightCurrent >= 28 && !eyes_ref->cyclops) {
                int16_t glint_rx = eyes_ref->eyeRx + (int16_t)(eyes_ref->eyeRwidthCurrent * 0.72f);
                int16_t glint_ry = eyes_ref->eyeRy + (int16_t)(eyes_ref->eyeRheightCurrent * 0.26f);
                drawGlint(glint_rx, glint_ry, 4);
            }

            // 3. Red Blush Cheeks (Under smiling/laughing eyes)
            if (enable_blush && !eyes_ref->cyclops) {
                int16_t blush_y = eyes_ref->eyeLy + eyes_ref->eyeLheightCurrent - 10;
                drawBlushPill(eyes_ref->eyeLx - 8, blush_y, 36, 14);
                drawBlushPill(eyes_ref->eyeRx + eyes_ref->eyeRwidthCurrent - 28, blush_y, 36, 14);
            }
        }

        // Single push to LCD over SPI DMA
        display_draw_framebuffer(framebuffer);
    }
};

// ─── Eye Mode Cycle State Machine ─────────────────────────
enum EyeMode {
    MODE_DEFAULT_IDLE = 0, // 0. Default Loona glowing eyes with blinking & smooth gaze
    MODE_HAPPY,            // 1. Happy smiling eyes with red glowing blush cheeks
    MODE_LAUGHING,         // 2. Laughing bouncing animation with blush
    MODE_ANGRY,            // 3. Fierce fiery focused angry eyes
    MODE_TIRED,            // 4. Sleepy heavy eyelids
    MODE_CONFUSED,         // 5. Shivering left-right confused animation
    MODE_SWEATING,         // 6. Anxious sweating forehead drops
    MODE_CURIOUS_LOOK,     // 7. Curious eyes scanning all 8 directions with dynamic scaling
    MODE_CYCLOPS,          // 8. Single big cyclops eye
    MODE_WINKING,          // 9. Signature Loona playful wink (arch + glowing eye)
    MODE_MAX_COUNT
};

static const char *MODE_NAMES[MODE_MAX_COUNT] = {
    "0/9: DEFAULT IDLE (Loona Amber Glow & Catchlight)",
    "1/9: HAPPY MOOD (Smiling Eyes & Red Blush Cheeks)",
    "2/9: LAUGHING ANIMATION (Joyful Up/Down Shaking + Blush)",
    "3/9: ANGRY MOOD (Fiery Slanted Eyelids)",
    "4/9: TIRED MOOD (Sleepy Droopy Eyelids)",
    "5/9: CONFUSED ANIMATION (Rapid Side-to-Side Shivering)",
    "6/9: SWEATING (Anxious Dripping Sweat)",
    "7/9: CURIOUS GAZE (Looking Around 8 Directions)",
    "8/9: CYCLOPS MODE (Single Centered Robot Eye)",
    "9/9: WINKING (Loona Arched Wink & Specular Glint)"
};

static void reset_eye_defaults(RoboEyes<ESP_ILI9341_Display> &eyes, ESP_ILI9341_Display &display) {
    eyes.setCyclops(false);
    eyes.setSweat(false);
    eyes.setCuriosity(false);
    eyes.setHFlicker(false);
    eyes.setVFlicker(false);

    // Loona-proportioned soft squircles
    eyes.setWidth(74, 74);
    eyes.setHeight(104, 104);
    eyes.setBorderradius(28, 28);
    eyes.setSpacebetween(30);

    eyes.setPosition(DEFAULT);
    eyes.setMood(DEFAULT);
    eyes.setAutoblinker(true, 2, 2);
    eyes.setIdleMode(true, 2, 2);
    eyes.open();

    display.active_theme = THEME_AMBER_GOLD;
    display.enable_blush = false;
    display.is_winking = false;
}

static void apply_mode(EyeMode mode, RoboEyes<ESP_ILI9341_Display> &eyes, ESP_ILI9341_Display &display) {
    reset_eye_defaults(eyes, display);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, ">>> Displaying Loona Eye Mode: %s", MODE_NAMES[mode]);
    ESP_LOGI(TAG, "========================================");

    switch (mode) {
        case MODE_DEFAULT_IDLE:
            eyes.setMood(DEFAULT);
            eyes.setAutoblinker(true, 2, 2);
            eyes.setIdleMode(true, 2, 2);
            break;

        case MODE_HAPPY:
            eyes.setMood(HAPPY);
            eyes.setAutoblinker(true, 2, 3);
            eyes.setIdleMode(true, 3, 2);
            display.enable_blush = true;
            break;

        case MODE_LAUGHING:
            eyes.setMood(HAPPY);
            eyes.anim_laugh();
            eyes.setAutoblinker(false);
            eyes.setIdleMode(false);
            display.enable_blush = true;
            break;

        case MODE_ANGRY:
            display.active_theme = THEME_FIERY_RED;
            eyes.setMood(ANGRY);
            eyes.setAutoblinker(true, 3, 2);
            eyes.setIdleMode(false);
            eyes.setPosition(DEFAULT);
            break;

        case MODE_TIRED:
            display.active_theme = THEME_TIRED_BLUE;
            eyes.setMood(TIRED);
            eyes.setAutoblinker(true, 4, 3);
            eyes.setIdleMode(true, 4, 3);
            break;

        case MODE_CONFUSED:
            eyes.setMood(DEFAULT);
            eyes.anim_confused();
            eyes.setAutoblinker(false);
            eyes.setIdleMode(false);
            break;

        case MODE_SWEATING:
            eyes.setMood(DEFAULT);
            eyes.setSweat(true);
            eyes.setAutoblinker(true, 2, 2);
            eyes.setIdleMode(true, 2, 2);
            break;

        case MODE_CURIOUS_LOOK:
            eyes.setMood(DEFAULT);
            eyes.setCuriosity(true);
            eyes.setIdleMode(false);
            eyes.setAutoblinker(true, 3, 2);
            break;

        case MODE_CYCLOPS:
            eyes.setWidth(108, 108);
            eyes.setHeight(108, 108);
            eyes.setBorderradius(32, 32);
            eyes.setCyclops(true);
            eyes.setAutoblinker(true, 2, 2);
            eyes.setIdleMode(true, 2, 2);
            break;

        case MODE_WINKING:
            display.is_winking = true;
            eyes.setMood(DEFAULT);
            eyes.setIdleMode(false);
            eyes.setAutoblinker(false);
            break;

        default:
            break;
    }
}

static volatile bool s_roboeyes_active = false;
static volatile uint16_t s_eye_color_be = SWAP_BYTES(COLOR_YELLOW);
static volatile int s_requested_mood = -1;

// ─── FreeRTOS Background Eye Animation Task ───────────────
static void roboeyes_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting Loona-Style RoboEyes Engine (320x240)...");

    ESP_ILI9341_Display *display = new ESP_ILI9341_Display(LCD_H_RES, LCD_V_RES);
    if (!display->framebuffer) {
        ESP_LOGE(TAG, "Cannot start RoboEyes task: framebuffer allocation failed!");
        delete display;
        vTaskDelete(NULL);
        return;
    }

    RoboEyes<ESP_ILI9341_Display> eyes(*display);
    display->eyes_ref = &eyes; // Link eye coordinates for atomic display rendering

    eyes.begin(LCD_H_RES, LCD_V_RES, 40); // 40 FPS target
    reset_eye_defaults(eyes, *display);

    EyeMode current_mode = MODE_DEFAULT_IDLE;
    apply_mode(current_mode, eyes, *display);

    uint32_t last_mode_switch_ms = millis();
    uint32_t last_sub_action_ms = millis();
    int sub_step = 0;

    const uint32_t MODE_INTERVAL_MS = 15000; // 15 seconds per eye mode

    while (1) {
        if (!s_roboeyes_active) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint32_t now = millis();

        if (s_requested_mood >= 0 && s_requested_mood < MODE_MAX_COUNT) {
            current_mode = (EyeMode)s_requested_mood;
            s_requested_mood = -1;
            last_mode_switch_ms = now;
            apply_mode(current_mode, eyes, *display);
        }

        // ─── Check 15-Second Mode Cycle ───────────────────
        if (now - last_mode_switch_ms >= MODE_INTERVAL_MS) {
            current_mode = (EyeMode)((current_mode + 1) % MODE_MAX_COUNT);
            last_mode_switch_ms = now;
            last_sub_action_ms = now;
            sub_step = 0;
            apply_mode(current_mode, eyes, *display);
        }

        // ─── In-Mode Periodic Sub-Actions ─────────────────
        switch (current_mode) {
            case MODE_LAUGHING:
                if (now - last_sub_action_ms >= 1800) {
                    eyes.anim_laugh();
                    last_sub_action_ms = now;
                }
                break;

            case MODE_CONFUSED:
                if (now - last_sub_action_ms >= 1500) {
                    eyes.anim_confused();
                    last_sub_action_ms = now;
                }
                break;

            case MODE_CURIOUS_LOOK:
                if (now - last_sub_action_ms >= 1500) {
                    static const unsigned char positions[] = {N, NE, E, SE, S, SW, W, NW, DEFAULT};
                    sub_step = (sub_step + 1) % (sizeof(positions) / sizeof(positions[0]));
                    eyes.setPosition(positions[sub_step]);
                    last_sub_action_ms = now;
                }
                break;

            case MODE_WINKING:
                // Loona Signature Wink: Left eye stays curved wink bar, right eye blinks playfully
                if (now - last_sub_action_ms >= 2200) {
                    eyes.blink(false, true); // Blink right eye playfully
                    last_sub_action_ms = now;
                }
                break;

            default:
                break;
        }

        // ─── Update Eyes (RoboEyes library updates physics and triggers display() once with all overlays)
        eyes.update();

        // Small yield to let FreeRTOS tasks run smoothly
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

extern "C" void roboeyes_start_cycling_task(void) {
    xTaskCreatePinnedToCore(
        roboeyes_task,
        "roboeyes_task",
        8192,
        NULL,
        5,
        NULL,
        1
    );
    ESP_LOGI(TAG, "Loona-Style RoboEyes background engine ready on Core 1!");
}

extern "C" void roboeyes_set_active(bool active) {
    s_roboeyes_active = active;
}

extern "C" bool roboeyes_is_active(void) {
    return s_roboeyes_active;
}

extern "C" void roboeyes_set_color(uint16_t color_rgb565) {
    s_eye_color_be = SWAP_BYTES(color_rgb565);
}

extern "C" void roboeyes_trigger_mood(int mood_index) {
    s_requested_mood = mood_index;
    s_roboeyes_active = true;
}
