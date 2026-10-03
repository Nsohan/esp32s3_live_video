#include "http_stream.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "camera_driver.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "tts_service.h"
#include "battery_service.h"
#include "audio_player.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include "i2s_mic.h"
#include "wake_word_service.h"

static const char *TAG = "http_stream";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static volatile bool streaming_enabled = true;
static volatile bool s_client_streaming = false;
static volatile bool s_audio_streaming = false;

bool http_stream_is_active(void)
{
    return s_client_streaming;
}

bool http_stream_audio_is_active(void)
{
    return s_audio_streaming;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!camera_driver_is_initialized()) {
        esp_err_t cerr = camera_driver_init();
        if (cerr != ESP_OK) {
            ESP_LOGE(TAG, "Camera lazy init failed for stream: 0x%x", cerr);
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "503: Camera hardware unavailable or failed to initialize", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
    }

    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[160];
    int frame_count = 0;

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    // Set HTTP response headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    ESP_LOGI(TAG, "Stream handler started for client");
    s_client_streaming = true;

    while (streaming_enabled) {
        fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Send boundary and part header in one packet for maximum network throughput
        size_t hlen = snprintf(part_buf, sizeof(part_buf),
                               "\r\n--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned int)fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Client stream ended (code: %d)", res);
            break;
        }

        frame_count++;
        if (frame_count % 60 == 0) {
            ESP_LOGI(TAG, "Streamed %d frames smoothly", frame_count);
        }

        // Steady frame pacing (~20-25 FPS) so lwIP Wi-Fi buffers and TCP bandwidth stay clean and unchoked
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    s_client_streaming = false;
    ESP_LOGI(TAG, "Stream handler closed. Total frames: %d", frame_count);
    return res;
}

#define AUDIO_STREAM_CHUNK_SAMPLES 512

static esp_err_t audio_stream_handler(httpd_req_t *req)
{
    if (s_audio_streaming) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "503: Audio stream already in use by another client", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    i2s_mic_init();

    bool paused_ww = false;
    if (wake_word_service_is_running()) {
        wake_word_service_pause();
        paused_ww = true;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "X-Audio-Sample-Rate", "16000");

    ESP_LOGI(TAG, "Live mic audio stream started (INMP441 16kHz mono raw PCM)");
    s_audio_streaming = true;

    int16_t *chunk_buf = malloc(AUDIO_STREAM_CHUNK_SAMPLES * sizeof(int16_t));
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio chunk buffer");
        if (paused_ww) wake_word_service_resume();
        s_audio_streaming = false;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t res = ESP_OK;
    while (s_audio_streaming) {
        size_t samples_read = 0;
        esp_err_t rerr = i2s_mic_read(chunk_buf, AUDIO_STREAM_CHUNK_SAMPLES, &samples_read, 100);
        if (rerr != ESP_OK || samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        res = httpd_resp_send_chunk(req, (const char *)chunk_buf, samples_read * sizeof(int16_t));
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Audio client stream ended (code: %d)", res);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }

    free(chunk_buf);
    httpd_resp_send_chunk(req, NULL, 0);
    s_audio_streaming = false;

    if (paused_ww) {
        wake_word_service_resume();
    }

    ESP_LOGI(TAG, "Live mic audio stream closed");
    return res;
}

static void stream_async_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    stream_handler(req);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t stream_async_handler(httpd_req_t *req)
{
    if (s_client_streaming) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "503: Video stream already active", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start async video stream: %d", err);
        return err;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(stream_async_task, "stream_async", 4096, copy, 4, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream_async task");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void audio_async_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    audio_stream_handler(req);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t audio_async_handler(httpd_req_t *req)
{
    if (s_audio_streaming) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "503: Audio stream already active", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start async audio stream: %d", err);
        return err;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(audio_async_task, "audio_async", 4096, copy, 4, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio_async task");
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Control endpoint to start/stop streaming
static esp_err_t control_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_FAIL;
    }

    buf[ret] = '\0';

    if (strstr(buf, "\"action\":\"start\"")) {
        streaming_enabled = true;
        ESP_LOGI(TAG, "Streaming started by user");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"started\"}", strlen("{\"status\":\"started\"}"));
    }
    else if (strstr(buf, "\"action\":\"stop\"")) {
        streaming_enabled = false;
        ESP_LOGI(TAG, "Streaming stopped by user");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"stopped\"}", strlen("{\"status\":\"stopped\"}"));
    }
    else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid action");
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Capture single frame
static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!camera_driver_is_initialized()) {
        esp_err_t cerr = camera_driver_init();
        if (cerr != ESP_OK) {
            ESP_LOGE(TAG, "Camera lazy init failed for capture: 0x%x", cerr);
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "503: Camera hardware unavailable or failed to initialize", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to capture frame");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    return ESP_OK;
}

// Direct Raw PCM playback endpoint (/api/play_pcm?rate=16000)
static esp_err_t play_pcm_handler(httpd_req_t *req)
{
    char query[64];
    int rate = 16000;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char rstr[16];
        if (httpd_query_key_value(query, "rate", rstr, sizeof(rstr)) == ESP_OK) {
            rate = atoi(rstr);
            if (rate < 8000 || rate > 48000) rate = 16000;
        }
    }

    char buf[1024];
    int ret;
    while ((ret = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        tts_play_pcm(buf, (size_t)ret, (uint32_t)rate);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
}

// TTS Speech POST endpoint (supports engine="cloud"|"sam", lang="en"|"es"|"fr"|"ja"|"de", preset=0..6)
static esp_err_t speak_post_handler(httpd_req_t *req)
{
    char buf[350];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char speak_text[160] = {0};

    // Parse engine: cloud vs sam
    tts_mode_t mode = TTS_MODE_CLOUD_NATURAL;
    char *engine_key = strstr(buf, "\"engine\":");
    if (engine_key && strstr(engine_key, "\"sam\"")) {
        mode = TTS_MODE_OFFLINE_SAM;
    }

    // Parse language for cloud speech
    char lang[16] = "en";
    char *lang_key = strstr(buf, "\"lang\":");
    if (lang_key) {
        char *start = strchr(lang_key + 7, '\"');
        if (start) {
            start++;
            char *end = strchr(start, '\"');
            if (end && (end - start < (int)sizeof(lang))) {
                memcpy(lang, start, end - start);
                lang[end - start] = '\0';
            }
        }
    }

    // Parse preset for SAM offline synth
    int preset = 4; // Loona / Mascot default
    char *preset_key = strstr(buf, "\"preset\":");
    if (preset_key) {
        preset = atoi(preset_key + 9);
        tts_set_preset((tts_voice_preset_t)preset);
    }

    // Parse text from JSON: {"text":"..."}
    char *text_key = strstr(buf, "\"text\":");
    if (text_key) {
        char *start = strchr(text_key + 7, '\"');
        if (start) {
            start++;
            char *end = strchr(start, '\"');
            if (end) {
                size_t len = end - start;
                if (len >= sizeof(speak_text)) len = sizeof(speak_text) - 1;
                memcpy(speak_text, start, len);
                speak_text[len] = '\0';
            }
        }
    } else {
        // Plain text body fallback
        size_t len = strlen(buf);
        if (len >= sizeof(speak_text)) len = sizeof(speak_text) - 1;
        memcpy(speak_text, buf, len);
        speak_text[len] = '\0';
    }

    if (strlen(speak_text) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No speech text provided");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web TTS speak request: '%s' (mode=%s, lang=%s, preset=%d)",
             speak_text, (mode == TTS_MODE_CLOUD_NATURAL) ? "cloud" : "sam", lang, preset);
    tts_speak_mode(speak_text, mode, lang);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char *resp = "{\"status\":\"ok\",\"message\":\"Speech enqueued\"}";
    return httpd_resp_send(req, resp, strlen(resp));
}

// TTS Speech GET endpoint (?text=hello&engine=cloud|sam&lang=en)
static esp_err_t speak_get_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[160];
        if (httpd_query_key_value(query, "text", param, sizeof(param)) == ESP_OK) {
            for (char *p = param; *p; p++) {
                if (*p == '+') *p = ' ';
            }
            tts_mode_t mode = TTS_MODE_CLOUD_NATURAL;
            char eng[16];
            if (httpd_query_key_value(query, "engine", eng, sizeof(eng)) == ESP_OK && strcmp(eng, "sam") == 0) {
                mode = TTS_MODE_OFFLINE_SAM;
            }
            char lang[16] = "en";
            httpd_query_key_value(query, "lang", lang, sizeof(lang));

            ESP_LOGI(TAG, "Web GET TTS request: '%s' (%s, %s)", param, mode == TTS_MODE_CLOUD_NATURAL ? "cloud" : "sam", lang);
            tts_speak_mode(param, mode, lang);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            return httpd_resp_send(req, "{\"status\":\"ok\"}", strlen("{\"status\":\"ok\"}"));
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ?text= query param");
    return ESP_FAIL;
}

// Telemetry & Status API (/api/status)
static esp_err_t status_handler(httpd_req_t *req)
{
    char json[160];
    snprintf(json, sizeof(json),
             "{\"battery\":%d,\"charging\":%s,\"streaming\":%s,\"tts_busy\":%s}",
             battery_service_get_percentage(),
             battery_service_is_charging() ? "true" : "false",
             s_client_streaming ? "true" : "false",
             tts_is_busy() ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

// Audio Effects API (/api/fx?sound=happy|click|tone)
static esp_err_t fx_handler(httpd_req_t *req)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char sound[32];
        if (httpd_query_key_value(query, "sound", sound, sizeof(sound)) == ESP_OK) {
            if (strcmp(sound, "happy") == 0) {
                audio_player_play_happy_sound();
            } else if (strcmp(sound, "click") == 0) {
                audio_player_play_ui_click();
            } else if (strcmp(sound, "tone") == 0) {
                audio_player_play_test_tone();
            } else if (strcmp(sound, "curious") == 0) {
                audio_player_play_curious_sound();
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
}

// Deferred restart task so HTTP response finishes cleanly before chip reboots
static void ota_restart_task(void *pvParameter)
{
    ESP_LOGI(TAG, "OTA: System restarting in 1200ms...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

// Web Serial Monitor Circular Buffer & Hook
#define WEB_LOG_BUFFER_SIZE (24 * 1024)
static char s_web_log_buf[WEB_LOG_BUFFER_SIZE];
static uint32_t s_web_log_total_bytes = 0;
static SemaphoreHandle_t s_web_log_mutex = NULL;
static vprintf_like_t s_prev_vprintf = NULL;

static int web_log_vprintf(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int ret = 0;
    if (s_prev_vprintf) {
        ret = s_prev_vprintf(fmt, ap);
    } else {
        ret = vprintf(fmt, ap);
    }

    if (!s_web_log_mutex || xPortInIsrContext()) {
        va_end(ap_copy);
        return ret;
    }

    char tmp[384];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap_copy);
    va_end(ap_copy);

    if (len > 0) {
        if (len >= (int)sizeof(tmp)) {
            len = sizeof(tmp) - 1;
        }

        if (xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            uint32_t head = s_web_log_total_bytes;
            for (int i = 0; i < len; i++) {
                s_web_log_buf[(head + i) % WEB_LOG_BUFFER_SIZE] = tmp[i];
            }
            s_web_log_total_bytes += len;
            xSemaphoreGive(s_web_log_mutex);
        }
    }

    return ret;
}

void web_log_init(void)
{
    if (!s_web_log_mutex) {
        s_web_log_mutex = xSemaphoreCreateMutex();
        s_prev_vprintf = esp_log_set_vprintf(web_log_vprintf);
        ESP_LOGI(TAG, "Web Serial Monitor hook active (buffer: %d KB)", WEB_LOG_BUFFER_SIZE / 1024);
    }
}

// Web Serial Monitor logs API (/api/logs?since=N)
static esp_err_t logs_get_handler(httpd_req_t *req)
{
    uint32_t since = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[32];
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, NULL, 10);
        }
    }

    char head_str[32];
    uint32_t total = 0;
    uint32_t start_pos = 0;
    uint32_t count = 0;

    if (!s_web_log_mutex) {
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "X-Log-Head", "0");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "", 0);
    }

    if (xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "Busy", HTTPD_RESP_USE_STRLEN);
    }

    total = s_web_log_total_bytes;

    if (since == 0 || since < (total > WEB_LOG_BUFFER_SIZE ? total - WEB_LOG_BUFFER_SIZE : 0) || since > total) {
        uint32_t max_backlog = 8192;
        if (total < max_backlog) {
            start_pos = 0;
            count = total;
        } else {
            start_pos = total - max_backlog;
            count = max_backlog;
        }
    } else {
        start_pos = since;
        count = total - since;
    }

    char *out = NULL;
    if (count > 0) {
        out = malloc(count + 1);
        if (!out && count > 2048) {
            count = 2048;
            start_pos = total - count;
            out = malloc(count + 1);
        }
        if (out) {
            for (uint32_t i = 0; i < count; i++) {
                out[i] = s_web_log_buf[(start_pos + i) % WEB_LOG_BUFFER_SIZE];
            }
            out[count] = '\0';
        } else {
            count = 0;
        }
    }

    xSemaphoreGive(s_web_log_mutex);

    snprintf(head_str, sizeof(head_str), "%lu", (unsigned long)total);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "X-Log-Head", head_str);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    esp_err_t res = ESP_OK;
    if (out && count > 0) {
        res = httpd_resp_send(req, out, count);
        free(out);
    } else {
        res = httpd_resp_send(req, "", 0);
    }

    return res;
}

// Clear Web Serial Monitor log buffer (/api/logs/clear)
static esp_err_t logs_clear_handler(httpd_req_t *req)
{
    if (s_web_log_mutex && xSemaphoreTake(s_web_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_web_log_buf, 0, sizeof(s_web_log_buf));
        s_web_log_total_bytes = 0;
        xSemaphoreGive(s_web_log_mutex);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
}

// OTA Information endpoint (/api/ota_info)
static esp_err_t ota_info_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *app = esp_app_get_description();

    char resp[300];
    snprintf(resp, sizeof(resp),
             "{\"running\":\"%s\",\"target\":\"%s\",\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"idf\":\"%s\"}",
             running ? running->label : "factory",
             next ? next->label : "unknown",
             app ? app->version : "1.0.0",
             app ? app->date : "unknown",
             app ? app->time : "",
             app ? app->idf_ver : "");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, strlen(resp));
}

