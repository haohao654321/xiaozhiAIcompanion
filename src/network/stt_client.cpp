/**
 * @file stt_client.cpp
 * @brief 百度语音识别 (STT) 客户端实现
 *
 * 流程: RAW POST PCM → 百度 → JSON {err_no, result[]}
 * 鉴权: Authorization: Bearer <API Key>
 */
#include "stt_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

String STTClient::recognize(const int16_t* pcm, uint32_t samples) {
    if (!pcm || samples == 0) {
        Serial.println("[STT] ERROR: empty PCM data");
        return "";
    }

    uint32_t byteLen = samples * sizeof(int16_t);
    Serial.printf("[STT] Sending %u samples (%u bytes) to Baidu...\n", samples, byteLen);

    HTTPClient http;
    // RAW POST: 音频直接放 body, 参数在 URL + header
    // HTTPS 加密传输 (语音内容敏感, 不裸奔); setInsecure 跳过证书校验
    String url = String(BAIDU_STT_URL) + "?cuid=" + BAIDU_CUID + "&dev_pid=1537";
    WiFiClientSecure secure;
    secure.setInsecure();
    http.begin(secure, url);
    http.setTimeout(15000);

    // API Key 鉴权 (无需 access_token)
    String auth = "Bearer ";
    auth += BAIDU_API_KEY;
    http.addHeader("Authorization", auth);
    // RAW 模式: Content-Type 指定格式和采样率
    http.addHeader("Content-Type", "audio/pcm;rate=16000");

    int code = http.POST((uint8_t*)pcm, byteLen);
    // v1k: 重试一次 — ESP32 WiFi 长时间空闲后 SSL 连接偶尔失败
    //      (start_ssl_client: -1), 重试通常可恢复
    if (code <= 0) {
        Serial.printf("[STT] HTTP FAIL: %s (code=%d), retrying in 1s...\n",
                      http.errorToString(code).c_str(), code);
        http.end();
        delay(1000);
        // 重建 secure client + http (SSL 连接状态已泄, 必须全新)
        secure.setInsecure();  // WiFiClientSecure 可复用
        http.begin(secure, url);
        http.setTimeout(15000);
        http.addHeader("Authorization", auth);
        http.addHeader("Content-Type", "audio/pcm;rate=16000");
        code = http.POST((uint8_t*)pcm, byteLen);
        if (code <= 0) {
            Serial.printf("[STT] Retry FAIL: %s (code=%d)\n",
                          http.errorToString(code).c_str(), code);
            http.end();
            return "";
        }
        Serial.println("[STT] Retry succeeded");
    }

    String body = http.getString();
    http.end();

    Serial.printf("[STT] HTTP %d, %u bytes response\n", code, body.length());

    // 解析 JSON: {"err_no":0,"err_msg":"success.","result":["识别文字"]}
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[STT] JSON parse error: %s\n", err.c_str());
        Serial.printf("[STT] Raw: %.200s\n", body.c_str());
        return "";
    }

    int errNo = doc["err_no"] | -1;
    if (errNo != 0) {
        const char* errMsg = doc["err_msg"] | "unknown";
        Serial.printf("[STT] API error: err_no=%d, err_msg=%s\n", errNo, errMsg);
        return "";
    }

    String text = doc["result"][0] | "";
    Serial.printf("[STT] Recognized: \"%s\"\n", text.c_str());
    return text;
}
