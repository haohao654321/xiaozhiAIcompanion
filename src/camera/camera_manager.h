/**
 * @file camera_manager.h
 * @brief 摄像头管理器 — 自动检测 GC2145/OV3660，选择最优像素格式
 */

#pragma once

#include "esp_camera.h"

// 传感器 PID 常量
#define GC2145_PID_VAL     0x2145
#define GC2145_PID_ALT     0x40DC   // 部分 GC2145 返回的替代 PID
#define OV3660_PID_VAL     0x3660
#define OV5640_PID_VAL     0x5640
#define OV2640_PID_VAL     0x2640

// 摄像头型号枚举
enum CameraModel {
    CAM_UNKNOWN = 0,
    CAM_GC2145,
    CAM_OV3660,
    CAM_OV5640,
    CAM_OV2640,
};

class CameraManager {
public:
    /// @brief 初始化摄像头（自动检测型号并选择最优格式）
    /// @return true=成功
    bool begin();

    /// @brief 获取一帧
    /// @return camera_fb_t* (用完需 returnFrame)
    camera_fb_t* getFrame();

    /// @brief 归还帧缓冲
    void returnFrame(camera_fb_t* fb);

    /// @brief 获取传感器对象
    sensor_t* getSensor();

    // ── 查询接口 ──
    CameraModel getModel() const { return _model; }
    uint16_t getPID() const { return _pid; }
    bool isHardwareJPEG() const { return _hwJPEG; }
    int getWidth() const { return _width; }
    int getHeight() const { return _height; }
    const char* getModelStr() const;
    const char* getPixelFormatStr() const;

private:
    CameraModel  _model    = CAM_UNKNOWN;
    uint16_t     _pid      = 0;
    bool         _hwJPEG   = false;
    int          _width    = 0;
    int          _height   = 0;
    pixformat_t  _pixFmt   = PIXFORMAT_RGB565;

    bool initOnce(pixformat_t fmt, framesize_t size);
    CameraModel identifyModel(uint16_t pid);
};
