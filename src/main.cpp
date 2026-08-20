/**
 * @file main.cpp
 * @brief AI Desktop Companion — 主入口
 *
 * P1 骨架阶段：
 *   - 初始化串口、打印系统信息
 *   - 摄像头自动检测 PID（GC2145 / OV3660），串口输出型号
 *   - 各外设模块 stub 初始化（打印引脚和状态，P2 逐步填入实现）
 *
 * P4.5 摄像头应用层：
 *   - BOOT 键: 单击=切情绪, 双击=拍照存SD
 *   - 视觉对话: 语音说"看看/看下/瞅瞅..." → 拍照 → GLM-4V-Flash → TTS 播报
 *   - Web 视频流: http://<ip>/ 浏览器实时看摄像头
 *
 * 编译: PlatformIO + Arduino framework (ESP32-S3 N16R8)
 */

#include <Arduino.h>
#include "config/board_config.h"
#include "config/system_config.h"
#include "camera/camera_manager.h"
#include "camera/capture_manager.h"
#include "display/display_manager.h"
#include "display/display_state.h"   // P6: 5 状态中文显示
#include "audio_in/mic_manager.h"
#include "audio_in/wake_word.h"     // P4-1: esp-sr 唤醒词
#include "audio_out/speaker_manager.h"
#include "led/led_controller.h"
#include "network/wifi_manager.h"
#include "network/camera_web.h"
#include "companion/state_machine.h"
#include "companion/conversation_manager.h"
#include "vision/face_recognition.h"   // P7a: 人脸识别 (本地)

// ── 全局实例 ──
CameraManager       camera;
CaptureManager      capture;
DisplayManager      display;
DisplayState        ui;               // P6: 5 状态中文显示状态机
MicManager          mic;
WakeWord            wakeWord;         // P4-1: "你好小智" 离线唤醒
SpeakerManager      speaker;
LEDController       led;
WiFiManagerCompanion wifi;
ConversationManager  conversation;
FaceRecognition      faceRecognition; // P7a: 人脸检测+识别 (3人注册/欢迎)
static bool g_webStarted = false;   // P4.5: 摄像头 Web 是否已启动

// ── P3: BOOT 按键 (GPIO0, 按下沿触发 + 防抖)
//    P4.5: 单击=切情绪, 双击(400ms内)=拍照存SD ──
static bool     g_btnDown = false;      // 当前按下状态
static uint32_t g_btnDownAt = 0;        // 本次按下时刻
static uint32_t g_btnLastPress = 0;     // 上次单击时刻 (双击窗口判定)

static void setupButton() {
    pinMode(BOOT_BTN_GPIO_NUM, INPUT_PULLUP);
    g_btnDown = (digitalRead(BOOT_BTN_GPIO_NUM) == LOW);
}

// 拍照动作 (双击触发)
static void takePhotoAction() {
    String path;
    bool ok = capture.takePhotoAndSave(&path);
    if (ok) {
        Serial.printf("[BTN] Photo saved: %s\n", path.c_str());
        ui.showMessage("已存卡", 2000);
    } else {
        Serial.println("[BTN] Photo FAILED");
        ui.showMessage("没存上", 2000);
    }
}

static void pollButton() {
    bool down = (digitalRead(BOOT_BTN_GPIO_NUM) == LOW);
    uint32_t now = millis();

    if (down && !g_btnDown) {                     // 按下沿
        if (g_btnLastPress && (now - g_btnLastPress < 400)) {
            g_btnLastPress = 0;                   // 双击, 取消待定单击
            Serial.println("[BTN] Double press -> take photo");
            takePhotoAction();
        } else {
            g_btnDownAt = now;                    // 等待松开判定单击
        }
        g_btnDown = true;
    }

    if (!down && g_btnDown) {                     // 松开沿
        if (now - g_btnDownAt >= 50) {            // 防抖
            if (g_btnLastPress == 0) {            // 无双击发生 → 单击
                Serial.println("[BTN] Single press -> manual wake");
                conversation.manualWake();        // P6: 手动唤醒 (替代旧切表情)
            }
        }
        g_btnDown = false;
        g_btnLastPress = now;                     // 记录供双击窗口
    }
}

