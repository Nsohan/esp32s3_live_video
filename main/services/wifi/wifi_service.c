#include "wifi_service.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>
#include "app_launcher.h"
#include "http_stream.h"

#include "esp_sntp.h"

static const char *TAG = "wifi_service";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static int s_retry_num = 0;
static char s_current_ip[32] = "Connecting...";
static char s_saved_ssid[32] = "";
static bool s_server_started = false;
static bool s_is_connected = false;

static void init_sntp(void)
{
    static bool sntp_started = false;
    if (!sntp_started) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        sntp_started = true;
        ESP_LOGI(TAG, "SNTP network time sync started");
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", disconn ? disconn->reason : -1);
        bool was_connected = s_is_connected;
        s_is_connected = false;

        if (s_retry_num < MAX_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_num, MAX_RETRY);
            esp_wifi_connect();
            if (was_connected) {
                snprintf(s_current_ip, sizeof(s_current_ip), "Reconnecting...");
                app_launcher_set_wifi_info(s_saved_ssid, s_current_ip);
            }
        } else {
            ESP_LOGW(TAG, "WiFi connection failed after %d attempts", MAX_RETRY);
            snprintf(s_current_ip, sizeof(s_current_ip), "Disconnected");
            app_launcher_set_wifi_info(s_saved_ssid, s_current_ip);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "[OK] WiFi Connected! IP: %s", s_current_ip);
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        // Update App Launcher status bar live
        app_launcher_set_wifi_info(s_saved_ssid, s_current_ip);

        // Start background SNTP network clock sync
        init_sntp();

        // Start HTTP stream server once IP is obtained
        if (!s_server_started) {
            start_camera_server();
            s_server_started = true;
        }
    }
}

const char* wifi_service_get_ip_string(void)
{
    return s_current_ip;
}

bool wifi_service_is_connected(void)
{
    return s_is_connected;
}

void wifi_service_init(const char *ssid, const char *password)
{
    strncpy(s_saved_ssid, ssid, sizeof(s_saved_ssid) - 1);

    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.static_rx_buf_num = 4;
    cfg.dynamic_rx_buf_num = 16;
    cfg.dynamic_tx_buf_num = 16;
    cfg.mgmt_sbuf_num = 16;
    
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
        return;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                         &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                         &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi in background: %s", ssid);
}
