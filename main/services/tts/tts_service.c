#include "tts_service.h"
#include "sam/sam.h"
#include "sam/reciter.h"
#include "sam/debug.h"
#include "i2s_audio.h"
#include "audio_player.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "tts_service";

typedef struct {
    char text[256];
} tts_msg_t;

static QueueHandle_t s_tts_queue = NULL;
static volatile bool s_tts_busy = false;

// Default voice parameters: retro pet robot
static uint8_t s_pitch = 64;
static uint8_t s_speed = 72;
static uint8_t s_mouth = 128;
static uint8_t s_throat = 128;

void tts_set_voice(uint8_t pitch, uint8_t speed, uint8_t mouth, uint8_t throat)
{
    s_pitch = pitch;
    s_speed = speed;
    s_mouth = mouth;
    s_throat = throat;
}

void tts_set_preset(tts_voice_preset_t preset)
{
    switch (preset) {
        case TTS_VOICE_PET_LITTLE:
            tts_set_voice(84, 82, 150, 140);
            break;
        case TTS_VOICE_DEEP_BOT:
            tts_set_voice(42, 68, 110, 105);
            break;
        case TTS_VOICE_ELF:
            tts_set_voice(92, 95, 160, 150);
            break;
        case TTS_VOICE_ROBOT:
        default:
            tts_set_voice(64, 72, 128, 128);
            break;
    }
}

bool tts_is_busy(void)
{
    return s_tts_busy;
}

esp_err_t tts_speak(const char *text)
{
    if (!text || strlen(text) == 0) return ESP_ERR_INVALID_ARG;
    if (!s_tts_queue) {
        ESP_LOGE(TAG, "TTS service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    tts_msg_t msg;
    size_t tlen = strlen(text);
    if (tlen >= sizeof(msg.text)) tlen = sizeof(msg.text) - 1;
    memcpy(msg.text, text, tlen);
    msg.text[tlen] = '\0';

    if (xQueueSend(s_tts_queue, &msg, pdMS_TO_TICKS(100)) != pdPASS) {
        ESP_LOGW(TAG, "TTS queue full, dropping message");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void tts_task(void *arg)
{
    tts_msg_t msg;
    while (1) {
        if (xQueueReceive(s_tts_queue, &msg, portMAX_DELAY) == pdPASS) {
            s_tts_busy = true;
            ESP_LOGI(TAG, "Synthesizing text: \"%s\"", msg.text);

            char input[256];
            memset(input, 0, sizeof(input));
            size_t mlen = strlen(msg.text);
            if (mlen >= sizeof(input) - 2) mlen = sizeof(input) - 2;
            memcpy(input, msg.text, mlen);
            input[mlen] = '\0';

            // Convert to uppercase (SAM requires uppercase ASCII)
            for (int i = 0; input[i] != 0; i++) {
                input[i] = toupper((unsigned char)input[i]);
            }

            // Append terminator '[' for TextToPhonemes
            strncat(input, "[", sizeof(input) - strlen(input) - 1);

            int phoneme_res = TextToPhonemes((unsigned char *)input);
            if (!phoneme_res) {
                ESP_LOGE(TAG, "TextToPhonemes failed for: %s", msg.text);
                s_tts_busy = false;
                continue;
            }

            // Configure SAM voice
            SetSpeed(s_speed);
            SetPitch(s_pitch);
            SetMouth(s_mouth);
            SetThroat(s_throat);
            SetInput(input);

            if (!SAMMain()) {
                ESP_LOGE(TAG, "SAMMain failed");
                FreeBuffer();
                s_tts_busy = false;
                continue;
            }

            char *buf = GetBuffer();
            int sample_count = GetBufferLength() / 50;

            if (buf && sample_count > 0) {
                ESP_LOGI(TAG, "Generated %d samples at 22050 Hz. Playing...", sample_count);

                // Configure I2S for 22050 Hz 16-bit mono
                i2s_audio_set_params(22050, 16, 1);

                // Convert 8-bit unsigned PCM [0..255] to 16-bit signed PCM [-32768..32767]
                // Stream in chunks of 512 samples
                int16_t chunk[512];
                int offset = 0;

                while (offset < sample_count) {
                    int chunk_len = (sample_count - offset < 512) ? (sample_count - offset) : 512;
                    for (int i = 0; i < chunk_len; i++) {
                        uint8_t u8 = (uint8_t)buf[offset + i];
                        // Convert unsigned 8-bit centered at 128 to signed 16-bit
                        chunk[i] = (int16_t)(((int32_t)u8 - 128) * 256);
                    }

                    size_t written = 0;
                    i2s_audio_write(chunk, chunk_len * sizeof(int16_t), &written, 500);
                    offset += chunk_len;
                }

                // Small silence at end for clean tail
                memset(chunk, 0, sizeof(chunk));
                size_t written = 0;
                i2s_audio_write(chunk, 256 * sizeof(int16_t), &written, 200);
            }

            FreeBuffer();
            s_tts_busy = false;
            ESP_LOGI(TAG, "Speech finished");
        }
    }
}

esp_err_t tts_service_init(void)
{
    if (s_tts_queue != NULL) {
        return ESP_OK;
    }

    s_tts_queue = xQueueCreate(4, sizeof(tts_msg_t));
    if (!s_tts_queue) {
        ESP_LOGE(TAG, "Failed to create TTS queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_task,
        "tts_task",
        16384,
        NULL,
        4,
        NULL,
        1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create TTS task");
        vQueueDelete(s_tts_queue);
        s_tts_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TTS Service initialized successfully");
    return ESP_OK;
}
