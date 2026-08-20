/**
 * @file conversation_manager.cpp
 * @brief 对话编排器实现 — P4 + P6 显示状态机
 *
 * P6 全链路: 唤醒 → 聆听 → 思考(云端) → 播报 → 等待倒数 → (命令/追问/唤醒/超时)
 * 旧情绪变脸体系已移除, 显示全部由 DisplayState 5 态驱动。
 */
#include "conversation_manager.h"
#include "../config/system_config.h"
#include "../config/board_config.h"    // MIC_SAMPLE_RATE
#include "../display/display_manager.h"
#include "../display/display_state.h"
#include "../led/led_controller.h"
#include "esp_heap_caps.h"
#include <WiFi.h>          // v1y: STT 失败时 WiFi.reconnect() 刷新链路
#include <string.h>

// v1h/v1j: 语音提示短语 (TTS per=0 度小美标准女声, 与对话回复同声线)
const char* const ConversationManager::VP_PHRASES[] = {
    "主人您好",   // VP_GREETING   — 唤醒应答
    "我先退下有事叫我",   // VP_SLEEP  — 超时/睡觉命令，提示后退睡
    "我在思考",   // VP_THINK_WAKE — 唤醒后问问题进入思考 (v1j)
    "我想想",     // VP_THINK_ASK  — 等待窗直接发问进入思考 (v1j)
};

// main.cpp 全局实例
extern DisplayManager display;
extern DisplayState   ui;
extern LEDController  led;

void ConversationManager::begin(MicManager* mic, SpeakerManager* spk,
                                WiFiManagerCompanion* wifi,
                                CaptureManager* capture, WakeWord* wake,
                                FaceRecognition* face) {
    _mic  = mic;
    _spk  = spk;
    _wifi = wifi;
    _capture = capture;
    _wake = wake;
    _face = face;
    _state = CONV_IDLE;
    _llm.resetHistory();
    _memory.begin();   // P10: 加载 SD 长期记忆 (无卡则静默降级)
    Serial.printf("[CONV] Conversation manager ready (STT: Baidu, LLM: GLM-4-Flash, TTS: Baidu PCM, Vision: GLM-4V-Flash, Wake: %s, Face: %s, Mem: %s)\n",
                  (_wake && _wake->isReady()) ? _wake->wordName() : "VAD-fallback",
                  (_face && _face->isReady()) ? "ON" : "off",
                  _memory.available() ? "SD" : "off");
}

// ── 进入状态 + LED 映射 ──
void ConversationManager::_enterState(ConvState s) {
    _state = s;
    _stateEnter = millis();
    static const char* names[] = {"IDLE","WAIT","RECORDING","STT","LLM","TTS","PLAYING"};
    Serial.printf("[CONV] -> %s\n", names[s]);
    _applyLed();
}

// P6: 显示状态 → LED 情绪映射 (P3 FSM 退役, LED 跟随 5 态)
void ConversationManager::_applyLed() {
    CompanionEmotion e;
    switch (_state) {
        case CONV_RECORDING: e = EMOTION_HAPPY;    break;   // 聆听
        case CONV_STT:
        case CONV_LLM:
        case CONV_TTS:       e = EMOTION_THINKING; break;   // 思考
        case CONV_PLAYING:   e = EMOTION_HAPPY;    break;   // 播报
        case CONV_WAIT:      e = EMOTION_NEUTRAL;  break;   // 等待
        default:             e = EMOTION_SLEEPY;   break;   // 睡眠
    }
    led.setEmotion(e);
    led.update();
}

// ── 是否可以开始对话 (P4 降级模式: 唤醒词未就绪时纯 VAD 触发) ──
bool ConversationManager::_canStart() const {
    if (_state != CONV_IDLE) return false;
    if (_wakeOnly) return false;
    if (!_wifi->isConnected()) return false;
    if (millis() < _cooldownUntil) return false;
    if (_spk->isPlaying()) return false;
    if (_spk->feedbackGateRemainingMs() > 0) return false;
    if (_wake && _wake->isReady()) return false;   // 唤醒词就绪 → 必须先唤醒
    if (_mic->getLevel() < 2) return false;
    return true;
}

