#include "audio_player.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "i2s_audio.h"
#include "sdcard.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "audio_player";

#define MP3_STREAM_BUF_SIZE   4096

typedef enum {
    CMD_PLAY_FILE,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_STOP,
    CMD_PLAY_CLICK,
    CMD_PLAY_HAPPY,
    CMD_PLAY_TEST_TONE
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t type;
    char filepath[256];
} audio_cmd_t;

static QueueHandle_t s_audio_cmd_queue = NULL;
static TaskHandle_t s_audio_task_handle = NULL;
static audio_player_state_t s_player_state = AUDIO_STATE_IDLE;
static char s_current_track_name[128] = "No Track";

static void extract_filename(const char *path, char *dest, size_t dest_sz)
{
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    p = (p != NULL) ? p + 1 : path;
    strncpy(dest, p, dest_sz - 1);
    dest[dest_sz - 1] = '\0';
}

static bool play_wav_file(FILE *f)
{
    uint8_t header[44];
    if (fread(header, 1, 44, f) != 44) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return false;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Not a valid RIFF/WAVE file");
        return false;
    }

    uint16_t channels = *(uint16_t *)(header + 22);
    uint32_t sample_rate = *(uint32_t *)(header + 24);
    uint16_t bits = *(uint16_t *)(header + 34);

    ESP_LOGI(TAG, "WAV Info: %lu Hz, %d bits, %d ch", (unsigned long)sample_rate, bits, channels);
    i2s_audio_set_params(sample_rate, bits, channels);

    uint8_t buf[1024];
    while (s_player_state != AUDIO_STATE_IDLE) {
        if (s_player_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check if new command arrived
        audio_cmd_t next_cmd;
        if (xQueuePeek(s_audio_cmd_queue, &next_cmd, 0) == pdTRUE) {
            return false;
        }

        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) break;

        size_t written = 0;
        i2s_audio_write(buf, r, &written, 100);
    }
    return true;
}

static bool play_mp3_file(FILE *f)
{
    mp3dec_t mp3d;
    mp3dec_init(&mp3d);

    uint8_t *input_buf = malloc(MP3_STREAM_BUF_SIZE);
    mp3d_sample_t *pcm_out = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(mp3d_sample_t));

    if (!input_buf || !pcm_out) {
        ESP_LOGE(TAG, "Failed to allocate MP3 decode buffers");
        if (input_buf) free(input_buf);
        if (pcm_out) free(pcm_out);
        return false;
    }

    int bytes_left = 0;
    bool is_first_frame = true;

    while (s_player_state != AUDIO_STATE_IDLE) {
        if (s_player_state == AUDIO_STATE_PAUSED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Check if next command is pending
        audio_cmd_t next_cmd;
        if (xQueuePeek(s_audio_cmd_queue, &next_cmd, 0) == pdTRUE) {
            break;
        }

        // Fill stream buffer
        if (bytes_left < (MP3_STREAM_BUF_SIZE / 2)) {
            if (bytes_left > 0) {
                memmove(input_buf, input_buf + (MP3_STREAM_BUF_SIZE - bytes_left), bytes_left);
            }
            size_t read_cnt = fread(input_buf + bytes_left, 1, MP3_STREAM_BUF_SIZE - bytes_left, f);
            bytes_left += read_cnt;
            if (bytes_left == 0) {
                break; // End of file reached
            }
        }

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&mp3d, input_buf, bytes_left, pcm_out, &info);

        if (info.frame_bytes > 0) {
            bytes_left -= info.frame_bytes;
            memmove(input_buf, input_buf + info.frame_bytes, bytes_left);

            if (samples > 0) {
                if (is_first_frame) {
                    ESP_LOGI(TAG, "MP3 Stream: %d Hz, %d Channels, %d kbps",
                             info.hz, info.channels, info.bitrate_kbps);
                    i2s_audio_set_params(info.hz, 16, info.channels);
                    is_first_frame = false;
                }

                size_t pcm_bytes = samples * info.channels * sizeof(mp3d_sample_t);
                size_t written = 0;
                i2s_audio_write(pcm_out, pcm_bytes, &written, 150);
            }
        } else {
            // Skip bad/sync bytes
            if (bytes_left > 0) {
                bytes_left--;
                memmove(input_buf, input_buf + 1, bytes_left);
            }
        }
    }

    free(input_buf);
    free(pcm_out);
    return true;
}

