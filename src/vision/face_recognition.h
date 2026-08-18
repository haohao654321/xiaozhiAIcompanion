/**
 * @file face_recognition.h
 * @brief P7a 人脸识别模块 — 本地检测 + 识别 (ESP-DL, 全程离线, 不花云端钱)
 *
 * 组件 (Arduino core 3.0.7 内置 esp-dl 预编译库, 自动链接):
 *   - 检测: HumanFaceDetectMSR01 (MTMN, ~14ms/帧 @320x240, 阈值 0.3 已验证)
 *   - 识别: FaceRecognition112V1S8 (MobileFaceNet 112x112 S8, 权重 ~1.45MB)
 *           S8 量化: 精度略低于 S16 (LFW 98%+ vs 99%), 但 Flash 减半 — P7a 选 S8 给 P7b 留空间
 *   - 持久化: 人脸 ID 写入 flash "fr" 分区 (断电不丢; 3 人 × ~1KB)
 *
 * 用法:
 *   FaceRecognition face;            // main.cpp 全局
 *   face.begin();                    // setup 里调用 (摄像头就绪后; 惰性 new, 无构造时序风险)
 *   face.process(rgb, w, h, ...)     // 识别一帧
 *   face.enroll(rgb, w, h, "张三")    // 注册当前帧人脸
 *
 * 输入帧: 必须是 RGB888 连续缓冲 (fmt2rgb888 转换, PSRAM 分配), 模块不接管所有权。
 */
#pragma once

#include <Arduino.h>
#include "human_face_detect_msr01.hpp"
#include "face_recognition_112_v1_s8.hpp"

// flash 分区 label (与 partitions_custom.csv 的 fr 分区一致)
#define FACE_PARTITION_LABEL "fr"

class FaceRecognition {
public:
    /** 初始化: 惰性 new 检测器+识别器 (PSRAM 已就绪), 设置 flash 分区 + 加载已注册 ID */
    void begin();

    bool isReady() const { return _ready; }
    int  enrolledCount() const;              // 已注册人数

    /**
     * 检测 + 识别一帧 RGB888
     * @return 0=识别到注册成员 (outId>=0) | 1=无人脸 | 2=有人脸但未注册 | -1=未就绪
     * @note rgb 非 const: ESP-DL infer<T>() 要求可写指针 (接口语义上只读, 内部零拷贝不改数据)
     */
    int process(uint8_t* rgb, int w, int h,
                int& outId, String& outName, float& outScore);

    /**
     * 注册当前帧中置信度最高的一张脸 (名字可选, 空串 = 无名)
     * @return face id (>=0) | -1=模型失败 | -2=没检测到脸 | -3=脸太小/分数太低不建议注册
     */
    int enroll(uint8_t* rgb, int w, int h, const char* name);

    void setThreshold(float t);
    float getThreshold() const;

    /** 清空全部注册 (true=同步写 flash) */
    void clearAll(bool updateFlash = true);
    /** 删除最后一个注册 (true=同步写 flash) */
    void deleteLast(bool updateFlash = true);
    /** 打印当前注册列表 (串口) */
    void printIds();

private:
    // 惰性 new: begin() 里 PSRAM 就绪后再构造, 避免全局静态构造时序问题
    HumanFaceDetectMSR01*    _detector   = nullptr;
    FaceRecognition112V1S8*  _recognizer = nullptr;
    bool _ready = false;
};
