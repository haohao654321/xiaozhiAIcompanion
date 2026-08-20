/**
 * @file speaker_manager.cpp
 * @brief MAX98357A I2S 功放管理器 — P2 实现
 *
 * 链路: ESP32-S3 I2S0 (master TX) → MAX98357A (I2S 标准模式)
 * 播放: 16kHz / 16bit / 双声道(mono 复制) / DMA 8x512
 *
 * API 选择: 经典 i2s_driver_install (core 3.0.7 的 SDK 无新 i2s_std API,
 *           此 API 稳定可用且资料最多)
 *
 * P2 验证: 上电播放 C5-E5-G5-C6 上行音阶, 每次 RST 重放
 */
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include "speaker_manager.h"
#include "../config/board_config.h"
#include "../config/system_config.h"   // P5 v1n: EMOTION_CHIMES_ENABLED

// ── 内部常量 ──
#define SPK_DMA_BUF_COUNT   8
#define SPK_DMA_BUF_LEN     512      // 每缓冲 512 帧 (双声道 16bit = 2048B)
#define SPK_CHUNK_SAMPLES   256      // 每次推 DMA 的 mono sample 数 (栈上 1KB)
// 门控尾音必须覆盖麦克风低通平滑的衰减时间: 平滑系数 0.7/0.3, 提示音
// 峰值 smooth(~1000-1300) 衰减到阈值 800 以下约需 4~7 窗 (~1-1.75s)。
// 之前 300ms 太短 → 门控关闭后 smooth 仍 >=800 → 状态机误判"说话" → 自激。
#define SPK_FEEDBACK_GATE_MS 1500    // 提示音结束后再静默 1500ms (平滑余韵)

// demo 上行音阶: C5 E5 G5 C6
static const uint16_t kDemoFreq[] = { 523, 659, 784, 1047 };
static const uint32_t kDemoMs[]   = { 250, 250, 250, 500 };
static const int      kDemoLen    = 4;

static const i2s_port_t kPort = (i2s_port_t)SPK_I2S_PORT;

// ── 初始化 ──
bool SpeakerManager::begin() {
    // 1) 安装 I2S 驱动 (master TX)
    i2s_config_t cfg = {};
    cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate       = SPK_SAMPLE_RATE;
    cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format    = I2S_CHANNEL_FMT_RIGHT_LEFT;   // mono 复制到双声道
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S; // MAX98357A 标准 I2S
    cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count     = SPK_DMA_BUF_COUNT;
    cfg.dma_buf_len       = SPK_DMA_BUF_LEN;
    cfg.use_apll          = false;
    cfg.tx_desc_auto_clear = true;

    esp_err_t err = i2s_driver_install(kPort, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[SPK] FAIL: i2s_driver_install err=0x%x\n", err);
        _ready = false;
        return false;
    }

    // 2) 绑定引脚
    i2s_pin_config_t pins = {};
    pins.bck_io_num   = SPK_BCLK_GPIO_NUM;
    pins.ws_io_num    = SPK_LRCK_GPIO_NUM;
    pins.data_out_num = SPK_DIN_GPIO_NUM;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    err = i2s_set_pin(kPort, &pins);
    if (err != ESP_OK) {
        Serial.printf("[SPK] FAIL: i2s_set_pin err=0x%x\n", err);
        i2s_driver_uninstall(kPort);
        _ready = false;
        return false;
    }

    _ready   = true;
    _demoIdx = 0;
    _demoOn  = false;                  // P3: 音阶 demo 关闭
    Serial.printf("[SPK] MAX98357A OK (I2S%d TX, %dHz, 16bit, vol=%d%%)\n",
                  SPK_I2S_PORT, SPK_SAMPLE_RATE, _volPct);
    return true;
}

// ── P3: 情绪切换提示音 (单音, 非阻塞; NEUTRAL/SAD 静音) ──
void SpeakerManager::playChime(CompanionEmotion e) {
    if (!_ready) return;
#if !EMOTION_CHIMES_ENABLED
    // v1n: 全部禁声 — THINKING/SLEEPY 由安静计时随机触发, SURPRISED 由睡着被吵醒触发,
    //      任一提示音 = 音长+1500ms 尾音门控, 会随机屏蔽 WakeNet 喂入 (v1m 实测 THINKING
    //      音正好盖住用户开口的 0.5s 语音段)。反馈交给屏幕表情/Toast。
    (void)e; return;
#else
    switch (e) {
        // v1m: HAPPY 不播提示音 — VAD 说话先于 WakeNet 触发 HAPPY,
        //      120ms 音 + 1500ms 门控 = 1620ms 屏蔽 WakeNet 喂入,
        //      "你好小智"后半段全被吞 → 唤醒零命中。屏幕 HAPPY 表情已够反馈。
        case EMOTION_HAPPY:                             break;   // (was playTone(659,120))
        case EMOTION_SURPRISED: playTone(1047, 100);  break;   // C6 尖亮
        case EMOTION_THINKING:  playTone(392, 150);   break;   // G4 低沉
        case EMOTION_SLEEPY:    playTone(220, 300);   break;   // A3 极低轻
        default:                break;                        // NEUTRAL/SAD 静音
    }
#endif
}

