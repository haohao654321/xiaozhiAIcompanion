/**
 * @file display_manager.h
 * @brief ST7789 TFT 显示管理器 (P2 实现)
 *
 * 设计:
 *  - 基于 Arduino_GFX 库 (Arduino_ESP32SPI + Arduino_ST7789)
 *  - 情绪驱动: setEmotion() 接收 CompanionEmotion, 绘制对应表情
 *  - 非阻塞: update() 每帧调用, 内部 6 秒轮播演示 (P2 阶段)
 *  - P3 状态机广播后, 屏幕表情与 LED 灯效同步响应
 */
#pragma once

class Arduino_GFX;   // fwd (P6: display_state 低层画布访问用)

class DisplayManager {
public:
    bool begin();                      // 初始化屏幕 + 背光 + 开机画面
    void setEmotion(int emotionId);    // 按情绪绘制表情 (CompanionEmotion) [P6 起仅隔离调试模式用]
    void showText(const char* text);   // 中央显示一行文字 (ASCII)
    void showToast(const char* text, uint32_t durationMs);  // ASCII 临时提示 [P6 起建议用 DisplayState.showMessage]
    void setWiFiConnected(bool ok);    // [P6 起由 DisplayState.setOnline 接管]
    void update();                     // toast 到期恢复

    /** P6: 低层画布访问 (display_state 直接绘制用) */
    static Arduino_GFX* canvas();

private:
    bool _ready = false;
    bool _demoMode = false;            // P3: 默认关, 情绪由状态机驱动
    int  _curEmotion = -1;             // 当前已绘制情绪 (避免重复刷新)
    bool _wifiOK = false;              // P4: WiFi 连接状态
    uint32_t _lastSwitch = 0;
    int  _demoIdx = 0;

    // P4.5: toast 临时提示
    uint32_t _toastUntil = 0;          // 提示截止时刻
    int      _toastRestoreEmotion = -1;// 提示结束恢复的情绪
    char     _toastText[24];

    void drawFace(int emotionId);      // 画脸: 背景色 + 眼睛 + 嘴巴 + 情绪名
    void drawEye(int16_t cx, int16_t cy, bool open);
    void drawMouth(int16_t cx, int16_t cy, int emotionId);
    void drawArcDots(int16_t cx, int16_t cy, int16_t r, int16_t a0, int16_t a1, uint16_t color);
};