// ── 打印系统信息 ──
void printSystemInfo() {
    Serial.println("\n");
    Serial.println("╔══════════════════════════════════════════════╗");
    Serial.println("║     AI Desktop Companion  —  ESP32-S3       ║");
    Serial.printf("║     FW: %-37s║\n", FW_VERSION);
    Serial.println("╚══════════════════════════════════════════════╝");

    Serial.println("\n--- System Info ---");
    Serial.printf("  Chip:        %s Rev %d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("  Cores:       %d\n", ESP.getChipCores());
    Serial.printf("  Flash:       %d MB\n", ESP.getFlashChipSize() / 1048576);
    Serial.printf("  Flash Speed: %d MHz\n", ESP.getFlashChipSpeed() / 1000000);
    Serial.printf("  PSRAM:       %s", psramFound() ? "OK" : "MISSING");
    if (psramFound()) {
        Serial.printf(" (%d MB)", (int)(ESP.getPsramSize() / 1048576));
    }
    Serial.println();
    Serial.printf("  Free Heap:   %d KB\n", (int)(ESP.getFreeHeap() / 1024));
    Serial.printf("  Free PSRAM:  %d KB\n", (int)(ESP.getFreePsram() / 1024));

    Serial.println("\n--- Pin Map (board_config.h) ---");
    Serial.println("  Camera:  D0-D7=11,9,8,10,12,18,17,16 | XCLK=15 PCLK=13");
    Serial.println("           VSYNC=6 HREF=7 SDA=4 SCL=5");
    Serial.printf("  Display: SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d (%dx%d)\n",
                  LCD_SCK_GPIO_NUM, LCD_MOSI_GPIO_NUM, LCD_CS_GPIO_NUM,
                  LCD_DC_GPIO_NUM, LCD_RST_GPIO_NUM, LCD_BL_GPIO_NUM,
                  LCD_WIDTH, LCD_HEIGHT);
    Serial.printf("  Speaker: BCLK=%d LRCK=%d DIN=%d (I2S%d)\n",
                  SPK_BCLK_GPIO_NUM, SPK_LRCK_GPIO_NUM, SPK_DIN_GPIO_NUM, SPK_I2S_PORT);
    Serial.printf("  Mic:     WS=%d SCK=%d SD=%d (I2S%d)\n",
                  MIC_WS_GPIO_NUM, MIC_SCK_GPIO_NUM, MIC_SD_GPIO_NUM, MIC_I2S_PORT);
    Serial.printf("  LED:     GPIO%d (%d LEDs)\n", WS2812_GPIO_NUM, WS2812_COUNT);
    Serial.printf("  SD:      CLK=%d CMD=%d D0=%d\n", SD_MMC_CLK_GPIO_NUM, SD_MMC_CMD_GPIO_NUM, SD_MMC_D0_GPIO_NUM);
    Serial.println();
}

// ── setup ──
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(500);

    printSystemInfo();

    // ── 摄像头（核心，P1 需要确认型号）──
    Serial.println(">>> [1/7] Camera (PID detection) <<<");
    bool camOk = camera.begin();
    capture.begin(&camera);   // P4.5: 拍照管理器 (探测 SD)

    // ── 外设 stub（P2 逐步实现）──
    Serial.println(">>> [2/7] Display (ST7789) <<<");
    display.begin();
    ui.begin(&display);                    // P6: 显示状态机 (初始睡眠)
    ui.showMessage("连接中", 12000);        // WiFi 连接期间显示

    Serial.println(">>> [3/7] Microphone (INMP441) <<<");
    mic.begin();

    Serial.println(">>> [4/7] Speaker (MAX98357A) <<<");
    speaker.begin();

    Serial.println(">>> [5/7] LED (WS2812B) <<<");
    led.begin();

    Serial.println(">>> [6/7] WiFi <<<");
    wifi.connect();

    // ── P4-1: 唤醒词 (模型在 flash model 分区; 失败自动降级 VAD 触发) ──
    // P8d: MultiNet 命令词已退役 (用户说话随意, 固定短语命中不了) —
    //   改由 STT 文本关键词路由替代, 唤醒后说话一律录音→STT→路由
    if (wakeWord.begin(WAKE_MODEL_PARTITION)) {
        mic.setWakeWord(&wakeWord);                 // 环境监听 PCM 顺手喂 WakeNet
        ui.showMessage("小智", 2000);
    } else {
        display.showToast("no wake model", 2500);   // 调试信息 (ASCII)
    }

    conversation.begin(&mic, &speaker, &wifi, &capture, &wakeWord, &faceRecognition);
    setupButton();

    // ── P7a: 人脸识别初始化 (检测器+识别器, flash 加载已注册 ID) ──
    faceRecognition.begin();

    // ── P4.5: 摄像头 Web 服务器 (浏览器实时视频流) ──
    if (camOk && wifi.isConnected()) {
        g_webStarted = startCameraWeb(&capture);
    } else {
        Serial.printf("[MAIN] Camera web skipped (cam=%d, wifi=%d)\n", camOk, wifi.isConnected());
    }

    // ── P6: 初始状态 = 睡眠 (旧情绪广播/提示音体系已废弃) ──
    ui.setOnline(wifi.isConnected());
    ui.setSleep();
    led.setEmotion(EMOTION_SLEEPY);
    led.update();

    // ── 启动摘要 ──
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║              BOOT COMPLETE                   ║");
    Serial.println("╚══════════════════════════════════════════════╝");
    Serial.printf("  Camera:  %s %s (PID 0x%04X, %s, %dx%d)\n",
                  camOk ? "OK" : "FAIL",
                  camera.getModelStr(),
                  camera.getPID(),
                  camera.getPixelFormatStr(),
                  camera.getWidth(), camera.getHeight());
    Serial.printf("  HW JPEG: %s | SD: %s\n",
                  camera.isHardwareJPEG() ? "YES" : "NO (software)",
                  capture.sdAvailable() ? "OK" : "MISSING");
    Serial.println("  P6: 5-state CN display (sleep/listen/think/speak/wait + countdown)");
    Serial.printf("  WiFi:   %s %s (RSSI %d dBm)\n",
                  wifi.isConnected() ? "OK" : "FAIL",
                  wifi.getIP().c_str(),
                  wifi.getRSSI());
    Serial.println("  P4: WiFi ready for cloud LLM conversation");
    Serial.println("  P4.5: Camera ready (double-press BOOT = photo, say \"看看\" = vision chat)");
    if (faceRecognition.isReady()) {
        Serial.printf("  P7a: Face recognition ready (%d enrolled, thresh %.2f; REG=name to enroll)\n",
                      faceRecognition.enrolledCount(), faceRecognition.getThreshold());
    }
    if (camOk && wifi.isConnected()) {
        Serial.printf("  Web:    http://%s/  (MJPEG stream)\n", wifi.getIP().c_str());
    }
    Serial.println("\n  >>> Boot done. Speak near mic / press BOOT to test <<<");
    Serial.println("  >>> Serial: WAKEONLY (P4.1 isolation) | WAKETH=x | VADTH=x | REG=name (P7a enroll) | HELP <<<");
    Serial.println();
}

