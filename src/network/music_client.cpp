/**
 * @file music_client.cpp
 * @brief 云端迷你音乐客户端 (P7b 方案二: HTTP 回传 PCM)
 *
 * 链路: GET <MUSIC_SERVER_URL>/music?q=<歌名> → 裸 PCM (16kHz 16bit mono)
 * 实现: 复用 TTS 的流式下载模式 (HTTPClient + getStreamPtr + 分块 read)
 *
 * 特性:
 *  - 中文歌名 URL 编码 (UTF-8)
 *  - 下载到 PSRAM (8MB 足够放几分钟歌: 60s ≈ 1.92MB)
 *  - 响应大小从 Content-Length 读取 (无 chunked, 简单 HTTP 服务)
 *  - 超时/断网/404 一律返回 false
 */
#include "music_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFi.h>

// URL 编码 (处理中文/特殊字符, 与 TTSClient 同款)
String MusicClient::_urlEncode(const char* str) {
    String out;
    out.reserve(strlen(str) * 3);
    while (*str) {
        char c = *str++;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
            out += buf;
        }
    }
    return out;
}

bool MusicClient::fetchSong(const String& songName, int16_t** outBuf,
                            uint32_t* outSamples, float* outSeconds) {
    *outBuf = nullptr;
    *outSamples = 0;
    if (outSeconds) *outSeconds = 0;

    if (songName.length() == 0) {
        Serial.println("[MUSIC] ERROR: empty song name");
        return false;
    }

    // 组装 URL: <base>/music?q=<urlencoded>
    String url = MUSIC_SERVER_URL;
    url += "/music?q=";
    url += _urlEncode(songName.c_str());
    Serial.printf("[MUSIC] GET %s\n", url.c_str());

    HTTPClient http;
    // 本地/公网 HTTP (非 HTTPS) — 不加密, 音乐无敏感数据
    if (!http.begin(url)) {
        Serial.println("[MUSIC] HTTP begin failed");
        return false;
    }
    http.setTimeout(MUSIC_DOWNLOAD_TIMEOUT_MS);

    int code = http.GET();
    if (code <= 0) {
        Serial.printf("[MUSIC] HTTP FAIL: %s (code=%d)\n",
                      http.errorToString(code).c_str(), code);
        http.end();
        return false;
    }

    if (code != 200) {
        Serial.printf("[MUSIC] HTTP %d (song \"%s\" not found?)\n", code, songName.c_str());
        http.end();
        return false;
    }

    // 读取 Content-Length (服务端给明确长度; 兜底 120s ≈ 3.84MB)
    int totalLen = http.getSize();
    if (totalLen <= 0) totalLen = 16000 * 2 * 120;

    int16_t* buf = (int16_t*)heap_caps_malloc(totalLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        Serial.printf("[MUSIC] PSRAM alloc failed (%d bytes)\n", totalLen);
        http.end();
        return false;
    }

    // 流式下载 (与 TTS 同款)
    WiFiClient* stream = http.getStreamPtr();
    uint8_t* p = (uint8_t*)buf;
    int total = 0;
    uint32_t timeout = millis() + MUSIC_DOWNLOAD_TIMEOUT_MS;

    while (http.connected() && millis() < timeout && total < totalLen) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = (avail < totalLen - total) ? avail : (totalLen - total);
            int rd = stream->read(p, toRead);
            if (rd > 0) {
                p += rd;
                total += rd;
            }
        } else {
            delay(2);
        }
    }

    http.end();

    if (total == 0) {
        Serial.println("[MUSIC] No audio data received");
        heap_caps_free(buf);
        return false;
    }

    *outBuf = buf;
    *outSamples = total / 2;   // 16bit = 2 bytes/sample
    if (outSeconds) *outSeconds = *outSamples / 16000.0f;
    Serial.printf("[MUSIC] OK: \"%s\" %u samples (%.1fs, %d bytes)\n",
                  songName.c_str(), *outSamples, *outSamples / 16000.0f, total);
    return true;
}
