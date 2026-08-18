/**
 * @file mic_manager.cpp
 * @brief INMP441 I2S 麦克风管理器 — P2 实现
 *
 * 链路: INMP441 → ESP32-S3 I2S1 (master RX, 32bit slot)
 * 采样: 16kHz / 16bit mono (INMP441 24bit 数据左对齐于 32bit 槽, 取高 16 位)
 *
 * P2 验证 (demo):
 *   上电 → 自动录 4 秒环境音 (PSRAM 缓冲) → 写 WAV 到 SD 卡
 *        → 串口打印 peak/RMS → 喇叭回放刚才的声音 (mic → SD → speaker 全链路)
 *   每按一次 RST 重录一轮
 *
 * 注意:
 *   - INMP441 L/R 引脚接 GND 时数据在左声道 (WS 高) 输出
 *   - 经典 i2s_driver_install API (与 speaker 一致, core 3.0.7)
 *   - SD 卡 1-bit SDMMC (CLK=39 CMD=38 D0=40), 无卡则跳过保存只回放
 */
#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <FS.h>
#include <SD_MMC.h>
#include "mic_manager.h"
#include "wake_word.h"                    // P4-1: 唤醒词检测
#include "../config/board_config.h"
#include "../audio_out/speaker_manager.h"

// ── 内部常量 ──
#define MIC_DMA_BUF_COUNT   8
#define MIC_DMA_BUF_LEN     512          // 每缓冲 512 帧 (32bit slot)
#define MIC_REC_SECONDS     4
#define MIC_REC_SAMPLES     (MIC_SAMPLE_RATE * MIC_REC_SECONDS)  // 64000 = 128KB PSRAM
#define MIC_CHUNK           1024         // 每帧 update 读入块
#define MIC_PLAY_CHUNK      2048         // 每帧回放推块 (约 8KB stereo)
#define MIC_REC_TIMEOUT_MS  12000        // 防卡死兜底
#define MIC_PREROLL_S       2            // 预滚缓冲秒数 (P5: 1→2s, 兜住唤醒检测偏晚, 命令词完整落入)
#define MIC_DRAIN_MAX       6            // 每次 update 最多排空轮数 (6×512=192ms, 防卡)

// ── 环境监听 (P3): Energy + ZCR 双特征 VAD ──
//   参考: ESP32 社区成熟方案, 能量(RMS)+过零率(ZCR) 双阈值判定
//   纯 RMS 阈值无法区分"人声"和"电噪声/振动" → 误触发 HAPPY
//   ZCR 能有效区分: 人声有特定过零率范围, 电噪声/振动特征不同
//   16bit 满量程 32767; 静音噪声底 ~10-80, 轻声 ~几百, 说话 ~几千
#define MIC_ENV_WINDOW_MS   250          // RMS/ZCR 计算窗口
#define MIC_ENV_LV0_RMS     80            //  < 80    → level 0 (静音)
// v12c: 说话能量下限改为运行时成员 _speechThr (默认 100, 串口 VADTH= 实时调)
//   历史: v11 600 → v12 350 → v12b 300 → v12c 200 → v12c 100(NEAR 数据: 正常说话 rms 400-575, 轻声 200-400)
//   注意: 实际说话阈值 = max(_speechThr, 噪声基底×3); 安静房间 nf~70 → 有效阈值~210, 调 100 主要影响极安静环境
#define MIC_ENV_LV2_RMS     8000          //  < 8000  → level 2 (正常说话)
                                          //  >=8000  → level 3 (大声/近麦)
// ZCR 人声范围 (@16kHz 采样):
//   人声基频 80~400Hz → ZCR 0.01~0.05 (元音); 辅音 → ZCR 0.10~0.45
//   电源纹波 50Hz → ZCR~0.006; 桌面振动 → ZCR<0.01; 白噪声 → ZCR~0.5
#define MIC_ZCR_VOICE_MIN   0.01         // 过零率下限 (滤低频噪声/振动)
#define MIC_ZCR_VOICE_MAX   0.45         // 过零率上限 (滤高频电噪声)
// 自适应噪声基底: 安静时滑动平均, 说话阈值 = max(固定, 噪声基底×3)
#define MIC_NOISE_FLOOR_K   0.95         // 噪声基底平滑系数 (越大越慢)
#define MIC_SPEECH_MULT      3.0          // 说话阈值 = 噪声基底 × 此系数
// 平滑系数 (仅诊断显示, 不参与判定)
#define MIC_SMOOTH_UP       0.55
#define MIC_SMOOTH_DOWN     0.15

