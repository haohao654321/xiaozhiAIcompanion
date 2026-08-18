/**
 * @file vision_client.cpp
 * @brief 智谱 GLM-4V-Flash 视觉客户端实现
 *
 * 流程: JPEG → base64 → 构造多模态 JSON → POST → 解析 choices[0].message.content
 * 内存: base64 字符串 + JSON body 用 String (ESP32 堆/PSRAM 动态分配)
 */
#include "vision_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ── base64 编码 (标准 RFC4648, 无换行) ──
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String base64Encode(const uint8_t* data, size_t len) {
    String out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out += B64_TABLE[(n >> 18) & 0x3F];
        out += B64_TABLE[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_TABLE[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_TABLE[n & 0x3F] : '=';
    }
    return out;
}

String VisionClient::describe(const uint8_t* jpeg, size_t len, const String& prompt) {
    if (!jpeg || len == 0) {
        Serial.println("[VIS] ERROR: empty jpeg");
        return "";
    }
    if (len > VISION_MAX_IMAGE_BYTES) {
        Serial.printf("[VIS] ERROR: image too large (%u > %u)\n",
                      (unsigned)len, (unsigned)VISION_MAX_IMAGE_BYTES);
        return "";
    }

    // ── 1. base64 ──
    Serial.printf("[VIS] Encoding %u bytes JPEG...\n", (unsigned)len);
    String b64 = base64Encode(jpeg, len);
    Serial.printf("[VIS] Base64: %u chars\n", b64.length());

    // ── 2. 构造多模态 JSON ──
    // {"model":"glm-4v-flash","messages":[{"role":"user","content":[
    //   {"type":"text","text":"..."},
    //   {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,..."}}]}]}
    String json = "{\"model\":\"";
    json += ZHIPU_VISION_MODEL;
    json += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";

    // 转义 prompt (JSON 特殊字符)
    for (size_t j = 0; j < prompt.length(); j++) {
        char ch = prompt[j];
        if      (ch == '"')  json += "\\\"";
        else if (ch == '\\') json += "\\\\";
        else if (ch == '\n') json += "\\n";
        else if (ch == '\r') json += "\\r";
        else if ((uint8_t)ch < 0x20) continue;
        else json += ch;
    }

    json += "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,";
    json += b64;
    json += "\"}}]}],\"temperature\":0.8,\"max_tokens\":200}";
    b64 = "";   // 释放 base64 (String 内部立即归还堆)

    Serial.printf("[VIS] Request body: %u bytes\n", json.length());

    // ── 3. HTTP POST ──
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, ZHIPU_LLM_URL);   // 同一端点
    http.setTimeout(30000);              // 视觉模型稍慢
    http.addHeader("Content-Type", "application/json");
    String auth = "Bearer ";
    auth += ZHIPU_API_KEY;
    http.addHeader("Authorization", auth);

    int code = http.POST(json);
    if (code <= 0) {
        Serial.printf("[VIS] HTTP FAIL: %s (code=%d)\n", http.errorToString(code).c_str(), code);
        http.end();
        return "";
    }

    String body = http.getString();
    http.end();
    Serial.printf("[VIS] HTTP %d, %u bytes response\n", code, body.length());

    // ── 4. 解析 ──
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[VIS] JSON parse error: %s\n", err.c_str());
        Serial.printf("[VIS] Raw: %.200s\n", body.c_str());
        return "";
    }
    if (!doc["error"].isNull()) {
        const char* msg = doc["error"]["message"] | "unknown";
        Serial.printf("[VIS] API error: %s\n", msg);
        return "";
    }

    String reply = doc["choices"][0]["message"]["content"] | "";
    if (reply.length() == 0) {
        Serial.println("[VIS] Empty response");
        return "";
    }
    Serial.printf("[VIS] Reply: \"%s\"\n", reply.c_str());
    return reply;
}
