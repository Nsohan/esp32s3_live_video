#include "display.h"
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_ili9341_init_cmds_1.h"

#define SWAP_BYTES(x) ((uint16_t)((((uint16_t)(x) & 0xFF) << 8) | (((uint16_t)(x) >> 8) & 0xFF)))

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
#define DISPLAY_CHUNK_LINES 40

#define DISPLAY_CHUNK_PIXELS (LCD_H_RES * DISPLAY_CHUNK_LINES)
#define DISPLAY_NUM_CHUNKS   (LCD_V_RES / DISPLAY_CHUNK_LINES)

static SemaphoreHandle_t s_display_mutex = NULL;
static SemaphoreHandle_t s_trans_done_sem = NULL;
static uint16_t *s_chunk_buffers[DISPLAY_NUM_CHUNKS] = {NULL};

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    if (s_trans_done_sem) {
        xSemaphoreGiveFromISR(s_trans_done_sem, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}

esp_err_t display_init(void)
{
    if (!s_display_mutex) {
        s_display_mutex = xSemaphoreCreateMutex();
    }
    if (!s_trans_done_sem) {
        s_trans_done_sem = xSemaphoreCreateBinary();
    }

    ESP_LOGI(TAG, "Initializing SPI bus for display...");

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = LCD_PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_CHUNK_PIXELS * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Attaching LCD to SPI bus...");
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 20 * 1000 * 1000, // 20 MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    esp_lcd_panel_io_register_event_callbacks(s_io_handle, &cbs, NULL);

    ESP_LOGI(TAG, "Creating LCD panel driver (ILI9341_2_DRIVER)...");
    static const ili9341_vendor_config_t vendor_config = {
        .init_cmds = ili9341_lcd_init_vendor,
        .init_cmds_size = sizeof(ili9341_lcd_init_vendor) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };

    ret = esp_lcd_new_panel_ili9341(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ILI9341 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Resetting and initializing panel...");
    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);

    // Set Landscape Mode (320 wide x 240 tall)
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, false, false);
    esp_lcd_panel_invert_color(s_panel_handle, true);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // Allocate dedicated DMA buffers for all 6 chunks
    for (int i = 0; i < DISPLAY_NUM_CHUNKS; i++) {
        if (!s_chunk_buffers[i]) {
            s_chunk_buffers[i] = (uint16_t *)heap_caps_malloc(DISPLAY_CHUNK_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
            if (!s_chunk_buffers[i]) {
                s_chunk_buffers[i] = (uint16_t *)heap_caps_malloc(DISPLAY_CHUNK_PIXELS * sizeof(uint16_t), MALLOC_CAP_DEFAULT);
            }
        }
    }

    ESP_LOGI(TAG, "Display initialized successfully (ILI9341 320x240 Landscape)!");
    return ESP_OK;
}

void display_fill_screen(uint16_t color)
{
    if (!s_panel_handle) return;
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);

    uint16_t be_color = SWAP_BYTES(color);

    for (int i = 0; i < DISPLAY_NUM_CHUNKS; i++) {
        if (s_chunk_buffers[i]) {
            for (size_t p = 0; p < DISPLAY_CHUNK_PIXELS; p++) {
                s_chunk_buffers[i][p] = be_color;
            }
            int y = i * DISPLAY_CHUNK_LINES;
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LCD_H_RES, y + DISPLAY_CHUNK_LINES, s_chunk_buffers[i]);
            if (s_trans_done_sem) {
                xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(100));
            }
        }
    }

    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}

void display_draw_test_pattern(void)
{
    if (!s_panel_handle) return;

    ESP_LOGI(TAG, "Drawing color bar test pattern...");
    const uint16_t colors[] = {
        COLOR_RED,
        COLOR_GREEN,
        COLOR_BLUE,
        COLOR_YELLOW,
        COLOR_CYAN,
        COLOR_MAGENTA,
        COLOR_WHITE,
        COLOR_BLACK
    };
    const int bar_count = sizeof(colors) / sizeof(colors[0]);
    const int bar_height = LCD_V_RES / bar_count; // 30 pixels each

    for (int b = 0; b < bar_count; b++) {
        uint16_t be_color = SWAP_BYTES(colors[b]);
        size_t pixels = LCD_H_RES * bar_height;
        uint16_t *buf = (uint16_t *)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!buf) continue;

        for (size_t i = 0; i < pixels; i++) {
            buf[i] = be_color;
        }
        int y_start = b * bar_height;
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y_start, LCD_H_RES, y_start + bar_height, buf);
        vTaskDelay(pdMS_TO_TICKS(10));
        free(buf);
    }
}