static const i2s_port_t kPort = (i2s_port_t)MIC_I2S_PORT;
extern SpeakerManager speaker;           // main.cpp 全局实例 (demo 回放用)
static uint32_t s_wakeGatedSkips = 0;    // P4-1 诊断: 喇叭门控跳过喂唤醒词的 update 次数

// ── 初始化 ──
bool MicManager::begin() {
    // 1) 安装 I2S 驱动 (master RX, 32bit slot)
    i2s_config_t cfg = {};
    cfg.mode              = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate       = MIC_SAMPLE_RATE;
    cfg.bits_per_sample   = I2S_BITS_PER_SAMPLE_32BIT;   // INMP441 24bit 数据 / 32bit 帧
    cfg.channel_format    = I2S_CHANNEL_FMT_RIGHT_LEFT;  // 帧 = [L 32bit][R 32bit], 取左槽
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags  = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count     = MIC_DMA_BUF_COUNT;
    cfg.dma_buf_len       = MIC_DMA_BUF_LEN;
    cfg.use_apll          = false;

    esp_err_t err = i2s_driver_install(kPort, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[MIC] FAIL: i2s_driver_install err=0x%x\n", err);
        _ready = false;
        return false;
    }

    // 2) 绑定引脚
    i2s_pin_config_t pins = {};
    pins.bck_io_num    = MIC_SCK_GPIO_NUM;   // BCLK
    pins.ws_io_num     = MIC_WS_GPIO_NUM;    // LRCK
    pins.data_in_num   = MIC_SD_GPIO_NUM;    // SD
    pins.data_out_num  = I2S_PIN_NO_CHANGE;
    err = i2s_set_pin(kPort, &pins);
    if (err != ESP_OK) {
        Serial.printf("[MIC] FAIL: i2s_set_pin err=0x%x\n", err);
        i2s_driver_uninstall(kPort);
        _ready = false;
        return false;
    }

    // 3) 清掉 DMA 上电残留 (避免首帧垃圾数据)
    int16_t flush[MIC_CHUNK];
    read(flush, MIC_CHUNK);

    // 4) 录制缓冲放 PSRAM (128KB, 不占内部 heap)
    _recCap = MIC_REC_SAMPLES;
    _recBuf = (int16_t*)heap_caps_malloc(_recCap * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_recBuf) {
        Serial.println("[MIC] WARN: PSRAM record buffer alloc failed (4s @16kHz = 128KB)");
        _recCap = 0;
    }

    // 4b) 预滚环形缓冲 (P4.6): 最近 1.2s 音频, 触发录音时拼开头
    _preRollCap = MIC_SAMPLE_RATE * MIC_PREROLL_S;
    _preRollBuf = (int16_t*)heap_caps_malloc(_preRollCap * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_preRollBuf) {
        Serial.println("[MIC] WARN: pre-roll buffer alloc failed (对话开头可能缺字)");
        _preRollCap = 0;
    }

    // 5) SD 卡 (1-bit SDMMC; 无卡不阻塞, 录音只回放不保存)
    SD_MMC.setPins(SD_MMC_CLK_GPIO_NUM, SD_MMC_CMD_GPIO_NUM, SD_MMC_D0_GPIO_NUM);
    if (SD_MMC.begin("/sdcard", true)) {          // true = 1-bit 模式 (只接 D0)
        _sdOk = true;
        Serial.printf("[MIC] SD card OK: %llu MB free\n",
                      (unsigned long long)(SD_MMC.totalBytes() - SD_MMC.usedBytes()) / 1048576);
    } else {
        Serial.println("[MIC] SD card NOT found (recording will play back only)");
    }

    _ready = true;
    _state = DemoState::IDLE;
    _demoEnable = false;   // P3: 默认环境监听模式, 录音 demo 关闭
    _envT = millis();
    Serial.printf("[MIC] INMP441 OK (I2S%d RX, %dHz, 32bit slot)\n",
                  MIC_I2S_PORT, MIC_SAMPLE_RATE);
    Serial.printf("[MIC]   VAD: %dms win | Energy+ZCR dual | zcr:[%.2f~%.2f] thr>=%.0f+nf*%.0f | NEAR>=100\n",
                  MIC_ENV_WINDOW_MS, (double)MIC_ZCR_VOICE_MIN, (double)MIC_ZCR_VOICE_MAX,
                  _speechThr, (double)MIC_SPEECH_MULT);
    return true;
}

