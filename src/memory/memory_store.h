/**
 * @file memory_store.h
 * @brief P10: 长期记忆存储 — SD 卡 /memory.txt 读写 (ESP32 本地)
 *
 * 设计 (第一版 MVP, 简单可靠):
 *   - 存储: /sdcard/memory.txt, UTF-8 纯文本, 每行一条记忆
 *   - 加载: begin() 读入 RAM (String 缓存), 对话时随 /ask 发给云端注入 prompt
 *   - 写入: add() 去重追加 + 上限裁剪 (MAX_MEM_BYTES, 超限删最旧)
 *   - 降级: SD 卡不可用 → available()=false, 路由提示"没记住"但系统不受影响
 *
 * SD 挂载: capture_manager 已 SD_MMC.begin("/sdcard", true), 本模块直接用
 * (main.cpp 顺序: capture.begin() 在 conversation.begin() 之前)
 */
#pragma once
#include <Arduino.h>

class MemoryStore {
public:
    /** 加载 SD 记忆文件 (无卡/无文件则保持空, available=false) */
    void begin();

    /** 追加一条记忆 (精确去重; 超上限删最旧行; 返回是否写入) */
    bool add(const String& entry);

    /** 全部记忆文本 (多行, 供 /ask 的 memory 字段) */
    const String& text() const { return _memText; }

    /** SD 卡 + 文件是否可用 */
    bool available() const { return _avail; }

    /** 清空全部记忆 (串口调试用) */
    void clearAll();

    static const size_t MAX_MEM_BYTES = 1536;   // 上限 ~1.5KB (约 30 条中文记忆)

private:
    bool _load();
    bool _save();

    String _memText;
    bool   _avail = false;
};
