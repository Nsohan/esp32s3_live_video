#include "http_stream.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "http_stream";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static volatile bool streaming_enabled = true;

static esp_err_t stream_handler(httpd_req_t *req)
{
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

// Main UI with Start/Stop buttons
static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "    <title>PetBot Camera Control</title>"
        "    <meta charset='utf-8'>"
        "    <meta name='viewport' content='width=device-width, initial-scale=1'>"
        "    <style>"
        "        * { margin: 0; padding: 0; box-sizing: border-box; }"
        "        body {"
        "            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;"
        "            background: #0f172a;"
        "            color: #f8fafc;"
        "            min-height: 100vh;"
        "            display: flex;"
        "            flex-direction: column;"
        "            align-items: center;"
        "            padding: 20px;"
        "        }"
        "        .container {"
        "            width: 100%;"
        "            max-width: 680px;"
        "            background: #1e293b;"
        "            border: 1px solid #334155;"
        "            border-radius: 16px;"
        "            box-shadow: 0 20px 40px rgba(0,0,0,0.5);"
        "            overflow: hidden;"
        "        }"
        "        .header {"
        "            background: #0f172a;"
        "            padding: 20px;"
        "            text-align: center;"
        "            border-bottom: 1px solid #334155;"
        "        }"
        "        .header h1 { font-size: 1.5em; color: #38bdf8; margin-bottom: 4px; }"
        "        .header p { color: #94a3b8; font-size: 0.9em; }"
        "        .video-container {"
        "            background: #000;"
        "            position: relative;"
        "            min-height: 320px;"
        "            display: flex;"
        "            align-items: center;"
        "            justify-content: center;"
        "        }"
        "        #stream {"
        "            width: 100%;"
        "            height: auto;"
        "            max-height: 480px;"
        "            object-fit: contain;"
        "            display: block;"
        "        }"
        "        #placeholder {"
        "            display: none;"
        "            color: #94a3b8;"
        "            text-align: center;"
        "            padding: 40px;"
        "        }"
        "        .controls {"
        "            padding: 20px;"
        "            background: #1e293b;"
        "            display: flex;"
        "            gap: 12px;"
        "            justify-content: center;"
        "            flex-wrap: wrap;"
        "        }"
        "        button {"
        "            padding: 10px 24px;"
        "            font-size: 15px;"
        "            font-weight: 600;"
        "            border: none;"
        "            border-radius: 8px;"
        "            cursor: pointer;"
        "            transition: all 0.2s ease;"
        "        }"
        "        button:hover { transform: translateY(-1px); filter: brightness(1.1); }"
        "        .btn-start { background: #22c55e; color: #052e16; }"
        "        .btn-stop { background: #ef4444; color: #450a0a; }"
        "        .btn-capture { background: #38bdf8; color: #082f49; }"
        "        .status {"
        "            padding: 12px 20px;"
        "            background: #0f172a;"
        "            display: flex;"
        "            justify-content: space-between;"
        "            align-items: center;"
        "            font-size: 14px;"
        "            border-top: 1px solid #334155;"
        "        }"
        "        .status-badge {"
        "            padding: 4px 12px;"
        "            border-radius: 12px;"
        "            font-weight: bold;"
        "        }"
        "        .status-badge.streaming { background: #22c55e22; color: #4ade80; border: 1px solid #22c55e44; }"
        "        .status-badge.stopped { background: #ef444422; color: #f87171; border: 1px solid #ef444444; }"
        "    </style>"
        "</head>"
        "<body>"
        "    <div class='container'>"
        "        <div class='header'>"
        "            <h1>🤖 PetBot Live Camera</h1>"
        "            <p>ESP32-S3 OV2640 Real-Time Stream</p>"
        "        </div>"
        "        <div class='video-container'>"
        "            <img id='stream' src='/stream' alt='Live Video Feed'>"
        "            <div id='placeholder'>"
        "                <h3>📷 Stream Paused</h3>"
        "            </div>"
        "        </div>"
        "        <div class='controls'>"
        "            <button class='btn-start' onclick='startStream()'>▶ Start Stream</button>"
        "            <button class='btn-stop' onclick='stopStream()'>⏹ Stop Stream</button>"
        "            <button class='btn-capture' onclick='capture()'>📸 Capture Snapshot</button>"
        "        </div>"
        "        <div class='status'>"
        "            <span>Stream Status:</span>"
        "            <span id='status' class='status-badge streaming'>Streaming 🟢</span>"
        "        </div>"
        "    </div>"
        "    <script>"
        "        let streamImg = document.getElementById('stream');"
        "        let placeholder = document.getElementById('placeholder');"
        "        let statusBadge = document.getElementById('status');"
        "        "
        "        function startStream() {"
        "            streamImg.src = '/stream?' + Date.now();"
        "            streamImg.style.display = 'block';"
        "            placeholder.style.display = 'none';"
        "            statusBadge.className = 'status-badge streaming';"
        "            statusBadge.innerHTML = 'Streaming 🟢';"
        "        }"
        "        "
        "        function stopStream() {"
        "            streamImg.src = '';"
        "            streamImg.style.display = 'none';"
        "            placeholder.style.display = 'block';"
        "            statusBadge.className = 'status-badge stopped';"
        "            statusBadge.innerHTML = 'Stopped 🔴';"
        "        }"
        "        "
        "        async function capture() {"
        "            try {"
        "                const a = document.createElement('a');"
        "                a.href = '/capture?' + Date.now();"
        "                a.download = `petbot_${Date.now()}.jpg`;"
        "                document.body.appendChild(a);"
        "                a.click();"
        "                document.body.removeChild(a);"
        "            } catch(e) {"
        "                alert('Snapshot failed');"
        "            }"
        "        }"
        "        "
        "        streamImg.onerror = function() {"
        "            setTimeout(() => { if (streamImg.src) streamImg.src = '/stream?' + Date.now(); }, 1000);"
        "        };"
        "    </script>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, strlen(html));
}

void start_camera_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.stack_size = 10240;
    config.max_uri_handlers = 10;
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

        ESP_LOGI(TAG, "Camera stream server started on port 80");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}
