/**
 * @file ask_client.h
 * @brief P8e: LLM 联网问答客户端 — 调知识服务网关 /ask 端点
 *
 * 链路: POST <KNOWLEDGE_SERVER_URL>/ask {"text":"用户STT文本"} → 纯文本(≤50字, UTF-8)
 * 网关端调 GLM-4-Flash + web_search(兜底) + get_weather(function calling)
 * 文本直接进 TTS 合成播放, 不经过 ESP32 本地 LLM (编排权在 Python 网关)
 *
 * 服务端: tools/knowledge_server.py (POST /ask 端点)
 * 断网/超时/服务不可用 → 返回 false, conversation_manager 降级走本地裸 LLM
 */
#pragma once
#include <Arduino.h>

class AskClient {
public:
    /**
     * 联网问答
     * @param userText  用户 STT 文本 (任意自然语言)
     * @param outReply  [out] ≤50字 回复文本 (UTF-8)
     * @param memory    P10: 长期记忆文本 (SD 卡条目, 空串=不带), 云端注入 prompt
     * @return true 成功; false 断网/超时/服务不可用
     */
    bool ask(const String& userText, String& outReply,
             const String& memory = "");

    /**
     * v1w: 追加一轮对话到历史 (user + assistant 成对)
     * 历史满 2 轮时淘汰最旧的一轮
     */
    void pushHistory(const String& userText, const String& assistantReply);

    /** v1w: 清空对话历史 (新话题/睡眠后) */
    void clearHistory();

private:
    static String _escapeJSON(const String& s);

    // v1w: 对话历史 (最多 2 轮, 每轮 user+assistant 约 100 字 ≈ 300 字节)
    // 用固定数组避免 String 堆碎片
    static constexpr uint8_t MAX_HISTORY = 2;
    String _historyUser[MAX_HISTORY];
    String _historyAssistant[MAX_HISTORY];
    uint8_t _historyCount = 0;
    uint8_t _historyHead = 0;  // 最旧一轮的索引 (环形缓冲)
};
