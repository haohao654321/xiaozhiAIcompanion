/**
 * @file display_state.cpp
 * @brief P6 显示状态机实现 — 5 状态全中文 + 等待倒数 + 彩蛋
 *
 * 重绘只在 状态/倒数/消息 变化时发生 (主循环 update 只查时间, 不刷屏)。
 * 中文绘制: font_cn.h 点阵子集 (24x24), UTF-8 解码 → 码点二分查找。
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "display_state.h"
#include "font_cn.h"
#include "../config/board_config.h"
#include "../config/utf8_util.h"

// ── 状态底色 (RGB565) ──
static uint16_t stateBg(DispStateId s) {
    switch (s) {
        case DISP_SLEEP:  return 0x3807;   // 暗紫 (暗于 P3 SLEEPY, 睡觉感)
        case DISP_LISTEN: return 0x0362;   // 青绿 (竖耳聆听)
        case DISP_THINK:  return 0x024F;   // 蓝 (想事)
        case DISP_SPEAK:  return 0xF9C4;   // 暖黄 (说话)
        case DISP_WAIT:   return 0x39E7;   // 灰蓝 (待机)
        default:          return 0x0000;
    }
}

// ── 中文点阵: UTF-8 解码 + 码点二分查找 ──
static const CnGlyph* findGlyph(uint16_t cp) {
    int lo = 0, hi = CN_GLYPH_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t m = pgm_read_word(&kCnGlyphs[mid].cp);
        if (m == cp) return &kCnGlyphs[mid];
        if (m < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

int16_t cnTextWidth(const char* utf8) {
    int16_t w = 0;
    for (const char* p = utf8; *p; ) {
        uint8_t c = (uint8_t)*p;
        if (c < 0x80) { w += 12; p += 1; }
        else if ((c & 0xE0) == 0xC0) { w += 24; p += 2; }
        else if ((c & 0xF0) == 0xE0) { w += 24; p += 3; }
        else { w += 24; p += 4; }
    }
    return w;
}

void drawCnText(Arduino_GFX* g, int16_t x, int16_t y, const char* utf8, uint16_t color) {
    int16_t cx = x;
    for (const char* p = utf8; *p; ) {
        uint8_t c = (uint8_t)*p;
        uint16_t cp; int len;
        if (c < 0x80)        { cp = c;       len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else                 { cp = c & 0x07; len = 4; }
        for (int i = 1; i < len && p[i]; i++) cp = (cp << 6) | ((uint8_t)p[i] & 0x3F);
        p += len;

        if (cp < 0x80) {           // ASCII: 用内置字体补画 (文案里基本没有)
            g->setTextColor(color);
            g->setTextSize(2);
            g->setCursor(cx, y + 4);
            g->print((char)cp);
            cx += 12;
            continue;
        }
        const CnGlyph* gl = findGlyph(cp);
        if (!gl) { cx += 24; continue; }   // 字库缺字: 留空跳过
        for (int r = 0; r < 24; r++) {
            const uint8_t* row = gl->bits + r * 3;
            uint8_t b0 = pgm_read_byte(row), b1 = pgm_read_byte(row + 1), b2 = pgm_read_byte(row + 2);
            if (b0) for (int col = 0; col < 8; col++)  if (b0 & (0x80 >> col)) g->drawPixel(cx + col, y + r, color);
            if (b1) for (int col = 0; col < 8; col++)  if (b1 & (0x80 >> col)) g->drawPixel(cx + 8 + col, y + r, color);
            if (b2) for (int col = 0; col < 8; col++)  if (b2 & (0x80 >> col)) g->drawPixel(cx + 16 + col, y + r, color);
        }
        cx += 24;
    }
}

// ── 初始化 ──
void DisplayState::begin(DisplayManager* dm) {
    _dm = dm;
    _ready = (dm != nullptr);
    _msgText[0] = '\0';
    setSleep();
    Serial.println("[UI] DisplayState ready (5-state CN)");
}

// ── 基础状态 ──
// 说明: 消息(_msgUntil)/彩蛋(_easterIdx)是叠加层; setWait/setSleep 在叠加层
//       活跃时只记录目标状态, 到期 _redraw 自然显示最新状态脸 (提示不被瞬间刷掉)。
void DisplayState::setSleep()  {
    if (_easterIdx >= 0) { _state = DISP_SLEEP; return; }   // 彩蛋底层就是睡眠
    if (_state == DISP_SLEEP && !_msgUntil) return;
    _state = DISP_SLEEP; _msgUntil = 0; _zStep = -1; _redraw();
}
void DisplayState::setListen() { if (_state == DISP_LISTEN) return; _state = DISP_LISTEN; _msgUntil = 0; _easterIdx = -1; _redraw(); }
void DisplayState::setThink()  { if (_state == DISP_THINK)  return; _state = DISP_THINK;  _msgUntil = 0; _easterIdx = -1; _redraw(); }
void DisplayState::setSpeak()  { if (_state == DISP_SPEAK)  return; _state = DISP_SPEAK;  _msgUntil = 0; _easterIdx = -1; _redraw(); }

void DisplayState::setWait(int8_t sec) {
    if (_state == DISP_WAIT && sec == _waitSec && _easterIdx < 0) return;
    _state = DISP_WAIT;
    _waitSec = sec;
    if (_msgUntil || _easterIdx >= 0) return;   // 提示/彩蛋显示中: 只记状态, 到期再绘
    _redraw();
}

// ── 临时中文提示 (到时恢复状态脸) ──
void DisplayState::showMessage(const char* cnText, uint32_t ms) {
    if (!_ready) return;
    utf8Copy(_msgText, sizeof(_msgText), cnText);   // UTF-8 安全截断 (v1g)
    _msgUntil = millis() + ms;

    Arduino_GFX* g = DisplayManager::canvas();
    g->fillScreen(0x0000);
    // 自动换行: 每行最多 8 字 (192px)
    int len = strlen(_msgText), lineStart = 0, row = 0;
    while (lineStart < len) {
        // 取 8 个"字符"(UTF-8 码点) 为一行
        int cpCount = 0, i = lineStart;
        while (i < len && cpCount < 8) {
            uint8_t c = (uint8_t)_msgText[i];
            i += (c < 0x80) ? 1 : (((c & 0xF0) == 0xE0) ? 3 : 2);
            cpCount++;
        }
        char line[32];
        // v1g: UTF-8 安全钳位 — 8 码点遇 4 字节 emoji 可能超 31B, 原 min() 会切坏字节
        int n = utf8ClampBytes(_msgText, lineStart, min(i - lineStart, (int)(sizeof(line) - 1)));
        strncpy(line, _msgText + lineStart, n);
        line[n] = '\0';
        int16_t w = cnTextWidth(line);
        drawCnText(g, (LCD_WIDTH - w) / 2, 84 + row * 34, line, 0xFFFF);
        row++;
        lineStart = i;
    }
    Serial.printf("[UI] msg: \"%s\" (%ums)\n", _msgText, ms);
}

// ── 彩蛋小表情 (5s 后回睡眠脸) ──
void DisplayState::setEaster(int idx) {
    if (!_ready) return;
    _easterIdx = (int8_t)idx;
    _easterUntil = millis() + 5000;
    _state = DISP_SLEEP;              // 彩蛋的"底层"是睡眠 (到期回睡眠脸)
    _msgUntil = 0;
    _zStep = -1;

    static const char* names[] = {"吐舌", "眨眼", "微笑"};
    Arduino_GFX* g = DisplayManager::canvas();
    g->fillScreen(0xF9C4);
    bool wink = (idx == 1);
    // 左眼
    if (wink) { g->drawLine(68, 100, 92, 100, 0x0000); }
    else { g->fillCircle(80, 100, 14, 0xFFFF); g->fillCircle(80, 100, 7, 0x0000); g->fillCircle(83, 97, 2, 0xFFFF); }
    // 右眼
    g->fillCircle(160, 100, 14, 0xFFFF); g->fillCircle(160, 100, 7, 0x0000); g->fillCircle(163, 97, 2, 0xFFFF);
    // 嘴: 微笑弧
    for (int a = 15; a <= 165; a += 4) {
        float rad = a * PI / 180.0f;
        g->fillCircle(120 + (int)(18 * cosf(rad)), 140 + (int)(18 * sinf(rad)) - 8, 2, 0x0000);
    }
    // 吐舌
    if (idx == 0) g->fillRoundRect(112, 152, 16, 14, 6, 0xF800);
    _drawLabel(names[idx % 3]);
    _drawChrome();
    Serial.printf("[UI] easter: %s (5s)\n", names[idx % 3]);
}

// ── WiFi 在线/离线 ──
void DisplayState::setOnline(bool ok) {
    if (_online == ok) return;
    _online = ok;
    _redraw();
}

// ── 主循环驱动: 消息/彩蛋到期 + 睡眠 Z 动画 ──
void DisplayState::update() {
    if (!_ready) return;
    uint32_t now = millis();

    if (_msgUntil && now >= _msgUntil) {
        _msgUntil = 0;
        _redraw();
    }
    if (_easterIdx >= 0 && now >= _easterUntil) {
        _easterIdx = -1;
        _redraw();
    }

    // 睡眠 Z 浮动: 每 1.6s 前进一步 (局部擦写, 不整屏刷)
    if (_state == DISP_SLEEP && !_msgUntil && _easterIdx < 0) {
        if (now - _lastZ >= 1600) {
            _lastZ = now;
            _drawZ(true);                          // 擦旧
            _zStep = (_zStep + 2) % 6;             // 0,2,4 → 三个位置循环
            _drawZ(false);                         // 画新
        }
    }
}

// ── 整屏重绘 (状态变化时) ──
void DisplayState::_redraw() {
    if (!_ready) return;
    if (_easterIdx >= 0) { setEaster(_easterIdx); _easterUntil = millis() + 5000; return; }
    if (_msgUntil) { showMessage(_msgText, _msgUntil - millis()); return; }
    _drawFace(_state);
    _drawChrome();
}

// ── 状态脸 ──
void DisplayState::_drawFace(DispStateId s) {
    Arduino_GFX* g = DisplayManager::canvas();
    uint16_t bg = stateBg(s);
    g->fillScreen(bg);

    switch (s) {
    case DISP_SLEEP:
        g->drawLine(68, 100, 92, 100, 0x0000);      // 闭眼
        g->drawLine(148, 100, 172, 100, 0x0000);
        g->drawLine(106, 140, 134, 140, 0x0000);    // 平嘴
        _drawLabel("睡眠");
        break;
    case DISP_LISTEN:
        g->fillCircle(80, 100, 14, 0xFFFF);  g->fillCircle(80, 100, 7, 0x0000);
        g->fillCircle(83, 97, 2, 0xFFFF);
        g->fillCircle(160, 100, 14, 0xFFFF); g->fillCircle(160, 100, 7, 0x0000);
        g->fillCircle(163, 97, 2, 0xFFFF);
        for (int a = 200; a <= 340; a += 4) {       // 小微笑 (∪)
            float rad = a * PI / 180.0f;
            g->fillCircle(120 + (int)(16 * cosf(rad)), 138 + (int)(16 * sinf(rad)), 2, 0x0000);
        }
        _drawLabel("聆听中");
        break;
    case DISP_THINK:
        g->fillCircle(80, 100, 14, 0xFFFF);  g->fillCircle(84, 95, 7, 0x0000);   // 瞳孔上瞟
        g->fillCircle(160, 100, 14, 0xFFFF); g->fillCircle(164, 95, 7, 0x0000);
        g->drawLine(106, 140, 126, 140, 0x0000);   // 歪嘴 + 手点
        g->fillCircle(134, 136, 3, 0x0000);
        _drawLabel("思考中");
        break;
    case DISP_SPEAK:
        g->fillCircle(80, 100, 14, 0xFFFF);  g->fillCircle(80, 100, 7, 0x0000);
        g->fillCircle(83, 97, 2, 0xFFFF);
        g->fillCircle(160, 100, 14, 0xFFFF); g->fillCircle(160, 100, 7, 0x0000);
        g->fillCircle(163, 97, 2, 0xFFFF);
        g->fillCircle(120, 140, 11, 0x0000);        // 张嘴 O
        _drawLabel("播报中");
        break;
    case DISP_WAIT: {
        g->fillCircle(80, 90, 11, 0xFFFF);  g->fillCircle(80, 90, 5, 0x0000);   // 半睁小眼
        g->fillCircle(160, 90, 11, 0xFFFF); g->fillCircle(160, 90, 5, 0x0000);
        g->drawLine(106, 126, 134, 126, 0x0000);
        _drawLabel("等待");
        _drawWaitNumber(_waitSec);
        break;
    }
    }
}

void DisplayState::_drawLabel(const char* cn) {
    Arduino_GFX* g = DisplayManager::canvas();
    int16_t w = cnTextWidth(cn);
    drawCnText(g, (LCD_WIDTH - w) / 2, 182, cn, 0xFFFF);
}

void DisplayState::_drawWaitNumber(int8_t sec) {
    if (sec < 0) return;
    Arduino_GFX* g = DisplayManager::canvas();
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", sec);
    g->setTextColor(0xFFFF, stateBg(DISP_WAIT));
    g->setTextSize(3);
    int16_t w = strlen(buf) * 18;
    g->setCursor((LCD_WIDTH - w) / 2, 146);
    g->print(buf);
}

void DisplayState::_drawChrome() {
    Arduino_GFX* g = DisplayManager::canvas();
    if (_online) {
        g->fillCircle(222, 18, 5, 0x07E0);          // 绿点
    } else {
        drawCnText(g, 190, 8, "离线", 0xF800);      // 红字
    }
}

// ── 睡眠 Z: 三个位置轮换 (小写 z, size2) ──
void DisplayState::_drawZ(bool erase) {
    Arduino_GFX* g = DisplayManager::canvas();
    if (_zStep < 0) return;
    static const int16_t zx[] = {150, 166, 182};
    static const int16_t zy[] = {72, 56, 40};
    int8_t i = _zStep / 2;
    if (erase) {
        if (_zStep < 0) return;
        g->fillRect(zx[_zStep / 2] - 2, zy[_zStep / 2] - 2, 20, 20, stateBg(DISP_SLEEP));
    } else {
        g->setTextColor(0xFFFF, stateBg(DISP_SLEEP));
        g->setTextSize(2);
        g->setCursor(zx[i], zy[i]);
        g->print('z');
    }
}