// ── 主循环驱动 ──
void ConversationManager::update() {
    // v1u: 隔离模式命中 800ms 后回 NEUTRAL (调试模式, 保留旧表情显示)
    if (_wakeOnly && _isoHitAt && millis() - _isoHitAt >= 800) {
        _isoHitAt = 0;
        display.setEmotion(EMOTION_NEUTRAL);
        led.setEmotion(EMOTION_NEUTRAL);
        led.update();
    }

    // P7b: 钢琴旋律异步驱动 (每音 400ms, 非阻塞)
    if (_melodyActive) {
        if (millis() >= _melodyNextAt) {
            if (_melodyIdx < _melodyCount) {
                _spk->playTone(_melodyNotes[_melodyIdx], 360);
                _melodyIdx++;
                _melodyNextAt = millis() + 400;
            } else {
                _melodyActive = false;
                Serial.println("[CONV] Melody finished");
            }
        }
    }

    // P6: 唤醒事件公共处理 (IDLE 和 WAIT 都可唤醒)
    // v1z-A: 播报中唤醒 → 立即打断进聆听 (方案A: 用户喊"你好小智"即可停播)
    if (!_wakeOnly && _state != CONV_RECORDING &&
        _wake && _wake->isReady() && _wake->consumeWakeEvent()) {
        if (millis() >= _cooldownUntil) {
            if (_spk->isPlaying()) {
                Serial.println("[CONV] Wake during playback -> interrupt");
                _spk->stop();                           // 立即静音 (清缓冲+DMA)
                _melodyActive = false;                  // 本地旋律也停
                if (_ttsBuf) {                          // 防御: 未移交播放的 TTS 缓冲
                    heap_caps_free(_ttsBuf);
                    _ttsBuf = nullptr;
                }
                _ttsSamples = 0;
                _enterWait(true);                       // 聆听窗口 (10s 无语音回睡)
                _mic->resetNoiseFloor();
                _spk->playTone(880, 100);               // 短"叮"唤醒反馈
                return;
            }
            Serial.println("[CONV] Wake word trigger");
            // v1c: 唤醒后不立即录音 (旧方案录到唤醒词→STT→闪"思考中"白绕一圈);
            // 进聆听窗口: 显示聆听脸等真正说话, MN 命令/云端问题/10s无语音直接睡
            if (!_wifi->isConnected())
                Serial.println("[CONV] Offline: local-cmd window only");
            _enterWait(true);
            // v1s: 唤醒后重置噪声基底 — 提示音余韵会把 nf 拉高 (如 344),
            //   动态阈值 nf×3=1032 会把轻声说话滤成噪声 → 吞话。重置后从 0 开始学。
            _mic->resetNoiseFloor();
            // v1x: 唤醒应答改短提示音 (原"主人您好"1.3s语音 + 1.5s声反馈门控 = 2.8s
            //   吞掉唤醒后第一句话 → "唤醒不灵"。短音 100ms + 门控 = 1.6s, 释放对话窗口)
            _spk->playTone(880, 100);          // 短"叮"声唤醒反馈
            return;
        }
    }

    switch (_state) {
    case CONV_IDLE:
        if (_canStart()) {                          // 唤醒词未就绪的降级 VAD
            _startRecording();
        } else {
            _updateFaceCheck();                     // P7a: 睡眠时周期性人脸识别
        }
        break;
    case CONV_WAIT:
        _updateWait();
        break;
    case CONV_RECORDING:
        _updateRecording();
        break;
    case CONV_STT:
    case CONV_LLM:
    case CONV_TTS:
        if (_state == CONV_STT) _doSTT();
        else if (_state == CONV_LLM) _doLLM();
        else _doTTS();
        break;
    case CONV_PLAYING:
        _doPlaying();
        break;
    }
}

// ── 开始录音 (聆听中) ──
void ConversationManager::_startRecording() {
    _thinkVPPlayed = false;                      // v1j: 新一轮对话, 思考提示待播
    _mic->startConversationRecording();
    ui.setListen();
    _silenceStart = 0;
    _vadPendingAt = 0;
    _enterState(CONV_RECORDING);
}

// ── 录音中: 静音停止 ──
void ConversationManager::_updateRecording() {
    uint32_t now = millis();
    uint32_t elapsed = now - _stateEnter;

    if (elapsed > CONV_MAX_RECORD_S * 1000) {
        Serial.println("[CONV] Max record time reached");
        _recSamples = _mic->stopConversationRecording(&_recBuf);
        if (_recSamples > MIC_SAMPLE_RATE / 4) _enterState(CONV_STT);
        else _cleanup();
        return;
    }

    if (_mic->getLevel() < 2) {
        if (_silenceStart == 0) _silenceStart = now;
        // v1c: 录音不含唤醒词 (聆听窗口确认后才录), 统一 1s 静音判停
        if (now - _silenceStart >= CONV_SILENCE_MS) {
            _recSamples = _mic->stopConversationRecording(&_recBuf);
            Serial.printf("[CONV] Recording: %u samples (%.1fs)\n",
                          _recSamples, _recSamples / (float)MIC_SAMPLE_RATE);
            if (elapsed < CONV_MIN_RECORD_MS || _recSamples < MIC_SAMPLE_RATE / 4) {
                Serial.println("[CONV] Too short, discarding");
                _cleanup();
            } else {
                _enterState(CONV_STT);
            }
        }
    } else {
        _silenceStart = 0;
    }
}

// ── P5 (v1k): STT 文本是否"只有唤醒词" ──
bool ConversationManager::_isWakeOnly(const String& text) {
    if (text.length() == 0) return false;
    if (text.indexOf("小智") < 0) return false;
    int content = 0;
    for (size_t i = 0; i < text.length(); i++) {
        char c = text[i];
        if ((uint8_t)c < 0x80) {
            if (c != ' ' && c != '?' && c != '!' && c != '.' && c != ','
                && c != '\xef') content++;
        } else {
            content++;
            i += ((uint8_t)c >= 0xF0) ? 3 : (((uint8_t)c >= 0xE0) ? 2 : 1);
        }
    }
    return content <= 6;
}

