# -*- coding: utf-8 -*-
"""
gen_font_cn.py — 生成中文点阵字库子集 (P6.1)
用法: python tools/gen_font_cn.py
输出: src/display/font_cn.h  (PROGMEM, 每字 24x24 = 72 字节)

字体源: Windows 系统字体 (msyh.ttc 优先), 只生成固件用到的固定文案字符,
整库 ~36 字 x 72B ≈ 2.6KB, 不带完整字库 (资源占用原则)。

需要改文案 → 改下方 TEXTS 后重跑本脚本, 再重编固件。
"""
from PIL import Image, ImageDraw, ImageFont
import os

# ── 固件用到的全部中文文案 (改文案在这里加, 重新生成) ──
TEXTS = [
    "睡眠",          # 状态: 睡眠
    "聆听中",        # 状态: 聆听中
    "思考中",        # 状态: 思考中
    "播报中",        # 状态: 播报中
    "等待",          # 状态: 等待 (附数字倒数)
    "网络不好，请再说一遍",   # 失败提示
    "拍照中",        # PHOTO 命令
    "连接中",        # 开机 WiFi
    "离线",          # WiFi 断线图标旁
    "已存卡",        # 拍照成功
    "没存上",        # 拍照失败
    "吐舌", "眨眼", "微笑",  # 换个表情彩蛋
    "好梦",          # 睡觉命令
    "小智",          # 开机标题
    "欢迎回来",      # P7a: 人脸识别欢迎 (屏幕播报)
    "已录入",        # P7a: 注册成功
    # "没录上" 的"没/上"已在"没存上"里
]

FONT_CANDIDATES = [
    r"C:\Windows\Fonts\msyh.ttc",    # 微软雅黑
    r"C:\Windows\Fonts\simhei.ttf",  # 黑体
    r"C:\Windows\Fonts\simsun.ttc",  # 宋体
]

SIZE = 24  # 点阵尺寸 24x24

def load_font():
    for p in FONT_CANDIDATES:
        if os.path.exists(p):
            return ImageFont.truetype(p, SIZE), p
    raise SystemExit("no Chinese font found")

def render_glyph(font, ch):
    """渲染单字 → 24x24 位图 (每行 3 字节 MSB first)"""
    img = Image.new("1", (SIZE, SIZE), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (SIZE - w) // 2 - bbox[0]
    y = (SIZE - h) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=1)
    px = img.load()
    rows = []
    for ry in range(SIZE):
        row = 0
        b = bytearray(3)
        for rx in range(SIZE):
            if px[rx, ry]:
                b[rx // 8] |= (0x80 >> (rx % 8))
        rows.extend(b)
    return bytes(rows)

def main():
    font, path = load_font()
    chars = sorted(set("".join(TEXTS) + "，"))  # 去重 + 按码点排序 (二分查找)
    print(f"font: {path}, {len(chars)} glyphs")

    out = []
    out.append("/**")
    out.append(" * font_cn.h — 中文点阵字库子集 24x24 (P6.1 自动生成, 勿手改)")
    out.append(" * 生成: tools/gen_font_cn.py | 字符数: %d | 体积: %d B" % (len(chars), len(chars) * 72))
    out.append(" * 改文案: 编辑脚本 TEXTS 后重跑生成")
    out.append(" */")
    out.append("#pragma once")
    out.append("#include <pgmspace.h>")
    out.append("")
    out.append("struct CnGlyph { uint16_t cp; uint8_t bits[72]; };  // cp=Unicode 码点")
    out.append("")
    out.append("static const CnGlyph kCnGlyphs[] PROGMEM = {")
    for ch in chars:
        cp = ord(ch)
        bits = render_glyph(font, ch)
        hexs = ",".join("0x%02X" % b for b in bits)
        out.append("    {0x%04X, {%s}}," % (cp, hexs))
    out.append("};")
    out.append("")
    out.append("#define CN_GLYPH_COUNT %d" % len(chars))

    dst = os.path.join(os.path.dirname(__file__), "..", "src", "display", "font_cn.h")
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print(f"written: {dst} ({os.path.getsize(dst)} bytes source)")

if __name__ == "__main__":
    main()
