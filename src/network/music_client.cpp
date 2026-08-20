/**
 * @file music_client.cpp
 * @brief 云端迷你音乐客户端 (P7b 方案二: HTTP 回传 PCM base64)
 *
 * 链路: GET <MUSIC_SERVER_URL>/music?q=<歌名> → base64 文本 → 固件端解码成 PCM
 * 实现: 复用 TTS 的流式下载模式 (HTTPClient + getStreamPtr + 分块 read)
 *
 * ⚠️ v2a 修复: 阿里云 FC 3.0 只能返回文本, /music 返回 base64 编码的 PCM;
 *   早期 music_client 把 base64 文本直接当 PCM 播放 → 全是杂音!
 *   现在下载后先 base64 解码再交 I2S。解码输出按 3/4 预估给 Content-Length
 *   下载分两段: 先读 base64 文本到临时缓冲, 再解码到 PSRAM。
 *
 * 特性:
 *  - 中文歌名 URL 编码 (UTF-8)
 *  - 下载到 PSRAM (8MB 足够放几分钟歌: 60s ≈ 1.92MB)
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

// ── base64 解码 (标准 RFC 4648, 无依赖手写) ──
// 输入 b64 文本 → 输出 malloc 的原始 bytes; 失败返回 nullptr
uint8_t* MusicClient::_decodeBase64(const uint8_t* b64, size_t b64Len, size_t* outBytes) {
    auto val = [](uint8_t c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    // 去掉末尾 padding '='
    size_t n = b64Len;
    while (n > 0 && (b64[n-1] == '=' || b64[n-1] == '\r' || b64[n-1] == '\n')) n--;
    size_t outLen = (n * 3) / 4;
    uint8_t* out = (uint8_t*)heap_caps_malloc(outLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return nullptr;

    size_t o = 0, i = 0;
    while (i + 4 <= n) {
        int a = val(b64[i]), b = val(b64[i+1]), c = val(b64[i+2]), d = val(b64[i+3]);
        if (a < 0 || b < 0) { heap_caps_free(out); return nullptr; }   // 非法字符
        uint32_t v = (a << 18) | (b << 12);
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) { v |= (c << 6); out[o++] = (uint8_t)(v >> 8); }
        if (d >= 0) { v |= d; out[o++] = (uint8_t)v; }
        i += 4;
    }
    *outBytes = o;
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
    // 云函数 HTTPS — 不传 CA 默认跳过证书校验
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

    // v2a: 服务端返回 base64 文本 (400KB b64 ≈ 300KB PCM, 15s 小星星)
    // v2c: 阿里云 FC 返回 Transfer-Encoding: chunked 无 Content-Length,
    //   http.getSize() 返回 -1 → 兜底 2.5MB 但实际读到 chunked 尾帧多余字节
    //   → base64 解码混入非法字符。改: 分配上限 1.5MB, 读到流关闭为止。
    const int MAX_B64_BYTES = 16000 * 2 * 60 / 3 * 4;   // 60s b64 ≈ 1.28MB, 给 1.5MB 余量
    int totalLen = http.getSize();
    if (totalLen <= 0 || totalLen > MAX_B64_BYTES) totalLen = MAX_B64_BYTES;

    uint8_t* b64buf = (uint8_t*)heap_caps_malloc(totalLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64buf) {
        Serial.printf("[MUSIC] PSRAM alloc failed (%d bytes)\n", totalLen + 1);
        http.end();
        return false;
    }

    // ── v2e: 手动解析 Transfer-Encoding: chunked ──
    // 阿里云 FC 返回 chunked 无 Content-Length, 且 http.getStreamPtr()
    // 拿到的是原始 TCP 流 —— HTTPClient **不会**自动解 chunked,
    // chunk 帧的 "大小hex\r\n" 和 "\r\n" 会被当数据读进来, 混入 base64
    // → _decodeBase64 判定非法字符而 FAIL (上一版 v2d 直接读流的 bug)
    //
    // 这里按 chunk 规范手解: [hex\r\n][数据\r\n][hex\r\n][数据\r\n]...[0\r\n][\r\n]
    WiFiClient* stream = http.getStreamPtr();
    int total = 0;
    bool inChunkData = false;    // true = 正在读 chunk 数据区 (需跳过尾部 \r\n)
    int chunkRemain = 0;         // 当前 chunk 剩余数据字节
    int crlfSkip = 0;            // 待跳过的 \r\n 字节数 (chunk 边界)
    bool sawTerminator = false;  // 读到 0 长度的终止 chunk
    char chunkSizeBuf[16];       // 累积 chunk 大小 hex 字符串
    size_t chunkSizeHexLen = 0;  // 当前累积的 hex 字符数
    uint32_t timeout = millis() + MUSIC_DOWNLOAD_TIMEOUT_MS;

    while (millis() < timeout && total < totalLen && !sawTerminator) {
        if (!http.connected()) {
            // FC 传完最后一个 chunk 后可能主动断开; 已拿到数据就算完成
            if (total > 0) break;
            Serial.println("[MUSIC] conn closed before any data");
            http.end();
            heap_caps_free(b64buf);
            return false;
        }
        int avail = stream->available();
        if (avail <= 0) { delay(2); continue; }
        int c = stream->read();
        if (c < 0) { delay(2); continue; }

        if (crlfSkip > 0) {           // 跳过 chunk 边界的 \r\n
            crlfSkip--;
            continue;
        }
        if (inChunkData) {
            if (chunkRemain > 0) {
                b64buf[total++] = (uint8_t)c;   // chunk 数据区字节 → base64
                chunkRemain--;
                if (chunkRemain == 0) {
                    crlfSkip = 2;               // chunk 结尾的 \r\n
                    inChunkData = false;
                }
            }
            continue;
        }
        // 读取 chunk 大小行: hex 到 \n 结束
        if (c == '\n') {
            // 解析已积累的大小字节
            if (chunkSizeHexLen == 0) {
                // 空行(单独 \n)出现说明上一个 chunk 刚结束且没走 crlfSkip? 容错跳过
                continue;
            }
            // 忽略 "hex;ext" 形式的扩展 (分号后截断)
            size_t hexLen = chunkSizeHexLen;
            for (size_t k = 0; k < hexLen; k++) {
                if (chunkSizeBuf[k] == ';') { hexLen = k; break; }
            }
            long sz = strtol((const char*)chunkSizeBuf, nullptr, 16);
            chunkSizeHexLen = 0;
            if (sz <= 0) {              // 终止 chunk: 0\r\n\r\n
                sawTerminator = true;
                break;
            }
            chunkRemain = (int)sz;
            inChunkData = true;
        } else if (c != '\r') {
            if (chunkSizeHexLen < sizeof(chunkSizeBuf) - 1)
                chunkSizeBuf[chunkSizeHexLen++] = (char)c;
        }
    }
    http.end();

    b64buf[total] = '\0';                       // 截断安全

    if (total == 0) {
        Serial.println("[MUSIC] No audio data received");
        heap_caps_free(b64buf);
        return false;
    }

    // v2a: base64 → 原始 PCM bytes
    size_t pcmBytes = 0;
    uint8_t* pcm = _decodeBase64(b64buf, (size_t)total, &pcmBytes);
    heap_caps_free(b64buf);
    if (!pcm || pcmBytes < 4) {
        Serial.printf("[MUSIC] base64 decode FAIL (b64=%d bytes)\n", total);
        if (pcm) heap_caps_free(pcm);
        return false;
    }

    *outBuf = (int16_t*)pcm;
    *outSamples = pcmBytes / 2;                 // 16bit = 2 bytes/sample
    if (outSeconds) *outSeconds = *outSamples / 16000.0f;
    Serial.printf("[MUSIC] OK: \"%s\" %u samples (%.1fs, b64 %d -> pcm %u bytes)\n",
                  songName.c_str(), *outSamples, *outSamples / 16000.0f, total, (unsigned)pcmBytes);
    return true;
}