// ── 流式读取: 32bit 帧 [L][R] → 取左槽高 16 位 (24bit→16bit) ──
int MicManager::read(int16_t* buffer, int samples) {
    if (!_ready || !buffer || samples <= 0) return 0;

    int got = 0;
    int32_t frames[64 * 2];            // 64 帧 = 512B, 栈上
    while (got < samples) {
        size_t bytes = 0;
        esp_err_t err = i2s_read(kPort, frames, sizeof(frames), &bytes, 20 / portTICK_PERIOD_MS);
        if (err != ESP_OK) break;
        int nFrames = (int)(bytes / 8);
        if (nFrames == 0) break;       // 超时无数据
        for (int i = 0; i < nFrames && got < samples; i++) {
            // INMP441: 24bit 数据左对齐于 32bit 槽; L/R 接 GND → 左槽 (WS 高)
            buffer[got++] = (int16_t)(frames[i * 2] >> 16);   // 取高 16 位有效, 不削波
        }
    }
    return got;
}

// ── 非阻塞驱动: 环境监听(常驻) + 对话录音 + demo(默认关) ──
void MicManager::update() {
    if (!_ready) return;

    // P4: 对话录音模式 — 录音数据同时喂 VAD (静音检测), 不单独读 I2S
    //     (若环境监听和录音各读一路, I2S 流会被瓜分 → 录音丢样本)
    if (_convRecording) {
        _updateConvRecording();
        return;
    }

    // P3: 环境监听 (每次 update 读一小块, 250ms 窗口算 RMS → 音量等级)
    _updateEnvSense();

    // P2 录音 demo (验证用, 默认关闭)
    if (!_demoEnable) return;

    switch (_state) {
    case DemoState::IDLE:
        _startDemo();
        break;

    case DemoState::RECORDING: {
        if (millis() - _tStart > MIC_REC_TIMEOUT_MS) {
            Serial.println("[MIC] WARN: record timeout (no mic data?), forcing end");
            _recPos = _recCap;         // 强制结束
            break;
        }
        int want = (int)((_recCap - _recPos) < MIC_CHUNK ? (_recCap - _recPos) : MIC_CHUNK);
        int got = read(_recBuf + _recPos, want);
        if (got > 0) {
            for (int i = 0; i < got; i++) {
                int32_t s = _recBuf[_recPos + i];
                if (s < 0) s = -s;
                if (s > _peak) _peak = s;
                _rmsAcc += (int64_t)s * s;
            }
            _recPos += got;
        }
        if (_recPos >= _recCap) {
            double rms = (_recPos > 0) ? sqrt((double)_rmsAcc / _recPos) : 0.0;
            Serial.printf("[MIC] Captured %u samples (%.1fs) | peak=%d | RMS=%.0f\n",
                          _recPos, _recPos / (float)MIC_SAMPLE_RATE, _peak, rms);
            _state = DemoState::SAVING;
        }
        break;
    }

    case DemoState::SAVING:
        _saveAndPlay();
        break;

    case DemoState::PLAYING: {
        int remain = (int)(_recCap - _playPos);
        int chunk  = (remain < MIC_PLAY_CHUNK) ? remain : MIC_PLAY_CHUNK;
        int w = speaker.write(_recBuf + _playPos, chunk);   // 阻塞一小段, 可接受
        if (w > 0) _playPos += w;
        if (w <= 0 || _playPos >= _recCap) {
            _state = DemoState::DONE;
            Serial.println("[MIC] Playback finished (press RST to record again)");
        }
        break;
    }

    case DemoState::DONE:
        // 保持完成态, 等待 RST 重录
        break;
    }
}

