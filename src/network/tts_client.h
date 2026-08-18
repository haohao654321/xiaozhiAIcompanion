/**
 * @file tts_client.h
 * @brief 百度语音合成 (TTS) 客户端 — P4
 *
 * 鉴权: API Key 鉴权 (Authorization: Bearer <key>), 无需换 access_token
 * 接口: 百度短文本在线合成
 * 输入: 文字
 * 输出: 16kHz/16bit/mono PCM (直接可推 I2S, 无需 MP3 解码)
 *
 * 文档: https://cloud.baidu.com/doc/SPEECH/s/Jk4qg3jzp
 */
#pragma once
#include <Arduino.h>
#include <stdint.h>

class TTSClient {
public:
    /**
     * 语音合成: 文字 → PCM 音频
     * @param text       待合成文字 (UTF-8)
     * @param outBuf     [out] PSRAM 中分配的 PCM buffer (调用者负责 free)
     * @param outSamples [out] 采样数
     * @return true 成功
     */
    bool synthesize(const String& text, int16_t** outBuf, uint32_t* outSamples);

private:
    static String _urlEncode(const char* str);
};
