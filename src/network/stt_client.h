/**
 * @file stt_client.h
 * @brief 百度语音识别 (STT) 客户端 — P4
 *
 * 鉴权: API Key 鉴权 (Authorization: Bearer <key>), 无需换 access_token
 * 接口: 百度短语音识别标准版 (≤60s)
 * 输入: 16kHz/16bit/mono PCM (raw, 无 WAV 头)
 * 输出: 识别出的文字 (String)
 *
 * 文档: https://cloud.baidu.com/doc/SPEECH/s/Jlbxdezuf
 */
#pragma once
#include <Arduino.h>

class STTClient {
public:
    /**
     * 语音识别: 发送 PCM 到百度, 返回识别文字
     * @param pcm      16bit mono 16kHz PCM 数据
     * @param samples  采样数 (不是字节数)
     * @return 识别文字, 空串表示失败
     */
    String recognize(const int16_t* pcm, uint32_t samples);
};