// ── loop ──
void loop() {

    // v1r: 串口命令 — 自动化测试 (唤醒词 TTS 基线 + 调试)
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toUpperCase();

        if (cmd == "WAKEONLY") {
            // v1u: P4.1 隔离模式 — 只测唤醒, 屏蔽 VAD/云端
            bool on = !conversation.wakeOnly();
            conversation.setWakeOnly(on);
            if (on) {
                Serial.println("\n[WAKE-ONLY] === P4.1 ISOLATION MODE ON ===");
                Serial.println("[WAKE-ONLY] VAD->conversation blocked, cloud blocked.");
                Serial.println("[WAKE-ONLY] Say '你好小智' repeatedly; each hit = LED flash + 'Wake!' toast + '[CONV] WAKE-ONLY HIT' log.");
                Serial.println("[WAKE-ONLY] Send WAKEONLY again to exit.\n");
                display.showToast("ISO MODE ON", 2000);
            } else {
                Serial.println("[WAKE-ONLY] === isolation OFF, normal operation ===");
                display.showToast("ISO MODE OFF", 2000);
            }
        } else if (cmd.startsWith("WAKETH=")) {
            // v1u: 运行时调灵敏度, e.g. WAKETH=0.7
            float t = cmd.substring(7).toFloat();
            if (t >= 0.5f && t <= 0.9999f) {
                wakeWord.setThreshold(t);
            } else {
                Serial.printf("[WAKE] invalid threshold (need 0.5~0.9999), current %.4f\n",
                              wakeWord.getThreshold());
            }
        } else if (cmd.startsWith("VADTH")) {
            // v1v: 运行时调 VAD 说话 RMS 下限, e.g. VADTH=150 (越低越灵敏)
            if (cmd.startsWith("VADTH=")) {
                float v = cmd.substring(6).toFloat();
                if (v >= 80.0f && v <= 2000.0f) mic.setSpeechThreshold(v);
                else Serial.println("[VAD] invalid value (need 80~2000)");
            }
            Serial.printf("[VAD] speech RMS threshold = %.0f (lower = easier to trigger)\n",
                          mic.getSpeechThreshold());
        } else if (cmd.startsWith("REG=")) {
            // P7a: 注册人脸 REG=张三 (站摄像头前, 脸占画面 1/4 以上)
            // 名字可选: REG 或 REG= (空名也能注册, 识别后只播"欢迎回来")
            String name = cmd.substring(4);
            name.trim();
            conversation.enrollFace(name);
        } else if (cmd == "REG") {
            conversation.enrollFace("");
        } else if (cmd == "FACELIST") {
            conversation.faceList();
        } else if (cmd == "FACEDEL") {
            conversation.faceDelete();
        } else if (cmd == "FACECLR") {
            conversation.faceClear();
        } else if (cmd.startsWith("FACETH=")) {
            float t = cmd.substring(7).toFloat();
            if (t >= 0.1f && t <= 0.99f) conversation.faceSetThreshold(t);
            else Serial.println("[FACE] invalid threshold (need 0.1~0.99)");
        } else if (cmd == "FACETEST") {
            conversation.faceTest();
        } else if (cmd == "MEMCLEAR") {
            // P10: 清空 SD 长期记忆 (误存/测试后清理)
            conversation.clearMemory();
            Serial.println("[MEM] cleared by serial command");
        } else if (cmd == "MEMLIST") {
            // P10: 打印当前长期记忆内容
            conversation.listMemory();
        } else if (cmd == "HELP") {
            Serial.println("Serial commands:");
            Serial.println("  WAKEONLY  - toggle P4.1 isolation (wake-only, no conv/cloud)");
            Serial.println("  WAKETH=x  - set wake threshold 0.5~0.9999 (lower=easier to trigger)");
            Serial.println("  VADTH=x   - set VAD speech RMS threshold 80~2000 (default 100, lower=easier)");
            Serial.println("  REG[=name] - P7a enroll current face (optional name)");
            Serial.println("  FACELIST / FACEDEL / FACECLR / FACETH=x / FACETEST - P7a face mgmt");
            Serial.println("  MEMCLEAR / MEMLIST - P10 long-term memory clear / list (SD)");
        }
    }

    // P3: 麦克风环境监听 (常驻 RMS → 音量等级 0~3)
    mic.update();

    // P4: WiFi 非阻塞维护 (断线重连)
    static bool g_wifiLast = false;
    wifi.update();
    if (wifi.isConnected() != g_wifiLast) {
        g_wifiLast = wifi.isConnected();
        ui.setOnline(g_wifiLast);              // P6: 绿点/红字离线
        Serial.printf("[MAIN] WiFi status: %s\n", g_wifiLast ? "connected" : "disconnected");
        // P4.5: WiFi 恢复后若摄像头可用且 web 未启动, 补启动
        if (g_wifiLast && !g_webStarted) {
            g_webStarted = startCameraWeb(&capture);
        }
    }

    // P3: BOOT 键 (单击=手动唤醒 / 双击=拍照)
    pollButton();

    // P4: 对话管理器 (非阻塞; STT/LLM/TTS 阶段会阻塞)
    conversation.update();

    // P6: 显示状态机驱动 (消息到期/睡眠 Z 动画)
    ui.update();

    // P6: 旧 FSM (听声变脸/安静犯困链/BOOT 切表情) 已废弃 —
    //     显示由 DisplayState 5 态驱动, LED 由 conversation._applyLed 映射

    // 渲染驱动 (呼吸/动画/DMA 推送)
    led.update();
    display.update();
    speaker.update();
    delay(1);   // 让出 CPU
}
