/**
 * @file wifi_manager.h
 * @brief WiFi 连接管理器 — P4 实现 (多 AP 备选)
 *
 * 功能:
 *   - STA 模式连接, 多 AP 按顺序尝试 (credentials from system_config.h)
 *   - 阻塞连接 (单 AP 超时 WIFI_TIMEOUT_MS), 非 blocking 重连 (loop update)
 *   - 断线重连时自动轮换到下一个 AP (家里/公司两个热点都能用)
 *   - 关闭省电模式 (WiFi.setSleep(false)) 保证低延迟响应
 *   - 状态查询: isConnected / getIP / getRSSI
 *
 * P4 后续: 为 STT/LLM/TTS HTTP 请求提供网络基础
 */
#pragma once

#include <WiFi.h>

class WiFiManagerCompanion {
public:
    /// 依次尝试所有 AP (每个阻塞超时 WIFI_TIMEOUT_MS)
    /// 返回是否成功连接
    bool connect();

    /// 非阻塞: 每 5s 检查连接, 断线时 10s 后自动重连 (轮换 AP)
    void update();

    /// 当前是否已连接
    bool isConnected() const;

    /// 当前 IP 地址 (未连接返回 "0.0.0.0")
    String getIP() const;

    /// 信号强度 dBm (未连接返回 0)
    int8_t getRSSI() const;

    /// 当前连接的 SSID (未连接返回 "")
    String getSSID() const;

private:
    bool     _tryAP(int idx);          // 连接指定 AP (阻塞)
    bool     _connected = false;
    int      _curAP = 0;               // 当前/下次优先尝试的 AP 索引
    uint32_t _lastCheck = 0;           // 上次状态检查时间
    uint32_t _reconnectAt = 0;         // 下次重连时间
};
