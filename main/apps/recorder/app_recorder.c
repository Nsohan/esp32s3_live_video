#include "app_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "display_gfx.h"
#include "i2s_mic.h"
#include "i2s_audio.h"
#include "audio_player.h"
#include "sdcard.h"
#include "wake_word_service.h"

static const char *TAG = "app_recorder";

#define RECORDER_FILEPATH   "/sdcard/recordings/rec_latest.wav"
#define RECORDER_DIR        "/sdcard/recordings"
#define VU_SEGMENTS_COUNT   20
#define VU_START_X          21
#define VU_START_Y          98
#define VU_SEG_W            12
#define VU_SEG_H            22
#define VU_SEG_GAP          2

typedef enum {
    MODE_IDLE,
    MODE_RECORDING,
    MODE_PLAYING,
    MODE_LIVE_VOICE
} recorder_mode_t;

typedef struct {
    char riff_id[4];        // "RIFF"
    uint32_t riff_sz;       // Total file size - 8
    char wave_id[4];        // "WAVE"
    char fmt_id[4];         // "fmt "
    uint32_t fmt_sz;        // 16
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;  // 1 = Mono
    uint32_t sample_rate;   // 16000
    uint32_t byte_rate;     // sample_rate * num_channels * 2 = 32000
    uint16_t block_align;   // num_channels * 2 = 2
    uint16_t bits_per_sample;// 16
    char data_id[4];        // "data"
    uint32_t data_sz;       // data payload bytes
} __attribute__((packed)) wav_header_t;

static recorder_mode_t s_mode = MODE_IDLE;
static TaskHandle_t s_rec_task_handle = NULL;
static volatile bool s_rec_running = false;
static uint32_t s_rec_start_time_ms = 0;
static uint32_t s_rec_elapsed_ms = 0;
static uint32_t s_total_recorded_bytes = 0;

static TaskHandle_t s_live_task_handle = NULL;
static volatile bool s_live_running = false;
static volatile int s_live_rms = 0;

// RAM backup buffer if SD card is not present
#define RAM_BUFFER_MAX_BYTES (320 * 1024) // 10 seconds of 16kHz 16-bit mono
static int16_t *s_ram_buffer = NULL;
static size_t s_ram_buffer_bytes = 0;
static bool s_has_recording = false;

// Cached values to avoid screen flicker on updates
static int s_prev_lit_segments = -1;
static int s_prev_peak_segment = -1;
static float s_decay_peak = 0.0f;
static int s_last_drawn_rms = -1;
static int s_last_drawn_peak = -1;
static recorder_mode_t s_last_drawn_mode = (recorder_mode_t)-1;
static uint32_t s_last_drawn_sec = 0xFFFFFFFF;
static bool s_last_blink_state = false;

static void write_wav_header(FILE *f, uint32_t data_size)
{
    wav_header_t hdr = {
        .riff_id = {'R', 'I', 'F', 'F'},
        .riff_sz = 36 + data_size,
        .wave_id = {'W', 'A', 'V', 'E'},
        .fmt_id = {'f', 'm', 't', ' '},
        .fmt_sz = 16,
        .audio_format = 1,
        .num_channels = 1,
        .sample_rate = I2S_MIC_SAMPLE_RATE,
        .byte_rate = I2S_MIC_SAMPLE_RATE * 1 * 2,
        .block_align = 2,
        .bits_per_sample = 16,
        .data_id = {'d', 'a', 't', 'a'},
        .data_sz = data_size
    };
    fwrite(&hdr, 1, sizeof(wav_header_t), f);
}

