#include "http_stream.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "esp_log.h"

static const char *TAG = "http_stream";

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static volatile bool streaming_enabled = true;

static esp_err_t stream_handler(httpd_req_t *req)
{
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[128];
    int frame_count = 0;
    int invalid_frames = 0;

    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    // Set headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "X-Framerate", "30");

    ESP_LOGI(TAG, "Stream handler started");

    while (streaming_enabled) {
        // Get frame
        fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Camera capture failed");
            invalid_frames++;
            if (invalid_frames > 10) {
                ESP_LOGE(TAG, "Too many failed captures, stopping stream");
                res = ESP_FAIL;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Validate JPEG data
        bool valid_jpeg = (fb->len > 0 &&
                          fb->buf[0] == 0xFF &&
                          fb->buf[1] == 0xD8 &&
                          fb->buf[fb->len-2] == 0xFF &&
                          fb->buf[fb->len-1] == 0xD9);

        if (!valid_jpeg) {
            ESP_LOGW(TAG, "Invalid JPEG frame %d: size=%d", frame_count + 1, fb->len);
            esp_camera_fb_return(fb);
            invalid_frames++;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Valid frame received
        invalid_frames = 0;
        frame_count++;

        if (frame_count % 30 == 0) {
            ESP_LOGI(TAG, "Streamed %d valid frames", frame_count);
        }

        // Send boundary
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Client disconnected (boundary send failed)");
            esp_camera_fb_return(fb);
            break;
        }

        // Send part header
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Client disconnected (header send failed)");
            esp_camera_fb_return(fb);
            break;
        }

        // Send JPEG data
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            ESP_LOGW(TAG, "Client disconnected (data send failed)");
            break;
        }

        // Small delay between frames to prevent flooding (30 fps)
        vTaskDelay(pdMS_TO_TICKS(33));
    }

    ESP_LOGI(TAG, "Stream handler stopped, sent %d valid frames", frame_count);
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

    // Parse JSON
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
        "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;"
        "            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "            min-height: 100vh;"
        "            padding: 20px;"
        "        }"
        "        .container {"
        "            max-width: 1200px;"
        "            margin: 0 auto;"
        "            background: white;"
        "            border-radius: 20px;"
        "            box-shadow: 0 20px 60px rgba(0,0,0,0.3);"
        "            overflow: hidden;"
        "        }"
        "        .header {"
        "            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);"
        "            color: white;"
        "            padding: 30px;"
        "            text-align: center;"
        "        }"
        "        .header h1 { font-size: 2em; margin-bottom: 10px; }"
        "        .header p { opacity: 0.9; }"
        "        .video-container {"
        "            background: #000;"
        "            position: relative;"
        "            min-height: 480px;"
        "            display: flex;"
        "            align-items: center;"
        "            justify-content: center;"
        "        }"
        "        #stream {"
        "            width: 100%;"
        "            max-height: 70vh;"
        "            object-fit: contain;"
        "            display: none;"
        "        }"
        "        #placeholder {"
        "            color: white;"
        "            text-align: center;"
        "            padding: 40px;"
        "        }"
        "        .controls {"
        "            padding: 30px;"
        "            background: #f8f9fa;"
        "            display: flex;"
        "            gap: 15px;"
        "            justify-content: center;"
        "            flex-wrap: wrap;"
        "        }"
        "        button {"
        "            padding: 12px 30px;"
        "            font-size: 16px;"
        "            font-weight: bold;"
        "            border: none;"
        "            border-radius: 50px;"
        "            cursor: pointer;"
        "            transition: all 0.3s ease;"
        "            box-shadow: 0 2px 5px rgba(0,0,0,0.2);"
        "        }"
        "        button:hover { transform: translateY(-2px); box-shadow: 0 4px 10px rgba(0,0,0,0.3); }"
        "        button:active { transform: translateY(0); }"
        "        .btn-start { background: #28a745; color: white; }"
        "        .btn-start:hover { background: #218838; }"
        "        .btn-stop { background: #dc3545; color: white; }"
        "        .btn-stop:hover { background: #c82333; }"
        "        .btn-capture { background: #ffc107; color: #333; }"
        "        .btn-capture:hover { background: #e0a800; }"
        "        .status {"
        "            padding: 15px 30px;"
        "            background: #e9ecef;"
        "            text-align: center;"
        "            font-weight: bold;"
        "            display: flex;"
        "            justify-content: space-between;"
        "            align-items: center;"
        "            flex-wrap: wrap;"
        "            gap: 10px;"
        "        }"
        "        .status-badge {"
        "            padding: 5px 15px;"
        "            border-radius: 20px;"
        "            font-size: 14px;"
        "            font-weight: bold;"
        "        }"
        "        .status-badge.stopped { background: #dc3545; color: white; }"
        "        .status-badge.streaming { background: #28a745; color: white; }"
        "        .info {"
        "            padding: 20px;"
        "            background: #f8f9fa;"
        "            text-align: center;"
        "            color: #666;"
        "            font-size: 14px;"
        "            border-top: 1px solid #dee2e6;"
        "        }"
        "        @media (max-width: 768px) {"
        "            button { padding: 10px 20px; font-size: 14px; }"
        "            .header h1 { font-size: 1.5em; }"
        "        }"
        "    </style>"
        "</head>"
        "<body>"
        "    <div class='container'>"
        "        <div class='header'>"
        "            <h1>🤖 PetBot Camera Controller</h1>"
        "            <p>ESP32-S3 with OV2640 Camera</p>"
        "        </div>"
        "        <div class='video-container'>"
        "            <img id='stream' src='' alt='Camera Stream'>"
        "            <div id='placeholder'>"
        "                <h2>📷 Camera Ready</h2>"
        "                <p>Click Start Streaming to begin</p>"
        "            </div>"
        "        </div>"
        "        <div class='controls'>"
        "            <button class='btn-start' onclick='startStream()'>▶ Start Streaming</button>"
        "            <button class='btn-stop' onclick='stopStream()'>⏹ Stop Streaming</button>"
        "            <button class='btn-capture' onclick='capture()'>📸 Capture Photo</button>"
        "        </div>"
        "        <div class='status'>"
        "            <span>📡 Stream Status:</span>"
        "            <span id='status' class='status-badge stopped'>Stopped</span>"
        "        </div>"
        "        <div class='info'>"
        "            <p>🔧 Tip: Use Start/Stop to control the video stream. Capture takes a single photo.</p>"
        "        </div>"
        "    </div>"
        "    <script>"
        "        let streamActive = false;"
        "        let streamImg = document.getElementById('stream');"
        "        let placeholder = document.getElementById('placeholder');"
        "        let statusBadge = document.getElementById('status');"
        "        "
        "        function updateUI(active) {"
        "            streamActive = active;"
        "            if (active) {"
        "                streamImg.style.display = 'block';"
        "                placeholder.style.display = 'none';"
        "                statusBadge.className = 'status-badge streaming';"
        "                statusBadge.innerHTML = 'Streaming 🟢';"
        "            } else {"
        "                streamImg.style.display = 'none';"
        "                placeholder.style.display = 'block';"
        "                streamImg.src = '';"
        "                statusBadge.className = 'status-badge stopped';"
        "                statusBadge.innerHTML = 'Stopped 🔴';"
        "            }"
        "        }"
        "        "
        "        async function startStream() {"
        "            try {"
        "                const response = await fetch('/control', {"
        "                    method: 'POST',"
        "                    headers: { 'Content-Type': 'application/json' },"
        "                    body: JSON.stringify({ action: 'start' })"
        "                });"
        "                const data = await response.json();"
        "                if (data.status === 'started') {"
        "                    streamImg.src = '/stream?' + new Date().getTime();"
        "                    updateUI(true);"
        "                }"
        "            } catch(e) {"
        "                console.error('Start failed:', e);"
        "                alert('Failed to start stream');"
        "            }"
        "        }"
        "        "
        "        async function stopStream() {"
        "            try {"
        "                const response = await fetch('/control', {"
        "                    method: 'POST',"
        "                    headers: { 'Content-Type': 'application/json' },"
        "                    body: JSON.stringify({ action: 'stop' })"
        "                });"
        "                const data = await response.json();"
        "                if (data.status === 'stopped') {"
        "                    updateUI(false);"
        "                }"
        "            } catch(e) {"
        "                console.error('Stop failed:', e);"
        "                updateUI(false);"
        "            }"
        "        }"
        "        "
        "        async function capture() {"
        "            try {"
        "                const response = await fetch('/capture');"
        "                if (response.ok) {"
        "                    const blob = await response.blob();"
        "                    const url = URL.createObjectURL(blob);"
        "                    const a = document.createElement('a');"
        "                    a.href = url;"
        "                    a.download = `petbot_capture_${Date.now()}.jpg`;"
        "                    document.body.appendChild(a);"
        "                    a.click();"
        "                    document.body.removeChild(a);"
        "                    URL.revokeObjectURL(url);"
        "                    alert('Photo saved!');"
        "                } else {"
        "                    alert('Capture failed');"
        "                }"
        "            } catch(e) {"
        "                console.error('Capture failed:', e);"
        "                alert('Failed to capture photo');"
        "            }"
        "        }"
        "        "
        "        // Auto-reconnect on error"
        "        streamImg.onerror = function() {"
        "            if (streamActive) {"
        "                console.log('Stream error, reconnecting...');"
        "                setTimeout(() => {"
        "                    if (streamActive) {"
        "                        streamImg.src = '/stream?' + new Date().getTime();"
        "                    }"
        "                }, 1000);"
        "            }"
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
    config.stack_size = 8192;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;

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
        ESP_LOGI(TAG, "Open http://192.168.1.11/ in your browser");
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}