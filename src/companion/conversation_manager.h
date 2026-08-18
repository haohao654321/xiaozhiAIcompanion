/**
 * @file conversation_manager.h
 * @brief 对话编排器 — P4 核心 + P6 显示状态机接入
 *
 * 全链路: 唤醒词 → 录音 → STT → LLM → TTS → 播放 → 等待倒数
 *
 * P6 状态机 (显示 5 态联动):
 *   IDLE(睡眠) --唤醒--> WAIT-listen(聆听中10s, 不录音) --说话--> RECORDING --静音--> STT/LLM/TTS(思考中)
 *     --TTS--> PLAYING(播报中) --播完--> WAIT(等待倒数10s)
 *   WAIT-listen: 唤醒后空窗 (尾音免疫700ms); MN命令→本地 | VAD→云端录音 | 10s无语音→直接睡眠
 *   WAIT: 命令词→本地直达 | VAD→云端 | 再唤醒→聆听 | 超时→IDLE(睡眠)
 *   失败(任一云端环节): 提示"网络不好，请再说一遍" → WAIT
 *
 * v1c 修复:
 *   - 唤醒后不再立即录音(旧: 录到唤醒词→STT→闪"思考中"→白费云端调用), 改进聆听窗口等真正说话
 *   - 等待窗口 VAD 由"连续说话满1s"改为"连续600ms 或 说完静音600ms", 修复短句(如"几点了")漏听
 *
 * P6 变更:
 *   - 旧情绪体系 (forceEmotion/HAPPY/THINKING 变脸) 移除, 由 DisplayState 5 态接管
 *   - P3 FSM (安静30s/90s 犯困链) 退役: 常态直接是睡眠, 等待超时也回睡眠
 */
#pragma once
#include <Arduino.h>
#include "../audio_in/mic_manager.h"
#include "../audio_out/speaker_manager.h"
#include "../network/wifi_manager.h"
#include "../companion/state_machine.h"     // 仅 CompanionEmotion 枚举 (LED 用)
#include "../network/stt_client.h"
#include "../network/llm_client.h"
#include "../network/tts_client.h"
#include "../network/music_client.h"   // P7b: 云端迷你音乐 (HTTP 回传 PCM)
#include "../network/weather_client.h"  // P8a: 天气查询 (知识服务网关)
#include "../network/vision_client.h"     // P4.5: GLM-4V-Flash 视觉理解
#include "../camera/capture_manager.h"    // P4.5: 拍照取帧
#include "../audio_in/wake_word.h"        // P4-1: 离线唤醒词
#include "../vision/face_recognition.h"   // P7a: 人脸识别 (本地)

class ConversationManager {
public:
    void begin(MicManager* mic, SpeakerManager* spk,
               WiFiManagerCompanion* wifi,
               CaptureManager* capture, WakeWord* wake = nullptr,
               FaceRecognition* face = nullptr);

    /** 主循环驱动 (非阻塞, STT/LLM/TTS 阶段会阻塞) */
    void update();

    /** 是否正在对话中 (等待窗口不算, 允许唤醒打断) */
    bool isActive() const { return _state != CONV_IDLE && _state != CONV_WAIT; }

    /** P6: BOOT 单击 = 手动唤醒 (免喊唤醒词的调试入口) */
    void manualWake();

    // v1u: P4.1 隔离模式 — 屏蔽录音/云端/本地命令, 唤醒命中只闪灯+日志
    void setWakeOnly(bool on) { _wakeOnly = on; }
    bool wakeOnly() const { return _wakeOnly; }

    // ── P7a: 人脸注册/管理 (串口命令入口) ──
    int  enrollFace(const String& name);      // 取一帧注册当前人脸 → face id
    void faceList();                          // 打印注册列表
    void faceDelete();                        // 删除最后一个
    void faceClear();                         // 清空全部
    void faceSetThreshold(float t);           // 调识别阈值
    void faceTest();                          // 单帧识别调试

private:
    enum ConvState : uint8_t {
        CONV_IDLE,         // 睡眠, 等唤醒词
        CONV_WAIT,         // P6: 等待窗口 (倒数10s; MN 优先 + VAD 延迟1s)
        CONV_RECORDING,    // 录音中 (聆听中)
        CONV_STT,          // 语音识别 (思考中)
        CONV_LLM,          // LLM 对话 (思考中)
        CONV_TTS,          // 语音合成 (思考中)
        CONV_PLAYING,      // 播放回复 (播报中)
    };

    // 依赖
    MicManager*              _mic  = nullptr;
    SpeakerManager*          _spk  = nullptr;
    WiFiManagerCompanion*    _wifi = nullptr;
    CaptureManager*          _capture = nullptr;
    WakeWord*                _wake = nullptr;
    FaceRecognition*         _face = nullptr;   // P7a: 人脸识别 (nullptr=未启用)

    // 客户端
    STTClient  _stt;
    LLMClient  _llm;
    TTSClient  _tts;
    VisionClient _vision;
    MusicClient _music;   // P7b: 云端迷你音乐
    WeatherClient _weather;  // P8a: 天气查询 (知识服务网关)

    // 状态
    ConvState  _state = CONV_IDLE;
    uint32_t   _stateEnter = 0;
    uint32_t   _silenceStart = 0;
    uint32_t   _cooldownUntil = 0;

    // P6: 等待窗口
    uint32_t   _waitDeadline = 0;      // 等待截止
    int8_t     _waitShownSec = -1;     // 已绘制的倒数秒数
    bool       _waitListen = false;    // v1c: 聆听模式 (唤醒后空窗, 显示聆听脸, 不倒数)
    uint32_t   _vadPendingAt = 0;      // P6: VAD 说话确认起点
    uint32_t   _vadLastSpeech = 0;     // v1c: 最近一次 lv>=2 时刻 (结束式确认用)