// ── STT ──
void ConversationManager::_doSTT() {
    ui.setThink();                               // 屏幕同步进"思考中"
    // v1j: 进入思考 → 语音提示 (唤醒后"我在思考" / 直接发问"我想想")
    //      阻塞播完再走 STT: STT 是同步 HTTP, 期间主循环停摆、speaker 无人喂 DMA, 不阻塞会卡音
    if (!_thinkVPPlayed) {
        _playVPBlocking(_waitListen ? VP_THINK_WAKE : VP_THINK_ASK);
        _thinkVPPlayed = true;
    }

    // v1k: STT 前检查 WiFi (SSL 连接失败有时是 WiFi 刚好断了一下)
    if (!_wifi || !_wifi->isConnected()) {
        Serial.println("[CONV] STT skipped: WiFi not connected");
        if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
        _recSamples = 0;
        _failWait();
        return;
    }
    _sttText = _stt.recognize(_recBuf, _recSamples);

    // v1y: STT 失败 → WiFi 断开重连 + 再试一次 (修"长时间空闲后 SSL 假死")
    //   errno 118 Host unreachable 常出现在设备睡眠/空闲几分钟后: WiFi 关联状态
    //   正常 (isConnected=true) 但 TCP 链路已死。断开重连刷新链路/DNS 再试。
    if (_sttText.length() == 0 && _wifi) {
        Serial.println("[CONV] STT fail -> WiFi.reconnect + retry once");
        WiFi.disconnect();
        delay(300);
        WiFi.reconnect();
        delay(2000);
        _sttText = _stt.recognize(_recBuf, _recSamples);
    }

    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;

    if (_sttText.length() == 0) {
        Serial.println("[CONV] STT failed");
        _failWait();
        return;
    }

    // 唤醒词独句 → 回聆听窗口 (罕见: 唤醒词被录进去了, 等真正说话)
    if (_wake && _wake->isReady() && _isWakeOnly(_sttText)) {
        Serial.printf("[CONV] Wake-only: \"%s\" -> listen window\n", _sttText.c_str());
        _sttText = "";
        _llmReply = "";
        _enterWait(true);
        return;
    }

    // P8d: STT 文本路由 (替代 MN 命令词 — 任意自然语言, 关键词命中即本地动作/真查询)
    //   命中 → 直接执行 (天气真数据/音乐/拍照/视觉/表情/睡觉), 不送 LLM 嘴炮
    //   动作内部负责状态转移与缓冲清理 (LOOK 需保留 _sttText 走视觉 LLM)
    if (_routeText(_sttText)) return;

    // P8e: 路由未命中 → 先走 /ask 联网问答 (B+C: web_search 兜底 + function calling)
    //   成功 → 约50字纯文本直接进 TTS, 不经本地裸 LLM
    //   失败 → 降级走裸 LLM (断网/网关不可用时的兜底)
    {
        String askReply;
        // P10: /ask 带长期记忆 (SD 卡里的"记住XXX"条目 → 云端注入 prompt)
        if (_ask.ask(_sttText, askReply, _memory.text())) {
            Serial.printf("[CONV] /ask OK -> TTS\n");
            _llmReply = askReply;
            // v1w: 对话成功 → 压入历史 (user问 + assistant答)
            _ask.pushHistory(_sttText, _llmReply);
            _enterState(CONV_TTS);
            return;
        }
        Serial.println("[CONV] /ask failed, fallback to local LLM");
    }

    _enterState(CONV_LLM);
}

// ── P4.5: 视觉触发词检测 ──
bool ConversationManager::_containsVisionTrigger(const String& text) {
    if (text.length() == 0) return false;
    String list = VISION_TRIGGER_WORDS;
    int start = 0;
    while (start < (int)list.length()) {
        int comma = list.indexOf(',', start);
        String word = (comma < 0) ? list.substring(start) : list.substring(start, comma);
        word.trim();
        if (word.length() > 0 && text.indexOf(word) >= 0) {
            Serial.printf("[CONV] Vision trigger: \"%s\" in \"%s\"\n",
                          word.c_str(), text.c_str());
            return true;
        }
        if (comma < 0) break;
        start = comma + 1;
    }
    return false;
}

// ── LLM (含视觉分支) ──
void ConversationManager::_doLLM() {
    ui.setThink();                               // 屏幕同步进"思考中"
    // v1j: 思考语音提示 — STT 路径已播(标志 true)则跳过; LOOK 命令直达(标志 false)在此播
    if (!_thinkVPPlayed) {
        _playVPBlocking(_waitListen ? VP_THINK_WAKE : VP_THINK_ASK);
        _thinkVPPlayed = true;
    }

    String reply;
    bool visionUsed = false;

    if (_containsVisionTrigger(_sttText) && _capture && _capture->cameraReady()) {
        Serial.println("[CONV] Vision mode: capturing photo...");
        uint8_t* jpeg = nullptr;
        size_t   len  = 0;
        if (_capture->captureJpeg(&jpeg, &len)) {
            reply = _vision.describe(jpeg, len, VISION_PROMPT_DEFAULT);
            free(jpeg);
            visionUsed = (reply.length() > 0);
        }
        if (!visionUsed) Serial.println("[CONV] Vision failed, falling back to text LLM");
    }

    if (!visionUsed) reply = _llm.chat(_sttText);

    if (reply.length() == 0) {
        Serial.println("[CONV] LLM failed");
        _failWait();
        return;
    }
    _llmReply = reply;
    _enterState(CONV_TTS);
}

// ── TTS 前 emoji 剥离 (4字节序列) ──
static void stripEmoji(String& s) {
    if (s.indexOf((char)0xF0) < 0) return;
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); ) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 0xF0 && c <= 0xF4 && i + 3 < s.length()) { i += 4; continue; }
        if (c == 0xEF && i + 2 < s.length() &&
            (uint8_t)s[i+1] == 0xB8 && (uint8_t)s[i+2] >= 0x80 && (uint8_t)s[i+2] <= 0x8F) {
            i += 3; continue;
        }
        out += (char)c; i++;
    }
    s = out;
}

// ── TTS ──
void ConversationManager::_doTTS() {
    ui.setThink();

    stripEmoji(_llmReply);
    bool ok = _tts.synthesize(_llmReply, &_ttsBuf, &_ttsSamples);

    if (!ok || !_ttsBuf || _ttsSamples == 0) {
        Serial.println("[CONV] TTS failed");
        _failWait();
        return;
    }
    Serial.printf("[CONV] TTS OK: %u samples (%.1fs)\n", _ttsSamples, _ttsSamples / (float)MIC_SAMPLE_RATE);
    _enterState(CONV_PLAYING);
}

