/**
 * @file display_state.h
 * @brief P6 显示状态机 — 5 状态全中文 + 等待倒数 + 彩蛋表情
 *
 * 状态 (DispState):
 *   SLEEP  睡眠   闭眼瞌睡脸 + "睡眠"     常态/等待超时/睡觉命令
 *   LISTEN 聆听中 睁眼脸 + "聆听中"       唤醒词命中
 *   THINK  思考中 想事脸 + "思考中"       云端 STT/LLM/TTS、视觉识别
 *   SPEAK  播报中 说话脸 + "播报中"       TTS 播放
 *   WAIT   等待   待机脸 + "等待" + 大数字倒数  播完/光唤醒/失败后
 *
 * 叠加层:
 *   showMessage(中文, ms)  临时提示 (网络不好/拍照中...), 到时恢复底层状态脸
 *   setEaster(idx)         彩蛋小表情 (吐舌/眨眼/微笑), 5s 后回睡眠脸
 *   setOnline(ok)          右上角: 绿点=在线 / 红字"离线"
 *
 * 重绘策略: 状态或倒数数值变化才整屏重绘 (省 SPI 带宽, 防闪烁);
 *           SLEEP 有轻量 "Z" 浮动动画 (局部擦写)。
 */
#pragma once
#include <Arduino.h>
#include "display_manager.h"

enum DispStateId : uint8_t {
    DISP_SLEEP = 0,
    DISP_LISTEN,
    DISP_THINK,
    DISP_SPEAK,
    DISP_WAIT,
};

class DisplayState {
public:
    void begin(DisplayManager* dm);

    // ── 5 个基础状态 ──
    void setSleep();
    void setListen();
    void setThink();
    void setSpeak();
    void setWait(int8_t secRemaining);    // -1=不显示数字; 10..0 倒数

    // ── 叠加层 ──
    void showMessage(const char* cnText, uint32_t ms);  // 中文临时提示
    void setEaster(int idx);             // 0=吐舌 1=眨眼 2=微笑 (5s 后回睡眠)
    void setOnline(bool ok);             // WiFi 在线状态

    void update();                       // 主循环驱动 (消息到期/Z动画)

    DispStateId state() const { return _state; }

private:
    DisplayManager* _dm = nullptr;
    bool     _ready = false;

    DispStateId _state = DISP_SLEEP;
    int8_t   _waitSec = -1;              // 当前已绘制的倒数秒数
    bool     _online = true;

    // 临时消息
    char     _msgText[48];
    uint32_t _msgUntil = 0;

    // 彩蛋
    int8_t   _easterIdx = -1;            // -1=无
    uint32_t _easterUntil = 0;

    // 睡眠 Z 动画
    uint32_t _lastZ = 0;
    int8_t   _zStep = -1;

    void _redraw();                      // 按当前状态整屏重绘
    void _drawFace(DispStateId s);
    void _drawLabel(const char* cn);
    void _drawWaitNumber(int8_t sec);
    void _drawChrome();                  // 右上角在线/离线
    void _drawZ(bool erase);
};

// 中文 24x24 绘制 (display_state.cpp 实现, font_cn.h 字库)
void drawCnText(Arduino_GFX* g, int16_t x, int16_t y, const char* utf8, uint16_t color);
int16_t cnTextWidth(const char* utf8);