void display_draw_petbot_face(void)
{
    if (!s_panel_handle) return;

    ESP_LOGI(TAG, "Drawing PetBot face (Full 320x240 screen)...");
    display_fill_screen(COLOR_BLACK);

    const int eye_w = 76;
    const int eye_h = 110;
    const int eye_r = 22;
    const int left_x = 55;
    const int right_x = 189;
    const int eye_y = 65;

    uint16_t *line_buf = (uint16_t *)heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line_buf) return;

    uint16_t be_cyan = SWAP_BYTES(COLOR_CYAN);
    uint16_t be_white = SWAP_BYTES(COLOR_WHITE);
    uint16_t be_black = 0x0000;

    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            line_buf[x] = be_black;
        }

        if (y >= eye_y && y < eye_y + eye_h) {
            int dy = y - eye_y;
            for (int x = 0; x < LCD_H_RES; x++) {
                int in_left = (x >= left_x && x < left_x + eye_w);
                int in_right = (x >= right_x && x < right_x + eye_w);

                if (in_left || in_right) {
                    int ex = in_left ? (x - left_x) : (x - right_x);
                    bool inside = true;

                    if (ex < eye_r && dy < eye_r) {
                        inside = ((eye_r - ex) * (eye_r - ex) + (eye_r - dy) * (eye_r - dy)) <= (eye_r * eye_r);
                    } else if (ex >= eye_w - eye_r && dy < eye_r) {
                        int dx = ex - (eye_w - eye_r - 1);
                        inside = (dx * dx + (eye_r - dy) * (eye_r - dy)) <= (eye_r * eye_r);
                    } else if (ex < eye_r && dy >= eye_h - eye_r) {
                        int dy2 = dy - (eye_h - eye_r - 1);
                        inside = ((eye_r - ex) * (eye_r - ex) + dy2 * dy2) <= (eye_r * eye_r);
                    } else if (ex >= eye_w - eye_r && dy >= eye_h - eye_r) {
                        int dx = ex - (eye_w - eye_r - 1);
                        int dy2 = dy - (eye_h - eye_r - 1);
                        inside = (dx * dx + dy2 * dy2) <= (eye_r * eye_r);
                    }

                    if (inside) {
                        if (ex >= 44 && ex <= 60 && dy >= 18 && dy <= 38) {
                            line_buf[x] = be_white;
                        } else {
                            line_buf[x] = be_cyan;
                        }
                    }
                }
            }
        }
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LCD_H_RES, y + 1, line_buf);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    free(line_buf);
}

void display_draw_framebuffer(const uint16_t *buffer)
{
    if (!s_panel_handle || !buffer) return;
    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);

    for (int i = 0; i < DISPLAY_NUM_CHUNKS; i++) {
        if (s_chunk_buffers[i]) {
            int y = i * DISPLAY_CHUNK_LINES;
            int lines_to_draw = ((y + DISPLAY_CHUNK_LINES) <= LCD_V_RES) ? DISPLAY_CHUNK_LINES : (LCD_V_RES - y);
            memcpy(s_chunk_buffers[i], buffer + (y * LCD_H_RES), LCD_H_RES * lines_to_draw * sizeof(uint16_t));
            esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LCD_H_RES, y + lines_to_draw, s_chunk_buffers[i]);
            if (s_trans_done_sem) {
                xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(100));
            }
        }
    }

    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}

void display_draw_color_block(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel_handle || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    size_t pixels = w * h;
    uint16_t *buf = (uint16_t *)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!buf) {
        buf = (uint16_t *)malloc(pixels * sizeof(uint16_t));
    }
    if (!buf) return;

    uint16_t be_color = SWAP_BYTES(color);
    for (size_t i = 0; i < pixels; i++) {
        buf[i] = be_color;
    }

    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, buf);
    if (s_trans_done_sem) {
        xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(100));
    }
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);

    free(buf);
}

void display_draw_bitmap_block(int x, int y, int w, int h, const uint16_t *buffer)
{
    if (!s_panel_handle || !buffer || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || (x + w) > LCD_H_RES || (y + h) > LCD_V_RES) return;

    if (s_display_mutex) xSemaphoreTake(s_display_mutex, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(s_panel_handle, x, y, x + w, y + h, buffer);
    if (s_trans_done_sem) {
        xSemaphoreTake(s_trans_done_sem, pdMS_TO_TICKS(100));
    }
    if (s_display_mutex) xSemaphoreGive(s_display_mutex);
}
