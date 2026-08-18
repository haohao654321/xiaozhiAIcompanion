/**
 * @file llm_client.h
 * @brief 智谱 GLM-4-Flash LLM 对话客户端 — P4
 *
 * 鉴权: API Key (Authorization: Bearer <key>)
 * 接口: OpenAI 兼容 /chat/completions
 * 模型: glm-4-flash (免费)
 * 特性: 保留最近 N 轮对话上下文
 *
 * 文档: https://open.bigmodel.cn/dev/api
 */
#pragma once
#include <Arduino.h>
#include "../config/system_config.h"   // LLM_MAX_HISTORY / LLM_SYSTEM_PROMPT

class LLMClient {
public:
    /** 重置对话历史 */
    void resetHistory();

    /**
     * 发送对话请求
     * @param userText 用户文字
     * @return AI 回复文字, 空串表示失败
     */
    String chat(const String& userText);

private:
    struct Msg { String role; String content; };
    static const int MAX_MSGS = LLM_MAX_HISTORY * 2 + 1;  // system + N*2 (user+assistant)
    Msg _msgs[MAX_MSGS];
    int _msgCount = 0;

    String _buildRequest(const String& userText);
};
