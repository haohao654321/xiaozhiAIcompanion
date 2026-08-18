/**
 * @file capture_manager.h
 * @brief 拍照管理器 — 取帧 → JPEG → 存 SD（P4.5 摄像头应用层基础）
 *
 * 职责:
 *   - captureJpeg(): 取一帧并转为 JPEG 缓冲（RGB565 用软件压缩, 硬件 JPEG 直接拿）
 *   - saveJpegToSD(): JPEG 缓冲写入 SD 卡 /captures/
 *   - takePhotoAndSave(): 一键拍照存卡（返回文件信息）
 *   - busy 标志: 供视频流等长占摄像头资源的模块避让
 *
 * 内存所有权:
 *   - captureJpeg() 的 outBuf 由调用者负责 free()（与 img_converters 约定一致）
 *   - 无 SD 卡时拍照降级: 只取帧打印信息, 不写卡
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "camera_manager.h"

class CaptureManager {
public:
    /** 初始化: 探测 SD 卡（1-bit SDMMC, 无卡不阻塞） */
    void begin(CameraManager* cam);

    /**
     * 取一帧并转 JPEG
     * @param outBuf  输出 JPEG 缓冲 (PSRAM/堆分配, 调用者 free())
     * @param outLen  输出长度
     * @param quality JPEG 质量 0~100 (仅 RGB565 软件压缩时生效)
     * @return true=成功
     */
    bool captureJpeg(uint8_t** outBuf, size_t* outLen, int quality = 80);

    /** 保存 JPEG 到 SD /captures/photo_<millis>.jpg */
    bool saveJpegToSD(const uint8_t* jpeg, size_t len, String* outPath = nullptr);

    /**
     * P7a: 取一帧并转 RGB888 (人脸检测/识别输入)
     * @param outBuf  输出 RGB888 缓冲 (PSRAM 分配, 调用者 heap_caps_free)
     * @param outW    帧宽
     * @param outH    帧高
     * @return true=成功
     */
    bool captureRgb888(uint8_t** outBuf, int* outW, int* outH);

    /** 一键: 拍照并存 SD。返回 true=成功 (照片已落卡) */
    bool takePhotoAndSave(String* outPath = nullptr);

    // ── 摄像头占用互斥（视频流/拍照避让） ──
    bool isBusy() const { return _busy; }
    void setBusy(bool b) { _busy = b; }

    bool sdAvailable() const { return _sdOk; }
    bool cameraReady() const { return _camReady; }

private:
    CameraManager* _cam = nullptr;
    bool _camReady = false;   // 摄像头已初始化
    bool _sdOk     = false;   // SD 卡可用
    volatile bool _busy = false;
};
