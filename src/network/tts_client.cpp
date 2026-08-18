/**
 * @file tts_client.cpp
 * @brief 百度语音合成 (TTS) 客户端实现
 *
 * 流程: POST form-encoded → 百度 → PCM 二进制 or JSON 错误
 * 鉴权: Authorization: Bearer <API Key>
 * 格式: aue=4 (PCM 16kHz 16bit mono), 直接可推 I2S
 */
#include "tts_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// URL 编码 (处理中文/特殊字符)
String TTSClient::_urlEncode(const char* str) {
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

bool TTSClient::synthesize(const String& text, int16_t** outBuf, uint32_t* outSamples) {
    *outBuf = nullptr;
    *outSamples = 0;

    if (text.length() == 0) {
        Serial.println("[TTS] ERROR: empty text");
        return false;
    }

    // 截断过长文本
    String tex = text;
    if (tex.length() > CONV_TTS_MAX_CHARS * 3) {  // UTF-8 中文 3 字节/字
        tex = tex.substring(0, CONV_TTS_MAX_CHARS * 3);
    }

    Serial.printf("[TTS] Synthesizing: \"%s\" (%u bytes)\n", tex.c_str(), tex.length());

    // 构建 form-encoded body
    String body = "tex=" + _urlEncode(tex.c_str());
    body += "&lan=zh&cuid=" BAIDU_CUID;
    body += "&ctp=1&aue=4";        // PCM 16kHz
    body += "&spd=5&pit=5&vol=5";  // 语速/音调/音量 (0-15)
    body += "&per=0";               // 度小美 (标准女声)

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, BAIDU_TTS_URL);
    http.setTimeout(15000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String auth = "Bearer ";
    auth += BAIDU_API_KEY;
    http.addHeader("Authorization", auth);

    // 收集 Content-Type 响应头 (区分音频/错误)
    const char* headerKeys[] = { "Content-Type" };
    http.collectHeaders(headerKeys, 1);

    int code = http.POST(body);
    if (code <= 0) {
        Serial.printf("[TTS] HTTP FAIL: %s (code=%d)\n", http.errorToString(code).c_str(), code);
        http.end();
        return false;
    }

    String contentType = http.header("Content-Type");
    Serial.printf("[TTS] HTTP %d, Content-Type: %s\n", code, contentType.c_str());

    // 判断响应类型
    if (contentType.startsWith("audio/")) {
        // 成功: 读取 PCM 二进制数据到 PSRAM
        int totalLen = http.getSize();   // -1 = 未知
        if (totalLen < 0) totalLen = 640000;  // 兜底 20s (CONV_TTS_MAX_CHARS=100字 上限)

        int16_t* buf = (int16_t*)heap_caps_malloc(totalLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            Serial.println("[TTS] PSRAM alloc failed");
            http.end();
            return false;
        }

        // getStreamPtr 返回基类 WiFiClient*, read() 是虚函数会正确分派到 secure client
        WiFiClient* stream = http.getStreamPtr();
        uint8_t* p = (uint8_t*)buf;
        int total = 0;
        uint32_t timeout = millis() + 10000;

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
            Serial.println("[TTS] No audio data received");
            heap_caps_free(buf);
            return false;
        }

        *outBuf = buf;
        *outSamples = total / 2;   // 16bit = 2 bytes/sample
        Serial.printf("[TTS] OK: %u samples (%d bytes, %.1fs)\n",
                      *outSamples, total, *outSamples / 16000.0f);
        return true;
    }

    // 失败: JSON 错误信息
    String errBody = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, errBody);
    if (!err) {
        int errNo = doc["err_no"] | -1;
        const char* errMsg = doc["err_msg"] | "unknown";
        Serial.printf("[TTS] API error: err_no=%d, err_msg=%s\n", errNo, errMsg);
    } else {
        Serial.printf("[TTS] Unknown error: %.200s\n", errBody.c_str());
    }
    return false;
}
