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

static const char *TAG = "http_stream";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static volatile bool streaming_enabled = true;
static volatile bool s_client_streaming = false;

bool http_stream_is_active(void)
{
    return s_client_streaming;
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

        // Minimal yield to keep socket and FreeRTOS tasks responsive
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_client_streaming = false;
    ESP_LOGI(TAG, "Stream handler closed. Total frames: %d", frame_count);
    return res;
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

// TTS Speech POST endpoint
static esp_err_t speak_post_handler(httpd_req_t *req)
{
    char buf[300];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char speak_text[160] = {0};

    // Parse preset
    int preset = 0;
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

    ESP_LOGI(TAG, "Web TTS speak request: '%s' (preset %d)", speak_text, preset);
    tts_speak(speak_text);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const char *resp = "{\"status\":\"ok\",\"message\":\"Speech enqueued\"}";
    return httpd_resp_send(req, resp, strlen(resp));
}

// TTS Speech GET endpoint (?text=hello)
static esp_err_t speak_get_handler(httpd_req_t *req)
{
    char query[256];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param[160];
        if (httpd_query_key_value(query, "text", param, sizeof(param)) == ESP_OK) {
            for (char *p = param; *p; p++) {
                if (*p == '+') *p = ' ';
            }
            ESP_LOGI(TAG, "Web GET TTS request: '%s'", param);
            tts_speak(param);
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
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"status\":\"ok\"}", 15);
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
        "        .btn-stop { background: #ef4444; color: #450a0a; }"
        "        .btn-cap { background: #38bdf8; color: #082f49; }"
        "        /* Voice Section */"
        "        .voice-body {"
        "            padding: 18px 20px;"
        "            display: flex;"
        "            flex-direction: column;"
        "            gap: 14px;"
        "        }"
        "        .input-row {"
        "            display: flex;"
        "            gap: 8px;"
        "            align-items: center;"
        "            flex-wrap: wrap;"
        "        }"
        "        .text-wrap { flex: 1; min-width: 200px; }"
        "        #tts-input {"
        "            width: 100%;"
        "            padding: 10px 14px;"
        "            background: #0f172a;"
        "            border: 1px solid #475569;"
        "            border-radius: 8px;"
        "            color: #f8fafc;"
        "            font-size: 14px;"
        "            outline: none;"
        "        }"
        "        #tts-input:focus { border-color: #38bdf8; box-shadow: 0 0 0 2px rgba(56,189,248,0.25); }"
        "        #voice-select {"
        "            padding: 10px 10px;"
        "            background: #0f172a;"
        "            border: 1px solid #475569;"
        "            border-radius: 8px;"
        "            color: #e2e8f0;"
        "            font-size: 13px;"
        "            outline: none;"
        "            cursor: pointer;"
        "        }"
        "        .btn-speak {"
        "            background: linear-gradient(135deg, #0284c7, #2563eb);"
        "            color: #fff;"
        "            padding: 10px 22px;"
        "            white-space: nowrap;"
        "        }"
        "        .btn-speak:disabled { opacity: 0.5; cursor: not-allowed; }"
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
        "                <button id='btn-stop' class='btn-stop' onclick='window.stopLiveStream()'>⏹ Stop Stream</button>\n"
        "                <button id='btn-cap' class='btn-cap' onclick='window.captureSnapshot()'>📸 Capture Snapshot</button>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <!-- Voice Synthesizer Card -->\n"
        "        <div class='card'>\n"
        "            <div class='card-header'>\n"
        "                <div class='card-title'>🗣️ PetBot Speaker Voice (Offline TTS)</div>\n"
        "                <span id='tts-badge' class='badge badge-off'>Ready</span>\n"
        "            </div>\n"
        "            <div class='voice-body'>\n"
        "                <div class='input-row'>\n"
        "                    <div class='text-wrap'>\n"
        "                        <input type='text' id='tts-input' placeholder='Type something for PetBot to say...' maxlength='100' autocomplete='off'>\n"
        "                    </div>\n"
        "                    <select id='voice-select'>\n"
        "                        <option value='0'>🤖 Classic Robot</option>\n"
        "                        <option value='1'>🐶 Cute Pet</option>\n"
        "                        <option value='2'>🦾 Deep Bot</option>\n"
        "                        <option value='3'>🧚 Elf Voice</option>\n"
        "                    </select>\n"
        "                    <button class='btn-speak' id='btn-speak' onclick='window.sendTTS()'>🔊 Speak</button>\n"
        "                </div>\n"
        "                <div class='chips'>\n"
        "                    <span class='chips-title'>Quick:</span>\n"
        "                    <button class='chip' onclick='window.quickSay(\"Hello master\")'>👋 Hello</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"Good boy\")'>🐶 Good boy</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"I am PetBot\")'>🤖 PetBot</button>\n"
        "                    <button class='chip' onclick='window.quickSay(\"I see you\")'>👀 I see you</button>\n"
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
        "                <button class='btn-fx' onclick='window.triggerSound(\"happy\")'>✨ Happy Chime</button>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"click\")'>🔔 Button Click</button>\n"
        "                <button class='btn-fx' onclick='window.triggerSound(\"tone\")'>🔊 1000Hz Test Tone</button>\n"
        "            </div>\n"
        "        </div>\n"
        "    </div>\n"
        "    <script>\n"
        "        console.log('PetBot: Initializing dashboard...');\n"
        "        var streamImg = document.getElementById('stream-img');\n"
        "        var standbyBox = document.getElementById('standby-box');\n"
        "        var streamControls = document.getElementById('stream-controls');\n"
        "        var streamBadge = document.getElementById('stream-badge');\n"
        "        var isStreaming = false;\n"
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
        "            streamImg.src = '';\n"
        "            streamImg.style.display = 'none';\n"
        "            standbyBox.style.display = 'flex';\n"
        "            streamControls.style.display = 'none';\n"
        "            streamBadge.className = 'badge badge-off';\n"
        "            streamBadge.innerHTML = 'Standby ⚪';\n"
        "            isStreaming = false;\n"
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
        "            var select = document.getElementById('voice-select');\n"
        "            var preset = select ? parseInt(select.value) : 0;\n"
        "            var btn = document.getElementById('btn-speak');\n"
        "            var badge = document.getElementById('tts-badge');\n"
        "            if (btn) btn.disabled = true;\n"
        "            if (badge) {\n"
        "                badge.className = 'badge badge-speaking';\n"
        "                badge.innerHTML = 'Speaking... 🔊';\n"
        "            }\n"
        "            try {\n"
        "                console.log('PetBot: Sending TTS ->', text);\n"
        "                var resp = await fetch('/speak', {\n"
        "                    method: 'POST',\n"
        "                    headers: {'Content-Type': 'application/json'},\n"
        "                    body: JSON.stringify({text: text, preset: preset})\n"
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
        "        console.log('PetBot: Dashboard fully loaded & operational!');\n"
        "    </script>\n"
        "</body>\n"
        "</html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, strlen(html));
}

void start_camera_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 10240;
    config.max_uri_handlers = 14;
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

        httpd_uri_t stream_uri = {
            .uri = "/stream",
            .method = HTTP_GET,
            .handler = stream_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stream_uri);

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

        ESP_LOGI(TAG, "PetBot Dashboard server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}