    // v1u: P4.1 隔离模式
    bool       _wakeOnly = false;
    uint32_t   _isoHitAt = 0;

    // 录音数据
    int16_t*   _recBuf = nullptr;
    uint32_t   _recSamples = 0;

    String     _sttText;
    String     _llmReply;

    int16_t*   _ttsBuf = nullptr;
    uint32_t   _ttsSamples = 0;

    // 内部方法
    void _enterState(ConvState s);
    void _applyLed();                          // P6: 显示状态 → LED 情绪映射
    bool _canStart() const;
    void _startRecording();
    void _updateRecording();
    void _doSTT();
    void _doLLM();
    void _doTTS();
    void _doPlaying();
    void _enterWait(bool listenMode = false);  // P6: 进等待窗口 (listen=聆听模式: 唤醒后空窗)
    void _updateWait();                        // P6: 等待窗口逻辑 (MN优先/VAD双确认/超时)
    void _failWait();                          // P6: 云端失败 → 提示 → 等待
    void _cleanup();                           // 回睡眠 IDLE
    bool _containsVisionTrigger(const String& text);
    bool _isWakeOnly(const String& text);

    void _cmdCleanup(bool toWait);            // P6: 命令后回等待或睡眠

    // ── P8d: STT 文本路由 (替代 MN 命令词 — 用户说话随意, 固定短语命中不了) ──
    //   唤醒 → VAD 录音 → STT → 文本关键词路由 → 本地动作/真查询 / 放行 LLM
    enum RouteAction : uint8_t {
        ROUTE_NONE = 0,     // 无命中 → 放行 LLM
        ROUTE_WEATHER,      // 天气真查询 (城市/天数/穿衣 由解析参数决定)
        ROUTE_MUSIC,        // 云端音乐
        ROUTE_PHOTO,        // 拍照存 SD
        ROUTE_LOOK,         // 看看 → 视觉对话 (GLM-4V)
        ROUTE_EMOTION,      // 换个表情 (彩蛋)
        ROUTE_SLEEP,        // 睡觉
    };
    bool        _routeText(const String& text);                    // 关键词路由入口 (返回 true=已处理)
    RouteAction _parseRoute(const String& text, String& city,      // 关键词+参数解析
                            int& days, bool& clothes);
    bool        _handleLocalAction(RouteAction act, const String& city,
                                   int days, bool clothes);        // 执行路由动作

    // P7b: 钢琴旋律 (小星星, 非阻塞逐音驱动) — 云端音乐下载失败时的降级
    void        _playPianoMelody();
    bool        _melodyActive = false;
    uint8_t     _melodyIdx = 0;
    uint8_t     _melodyCount = 0;
    uint32_t    _melodyNextAt = 0;
    uint16_t    _melodyNotes[16] = {0};        // 旋律音符表 (最大16音)

    // P7b: 云端音乐播放 (方案二: 下载 PCM → playPCM 非阻塞)
    void        _playCloudMusic();             // P8d: 音乐路由 (文本关键词"音乐/歌/唱"命中) → 下载 → PLAYING
    void        _playBlockingTTS(const String& text); // v1o: 阻塞播报任意 TTS 文本 (用于音乐提示语)
    String      _musicQueue[3];                // 点歌队列 (轮换备选曲目)
    uint8_t     _musicQueueLen = 0;            // 队列长度
    uint8_t     _musicQueueIdx = 0;            // 当前播放索引

    // P7a: 睡眠时人脸识别 → 欢迎播报
    void _updateFaceCheck();                   // CONV_IDLE 里周期性跑 (节流/冷却内跳过)
    void _greetFace(const String& name);       // 动态 TTS "欢迎回来，<name>" + 屏幕
    uint32_t _faceNextAt = 0;                  // 下一帧检查时刻 (节流)
    uint32_t _faceGreetCooldown = 0;           // 欢迎冷却 (同人 N 秒内不重复)
    int      _faceLastId = -1;                 // 最近欢迎过的人 (id 变化才再欢迎)

    // v1h/v1j: 语音提示 (唤醒应答/睡眠/思考), 懒合成 + PSRAM 缓存
    // v1j: 思考提示分两种 — 唤醒后问问题("我在思考") / 等待窗直接发问("我想想")
    enum VoicePromptId : uint8_t {
        VP_GREETING,     // 主人您好 — 唤醒应答
        VP_SLEEP,        // 我先睡了 — 超时/睡觉
        VP_THINK_WAKE,   // 我在思考 — 唤醒后问问题进思考
        VP_THINK_ASK,    // 我想想   — 等待窗直接发问进思考
        VP_COUNT         // = 4
    };
    static const char* const VP_PHRASES[VP_COUNT];
    int16_t* _vpCache[VP_COUNT] = {nullptr, nullptr, nullptr, nullptr};  // PSRAM 缓存 (常驻)
    size_t   _vpLen[VP_COUNT]   = {0, 0, 0, 0};
    bool     _vpTried[VP_COUNT] = {false, false, false, false};          // 合成失败标记
    void _playVP(uint8_t id);                    // 懒合成+缓存; 播放时拷贝一份给 playPCM
    void _playVPBlocking(uint8_t id);            // v1j: 阻塞播完再返回 (思考提示用, STT/LLM 前喂 DMA)

    // v1j: 思考语音提示已播标志 (STT→LLM 只播一次; 每次录音/LOOK 命令重置)
    bool _thinkVPPlayed = false;
};
