/**
 * @file camera_manager.cpp
 * @brief 摄像头管理器实现 — 自动检测 GC2145/OV3660 并选择最优像素格式
 *
 * 检测流程：
 *   1. 先用 PIXFORMAT_RGB565 初始化（所有传感器都支持）
 *   2. 读取 PID 确认型号
 *   3. 如果是 OV3660/OV5640/OV2640 → 反初始化，用 PIXFORMAT_JPEG 重新初始化（硬件 JPEG）
 *   4. 如果是 GC2145 → 保持 RGB565（软件 JPEG）
 *   5. 串口打印 PID 和型号，供开发者确认
 */

#include <Arduino.h>
#include "camera_manager.h"
#include "../config/board_config.h"

// ── 型号识别 ──────────────────────────────────────────
CameraModel CameraManager::identifyModel(uint16_t pid) {
    switch (pid) {
        case GC2145_PID_VAL:
        case GC2145_PID_ALT:  return CAM_GC2145;
        case OV3660_PID_VAL:  return CAM_OV3660;
        case OV5640_PID_VAL:  return CAM_OV5640;
        case OV2640_PID_VAL:  return CAM_OV2640;
        default:              return CAM_UNKNOWN;
    }
}

const char* CameraManager::getModelStr() const {
    switch (_model) {
        case CAM_GC2145:  return "GC2145";
        case CAM_OV3660:  return "OV3660";
        case CAM_OV5640:  return "OV5640";
        case CAM_OV2640:  return "OV2640";
        default:          return "UNKNOWN";
    }
}

const char* CameraManager::getPixelFormatStr() const {
    switch (_pixFmt) {
        case PIXFORMAT_JPEG:      return "JPEG";
        case PIXFORMAT_RGB565:    return "RGB565";
        case PIXFORMAT_YUV422:    return "YUV422";
        case PIXFORMAT_GRAYSCALE: return "GRAYSCALE";
        default:                  return "UNKNOWN";
    }
}

// ── 底层初始化（指定格式和分辨率） ────────────────────
bool CameraManager::initOnce(pixformat_t fmt, framesize_t size) {
    camera_config_t config;
    memset(&config, 0, sizeof(config));

    // 引脚（来自 board_config.h）
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = Y2_GPIO_NUM;
    config.pin_d1        = Y3_GPIO_NUM;
    config.pin_d2        = Y4_GPIO_NUM;
    config.pin_d3        = Y5_GPIO_NUM;
    config.pin_d4        = Y6_GPIO_NUM;
    config.pin_d5        = Y7_GPIO_NUM;
    config.pin_d6        = Y8_GPIO_NUM;
    config.pin_d7        = Y9_GPIO_NUM;
    config.pin_xclk      = XCLK_GPIO_NUM;
    config.pin_pclk      = PCLK_GPIO_NUM;
    config.pin_vsync     = VSYNC_GPIO_NUM;
    config.pin_href      = HREF_GPIO_NUM;
    config.pin_sccb_sda  = SIOD_GPIO_NUM;
    config.pin_sccb_scl  = SIOC_GPIO_NUM;
    config.pin_pwdn      = PWDN_GPIO_NUM;
    config.pin_reset     = RESET_GPIO_NUM;

    // 参数
    config.xclk_freq_hz  = 14000000;       // 14MHz (GC2145 稳定; OV3660 也兼容)
    config.pixel_format  = fmt;
    config.frame_size    = size;
    config.jpeg_quality  = 12;             // JPEG 质量 (仅 JPEG 有效)
    config.fb_count      = 2;
    config.grab_mode     = CAMERA_GRAB_LATEST;
    config.fb_location   = CAMERA_FB_IN_PSRAM;

    // PSRAM 检查
    if (!psramFound()) {
        Serial.println("[CAM] WARNING: PSRAM not found! Falling back to DRAM.");
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count    = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] esp_camera_init failed: 0x%x (fmt=%d)\n", err, fmt);
        return false;
    }

    _pixFmt = fmt;
    return true;
}

// ── 主初始化流程 ──────────────────────────────────────
bool CameraManager::begin() {
    Serial.println("\n========== Camera Init ==========");

    // Step 1: 用 RGB565 初始化（所有传感器兼容）
    Serial.println("[CAM] Step 1: init with RGB565 (universal probe)...");
    if (!initOnce(PIXFORMAT_RGB565, FRAMESIZE_QVGA)) {
        Serial.println("[CAM] RGB565 init failed! Check wiring.");
        return false;
    }

    // Step 2: 读取 PID
    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("[CAM] Failed to get sensor object");
        return false;
    }

    _pid   = s->id.PID;
    _model = identifyModel(_pid);
    _hwJPEG = false;

    Serial.printf("[CAM] >>> PID: 0x%04X | Model: %s <<<\n", _pid, getModelStr());

    // Step 3: 如果支持硬件 JPEG，反初始化后重新初始化
    if (_model == CAM_OV3660 || _model == CAM_OV5640 || _model == CAM_OV2640) {
        Serial.println("[CAM] Step 3: sensor supports HW JPEG, re-initing with PIXFORMAT_JPEG...");
        esp_camera_deinit();

        if (!initOnce(PIXFORMAT_JPEG, FRAMESIZE_QVGA)) {
            Serial.println("[CAM] JPEG re-init failed! Falling back to RGB565...");
            initOnce(PIXFORMAT_RGB565, FRAMESIZE_QVGA);
        } else {
            _hwJPEG = true;
            Serial.println("[CAM] HW JPEG enabled");
        }
    } else if (_model == CAM_GC2145) {
        Serial.println("[CAM] GC2145: keeping RGB565 (no HW JPEG), using SW encode");
    } else {
        Serial.printf("[CAM] Unknown PID 0x%04X, keeping RGB565\n", _pid);
    }

    // Step 4: 应用传感器默认调整
    // P7a 修正: vflip 从 1 → 0。esp-face-test 实测: 无翻转=画面正立=人脸检测正常 (score 0.64);
    // vflip(1) 会让画面上下颠倒, MTMN 是正脸检测模型, 倒脸直接 0 results (FACETEST no face)。
    // 副带收益: P4.5 视觉对话/拍照的 MJPEG 画面也变正立, 发给 GLM-4V 识别更准。
    s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 0);  // 不翻转（OV3660 默认取向即为正立）
    }

    // 记录分辨率
    _width  = 320;
    _height = 240;

    // Step 5: 打印最终状态
    Serial.println("[CAM] ----- Final Status -----");
    Serial.printf("[CAM]   Model:       %s (PID 0x%04X)\n", getModelStr(), _pid);
    Serial.printf("[CAM]   Pixel Format: %s\n", getPixelFormatStr());
    Serial.printf("[CAM]   HW JPEG:     %s\n", _hwJPEG ? "YES" : "NO (software)");
    Serial.printf("[CAM]   Resolution:  %dx%d (QVGA)\n", _width, _height);
    Serial.printf("[CAM]   PSRAM:       %s (%d MB)\n",
                  psramFound() ? "OK" : "MISSING",
                  psramFound() ? (int)(ESP.getPsramSize() / 1048576) : 0);
    Serial.println("[CAM] ===========================\n");

    return true;
}

camera_fb_t* CameraManager::getFrame() {
    return esp_camera_fb_get();
}

void CameraManager::returnFrame(camera_fb_t* fb) {
    if (fb) esp_camera_fb_return(fb);
}

sensor_t* CameraManager::getSensor() {
    return esp_camera_sensor_get();
}
