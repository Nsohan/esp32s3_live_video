#include "battery_service.h"
#include "esp_log.h"

static const char *TAG = "battery_service";

static uint8_t s_battery_percentage = 98;
static bool s_is_charging = false;

void battery_service_init(void)
{
    ESP_LOGI(TAG, "Battery monitoring service initialized (Level: %u%%)", s_battery_percentage);
}

uint8_t battery_service_get_percentage(void)
{
    return s_battery_percentage;
}

bool battery_service_is_charging(void)
{
    return s_is_charging;
}

void battery_service_set_level(uint8_t percentage, bool is_charging)
{
    if (percentage > 100) percentage = 100;
    s_battery_percentage = percentage;
    s_is_charging = is_charging;
}
