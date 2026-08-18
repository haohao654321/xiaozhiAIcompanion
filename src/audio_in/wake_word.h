/**
 * @file wake_word.h
 * @brief P4-1 离线唤醒词检测 — esp-sr WakeNet9 ("你好小智")
 *
 * 架构 (资源占用优先):
 *   - 模型数据 (284KB) 不进固件, 放独立 "model" flash 分区, esp_srmodel_init
 *     mmap 后按需读取 → 固件体积几乎零增长, 换唤醒词不用重编固件
 *   - WakeNet9 运行时 ~24KB RAM + 少量 CPU (每 30ms 一帧推理), 无 PSRAM 大缓冲
 *   - 音频零拷贝复用: mic_manager 环境监听排空式读取的同一块 PCM 顺手喂进来,
 *     不增加任何 I2S 读取 (避免当年录音欠采样的坑)
 *
 * 触发逻辑:
 *   唤醒命中 → 置 armed 窗口 (WAKE_ARM_WINDOW_MS)
 *   conversation_manager 在 armed 窗口内: 唤醒事件立即开录 / VAD 说话也可开录
 *   唤醒词失效 (模型分区未烧) → 自动降级回纯 VAD 触发, 不影响现有功能
 *
 * 声反馈防自激: 喇叭提示音期间由 mic_manager 跳过喂入 (见 _updateEnvSense)
 */
#pragma once
#include <stdint.h>

extern "C" {
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
}

class WakeWord {
public:
    /**
     * 加载模型分区并初始化 WakeNet
     * @return true=唤醒词就绪; false=模型缺失/初始化失败 (调用方降级)
     */
    bool begin(const char* partitionLabel = "model");

    /** 喂入 16kHz/16bit mono PCM (mic_manager 排空式读取时调用, 零拷贝追加) */
    void feed(const int16_t* buf, int n);

    /** 是否成功加载 (决定 conversation 用唤醒模式还是降级 VAD 模式) */
    bool isReady() const { return _ready; }

    /** 消费一次唤醒事件 (读后清零); armed 窗口内 VAD 也可触发对话 */
    bool consumeWakeEvent();

    /** 唤醒后 WAKE_ARM_WINDOW_MS 内为 true (此窗口内说话即可进入对话) */
    bool isArmed() const;

    /** 当前唤醒词名字 (调试用, e.g. "你好小智") */
    const char* wordName() const { return _wordName; }

    /** 模型名 (e.g. "wn9_nihaoxiaozhi") */
    const char* modelName() const { return _modelName; }

    /** 唤醒累计触发次数 (诊断) */
    uint32_t triggerCount() const { return _triggerCount; }

    /** 运行时调灵敏度: 阈值范围 0.5~0.9999, 越低越容易触发 (误报也升) */
    bool setThreshold(float t);

    /** 当前检测阈值 (word_index=1, 单唤醒词) */
    float getThreshold() const;

private:
    bool              _ready = false;
    srmodel_list_t*   _models = nullptr;
    char*             _modelName = nullptr;
    char*             _wordName = nullptr;
    const esp_wn_iface_t* _wn = nullptr;
    model_iface_data_t*   _wnData = nullptr;

    int               _chunk = 0;        // 每次检测需要的样本数 (wn9 ~480)
    int16_t*          _stage = nullptr;  // 暂存缓冲 (不足一 chunk 时攒着)
    int               _stageLen = 0;

    volatile bool     _wakeEvent = false;
    uint32_t          _armedUntil = 0;
    uint32_t          _triggerCount = 0;

    // P4-1 诊断 (v1c): feed/detect 统计, 区分"没喂到" vs "识别不出"
    uint32_t          _feedTotal = 0;     // 累计喂入样本数
    uint32_t          _detectTotal = 0;   // detect 调用次数
    uint32_t          _detectNoHit = 0;   // 返回 NO_DETECT 次数
    uint64_t          _diagSumSq = 0;     // 喂入信号平方和 (RMS 诊断)
    uint32_t          _diagCnt = 0;       // 喂入样本计数
    uint32_t          _diagT = 0;         // 上次诊断打印时刻
};
