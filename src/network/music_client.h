/**
 * @file music_client.h
 * @brief 云端迷你音乐客户端 (P7b 方案二: HTTP 回传 PCM)
 *
 * 背景: 云端 LLM 是纯文本, TTS 只能"念字"不能"唱", 所以单独起一个
 * 极简音乐服务 (tools/music_server/server.py), 按歌名返回 16kHz/16bit/mono
 * 的裸 PCM, 本客户端下载到 PSRAM 后直接喂 I2S 播放。
 *
 * 服务器: 先本地 PC 跑通 (局域网 IP), 后续可部署到公网 (云函数/VPS)。
 * 地址在 system_config.h 的 MUSIC_SERVER_URL 配置。
 *
 * 请求: GET <MUSIC_SERVER_URL>/music?q=<URL编码歌名>
 * 响应: 200 audio/x-pcm;rate=16000 (原始 PCM bytes)
 *       404 JSON 错误 (歌不存在)
 */
#pragma once
#include <Arduino.h>
#include <stdint.h>

class MusicClient {
public:
    /**
     * 下载并返回整首歌曲的 PCM 音频
     * @param songName   歌名 (中文, 如 "小星星" / "致爱丽丝")
     * @param outBuf     [out] PSRAM 中分配的 PCM buffer (调用者负责 free)
     * @param outSamples [out] 采样数 (0 = 失败)
     * @param outSeconds [out] 可选: 歌曲时长秒数
     * @return true 成功
     */
    bool fetchSong(const String& songName, int16_t** outBuf,
                   uint32_t* outSamples, float* outSeconds = nullptr);

private:
    static String _urlEncode(const char* str);
};
