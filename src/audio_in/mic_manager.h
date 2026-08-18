/**
 * @file mic_manager.h
 * @brief INMP441 I2S 麦克风管理器 — P2 实现 + P3 环境监听
 *
 * 功能:
 *   - I2S1 RX 经典 API 初始化 (WS=3 / SCK=46 / SD=20, 32bit slot)
 *   - read(): 流式读取 16bit mono PCM (P3 VAD / P4 录音用)
 *   - 环境监听 (P3): 常驻计算 RMS → getLevel() 返回 0~3 音量等级, 喂状态机
 *   - P2 录音 demo 保留但默认关闭 (setDemoEnabled(true) 可开)
 */
#pragma once

#include <stdint.h>

class WakeWord;   // P4-1 前向声明 (唤醒词检测, 同目录 wake_word.h)

class MicManager {
public:
    bool begin();

    /** P4-1: 挂载唤醒词检测器 (环境监听读到的 PCM 顺手喂给它, 零额外读取) */
    void setWakeWord(WakeWord* w) { _wake = w; }

    /** 流式读取 PCM (16bit mono), 返回实际读到的 sample 数, 0=超时/失败 */
    int read(int16_t* buffer, int samples);

    /** 非阻塞驱动: 环境监听 (常驻) + 录音 demo 状态机 (默认关) */
    void update();

    /** 当前环境音量等级: 0=静音 1=轻微 2=正常说话 3=大声 (供状态机) */
    int getLevel() const { return _envLevel; }

    /** 最近一帧 RMS (调试用) */
    double getRMS() const { return _envRMS; }

    /** P2 录音 demo 开关 (验证用; P3 默认关) */
    void setDemoEnabled(bool on) { _demoEnable = on; }

    /** v1v: 运行时调说话判定 RMS 下限 (80~2000, 默认 100; 串口 VADTH=xxx) */
    void setSpeechThreshold(double v) { if (v >= 80.0 && v <= 2000.0) _speechThr = v; }
    double getSpeechThreshold() const { return _speechThr; }

    bool isRecording() const { return _state == DemoState::RECORDING; }

    // ── P4: 对话录音 (VAD 触发, 非 demo) ──
    /** 开始对话录音: 分配 PSRAM buffer, 切换到对话录音模式 */
    void startConversationRecording();

    /** 停止对话录音: 返回 PCM 数据指针和采样数 (调用者负责 free) */
    uint32_t stopConversationRecording(int16_t** outBuf);

private:
    enum class DemoState : uint8_t { IDLE, RECORDING, SAVING, PLAYING, DONE };

    void _startDemo();
    void _saveAndPlay();
    void _updateEnvSense();
    void _envAccumulate(const int16_t* buf, int n);   // P4: 累加 RMS+ZCR 到环境窗口 (VAD)
    void _updateConvRecording();           // P4: 对话录音非阻塞驱动
    void _preRollPush(const int16_t* buf, int n);     // P4.6: 预滚环形缓冲写入

    bool       _ready = false;
    DemoState  _state = DemoState::IDLE;
    bool       _demoEnable = false;   // P2 录音 demo 默认关闭 (P3 环境监听为主)

    // 环境监听状态
    int        _envLevel = 0;         // 0~3
    double     _speechThr = 100.0;    // v12c: 说话 RMS 下限 (300→200→100 轻声可触发; VADTH= 实时调)
    double     _envRMS  = 0.0;        // 本窗原始 RMS (去DC)
    double     _envSmooth = 0.0;      // 一阶低通平滑 RMS (诊断显示)
    double     _envMaxRMS = 0.0;      // 区间 RMS 峰值 (诊断)
    double     _envZcrRate = 0.0;     // 本窗过零率 (0.0~1.0)
    double     _envNoiseFloor = 0.0;  // 自适应噪声基底 (滑动平均)
    int64_t    _envAcc  = 0;
    uint32_t   _envCnt  = 0;
    uint32_t   _envZcr = 0;           // 窗口过零计数
    int16_t    _envPrevAc = 0;        // 上一个去DC样本 (跨buffer算ZCR)
    bool       _envHavePrev = false;
    uint32_t   _envT    = 0;

    int16_t*   _recBuf = nullptr;   // PSRAM 录制缓冲
    uint32_t   _recCap = 0;         // 缓冲容量 (samples)
    uint32_t   _recPos = 0;         // 已录 (samples)
    uint32_t   _playPos = 0;        // 回放位置 (samples)
    int32_t    _peak  = 0;          // 峰值
    int64_t    _rmsAcc = 0;         // RMS 累加
    bool       _sdOk  = false;      // SD 卡是否挂载成功
    uint32_t   _demoCount = 0;      // WAV 文件名编号
    uint32_t   _tStart = 0;         // 录音起始 (防卡死超时)

    // P4: 对话录音 (独立于 demo)
    bool       _convRecording = false;
    int16_t*   _convBuf = nullptr;    // 对话录音 buffer (PSRAM, 10s @16kHz = 320KB)
    uint32_t   _convCap = 0;          // 容量 (samples)
    uint32_t   _convPos = 0;          // 已录 (samples)

    // P4.6: 预滚环形缓冲 (常驻保留最近 ~1.2s 音频)
    //   环境监听时持续写入; startConversationRecording 时拼到录音开头,
    //   补回 VAD 触发延迟丢失的句子开头 (单 tick 触发仍有 ~0.5s 窗口延迟)
    int16_t*   _preRollBuf = nullptr; // PSRAM, 19200 samples = 37.5KB
    uint32_t   _preRollCap = 0;
    uint32_t   _preRollPos = 0;       // 环形写位置
    uint32_t   _preRollCount = 0;     // 有效样本数 (≤cap)

    // P4-1: 唤醒词检测 (环境监听同一块 PCM 喂入; 喇叭提示音期间跳过防自激)
    WakeWord*  _wake = nullptr;
};
