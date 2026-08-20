# 小智桌面伴侣 · 路线图

> 2026-08-20 更新（P10 长期记忆 MVP + 播报打断方案A）
> 当前固件: P8f-v1z

## 已完成 ✅

| 阶段 | 内容 | 状态 |
|---|---|---|
| P1 | 工程骨架 / 构建链 / 引脚定义 / 摄像头 PID 检测 | ✅ |
| P2 | 外设连通 (LED/ST7789/MAX98357A/INMP441/SD/OV3660) | ✅ |
| P3 | 状态机联动 + Energy+ZCR 双特征 VAD | ✅ |
| P4 | 云端对话 STT→LLM→TTS 全链路 | ✅ |
| P4.1 | 唤醒词 "你好小智" (WakeNet9, 5/5 零误报) | ✅ |
| P4.5 | 拍照存SD / 视觉对话 / MJPEG流 | ✅ |
| P5 | 本地命令词 MultiNet | ❌ 已退役 (P8d 改文本路由) |
| P6 | 双层中文显示 (5态+倒数) | ✅ |
| P7a | 人脸识别 3人注册 + 欢迎播报 | ✅ |
| P7b | 云端迷你音乐 (HTTP PCM) | ✅ |
| P8a | 真查询天气 (知识网关 + 和风天气) | ✅ |
| P8d | **架构切换: STT 文本关键词路由替代命令词** | ✅ |
| P8d | VAD 吞话修复 (SPEECH_MULT 1.8 + resetNoiseFloor + tailGuard 300ms) | ✅ |
| **P8e** | **B+C 合一: LLM 联网问答 (web_search 兜底 + function calling)** | **✅ 编译通过** |

## 当前主线: P8 真查询知识服务

**核心优势**：小智优于传统对话机器人——天气/新闻/百科等真实数据直接播报，不做 LLM 嘴炮。

**P8e 架构 (B+C 合一)**：
- STT 关键词路由没命中 → POST /ask → GLM-4-Flash + `web_search`(联网兜底) + `get_weather`(function calling)
- LLM 联网搜或者调精确 API → ≤50 字纯文本 → TTS 播报
- 查火车、查新闻、问百科……什么都能答
- 新增查询类型只改 Python 网关加 tool 定义，**不烧固件**

| 阶段 | 内容 | 状态 |
|---|---|---|
| P8a | 天气 (实时/预报/穿衣) | ✅ |
| P8d | STT 文本关键词路由 + VAD 吞话修复 | ✅ |
| **P8e** | **B+C 合一: /ask 端点 (web_search + function calling, 火车/天气精确, 百科兜底)** | **✅** |
| **P8f** | **地名纠错 + 指代消解 + 景区映射 + 连续对话 history 管理 + WiFi重连** | **✅ 固件 v1y, 待烧录验证** |
| **P8b** | **新闻 / 百科 / 景点 关键词路由 + prompt 强化 (web_search 精准兜底)** | **✅ 已部署+实测** |
| **P8c** | **股票实时查询 (腾讯财经接口, get_stock function)** | **✅ 已部署+实测** |
| **P10** | **长期记忆 MVP: SD卡"记住XXX"→/ask memory字段→云端注入prompt** | **✅ 云端已部署+实测, 固件 v1z 已编译待烧录** |
| **v1z** | **睡眠命令词 (退下/停止/停下/停) + 播放中唤醒词打断 (方案A) + P10记忆路由 + MEMCLEAR/MEMLIST** | **✅ 编译通过, 待烧录验证** |

架构：ESP32 只发 HTTP → PC 知识服务网关 (knowledge_server.py) → 返回 ≤100 字纯文本 → TTS。新增查询类型只改 PC 端 Python，不重烧固件。

**问什么都能答 5 层体系 (P8e)**:
1. system prompt 强化: 有 function 优先调, 禁止推脱
2. tool description: 每个 function 写明"必须优先调用"
3. `_strip_evade_phrase`: 后处理拦截"请去XX查"等推脱语
4. 智能工具优先级: 文本命中关键词 → 只放对应 function (不放 web_search)
5. 输出 100 字 (与 ESP32 CONV_TTS_MAX_CHARS 对齐)

## 后续规划

- **P9 智能家居**：IR 红外 / 阿里云 MQTT 联天猫精灵门磁灯泡
- **P10 长期记忆增强**：记忆云端 LLM 提取（"记住XXX"不靠本地规则）、记忆管理（查看/删除）、按话题分组
- **P7 家庭陪伴增强**：云端消息中继 + 手机 APP (BLE 配网/留言/人脸注册)
- **播报打断方案 B**：VAD 人声直接打断（有自激风险，需真机调参）

## 资源占用 (P8f-v1z 编译后)

- RAM: 26.5% (86.8KB/320KB)
- Flash: 61.3% (3.22MB/5MB) — 剩 1.78MB
- PSRAM: 8MB 富余
- **ESP32 不是瓶颈，瓶颈在服务端 API 免费额度** (已做网关缓存)

