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
static uint16_t *s_dma_buffer = NULL;

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI bus for display...");

    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * sizeof(uint16_t),
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
        .pclk_hz = 20 * 1000 * 1000, // 20 MHz for stable breadboard SPI
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

    ESP_LOGI(TAG, "Creating LCD panel driver (ILI9341_2_DRIVER)...");
    static const ili9341_vendor_config_t vendor_config = {
        .init_cmds = ili9341_lcd_init_vendor,
        .init_cmds_size = sizeof(ili9341_lcd_init_vendor) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
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
    esp_lcd_panel_invert_color(s_panel_handle, false);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    ESP_LOGI(TAG, "Display initialized successfully (ILI9341 320x240 Landscape)!");
    return ESP_OK;
}

void display_fill_screen(uint16_t color)
{
    if (!s_panel_handle) return;

    const int chunk_lines = 20;
    size_t chunk_pixels = LCD_H_RES * chunk_lines;
    
    if (!s_dma_buffer) {
        s_dma_buffer = (uint16_t *)heap_caps_malloc(chunk_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!s_dma_buffer) {
            ESP_LOGE(TAG, "No memory for display DMA buffer");
            return;
        }
    }

    uint16_t be_color = SWAP_BYTES(color);
    for (size_t i = 0; i < chunk_pixels; i++) {
        s_dma_buffer[i] = be_color;
    }

    for (int y = 0; y < LCD_V_RES; y += chunk_lines) {
        int lines_to_draw = ((y + chunk_lines) <= LCD_V_RES) ? chunk_lines : (LCD_V_RES - y);
        esp_lcd_panel_draw_bitmap(s_panel_handle, 0, y, LCD_H_RES, y + lines_to_draw, s_dma_buffer);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
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