// ── 环境监听: 排空式读取 → 250ms 窗口 RMS+ZCR → Energy+ZCR 双特征 VAD ──
//   参考: ESP32 社区成熟方案 (Energy + Zero Crossing Rate dual-threshold VAD)
//   纯 RMS 无法区分人声和电噪声/振动 → 误触发;
//   ZCR 能有效区分: 人声 0.01~0.45, 电源纹波/振动 <0.01, 高频电噪声 >0.45
//   P4.6 排空式: 旧版每轮只读 128 样本(8ms), 循环一慢(WiFi/视频流/串口打印)
//   就欠采样 → DMA 溢出丢样本。现在循环读到 DMA 空, 保证实时完整采集。
void MicManager::_updateEnvSense() {
    int16_t buf[512];                   // 512 sample = 32ms @16kHz
    // P4-1: 喇叭提示音期间不喂唤醒词 (自己的声音防自激唤醒)
    bool wakeGated = speaker.feedbackGateRemainingMs() > 0;
    if (wakeGated) s_wakeGatedSkips++;
    for (int round = 0; round < MIC_DRAIN_MAX; round++) {
        int got = read(buf, 512);
        if (got <= 0) break;
        _envAccumulate(buf, got);
        _preRollPush(buf, got);
        if (_wake && !wakeGated) _wake->feed(buf, got);   // P4-1: 同一块 PCM 喂 WakeNet
        if (got < 512) break;           // DMA 已空, 读干净了
    }
}

// ── 将一段 PCM 累加进环境窗口 (RMS+ZCR), 窗口到点做 VAD 判定 ──
//   普通模式: _updateEnvSense 读入后调用; 对话录音模式: 录音数据喂入,
//   保证 VAD 持续更新 (静音检测依赖 getLevel()) 且不丢录音样本
void MicManager::_envAccumulate(const int16_t* buf, int n) {
    // 计算 buffer DC offset (INMP441 可能有 DC bias, 影响 ZCR 准确性)
    int32_t dcSum = 0;
    for (int i = 0; i < n; i++) dcSum += buf[i];
    int16_t dc = (n > 0) ? (int16_t)(dcSum / n) : 0;

    // 累加 RMS (去DC) + 过零率 (去DC后符号变化)
    for (int i = 0; i < n; i++) {
        int16_t ac = buf[i] - dc;        // 去 DC
        int32_t s = ac;
        if (s < 0) s = -s;
        _envAcc += (int64_t)s * s;
        _envCnt++;

        // ZCR: 相邻样本符号变化 (跨 buffer 边界连续)
        if (i > 0) {
            int16_t prevAc = buf[i - 1] - dc;
            if ((ac >= 0) != (prevAc >= 0)) _envZcr++;
        } else if (_envHavePrev) {
            if ((ac >= 0) != (_envPrevAc >= 0)) _envZcr++;
        }
    }
    // 保存最后一个去DC样本供下次跨 buffer ZCR 使用
    if (n > 0) {
        _envPrevAc = buf[n - 1] - dc;
        _envHavePrev = true;
    }

    if (millis() - _envT >= MIC_ENV_WINDOW_MS) {
        _envRMS = (_envCnt > 0) ? sqrt((double)_envAcc / _envCnt) : 0.0;
        _envZcrRate = (_envCnt > 1) ? (double)_envZcr / (_envCnt - 1) : 0.0;
        _envAcc = 0;
        _envCnt = 0;
        _envZcr = 0;
        _envT   = millis();

        // 一阶低通 (仅诊断显示, 不参与判定)
        double k = (_envRMS > _envSmooth) ? MIC_SMOOTH_UP : MIC_SMOOTH_DOWN;
        _envSmooth = _envSmooth + k * (_envRMS - _envSmooth);

        // ── Energy + ZCR 双特征 VAD ──
        // 自适应说话阈值 = max(运行时下限 _speechThr, 噪声基底 × 3)
        double speechThr = _speechThr;
        if (_envNoiseFloor > 1.0) {
            double dyn = _envNoiseFloor * MIC_SPEECH_MULT;
            if (dyn > speechThr) speechThr = dyn;
        }

        bool hasEnergy = _envRMS >= speechThr;
        bool isVoice   = (_envZcrRate >= MIC_ZCR_VOICE_MIN &&
                          _envZcrRate <= MIC_ZCR_VOICE_MAX);

        if      (_envRMS < MIC_ENV_LV0_RMS) _envLevel = 0;     // 静音
        else if (!hasEnergy)                 _envLevel = 1;     // 环境底噪
        else if (!isVoice)                   _envLevel = 1;     // 有能量但非人声 (电噪声/振动)
        else if (_envRMS < MIC_ENV_LV2_RMS) _envLevel = 2;     // 人声 (正常说话)
        else                                 _envLevel = 3;     // 人声 (大声/近麦)

        // 非说话时更新噪声基底 (滑动平均)
        if (_envLevel < 2) {
            if (_envNoiseFloor < 1.0) _envNoiseFloor = _envRMS;
            else _envNoiseFloor = _envNoiseFloor * MIC_NOISE_FLOOR_K
                                + _envRMS * (1.0 - MIC_NOISE_FLOOR_K);
        }

        // 跟踪 RMS 峰值 (诊断)
        if (_envRMS > _envMaxRMS) _envMaxRMS = _envRMS;

        // 诊断 A: 人声判定实时打印
        if (_envLevel >= 2) {
            Serial.printf("[MIC] **SPEECH** lv=%d rms=%.0f zcr=%.3f nf=%.0f thr=%.0f\n",
                          _envLevel, _envRMS, _envZcrRate, _envNoiseFloor, speechThr);
        }
        // 诊断 A2: 高 RMS 但被 ZCR 滤掉 (确认过滤效果)
        else if (_envRMS >= _speechThr) {
            Serial.printf("[MIC] **NOISE** rms=%.0f zcr=%.3f (filtered)\n",
                          _envRMS, _envZcrRate);
        }
        // 诊断 A3: NEAR_MISS — rms>=100 但未达说话阈值 (v11: 排查"叫没反应")
        //   如果用户在说话但 rms 只有 100-599, 说明 mic 灵敏度/距离问题
        else if (_envRMS >= 100) {
            Serial.printf("[MIC] **NEAR** rms=%.0f zcr=%.3f lv=%d (below thr=%.0f)\n",
                          _envRMS, _envZcrRate, _envLevel, speechThr);
        }

        // 诊断 B: 每 5s 打印环境快照
        static uint32_t s_dbgT = 0;
        if (millis() - s_dbgT >= 5000) {
            s_dbgT = millis();
            Serial.printf("[MIC] env lv=%d rms=%.0f zcr=%.3f nf=%.0f maxRMS=%.0f wakeGate=%u (thr: %.0f/%.0f/%.0f)\n",
                          _envLevel, _envRMS, _envZcrRate, _envNoiseFloor, _envMaxRMS, s_wakeGatedSkips,
                          (double)MIC_ENV_LV0_RMS, _speechThr,
                          (double)MIC_ENV_LV2_RMS);
            _envMaxRMS = 0.0;
        }
    }
}

