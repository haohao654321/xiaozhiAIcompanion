# 小智 · AI Desktop Companion

多模态 AI 桌面情感伴侣。基于 ESP32-S3，通过**语音对话 + 摄像头视觉 + 人脸识别**，驱动 TFT 表情屏、语音功放、RGB 灯效。核心特色是**真查询**：天气等真实数据直接查知识服务网关播报，不做 LLM 嘴炮。

## ✨ 功能

- 🎤 **离线唤醒词**：喊"你好小智"唤醒（esp-sr WakeNet9，真人 5/5 全中零误报）
- 💬 **云端对话**：唤醒后任意说话 → 百度 STT → 智谱 GLM-4-Flash → 百度 TTS 播报
- 📍 **真查询天气**：说"今天天气 / 明天冷不冷 / 北京天气" → 和风天气真数据 → TTS 播报（不做 LLM 编造）
- 🎵 **云端音乐**：说"放首歌 / 弹钢琴" → PC 网关合成小星星等旋律播放
- 📷 **视觉对话**：说"看看" → 拍照 → GLM-4V-Flash 描述画面
- 📸 **拍照存卡**：说"拍照" 或 双击 BOOT → 存 SD 卡
- 👤 **人脸识别**：睡眠时周期检测，识别到已注册家人 → "欢迎回来，<名字>"
- 🎨 **状态显示**：ST7789 5 态中文屏（睡眠/聆听/思考/播报/等待）+ WS2812 情绪灯

## 🧠 触发架构 (P8d)

```
唤醒词"你好小智" → 聆听窗口 → VAD 录音 → 百度 STT 识别文本
   → 文本关键词路由:
      天气/穿衣/冷不冷 → 知识网关真查询 (城市/天数/穿衣) → TTS
      音乐/歌/唱     → 云端音乐
      看看           → 视觉对话
      拍照           → 拍照
      表情/睡觉      → 本地动作
      未命中         → LLM 自然对话
```

关键词路由替代了旧 MultiNet 命令词——**用户随意说话也能命中**（只要关键词在），不依赖固定短语。天气查询城市表覆盖 40+ 城市，支持"明天/后天"+ 穿衣建议。

## 🔧 硬件

| 外设 | 型号 | 接口 | 关键引脚 |
|---|---|---|---|
| 主控 | GOOUUU ESP32-S3-CAM (N16R8) | — | 16MB Flash + 8MB PSRAM |
| 摄像头 | OV3660 / GC2145 (板载) | DVP 8-bit | 见 board_config.h |
| 显示屏 | ST7789 1.5" 240x240 | SPI | SCK=21 MOSI=47 CS=41 DC=19 RST=45 BL=42 |
| 功放 | MAX98357A | I2S | BCLK=1 LRCK=2 DIN=14 |
| 麦克风 | INMP441 | I2S | WS=3 SCK=46 SD=20 |
| LED | WS2812B (板载) | — | GPIO48 |
| SD卡 | 板载 | SDMMC | CLK=39 CMD=38 D0=40 |

> ⚠️ N16R8 八进制 PSRAM 占用 GPIO 33~37，**绝不能用**。DC/SD 引脚已从 35/36 改到 19/20。

## 🚀 快速开始

1. 安装 [PlatformIO](https://platformio.org/)（VSCode 插件）
2. 克隆本仓库，**复制 `config/system_config.example.h` → `src/config/system_config.h`**，填入真实 WiFi 和 API key
3. 修改 `platformio.ini` 的 `upload_port` / `monitor_port` 为实际 COM 口
4. 烧录唤醒词模型（一次性）：`esptool write_flash 0x520000 tools/srmodels.bin`
5. 启动 PC 知识网关：`cd tools && set QWEATHER_KEY=xxx && python knowledge_server.py`
6. 编译上传，串口监视器 (115200) 查看日志

## 📦 PC 端知识服务网关 (tools/knowledge_server.py)

纯 Python 标准库，零依赖。ESP32 通过局域网 HTTP 调用：
- `GET /weather?city=西安&days=1&clothes=1` → 天气真数据文本
- `GET /music?q=小星星` → PCM 音乐
- `GET /list` → 歌曲列表

天气数据来自**和风天气**（新版 API：`X-QW-Api-Key` 请求头 + gzip 响应），网关做 1h 缓存防超免费额度。

## 📁 项目结构

```
src/
  audio_in/      mic_manager (INMP441 VAD) + wake_word (唤醒词)
  audio_out/     speaker_manager (MAX98357A)
  camera/        camera_manager + capture_manager (拍照/取帧)
  companion/     conversation_manager (对话编排+文本路由) + state_machine (情绪枚举)
  config/        board_config.h (引脚) + system_config.h (凭据, 不入库)
  display/       display_manager + display_state (5态屏) + font_cn.h (中文字库)
  led/           led_controller (WS2812B)
  network/       wifi/stt/llm/tts/vision/music/weather 各客户端
  vision/        face_recognition (ESP-DL 人脸识别)
tools/           knowledge_server.py (PC网关) + srmodels.bin (唤醒词模型) + 构建脚本
```

## 🛡️ 安全

- 真实凭据（API key、WiFi 密码）在 `src/config/system_config.h`，已被 `.gitignore` 排除
- 仓库只提交 `config/system_config.example.h` 占位模板
- API key 不上 ESP32 固件（天气/音乐走 PC 网关，key 只存 PC 端）

## 📄 License

MIT — 详见 LICENSE。