// ── 非阻塞驱动 ──
void SpeakerManager::update() {
    if (!_ready) return;

    // 1) 推当前 tone 到 DMA (尽量写, 不阻塞)
    if (_len > 0) {
        int16_t tmp[SPK_CHUNK_SAMPLES * 2];   // mono → stereo 复制缓冲
        while (_pos < _len) {
            size_t chunk = (_len - _pos < SPK_CHUNK_SAMPLES) ? (_len - _pos) : SPK_CHUNK_SAMPLES;
            for (size_t i = 0; i < chunk; i++) {
                tmp[i * 2]     = _buf[_pos + i];   // 左
                tmp[i * 2 + 1] = _buf[_pos + i];   // 右
            }
            size_t bytes = 0;
            esp_err_t err = i2s_write(kPort, tmp, chunk * 2 * sizeof(int16_t), &bytes, 0);
            if (err != ESP_OK || bytes == 0) break;   // DMA 满, 下一帧再推
            _pos += chunk;
        }
        if (_pos >= _len) _freeBuf();   // 播完释放 PSRAM
    }

    // 2) demo 序列: 上一音播完且 demo 未结束 → 播下一个 (连续无间隔)
    if (_demoOn && _len == 0) {
        if (_demoIdx < kDemoLen) {
            playTone(kDemoFreq[_demoIdx], kDemoMs[_demoIdx]);
            _demoIdx++;
        } else {
            _demoOn = false;   // 整个音阶播完, demo 结束
        }
    }
}

// ── 声反馈门控: 播放中 + 尾音余韵窗口内返回剩余 ms (0 = 可感知) ──
//   提示音/系统自播声音会被桌面上的 INMP441 直接拾取, 若状态机把它当成
//   "有人在说话" 会清零安静计时 → 永远进不了 THINKING/SLEEPY, 且切情绪
//   播提示音 → 又被听到 → 再切, 形成自激。此窗口内必须忽略麦克风。
//   v2c: 对于长时播放(TTS/音乐 2-20s), 门控最多 500ms — 用户需要能在播放中
//   唤醒词打断, 不能整个播放期都屏蔽 WakeNet 喂入。
uint32_t SpeakerManager::feedbackGateRemainingMs() const {
    uint32_t end = _lastPlayStart + _lastPlayMs + SPK_FEEDBACK_GATE_MS;
    uint32_t now = millis();
    uint32_t remaining = (now < end) ? (end - now) : 0;
    // 长时播放: 门控最多 500ms (TTS/音乐期间用户需能唤醒打断)
    if (remaining > 500) remaining = 500;
    return remaining;
}

// ── 播放指定频率正弦波 (非阻塞) ──
bool SpeakerManager::playTone(uint16_t freqHz, uint32_t ms) {
    if (!_ready) return false;
    _freeBuf();

    _lastPlayStart = millis();      // 声反馈门控起点
    _lastPlayMs    = ms;

    uint32_t n = (uint32_t)SPK_SAMPLE_RATE * ms / 1000;
    if (n == 0) return false;

    // 波形放 PSRAM, 不占内部 heap (1s 最长 ~32KB)
    _buf = (int16_t*)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_buf) {
        Serial.println("[SPK] tone buffer alloc failed");
        return false;
    }

    const float amp = 32767.0f * _volPct / 100.0f;
    for (uint32_t i = 0; i < n; i++) {
        _buf[i] = (int16_t)(amp * sinf(2.0f * M_PI * freqHz * i / SPK_SAMPLE_RATE));
    }
    _len = n;
    _pos = 0;
    return true;
}

// ── 播放外部 PCM (16bit mono, 阻塞) — P3/P4 用 ──
int SpeakerManager::write(const int16_t* mono, int samples) {
    if (!_ready || !mono || samples <= 0) return 0;
    _demoOn = false;   // 外部播放暂停 demo
    _freeBuf();        // 清掉 tone

    int16_t stereo[512 * 2];
    int written = 0;
    while (written < samples) {
        int chunk = (samples - written < 512) ? (samples - written) : 512;
        for (int i = 0; i < chunk; i++) {
            stereo[i * 2]     = mono[written + i];
            stereo[i * 2 + 1] = mono[written + i];
        }
        size_t bytes = 0;
        esp_err_t err = i2s_write(kPort, stereo, chunk * 2 * sizeof(int16_t), &bytes, portMAX_DELAY);
        if (err != ESP_OK) break;
        written += (int)(bytes / (2 * sizeof(int16_t)));
    }
    return written;
}

// ── P4: 非阻塞播放外部 PCM (TTS 音频) ──
//   接管 buffer 所有权, 播完后由 speaker 自动 free
void SpeakerManager::playPCM(int16_t* pcm, size_t samples) {
    if (!_ready || !pcm || samples == 0) return;
    _demoOn = false;
    _freeBuf();   // 清掉当前 tone (free 旧 buffer)

    _buf     = pcm;       // 接管所有权
    _len     = samples;
    _pos     = 0;

    _lastPlayStart = millis();  // 声反馈门控起点
    _lastPlayMs    = (uint32_t)(samples * 1000UL / SPK_SAMPLE_RATE);
}

// ── v1z-A: 立即停止播放 (唤醒打断播报用) ──
//   1) _freeBuf() 释放未播完的 PCM/tone 缓冲
//   2) i2s_zero_dma_buffer 清掉 DMA 里已排队的数据 → 喇叭立即静音
void SpeakerManager::stop() {
    if (!_ready) return;
    _freeBuf();
    i2s_zero_dma_buffer(kPort);
}

// ── 释放缓冲 (tone + 外部 PCM 统一 free) ──
void SpeakerManager::_freeBuf() {
    if (_buf) {
        heap_caps_free(_buf);
        _buf = nullptr;
    }
    _len = 0;
    _pos = 0;
}
