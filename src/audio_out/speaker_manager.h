/**
 * @file speaker_manager.h
 * @brief MAX98357A I2S 功放管理器 (P2 实现 + P3 情绪提示音)
 *
 * 链路: ESP32-S3 I2S0 (master TX) → MAX98357A (I2S 标准模式)
 * 播放: 16kHz / 16bit / 双声道(mono 复制) / DMA 缓冲
 *
 * 设计:
 *  - 非阻塞: 正弦波预生成到 PSRAM, update() 每帧推 DMA, 不卡主循环
 *  - P2: 上电 C5-E5-G5-C6 音阶 (验证完成, P3 改为单音"叮")
 *  - P3: playChime() 情绪切换提示音 (不同情绪不同音)
 *  - 预留: write() 播放外部 PCM (WAV/TTS), setVolume() 软件音量
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "../companion/state_machine.h"   // CompanionEmotion

class SpeakerManager {
public:
    bool begin();                                  // 初始化 I2S0 TX + 引脚
    void update();                                 // 非阻塞驱动: 推 DMA + demo 序列
    bool playTone(uint16_t freqHz, uint32_t ms);   // 播放指定频率正弦波(非阻塞)
    bool isPlaying() const { return _len > 0; }    // 是否正在播放
    void setVolume(uint8_t pct) { _volPct = (pct > 100) ? 100 : pct; }  // 0~100
    uint8_t volume() const { return _volPct; }
    int  write(const int16_t* mono, int samples);  // 播放外部 PCM (16bit mono, 阻塞)

    /** P4: 非阻塞播放外部 PCM (TTS 音频)。接管 buffer 所有权,
     *  播完自动 free — 调用者不得再释放/复用该指针 */
    void playPCM(int16_t* pcm, size_t samples);

    /** v1z-A: 立即停止播放 — 清缓冲 + 清 DMA 残留 (唤醒打断播报用) */
    void stop();

    /** P3: 情绪切换提示音 (单音, 非阻塞; NEUTRAL/SAD 静音) */
    void playChime(CompanionEmotion e);

    /**
     * 声反馈门控: 距"最近一次系统自播声音"结束还有多少 ms (含尾音窗口),
     * 0 = 麦克风可以正常感知。用于抑制提示音被 INMP441 拾取 → 状态机
     * 误判"在说话" → 清零安静计时的自激回路。
     */
    uint32_t feedbackGateRemainingMs() const;

private:
    void _freeBuf();

    uint32_t _lastPlayStart = 0;    // 最近一次 playTone 起始时刻 (声反馈门控)
    uint32_t _lastPlayMs    = 0;    // 最近一次 playTone 时长 ms

    bool     _ready    = false;
    uint8_t  _volPct   = 30;        // 默认音量 30% (防爆音)
    int16_t* _buf      = nullptr;   // tone/PCM 缓冲 (PSRAM, speaker 拥有并负责 free)
    size_t   _len      = 0;         // mono sample 总数
    size_t   _pos      = 0;         // 已推入 DMA 的 sample 数
    int      _demoIdx  = 0;         // demo 音阶下标 (P3 不再使用)
    bool     _demoOn   = false;     // P3: 音阶 demo 关闭, 上电播单音
};
