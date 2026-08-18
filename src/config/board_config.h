/**
 * @file board_config.h
 * @brief AI Desktop Companion — 全板引脚定义（单一真相来源）
 *
 * 硬件: GOOUUU ESP32-S3-CAM (ESP32-S3-WROOM-1 N16R8, 16MB Flash + 8MB OPI PSRAM)
 *
 * !! 重要：N16R8 使用八进制 PSRAM，GPIO 33~37 被 PSRAM 占用，不可用作普通 GPIO !!
 *    原 WebServer 项目中 ST7789 DC=35、INMP441 SD=36 均踩雷，已修正。
 *
 * 本文件整合了原 camera_pins.h / st7789_pins.h / inmp441_pins.h / max98357a_pins.h
 * 四个文件的引脚定义，作为全项目唯一的引脚真相来源。
 */

#pragma once

// ============================================================
//  摄像头 (OV3660 / GC2145, 8-bit DVP)
//  引脚与原 WebServer camera_pins.h 完全一致，未做改动
// ============================================================
#define PWDN_GPIO_NUM    -1    // 未使用
#define RESET_GPIO_NUM   -1    // 未使用
#define XCLK_GPIO_NUM    15    // 外部时钟 (14~20MHz)
#define SIOD_GPIO_NUM    4     // SCCB SDA
#define SIOC_GPIO_NUM    5     // SCCB SCL
#define Y2_GPIO_NUM      11    // D0
#define Y3_GPIO_NUM      9     // D1
#define Y4_GPIO_NUM      8     // D2
#define Y5_GPIO_NUM      10    // D3
#define Y6_GPIO_NUM      12    // D4
#define Y7_GPIO_NUM      18    // D5
#define Y8_GPIO_NUM      17    // D6
#define Y9_GPIO_NUM      16    // D7
#define VSYNC_GPIO_NUM   6     // 垂直同步
#define HREF_GPIO_NUM    7     // 水平参考
#define PCLK_GPIO_NUM    13    // 像素时钟

// ============================================================
//  ST7789 TFT LCD (1.5寸 240x240, SPI)
//  修正: DC 从 35(PSRAM冲突) → 19
//  修正: 屏幕尺寸从 240x320 → 240x240
//  GPIO 19/20 原生 USB 未启用(CH340 占 USB)，可安全使用
// ============================================================
#define LCD_SCK_GPIO_NUM    21    // SPI SCK
#define LCD_MOSI_GPIO_NUM   47    // SPI MOSI
#define LCD_CS_GPIO_NUM     41    // 片选
#define LCD_DC_GPIO_NUM     19    // 数据/命令 (原35→19, 避开PSRAM)
#define LCD_RST_GPIO_NUM    45    // 复位
#define LCD_BL_GPIO_NUM     42    // 背光 (高电平点亮)

#define LCD_WIDTH           240
#define LCD_HEIGHT          240   // 1.5寸屏为 240x240 (非320)
#define LCD_SPI_FREQ_HZ     40000000   // 40MHz; 杜邦线长则降到 10~20MHz
#define LCD_INVERT_COLOR    1     // IPS 屏需要颜色反转
#define LCD_RGB_ORDER       1     // 0=BGR, 1=RGB

// ============================================================
//  MAX98357A I2S 功放 (音频输出)
//  引脚与原 max98357a_pins.h 一致，未改动
// ============================================================
#define SPK_BCLK_GPIO_NUM   1     // BCLK 位时钟
#define SPK_LRCK_GPIO_NUM   2     // LRCLK 帧时钟
#define SPK_DIN_GPIO_NUM    14    // DIN 数据输入
#define SPK_I2S_PORT        0     // I2S 端口 0 (喇叭)
#define SPK_SAMPLE_RATE     16000 // 播放采样率 16kHz
#define SPK_SLOT_BITS       32    // 32bit slot, mono 左槽

// ============================================================
//  INMP441 I2S 麦克风 (音频输入)
//  修正: SD 从 36(PSRAM冲突) → 20
// ============================================================
#define MIC_WS_GPIO_NUM     3     // WS / LRCK
#define MIC_SCK_GPIO_NUM    46    // SCK / BCLK
#define MIC_SD_GPIO_NUM     20    // SD 数据输出 (原36→20, 避开PSRAM)
#define MIC_I2S_PORT        1     // I2S 端口 1 (麦克风)
#define MIC_SAMPLE_RATE     16000 // 采集采样率 16kHz
#define MIC_SLOT_BITS       32    // INMP441: 24bit数据/32bit帧, 右移12位转16bit

// ============================================================
//  WS2812B RGB LED (板载)
// ============================================================
#define WS2812_GPIO_NUM     48    // 板载数据引脚
#define WS2812_COUNT        1     // LED 数量 (板载1颗)

// ============================================================
//  SD 卡 (板载 SDMMC)
//  注意: 不用 SD_CLK_GPIO_NUM/SD_CMD_GPIO_NUM 命名，
//        与 ESP32-S3 SDK 内置宏 (io_mux_reg.h) 冲突，改用 SD_MMC_ 前缀
// ============================================================
#define SD_MMC_CLK_GPIO_NUM  39
#define SD_MMC_CMD_GPIO_NUM  38
#define SD_MMC_D0_GPIO_NUM   40

// ============================================================
//  BOOT 按键
// ============================================================
#define BOOT_BTN_GPIO_NUM   0     // Strapping pin: boot 时不能拉低

// ============================================================
//  空闲引脚 (可供扩展)
//  43, 44 — UART0 TX/RX (禁用 Serial 后可用)
// ============================================================
