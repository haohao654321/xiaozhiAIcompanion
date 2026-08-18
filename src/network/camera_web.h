/**
 * @file camera_web.h
 * @brief 摄像头 Web 服务器 — MJPEG 实时视频流 + 拍照 (P4.5)
 *
 * 端点 (端口 80):
 *   GET /         → 网页 (内嵌 HTML, 浏览器直接看摄像头)
 *   GET /stream   → MJPEG 实时视频流 (multipart/x-mixed-replace)
 *   GET /capture  → 拍一张照 (返回 JPEG; SD 可用时同时存卡)
 *
 * 来源: 移植自旧项目 Goouuu-CAM-WebServer (已验证)
 * 简化: 去掉 WebSocket 控制/寄存器调试, 保留流+拍照核心
 */
#pragma once
#include <stdbool.h>
#include "../camera/capture_manager.h"

/**
 * 启动摄像头 Web 服务器
 * @param capture 拍照管理器 (提供取帧)
 * @return true=启动成功
 */
bool startCameraWeb(CaptureManager* capture);
