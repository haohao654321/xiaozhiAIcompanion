/**
 * @file led_controller.h
 * @brief WS2812B RGB LED 控制器 — 情绪驱动呼吸灯 (P2 实现)
 *
 * 设计要点：
 *   - 由 CompanionEmotion 驱动（与 P3 状态机共用枚举，届时直接对接）
 *   - 非阻塞：update() 每帧调用，millis() 驱动，不阻塞主循环
 *   - P2 演示模式：自动轮播全部情绪，方便一眼验证所有灯效
 */
#pragma once
#include <Arduino.h>
#include "../companion/state_machine.h"

class LEDController {
public:
    bool begin();
    void update();                       // 每帧调用（~60fps 内部节流）

    // 情绪驱动（P3 状态机调用）
    void setEmotion(CompanionEmotion e);

    // 直接控制（调试/特殊用途）
    void setRGB(uint8_t r, uint8_t g, uint8_t b);
    void setBrightness(float maxBrightness);   // 0.0 ~ 1.0，全局亮度上限

    // P2 演示模式：每 6 秒轮播一种情绪
    void setDemoMode(bool enable);

private:
    struct EmotionStyle {
        uint8_t  r, g, b;      // 基础颜色
        uint16_t periodMs;     // 呼吸周期
        uint8_t  maxBright;    // 该情绪峰值亮度 (0-255)
    };
    static const EmotionStyle _styles[];   // 情绪 → 灯效映射表

    void _applyStyle(const EmotionStyle& s, uint32_t now);
    void _render();

    bool _ready = false;
    bool _demo  = false;                       // P3: 默认关, 情绪由状态机驱动
    float _maxBrightness = 0.7f;
    CompanionEmotion _emotion = EMOTION_NEUTRAL;

    uint8_t  _r = 0, _g = 0, _b = 0;      // 当前渲染颜色
    uint32_t _lastUpdate = 0;
    uint32_t _demoStart  = 0;
};
