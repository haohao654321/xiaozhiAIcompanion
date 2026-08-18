/**
 * @file vision_client.h
 * @brief 智谱 GLM-4V-Flash 视觉理解客户端 — P4.5 摄像头应用层
 *
 * 鉴权: API Key (Authorization: Bearer <key>) — 与 LLMClient 相同
 * 接口: OpenAI 兼容 /chat/completions, content 为多模态数组
 * 模型: glm-4v-flash (免费)
 *
 * 请求格式:
 *   messages[0].content = [
 *     {"type":"text", "text":"<prompt>"},
 *     {"type":"image_url", "image_url":{"url":"data:image/jpeg;base64,<data>"}}
 *   ]
 *
 * 文档: https://open.bigmodel.cn/dev/api/normal-model/glm-4v
 */
#pragma once
#include <Arduino.h>

class VisionClient {
public:
    /**
     * 发送一张 JPEG 图片 + 文字提问给视觉模型
     * @param jpeg   JPEG 数据 (硬件JPEG或软件压缩均可)
     * @param len    JPEG 长度
     * @param prompt 文字指令 (如 "看看周围有什么")
     * @return AI 描述文字, 空串表示失败
     */
    String describe(const uint8_t* jpeg, size_t len, const String& prompt);
};
