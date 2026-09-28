#include "sdcard.h"
#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "display.h"
#include "touch_xpt2046.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sdcard_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    // Ensure Touch controller CS (GPIO 38) is deselected (HIGH) so XPT2046 releases MISO (GPIO 39)
    gpio_set_direction((gpio_num_t)TOUCH_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TOUCH_PIN_CS, 1);
    gpio_set_pull_mode((gpio_num_t)TOUCH_PIN_CS, GPIO_PULLUP_ONLY);

    // Configure SD Card CS pin with internal pullup & deselect
    gpio_set_direction((gpio_num_t)SDCARD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SDCARD_PIN_CS, 1);
    gpio_set_pull_mode((gpio_num_t)SDCARD_PIN_CS, GPIO_PULLUP_ONLY);

    // Maximize drive capability on all SPI lines to overcome onboard series resistors (R1, R2, R3)
    gpio_set_drive_capability((gpio_num_t)LCD_PIN_SCK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)LCD_PIN_MOSI, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)SDCARD_PIN_CS, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)LCD_PIN_MISO, GPIO_DRIVE_CAP_3);

    // Ensure shared MISO line has internal pullup enabled
    gpio_set_pull_mode((gpio_num_t)LCD_PIN_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)SDCARD_PIN_CS, GPIO_PULLUP_ONLY);

    // Send dummy clock cycles with CS=HIGH to wake up SD card into SPI mode
    spi_device_handle_t dummy_dev = NULL;
    spi_device_interface_config_t dummy_cfg = {
        .clock_speed_hz = 100 * 1000, // 100 kHz gentle clock
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    if (spi_bus_add_device(LCD_HOST, &dummy_cfg, &dummy_dev) == ESP_OK) {
        uint8_t dummy_data[32];
        memset(dummy_data, 0xFF, sizeof(dummy_data));
        spi_transaction_t t = {
            .length = sizeof(dummy_data) * 8,
            .tx_buffer = dummy_data,
        };
        spi_device_polling_transmit(dummy_dev, &t);
        spi_bus_remove_device(dummy_dev);
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz high-speed SPI

    sdspi_device_config_t dev_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev_config.gpio_cs = (gpio_num_t)SDCARD_PIN_CS;
    dev_config.host_id = (spi_host_device_t)LCD_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &dev_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card (%s). Check card insertion & CS wiring!", esp_err_to_name(ret));
        s_mounted = false;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "MicroSD Card mounted successfully at '%s'", SDCARD_MOUNT_POINT);

    // Print card metadata
    sdmmc_card_print_info(stdout, s_card);

    uint32_t total_mb = 0, free_mb = 0;
    if (sdcard_get_space_mb(&total_mb, &free_mb) == ESP_OK) {
        ESP_LOGI(TAG, "SD Card Capacity: %lu MB Total | %lu MB Free", (unsigned long)total_mb, (unsigned long)free_mb);
    }

    // List sample directories
    sdcard_print_directory(SDCARD_MOUNT_POINT "/sounds");
    sdcard_print_directory(SDCARD_MOUNT_POINT "/music");

    return ESP_OK;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

esp_err_t sdcard_get_space_mb(uint32_t *out_total_mb, uint32_t *out_free_mb)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs;
    DWORD fre_clust, fre_sect, tot_sect;

    if (f_getfree("0:", &fre_clust, &fs) != FR_OK) {
        return ESP_FAIL;
    }

    tot_sect = (fs->n_fatent - 2) * fs->csize;
    fre_sect = fre_clust * fs->csize;

    // Assuming 512 bytes per sector (standard SD)
    if (out_total_mb) *out_total_mb = (uint32_t)(tot_sect / 2048);
    if (out_free_mb)  *out_free_mb  = (uint32_t)(fre_sect / 2048);

    return ESP_OK;
}

void sdcard_print_directory(const char *dir_path)
{
    if (!s_mounted) {
        ESP_LOGW(TAG, "SD card not mounted");
        return;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open directory: %s", dir_path);
        return;
    }

    ESP_LOGI(TAG, "─── Listing Directory: %s ───", dir_path);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        ESP_LOGI(TAG, "  [%02d] %s (Type: %d)", ++count, entry->d_name, entry->d_type);
    }
    closedir(dir);
    ESP_LOGI(TAG, "───────────────────────────────── (%d items)", count);
}

void sdcard_unmount(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_mounted = false;
        ESP_LOGI(TAG, "SD Card unmounted successfully");
    }
}