// ── 开始一轮录制 demo ──
void MicManager::_startDemo() {
    if (!_recBuf || _recCap == 0) {
        _state = DemoState::DONE;
        return;
    }
    _recPos = 0;
    _playPos = 0;
    _peak = 0;
    _rmsAcc = 0;
    _tStart = millis();
    _state = DemoState::RECORDING;
    Serial.printf("[MIC] Recording %ds ... speak near the mic!\n", MIC_REC_SECONDS);
}

// ── 存 WAV → 开始回放 ──
void MicManager::_saveAndPlay() {
    uint32_t n = _recPos;

    if (_sdOk && n > 0) {
        char path[32];
        snprintf(path, sizeof(path), "/sdcard/rec_%lu.wav", (unsigned long)(++_demoCount));
        File f = SD_MMC.open(path, FILE_WRITE);
        if (f) {
            // 写 44 字节 WAV 头 (PCM / mono / 16kHz / 16bit)
            uint8_t hdr[44];
            memset(hdr, 0, sizeof(hdr));
            memcpy(hdr + 0,  "RIFF", 4);
            uint32_t v32 = 36 + n * 2;             memcpy(hdr + 4,  &v32, 4);
            memcpy(hdr + 8,  "WAVE", 4);
            memcpy(hdr + 12, "fmt ", 4);
            v32 = 16;                              memcpy(hdr + 16, &v32, 4);
            uint16_t v16 = 1;                      memcpy(hdr + 20, &v16, 2);  // PCM
            v16 = 1;                               memcpy(hdr + 22, &v16, 2);  // mono
            v32 = MIC_SAMPLE_RATE;                 memcpy(hdr + 24, &v32, 4);
            v32 = MIC_SAMPLE_RATE * 2;             memcpy(hdr + 28, &v32, 4);  // byte rate
            v16 = 2;                               memcpy(hdr + 32, &v16, 2);  // block align
            v16 = 16;                              memcpy(hdr + 34, &v16, 2);  // bits
            memcpy(hdr + 36, "data", 4);
            v32 = n * 2;                           memcpy(hdr + 40, &v32, 4);

            f.write(hdr, sizeof(hdr));
            size_t w = f.write((const uint8_t*)_recBuf, n * 2);
            f.close();
            Serial.printf("[MIC] Saved %s (%zu / %u bytes)\n", path, w, n * 2);
        } else {
            Serial.println("[MIC] WAV open failed (SD write error)");
        }
    } else {
        Serial.println("[MIC] SD not available, skipping WAV save");
    }

    _playPos = 0;
    _state = DemoState::PLAYING;
    Serial.println("[MIC] Playing back captured audio ...");
}

