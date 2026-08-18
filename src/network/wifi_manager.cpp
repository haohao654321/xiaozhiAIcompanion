/**
 * @file wifi_manager.cpp
 * @brief WiFi 连接管理器实现 — P4 (多 AP 备选 + 断线轮换重连)
 *
 * ESP32-S3 内置 WiFi (仅 2.4GHz), 无需额外引脚。
 * AP 列表在 system_config.h (WIFI1_SSID / WIFI2_SSID ...)。
 */
#include <Arduino.h>
#include <time.h>
#include "wifi_manager.h"
#include "../config/system_config.h"

// ── P6: NTP 对时 (中国 UTC+8), 幂等 — 连上/重连成功后调用 ──
// 时间同步后 LLM system prompt 可注入实时时间 ("几点了"类问题可答)
static void _startNtp() {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org");
    Serial.println("[NET] NTP time sync started (UTC+8)");
}

// AP 凭据表 (与 system_config.h 的 WIFIx_SSID/PASS 对应)
static const char* const kAP_SSID[] = { WIFI1_SSID, WIFI2_SSID };
static const char* const kAP_PASS[] = { WIFI1_PASS, WIFI2_PASS };
static const int kAPCount = sizeof(kAP_SSID) / sizeof(kAP_SSID[0]);

// ── 连接指定 AP (阻塞, 超时 WIFI_TIMEOUT_MS) ──
bool WiFiManagerCompanion::_tryAP(int idx) {
    Serial.printf("[NET] WiFi connecting to '%s' ...", kAP_SSID[idx]);
    Serial.println();

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);          // 关省电模式, 降低延迟 (AI 对话需要快速响应)
    WiFi.setAutoReconnect(true);   // SDK 级自动重连 (我们也有应用级重连兜底)
    WiFi.begin(kAP_SSID[idx], kAP_PASS[idx]);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        delay(200);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        _curAP = idx;
        _connected = true;
        Serial.printf("[NET] WiFi OK! AP: %s | IP: %s | RSSI: %d dBm\n",
                      kAP_SSID[idx], WiFi.localIP().toString().c_str(), WiFi.RSSI());
        _startNtp();
        return true;
    }
    Serial.printf("[NET] AP '%s' timeout (%ds)\n", kAP_SSID[idx], WIFI_TIMEOUT_MS / 1000);
    WiFi.disconnect();
    return false;
}

// ── 依次尝试所有 AP ──
bool WiFiManagerCompanion::connect() {
    for (int i = 0; i < kAPCount; i++) {
        int idx = (_curAP + i) % kAPCount;      // 从上次成功的 AP 优先开始
        if (_tryAP(idx)) return true;
    }
    _connected = false;
    Serial.println("[NET] All APs failed");
    Serial.println("[NET]   Companion will run offline (P3 emotion FSM still works)");
    return false;
}

void WiFiManagerCompanion::update() {
    uint32_t now = millis();

    // 每 5s 检查一次连接状态 (避免频繁 WiFi.status() 调用)
    if (now - _lastCheck < 5000) return;
    _lastCheck = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (!_connected) {
            _connected = true;
            Serial.printf("[NET] WiFi reconnected! AP: %s | IP: %s | RSSI: %d dBm\n",
                          WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
            _startNtp();
        }
    } else {
        if (_connected) {
            _connected = false;
            Serial.println("[NET] WiFi disconnected, will retry...");
        }
        // 10s 间隔重试, 每次轮换到下一个 AP (避免死磕连不上的热点)
        if (now >= _reconnectAt) {
            _curAP = (_curAP + 1) % kAPCount;   // 下一个 AP
            Serial.printf("[NET] WiFi retrying with '%s'...\n", kAP_SSID[_curAP]);
            WiFi.disconnect();
            WiFi.begin(kAP_SSID[_curAP], kAP_PASS[_curAP]);
            _reconnectAt = now + 10000;
        }
    }
}

bool WiFiManagerCompanion::isConnected() const {
    return _connected && WiFi.status() == WL_CONNECTED;
}

String WiFiManagerCompanion::getIP() const {
    if (isConnected()) return WiFi.localIP().toString();
    return "0.0.0.0";
}

int8_t WiFiManagerCompanion::getRSSI() const {
    if (isConnected()) return WiFi.RSSI();
    return 0;
}

String WiFiManagerCompanion::getSSID() const {
    if (isConnected()) return WiFi.SSID();
    return "";
}