// ── 播放 ──
void ConversationManager::_doPlaying() {
    if (_ttsBuf) {
        ui.setSpeak();
        _spk->playPCM(_ttsBuf, _ttsSamples);
        _ttsBuf = nullptr;
        _spk->update();
        Serial.println("[CONV] Playback started");
        return;
    }
    _spk->update();
    if (!_spk->isPlaying() && millis() - _stateEnter > 500) {
        Serial.println("[CONV] Playback finished");
        _enterWait();          // P6: 播完 → 等待倒数
    }
}

// ── P6: 进等待窗口 ──
//   listenMode=true  → 聆听窗口 (唤醒后空窗): 显示聆听脸, 不倒数; 说话→云端录音; 10s 无语音→睡眠
//   listenMode=false → 倒数窗口 (播完/失败/命令后): 大数字倒数
void ConversationManager::_enterWait(bool listenMode) {
    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;
    _waitDeadline = millis() + CONV_WAIT_WINDOW_MS;
    _waitShownSec = -1;
    _vadPendingAt = 0;
    _waitListen = listenMode;
    _enterState(CONV_WAIT);
    if (listenMode) ui.setListen();
}

// ── P6: 等待窗口逻辑 ──
//   优先级: 再唤醒(顶部公共块) > VAD 云端 > 超时回睡
//   P8d: MN 命令词已退役 — 说话一律 VAD 录音 → STT → 文本路由 (任意自然语言)
void ConversationManager::_updateWait() {
    uint32_t now = millis();

    // 1. 倒数显示 (仅倒数模式; 聆听模式保持聆听脸) + 超时
    if (!_waitListen && _wifi->isConnected()) {
        int remain = (int)((_waitDeadline - now) / 1000) + 1;
        if (remain < 0) remain = 0;
        if (remain > CONV_WAIT_WINDOW_MS / 1000) remain = CONV_WAIT_WINDOW_MS / 1000;
        if (remain != _waitShownSec) {
            _waitShownSec = remain;
            ui.setWait(remain);                  // 值变化才刷屏
        }
    }
    if (now >= _waitDeadline) {
        Serial.println(_waitListen ? "[CONV] Listen timeout -> sleep"
                                   : "[CONV] Wait timeout -> sleep");
        _cleanup();
        return;
    }
    // 2. VAD → 云端 (断网跳过)
    if (!_wifi->isConnected()) { _vadPendingAt = 0; return; }
    if (now < _cooldownUntil || _spk->isPlaying() ||
        _spk->feedbackGateRemainingMs() > 0) {
        _vadPendingAt = 0;
        return;
    }
    // 聆听模式: 唤醒词尾音免疫 — 进窗后头 700ms 的语音不置 pending
    uint32_t tailGuard = _waitListen ? CONV_LISTEN_TAIL_GUARD_MS : 0;
    if (_mic->getLevel() >= 2 && now - _stateEnter >= tailGuard) {
        if (_vadPendingAt == 0) _vadPendingAt = now;
        _vadLastSpeech = now;
        // 连续式: 持续说满 600ms → 长句立即录 (2s 预滚兜开头)
        if (now - _vadPendingAt >= CONV_VAD_TRIG_MS) {
            Serial.println("[CONV] VAD trigger in WAIT (speech ongoing)");
            _startRecording();
        }
    } else if (_vadPendingAt != 0) {
        // v1c 修复漏听: 旧逻辑要求"连续 lv>=2 满 1s", 短句("几点了")说完能量回落
        // pending 即被清零 → 永远触发不了。改为结束式确认:
        //   说过话(哪怕断续) 且静音 600ms → 录 (P8d: 不再等 MN 结算, 直接录)
        if (now - _vadLastSpeech >= CONV_VAD_TRIG_MS) {
            Serial.println("[CONV] VAD trigger in WAIT (speech ended)");
            _startRecording();                   // 2s 预滚兜回整句
        } else if (now - _vadPendingAt >= 2500) {
            _vadPendingAt = 0;                   // 久无下文, 放弃
        }
    }
}

// ── P6: 云端失败 → 提示 + 等待窗口 (可立即重说) ──
void ConversationManager::_failWait() {
    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;
    _ttsBuf = nullptr;
    _sttText = "";
    _llmReply = "";
    _spk->playTone(330, 250);                    // 低音提示
    ui.showMessage("网络不好，请再说一遍", 2500);
    _cooldownUntil = millis() + CONV_COOLDOWN_MS;
    _enterWait();
}