static void recorder_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio Recording Task started");
    FILE *f = NULL;
    bool sd_mode = sdcard_is_mounted();

    if (sd_mode) {
        mkdir(RECORDER_DIR, 0777);
        f = fopen(RECORDER_FILEPATH, "wb");
        if (f) {
            write_wav_header(f, 0); // Placeholder header
        } else {
            ESP_LOGW(TAG, "Failed to create SD file, falling back to RAM buffer");
            sd_mode = false;
        }
    }

    if (!sd_mode) {
        if (!s_ram_buffer) {
            s_ram_buffer = (int16_t *)heap_caps_malloc(RAM_BUFFER_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_ram_buffer) {
                s_ram_buffer = (int16_t *)malloc(64 * 1024); // Internal fallback (2 sec)
            }
        }
        s_ram_buffer_bytes = 0;
    }

    int16_t pcm_buf[512];
    int tick = 0;
    s_total_recorded_bytes = 0;

    while (s_rec_running) {
        size_t samples_read = 0;
        esp_err_t err = i2s_mic_read(pcm_buf, 512, &samples_read, 200);
        if (err != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 1. Calculate RMS & log to serial monitor in exact requested format
        double sum = 0;
        for (size_t i = 0; i < samples_read; i++) {
            sum += (double)pcm_buf[i] * pcm_buf[i];
        }
        int rms = (int)sqrt(sum / samples_read);

        if (++tick >= 8) {
            tick = 0;
            int bars = rms / 400;
            if (bars > 30) bars = 30;
            char meter[32];
            memset(meter, '#', bars);
            meter[bars] = 0;
            ESP_LOGI("i2s_mic", "RMS %5d |%s", rms, meter);
        }

        // 2. Write PCM audio to storage
        size_t chunk_bytes = samples_read * sizeof(int16_t);
        if (f) {
            fwrite(pcm_buf, 1, chunk_bytes, f);
            s_total_recorded_bytes += chunk_bytes;
        } else if (s_ram_buffer) {
            size_t max_b = s_ram_buffer ? (320 * 1024) : (64 * 1024);
            if (s_ram_buffer_bytes + chunk_bytes <= max_b) {
                memcpy((uint8_t *)s_ram_buffer + s_ram_buffer_bytes, pcm_buf, chunk_bytes);
                s_ram_buffer_bytes += chunk_bytes;
                s_total_recorded_bytes = s_ram_buffer_bytes;
            } else {
                ESP_LOGI(TAG, "RAM recording buffer full");
                break;
            }
        }

        s_rec_elapsed_ms = (uint32_t)(esp_timer_get_time() / 1000ULL) - s_rec_start_time_ms;
    }

    // 3. Finalize WAV header if file was used
    if (f) {
        fseek(f, 0, SEEK_SET);
        write_wav_header(f, s_total_recorded_bytes);
        fclose(f);
        ESP_LOGI(TAG, "Recording saved: %s (%lu bytes)", RECORDER_FILEPATH, (unsigned long)s_total_recorded_bytes);
    } else {
        ESP_LOGI(TAG, "Recording stored in RAM buffer (%zu bytes)", s_ram_buffer_bytes);
    }

    s_has_recording = (s_total_recorded_bytes > 0);
    s_rec_running = false;
    s_rec_task_handle = NULL;
    vTaskDelete(NULL);
}

#define LIVE_VOICE_GAIN  (1.0f) // Normal clean 1:1 gain, no artificial noise boost

