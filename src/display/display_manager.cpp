/**
 * @file display_manager.cpp
 * @brief ST7789 TFT 显示管理器 — P2 实现 (Arduino_GFX)
 *
 * 接线 (杜邦线):
 *   屏幕 SCK  -> GPIO 21
 *   屏幕 MOSI -> GPIO 47
 *   屏幕 CS   -> GPIO 41
 *   屏幕 DC   -> GPIO 19   (原35被PSRAM占用, 已修正)
 *   屏幕 RST  -> GPIO 45
 *   屏幕 BL   -> GPIO 42
 *   屏幕 VCC  -> 3V3
 *   屏幕 GND  -> GND
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "display_manager.h"
#include "../config/board_config.h"
#include "../config/utf8_util.h"
#include "../companion/state_machine.h"

// GFX 实例 (堆分配, 避免大对象在栈上)
static Arduino_GFX* gfx = nullptr;

// P6: 低层画布访问 (display_state 用) — 命名避开全局 gfx, 防成员作用域遮蔽
Arduino_GFX* DisplayManager::canvas() { return gfx; }

// ── 情绪 → 主题色 (RGB565) ──
static uint16_t emotionColor(int e) {
    switch (e) {
        case EMOTION_HAPPY:     return 0xFBE0;   // 暖黄
        case EMOTION_SAD:       return 0x001F;   // 蓝
        case EMOTION_SURPRISED: return 0xF81F;   // 品红
        case EMOTION_THINKING:  return 0x07FF;   // 青
        case EMOTION_SLEEPY:    return 0x580F;   // 暗紫
        case EMOTION_NEUTRAL:
        default:                return 0xDF7B;   // 淡灰
    }
}

static const char* emotionName(int e) {
    switch (e) {
        case EMOTION_HAPPY:     return "HAPPY";
        case EMOTION_SAD:       return "SAD";
        case EMOTION_SURPRISED: return "SURPRISED";
        case EMOTION_THINKING:  return "THINKING";
        case EMOTION_SLEEPY:    return "SLEEPY";
        case EMOTION_NEUTRAL:
        default:                return "NEUTRAL";
    }
}

// ── 初始化 ──
bool DisplayManager::begin() {
    Serial.println("[DSP] ST7789 display init...");
    Serial.printf("[DSP]   Pins: SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d\n",
                  LCD_SCK_GPIO_NUM, LCD_MOSI_GPIO_NUM, LCD_CS_GPIO_NUM,
                  LCD_DC_GPIO_NUM, LCD_RST_GPIO_NUM, LCD_BL_GPIO_NUM);
    Serial.printf("[DSP]   Resolution: %dx%d @ %dMHz\n",
                  LCD_WIDTH, LCD_HEIGHT, LCD_SPI_FREQ_HZ / 1000000);

    // 背光 (高电平点亮)
    pinMode(LCD_BL_GPIO_NUM, OUTPUT);
    digitalWrite(LCD_BL_GPIO_NUM, HIGH);

    // SPI 总线 + ST7789 面板
    // 注意: Arduino_ESP32SPI 第 6 参是 spi_num (uint8_t), 不是频率!
    //   S3 上默认 FSPI=0 → SPI2 (用户自定义外设总线), 与板载接线匹配
    //   频率必须通过 gfx->begin(speed) 传入 (见下)
    Arduino_DataBus* bus = new Arduino_ESP32SPI(
        LCD_DC_GPIO_NUM, LCD_CS_GPIO_NUM,
        LCD_SCK_GPIO_NUM, LCD_MOSI_GPIO_NUM,
        GFX_NOT_DEFINED);                 // MISO 未用

    gfx = new Arduino_ST7789(
        bus,
        LCD_RST_GPIO_NUM,                // RST
        0,                               // rotation 0
        (LCD_INVERT_COLOR ? true : false), // IPS 反转
        LCD_WIDTH, LCD_HEIGHT,
        0, 0, 0, 0);                     // 行列偏移 (240x240 无偏移)

    if (!gfx->begin(LCD_SPI_FREQ_HZ)) {  // 显式 SPI 时钟 40MHz
        Serial.println("[DSP]   FAIL: gfx->begin() returned false");
        _ready = false;
        return false;
    }
    _ready = true;
    Serial.println("[DSP]   OK (Arduino_GFX ST7789, 240x240)");

    // 开机画面
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setTextSize(2);
    gfx->setCursor(28, 92);
    gfx->print("Companion");
    gfx->setTextSize(1);
    gfx->setCursor(62, 124);
    gfx->print("P2: ST7789 OK");

    _lastSwitch = millis();
    _demoIdx = 0;
    return true;
}

// ── 情绪 → 表情 ──
void DisplayManager::setEmotion(int emotionId) {
    if (!_ready || emotionId == _curEmotion) return;
    _curEmotion = emotionId;
    drawFace(emotionId);
}

// ── P4: WiFi 状态指示器 ──
void DisplayManager::setWiFiConnected(bool ok) {
    if (_wifiOK == ok) return;
    _wifiOK = ok;
    // 只画右上角小圆点, 不重绘整脸
    if (_ready) {
        uint16_t color = ok ? 0x07E0 : 0x7BEF;   // 绿=已连接 / 灰=未连接
        gfx->fillCircle(222, 18, 5, color);
    }
}

// ── 显示文字 ──
void DisplayManager::showText(const char* text) {
    if (!_ready) return;
    gfx->fillScreen(0x0000);
    gfx->setTextColor(0xFFFF, 0x0000);
    gfx->setTextSize(2);
    int16_t w = strlen(text) * 12;       // size2 每字符约 12px
    gfx->setCursor((LCD_WIDTH - w) / 2, 108);
    gfx->print(text);
}

// ── P4.5: 临时提示 (到时自动恢复表情) ──
void DisplayManager::showToast(const char* text, uint32_t durationMs) {
    if (!_ready) return;
    utf8Copy(_toastText, sizeof(_toastText), text);   // UTF-8 安全截断 (v1g)
    _toastUntil = millis() + durationMs;
    _toastRestoreEmotion = _curEmotion;
    showText(_toastText);
}

// ── P2 演示: 每 6 秒轮播 6 种情绪 ──
void DisplayManager::update() {
    if (!_ready) return;

    // P4.5: toast 到期 → 恢复表情
    uint32_t now = millis();
    if (_toastUntil && now >= _toastUntil) {
        _toastUntil = 0;
        if (_toastRestoreEmotion >= 0) setEmotion(_toastRestoreEmotion);
    }

    if (!_demoMode) return;
    if (now - _lastSwitch >= 6000) {
        _lastSwitch = now;
        _demoIdx = (_demoIdx + 1) % 6;   // 0..5 对应 EMOTION_NEUTRAL..SLEEPY
        setEmotion(_demoIdx);
    }
}

// ── 绘制整脸 ──
void DisplayManager::drawFace(int emotionId) {
    uint16_t bg = emotionColor(emotionId);
    gfx->fillScreen(bg);

    // 眼睛: 除 SLEEPY 外都睁眼
    bool eyesOpen = (emotionId != EMOTION_SLEEPY);
    drawEye(80, 100, eyesOpen);
    drawEye(160, 100, eyesOpen);

    // 嘴巴
    drawMouth(120, 150, emotionId);

    // 底部情绪名
    gfx->setTextColor(0xFFFF, bg);
    gfx->setTextSize(2);
    const char* name = emotionName(emotionId);
    int16_t w = strlen(name) * 12;
    gfx->setCursor((LCD_WIDTH - w) / 2, 205);
    gfx->print(name);

    // P4: 右上角 WiFi 状态指示器
    uint16_t wifiColor = _wifiOK ? 0x07E0 : 0x7BEF;   // 绿=已连接 / 灰=未连接
    gfx->fillCircle(222, 18, 5, wifiColor);
}

// ── 眼睛: 睁眼(圆) / 闭眼(线) ──
void DisplayManager::drawEye(int16_t cx, int16_t cy, bool open) {
    if (open) {
        gfx->fillCircle(cx, cy, 14, 0xFFFF);          // 眼白
        gfx->fillCircle(cx, cy, 7, 0x0000);           // 瞳孔
        gfx->fillCircle(cx + 3, cy - 3, 2, 0xFFFF);   // 高光
    } else {
        gfx->drawLine(cx - 12, cy, cx + 12, cy, 0x0000); // 闭眼
    }
}

// ── 嘴巴: 按情绪画形状 ──
void DisplayManager::drawMouth(int16_t cx, int16_t cy, int emotionId) {
    switch (emotionId) {
        case EMOTION_HAPPY:      // ∪ 大笑
            drawArcDots(cx, cy - 10, 18, 15, 165, 0x0000);
            break;
        case EMOTION_SAD:        // ∩ 哭泣
            drawArcDots(cx, cy + 10, 18, 195, 345, 0x0000);
            break;
        case EMOTION_SURPRISED:  // O 型
            gfx->fillCircle(cx, cy, 12, 0x0000);
            break;
        case EMOTION_THINKING:   // 平线 + 侧点
            gfx->drawLine(cx - 14, cy, cx + 4, cy, 0x0000);
            gfx->fillCircle(cx + 10, cy, 3, 0x0000);
            break;
        case EMOTION_SLEEPY:     // 平线 (微张)
            gfx->drawLine(cx - 14, cy, cx + 14, cy, 0x0000);
            break;
        case EMOTION_NEUTRAL:
        default:                 // 平线
            gfx->drawLine(cx - 14, cy, cx + 14, cy, 0x0000);
            break;
    }
}

// ── 沿弧线描点 (小圆点粗弧, 不依赖 drawArc) ──
void DisplayManager::drawArcDots(int16_t cx, int16_t cy, int16_t r,
                                 int16_t a0, int16_t a1, uint16_t color) {
    for (int16_t a = a0; a <= a1; a += 4) {
        float rad = a * PI / 180.0f;
        int16_t x = cx + (int16_t)(r * cosf(rad));
        int16_t y = cy + (int16_t)(r * sinf(rad));
        gfx->fillCircle(x, y, 2, color);
    }
}
