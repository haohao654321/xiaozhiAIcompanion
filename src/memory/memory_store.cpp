/**
 * @file memory_store.cpp
 * @brief P10: 长期记忆存储实现 (见 memory_store.h)
 */
#include "memory_store.h"
#include <SD_MMC.h>

#define MEM_FILE_PATH   "/memory.txt"

void MemoryStore::begin() {
    _avail = false;
    _memText = "";
    // SD 已由 capture_manager 挂载 (main.cpp: capture.begin() 在前), 这里直接复用;
    // 不重复调 SD_MMC.begin (重复挂载会失败/报错)。无卡时 _load 的 open 会失败 → 降级。
    _avail = _load();
    Serial.printf("[MEM] begin: %s (%u bytes, available=%d)\n",
                  MEM_FILE_PATH, _memText.length(), _avail ? 1 : 0);
}

bool MemoryStore::_load() {
    if (!SD_MMC.exists(MEM_FILE_PATH)) {
        Serial.printf("[MEM] %s not found (empty memory)\n", MEM_FILE_PATH);
        return true;    // 无文件不算故障, 首次使用
    }
    File f = SD_MMC.open(MEM_FILE_PATH, FILE_READ);
    if (!f) {
        Serial.printf("[MEM] open %s FAILED\n", MEM_FILE_PATH);
        return false;
    }
    _memText = f.readString();
    f.close();
    _memText.trim();
    return true;
}

bool MemoryStore::_save() {
    File f = SD_MMC.open(MEM_FILE_PATH, FILE_WRITE);
    if (!f) {
        Serial.printf("[MEM] write %s FAILED\n", MEM_FILE_PATH);
        return false;
    }
    f.print(_memText);
    f.close();
    return true;
}

bool MemoryStore::add(const String& entry) {
    String e = entry;
    e.trim();
    if (e.length() == 0 || !_avail) return false;

    // 精确去重 (已有该条目则跳过, 防"记住X"重复说)
    if (_memText.indexOf(e) >= 0) {
        Serial.printf("[MEM] duplicate, skipped: \"%s\"\n", e.c_str());
        return false;
    }

    // 追加 (换行分隔)
    if (_memText.length() > 0) _memText += "\n";
    _memText += e;

    // 超上限 → 删最旧行 (从头删到不超)
    while (_memText.length() > MAX_MEM_BYTES) {
        int nl = _memText.indexOf('\n');
        if (nl < 0) { _memText = ""; break; }        // 单行超限 → 清空重建
        _memText = _memText.substring(nl + 1);
    }

    if (!_save()) return false;
    Serial.printf("[MEM] added (%u bytes now): \"%s\"\n",
                  _memText.length(), e.c_str());
    return true;
}

void MemoryStore::clearAll() {
    _memText = "";
    if (_avail) {
        if (SD_MMC.exists(MEM_FILE_PATH)) SD_MMC.remove(MEM_FILE_PATH);
        Serial.println("[MEM] cleared");
    }
}
