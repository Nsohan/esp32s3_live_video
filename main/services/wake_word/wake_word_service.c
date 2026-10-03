#include "wake_word_service.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "i2s_mic.h"
#include "audio_player.h"
#include "roboeyes_display.h"
#include "app_launcher.h"
#include <dirent.h>
#include <strings.h>
#include "esp_random.h"
#include "sdcard.h"

static const char *TAG = "wake_word";

#define WAKE_RESPONSE_DIR_PRIMARY   "/sdcard/sounds/wake_word_response_sound"
#define WAKE_RESPONSE_DIR_ALT1      "/sdcard/wake_word_response_sound"
#define WAKE_RESPONSE_DIR_ALT2      "/sdcard/sound/wake_word_response_sound"

static char s_last_wake_sound[256] = "";

static bool play_random_wake_response_sound(void)
{
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, cannot play wake word response sound");
        return false;
    }

    const char *candidate_dirs[] = {
        WAKE_RESPONSE_DIR_PRIMARY,
        WAKE_RESPONSE_DIR_ALT1,
        WAKE_RESPONSE_DIR_ALT2
    };

    const char *target_dir = NULL;
    DIR *dir = NULL;

    for (size_t i = 0; i < sizeof(candidate_dirs) / sizeof(candidate_dirs[0]); i++) {
        dir = opendir(candidate_dirs[i]);
        if (dir) {
            target_dir = candidate_dirs[i];
            break;
        }
    }

    if (!dir || !target_dir) {
        ESP_LOGW(TAG, "Wake response directory not found (searched '%s')", WAKE_RESPONSE_DIR_PRIMARY);
        return false;
    }

    // Pass 1: Reservoir sampling choosing a random sound different from s_last_wake_sound
    char chosen_path[512] = "";
    char chosen_filename[256] = "";
    int valid_file_count = 0;
    int diff_file_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        const char *ext = strrchr(entry->d_name, '.');
        if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
            valid_file_count++;

            // Prefer files different from the last played track to avoid immediate repeat
            if (s_last_wake_sound[0] == '\0' || strcmp(entry->d_name, s_last_wake_sound) != 0) {
                diff_file_count++;
                if ((esp_random() % diff_file_count) == 0) {
                    snprintf(chosen_path, sizeof(chosen_path), "%s/%s", target_dir, entry->d_name);
                    snprintf(chosen_filename, sizeof(chosen_filename), "%s", entry->d_name);
                }
            }
        }
    }

    // If all valid files matched s_last_wake_sound (e.g. only 1 file in directory), fallback to it
    if (diff_file_count == 0 && valid_file_count > 0) {
        rewinddir(dir);
        int fallback_count = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            const char *ext = strrchr(entry->d_name, '.');
            if (ext && (strcasecmp(ext, ".mp3") == 0 || strcasecmp(ext, ".wav") == 0)) {
                fallback_count++;
                if ((esp_random() % fallback_count) == 0) {
                    snprintf(chosen_path, sizeof(chosen_path), "%s/%s", target_dir, entry->d_name);
                    snprintf(chosen_filename, sizeof(chosen_filename), "%s", entry->d_name);
                }
            }
        }
    }

    closedir(dir);

    if (chosen_path[0] != '\0') {
        snprintf(s_last_wake_sound, sizeof(s_last_wake_sound), "%s", chosen_filename);

        ESP_LOGI(TAG, "Playing wake response sound at FULL 100%% volume (%d available): '%s'",
                 valid_file_count, chosen_path);
        esp_err_t err = audio_player_play_sound_effect_at_volume(chosen_path, 100);
        return (err == ESP_OK);
    }

    ESP_LOGW(TAG, "No .mp3 or .wav files found in '%s'", target_dir);
    return false;
}

static srmodel_list_t *s_models = NULL;
static const esp_wn_iface_t *s_wn = NULL;
static model_iface_data_t *s_wn_data = NULL;
static char *s_model_name = NULL;
static char *s_wake_word_name = NULL;

static TaskHandle_t s_task_handle = NULL;
static volatile bool s_task_running = false;
static volatile bool s_paused = false;
static volatile bool s_in_mic_read = false;
static wake_word_callback_t s_callback = NULL;

static void wake_word_task(void *arg);