// OTA Firmware POST upload handler (/ota)
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting OTA update handler... Content-Length: %d", req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Error: Content-Length required", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No valid OTA update partition found!");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Error: No OTA partition found in partition table", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s, Writing OTA to: %s (addr: 0x%08lx, max_size: %lu)",
             running ? running->label : "factory",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    if ((size_t)req->content_len > update_partition->size) {
        ESP_LOGE(TAG, "Firmware binary (%d bytes) exceeds target partition (%lu bytes)",
                 req->content_len, (unsigned long)update_partition->size);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Error: Firmware size exceeds target partition capacity", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Stop active camera streaming to conserve memory and flash/DMA bus bandwidth
    streaming_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s (0x%x)", esp_err_to_name(err), err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Error: esp_ota_begin failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate 4KB OTA receive buffer");
        esp_ota_abort(ota_handle);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Error: Out of memory for OTA buffer", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    int received = 0;
    bool is_first = true;

    while (remaining > 0) {
        int to_read = (remaining < 4096) ? remaining : 4096;
        int ret = httpd_req_recv(req, buf, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            ESP_LOGE(TAG, "OTA socket receive failed: %d", ret);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "Error: Socket receive failed or disconnected", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        if (is_first) {
            // ESP32 app binary magic byte validation (must begin with 0xE9 / ESP_IMAGE_HEADER_MAGIC)
            if ((uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Invalid magic byte: 0x%02X (expected 0x%02X)", (uint8_t)buf[0], ESP_IMAGE_HEADER_MAGIC);
                free(buf);
                esp_ota_abort(ota_handle);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_send(req, "Error: Invalid image magic byte (not an ESP32 binary)", HTTPD_RESP_USE_STRLEN);
                return ESP_FAIL;
            }
            is_first = false;
        }

        err = esp_ota_write(ota_handle, (const void *)buf, ret);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write error: %s (0x%x)", esp_err_to_name(err), err);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_send(req, "Error: Flash write failed during OTA", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }

        received += ret;
        remaining -= ret;

        if (received % (128 * 1024) < ret || remaining == 0) {
            ESP_LOGI(TAG, "OTA Flashing: %d / %d bytes (%d%%)",
                     received, req->content_len, (received * 100) / req->content_len);
        }
    }

    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (0x%x)", esp_err_to_name(err), err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Error: Firmware verification failed", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s (0x%x)", esp_err_to_name(err), err);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Error: Failed to set boot partition", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA Succeeded! Partition '%s' configured for next boot. Restarting...", update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char *resp = "{\"status\":\"ok\",\"message\":\"Firmware flashed successfully! Rebooting...\"}";
    httpd_resp_send(req, resp, strlen(resp));

    // Schedule reboot after sending response
    xTaskCreate(&ota_restart_task, "ota_restart_task", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// Full Dashboard UI
static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "    <meta charset='utf-8'>"
        "    <title>PetBot Dashboard</title>"
        "    <meta name='viewport' content='width=device-width, initial-scale=1'>"
        "    <style>"
        "        * { margin: 0; padding: 0; box-sizing: border-box; }"
        "        body {"
        "            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;"
        "            background: #0b1120;"
        "            color: #f1f5f9;"
        "            min-height: 100vh;"
        "            padding: 20px 16px;"
        "            display: flex;"
        "            flex-direction: column;"
        "            align-items: center;"
        "        }"
        "        .dashboard {"
        "            width: 100%;"
        "            max-width: 720px;"
        "            display: flex;"
        "            flex-direction: column;"
        "            gap: 16px;"
        "        }"
        "        /* Top Bar */"
        "        .top-bar {"
        "            background: #1e293b;"
        "            border: 1px solid #334155;"
        "            border-radius: 16px;"
        "            padding: 16px 20px;"
        "            display: flex;"
        "            justify-content: space-between;"
        "            align-items: center;"
        "            flex-wrap: wrap;"
        "            gap: 12px;"
        "            box-shadow: 0 4px 20px rgba(0,0,0,0.3);"
        "        }"
        "        .brand {"
        "            display: flex;"
        "            align-items: center;"
        "            gap: 12px;"
        "        }"
        "        .brand-icon {"
        "            font-size: 28px;"
        "        }"
        "        .brand-text h1 {"
        "            font-size: 1.25rem;"
        "            font-weight: 700;"
        "            color: #38bdf8;"
        "            letter-spacing: -0.02em;"
        "        }"
        "        .brand-text p {"
        "            font-size: 0.78rem;"
        "            color: #94a3b8;"
        "        }"
        "        .telemetry {"
        "            display: flex;"
        "            gap: 8px;"
        "            align-items: center;"
        "            flex-wrap: wrap;"
        "        }"
        "        .pill {"
        "            font-size: 12px;"
        "            padding: 5px 12px;"
        "            border-radius: 20px;"
        "            background: #0f172a;"
        "            border: 1px solid #334155;"
        "            color: #cbd5e1;"
        "            display: inline-flex;"
        "            align-items: center;"
        "            gap: 6px;"
        "            font-weight: 500;"
        "        }"
        "        .pill.live {"
        "            background: #064e3b;"
        "            border-color: #059669;"
        "            color: #34d399;"
        "        }"
        "        /* Cards */"
        "        .card {"
        "            background: #1e293b;"
        "            border: 1px solid #334155;"
        "            border-radius: 16px;"
        "            overflow: hidden;"
        "            box-shadow: 0 8px 24px rgba(0,0,0,0.25);"
        "        }"
        "        .card-header {"
        "            padding: 14px 20px;"
        "            background: #0f172a;"
        "            border-bottom: 1px solid #334155;"
        "            display: flex;"
        "            justify-content: space-between;"
        "            align-items: center;"
        "        }"
        "        .card-title {"
        "            font-size: 0.98rem;"
        "            font-weight: 600;"
        "            color: #f8fafc;"
        "            display: flex;"
        "            align-items: center;"
        "            gap: 8px;"
        "        }"
        "        .badge {"
        "            font-size: 11px;"
        "            padding: 3px 10px;"
        "            border-radius: 12px;"
        "            font-weight: 600;"
        "        }"
        "        .badge-off { background: #334155; color: #94a3b8; }"
        "        .badge-live { background: #065f46; color: #34d399; border: 1px solid #059669; }"
        "        .badge-speaking { background: #7c2d12; color: #fb923c; border: 1px solid #ea580c; animation: pulse 1s infinite alternate; }"
        "        @keyframes pulse { from { opacity: 0.6; } to { opacity: 1; } }"
        "        /* Camera Area */"
        "        .screen-area {"
        "            background: #000;"
        "            position: relative;"
        "            min-height: 280px;"
        "            display: flex;"
        "            flex-direction: column;"
        "            align-items: center;"
        "            justify-content: center;"
        "        }"
        "        #stream-img {"
        "            width: 100%;"
        "            height: auto;"
        "            max-height: 480px;"
        "            object-fit: contain;"
        "            display: none;"
        "        }"
        "        .standby-box {"
        "            display: flex;"
        "            flex-direction: column;"
        "            align-items: center;"
        "            text-align: center;"
        "            padding: 40px 20px;"
        "            gap: 12px;"
        "        }"
        "        .lens-icon {"
        "            width: 68px;"
        "            height: 68px;"
        "            border-radius: 50%;"
        "            background: #1e293b;"
        "            border: 2px dashed #475569;"
        "            display: flex;"
        "            align-items: center;"
        "            justify-content: center;"
        "            font-size: 30px;"
        "            margin-bottom: 4px;"
        "            color: #94a3b8;"
        "        }"
        "        .standby-title { font-size: 1.15rem; font-weight: 600; color: #e2e8f0; }"
        "        .standby-desc { font-size: 0.85rem; color: #94a3b8; max-width: 380px; line-height: 1.4; }"
        "        .btn-launch {"
        "            margin-top: 6px;"
        "            padding: 12px 28px;"
        "            font-size: 15px;"
        "            font-weight: 700;"
        "            background: linear-gradient(135deg, #0284c7, #2563eb);"
        "            color: white;"
        "            border: none;"
        "            border-radius: 10px;"
        "            cursor: pointer;"
        "            box-shadow: 0 4px 14px rgba(37,99,235,0.4);"
        "            transition: all 0.2s ease;"
        "        }"
        "        .btn-launch:hover { transform: translateY(-2px); filter: brightness(1.15); }"
        "        .stream-actions {"
        "            padding: 12px 20px;"
        "            background: #1e293b;"
        "            display: flex;"
        "            justify-content: center;"
        "            gap: 12px;"
        "            flex-wrap: wrap;"
        "        }"
        "        button {"
        "            padding: 9px 18px;"
        "            font-size: 14px;"
        "            font-weight: 600;"
        "            border: none;"
        "            border-radius: 8px;"
        "            cursor: pointer;"
        "            transition: all 0.2s ease;"
        "        }"
        "        button:hover { filter: brightness(1.1); transform: translateY(-1px); }"
        "        .btn-stop { background: #ef4444; color: #ffffff; }"
        "        .btn-cap { background: #38bdf8; color: #082f49; }"
        "        .btn-listen { background: #10b981; color: #ffffff; }\n"
        "        .btn-listen.listening { background: #f43f5e; color: #ffffff; }\n"
        "        .audio-listen-bar {\n"
        "            display: none;\n"
        "            width: 100%;\n"
        "            background: #0f172a;\n"
        "            padding: 10px 14px;\n"
        "            border-radius: 8px;\n"
        "            border: 1px solid #334155;\n"
        "            margin-top: 8px;\n"
        "            align-items: center;\n"
        "            justify-content: space-between;\n"
        "            gap: 10px;\n"
        "            flex-wrap: wrap;\n"
        "        }\n"
        "        .audio-listen-meta {\n"
        "            display: flex;\n"
        "            align-items: center;\n"
        "            gap: 8px;\n"
        "            font-size: 13px;\n"
        "            color: #e2e8f0;\n"
        "        }\n"
        "        .vu-bar-wrap {\n"
        "            flex: 1;\n"
        "            min-width: 90px;\n"
        "            height: 10px;\n"
        "            background: #1e293b;\n"
        "            border-radius: 5px;\n"
        "            overflow: hidden;\n"
        "            border: 1px solid #475569;\n"
        "        }\n"
        "        .vu-bar-fill {\n"
        "            height: 100%;\n"
        "            width: 0%;\n"
        "            background: linear-gradient(90deg, #10b981 60%, #eab308 85%, #ef4444 100%);\n"
        "            transition: width 0.05s ease;\n"
        "        }\n"
        "        .vol-ctrl {\n"
        "            display: flex;\n"
        "            align-items: center;\n"
        "            gap: 6px;\n"
        "            font-size: 12px;\n"
        "            color: #94a3b8;\n"
        "        }\n"
        "        .vol-slider {\n"
        "            width: 75px;\n"
        "            cursor: pointer;\n"
        "        }\n"
        "        /* Voice Section */\n"
        "        .voice-body {\n"
        "            padding: 18px 20px;\n"
        "            display: flex;\n"
        "            flex-direction: column;\n"
        "            gap: 14px;\n"
        "        }\n"
        "        .vtabs {\n"
        "            display: flex;\n"
        "            gap: 6px;\n"
        "            background: #0f172a;\n"
        "            padding: 4px;\n"
        "            border-radius: 10px;\n"
        "            border: 1px solid #334155;\n"
        "        }\n"
        "        .vtab {\n"
        "            flex: 1;\n"
        "            padding: 8px 10px;\n"
        "            font-size: 12px;\n"
        "            font-weight: 600;\n"
        "            border-radius: 6px;\n"
        "            background: transparent;\n"
        "            color: #94a3b8;\n"
        "            border: none;\n"
        "            cursor: pointer;\n"
        "            transition: all 0.2s ease;\n"
        "            text-align: center;\n"
        "        }\n"
        "        .vtab.active {\n"
        "            background: #2563eb;\n"
        "            color: #ffffff;\n"
        "            box-shadow: 0 2px 8px rgba(37,99,235,0.4);\n"
        "        }\n"
        "        .input-row {\n"
        "            display: flex;\n"
        "            gap: 8px;\n"
        "            align-items: center;\n"
        "            flex-wrap: wrap;\n"
        "        }\n"
        "        .text-wrap { flex: 1; min-width: 200px; }\n"
        "        #tts-input {\n"
        "            width: 100%;\n"
        "            padding: 10px 14px;\n"
        "            background: #0f172a;\n"
        "            border: 1px solid #475569;\n"
        "            border-radius: 8px;\n"
        "            color: #f8fafc;\n"
        "            font-size: 14px;\n"
        "            outline: none;\n"
        "        }\n"
        "        #tts-input:focus { border-color: #38bdf8; box-shadow: 0 0 0 2px rgba(56,189,248,0.25); }\n"
        "        .voice-sel {\n"
        "            padding: 10px 10px;\n"
        "            background: #0f172a;\n"
        "            border: 1px solid #475569;\n"
        "            border-radius: 8px;\n"
        "            color: #e2e8f0;\n"
        "            font-size: 13px;\n"
        "            outline: none;\n"
        "            cursor: pointer;\n"
        "        }\n"
        "        .btn-speak {\n"
        "            background: linear-gradient(135deg, #0284c7, #2563eb);\n"
        "            color: #fff;\n"
        "            padding: 10px 22px;\n"
        "            white-space: nowrap;\n"
        "        }\n"
        "        .btn-speak:disabled { opacity: 0.5; cursor: not-allowed; }\n"
        "        .mic-box {\n"
        "            display: none;\n"
        "            flex-direction: column;\n"
        "            align-items: center;\n"
        "            gap: 12px;\n"
        "            padding: 10px 0;\n"
        "        }\n"
        "        .btn-ptt {\n"
        "            width: 100%;\n"
        "            max-width: 320px;\n"
        "            padding: 16px 24px;\n"
        "            font-size: 15px;\n"
        "            font-weight: 700;\n"
        "            border-radius: 12px;\n"
        "            background: linear-gradient(135deg, #0284c7, #2563eb);\n"
        "            color: white;\n"
        "            cursor: pointer;\n"
        "            border: none;\n"
        "            box-shadow: 0 4px 14px rgba(37,99,235,0.35);\n"
        "            user-select: none;\n"
        "            touch-action: none;\n"
        "            transition: all 0.2s ease;\n"
        "        }\n"
        "        .btn-ptt.active {\n"
        "            background: linear-gradient(135deg, #dc2626, #b91c1c);\n"
        "            box-shadow: 0 0 20px rgba(220,38,38,0.7);\n"
        "            animation: pttPulse 0.8s infinite alternate;\n"
        "        }\n"
        "        @keyframes pttPulse { from { transform: scale(0.98); } to { transform: scale(1.02); } }\n"
        "        .mic-note { font-size: 12px; color: #94a3b8; text-align: center; }\n"
        "        .chips {"
        "            display: flex;"
        "            align-items: center;"
        "            gap: 6px;"
        "            flex-wrap: wrap;"
        "        }"
        "        .chips-title { font-size: 12px; color: #94a3b8; }"
        "        .chip {"
        "            padding: 4px 10px;"
        "            background: #334155;"
        "            color: #cbd5e1;"
        "            border: 1px solid #475569;"
        "            border-radius: 14px;"
        "            font-size: 12px;"
        "            font-weight: 500;"
        "            cursor: pointer;"
        "        }"
        "        .chip:hover { background: #475569; color: #fff; transform: translateY(-1px); }"
        "        /* Sounds Row */"
        "        .fx-row {"
        "            padding: 14px 20px;"
        "            display: flex;"
        "            gap: 10px;"
        "            align-items: center;"
        "            flex-wrap: wrap;"
        "        }"
        "        .btn-fx {"
        "            background: #334155;"
        "            color: #e2e8f0;"
        "            font-size: 13px;"
        "            border: 1px solid #475569;"
        "        }"
        "        /* OTA Firmware Card */"
        "        .ota-body { padding: 16px 20px; display: flex; flex-direction: column; gap: 12px; }"
        "        .ota-info-row { display: flex; gap: 14px; font-size: 13px; color: #94a3b8; flex-wrap: wrap; background: #0f172a; padding: 10px 14px; border-radius: 8px; border: 1px solid #334155; align-items: center; }"
        "        .ota-upload-row { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; }"
        "        .ota-file-name { font-size: 13px; color: #cbd5e1; flex: 1; min-width: 140px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }"
        "        .btn-file { background: #334155; color: #f1f5f9; padding: 10px 16px; border-radius: 8px; border: 1px solid #475569; font-size: 13px; font-weight: 600; cursor: pointer; transition: background 0.2s; }"
        "        .btn-file:hover { background: #475569; }"
        "        .btn-ota { background: linear-gradient(135deg, #10b981, #059669); color: white; padding: 10px 22px; border-radius: 8px; border: none; font-size: 13px; font-weight: 700; cursor: pointer; box-shadow: 0 4px 12px rgba(16,185,129,0.3); transition: opacity 0.2s; }"
        "        .btn-ota:disabled { opacity: 0.4; cursor: not-allowed; }"
        "        .progress-bar-bg { width: 100%; height: 10px; background: #0f172a; border-radius: 5px; overflow: hidden; border: 1px solid #334155; margin-top: 4px; }"
        "        .progress-bar-fill { height: 100%; background: linear-gradient(90deg, #38bdf8, #10b981); transition: width 0.2s ease; }"
        "        .ota-status-text { font-size: 12px; color: #38bdf8; margin-top: 4px; text-align: center; font-weight: 500; }\n"
        "        /* Serial Monitor Card */\n"
        "        .term-header-actions { display: flex; gap: 8px; align-items: center; }\n"
        "        .term-controls { display: flex; gap: 8px; padding: 10px 16px; background: #0f172a; border-bottom: 1px solid #1e293b; align-items: center; flex-wrap: wrap; }\n"
        "        .term-input { background: #1e293b; border: 1px solid #334155; color: #f1f5f9; padding: 5px 10px; border-radius: 6px; font-size: 12px; flex: 1; min-width: 140px; outline: none; }\n"
        "        .term-btn { background: #1e293b; border: 1px solid #334155; color: #cbd5e1; padding: 5px 12px; border-radius: 6px; font-size: 12px; font-weight: 500; cursor: pointer; transition: all 0.15s; display: inline-flex; align-items: center; gap: 4px; }\n"
        "        .term-btn:hover { background: #334155; color: #fff; }\n"
        "        .term-check { font-size: 12px; color: #94a3b8; display: inline-flex; align-items: center; gap: 5px; cursor: pointer; user-select: none; }\n"
        "        .term-box { background: #050914; height: 320px; overflow-y: auto; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; font-size: 11.5px; line-height: 1.45; padding: 12px 14px; color: #e2e8f0; white-space: pre-wrap; word-break: break-all; user-select: text; border-top: 1px solid #1e293b; }\n"
        "    </style>"
        "</head>"
        "<body>"
        "    <div class='dashboard'>"
        "        <!-- Top Telemetry Bar -->"
        "        <div class='top-bar'>"
        "            <div class='brand'>"
        "                <div class='brand-icon'>🤖</div>"
        "                <div class='brand-text'>"
        "                    <h1>PetBot Command Center</h1>"
        "                    <p>ESP32-S3 Live Telemetry & Control</p>"
        "                </div>"
        "            </div>"
        "            <div class='telemetry'>"
        "                <span class='pill' id='tele-bat'>🔋 <span id='bat-val'>--</span>%</span>"
        "                <span class='pill live'>⚡ Online</span>"
        "            </div>"
        "        </div>"
        "        "
        "        <!-- Live Camera Card -->"
        "        <div class='card'>"
        "            <div class='card-header'>"
        "                <div class='card-title'>📹 Live Vision Camera</div>"
        "                <span id='stream-badge' class='badge badge-off'>Standby ⚪</span>"
        "            </div>"
        "            <div class='screen-area'>"
        "                <!-- Standby Placeholder (Camera OFF) -->"
        "                <div id='standby-box' class='standby-box'>"
        "                    <div class='lens-icon'>📷</div>"
        "                    <div class='standby-title'>Live Camera is Standby</div>"
        "                    <div class='standby-desc'>Camera is disconnected to save battery & Wi-Fi bandwidth. Click below to start streaming.</div>"
        "                    <button id='btn-launch' class='btn-launch' onclick='window.startLiveStream()'>▶ Launch Live Camera</button>\n"
        "                </div>\n"
        "                <!-- Real Image Element (Only active when clicked) -->\n"
        "                <img id='stream-img' alt='PetBot Video Stream'>\n"
        "            </div>\n"
        "            <div class='stream-actions' id='stream-controls' style='display: none;'>\n"
        "                <div style='display: flex; gap: 10px; justify-content: center; width: 100%; flex-wrap: wrap;'>\n"
        "                    <button id='btn-stop' class='btn-stop' onclick='window.stopLiveStream()'>⏹ Stop Stream</button>\n"
        "                    <button id='btn-cap' class='btn-cap' onclick='window.captureSnapshot()'>📸 Snapshot</button>\n"
        "                    <button id='btn-listen' class='btn-listen' onclick='window.toggleAudioListening()'>🔊 Listen Mic</button>\n"
        "                </div>\n"
        "                <div id='audio-listen-bar' class='audio-listen-bar'>\n"
        "                    <div class='audio-listen-meta'>\n"
        "                        <span>🟢</span>\n"
        "                        <span><b>PetBot Mic:</b> 16kHz Live</span>\n"
        "                    </div>\n"
        "                    <div class='vu-bar-wrap' title='INMP441 Real-time Mic Audio Level'>\n"
        "                        <div id='vu-fill' class='vu-bar-fill'></div>\n"
        "                    </div>\n"
        "                    <div class='vol-ctrl'>\n"
        "                        <span>🔈</span>\n"
        "                        <input type='range' class='vol-slider' id='audio-volume' min='0' max='200' value='100' oninput='window.setAudioVolume(this.value)'>\n"
        "                        <span id='audio-vol-text'>100%</span>\n"
        "                    </div>\n"
        "                </div>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Voice Synthesizer Card -->\n"
        "        <div class='card'>\n"
        "            <div class='card-header'>\n"
        "                <div class='card-title'>🗣️ PetBot Speaker Voice</div>\n"
        "                <span id='tts-badge' class='badge badge-off'>Ready</span>\n"
        "            </div>\n"
        "            <div class='voice-body'>\n"
        "                <!-- Voice Engine Tabs -->\n"
        "                <div class='vtabs'>\n"
        "                    <button class='vtab active' id='tab-cloud' onclick='window.switchVoiceEngine(\"cloud\")'>☁️ Cloud Natural</button>\n"
        "                    <button class='vtab' id='tab-sam' onclick='window.switchVoiceEngine(\"sam\")'>🤖 Offline SAM</button>\n"
        "                    <button class='vtab' id='tab-mic' onclick='window.switchVoiceEngine(\"mic\")'>🎙️ Walkie-Talkie</button>\n"
        "                </div>\n"
        "                <!-- Text Speak Panel -->\n"
        "                <div id='text-speak-panel' class='input-row'>\n"
        "                    <div class='text-wrap'>\n"
        "                        <input type='text' id='tts-input' placeholder='Type something for PetBot to say...' maxlength='100' autocomplete='off'>\n"
        "                    </div>\n"
        "                    <!-- Cloud Language Select -->\n"
        "                    <select id='cloud-lang-select' class='voice-sel'>\n"
        "                        <option value='en-female' selected>🇺🇸 US English (Female)</option>\n"
        "                        <option value='en-male'>🇺🇸 US English (Male)</option>\n"
        "                        <option value='bn-female'>🇧🇩 বাংলা Bangla (Female)</option>\n"
        "                        <option value='bn-male'>🇧🇩 বাংলা Bangla (Male)</option>\n"
        "                        <option value='en-gb'>🇬🇧 UK English</option>\n"
        "                        <option value='es'>🇪🇸 Spanish</option>\n"
        "                        <option value='fr'>🇫🇷 French</option>\n"
        "                        <option value='ja'>🇯🇵 Japanese</option>\n"
        "                    </select>\n"
        "                    <!-- SAM Voice Select -->\n"
        "                    <select id='voice-select' class='voice-sel' style='display: none;'>\n"
        "                        <option value='4' selected>🦊 Loona / Mascot</option>\n"
        "                        <option value='5'>👨 Human Male</option>\n"
        "                        <option value='6'>👩 Human Female</option>\n"
        "                        <option value='0'>🤖 Classic Robot</option>\n"
        "                        <option value='1'>🐶 Cute Pet</option>\n"
        "                        <option value='2'>🦾 Deep Bot</option>\n"
        "                        <option value='3'>🧚 Elf Voice</option>\n"
        "                    </select>\n"
        "                    <button class='btn-speak' id='btn-speak' onclick='window.sendTTS()'>🔊 Speak</button>\n"
        "                </div>\n"
        "                <!-- Walkie-Talkie Push-To-Talk Panel -->\n"
        "                <div id='mic-panel' class='mic-box'>\n"
        "                    <button id='btn-ptt' class='btn-ptt'>🎙️ Hold to Speak</button>\n"
        "                    <div class='mic-note'>Hold to speak into your mic. Release to broadcast on PetBot speaker!</div>\n"
        "                </div>\n"
        "                <div class='chips'>\n"
        "                    <span class='chips-title'>Quick:</span>\n"
        "                    <button class='chip' onclick='window.quickSay(\"N? Are?\")'>🦊 N? Are?</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"What is that?\")'>🤔 What is that?</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"Hello master\")'>👋 Hello</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"হ্যালো কেমন আছেন\")'>👋 হ্যালো</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"আমি পেটবট\")'>🤖 আমি পেটবট</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"কেমন আছো বন্ধু\")'>🐶 কেমন আছো?</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"Warning intruder detected\")'>🚨 Warning</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"Feed me please\")'>🍖 Feed me</button>\n"
        "                </div>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Pet Sounds & Triggers Card -->\n"
        "        <div class='card'>\n"
        "            <div class='card-header'>\n"
        "                <div class='card-title'>🎵 Robot Sounds & Audio FX</div>\n"
        "            </div>\n"
        "            <div class='fx-row'>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"curious\")'>🦊 Curious Tilt (N? Are?)</button>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"happy\")'>✨ Happy Chime</button>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"click\")'>🔔 Button Click</button>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"tone\")'>🔊 1000Hz Test Tone</button>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Wireless Firmware OTA Card -->\n"
        "        <div class='card'>\n"
        "            <div class='card-header'>\n"
        "                <div class='card-title'>🚀 Wireless Firmware Update (OTA)</div>\n"
        "                <span id='ota-badge' class='badge badge-off'>Ready</span>\n"
        "            </div>\n"
        "            <div class='ota-body'>\n"
        "                <div class='ota-info-row'>\n"
        "                    <span>Active Slot: <b id='ota-part' style='color:#38bdf8'>Checking...</b></span>\n"
        "                    <span>Target: <b id='ota-target' style='color:#34d399'>--</b></span>\n"
        "                    <span>Version: <b id='ota-ver' style='color:#f1f5f9'>--</b></span>\n"
        "                    <span>Built: <b id='ota-date' style='color:#94a3b8'>--</b></span>\n"
        "                </div>\n"
        "                <div class='ota-upload-row'>\n"
        "                    <input type='file' id='ota-file' accept='.bin' style='display:none'>\n"
        "                    <button class='btn-file' type='button' onclick='document.getElementById(\"ota-file\").click()'>📁 Select .bin File</button>\n"
        "                    <span id='ota-file-name' class='ota-file-name'>No file selected</span>\n"
        "                    <button class='btn-ota' id='btn-flash-ota' type='button' onclick='window.uploadOTA()' disabled>⚡ Flash Firmware</button>\n"
        "                </div>\n"
        "                <div id='ota-progress-container' style='display:none;'>\n"
        "                    <div class='progress-bar-bg'>\n"
        "                        <div id='ota-progress-fill' class='progress-bar-fill' style='width:0%'></div>\n"
        "                    </div>\n"
        "                    <div id='ota-status-text' class='ota-status-text'>Uploading: 0%</div>\n"
        "                </div>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Live Serial Monitor Card -->\n"
        "        <div class='card'>\n"
        "            <div class='card-header'>\n"
        "                <div class='card-title'>🖥️ Live Serial Monitor & System Logs</div>\n"
        "                <div class='term-header-actions'>\n"
        "                    <span id='term-badge' class='badge badge-live'>Live 🟢</span>\n"
        "                </div>\n"
        "            </div>\n"
        "            <div class='term-controls'>\n"
        "                <input type='text' id='term-filter' class='term-input' placeholder='Filter logs (e.g. roboeyes, ota, tts, error)...'>\n"
        "                <select id='term-level' class='voice-sel' style='width:auto;padding:5px 8px;font-size:12px;'>\n"
        "                    <option value='all' selected>All Levels</option>\n"
        "                    <option value='error'>Errors (E)</option>\n"
        "                    <option value='warn'>Warnings (W)</option>\n"
        "                    <option value='info'>Info (I)</option>\n"
        "                </select>\n"
        "                <label class='term-check'><input type='checkbox' id='term-autoscroll' checked> Auto-scroll</label>\n"
        "                <button class='term-btn' id='term-btn-pause' onclick='window.toggleLogPause()'>⏸️ Pause</button>\n"
        "                <button class='term-btn' onclick='window.clearLogDisplay()'>🗑️ Clear</button>\n"
        "                <button class='term-btn' onclick='window.exportLogs()'>💾 Export</button>\n"
        "            </div>\n"
        "            <div id='term-box' class='term-box'>Connecting to live serial log stream...</div>\n"
        "        </div>\n"
        "    </div>\n"
        "    <script>\n"
        "        console.log('PetBot: Initializing dashboard...');\n"
        "        var streamImg = document.getElementById('stream-img');\n"
        "        var standbyBox = document.getElementById('standby-box');\n"
        "        var streamControls = document.getElementById('stream-controls');\n"
        "        var streamBadge = document.getElementById('stream-badge');\n"
        "        var isStreaming = false;\n"
        "        window.activeVoiceEngine = 'cloud';\n"
        "        \n"
        "        window.switchVoiceEngine = function(engine) {\n"
        "            window.activeVoiceEngine = engine;\n"
        "            document.getElementById('tab-cloud').className = 'vtab' + (engine === 'cloud' ? ' active' : '');\n"
        "            document.getElementById('tab-sam').className = 'vtab' + (engine === 'sam' ? ' active' : '');\n"
        "            document.getElementById('tab-mic').className = 'vtab' + (engine === 'mic' ? ' active' : '');\n"
        "            var cloudSel = document.getElementById('cloud-lang-select');\n"
        "            var samSel = document.getElementById('voice-select');\n"
        "            var textPnl = document.getElementById('text-speak-panel');\n"
        "            var micPnl = document.getElementById('mic-panel');\n"
        "            if (cloudSel) cloudSel.style.display = (engine === 'cloud') ? 'block' : 'none';\n"
        "            if (samSel) samSel.style.display = (engine === 'sam') ? 'block' : 'none';\n"
        "            if (textPnl) textPnl.style.display = (engine === 'mic') ? 'none' : 'flex';\n"
        "            if (micPnl) micPnl.style.display = (engine === 'mic') ? 'flex' : 'none';\n"
        "        };\n"
        "        \n"
        "        window.startLiveStream = function() {\n"
        "            console.log('PetBot: Launching stream...');\n"
        "            streamImg.src = '/stream?' + Date.now();\n"
        "            streamImg.style.display = 'block';\n"
        "            standbyBox.style.display = 'none';\n"
        "            streamControls.style.display = 'flex';\n"
        "            streamBadge.className = 'badge badge-live';\n"
        "            streamBadge.innerHTML = 'Live Feed 🟢';\n"
        "            isStreaming = true;\n"
        "        };\n"
        "        \n"
        "        window.stopLiveStream = function() {\n"
        "            console.log('PetBot: Stopping stream...');\n"
        "            if (window.stopAudioListening) window.stopAudioListening();\n"
        "            streamImg.onerror = null;\n"
        "            streamImg.src = '';\n"
        "            streamImg.style.display = 'none';\n"
        "            standbyBox.style.display = 'flex';\n"
        "            streamControls.style.display = 'none';\n"
        "            streamBadge.className = 'badge badge-off';\n"
        "            streamBadge.innerHTML = 'Standby ⚪';\n"
        "            isStreaming = false;\n"
        "        };\n"
        "        \n"
        "        var audioCtx = null;\n"
        "        var audioGain = null;\n"
        "        var audioReader = null;\n"
        "        var isAudioListening = false;\n"
        "        var pcmRemainder = null;\n"
        "        var nextPlayTime = 0;\n"
        "        \n"
        "        window.toggleAudioListening = function() {\n"
        "            if (isAudioListening) {\n"
        "                window.stopAudioListening();\n"
        "            } else {\n"
        "                window.startAudioListening();\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.setAudioVolume = function(val) {\n"
        "            var v = parseInt(val, 10);\n"
        "            var textEl = document.getElementById('audio-vol-text');\n"
        "            if (textEl) textEl.innerText = v + '%';\n"
        "            if (audioGain && audioCtx) {\n"
        "                audioGain.gain.setValueAtTime(v / 100.0, audioCtx.currentTime);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.startAudioListening = async function() {\n"
        "            if (isAudioListening) return;\n"
        "            var btnListen = document.getElementById('btn-listen');\n"
        "            var audioBar = document.getElementById('audio-listen-bar');\n"
        "            try {\n"
        "                var AudioContextClass = window.AudioContext || window.webkitAudioContext;\n"
        "                if (!audioCtx || audioCtx.state === 'closed') {\n"
        "                    audioCtx = new AudioContextClass();\n"
        "                }\n"
        "                if (audioCtx.state === 'suspended') {\n"
        "                    await audioCtx.resume();\n"
        "                }\n"
        "                \n"
        "                if (btnListen) {\n"
        "                    btnListen.innerHTML = '⏳ Connecting...';\n"
        "                    btnListen.disabled = true;\n"
        "                }\n"
        "                \n"
        "                var resp = await fetch('/audio_stream?' + Date.now());\n"
        "                if (!resp.ok) {\n"
        "                    throw new Error('Server returned HTTP ' + resp.status);\n"
        "                }\n"
        "                \n"
        "                pcmRemainder = null;\n"
        "                nextPlayTime = 0;\n"
        "                isAudioListening = true;\n"
        "                audioReader = resp.body.getReader();\n"
        "                \n"
        "                if (!audioGain) {\n"
        "                    audioGain = audioCtx.createGain();\n"
        "                    var volSlider = document.getElementById('audio-volume');\n"
        "                    var initVol = volSlider ? parseInt(volSlider.value, 10) / 100.0 : 1.0;\n"
        "                    audioGain.gain.setValueAtTime(initVol, audioCtx.currentTime);\n"
        "                    audioGain.connect(audioCtx.destination);\n"
        "                }\n"
        "                \n"
        "                if (btnListen) {\n"
        "                    btnListen.disabled = false;\n"
        "                    btnListen.className = 'btn-listen listening';\n"
        "                    btnListen.innerHTML = '🔇 Mute Mic';\n"
        "                }\n"
        "                if (audioBar) audioBar.style.display = 'flex';\n"
        "                \n"
        "                window.runAudioReader();\n"
        "            } catch(e) {\n"
        "                console.error('Audio listen error:', e);\n"
        "                window.stopAudioListening();\n"
        "                alert('Could not start live mic audio: ' + e.message);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.runAudioReader = async function() {\n"
        "            var vuFill = document.getElementById('vu-fill');\n"
        "            while (isAudioListening && audioReader) {\n"
        "                try {\n"
        "                    var chunk = await audioReader.read();\n"
        "                    if (chunk.done) break;\n"
        "                    var u8 = chunk.value;\n"
        "                    if (!u8 || u8.byteLength === 0) continue;\n"
        "                    \n"
        "                    var combined;\n"
        "                    if (pcmRemainder && pcmRemainder.byteLength > 0) {\n"
        "                        combined = new Uint8Array(pcmRemainder.byteLength + u8.byteLength);\n"
        "                        combined.set(pcmRemainder, 0);\n"
        "                        combined.set(u8, pcmRemainder.byteLength);\n"
        "                        pcmRemainder = null;\n"
        "                    } else {\n"
        "                        combined = u8;\n"
        "                    }\n"
        "                    \n"
        "                    var usableBytes = combined.byteLength - (combined.byteLength % 2);\n"
        "                    if (usableBytes < combined.byteLength) {\n"
        "                        pcmRemainder = combined.slice(usableBytes);\n"
        "                    }\n"
        "                    if (usableBytes === 0) continue;\n"
        "                    \n"
        "                    var numSamples = usableBytes / 2;\n"
        "                    var float32 = new Float32Array(numSamples);\n"
        "                    var view = new DataView(combined.buffer, combined.byteOffset, usableBytes);\n"
        "                    var sumSq = 0;\n"
        "                    for (var s = 0; s < numSamples; s++) {\n"
        "                        var v = view.getInt16(s * 2, true) / 32768.0;\n"
        "                        float32[s] = v;\n"
        "                        sumSq += v * v;\n"
        "                    }\n"
        "                    \n"
        "                    if (vuFill && numSamples > 0) {\n"
        "                        var rms = Math.sqrt(sumSq / numSamples);\n"
        "                        var pct = Math.min(100, Math.round(rms * 400));\n"
        "                        vuFill.style.width = pct + '%';\n"
        "                    }\n"
        "                    \n"
        "                    if (audioCtx && isAudioListening) {\n"
        "                        var audioBuffer = audioCtx.createBuffer(1, numSamples, 16000);\n"
        "                        audioBuffer.copyToChannel(float32, 0);\n"
        "                        \n"
        "                        var source = audioCtx.createBufferSource();\n"
        "                        source.buffer = audioBuffer;\n"
        "                        source.connect(audioGain);\n"
        "                        \n"
        "                        var curTime = audioCtx.currentTime;\n"
        "                        if (nextPlayTime < curTime) {\n"
        "                            nextPlayTime = curTime + 0.05;\n"
        "                        }\n"
        "                        source.start(nextPlayTime);\n"
        "                        nextPlayTime += audioBuffer.duration;\n"
        "                    }\n"
        "                } catch(err) {\n"
        "                    console.warn('Audio reader stream ended:', err);\n"
        "                    break;\n"
        "                }\n"
        "            }\n"
        "            window.stopAudioListening();\n"
        "        };\n"
        "        \n"
        "        window.stopAudioListening = function() {\n"
        "            isAudioListening = false;\n"
        "            if (audioReader) {\n"
        "                try { audioReader.cancel(); } catch(e) {}\n"
        "                audioReader = null;\n"
        "            }\n"
        "            pcmRemainder = null;\n"
        "            nextPlayTime = 0;\n"
        "            var btnListen = document.getElementById('btn-listen');\n"
        "            if (btnListen) {\n"
        "                btnListen.disabled = false;\n"
        "                btnListen.className = 'btn-listen';\n"
        "                btnListen.innerHTML = '🔊 Listen Mic';\n"
        "            }\n"
        "            var audioBar = document.getElementById('audio-listen-bar');\n"
        "            if (audioBar) audioBar.style.display = 'none';\n"
        "            var vuFill = document.getElementById('vu-fill');\n"
        "            if (vuFill) vuFill.style.width = '0%';\n"
        "        };\n"
        "        \n"
        "        window.captureSnapshot = function() {\n"
        "            try {\n"
        "                var a = document.createElement('a');\n"
        "                a.href = '/capture?' + Date.now();\n"
        "                a.download = 'petbot_' + Date.now() + '.jpg';\n"
        "                document.body.appendChild(a);\n"
        "                a.click();\n"
        "                document.body.removeChild(a);\n"
        "            } catch(e) {\n"
        "                alert('Snapshot failed: ' + e);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.sendTTS = async function(textOverride) {\n"
        "            var input = document.getElementById('tts-input');\n"
        "            var text = textOverride || (input ? input.value.trim() : '');\n"
        "            if (!text) { if (input) input.focus(); return; }\n"
        "            var engine = window.activeVoiceEngine || 'cloud';\n"
        "            if (engine === 'mic') engine = 'cloud';\n"
        "            var langSel = document.getElementById('cloud-lang-select');\n"
        "            var voiceSel = document.getElementById('voice-select');\n"
        "            var lang = langSel ? langSel.value : 'en';\n"
        "            var preset = voiceSel ? parseInt(voiceSel.value) : 4;\n"
        "            var btn = document.getElementById('btn-speak');\n"
        "            var badge = document.getElementById('tts-badge');\n"
        "            if (btn) btn.disabled = true;\n"
        "            if (badge) {\n"
        "                badge.className = 'badge badge-speaking';\n"
        "                badge.innerHTML = 'Speaking... 🔊';\n"
        "            }\n"
        "            try {\n"
        "                console.log('PetBot: Sending ' + engine + ' speech ->', text);\n"
        "                var resp = await fetch('/speak', {\n"
        "                    method: 'POST',\n"
        "                    headers: {'Content-Type': 'application/json'},\n"
        "                    body: JSON.stringify({text: text, engine: engine, lang: lang, preset: preset})\n"
        "                });\n"
        "                if (resp.ok && !textOverride && input) input.value = '';\n"
        "            } catch(e) {\n"
        "                console.error('TTS error:', e);\n"
        "                alert('Connection error to PetBot');\n"
        "            } finally {\n"
        "                setTimeout(function() {\n"
        "                    if (btn) btn.disabled = false;\n"
        "                    if (badge) {\n"
        "                        badge.className = 'badge badge-off';\n"
        "                        badge.innerHTML = 'Ready';\n"
        "                    }\n"
        "                }, 1500);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.quickSay = function(phrase) {\n"
        "            var input = document.getElementById('tts-input');\n"
        "            if (input) input.value = phrase;\n"
        "            window.sendTTS(phrase);\n"
        "        };\n"
        "        \n"
        "        window.triggerSound = async function(fx) {\n"
        "            try {\n"
        "                console.log('PetBot: Triggering sound ->', fx);\n"
        "                await fetch('/api/fx?sound=' + fx);\n"
        "            } catch(e) {\n"
        "                console.error('FX error:', e);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        var isPTT = false;\n"
        "        var pttStream = null;\n"
        "        var pttAudioCtx = null;\n"
        "        var pttPcmChunks = [];\n"
        "        \n"
        "        window.startPTT = async function(e) {\n"
        "            if (e) e.preventDefault();\n"
        "            if (isPTT) return;\n"
        "            try {\n"
        "                if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {\n"
        "                    alert('Microphone access requires HTTPS or localhost');\n"
        "                    return;\n"
        "                }\n"
        "                pttStream = await navigator.mediaDevices.getUserMedia({ audio: true });\n"
        "                pttAudioCtx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });\n"
        "                var source = pttAudioCtx.createMediaStreamSource(pttStream);\n"
        "                var proc = pttAudioCtx.createScriptProcessor(2048, 1, 1);\n"
        "                pttPcmChunks = [];\n"
        "                proc.onaudioprocess = function(ev) {\n"
        "                    if (!isPTT) return;\n"
        "                    var ch = ev.inputBuffer.getChannelData(0);\n"
        "                    var arr = new Int16Array(ch.length);\n"
        "                    for (var i = 0; i < ch.length; i++) {\n"
        "                        var s = Math.max(-1, Math.min(1, ch[i]));\n"
        "                        arr[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;\n"
        "                    }\n"
        "                    pttPcmChunks.push(arr);\n"
        "                };\n"
        "                source.connect(proc);\n"
        "                proc.connect(pttAudioCtx.destination);\n"
        "                window._pttSource = source;\n"
        "                window._pttProc = proc;\n"
        "                isPTT = true;\n"
        "                if (audioGain && audioCtx) audioGain.gain.setValueAtTime(0, audioCtx.currentTime);\n"
        "                var pttBtn = document.getElementById('btn-ptt');\n"
        "                if (pttBtn) { pttBtn.className = 'btn-ptt active'; pttBtn.innerText = '🔴 Transmitting Live...'; }\n"
        "                var badge = document.getElementById('tts-badge');\n"
        "                if (badge) { badge.className = 'badge badge-speaking'; badge.innerHTML = 'Mic Active 🎙️'; }\n"
        "            } catch(err) {\n"
        "                console.error('PTT Error:', err);\n"
        "                alert('Microphone error: ' + err.message);\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.stopPTT = async function(e) {\n"
        "            if (e) e.preventDefault();\n"
        "            if (!isPTT) return;\n"
        "            isPTT = false;\n"
        "            if (audioGain && audioCtx) {\n"
        "                var volSlider = document.getElementById('audio-volume');\n"
        "                var v = volSlider ? parseInt(volSlider.value, 10) / 100.0 : 1.0;\n"
        "                audioGain.gain.setValueAtTime(v, audioCtx.currentTime);\n"
        "            }\n"
        "            var pttBtn = document.getElementById('btn-ptt');\n"
        "            if (pttBtn) { pttBtn.className = 'btn-ptt'; pttBtn.innerText = '🎙️ Hold to Speak'; }\n"
        "            var badge = document.getElementById('tts-badge');\n"
        "            if (badge) { badge.className = 'badge badge-off'; badge.innerHTML = 'Ready'; }\n"
        "            if (window._pttSource) window._pttSource.disconnect();\n"
        "            if (window._pttProc) window._pttProc.disconnect();\n"
        "            if (pttStream) pttStream.getTracks().forEach(function(t) { t.stop(); });\n"
        "            if (pttAudioCtx) pttAudioCtx.close();\n"
        "            var total = 0;\n"
        "            for (var i = 0; i < pttPcmChunks.length; i++) total += pttPcmChunks[i].length;\n"
        "            if (total > 0) {\n"
        "                var merged = new Int16Array(total);\n"
        "                var off = 0;\n"
        "                for (var j = 0; j < pttPcmChunks.length; j++) {\n"
        "                    merged.set(pttPcmChunks[j], off);\n"
        "                    off += pttPcmChunks[j].length;\n"
        "                }\n"
        "                try {\n"
        "                    console.log('Sending PCM audio -> ' + merged.byteLength + ' bytes');\n"
        "                    await fetch('/api/play_pcm?rate=16000', {\n"
        "                        method: 'POST',\n"
        "                        headers: { 'Content-Type': 'application/octet-stream' },\n"
        "                        body: merged.buffer\n"
        "                    });\n"
        "                } catch(err) { console.error('Play PCM error:', err); }\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        var btnPtt = document.getElementById('btn-ptt');\n"
        "        if (btnPtt) {\n"
        "            btnPtt.addEventListener('mousedown', window.startPTT);\n"
        "            btnPtt.addEventListener('mouseup', window.stopPTT);\n"
        "            btnPtt.addEventListener('mouseleave', window.stopPTT);\n"
        "            btnPtt.addEventListener('touchstart', window.startPTT, { passive: false });\n"
        "            btnPtt.addEventListener('touchend', window.stopPTT, { passive: false });\n"
        "        }\n"
        "        \n"
        "        var ttsIn = document.getElementById('tts-input');\n"
        "        if (ttsIn) {\n"
        "            ttsIn.addEventListener('keydown', function(e) {\n"
        "                if (e.key === 'Enter') window.sendTTS();\n"
        "            });\n"
        "        }\n"
        "        \n"
        "        var btnLaunch = document.getElementById('btn-launch');\n"
        "        if (btnLaunch) btnLaunch.addEventListener('click', window.startLiveStream);\n"
        "        var btnStop = document.getElementById('btn-stop');\n"
        "        if (btnStop) btnStop.addEventListener('click', window.stopLiveStream);\n"
        "        var btnCap = document.getElementById('btn-cap');\n"
        "        if (btnCap) btnCap.addEventListener('click', window.captureSnapshot);\n"
        "        var btnSpeak = document.getElementById('btn-speak');\n"
        "        if (btnSpeak) btnSpeak.addEventListener('click', function() { window.sendTTS(); });\n"
        "        \n"
        "        window.updateTelemetry = async function() {\n"
        "            try {\n"
        "                var r = await fetch('/api/status');\n"
        "                if (r.ok) {\n"
        "                    var data = await r.json();\n"
        "                    var batEl = document.getElementById('bat-val');\n"
        "                    if (batEl && data.battery !== undefined) batEl.innerText = data.battery;\n"
        "                }\n"
        "            } catch(e) {}\n"
        "        };\n"
        "        setInterval(window.updateTelemetry, 4000);\n"
        "        window.updateTelemetry();\n"
        "        \n"
        "        var otaFileIn = document.getElementById('ota-file');\n"
        "        var otaFileName = document.getElementById('ota-file-name');\n"
        "        var btnFlashOta = document.getElementById('btn-flash-ota');\n"
        "        var otaSelectedFile = null;\n"
        "        if (otaFileIn) {\n"
        "            otaFileIn.addEventListener('change', function(e) {\n"
        "                if (e.target.files && e.target.files.length > 0) {\n"
        "                    otaSelectedFile = e.target.files[0];\n"
        "                    otaFileName.innerText = otaSelectedFile.name + ' (' + (otaSelectedFile.size / 1024 / 1024).toFixed(2) + ' MB)';\n"
        "                    btnFlashOta.disabled = false;\n"
        "                } else {\n"
        "                    otaSelectedFile = null;\n"
        "                    otaFileName.innerText = 'No file selected';\n"
        "                    btnFlashOta.disabled = true;\n"
        "                }\n"
        "            });\n"
        "        }\n"
        "        window.uploadOTA = function() {\n"
        "            if (!otaSelectedFile) return;\n"
        "            if (!confirm('Flash firmware ' + otaSelectedFile.name + ' over Wi-Fi?\\n\\nThe robot will automatically reboot when completed.')) return;\n"
        "            var progContainer = document.getElementById('ota-progress-container');\n"
        "            var progFill = document.getElementById('ota-progress-fill');\n"
        "            var statusText = document.getElementById('ota-status-text');\n"
        "            var otaBadge = document.getElementById('ota-badge');\n"
        "            progContainer.style.display = 'block';\n"
        "            btnFlashOta.disabled = true;\n"
        "            otaBadge.className = 'badge badge-speaking';\n"
        "            otaBadge.innerText = 'Flashing...';\n"
        "            var xhr = new XMLHttpRequest();\n"
        "            xhr.open('POST', '/ota', true);\n"
        "            xhr.setRequestHeader('Content-Type', 'application/octet-stream');\n"
        "            xhr.upload.onprogress = function(e) {\n"
        "                if (e.lengthComputable) {\n"
        "                    var pct = Math.round((e.loaded / e.total) * 100);\n"
        "                    progFill.style.width = pct + '%';\n"
        "                    statusText.innerText = 'Uploading & Flashing: ' + pct + '% (' + (e.loaded / 1024 / 1024).toFixed(2) + ' / ' + (e.total / 1024 / 1024).toFixed(2) + ' MB)';\n"
        "                }\n"
        "            };\n"
        "            xhr.onload = function() {\n"
        "                if (xhr.status >= 200 && xhr.status < 300) {\n"
        "                    progFill.style.width = '100%';\n"
        "                    statusText.innerHTML = '✅ <b>Flash Successful! PetBot is rebooting now...</b>';\n"
        "                    statusText.style.color = '#34d399';\n"
        "                    otaBadge.className = 'badge badge-live';\n"
        "                    otaBadge.innerText = 'Rebooting';\n"
        "                    var count = 8;\n"
        "                    var t = setInterval(function() {\n"
        "                        statusText.innerHTML = '✅ <b>Rebooting... Refreshing in ' + count + 's</b>';\n"
        "                        count--;\n"
        "                        if (count < 0) {\n"
        "                            clearInterval(t);\n"
        "                            window.location.reload();\n"
        "                        }\n"
        "                    }, 1000);\n"
        "                } else {\n"
        "                    statusText.innerHTML = '❌ Flash Failed: ' + (xhr.responseText || 'HTTP Error ' + xhr.status);\n"
        "                    statusText.style.color = '#f87171';\n"
        "                    otaBadge.className = 'badge badge-off';\n"
        "                    otaBadge.innerText = 'Failed';\n"
        "                    btnFlashOta.disabled = false;\n"
        "                }\n"
        "            };\n"
        "            xhr.onerror = function() {\n"
        "                statusText.innerHTML = '❌ Network connection error during OTA upload';\n"
        "                statusText.style.color = '#f87171';\n"
        "                btnFlashOta.disabled = false;\n"
        "            };\n"
        "            xhr.send(otaSelectedFile);\n"
        "        };\n"
        "        window.loadOTAInfo = async function() {\n"
        "            try {\n"
        "                var r = await fetch('/api/ota_info');\n"
        "                if (r.ok) {\n"
        "                    var data = await r.json();\n"
        "                    var partEl = document.getElementById('ota-part');\n"
        "                    var tgtEl = document.getElementById('ota-target');\n"
        "                    var verEl = document.getElementById('ota-ver');\n"
        "                    var dateEl = document.getElementById('ota-date');\n"
        "                    if (partEl && data.running) partEl.innerText = data.running;\n"
        "                    if (tgtEl && data.target) tgtEl.innerText = data.target;\n"
        "                    if (verEl && data.version) verEl.innerText = data.version;\n"
        "                    if (dateEl && data.date) dateEl.innerText = data.date + (data.time ? ' ' + data.time : '');\n"
        "                }\n"
        "            } catch(e) {}\n"
        "        };\n"
        "        window.loadOTAInfo();\n"
        "        \n"
        "        // Serial Monitor Terminal Logic\n"
        "        var logHead = 0;\n"
        "        var logPaused = false;\n"
        "        var logAutoScroll = true;\n"
        "        var logAllLines = [];\n"
        "        var termBox = document.getElementById('term-box');\n"
        "        var termFilter = document.getElementById('term-filter');\n"
        "        var termLevel = document.getElementById('term-level');\n"
        "        var termAutoScroll = document.getElementById('term-autoscroll');\n"
        "        var termBadge = document.getElementById('term-badge');\n"
        "        var termBtnPause = document.getElementById('term-btn-pause');\n"
        "        \n"
        "        function ansiToHtml(str) {\n"
        "            var esc = str.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');\n"
        "            return esc\n"
        "                .replace(/\\033\\[0;31m/g, '<span style=\"color:#f87171;font-weight:600;\">')\n"
        "                .replace(/\\033\\[1;31m/g, '<span style=\"color:#ef4444;font-weight:bold;\">')\n"
        "                .replace(/\\033\\[0;32m/g, '<span style=\"color:#34d399;font-weight:600;\">')\n"
        "                .replace(/\\033\\[0;33m/g, '<span style=\"color:#fbbf24;font-weight:600;\">')\n"
        "                .replace(/\\033\\[0;34m/g, '<span style=\"color:#60a5fa;\">')\n"
        "                .replace(/\\033\\[0;35m/g, '<span style=\"color:#c084fc;\">')\n"
        "                .replace(/\\033\\[0;36m/g, '<span style=\"color:#38bdf8;\">')\n"
        "                .replace(/\\033\\[0;37m/g, '<span style=\"color:#94a3b8;\">')\n"
        "                .replace(/\\033\\[0m/g, '</span>')\n"
        "                .replace(/\\033\\[[0-9;]*m/g, '');\n"
        "        }\n"
        "        \n"
        "        window.toggleLogPause = function() {\n"
        "            logPaused = !logPaused;\n"
        "            if (termBtnPause) termBtnPause.innerText = logPaused ? '▶️ Resume' : '⏸️ Pause';\n"
        "            if (termBadge) {\n"
        "                termBadge.className = logPaused ? 'badge badge-off' : 'badge badge-live';\n"
        "                termBadge.innerText = logPaused ? 'Paused ⏸️' : 'Live 🟢';\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.clearLogDisplay = function() {\n"
        "            logAllLines = [];\n"
        "            if (termBox) termBox.innerHTML = '<span style=\"color:#64748b;\">[Terminal Cleared]</span>\\n';\n"
        "            fetch('/api/logs/clear', { method: 'POST' }).catch(function(){});\n"
        "        };\n"
        "        \n"
        "        window.exportLogs = function() {\n"
        "            var rawText = logAllLines.join('\\n').replace(/\\033\\[[0-9;]*m/g, '');\n"
        "            var blob = new Blob([rawText], { type: 'text/plain' });\n"
        "            var a = document.createElement('a');\n"
        "            a.href = URL.createObjectURL(blob);\n"
        "            a.download = 'petbot_serial_' + Date.now() + '.txt';\n"
        "            a.click();\n"
        "        };\n"
        "        \n"
        "        function filterLine(line, kw, lvl) {\n"
        "            if (lvl === 'error' && !line.includes('E (') && !line.includes('[E]') && !line.toLowerCase().includes('error')) return false;\n"
        "            if (lvl === 'warn' && !line.includes('W (') && !line.includes('[W]') && !line.toLowerCase().includes('warn')) return false;\n"
        "            if (lvl === 'info' && !line.includes('I (') && !line.includes('[I]')) return false;\n"
        "            if (kw && !line.toLowerCase().includes(kw)) return false;\n"
        "            return true;\n"
        "        }\n"
        "        \n"
        "        window.renderLogs = function() {\n"
        "            if (!termBox) return;\n"
        "            var kw = termFilter ? termFilter.value.trim().toLowerCase() : '';\n"
        "            var lvl = termLevel ? termLevel.value : 'all';\n"
        "            var filtered = [];\n"
        "            for (var i = 0; i < logAllLines.length; i++) {\n"
        "                if (filterLine(logAllLines[i], kw, lvl)) {\n"
        "                    filtered.push(ansiToHtml(logAllLines[i]));\n"
        "                }\n"
        "            }\n"
        "            termBox.innerHTML = filtered.length > 0 ? filtered.join('\\n') : '<span style=\"color:#64748b;\">[No matching logs]</span>';\n"
        "            if (logAutoScroll) termBox.scrollTop = termBox.scrollHeight;\n"
        "        };\n"
        "        \n"
        "        if (termFilter) termFilter.addEventListener('input', window.renderLogs);\n"
        "        if (termLevel) termLevel.addEventListener('change', window.renderLogs);\n"
        "        if (termAutoScroll) {\n"
        "            termAutoScroll.addEventListener('change', function(e) {\n"
        "                logAutoScroll = e.target.checked;\n"
        "                if (logAutoScroll && termBox) termBox.scrollTop = termBox.scrollHeight;\n"
        "            });\n"
        "        }\n"
        "        if (termBox) {\n"
        "            termBox.addEventListener('scroll', function() {\n"
        "                var atBottom = termBox.scrollTop + termBox.clientHeight >= termBox.scrollHeight - 25;\n"
        "                if (!atBottom && logAutoScroll) {\n"
        "                    logAutoScroll = false;\n"
        "                    if (termAutoScroll) termAutoScroll.checked = false;\n"
        "                }\n"
        "            });\n"
        "        }\n"
        "        \n"
        "        var partialLine = '';\n"
        "        window.appendLogChunk = function(text) {\n"
        "            var full = partialLine + text;\n"
        "            var lines = full.split('\\n');\n"
        "            partialLine = lines.pop();\n"
        "            if (lines.length === 0) return;\n"
        "            \n"
        "            var kw = termFilter ? termFilter.value.trim().toLowerCase() : '';\n"
        "            var lvl = termLevel ? termLevel.value : 'all';\n"
        "            \n"
        "            for (var i = 0; i < lines.length; i++) {\n"
        "                var l = lines[i];\n"
        "                logAllLines.push(l);\n"
        "                if (logAllLines.length > 1200) logAllLines.shift();\n"
        "                if (filterLine(l, kw, lvl)) {\n"
        "                    var el = document.createElement('div');\n"
        "                    el.innerHTML = ansiToHtml(l);\n"
        "                    if (termBox.innerText.indexOf('Connecting to live serial') !== -1) termBox.innerHTML = '';\n"
        "                    termBox.appendChild(el);\n"
        "                    if (termBox.childNodes.length > 600) termBox.removeChild(termBox.firstChild);\n"
        "                }\n"
        "            }\n"
        "            if (logAutoScroll && termBox) {\n"
        "                termBox.scrollTop = termBox.scrollHeight;\n"
        "            }\n"
        "        };\n"
        "        \n"
        "        window.pollLogs = async function() {\n"
        "            if (!logPaused) {\n"
        "                try {\n"
        "                    var r = await fetch('/api/logs?since=' + logHead);\n"
        "                    if (r.ok) {\n"
        "                        var h = r.headers.get('X-Log-Head');\n"
        "                        if (h !== null) logHead = parseInt(h, 10);\n"
        "                        var txt = await r.text();\n"
        "                        if (txt.length > 0) {\n"
        "                            window.appendLogChunk(txt);\n"
        "                        }\n"
        "                    }\n"
        "                } catch(e) {}\n"
        "            }\n"
        "            setTimeout(window.pollLogs, 750);\n"
        "        };\n"
        "        window.pollLogs();\n"
        "        console.log('PetBot: Dashboard fully loaded & operational!');\n"
        "    </script>\n"
        "</body>\n"
        "</html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

void start_camera_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 10240;
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    config.send_wait_timeout = 10;
    config.recv_wait_timeout = 10;

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t index_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &index_uri);

        httpd_uri_t favicon_uri = {
            .uri = "/favicon.ico",
            .method = HTTP_GET,
            .handler = favicon_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &favicon_uri);

        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_async_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);

        httpd_uri_t audio_stream_uri = {
            .uri = "/audio_stream",
            .method = HTTP_GET,
            .handler = audio_async_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &audio_stream_uri);

        httpd_uri_t control_uri = {
            .uri = "/control",
            .method = HTTP_POST,
            .handler = control_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &control_uri);

        httpd_uri_t capture_uri = {
            .uri = "/capture",
            .method = HTTP_GET,
            .handler = capture_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &capture_uri);

        httpd_uri_t speak_post_uri = {
            .uri = "/speak",
            .method = HTTP_POST,
            .handler = speak_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &speak_post_uri);

        httpd_uri_t speak_get_uri = {
            .uri = "/speak",
            .method = HTTP_GET,
            .handler = speak_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &speak_get_uri);

        httpd_uri_t play_pcm_uri = {
            .uri = "/api/play_pcm",
            .method = HTTP_POST,
            .handler = play_pcm_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &play_pcm_uri);

        httpd_uri_t status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);

        httpd_uri_t fx_uri = {
            .uri = "/api/fx",
            .method = HTTP_GET,
            .handler = fx_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &fx_uri);

        httpd_uri_t ota_post_uri = {
            .uri = "/ota",
            .method = HTTP_POST,
            .handler = ota_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_post_uri);

        httpd_uri_t ota_info_uri = {
            .uri = "/api/ota_info",
            .method = HTTP_GET,
            .handler = ota_info_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_info_uri);

        httpd_uri_t logs_get_uri = {
            .uri = "/api/logs",
            .method = HTTP_GET,
            .handler = logs_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &logs_get_uri);

        httpd_uri_t logs_clear_uri = {
            .uri = "/api/logs/clear",
            .method = HTTP_POST,
            .handler = logs_clear_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &logs_clear_uri);

        // Confirm running app state and cancel rollback if enabled
        esp_ota_mark_app_valid_cancel_rollback();

        // Ensure web log hook is active
        web_log_init();

        ESP_LOGI(TAG, "PetBot Dashboard server started on port 80 (OTA & Web Serial ready)");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}
