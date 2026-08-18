/**
 * @file weather_client.cpp
 * @brief P8a 天气查询客户端 — 调知识服务网关拿真天气文本
 *
 * 实现: HTTPClient GET → 纯文本响应 (与 music_client 同款 HTTP 模式, 但返回文本)
 * 超时 8s (天气查询比音乐下载快, 服务端有缓存)
 */
#include "weather_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFi.h>

String WeatherClient::_urlEncode(const char* str) {
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

bool WeatherClient::fetch(const String& city, int days, bool clothes, String& outText) {
    outText = "";
    if (city.length() == 0) {
        Serial.println("[WEATHER] ERROR: empty city");
        return false;
    }
    if (days < 1) days = 1;
    if (days > 3) days = 3;

    // 组装 URL: <base>/weather?city=<urlencoded>&days=N&clothes=1
    String url = KNOWLEDGE_SERVER_URL;
    url += "/weather?city=";
    url += _urlEncode(city.c_str());
    url += "&days=";
    url += days;
    if (clothes) url += "&clothes=1";
    Serial.printf("[WEATHER] GET %s\n", url.c_str());

    HTTPClient http;
    if (!http.begin(url)) {
        Serial.println("[WEATHER] HTTP begin failed");
        return false;
    }
    http.setTimeout(8000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[WEATHER] HTTP FAIL (code=%d)\n", code);
        http.end();
        return false;
    }

    String body = http.getString();
    http.end();

    body.trim();
    if (body.length() == 0) {
        Serial.println("[WEATHER] empty response");
        return false;
    }

    // 安全截断 (天气文本 ≤50字 ≈150字节; TTS 上限 CONV_TTS_MAX_CHARS=100字)
    size_t maxBytes = (size_t)CONV_TTS_MAX_CHARS * 3;
    if (body.length() > maxBytes) {
        body = body.substring(0, maxBytes);
    }

    outText = body;
    Serial.printf("[WEATHER] OK: %s -> \"%s\"\n", city.c_str(), body.c_str());
    return true;
}