static void live_voice_task(void *arg)
{
    ESP_LOGI(TAG, "Live Voice Test started: Clean 1:1 Mic -> Speaker Loopback");
    i2s_audio_set_params(I2S_MIC_SAMPLE_RATE, 16, 1);

    int16_t in_buf[256];
    int16_t out_buf[256];
    int tick = 0;

    while (s_live_running) {
        size_t samples_read = 0;
        esp_err_t err = i2s_mic_read(in_buf, 256, &samples_read, portMAX_DELAY);
        if (err != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        int64_t sum_squares = 0;
        int32_t max_val = 0;

        for (size_t i = 0; i < samples_read; i++) {
            int16_t s = in_buf[i];
            out_buf[i] = s;

            sum_squares += (int64_t)s * s;
            int32_t abs_s = abs(s);
            if (abs_s > max_val) max_val = abs_s;
        }

        // Direct write to MAX98357A speaker
        size_t bytes_to_write = samples_read * sizeof(int16_t);
        size_t bytes_written = 0;
        i2s_audio_write(out_buf, bytes_to_write, &bytes_written, 100);

        // Compute RMS for live VU meter display
        int rms = (int)sqrt((double)sum_squares / samples_read);
        s_live_rms = rms;

        if (++tick >= 16) {
            tick = 0;
            int bars = rms / 400;
            if (bars > 30) bars = 30;
            char meter[32];
            memset(meter, '#', bars);
            meter[bars] = 0;
            ESP_LOGI("live_mic", "RMS %5d |%s", rms, meter);
        }
    }

    s_live_rms = 0;
    ESP_LOGI(TAG, "Live Voice Test stopped");
    s_live_task_handle = NULL;
    vTaskDelete(NULL);
}

void app_recorder_init(void)
{
    ESP_LOGI(TAG, "Opening Sound Recorder App...");
    s_mode = MODE_IDLE;
    s_prev_lit_segments = -1;
    s_prev_peak_segment = -1;
    s_decay_peak = 0.0f;
    s_last_drawn_rms = -1;
    s_last_drawn_peak = -1;
    s_last_drawn_mode = (recorder_mode_t)-1;
    s_last_drawn_sec = 0xFFFFFFFF;
    s_last_blink_state = false;

    // Check if recorded file already exists on SD
    if (sdcard_is_mounted()) {
        struct stat st;
        if (stat(RECORDER_FILEPATH, &st) == 0 && st.st_size > 44) {
            s_has_recording = true;
            s_total_recorded_bytes = st.st_size - 44;
            s_rec_elapsed_ms = (s_total_recorded_bytes * 1000) / (I2S_MIC_SAMPLE_RATE * 2);
        }
    }

    // Pause wake word service so recorder gets exclusive mic access
    wake_word_service_pause();

    // Start background live RMS monitor (serial logging active while in app)
    i2s_mic_start_level_monitor();
}

void app_recorder_stop(void)
{
    ESP_LOGI(TAG, "Closing Sound Recorder App...");

    // Stop recording task if running
    if (s_rec_running) {
        s_rec_running = false;
        for (int i = 0; i < 30 && s_rec_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    // Stop live voice test if running
    if (s_live_running) {
        s_live_running = false;
        for (int i = 0; i < 30 && s_live_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    // Stop playback if playing
    if (s_mode == MODE_PLAYING) {
        audio_player_stop();
    }

    s_mode = MODE_IDLE;

    // Stop live mic level monitor (logging completely stops)
    i2s_mic_stop_level_monitor();

    // Resume background wake word detection
    wake_word_service_resume();
}

static void draw_vu_scale(void)
{
    gfx_draw_string(VU_START_X, 126, "-40dB", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string(VU_START_X + 65, 126, "-20dB", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string(VU_START_X + 130, 126, "-10dB", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
    gfx_draw_string(VU_START_X + 195, 126, "-3dB", COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
    gfx_draw_string(VU_START_X + 245, 126, " 0dB", COLOR_RED_ACCENT, COLOR_CARD_BG, 1);
}

static void draw_action_buttons(void)
{
    // Button 1: Left [ REC / STOP ]
    if (s_mode == MODE_RECORDING) {
        gfx_fill_round_rect(10, 180, 94, 48, 6, COLOR_RED_ACCENT);
        gfx_draw_round_rect(10, 180, 94, 48, 6, COLOR_WHITE);
        gfx_fill_rect(47, 192, 18, 18, COLOR_WHITE); // Stop Square
        gfx_draw_string_centered(10, 214, 94, "STOP REC", COLOR_WHITE, COLOR_RED_ACCENT, 1);
    } else {
        gfx_fill_round_rect(10, 180, 94, 48, 6, COLOR_CARD_BG);
        gfx_draw_round_rect(10, 180, 94, 48, 6, COLOR_RED_ACCENT);
        gfx_fill_round_rect(47, 192, 18, 18, 9, COLOR_RED_ACCENT); // Rec Circle
        gfx_draw_string_centered(10, 214, 94, "RECORD", COLOR_RED_ACCENT, COLOR_CARD_BG, 1);
    }

    // Button 2: Center [ PLAY / STOP ]
    if (s_mode == MODE_PLAYING) {
        gfx_fill_round_rect(113, 180, 94, 48, 6, COLOR_GREEN_ACCENT);
        gfx_draw_round_rect(113, 180, 94, 48, 6, COLOR_WHITE);
        gfx_fill_rect(151, 192, 18, 18, COLOR_BLACK); // Stop Square
        gfx_draw_string_centered(113, 214, 94, "STOP", COLOR_BLACK, COLOR_GREEN_ACCENT, 1);
    } else {
        uint16_t border = s_has_recording ? COLOR_GREEN_ACCENT : COLOR_CARD_BORDER;
        uint16_t text_col = s_has_recording ? COLOR_GREEN_ACCENT : COLOR_TEXT_DIM;
        gfx_fill_round_rect(113, 180, 94, 48, 6, COLOR_CARD_BG);
        gfx_draw_round_rect(113, 180, 94, 48, 6, border);
        gfx_draw_string_centered(113, 192, 94, ">", text_col, COLOR_CARD_BG, 2);
        gfx_draw_string_centered(113, 214, 94, "PLAY", text_col, COLOR_CARD_BG, 1);
    }

    // Button 3: Right [ LIVE VOICE TEST ]
    if (s_mode == MODE_LIVE_VOICE) {
        gfx_fill_round_rect(216, 180, 94, 48, 6, COLOR_CYAN_ACCENT);
        gfx_draw_round_rect(216, 180, 94, 48, 6, COLOR_WHITE);
        gfx_fill_rect(254, 192, 18, 18, COLOR_BLACK); // Stop Square
        gfx_draw_string_centered(216, 214, 94, "STOP LIVE", COLOR_BLACK, COLOR_CYAN_ACCENT, 1);
    } else {
        gfx_fill_round_rect(216, 180, 94, 48, 6, COLOR_CARD_BG);
        gfx_draw_round_rect(216, 180, 94, 48, 6, COLOR_CYAN_ACCENT);
        gfx_draw_string_centered(216, 192, 94, "MIC > SPK", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
        gfx_draw_string_centered(216, 214, 94, "LIVE VOICE", COLOR_WHITE, COLOR_CARD_BG, 1);
    }
}

void app_recorder_draw(void)
{
    display_fill_screen(COLOR_BG_DARK);

    // 1. Top Header
    app_common_draw_header("SOUND RECORDER");

    // 2. Storage & Status Card
    gfx_fill_round_rect(10, 32, 300, 40, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(10, 32, 300, 40, 4, COLOR_CARD_BORDER);

    // 3. Central VU Meter Frame Card
    gfx_fill_round_rect(10, 78, 300, 94, 4, COLOR_CARD_BG);
    gfx_draw_round_rect(10, 78, 300, 94, 4, COLOR_CARD_BORDER);
    gfx_draw_string(20, 84, "MIC INPUT LEVEL (VU METER)", COLOR_WHITE, COLOR_CARD_BG, 1);

    // Initial empty VU meter slots
    for (int i = 0; i < VU_SEGMENTS_COUNT; i++) {
        int x = VU_START_X + i * (VU_SEG_W + VU_SEG_GAP);
        gfx_fill_rect(x, VU_START_Y, VU_SEG_W, VU_SEG_H, 0x10A2);
    }

    draw_vu_scale();
    draw_action_buttons();

    s_prev_lit_segments = -1;
    s_prev_peak_segment = -1;
    s_last_drawn_rms = -1;
    s_last_drawn_peak = -1;
    s_last_drawn_mode = (recorder_mode_t)-1;
    s_last_drawn_sec = 0xFFFFFFFF;
    s_last_blink_state = false;
}

void app_recorder_update(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // 1. Check if playback has completed
    if (s_mode == MODE_PLAYING) {
        if (audio_player_get_state() != AUDIO_STATE_PLAYING) {
            s_mode = MODE_IDLE;
            draw_action_buttons();
            i2s_mic_start_level_monitor(); // Resume monitor after playback
        }
    }

    // 2. Calculate time to display
    uint32_t ms_val = 0;
    if (s_mode == MODE_RECORDING) {
        ms_val = now - s_rec_start_time_ms;
    } else {
        ms_val = s_rec_elapsed_ms;
    }
    uint32_t total_sec = ms_val / 1000;
    uint32_t sec_tenths = (ms_val % 1000) / 100;
    uint32_t mins = total_sec / 60;
    uint32_t secs = total_sec % 60;

    // 3. Status & Storage card differential updates
    bool blink = (ms_val / 500) % 2 == 0;
    if (s_last_drawn_mode != s_mode || s_last_drawn_sec != total_sec || (s_mode == MODE_RECORDING && s_last_blink_state != blink)) {
        s_last_drawn_mode = s_mode;
        s_last_drawn_sec = total_sec;
        s_last_blink_state = blink;

        // Clear card interior
        gfx_fill_rect(12, 34, 296, 36, COLOR_CARD_BG);

        // Status badge
        if (s_mode == MODE_RECORDING) {
            uint16_t dot_color = blink ? COLOR_RED_ACCENT : COLOR_CARD_BG;
            gfx_fill_round_rect(18, 44, 12, 12, 6, dot_color);
            gfx_draw_string(34, 45, "REC", COLOR_RED_ACCENT, COLOR_CARD_BG, 1);
        } else if (s_mode == MODE_PLAYING) {
            gfx_draw_string(18, 45, "> PLAY", COLOR_GREEN_ACCENT, COLOR_CARD_BG, 1);
        } else if (s_mode == MODE_LIVE_VOICE) {
            uint16_t dot_color = blink ? COLOR_CYAN_ACCENT : COLOR_CARD_BG;
            gfx_fill_round_rect(18, 44, 12, 12, 6, dot_color);
            gfx_draw_string(34, 45, "LIVE", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
        } else {
            gfx_fill_round_rect(18, 44, 10, 10, 5, COLOR_CYAN_ACCENT);
            gfx_draw_string(32, 45, "READY", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
        }

        // Storage indicator
        if (s_mode == MODE_LIVE_VOICE) {
            gfx_draw_string(88, 38, "AUDIO LOOPBACK ACTIVE", COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
            gfx_draw_string(88, 52, "Mic Input -> Direct to Speaker", COLOR_WHITE, COLOR_CARD_BG, 1);
        } else if (sdcard_is_mounted()) {
            gfx_draw_string(88, 38, "SD: /recordings/", COLOR_WHITE, COLOR_CARD_BG, 1);
            char sz_str[32];
            snprintf(sz_str, sizeof(sz_str), "rec_latest.wav (%lu KB)", (unsigned long)(s_total_recorded_bytes / 1024));
            gfx_draw_string(88, 52, sz_str, COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
        } else {
            gfx_draw_string(88, 38, "STORAGE: RAM Buffer", COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
            gfx_draw_string(88, 52, "16 kHz Mono (No SD)", COLOR_TEXT_DIM, COLOR_CARD_BG, 1);
        }

        // Digital Timer readout
        char time_str[16];
        snprintf(time_str, sizeof(time_str), "%02lu:%02lu.%lu", (unsigned long)mins, (unsigned long)secs, (unsigned long)sec_tenths);
        gfx_draw_string(240, 44, time_str, COLOR_YELLOW_ACCENT, COLOR_CARD_BG, 1);
    }

    // 4. Live VU meter differential updates
    int rms = (s_mode == MODE_LIVE_VOICE) ? s_live_rms : i2s_mic_get_latest_rms();
    if (s_mode == MODE_PLAYING) {
        rms = 0; // Mute VU during speaker playback
    }

    // Map RMS (0 - 4500) to 0 - 20 segments
    int lit_count = (rms * VU_SEGMENTS_COUNT) / 4000;
    if (lit_count > VU_SEGMENTS_COUNT) lit_count = VU_SEGMENTS_COUNT;
    if (lit_count < 0) lit_count = 0;

    // Decay peak hold
    if (lit_count >= s_decay_peak) {
        s_decay_peak = (float)lit_count;
    } else {
        s_decay_peak -= 0.25f;
        if (s_decay_peak < 0.0f) s_decay_peak = 0.0f;
    }
    int peak_seg = (int)s_decay_peak;

    if (lit_count != s_prev_lit_segments || peak_seg != s_prev_peak_segment) {
        for (int i = 0; i < VU_SEGMENTS_COUNT; i++) {
            int x = VU_START_X + i * (VU_SEG_W + VU_SEG_GAP);
            uint16_t color;

            if (i < lit_count) {
                if (i < 12) color = COLOR_GREEN_ACCENT;
                else if (i < 17) color = COLOR_YELLOW_ACCENT;
                else color = COLOR_RED_ACCENT;
            } else if (i == peak_seg && peak_seg > 0) {
                color = COLOR_WHITE; // Peak hold dot
            } else {
                color = 0x10A2; // Unlit slot
            }
            gfx_fill_rect(x, VU_START_Y, VU_SEG_W, VU_SEG_H, color);
        }
        s_prev_lit_segments = lit_count;
        s_prev_peak_segment = peak_seg;
    }

    // 5. Telemetry numbers row
    int peak_val = peak_seg * (4000 / VU_SEGMENTS_COUNT);
    if (s_last_drawn_rms != rms || s_last_drawn_peak != peak_val) {
        s_last_drawn_rms = rms;
        s_last_drawn_peak = peak_val;

        char telem[64];
        snprintf(telem, sizeof(telem), "RMS: %-5d   Peak: %-5d   Rate: 16kHz", rms, peak_val);
        gfx_draw_string(20, 148, telem, COLOR_CYAN_ACCENT, COLOR_CARD_BG, 1);
    }
}

bool app_recorder_handle_touch(int tx, int ty, AppState *next_state, bool *needs_redraw)
{
    // 1. Back button (Top Left)
    if (app_common_is_back_pressed(tx, ty)) {
        app_recorder_stop();
        if (next_state) *next_state = STATE_APP_MENU;
        if (needs_redraw) *needs_redraw = true;
        return true;
    }

    // 2. Action buttons area (Y: 175 to 235)
    if (ty >= 175 && ty <= 235) {
        // Button 1: Left [ REC / STOP REC ]
        if (tx >= 10 && tx <= 104) {
            audio_player_play_ui_click();
            if (s_mode == MODE_RECORDING) {
                // Stop recording
                s_rec_running = false;
                s_mode = MODE_IDLE;
                draw_action_buttons();
                i2s_mic_start_level_monitor(); // Resume monitor
            } else {
                // If live voice is running, stop it first
                if (s_mode == MODE_LIVE_VOICE) {
                    s_live_running = false;
                    for (int i = 0; i < 30 && s_live_task_handle != NULL; i++) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
                // If currently playing, stop playback first
                if (s_mode == MODE_PLAYING) {
                    audio_player_stop();
                }
                // Stop level monitor to give recording task exclusive access to mic
                i2s_mic_stop_level_monitor();

                s_mode = MODE_RECORDING;
                s_rec_running = true;
                s_rec_start_time_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
                xTaskCreatePinnedToCore(recorder_task, "recorder", 4096, NULL, 5, &s_rec_task_handle, 1);
                draw_action_buttons();
            }
            return true;
        }

        // Button 2: Center [ PLAY / STOP ]
        if (tx >= 113 && tx <= 207) {
            audio_player_play_ui_click();
            if (s_mode == MODE_PLAYING) {
                audio_player_stop();
                s_mode = MODE_IDLE;
                draw_action_buttons();
                i2s_mic_start_level_monitor();
            } else if (s_has_recording) {
                if (s_mode == MODE_LIVE_VOICE) {
                    s_live_running = false;
                    for (int i = 0; i < 30 && s_live_task_handle != NULL; i++) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
                if (s_mode == MODE_RECORDING) {
                    s_rec_running = false;
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                i2s_mic_stop_level_monitor();

                if (sdcard_is_mounted()) {
                    ESP_LOGI(TAG, "Playing recording from SD card: %s", RECORDER_FILEPATH);
                    audio_player_play_sound_effect(RECORDER_FILEPATH);
                } else if (s_ram_buffer && s_ram_buffer_bytes > 0) {
                    ESP_LOGI(TAG, "Playing recording from RAM buffer (%zu bytes)", s_ram_buffer_bytes);
                    i2s_audio_set_params(I2S_MIC_SAMPLE_RATE, 16, 1);
                    size_t written = 0;
                    i2s_audio_write(s_ram_buffer, s_ram_buffer_bytes, &written, 1000);
                }

                s_mode = MODE_PLAYING;
                draw_action_buttons();
            }
            return true;
        }

        // Button 3: Right [ LIVE VOICE TEST ]
        if (tx >= 216 && tx <= 310) {
            audio_player_play_ui_click();
            if (s_mode == MODE_LIVE_VOICE) {
                // Stop live voice
                s_live_running = false;
                for (int i = 0; i < 30 && s_live_task_handle != NULL; i++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                s_mode = MODE_IDLE;
                draw_action_buttons();
                i2s_mic_start_level_monitor();
            } else {
                // Stop recording if active
                if (s_mode == MODE_RECORDING) {
                    s_rec_running = false;
                    for (int i = 0; i < 30 && s_rec_task_handle != NULL; i++) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                }
                // Stop playback if active
                if (s_mode == MODE_PLAYING) {
                    audio_player_stop();
                }
                // Stop level monitor to give live voice exclusive access to mic
                i2s_mic_stop_level_monitor();

                s_mode = MODE_LIVE_VOICE;
                s_live_running = true;
                xTaskCreatePinnedToCore(live_voice_task, "live_voice", 4096, NULL, 5, &s_live_task_handle, 1);
                draw_action_buttons();
            }
            return true;
        }
    }

    return false;
}
