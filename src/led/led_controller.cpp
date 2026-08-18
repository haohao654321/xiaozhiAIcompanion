/**
 * @file led_controller.cpp
 * @brief WS2812B RGB LED 控制器实现 — 情绪驱动呼吸灯 (P2)
 *
 * 呼吸算法：sin 包络（0→峰值→0 平滑过渡），亮度由情绪风格表决定。
 * update() 内部 16ms 节流（~60fps），RMT 驱动开销极小。
 */
#include <Adafruit_NeoPixel.h>
#include "led_controller.h"
#include "../config/board_config.h"

// ── 板载 WS2812B（GPIO48, 1 颗, GRB 排列）──
static Adafruit_NeoPixel _strip(WS2812_COUNT, WS2812_GPIO_NUM, NEO_GRB + NEO_KHZ800);

// ── 情绪 → 灯效映射表（顺序必须与 CompanionEmotion 枚举一致）──
//   NEUTRAL:   淡白 · 慢呼吸（柔和）
//   HAPPY:     暖黄 · 中速明亮（欢快）
//   SAD:       蓝   · 慢（低落）
//   SURPRISED: 品红 · 快脉冲（惊讶）
//   THINKING:  青   · 中慢（思考）
//   SLEEPY:    暗紫 · 极慢且暗（困倦）
const LEDController::EmotionStyle LEDController::_styles[] = {
    /*            R    G    B   period  maxB */
    /*NEUTRAL*/  {255, 255, 255, 3000,   90},
    /*HAPPY*/    {255, 180,  40, 1500,  200},
    /*SAD*/      { 60, 110, 255, 4000,  120},
    /*SURPRISED*/{255,  60, 200,  700,  220},
    /*THINKING*/ { 60, 200, 255, 2500,  150},
    /*SLEEPY*/   {130,  80, 200, 6000,   50},
};

bool LEDController::begin() {
    _strip.begin();
    _strip.setBrightness((uint8_t)(255.0f * _maxBrightness));
    _strip.show();

    _ready = true;
    _demoStart = millis();

    Serial.println("[LED] WS2812B OK (Adafruit NeoPixel)");
    Serial.printf("[LED]   Pin: GPIO%d | Count: %d | Mode: state-machine driven\n",
                  WS2812_GPIO_NUM, WS2812_COUNT);
    return true;
}

void LEDController::setEmotion(CompanionEmotion e) {
    if (e == _emotion) return;
    _emotion = e;
    if (_ready) _render();   // 立即切换，无需等下一帧
}

void LEDController::setRGB(uint8_t r, uint8_t g, uint8_t b) {
    if (!_ready) return;
    _r = r; _g = g; _b = b;
    _render();
}

void LEDController::setBrightness(float mb) {
    _maxBrightness = constrain(mb, 0.0f, 1.0f);
    if (_ready) _strip.setBrightness((uint8_t)(255.0f * _maxBrightness));
}

void LEDController::setDemoMode(bool enable) {
    _demo = enable;
    _demoStart = millis();
}

void LEDController::update() {
    if (!_ready) return;

    uint32_t now = millis();
    if (now - _lastUpdate < 16) return;   // ~60fps 节流
    _lastUpdate = now;

    // 演示模式：每 6 秒轮播一种情绪
    if (_demo) {
        CompanionEmotion emo = (CompanionEmotion)(((now - _demoStart) / 6000) % 6);
        if (emo != _emotion) _emotion = emo;
    }

    _applyStyle(_styles[_emotion], now);
    _render();
}

void LEDController::_applyStyle(const EmotionStyle& s, uint32_t now) {
    // sin 包络: 0 → 1 → 0（平滑呼吸）
    float phase   = (float)(now % s.periodMs) / (float)s.periodMs;   // 0.0~1.0
    float envelope = 0.5f * (1.0f - cosf(2.0f * PI * phase));        // 0→1→0
    float bright  = (float)s.maxBright / 255.0f * envelope;          // 0..maxB/255

    _r = (uint8_t)((float)s.r * bright);
    _g = (uint8_t)((float)s.g * bright);
    _b = (uint8_t)((float)s.b * bright);
}

void LEDController::_render() {
    _strip.setPixelColor(0, _strip.Color(_r, _g, _b));
    _strip.show();
}