static void audio_worker_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio Player Background Task started");
    audio_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_audio_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            switch (cmd.type) {
                case CMD_PLAY_FILE: {
                    extract_filename(cmd.filepath, s_current_track_name, sizeof(s_current_track_name));
                    ESP_LOGI(TAG, "Opening audio file: '%s'...", cmd.filepath);

                    FILE *f = fopen(cmd.filepath, "rb");
                    if (!f) {
                        ESP_LOGE(TAG, "Failed to open file: %s", cmd.filepath);
                        s_player_state = AUDIO_STATE_IDLE;
                        break;
                    }

                    s_player_state = AUDIO_STATE_PLAYING;

                    const char *ext = strrchr(cmd.filepath, '.');
                    if (ext && strcasecmp(ext, ".wav") == 0) {
                        play_wav_file(f);
                    } else {
                        play_mp3_file(f);
                    }

                    fclose(f);
                    s_player_state = AUDIO_STATE_IDLE;
                    ESP_LOGI(TAG, "Finished playback of '%s'", s_current_track_name);
                    break;
                }

                case CMD_PLAY_CLICK:
                    i2s_audio_play_beep(1800, 25);
                    break;

                case CMD_PLAY_HAPPY:
                    // Play a cheerful multi-tone Loona chirp
                    i2s_audio_play_beep(880, 60);
                    vTaskDelay(pdMS_TO_TICKS(15));
                    i2s_audio_play_beep(1174, 80);
                    vTaskDelay(pdMS_TO_TICKS(15));
                    i2s_audio_play_beep(1760, 120);
                    break;

                case CMD_PLAY_TEST_TONE:
                    ESP_LOGI(TAG, "Playing 1kHz test tone (1.0 sec)...");
                    i2s_audio_play_beep(1000, 1000);
                    break;

                case CMD_PAUSE:
                    s_player_state = AUDIO_STATE_PAUSED;
                    break;

                case CMD_RESUME:
                    s_player_state = AUDIO_STATE_PLAYING;
                    break;

                case CMD_STOP:
                    s_player_state = AUDIO_STATE_IDLE;
                    break;
            }
        }
    }
}

esp_err_t audio_player_init(void)
{
    if (s_audio_cmd_queue != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Audio Player Service...");

    // Initialize I2S Audio Driver default
    i2s_audio_init(44100, 16, 2);

    s_audio_cmd_queue = xQueueCreate(8, sizeof(audio_cmd_t));
    if (!s_audio_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create audio command queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        audio_worker_task,
        "audio_worker",
        8192,
        NULL,
        5, // High priority for smooth jitter-free audio
        &s_audio_task_handle,
        0  // Core 0
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio worker task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio Player Service initialized successfully!");
    return ESP_OK;
}

esp_err_t audio_player_play_file(const char *filepath)
{
    if (!filepath || strlen(filepath) == 0) return ESP_ERR_INVALID_ARG;

    audio_cmd_t cmd = {
        .type = CMD_PLAY_FILE,
    };
    strncpy(cmd.filepath, filepath, sizeof(cmd.filepath) - 1);

    // Stop current track and queue new one
    s_player_state = AUDIO_STATE_IDLE;
    xQueueReset(s_audio_cmd_queue);
    xQueueSend(s_audio_cmd_queue, &cmd, portMAX_DELAY);
    return ESP_OK;
}

void audio_player_pause(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) {
        s_player_state = AUDIO_STATE_PAUSED;
    }
}

void audio_player_resume(void)
{
    if (s_player_state == AUDIO_STATE_PAUSED) {
        s_player_state = AUDIO_STATE_PLAYING;
    }
}

void audio_player_toggle_play_pause(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) {
        s_player_state = AUDIO_STATE_PAUSED;
    } else if (s_player_state == AUDIO_STATE_PAUSED) {
        s_player_state = AUDIO_STATE_PLAYING;
    }
}

void audio_player_stop(void)
{
    s_player_state = AUDIO_STATE_IDLE;
    audio_cmd_t cmd = {.type = CMD_STOP};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

audio_player_state_t audio_player_get_state(void)
{
    return s_player_state;
}

const char* audio_player_get_current_track_name(void)
{
    return s_current_track_name;
}

void audio_player_set_volume(uint8_t volume)
{
    i2s_audio_set_volume(volume);
}

uint8_t audio_player_get_volume(void)
{
    return i2s_audio_get_volume();
}

void audio_player_play_ui_click(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return; // Don't interrupt music with clicks
    audio_cmd_t cmd = {.type = CMD_PLAY_CLICK};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

void audio_player_play_happy_sound(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return;
    audio_cmd_t cmd = {.type = CMD_PLAY_HAPPY};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}

void audio_player_play_test_tone(void)
{
    if (s_player_state == AUDIO_STATE_PLAYING) return;
    audio_cmd_t cmd = {.type = CMD_PLAY_TEST_TONE};
    xQueueSend(s_audio_cmd_queue, &cmd, 0);
}
