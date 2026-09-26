#include "touch_xpt2046.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display.h"

static const char *TAG = "touch_xpt2046";

// Commands for XPT2046 (12-bit differential mode)
#define CMD_READ_X 0xD0
#define CMD_READ_Y 0x90
#define CMD_READ_Z1 0xB0
#define CMD_READ_Z2 0xC0

static spi_device_handle_t s_touch_spi_handle = NULL;
static SemaphoreHandle_t s_touch_mutex = NULL;

static uint16_t touch_read_adc(uint8_t cmd)
{
    if (!s_touch_spi_handle) return 0;

    uint8_t tx_data[3] = {cmd, 0x00, 0x00};
    uint8_t rx_data[3] = {0, 0, 0};

    spi_transaction_t t = {
        .flags = 0,
        .length = 24, // 24 bits (3 bytes)
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t ret = spi_device_polling_transmit(s_touch_spi_handle, &t);
    if (ret != ESP_OK) {
        return 0;
    }

    uint16_t val = ((uint16_t)rx_data[1] << 8) | rx_data[2];
    return (val >> 3) & 0x0FFF; // 12-bit ADC value
}

esp_err_t touch_init(void)
{
    if (!s_touch_mutex) {
        s_touch_mutex = xSemaphoreCreateMutex();
    }

    ESP_LOGI(TAG, "Attaching XPT2046 Touch device to SPI bus...");
    ESP_LOGI(TAG, "Pins: CS=%d, IRQ=%d, MISO=%d, MOSI=%d, CLK=%d",
             TOUCH_PIN_CS, TOUCH_PIN_IRQ, TOUCH_PIN_MISO, TOUCH_PIN_MOSI, TOUCH_PIN_CLK);

    // 1. Configure T_IRQ pin as input with pullup
    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    // 2. Add XPT2046 as an SPI device on the existing LCD SPI bus
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 MHz for reliable touch ADC
        .mode = 0,                         // SPI mode 0
        .spics_io_num = TOUCH_PIN_CS,      // GPIO 38
        .queue_size = 1,
        .flags = 0,
    };

    esp_err_t ret = spi_bus_add_device(LCD_HOST, &devcfg, &s_touch_spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add XPT2046 to SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wake up XPT2046 (power down reference, enable PENIRQ)
    touch_read_adc(0x80);

    ESP_LOGI(TAG, "XPT2046 Touch hardware SPI device attached successfully!");
    return ESP_OK;
}

bool touch_is_touched(void)
{
    // T_IRQ goes LOW when screen is pressed
    return (gpio_get_level((gpio_num_t)TOUCH_PIN_IRQ) == 0);
}

static int compare_u16(const void *a, const void *b)
{
    return (*(uint16_t *)a - *(uint16_t *)b);
}

bool touch_read_raw(uint16_t *raw_x, uint16_t *raw_y)
{
    if (!touch_is_touched()) {
        return false;
    }

    if (s_touch_mutex) xSemaphoreTake(s_touch_mutex, portMAX_DELAY);

    // 7-sample median filter for high stability
    uint16_t samples_x[7];
    uint16_t samples_y[7];

    for (int i = 0; i < 7; i++) {
        samples_x[i] = touch_read_adc(CMD_READ_X);
        samples_y[i] = touch_read_adc(CMD_READ_Y);
        esp_rom_delay_us(15);
    }

    if (s_touch_mutex) xSemaphoreGive(s_touch_mutex);

    // Verify still touched
    if (!touch_is_touched()) {
        return false;
    }

    qsort(samples_x, 7, sizeof(uint16_t), compare_u16);
    qsort(samples_y, 7, sizeof(uint16_t), compare_u16);

    uint16_t median_x = samples_x[3];
    uint16_t median_y = samples_y[3];

    if (median_x < 150 || median_x > 3950 || median_y < 150 || median_y > 3950) {
        return false;
    }

    if (raw_x) *raw_x = median_x;
    if (raw_y) *raw_y = median_y;

    return true;
}

bool touch_read(int *out_x, int *out_y)
{
    uint16_t rx = 0, ry = 0;
    if (!touch_read_raw(&rx, &ry)) {
        return false;
    }

    int px = 0;
    int py = 0;

    // Apply orientation transforms for 320x240 Landscape
#if TOUCH_SWAP_XY
    // Sensor X moves with Display Y, Sensor Y moves with Display X
    px = ((int)ry - TOUCH_RAW_Y_MIN) * 320 / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
    py = ((int)rx - TOUCH_RAW_X_MIN) * 240 / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
#else
    px = ((int)rx - TOUCH_RAW_X_MIN) * 320 / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
    py = ((int)ry - TOUCH_RAW_Y_MIN) * 240 / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
#endif

#if TOUCH_INVERT_X
    px = 319 - px;
#endif

#if TOUCH_INVERT_Y
    py = 239 - py;
#endif

    // Clamp to screen boundaries
    if (px < 0) px = 0;
    if (px > 319) px = 319;
    if (py < 0) py = 0;
    if (py > 239) py = 239;

    // Log coordinates for calibration validation
    ESP_LOGI(TAG, "TOUCH: Raw[X=%u, Y=%u] => Screen[X=%d, Y=%d]", rx, ry, px, py);

    if (out_x) *out_x = px;
    if (out_y) *out_y = py;

    return true;
}

bool touch_read_all(int *out_x, int *out_y, uint16_t *out_rx, uint16_t *out_ry)
{
    uint16_t rx = 0, ry = 0;
    if (!touch_read_raw(&rx, &ry)) {
        return false;
    }

    int px = 0;
    int py = 0;

#if TOUCH_SWAP_XY
    px = ((int)ry - TOUCH_RAW_Y_MIN) * 320 / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
    py = ((int)rx - TOUCH_RAW_X_MIN) * 240 / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
#else
    px = ((int)rx - TOUCH_RAW_X_MIN) * 320 / (TOUCH_RAW_X_MAX - TOUCH_RAW_X_MIN);
    py = ((int)ry - TOUCH_RAW_Y_MIN) * 240 / (TOUCH_RAW_Y_MAX - TOUCH_RAW_Y_MIN);
#endif

#if TOUCH_INVERT_X
    px = 319 - px;
#endif

#if TOUCH_INVERT_Y
    py = 239 - py;
#endif

    if (px < 0) px = 0;
    if (px > 319) px = 319;
    if (py < 0) py = 0;
    if (py > 239) py = 239;

    if (out_x) *out_x = px;
    if (out_y) *out_y = py;
    if (out_rx) *out_rx = rx;
    if (out_ry) *out_ry = ry;

    return true;
}
