/**
 * @file ask_client.cpp
 * @brief P8e: LLM 联网问答客户端 — 调知识服务网关 /ask 端点
 *
 * 实现: HTTPClient POST JSON → 纯文本响应 (web_search 兜底 + function calling 精确层)
 * 超时 15s (LLM 联网搜索 + 可能两轮 function calling, 比纯天气查询慢)
 * 与 weather_client/music_client 同款 HTTP 模式, 但用 POST + JSON body
 */
#include "ask_client.h"
#include "../config/system_config.h"
#include <HTTPClient.h>
#include <WiFi.h>

String AskClient::_escapeJSON(const String& s) {
    String out;
    out.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
        char ch = s[i];
        if      (ch == '"')  out += "\\\"";
        else if (ch == '\\') out += "\\\\";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else if ((uint8_t)ch < 0x20) continue;
        else out += ch;
    }
    return out;
}

bool AskClient::ask(const String& userText, String& outReply, const String& memory) {
    outReply = "";
    if (userText.length() == 0) {
        Serial.println("[ASK] ERROR: empty input");
        return false;
    }

    // 组装 URL: <base>/ask
    String url = KNOWLEDGE_SERVER_URL;
    url += "/ask";

    // v1w: JSON body 加 history 字段
    // v1z: P10 加 memory 字段 (长期记忆注入)
    // {"text":"...","history":[["u1","a1"],...],"memory":"..."}
    String body = "{\"text\":\"";
    body += _escapeJSON(userText);
    body += "\"";
    if (_historyCount > 0) {
        body += ",\"history\":[";
        for (uint8_t i = 0; i < _historyCount; i++) {
            uint8_t idx = (_historyHead + i) % MAX_HISTORY;
            if (i > 0) body += ",";
            body += "[\"";
            body += _escapeJSON(_historyUser[idx]);
            body += "\",\"";
            body += _escapeJSON(_historyAssistant[idx]);
            body += "\"]";
        }
        body += "]";
    }
    if (memory.length() > 0) {
        body += ",\"memory\":\"";
        body += _escapeJSON(memory);
        body += "\"";
    }
    body += "}";

    Serial.printf("[ASK] POST %s (%u bytes, history=%d, mem=%u)\n",
                  url.c_str(), body.length(), _historyCount, memory.length());

    HTTPClient http;
    if (!http.begin(url)) {
        Serial.println("[ASK] HTTP begin failed");
        return false;
    }
    // 注: 不传 CA 证书的 https begin 默认跳过证书校验 (老 core 无 setInsecure, 无需调用)
    http.setTimeout(25000);  // 25s: 云函数冷启动3-5s + LLM联网搜索 + function calling
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    if (code != 200) {
        Serial.printf("[ASK] HTTP FAIL (code=%d)\n", code);
        http.end();
        return false;
    }

    String resp = http.getString();
    http.end();

    resp.trim();
    if (resp.length() == 0) {
        Serial.println("[ASK] empty response");
        return false;
    }

    // 安全截断 (回复 ≤50字 ≈150字节; ESP32 TTS 上限 CONV_TTS_MAX_CHARS=100字)
    size_t maxBytes = (size_t)CONV_TTS_MAX_CHARS * 3;
    if (resp.length() > maxBytes) {
        resp = resp.substring(0, maxBytes);
    }

    outReply = resp;
    Serial.printf("[ASK] OK: \"%s\"\n", outReply.substring(0, 80).c_str());
    return true;
}

// v1w: 追加一轮对话到历史 (user + assistant 成对)
void AskClient::pushHistory(const String& userText, const String& assistantReply) {
    if (userText.length() == 0 || assistantReply.length() == 0) return;

    uint8_t idx;
    if (_historyCount < MAX_HISTORY) {
        idx = (_historyHead + _historyCount) % MAX_HISTORY;
        _historyCount++;
    } else {
        // 满员: 覆盖最旧的一轮
        idx = _historyHead;
        _historyHead = (_historyHead + 1) % MAX_HISTORY;
    }
    _historyUser[idx] = userText;
    _historyAssistant[idx] = assistantReply;
    Serial.printf("[ASK] History push [%d/%d]: U=\"%s\" A=\"%s\"\n",
                  _historyCount, MAX_HISTORY,
                  _historyUser[idx].c_str(),
                  _historyAssistant[idx].c_str());
}

// v1w: 清空对话历史
void AskClient::clearHistory() {
    for (uint8_t i = 0; i < MAX_HISTORY; i++) {
        _historyUser[i] = "";
        _historyAssistant[i] = "";
    }
    _historyCount = 0;
    _historyHead = 0;
    Serial.println("[ASK] History cleared");
}
