/**
 * @file system_config.example.h
 * @brief 系统配置模板 (占位) — 复制为 src/config/system_config.h 并填入真实值
 *
 * ⚠️ 真实 system_config.h 含 API key 和家庭 WiFi 密码, 已被 .gitignore 排除, 不入库。
 * 本文件为可提交的占位模板, 所有敏感值用 <YOUR_XXX> 代替。
 *
 * 使用: 把本文件复制为 src/config/system_config.h, 替换 <YOUR_XXX> 占位符。
 */
#pragma once

// ---- WiFi (多 AP 备选) ----
#define WIFI1_SSID      "<YOUR_WIFI_SSID>"
#define WIFI1_PASS      "<YOUR_WIFI_PASS>"
#define WIFI2_SSID      "<YOUR_WIFI_SSID2>"
#define WIFI2_PASS      "<YOUR_WIFI_PASS2>"
#define WIFI_AP_COUNT   2
#define WIFI_TIMEOUT_MS 12000

// ---- 串口 ----
#define SERIAL_BAUD     115200

// ---- 系统版本 ----
#define FW_VERSION      "P8d-v1q"
#define COMPANION_NAME  "AI Desktop Companion"

// ---- P6 等待窗口 ----
#define CONV_WAIT_WINDOW_MS   10000
#define CONV_VAD_TRIG_MS      600
#define CONV_LISTEN_TAIL_GUARD_MS 700

// ---- P4-1 唤醒词 ----
#define WAKE_MODEL_PARTITION "model"
#define WAKE_MODEL_ADDR      0x520000
#define WAKE_ARM_WINDOW_MS   10000
#define WAKE_DETECT_MODE     DET_MODE_90
#define WAKE_DET_THRESHOLD   0.5f

// ---- P5 v1n: 情绪提示音总开关 ----
#define EMOTION_CHIMES_ENABLED 0

// ---- 云端 API (填入真实凭据) ----
// 百度智能云语音技术 (STT/TTS)
#define BAIDU_API_KEY    "<YOUR_BAIDU_API_KEY>"
#define BAIDU_STT_URL    "https://vop.baidu.com/server_api"
#define BAIDU_TTS_URL    "https://tsn.baidu.com/text2audio"
#define BAIDU_CUID       "esp32_companion_001"

// 智谱AI (LLM): GLM-4-Flash
#define ZHIPU_API_KEY    "<YOUR_ZHIPU_API_KEY>"
#define ZHIPU_LLM_URL    "https://open.bigmodel.cn/api/paas/v4/chat/completions"
#define ZHIPU_MODEL      "glm-4-flash"

// 智谱AI (视觉): GLM-4V-Flash
#define ZHIPU_VISION_MODEL   "glm-4v-flash"
#define VISION_MAX_IMAGE_BYTES  (300 * 1024)

// ---- 视觉对话触发词 ----
#define VISION_TRIGGER_WORDS  "看看,看下,看一,瞅瞅,看啥,看这,看那,这是什么,看到什么,看到啥,看看我,看我"
#define VISION_PROMPT_DEFAULT "仔细看看这张照片，用一两句简短口语描述你看到了什么（不超过50字，不用表情符号）"

// ---- 对话参数 ----
#define CONV_MAX_RECORD_S    10
#define CONV_MIN_RECORD_MS   500
#define CONV_SILENCE_MS      1000
#define CONV_COOLDOWN_MS     2000
#define CONV_TTS_MAX_CHARS   100

// ---- P7b 云端迷你音乐 ----
#define MUSIC_SERVER_URL     "http://<PC_LAN_IP>:8000"
#define MUSIC_DOWNLOAD_TIMEOUT_MS 30000

// ---- P8a 知识服务网关 (天气/音乐) ----
#define KNOWLEDGE_SERVER_URL  "http://<PC_LAN_IP>:8000"

// ---- P7a 人脸识别 ----
#define CONV_FACE_PERIOD_MS  800
#define CONV_FACE_GREET_MS   60000

#define LLM_SYSTEM_PROMPT    "你是\"小智\"，一个桌面AI情感伴侣，运行在ESP32小设备上，住在西安。性格活泼温暖，像好朋友一样。回复必须非常简短：不超过50字，一般只用一两句话。语气口语化、自然，可以带点俏皮。绝对禁止输出emoji和任何表情符号（你的语音通道无法朗读它们）。不要解释你是AI，不要长篇大论，不要连续提问。遇到不知道或不确定的事（比如具体地点、天气、温度、数字），直接说\"这个我不确定哦\"，绝不编造。作为住在西安的小智，聊天时可以多提到西安本地的话题：西安的天气、景点、美食、文化等，用户问西安或陕西相关的问题时优先用本地视角回答。如果用户说唱首歌、放音乐、弹钢琴这些，你直接说\"好嘞，给你弹一首\"或者\"来啦，钢琴曲走起\"，不要解释说不能播放，因为小智真的会弹钢琴（本地有旋律功能）。"
#define LLM_MAX_HISTORY      3
