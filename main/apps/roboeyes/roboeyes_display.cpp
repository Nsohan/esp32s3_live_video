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

// ─── Display Canvas Adapter for ESP-IDF ILI9341 ───────────
class ESP_ILI9341_Display {
public:
    int width;
    int height;
    uint16_t *framebuffer;
    uint16_t main_color_be;
    uint16_t bg_color_be;

    ESP_ILI9341_Display(int w = LCD_H_RES, int h = LCD_V_RES)
        : width(w), height(h), framebuffer(nullptr)
    {
        main_color_be = SWAP_BYTES(COLOR_CYAN);
        bg_color_be   = 0x0000; // Black

        // Allocate 320x240 frame buffer (try internal memory or PSRAM)
        size_t fb_size = width * height * sizeof(uint16_t);
        framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!framebuffer) {
            framebuffer = (uint16_t *)heap_caps_malloc(fb_size, MALLOC_CAP_DEFAULT);
        }
        if (framebuffer) {
            memset(framebuffer, 0, fb_size);
            ESP_LOGI(TAG, "Allocated %u bytes framebuffer for RoboEyes", (unsigned int)fb_size);
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
        if (r <= 0) {
            fillRect(x, y, w, h, color);
            return;
        }

        uint16_t c_be = (color == 0) ? bg_color_be : main_color_be;

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

            int16_t line_x = x + x_offset;
            int16_t line_w = w - (2 * x_offset);
            if (line_w > 0) {
                drawFastHLine(line_x, cur_y, line_w, c_be);
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

    void display() {
        if (framebuffer) {
            display_draw_framebuffer(framebuffer);
        }
    }
};

// ─── Eye Mode Cycle State Machine ─────────────────────────
enum EyeMode {
    MODE_DEFAULT_IDLE = 0, // 0. Default friendly eyes with smooth blinking and idle gaze
    MODE_HAPPY,            // 1. Happy smiling eyes
    MODE_LAUGHING,         // 2. Laughing bouncing animation
    MODE_ANGRY,            // 3. Fierce focused angry eyes
    MODE_TIRED,            // 4. Sleepy heavy eyelids
    MODE_CONFUSED,         // 5. Shivering left-right confused animation
    MODE_SWEATING,         // 6. Anxious sweating forehead drops
    MODE_CURIOUS_LOOK,     // 7. Curious eyes scanning all 8 directions with dynamic scaling
    MODE_CYCLOPS,          // 8. Single big cyclops eye
    MODE_WINKING,          // 9. Alternating playful winks
    MODE_MAX_COUNT
};

static const char *MODE_NAMES[MODE_MAX_COUNT] = {
    "0/9: DEFAULT IDLE (Natural Blinking & Smooth Gaze)",
    "1/9: HAPPY MOOD (Smiling Curved Eyelids)",
    "2/9: LAUGHING ANIMATION (Joyful Up/Down Shaking)",
    "3/9: ANGRY MOOD (Fierce Angled Eyelids)",
    "4/9: TIRED MOOD (Sleepy Droopy Eyelids)",
    "5/9: CONFUSED ANIMATION (Rapid Side-to-Side Shivering)",
    "6/9: SWEATING (Anxious Dripping Sweat)",
    "7/9: CURIOUS GAZE (Looking Around 8 Compass Directions)",
    "8/9: CYCLOPS MODE (Single Centered Robot Eye)",
    "9/9: WINKING (Playful Left & Right Winks)"
};

static void reset_eye_defaults(RoboEyes<ESP_ILI9341_Display> &eyes) {
    eyes.setCyclops(false);
    eyes.setSweat(false);
    eyes.setCuriosity(false);
    eyes.setHFlicker(false);
    eyes.setVFlicker(false);
    eyes.setWidth(72, 72);
    eyes.setHeight(92, 92);
    eyes.setBorderradius(20, 20);
    eyes.setSpacebetween(30);
    eyes.setPosition(DEFAULT);
    eyes.setMood(DEFAULT);
    eyes.setAutoblinker(true, 2, 2);
    eyes.setIdleMode(true, 2, 2);
    eyes.open();
}

static void apply_mode(EyeMode mode, RoboEyes<ESP_ILI9341_Display> &eyes) {
    reset_eye_defaults(eyes);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, ">>> Displaying Eye Mode: %s", MODE_NAMES[mode]);
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
            break;

        case MODE_LAUGHING:
            eyes.setMood(HAPPY);
            eyes.anim_laugh();
            eyes.setAutoblinker(false);
            eyes.setIdleMode(false);
            break;

        case MODE_ANGRY:
            eyes.setMood(ANGRY);
            eyes.setAutoblinker(true, 3, 2);
            eyes.setIdleMode(false);
            eyes.setPosition(DEFAULT);
            break;

        case MODE_TIRED:
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
            eyes.setWidth(100, 100);
            eyes.setHeight(100, 100);
            eyes.setBorderradius(24, 24);
            eyes.setCyclops(true);
            eyes.setAutoblinker(true, 2, 2);
            eyes.setIdleMode(true, 2, 2);
            break;

        case MODE_WINKING:
            eyes.setMood(HAPPY);
            eyes.setIdleMode(false);
            eyes.setAutoblinker(false);
            break;

        default:
            break;
    }
}

static volatile bool s_roboeyes_active = false;
static volatile uint16_t s_eye_color_be = SWAP_BYTES(COLOR_CYAN);
static volatile int s_requested_mood = -1;

// ─── FreeRTOS Background Eye Animation Task ───────────────
static void roboeyes_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting RoboEyes Animation Engine (320x240)...");

    ESP_ILI9341_Display *display = new ESP_ILI9341_Display(LCD_H_RES, LCD_V_RES);
    if (!display->framebuffer) {
        ESP_LOGE(TAG, "Cannot start RoboEyes task: framebuffer allocation failed!");
        delete display;
        vTaskDelete(NULL);
        return;
    }

    RoboEyes<ESP_ILI9341_Display> eyes(*display);
    eyes.begin(LCD_H_RES, LCD_V_RES, 40); // 40 FPS target
    reset_eye_defaults(eyes);

    EyeMode current_mode = MODE_DEFAULT_IDLE;
    apply_mode(current_mode, eyes);

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

        display->main_color_be = s_eye_color_be;

        if (s_requested_mood >= 0 && s_requested_mood < MODE_MAX_COUNT) {
            current_mode = (EyeMode)s_requested_mood;
            s_requested_mood = -1;
            last_mode_switch_ms = now;
            apply_mode(current_mode, eyes);
        }

        // ─── Check 15-Second Mode Cycle ───────────────────
        if (now - last_mode_switch_ms >= MODE_INTERVAL_MS) {
            current_mode = (EyeMode)((current_mode + 1) % MODE_MAX_COUNT);
            last_mode_switch_ms = now;
            last_sub_action_ms = now;
            sub_step = 0;
            apply_mode(current_mode, eyes);
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
                if (now - last_sub_action_ms >= 1800) {
                    if (sub_step % 2 == 0) {
                        eyes.blink(true, false);
                    } else {
                        eyes.blink(false, true);
                    }
                    sub_step++;
                    last_sub_action_ms = now;
                }
                break;

            default:
                break;
        }

        // ─── Update and Render Eyes ───────────────────────
        eyes.update();

        // Yield to maintain ~40 FPS smoothly
        vTaskDelay(pdMS_TO_TICKS(25));
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
    ESP_LOGI(TAG, "RoboEyes background engine ready on Core 1!");
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
