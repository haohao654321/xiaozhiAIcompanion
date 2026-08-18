/**
 * @file camera_web.cpp
 * @brief 摄像头 Web 服务器实现 — MJPEG 流 + 拍照
 *
 * 参考: 旧项目 httpd_stream.cpp / httpd_capture.cpp / httpd_server.cpp
 *   - multipart/x-mixed-replace 分块推帧 (STREAM_BOUNDARY / STREAM_PART)
 *   - RGB565 帧自动 frame2jpg 软件转换 (GC2145)
 *   - 拍照优先: 流循环检测 capture->isBusy() 暂停取帧, 避免抢帧
 */
#include <Arduino.h>
#include "esp_http_server.h"
#include "esp_camera.h"
#include "camera_web.h"

// ── MJPEG 流常量 (与旧项目一致) ──
#define PART_BOUNDARY      "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" PART_BOUNDARY
#define STREAM_BOUNDARY     "\r\n--" PART_BOUNDARY "\r\n"
#define STREAM_PART         "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n"

#define STREAM_FRAME_DELAY_MS  50    // ~10-15 fps, 降低 WiFi/CPU 负载

static httpd_handle_t s_httpd = NULL;
static CaptureManager* s_capture = NULL;

// ── 内嵌网页 (简洁版: 视频流 + 拍照按钮) ──
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 Camera</title>
<style>
body{background:#111;color:#eee;font-family:sans-serif;text-align:center;margin:0;padding:16px}
h1{font-size:18px;margin:8px 0}
img#stream{width:min(90vw,360px);border-radius:8px;border:2px solid #333;background:#000}
button{background:#2a7;color:#fff;border:none;border-radius:6px;padding:10px 24px;
       font-size:16px;margin:12px 4px;cursor:pointer}
button:active{opacity:.7}
#photo{display:none;width:min(90vw,360px);border-radius:8px;margin-top:12px;border:2px solid #333}
#status{font-size:12px;color:#888;margin-top:8px}
</style>
</head>
<body>
<h1>&#x4F19;&#x8BA1; &mdash; Camera</h1>
<img id="stream" src="/stream" alt="stream">
<br>
<button onclick="snap()">&#x62CD;&#x7167;</button>
<img id="photo" alt="photo">
<div id="status">MJPEG stream</div>
<script>
function snap(){
  var p=document.getElementById('photo');
  p.src='/capture?'+Date.now();
  p.style.display='block';
  document.getElementById('status').textContent='Photo captured';
}
</script>
</body>
</html>
)rawliteral";

// ── 首页 ──
static esp_err_t index_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

// ── MJPEG 流 (独立于拍照; 拍照时 isBusy 避让) ──
static esp_err_t stream_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        // 拍照/视觉对话占用摄像头时暂停取帧
        if (s_capture->isBusy()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t* jpg = NULL;
        size_t   jpg_len = 0;
        if (!s_capture->captureJpeg(&jpg, &jpg_len)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        char part[64];
        size_t hlen = snprintf(part, sizeof(part), STREAM_PART, jpg_len);

        esp_err_t res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)jpg, jpg_len);
        free(jpg);

        if (res != ESP_OK) {
            ESP_LOGI("camera_web", "Stream ended (client disconnected?)");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_DELAY_MS));
    }
    return ESP_OK;
}

// ── 拍照: 返回 JPEG 给浏览器, 同时存 SD (可用时) ──
static esp_err_t capture_handler(httpd_req_t* req) {
    uint8_t* jpg = NULL;
    size_t   jpg_len = 0;

    if (!s_capture->captureJpeg(&jpg, &jpg_len)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Capture failed");
        return ESP_FAIL;
    }

    // 存 SD (降级: 无卡跳过)
    if (s_capture->sdAvailable()) {
        s_capture->saveJpegToSD(jpg, jpg_len);
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char*)jpg, jpg_len);
    free(jpg);
    return res;
}

// ── 启动服务器 ──
bool startCameraWeb(CaptureManager* capture) {
    if (!capture || !capture->cameraReady()) {
        Serial.println("[WEB] Camera not ready, web server not started");
        return false;
    }
    s_capture = capture;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size  = 8192;   // 流 handler 帧转换栈开销, 默认 4096 偏小
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        Serial.println("[WEB] Failed to start http server");
        return false;
    }

    httpd_uri_t uri_index = {.uri="/",       .method=HTTP_GET, .handler=index_handler,  .user_ctx=NULL};
    httpd_uri_t uri_stream= {.uri="/stream", .method=HTTP_GET, .handler=stream_handler, .user_ctx=NULL};
    httpd_uri_t uri_cap   = {.uri="/capture",.method=HTTP_GET, .handler=capture_handler,.user_ctx=NULL};
    httpd_register_uri_handler(s_httpd, &uri_index);
    httpd_register_uri_handler(s_httpd, &uri_stream);
    httpd_register_uri_handler(s_httpd, &uri_cap);

    Serial.println("[WEB] Camera web server: http://<ip>/  (stream /stream, capture /capture)");
    return true;
}
