/**
 * @file state_machine.h
 * @brief 情绪枚举 — P3 事件驱动状态机已退役, 仅保留 CompanionEmotion 供 LED 映射
 *
 * P6 起: 显示由 DisplayState 5 态接管, LED 由 conversation_manager._applyLed 映射,
 * 原 CompanionStateMachine (update/cycleEmotion/forceEmotion 等) 为死代码已删除。
 * 仅 CompanionEmotion 枚举 + emotionName 仍被 led_controller 情绪映射使用。
 */
#pragma once

enum CompanionEmotion {
    EMOTION_NEUTRAL = 0,
    EMOTION_HAPPY,
    EMOTION_SAD,
    EMOTION_SURPRISED,
    EMOTION_THINKING,
    EMOTION_SLEEPY,
};

/** 情绪名 (调试/串口/屏幕) */
const char* emotionName(CompanionEmotion e);
