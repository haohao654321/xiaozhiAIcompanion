/**
 * @file capture_manager.cpp
 * @brief 拍照管理器实现
 *
 * 链路: esp_camera_fb_get → (RGB565?) frame2jpg 软件压缩 → SD_MMC 写文件
 * 参考: 旧项目 Goouuu-CAM-WebServer 已验证的 RGB565 取帧 + SD 保存逻辑
 */
#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include "img_converters.h"       // frame2jpg / fmt2rgb888 (RGB565 → JPEG / RGB888)
#include "esp_heap_caps.h"        // heap_caps_malloc (PSRAM)
#include "capture_manager.h"
#include "../config/board_config.h"  // SD_MMC_* 引脚

#define CAPTURE_SD_MOUNT   "/sdcard"
#define CAPTURE_DIR        "/captures"

void CaptureManager::begin(CameraManager* cam) {
    _cam = cam;
    _camReady = (cam != nullptr && cam->getPID() != 0);

    // SD 卡 (1-bit SDMMC, 与 mic_manager 相同参数; 无卡不阻塞)
    SD_MMC.setPins(SD_MMC_CLK_GPIO_NUM, SD_MMC_CMD_GPIO_NUM, SD_MMC_D0_GPIO_NUM);
    if (SD_MMC.begin(CAPTURE_SD_MOUNT, true)) {   // true = 1-bit 模式 (只接 D0)
        _sdOk = true;
        if (!SD_MMC.exists(CAPTURE_DIR)) {
            SD_MMC.mkdir(CAPTURE_DIR);
        }
        Serial.printf("[CAP] SD card OK: %llu MB free\n",
                      (unsigned long long)(SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1048576);
    } else {
        Serial.println("[CAP] SD card NOT found (photos will not be saved)");
    }

    Serial.printf("[CAP] Capture manager ready (camera=%s, SD=%s)\n",
                  _camReady ? "OK" : "FAIL",
                  _sdOk ? "OK" : "FAIL");
}

// ── 取帧 → JPEG ──
bool CaptureManager::captureJpeg(uint8_t** outBuf, size_t* outLen, int quality) {
    if (!_camReady || !_cam) {
        Serial.println("[CAP] Camera not ready");
        return false;
    }
    _busy = true;

    camera_fb_t* fb = _cam->getFrame();
    if (!fb) {
        Serial.println("[CAP] Failed to grab frame");
        _busy = false;
        return false;
    }

    bool ok = false;
    if (fb->format == PIXFORMAT_JPEG) {
        // 硬件 JPEG: 直接拷贝一份 (调用者 free)
        *outBuf = (uint8_t*)malloc(fb->len);
        if (*outBuf) {
            memcpy(*outBuf, fb->buf, fb->len);
            *outLen = fb->len;
            ok = true;
        }
    } else {
        // RGB565 / 其他: 软件压缩
        ok = frame2jpg(fb, quality, outBuf, outLen);
    }

    _cam->returnFrame(fb);
    _busy = false;

    if (!ok) {
        Serial.println("[CAP] JPEG conversion failed");
    } else {
        Serial.printf("[CAP] JPEG: %u bytes (fmt=%d)\n", (unsigned)(*outLen), fb->format);
    }
    return ok;
}

// ── P7a: 取帧 → RGB888 (人脸检测输入) ──
bool CaptureManager::captureRgb888(uint8_t** outBuf, int* outW, int* outH) {
    if (!_camReady || !_cam) {
        Serial.println("[CAP] Camera not ready");
        return false;
    }
    _busy = true;

    camera_fb_t* fb = _cam->getFrame();
    if (!fb) {
        Serial.println("[CAP] Failed to grab frame");
        _busy = false;
        return false;
    }

    size_t rgb_len = (size_t)fb->width * fb->height * 3;
    uint8_t* rgb = (uint8_t*)heap_caps_malloc(rgb_len, MALLOC_CAP_SPIRAM);
    if (!rgb) {
        Serial.println("[CAP] OOM for RGB888");
        _cam->returnFrame(fb);
        _busy = false;
        return false;
    }

    bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, rgb);
    int w = fb->width, h = fb->height;     // fb return 前保存
    _cam->returnFrame(fb);
    _busy = false;

    if (!ok) {
        heap_caps_free(rgb);
        Serial.println("[CAP] RGB888 conversion failed");
        return false;
    }
    *outBuf = rgb;
    *outW = w;
    *outH = h;
    return true;
}

// ── JPEG → SD ──
bool CaptureManager::saveJpegToSD(const uint8_t* jpeg, size_t len, String* outPath) {
    if (!_sdOk) {
        Serial.println("[CAP] SD not available, photo NOT saved");
        return false;
    }
    char path[48];
    snprintf(path, sizeof(path), CAPTURE_SD_MOUNT CAPTURE_DIR "/photo_%lu.jpg",
             (unsigned long)millis());

    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) {
        Serial.printf("[CAP] Failed to open %s\n", path);
        return false;
    }
    size_t written = f.write(jpeg, len);
    f.close();

    if (written != len) {
        Serial.printf("[CAP] Write incomplete: %u/%u\n", (unsigned)written, (unsigned)len);
        return false;
    }
    Serial.printf("[CAP] Saved: %s (%u bytes)\n", path, (unsigned)len);
    if (outPath) *outPath = path;
    return true;
}

// ── 一键拍照存卡 ──
bool CaptureManager::takePhotoAndSave(String* outPath) {
    uint8_t* jpeg = nullptr;
    size_t   len  = 0;

    if (!captureJpeg(&jpeg, &len)) {
        Serial.println("[CAP] Photo failed (frame/JPEG)");
        return false;
    }

    bool ok = saveJpegToSD(jpeg, len, outPath);
    free(jpeg);   // captureJpeg 的约定: 调用者 free
    return ok;
}
