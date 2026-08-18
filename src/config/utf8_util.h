/**
 * @file utf8_util.h
 * @brief UTF-8 安全截断工具 (header-only)
 *
 * 背景 (P6-disp-v1f 踩坑): 中文 3 字节/字, snprintf/strncpy 固定缓冲截断时
 * 若切在多字节序列中间 → 尾部残留坏字节:
 *   - 发给云端 → GLM 报 "Invalid UTF-8 middle byte 0x22" 直接 HTTP 400
 *   - 传给显示 → 点阵查表失败, 屏幕出乱码
 * 规则: 任何接收外部/中文文本的 char[] 缓冲, 一律用 utf8Copy 代替 strncpy。
 */
#pragma once
#include <stdint.h>
#include <string.h>

/**
 * 拷贝 src 到 dst (最多 dstSize-1 字节), 保证:
 *   1. NUL 结尾
 *   2. 永远不切断 UTF-8 多字节序列 (放不下的整个字符丢弃)
 */
inline void utf8Copy(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t len = strlen(src);
    if (len < dstSize) {                 // 全放得下
        memcpy(dst, src, len);
        dst[len] = '\0';
        return;
    }

    size_t n = dstSize - 1;              // 最多拷贝的字节数
    // 回退跳过续字节 (10xx xxxx), n 停在某个字符的首字节 (或 ASCII)
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;

    // 该首字节的完整序列若放不进 n+1 字节, 连首字节一起丢弃
    uint8_t c = (uint8_t)src[n];
    size_t seqLen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                   : ((c & 0xF0) == 0xE0) ? 3 : 4;
    if (n + seqLen > dstSize - 1) n = (n > 0) ? n - 1 : 0;

    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * 返回 ptr 从 offset 开始、最多 maxBytes 内的"完整 UTF-8 字符数"字节数
 * (用于换行/分片时不对齐到坏字节)
 */
inline size_t utf8ClampBytes(const char* s, size_t offset, size_t maxBytes) {
    size_t i = offset, end = offset + maxBytes;
    while (s[i]) {
        uint8_t c = (uint8_t)s[i];
        size_t seqLen = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2
                       : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (i + seqLen > end) break;     // 这个字符放不下, 不含它
        i += seqLen;
    }
    return i - offset;
}