// ── P8d: 本地动作执行 (触发源: STT 文本路由; v1v 起仅本地"肢体动作", 查询类已交 /ask) ──
bool ConversationManager::_handleLocalAction(RouteAction act) {
    switch (act) {
    case ROUTE_PHOTO: {
        _spk->playTone(659, 90);
        ui.showMessage("拍照中", 1200);
        bool ok = false;
        String path;
        if (_capture && _capture->cameraReady()) {
            ok = _capture->takePhotoAndSave(&path);
        }
        Serial.printf("[CONV] Route PHOTO: %s (%s)\n", ok ? "saved" : "FAILED",
                      ok ? path.c_str() : "no camera/SD");
        ui.showMessage(ok ? "已存卡" : "没存上", 2000);
        _cmdCleanup(true);                       // 回等待
        return true;
    }
    case ROUTE_LOOK: {
        _spk->playTone(659, 90);
        if (_capture && _capture->cameraReady() && _wifi->isConnected()) {
            if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
            _recSamples = 0;
            _sttText = "看看";
            _thinkVPPlayed = false;              // v1j: LOOK 直达 LLM, 思考提示在此播
            _enterState(CONV_LLM);               // 思考(视觉) → 播报 → 等待
            return true;
        }
        _spk->playTone(440, 120);                // 摄像头/网络不可用
        _cmdCleanup(true);
        return true;
    }
    case ROUTE_EMOTION: {
        // P6 彩蛋: 吐舌/眨眼/微笑 轮换, 5s 后回睡眠脸
        _spk->playTone(659, 90);
        int idx = random(3);
        ui.setEaster(idx);
        Serial.printf("[CONV] Route EMOTION -> easter %d\n", idx);
        _cmdCleanup(false);                      // 底层=睡眠, 彩蛋 5s 后自动回睡眠脸
        return true;
    }
    case ROUTE_SLEEP: {
        _spk->playTone(392, 120);
        ui.setSleep();
        ui.showMessage("好梦", 1500);
        _playVP(VP_SLEEP);                   // v1h: 睡觉命令也告别
        Serial.println("[CONV] Route SLEEP");
        _cmdCleanup(false);
        return true;
    }
    // ── P7b: 钢琴曲 — 云端下载 PCM 播放 (方案二), 失败降级本地旋律 ──
    case ROUTE_MUSIC:
        _spk->playTone(659, 90);
        Serial.println("[CONV] Route MUSIC -> cloud music");
        _playCloudMusic();
        return true;
    // ── P10: 记住XXX → 存 SD 长期记忆 ──
    case ROUTE_REMEMBER: {
        _spk->playTone(880, 90);                 // 高音反馈
        if (_memory.available() && _pendingMemory.length() > 0) {
            _memory.add(_pendingMemory);
            ui.showMessage("记住了", 1500);
            Serial.printf("[CONV] Memory saved: \"%s\"\n", _pendingMemory.c_str());
        } else {
            ui.showMessage("没记住", 1500);      // 无 SD 卡
            Serial.println("[CONV] Memory save FAILED (SD unavailable)");
        }
        _pendingMemory = "";
        _cmdCleanup(true);                       // 回等待
        return true;
    }
    default:
        return false;
    }
}

// ── P8d→v1v: STT 文本路由入口 ──
//   唤醒 → VAD 录音 → STT → 本地动作关键词 → 本地执行 (不送 LLM);
//   未命中 → 返回 false, _doSTT 放行 /ask 云函数 (查询类全交给 LLM+function calling)
bool ConversationManager::_routeText(const String& text) {
    RouteAction act = _parseRoute(text);
    if (act == ROUTE_NONE) return false;
    return _handleLocalAction(act);
}

// ── P8d→v1v: 关键词 → 本地动作 (天气/查询类已删, 全走 /ask 云函数) ──
//   优先级: 音乐 > 看看(视觉) > 拍照 > 表情 > 睡觉
ConversationManager::RouteAction ConversationManager::_parseRoute(
        const String& text) {
    if (text.length() == 0) return ROUTE_NONE;

    // ── 音乐 (播放/唱歌) ──
    static const char* const kMusicKw[] = {"音乐", "首歌", "点歌", "钢琴", "弹", "唱"};
    for (const char* kw : kMusicKw) {
        if (text.indexOf(kw) >= 0) {
            Serial.printf("[ROUTE] MUSIC (\"%s\")\n", text.c_str());
            return ROUTE_MUSIC;
        }
    }

    // ── 看看 → 视觉对话 (在拍照前: "看看照片"应是看图不是拍照) ──
    static const char* const kLookKw[] = {"看看", "看下", "看一下", "瞅瞅"};
    for (const char* kw : kLookKw) {
        if (text.indexOf(kw) >= 0) {
            Serial.printf("[ROUTE] LOOK (\"%s\")\n", text.c_str());
            return ROUTE_LOOK;
        }
    }

    // ── 拍照 ──
    static const char* const kPhotoKw[] = {"拍照", "拍个照", "拍张", "照相", "咔嚓"};
    for (const char* kw : kPhotoKw) {
        if (text.indexOf(kw) >= 0) {
            Serial.printf("[ROUTE] PHOTO (\"%s\")\n", text.c_str());
            return ROUTE_PHOTO;
        }
    }

    // ── 表情彩蛋 ──
    static const char* const kEmoKw[] = {"表情", "变个脸", "换脸", "换个表情"};
    for (const char* kw : kEmoKw) {
        if (text.indexOf(kw) >= 0) {
            Serial.printf("[ROUTE] EMOTION (\"%s\")\n", text.c_str());
            return ROUTE_EMOTION;
        }
    }

    // ── P10: 长期记忆 (记住/我叫/我喜欢 → 提取条目存 SD) ──
    //   优先级在表情之后、睡觉之前 ("记住我说晚安"仍以记住优先)
    {
        static const char* const kRemKw[] = {"记住", "记着", "别忘了"};
        for (const char* kw : kRemKw) {
            int idx = text.indexOf(kw);
            if (idx >= 0) {
                String c = text.substring(idx + strlen(kw));
                c.trim();
                if (c.length() >= 2 && !c.startsWith("了")) {   // "记住我说晚安"→"我说晚安"仍可存; "记住了"太短跳过
                    _pendingMemory = c;
                    Serial.printf("[ROUTE] REMEMBER (\"%s\")\n", c.c_str());
                    return ROUTE_REMEMBER;
                }
            }
        }
        static const char* const kFavKw[] = {"我喜欢", "我讨厌", "我不喜欢"};
        for (const char* kw : kFavKw) {
            int idx = text.indexOf(kw);
            if (idx >= 0) {
                String c = text.substring(idx + strlen(kw));
                c.trim();
                if (c.length() >= 2 && c != "你") {             // "我喜欢你"不存(空泛), "我喜欢听歌"→存
                    _pendingMemory = "用户" + String(kw) + c;
                    Serial.printf("[ROUTE] REMEMBER (\"%s\")\n", _pendingMemory.c_str());
                    return ROUTE_REMEMBER;
                }
            }
        }
        int nameIdx = text.indexOf("我叫");
        if (nameIdx >= 0) {
            // ⚠️ 2026-08-20 真机bug: substring(nameIdx+2) 是字符数不是字节数!
            //   "我叫"=6字节, +2 切在"叫"字中间 → 乱码 "用户叫�叫什么名字?"
            //   必须用 strlen("我叫")=6 字节偏移
            String c = text.substring(nameIdx + (int)strlen("我叫"));
            c.trim();
            // 问句过滤: "我叫什么名字?" 是查询不是陈述, 放行 /ask (云端带记忆回答)
            if (c.length() >= 2 && !c.startsWith("了")
                && c.indexOf("什么") < 0 && c.indexOf("？") < 0 && c.indexOf("?") < 0) {
                _pendingMemory = "用户叫" + c;
                Serial.printf("[ROUTE] REMEMBER (\"%s\")\n", _pendingMemory.c_str());
                return ROUTE_REMEMBER;
            }
        }
    }

    // ── 睡觉 ──
    // v1z: 增加"退下/停止/停下" (双字词可包含匹配); 单字"停"全等特判 (防"停车场/停机"误伤)
    static const char* const kSleepKw[] = {"睡觉", "晚安", "我睡了", "要睡了", "睡吧",
                                           "退下", "停止", "停下"};
    for (const char* kw : kSleepKw) {
        if (text.indexOf(kw) >= 0) {
            Serial.printf("[ROUTE] SLEEP (\"%s\")\n", text.c_str());
            return ROUTE_SLEEP;
        }
    }
    if (text == "停" || text == "停了" || text == "停吧") {
        Serial.printf("[ROUTE] SLEEP (\"%s\" exact)\n", text.c_str());
        return ROUTE_SLEEP;
    }

    return ROUTE_NONE;
}

