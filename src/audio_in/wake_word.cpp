/**
 * @file wake_word.cpp
 * @brief P4-1 离线唤醒词检测 — esp-sr WakeNet9 实现
 *
 * 依赖 (见 lib/esp-sr, tools/link_espsr.py):
 *   libwakenet.a + libc_speech_features.a + libdl_lib.a (预编译, ESP32-S3)
 *   + lib/esp-dsp-subset 源码 (fft/dotprod, WakeNet 特征提取需要)
 *   + src: model_path.c (esp_srmodel_init 从 model 分区 mmap 模型)
 *
 * 模型: tools/srmodels.bin (wn9_nihaoxiaozhi, 284KB) 烧到 0x520000 model 分区
 */
#include <Arduino.h>
#include "wake_word.h"
#include "../config/system_config.h"

bool WakeWord::begin(const char* partitionLabel) {
    // 1) 从 flash "model" 分区加载模型清单 (mmap, 不占 RAM)
    _models = esp_srmodel_init(partitionLabel);
    if (_models == nullptr || _models->num == 0) {
        Serial.printf("[WAKE] FAIL: no model in partition '%s' "
                      "(烧 tools/srmodels.bin 到 0x%x 后重试)\n",
                      partitionLabel, WAKE_MODEL_ADDR);
        return false;
    }

    // 2) 找 wakenet 模型 (前缀 "wn")
    _modelName = esp_srmodel_filter(_models, ESP_WN_PREFIX, NULL);
    if (_modelName == nullptr) {
        Serial.println("[WAKE] FAIL: no wakenet model found in partition");
        return false;
    }

    // 3) 唤醒词实例
    _wn = esp_wn_handle_from_name(_modelName);
    if (_wn == nullptr) {
        Serial.printf("[WAKE] FAIL: no wakenet handle for %s\n", _modelName);
        return false;
    }
    _wnData = _wn->create(_modelName, WAKE_DETECT_MODE);
    if (_wnData == nullptr) {
        Serial.println("[WAKE] FAIL: wakenet create() failed");
        return false;
    }

    // 4) 运行参数
    _chunk = _wn->get_samp_chunksize(_wnData);
    int rate = _wn->get_samp_rate(_wnData);
    _stage = (int16_t*)malloc(_chunk * sizeof(int16_t));   // ~1KB, 内部 RAM
    if (!_stage) {
        Serial.println("[WAKE] FAIL: stage buffer alloc");
        return false;
    }
    _stageLen = 0;

    // v1u: 应用自定义阈值 (低于 DET_MODE 默认值, 提高真人召回; 见 system_config.h)
    if (_wn->set_det_threshold) {
        _wn->set_det_threshold(_wnData, WAKE_DET_THRESHOLD, 1);
    }

    _wordName = esp_srmodel_get_wake_words(_models, _modelName);
    _ready = true;
    Serial.printf("[WAKE] WakeNet9 OK: model=%s word='%s' chunk=%d (%dms) rate=%d thr=%.4f\n",
                  _modelName, _wordName ? _wordName : "?", _chunk,
                  _chunk * 1000 / rate, rate, getThreshold());
    return true;
}

void WakeWord::feed(const int16_t* buf, int n) {
    if (!_ready || !buf || n <= 0) return;
    _feedTotal += n;

    // 诊断: 喂入信号 RMS 累加 (确认幅度正常, 对比 VAD 看到的 rms)
    int64_t sumSq = 0;
    for (int i = 0; i < n; i++) sumSq += (int64_t)buf[i] * buf[i];
    _diagSumSq += (uint64_t)sumSq;
    _diagCnt  += (uint32_t)n;

    while (n > 0) {
        // 攒满一个 chunk 再检测 (模型一次吃 _chunk 个样本)
        int want = _chunk - _stageLen;
        int take = (n < want) ? n : want;
        memcpy(_stage + _stageLen, buf, take * sizeof(int16_t));
        _stageLen += take;
        buf += take;
        n -= take;

        if (_stageLen < _chunk) break;   // 还不够一帧, 等下次

        _detectTotal++;
        // detect 内部会拷贝数据, _stage 可复用
        wakenet_state_t r = _wn->detect(_wnData, _stage);
        _stageLen = 0;

        if (r == WAKENET_DETECTED) {
            _triggerCount++;
            _wakeEvent = true;
            _armedUntil = millis() + WAKE_ARM_WINDOW_MS;
            Serial.printf("[WAKE] **'%s' DETECTED** (#%u) armed %ds\n",
                          _wordName ? _wordName : "?",
                          _triggerCount, WAKE_ARM_WINDOW_MS / 1000);
        } else {
            _detectNoHit++;
            // -1 = WAKENET_NO_CHANNEL_VERIFIED: 单麦模型检测命中后的正常返回值, 无害;
            //   其余非 0/1 返回值才值得关注
            if (r != WAKENET_NO_DETECT && (int)r != -1) {
                Serial.printf("[WAKE] detect ret=%d (unexpected)\n", (int)r);
            }
        }
    }

    // 诊断: 每 5s 打印一次 (区分"没喂到" vs "识别不出")
    if (millis() - _diagT >= 5000) {
        _diagT = millis();
        double rms = (_diagCnt > 0) ? sqrt((double)_diagSumSq / _diagCnt) : 0.0;
        Serial.printf("[WAKE] diag: fed=%.1fs rms=%.0f det=%u nohit=%u hits=%u armed=%d\n",
                      _feedTotal / 16000.0f, rms,
                      _detectTotal, _detectNoHit, _triggerCount, isArmed() ? 1 : 0);
        _diagSumSq = 0;
        _diagCnt   = 0;
    }
}

bool WakeWord::consumeWakeEvent() {
    if (!_wakeEvent) {
        // armed 窗口已过则顺带清标志 (防御性, 正常不会到这)
        return false;
    }
    _wakeEvent = false;
    return true;
}

bool WakeWord::isArmed() const {
    return _ready && (int32_t)(millis() - _armedUntil) < 0;
}

bool WakeWord::setThreshold(float t) {
    if (!_ready || t < 0.5f || t > 0.9999f) return false;
    int ok = _wn->set_det_threshold(_wnData, t, 1);   // word_index=1 (单唤醒词)
    Serial.printf("[WAKE] set threshold %.4f -> %s (now %.4f)\n",
                  t, ok ? "OK" : "FAIL", getThreshold());
    return ok != 0;
}

float WakeWord::getThreshold() const {
    if (!_ready) return -1.0f;
    return _wn->get_det_threshold(_wnData, 1);
}