esp_err_t wake_word_service_init(void)
{
    if (s_wn_data != NULL) {
        ESP_LOGW(TAG, "Wake word service already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Wake Word models from 'model' partition...");
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "Failed to load models from 'model' partition!");
        return ESP_FAIL;
    }

    // Filter wake word model (prefix "wn")
    s_model_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!s_model_name) {
        ESP_LOGE(TAG, "No WakeNet model found in flash partition!");
        return ESP_FAIL;
    }

    s_wake_word_name = esp_srmodel_get_wake_words(s_models, s_model_name);
    ESP_LOGI(TAG, "Found WakeNet model: '%s' (Wake word: '%s')",
             s_model_name, s_wake_word_name ? s_wake_word_name : "Jarvis");

    s_wn = esp_wn_handle_from_name(s_model_name);
    if (!s_wn) {
        ESP_LOGE(TAG, "Failed to get WakeNet interface for '%s'", s_model_name);
        return ESP_FAIL;
    }

    s_wn_data = s_wn->create(s_model_name, DET_MODE_95);
    if (!s_wn_data) {
        // Fallback to DET_MODE_90 if DET_MODE_95 fails
        ESP_LOGW(TAG, "DET_MODE_95 failed, trying DET_MODE_90...");
        s_wn_data = s_wn->create(s_model_name, DET_MODE_90);
    }

    if (!s_wn_data) {
        ESP_LOGE(TAG, "Failed to instantiate WakeNet model!");
        return ESP_FAIL;
    }

    int chunksize = s_wn->get_samp_chunksize(s_wn_data);
    int rate = s_wn->get_samp_rate(s_wn_data);
    ESP_LOGI(TAG, "WakeNet ready: chunk=%d samples, rate=%d Hz", chunksize, rate);

    return ESP_OK;
}

esp_err_t wake_word_service_start(void)
{
    if (!s_wn_data) {
        esp_err_t ret = wake_word_service_init();
        if (ret != ESP_OK) return ret;
    }

    if (s_task_running) {
        ESP_LOGW(TAG, "Wake word task already running");
        return ESP_OK;
    }

    s_task_running = true;
    s_paused = false;

    // Pin wake word neural network inference to Core 1
    // Stack: 8 KB (in bytes on ESP-IDF)
    BaseType_t ret = xTaskCreatePinnedToCore(
        wake_word_task,
        "wake_word_task",
        8192,
        NULL,
        5,
        &s_task_handle,
        1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wake_word_task");
        s_task_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wake word listening started in background (Core 1)");
    return ESP_OK;
}

void wake_word_service_stop(void)
{
    if (!s_task_running) return;
    s_task_running = false;

    for (int i = 0; i < 50 && s_task_handle != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wake_word_service_pause(void)
{
    if (!s_task_running) return;
    s_paused = true;
    for (int i = 0; i < 25 && s_in_mic_read; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "Wake word listening paused");
}

void wake_word_service_resume(void)
{
    if (!s_task_running) return;
    s_paused = false;
    ESP_LOGI(TAG, "Wake word listening resumed");
}

bool wake_word_service_is_running(void)
{
    return s_task_running && !s_paused;
}

void wake_word_service_set_callback(wake_word_callback_t cb)
{
    s_callback = cb;
}

static void wake_word_task(void *arg)
{
    int chunksize = s_wn->get_samp_chunksize(s_wn_data);
    if (chunksize <= 0) chunksize = 512;

    int16_t *mic_buf = malloc(chunksize * sizeof(int16_t));
    if (!mic_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio chunk buffer (%d samples)", chunksize);
        s_task_running = false;
        s_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, ">>> Wake word active! Speak: '%s' <<<",
             s_wake_word_name ? s_wake_word_name : "Jarvis");
    ESP_LOGI(TAG, "--------------------------------------------------");

    TickType_t last_trigger_tick = 0;

    while (s_task_running) {
        if (s_paused || audio_player_get_state() == AUDIO_STATE_PLAYING) {
            vTaskDelay(pdMS_TO_TICKS(50));
            last_trigger_tick = xTaskGetTickCount();
            continue;
        }

        size_t samples_read = 0;
        s_in_mic_read = true;
        esp_err_t err = i2s_mic_read(mic_buf, chunksize, &samples_read, 200);
        s_in_mic_read = false;

        if (err != ESP_OK || samples_read < (size_t)chunksize) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        wakenet_state_t state = s_wn->detect(s_wn_data, mic_buf);
        if (state == WAKENET_DETECTED) {
            TickType_t now = xTaskGetTickCount();
            // Debounce for 2.0 seconds so pet doesn't repeatedly trigger on echo
            if ((now - last_trigger_tick) > pdMS_TO_TICKS(2000)) {
                last_trigger_tick = now;

                ESP_LOGW(TAG, "**********************************************");
                ESP_LOGW(TAG, "*** WAKE WORD DETECTED: '%s'! ***",
                         s_wake_word_name ? s_wake_word_name : "Jarvis");
                ESP_LOGW(TAG, "**********************************************");

                // 1. Play random dynamic response sound from SD card (/sdcard/sounds/wake_word_response_sound)
                play_random_wake_response_sound();

                // 2. React on screen: if on App Menu, switch to RoboEyes view immediately
                if (app_launcher_get_current_state() == STATE_APP_MENU) {
                    app_launcher_switch_state(STATE_ROBOEYES_VIEW);
                }
                roboeyes_trigger_mood(ROBOEYES_MODE_HAPPY);

                // 3. User callback if set
                if (s_callback) {
                    s_callback(s_wake_word_name ? s_wake_word_name : "Jarvis");
                }
            }
        }
    }

    free(mic_buf);
    ESP_LOGI(TAG, "Wake word task exited");
    s_task_handle = NULL;
    vTaskDelete(NULL);
}