// ════════════════════════════════════════════════════════════════
// P4: 对话录音 (VAD 触发, 非 demo)
// ════════════════════════════════════════════════════════════════

void MicManager::startConversationRecording() {
    // 分配 PSRAM buffer: 10s @ 16kHz = 160000 samples = 320KB
    if (!_convBuf) {
        _convCap = MIC_SAMPLE_RATE * 10;   // 最大 10s
        _convBuf = (int16_t*)heap_caps_malloc(_convCap * sizeof(int16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_convBuf) {
            Serial.println("[MIC] WARN: conv record PSRAM alloc failed (320KB)");
            _convCap = 0;
            return;   // 保持非录音状态, conversation_manager 会因录音太短放弃
        }
    }
    _convPos = 0;

    // P4.6: 拼接预滚缓冲 → 补回 VAD 触发延迟 (窗口+调度 ~0.5s) 丢掉的句子开头
    uint32_t pre = 0;
    if (_preRollBuf && _preRollCount > 0) {
        pre = (_preRollCount < _convCap) ? _preRollCount : _convCap;
        // 环形缓冲按时间顺序拷出: 从"最老样本"位置开始绕环两段拷贝
        uint32_t start = (_preRollPos + _preRollCap - pre) % _preRollCap;
        uint32_t first = (_preRollCap - start < pre) ? (_preRollCap - start) : pre;  // start→缓冲末尾
        memcpy(_convBuf, _preRollBuf + start, first * sizeof(int16_t));
        memcpy(_convBuf + first, _preRollBuf, (pre - first) * sizeof(int16_t));
        _convPos = pre;
    }

    _convRecording = true;
    Serial.printf("[MIC] Conv recording START (max %.0fs, buf=%uKB, preroll=%.1fs)\n",
                  (float)_convCap / MIC_SAMPLE_RATE,
                  (unsigned)(_convCap * 2 / 1024),
                  pre / (float)MIC_SAMPLE_RATE);
}

uint32_t MicManager::stopConversationRecording(int16_t** outBuf) {
    _convRecording = false;
    uint32_t n = _convPos;
    *outBuf = _convBuf;   // 转移所有权, 调用者负责 free
    _convBuf = nullptr;
    _convCap = 0;
    _convPos = 0;
    Serial.printf("[MIC] Conv recording STOP: %u samples (%.1fs)\n", n, n / 16000.0f);
    return n;
}

void MicManager::_updateConvRecording() {
    if (!_convBuf || _convCap == 0) return;

    // P4.6: 排空式读取 — 循环一慢就欠采样 → 音频断续畸变 → STT 只能猜出单字
    int16_t buf[512];
    for (int round = 0; round < MIC_DRAIN_MAX; round++) {
        int got = read(buf, 512);
        if (got <= 0) break;

        // 录音数据同时喂 VAD — 对话期间静音检测依赖 getLevel(), 且避免双读分流
        _envAccumulate(buf, got);

        int space = (int)(_convCap - _convPos);
        int toCopy = (got < space) ? got : space;
        memcpy(_convBuf + _convPos, buf, toCopy * sizeof(int16_t));
        _convPos += toCopy;

        if (got < 512) break;           // DMA 已空
    }

    // 满 10s 自动停止 (conversation_manager 会在下一 tick 调 stop)
    if (_convPos >= _convCap) {
        Serial.println("[MIC] Conv buffer full (10s)");
        _convRecording = false;   // 停止读取, 等 stop 调用取数据
    }
}

// ── P4.6: 预滚环形缓冲写入 (环境监听时常驻保留最近 N 秒) ──
void MicManager::_preRollPush(const int16_t* buf, int n) {
    if (!_preRollBuf || _preRollCap == 0) return;
    for (int i = 0; i < n; i++) {
        _preRollBuf[_preRollPos] = buf[i];
        _preRollPos = (_preRollPos + 1) % _preRollCap;
        if (_preRollCount < _preRollCap) _preRollCount++;
    }
}