// ── P7b: 播放简易钢琴旋律 (小星星, 非阻塞, 在 update() 里逐音驱动) ──
void ConversationManager::_playPianoMelody() {
    // 简谱: 小星星前两句 C C G G A A G - F F E E D D C -
    static const uint16_t kPiano[] = {
        523, 523, 784, 784, 880, 880, 784,
        698, 698, 659, 659, 587, 587, 523,
    };
    _melodyCount = sizeof(kPiano) / sizeof(kPiano[0]);
    for (uint8_t i = 0; i < _melodyCount; i++) _melodyNotes[i] = kPiano[i];
    _melodyActive = true;
    _melodyIdx = 0;
    _melodyNextAt = 0;
    Serial.printf("[CONV] Melody start: %d notes\n", _melodyCount);
}

// ── v1o: 阻塞播报任意 TTS 文本 (喂 DMA 播完再返回, 用于音乐提示语) ──
void ConversationManager::_playBlockingTTS(const String& text) {
    uint32_t t0 = millis();
    while (_spk->isPlaying() && millis() - t0 < 2000) {
        _spk->update();
        delay(5);
    }
    int16_t* buf = nullptr; uint32_t n = 0;
    if (!_tts.synthesize(text, &buf, &n) || !buf || n == 0) {
        Serial.printf("[CONV] BlockingTTS synth fail: \"%s\"\n", text.c_str());
        return;
    }
    _spk->playPCM(buf, n);
    t0 = millis();
    while (_spk->isPlaying() && millis() - t0 < 6000) {
        _spk->update();
        delay(5);
    }
}

// ── P7b: 云端音乐播放 (方案二: 下载 PCM → playPCM 非阻塞) ──
//   命令命中 → 提示音 → v1o: 阻塞播报"好嘞，给你弹一首" → 下载到 PSRAM
//   → playPCM 挂给 speaker (非阻塞) → 进 CONV_PLAYING 状态
//   播完由 _doPlaying() 回等待 (与 TTS 同路径)
//   断网/下载失败 → 降级本地 melody + 屏幕提示
void ConversationManager::_playCloudMusic() {
    if (!_wifi || !_wifi->isConnected()) {
        Serial.println("[CONV] Cloud music offline -> local melody fallback");
        ui.showMessage("请再说一遍", 3000);   // 字库: 断网反馈
        _playPianoMelody();
        _cmdCleanup(true);
        return;
    }

    // 清理残留录音/文本 (命令穿梭可能带脏数据)
    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;
    _sttText = "";
    _llmReply = "";

    ui.showMessage("聆听", 2500);            // 字库: 下载中
    Serial.println("[CONV] Fetching cloud music (blocking download in PLAYING state)");

    // v1o: 下载前先播提示语, 让用户知道正在准备
    _playBlockingTTS("好嘞，给你弹一首");

    // 轮换点歌: 小星星 → 致爱丽丝 → 天空之城 → 循环 (再弹一首有新鲜感)
    static const char* kPlaylist[] = { "小星星", "致爱丽丝", "天空之城" };
    _musicQueueLen = sizeof(kPlaylist) / sizeof(kPlaylist[0]);
    for (uint8_t i = 0; i < _musicQueueLen; i++) _musicQueue[i] = kPlaylist[i];
    if (_musicQueueIdx >= _musicQueueLen) _musicQueueIdx = 0;
    String song = _musicQueue[_musicQueueIdx];
    _musicQueueIdx++;

    // 下载到 PSRAM (复用 _ttsBuf/_ttsSamples 传输变量, 播放阶段由 _doPlaying 消费)
    _ttsBuf = nullptr; _ttsSamples = 0;
    bool ok = _music.fetchSong(song, &_ttsBuf, &_ttsSamples);
    if (!ok || !_ttsBuf || _ttsSamples == 0) {
        Serial.println("[CONV] Cloud music FAIL -> local melody fallback");
        ui.showMessage("请再说一遍", 3000);   // 字库: 取歌失败
        if (_ttsBuf) { heap_caps_free(_ttsBuf); _ttsBuf = nullptr; }
        _playPianoMelody();
        _cmdCleanup(true);
        return;
    }

    ui.showMessage("好听", 5000);            // 字库: 播放中 (24x24 点阵只有子集字)
    _enterState(CONV_PLAYING);               // 下载完成 → 播报状态 (非阻塞播放)
}

