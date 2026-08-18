/**
 * @file weather_client.h
 * @brief P8a 天气查询客户端 — 调知识服务网关拿真天气文本
 *
 * 链路: GET <KNOWLEDGE_SERVER_URL>/weather?city=北京&clothes=1 → 纯文本(≤50字)
 * 文本直接进 TTS 合成播放, 不经过 LLM (真数据, 非嘴炮)
 *
 * 服务端: tools/knowledge_server.py (和风天气 API, 1h 缓存)
 * 断网/服务不可用 → 返回 false, conversation_manager 降级走 LLM
 */
#pragma once
#include <Arduino.h>

class WeatherClient {
public:
    /**
     * 查询天气 → 格式化播报文本
     * @param city     城市名 (如 "北京" / "西安" / "兰州")
     * @param days     预报天数 1=今天 2=明天 3=后天 (服务端取第 days 天)
     * @param clothes  是否带穿衣建议
     * @param outText  [out] ≤50字 播报文本
     * @return true 成功; false 断网/超时/服务不可用
     */
    bool fetch(const String& city, int days, bool clothes, String& outText);

private:
    static String _urlEncode(const char* str);
};
