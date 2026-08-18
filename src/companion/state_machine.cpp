/**
 * @file state_machine.cpp
 * @brief 情绪枚举实现 — P3 事件驱动状态机已退役, 仅保留 emotionName
 */
#include "state_machine.h"

const char* emotionName(CompanionEmotion e) {
    switch (e) {
        case EMOTION_HAPPY:     return "HAPPY";
        case EMOTION_SAD:       return "SAD";
        case EMOTION_SURPRISED: return "SURPRISED";
        case EMOTION_THINKING:  return "THINKING";
        case EMOTION_SLEEPY:    return "SLEEPY";
        case EMOTION_NEUTRAL:
        default:                return "NEUTRAL";
    }
}