// ── P6: 命令后清理 ──
//   toWait=true  → 回等待窗口 (倒数)
//   toWait=false → 回睡眠
void ConversationManager::_cmdCleanup(bool toWait) {
    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;
    _sttText = "";
    _llmReply = "";
    _cooldownUntil = millis() + CONV_COOLDOWN_MS;
    if (toWait) {
        _enterWait();
    } else {
        ui.setSleep();
        _enterState(CONV_IDLE);
    }
}

// ── P6: 回睡眠 (等待超时/录音太短/睡觉) ──
void ConversationManager::_cleanup() {
    if (_recBuf) { heap_caps_free(_recBuf); _recBuf = nullptr; }
    _recSamples = 0;
    _ttsBuf = nullptr;
    _sttText = "";
    _llmReply = "";
    _ask.clearHistory();                         // v1w: 睡眠后清空对话历史
    ui.setSleep();
    _playVP(VP_SLEEP);                           // v1h: 睡眠告别 "我先睡了"
    _cooldownUntil = millis() + CONV_COOLDOWN_MS;
    _enterState(CONV_IDLE);
}

// ── v1h: 语音提示播放 (懒合成 + PSRAM 缓存 + 拷贝播放) ──
//   首次调用: TTS 合成 → 存 PSRAM (常驻, 不释放)
//   后续调用: 拷贝缓存到临时 buf → playPCM (playPCM 会接管并 free 临时 buf)
//   合成失败/离线: 降级为短提示音
void ConversationManager::_playVP(uint8_t id) {
    if (id >= VP_COUNT || !_spk || _spk->isPlaying()) return;
    if (!_vpCache[id]) {
        if (!_wifi || !_wifi->isConnected()) { _spk->playTone(659, 100); return; }
        if (_vpTried[id]) { _spk->playTone(659, 100); return; }   // 合成失败过, 不重试
        _vpTried[id] = true;
        int16_t* buf = nullptr;
        uint32_t n = 0;
        if (!_tts.synthesize(String(VP_PHRASES[id]), &buf, &n) || !buf || n == 0) {
            Serial.printf("[VP] synth fail #%d \"%s\"\n", id, VP_PHRASES[id]);
            _spk->playTone(659, 100);
            return;
        }
        _vpCache[id] = buf;
        _vpLen[id]   = n;
        Serial.printf("[VP] cached #%d \"%s\": %u samples (%.1fs)\n",
                      id, VP_PHRASES[id], (unsigned)n, n / 16000.0f);
    }
    // playPCM 接管 buf 所有权并 free → 拷贝缓存再传 (缓存常驻)
    size_t bytes = _vpLen[id] * sizeof(int16_t);
    int16_t* tmp = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (tmp) {
        memcpy(tmp, _vpCache[id], bytes);
        _spk->playPCM(tmp, _vpLen[id]);
    }
}

// ── v1j: 阻塞播放语音提示 (思考提示专用) ──
//   STT/LLM 是同步 HTTP 请求, 期间主循环停摆 → speaker.update() 无人喂 DMA,
//   直接 playPCM 挂上不喂会"卡音"(播 256 样本即停)。此处主动循环喂 DMA 播完再返回。
//   先等旧音播完 (LOOK 命令的 90ms 反馈"叮"可能还在响, 否则 _playVP 的 isPlaying 检查会跳过)
void ConversationManager::_playVPBlocking(uint8_t id) {
    uint32_t t0 = millis();
    while (_spk->isPlaying() && millis() - t0 < 2000) {
        _spk->update();
        delay(5);
    }
    _playVP(id);
    t0 = millis();
    while (_spk->isPlaying() && millis() - t0 < 4000) {
        _spk->update();
        delay(5);
    }
}

