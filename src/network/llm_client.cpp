/**
 * @file llm_client.cpp
 * @brief 智谱 GLM-4-Flash LLM 客户端实现
 *
 * 流程: POST JSON {model, messages[]} → JSON {choices[0].message.content}
 * 鉴权: Authorization: Bearer <API Key>
 */
#include "llm_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

void LLMClient::resetHistory() {
    _msgCount = 0;
    // 第一条永远是 system prompt
    _msgs[0].role = "system";
    _msgs[0].content = LLM_SYSTEM_PROMPT;
    _msgCount = 1;
}

String LLMClient::_buildRequest(const String& userText) {
    // P6: 每次请求刷新 system prompt 里的实时时间 (NTP 同步后)
    // 模型本身不知道时间 ("几点了"会答"XX点XX分"), 注入后可正确回答
    _msgs[0].content = LLM_SYSTEM_PROMPT;
    time_t now = time(nullptr);
    if (now > 1700000000) {                    // NTP 已同步 (>2023-11)
        struct tm t;
        localtime_r(&now, &t);
        static const char* kWeekday[] = { "日", "一", "二", "三", "四", "五", "六" };
        // 注意: 注入文本约210字节 (中文3字节/字), 缓冲区必须 >=256,
        // 否则 snprintf 截断在汉字中间 → GLM 报 "Invalid UTF-8 middle byte 0x22"
        char tbuf[320];
        // 硬规则: 防止模型被"俏皮人格"带偏 (v1e 问时间只催睡觉不报数);
        // v1i 收紧: 仅明确问时间才报数, 否则"你报的是哪儿?"被误判成时间问题
        snprintf(tbuf, sizeof(tbuf),
                 "现在是%d年%d月%d日星期%s，%d点%d分。仅当用户明确问几点/时间/日期/星期几时，才先直接说出具体时间（几点几分/几号星期几），然后最多加一句简短俏皮的话；其他问题按字面理解，不要答时间。",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 kWeekday[t.tm_wday], t.tm_hour, t.tm_min);
        _msgs[0].content += tbuf;
    }

    // 添加用户消息
    if (_msgCount < MAX_MSGS) {
        _msgs[_msgCount].role = "user";
        _msgs[_msgCount].content = userText;
        _msgCount++;
    } else {
        // 满了: 移除最早的 1 轮 (index 1,2), 保留 system + 最近 N-1 轮
        // 注意: Msg 内含 String (堆指针), 不能 memmove 整块 (浅拷贝 →
        //       双重释放/悬空指针 → JSON 出现非法 UTF-8 字节), 必须逐元素赋值深拷贝
        for (int i = 3; i < _msgCount; i++) {
            _msgs[i - 2].role    = _msgs[i].role;
            _msgs[i - 2].content = _msgs[i].content;
        }
        _msgCount -= 2;
        _msgs[_msgCount].role = "user";
        _msgs[_msgCount].content = userText;
        _msgCount++;
    }

    // 手动构建 JSON (比 ArduinoJson 序列化更省内存)
    String json = "{\"model\":\"";
    json += ZHIPU_MODEL;
    json += "\",\"messages\":[";

    for (int i = 0; i < _msgCount; i++) {
        if (i > 0) json += ",";
        json += "{\"role\":\"";
        json += _msgs[i].role;
        json += "\",\"content\":\"";
        // 转义 JSON 特殊字符
        const String& c = _msgs[i].content;
        for (size_t j = 0; j < c.length(); j++) {
            char ch = c[j];
            if      (ch == '"')  json += "\\\"";
            else if (ch == '\\') json += "\\\\";
            else if (ch == '\n') json += "\\n";
            else if (ch == '\r') json += "\\r";
            else if (ch == '\t') json += "\\t";
            else if ((uint8_t)ch < 0x20) continue;  // 跳过其他控制字符
            else json += ch;
        }
        json += "\"}";
    }
    json += "],\"temperature\":0.8,\"max_tokens\":200}";
    return json;
}

String LLMClient::chat(const String& userText) {
    if (userText.length() == 0) {
        Serial.println("[LLM] ERROR: empty input");
        return "";
    }
    if (_msgCount == 0) resetHistory();

    Serial.printf("[LLM] User: \"%s\"\n", userText.c_str());
    String json = _buildRequest(userText);
    Serial.printf("[LLM] Request: %u bytes, %d messages\n", json.length(), _msgCount);

    // 失败时回滚刚追加的 user 消息 (避免失败堆积出连续 user 消息)
    auto rollback = [this]() {
        if (_msgCount > 1 && _msgs[_msgCount - 1].role == "user") _msgCount--;
    };

    WiFiClientSecure client;
    client.setInsecure();   // 跳过证书验证 (ESP32 常用做法)
    HTTPClient http;
    http.begin(client, ZHIPU_LLM_URL);
    http.setTimeout(20000);
    http.addHeader("Content-Type", "application/json");
    String auth = "Bearer ";
    auth += ZHIPU_API_KEY;
    http.addHeader("Authorization", auth);

    int code = http.POST(json);
    if (code <= 0) {
        Serial.printf("[LLM] HTTP FAIL: %s (code=%d)\n", http.errorToString(code).c_str(), code);
        http.end();
        rollback();
        return "";
    }

    String body = http.getString();
    http.end();

    Serial.printf("[LLM] HTTP %d, %u bytes response\n", code, body.length());

    // 解析 JSON: {choices:[{message:{content:"..."}}]}
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("[LLM] JSON parse error: %s\n", err.c_str());
        Serial.printf("[LLM] Raw: %.200s\n", body.c_str());
        rollback();
        return "";
    }

    // 检查 API 错误
    if (!doc["error"].isNull()) {
        const char* msg = doc["error"]["message"] | "unknown";
        Serial.printf("[LLM] API error: %s\n", msg);
        rollback();
        return "";
    }

    String reply = doc["choices"][0]["message"]["content"] | "";
    if (reply.length() == 0) {
        Serial.println("[LLM] Empty response");
        rollback();
        return "";
    }

    Serial.printf("[LLM] Reply: \"%s\"\n", reply.c_str());

    // 保存 AI 回复到历史
    if (_msgCount < MAX_MSGS) {
        _msgs[_msgCount].role = "assistant";
        _msgs[_msgCount].content = reply;
        _msgCount++;
    }

    return reply;
}