// ── P7a: 睡眠时人脸识别 → 欢迎播报 ──
//   条件: 已注册>=1人 / 已连网(TTS) / 节流800ms / 冷却 / 喇叭空闲 / 摄像头空闲
void ConversationManager::_updateFaceCheck() {
    uint32_t now = millis();
    if (_wakeOnly) return;                                // 隔离调试模式不打扰
    if (!_face || !_face->isReady()) return;
    if (_face->enrolledCount() == 0) return;              // 没人注册 → 不白费电
    if (now < _faceNextAt) return;
    _faceNextAt = now + CONV_FACE_PERIOD_MS;
    if (now < _cooldownUntil || _spk->isPlaying()) return;
    if (!_wifi->isConnected()) return;                    // 欢迎播报需云端 TTS
    if (_capture && _capture->isBusy()) return;           // 视频流/拍照占用

    uint8_t* rgb = nullptr;
    int w = 0, h = 0;
    if (!_capture || !_capture->captureRgb888(&rgb, &w, &h)) return;

    int id = -1; float score = 0;
    String name;
    int res = _face->process(rgb, w, h, id, name, score);
    heap_caps_free(rgb);

    // 无人/陌生人 → 遗忘 (人离开后回来才会再次欢迎)
    if (res != 0 || id < 0) { _faceLastId = -1; return; }

    // 同人持续在场不重复欢迎; 冷却期 (防转头丢帧 1 帧又回来的连欢迎)
    if (id == _faceLastId) return;
    if (now < _faceGreetCooldown) return;
    _faceLastId = id;
    _faceGreetCooldown = now + CONV_FACE_GREET_MS;

    Serial.printf("[FACE] Recognize id=%d name=\"%s\" score=%.3f -> greet\n",
                  id, name.c_str(), score);
    _greetFace(name);
}

// ── P7a: 欢迎播报 — 屏幕 "欢迎回来" + 语音 "欢迎回来，<name>" ──
void ConversationManager::_greetFace(const String& name) {
    ui.showMessage("欢迎回来", 3000);                  // 屏幕 (字库子集已含这4字)
    if (_spk->isPlaying()) return;

    String text = "欢迎回来";
    if (name.length() > 0) { text += "，"; text += name; }
    int16_t* buf = nullptr; uint32_t n = 0;
    if (!_tts.synthesize(text, &buf, &n) || !buf || n == 0) {
        Serial.printf("[FACE] greet TTS fail \"%s\" -> tone\n", text.c_str());
        _spk->playTone(659, 120);
        return;
    }
    _spk->playPCM(buf, n);
    uint32_t t0 = millis();
    while (_spk->isPlaying() && millis() - t0 < 6000) {   // 阻塞播完 (主循环同步)
        _spk->update();
        delay(5);
    }
    Serial.printf("[FACE] Greeted: \"%s\"\n", text.c_str());
}

// ── P7a: 串口命令入口 (REG= / FACELIST / FACEDEL / FACECLR / FACETH / FACETEST) ──
int ConversationManager::enrollFace(const String& name) {
    if (!_face || !_face->isReady()) {
        Serial.println("[FACE] module not ready");
        return -1;
    }
    if (!_capture || !_capture->cameraReady()) {
        Serial.println("[FACE] camera not ready");
        return -1;
    }
    uint8_t* rgb = nullptr; int w = 0, h = 0;
    if (!_capture->captureRgb888(&rgb, &w, &h)) {
        Serial.println("[FACE] capture failed");
        return -1;
    }
    int id = _face->enroll(rgb, w, h, name.c_str());
    heap_caps_free(rgb);
    if (id >= 0) {
        Serial.printf("[FACE] ENROLLED id=%d name=\"%s\" (total %d)\n",
                      id, name.c_str(), _face->enrolledCount());
        ui.showMessage("已录入", 2000);
    } else {
        Serial.printf("[FACE] enroll failed (code %d: -1 model, -2 no face, -3 low quality)\n", id);
        ui.showMessage("没录上", 2000);
    }
    return id;
}

void ConversationManager::faceList() {
    if (!_face || !_face->isReady()) { Serial.println("[FACE] not ready"); return; }
    _face->printIds();
}

void ConversationManager::faceDelete() {
    if (!_face || !_face->isReady()) { Serial.println("[FACE] not ready"); return; }
    _face->deleteLast(true);
}

void ConversationManager::faceClear() {
    if (!_face || !_face->isReady()) { Serial.println("[FACE] not ready"); return; }
    _face->clearAll(true);
}

void ConversationManager::faceSetThreshold(float t) {
    if (!_face || !_face->isReady()) { Serial.println("[FACE] not ready"); return; }
    _face->setThreshold(t);
    Serial.printf("[FACE] threshold -> %.2f\n", t);
}

void ConversationManager::faceTest() {
    if (!_face || !_face->isReady()) { Serial.println("[FACE] not ready"); return; }
    if (!_capture || !_capture->cameraReady()) { Serial.println("[FACE] camera not ready"); return; }
    uint8_t* rgb = nullptr; int w = 0, h = 0;
    if (!_capture->captureRgb888(&rgb, &w, &h)) { Serial.println("[FACE] capture failed"); return; }
    int id = -1; float score = 0; String name;
    int res = _face->process(rgb, w, h, id, name, score);
    heap_caps_free(rgb);
    switch (res) {
        case 0:  Serial.printf("[FACE-TEST] KNOWN id=%d name=\"%s\" score=%.3f\n",
                               id, name.c_str(), score); break;
        case 1:  Serial.println("[FACE-TEST] no face"); break;
        case 2:  Serial.printf("[FACE-TEST] stranger score=%.3f (enrolled=%d)\n",
                               score, _face->enrolledCount()); break;
        default: Serial.println("[FACE-TEST] module error"); break;
    }
}

// ── P6: BOOT 单击 = 手动唤醒 (调试入口, 免喊唤醒词) ──
void ConversationManager::manualWake() {
    if (_state != CONV_IDLE && _state != CONV_WAIT) return;
    Serial.println("[CONV] Manual wake (BOOT)");
    _enterWait(true);                            // v1c: 与语音唤醒同路径 (聆听窗口)
    _mic->resetNoiseFloor();                     // v1s: 同语音唤醒, 重置噪声基底
    _spk->playTone(880, 100);                    // v1x: 同语音唤醒, 短"叮"声 (替代"主人您好")
}
