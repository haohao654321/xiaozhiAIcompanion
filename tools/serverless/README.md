# 小智知识服务网关 — 云函数部署 (C2)

用户选定 **C2 云函数** 部署方案，解决"PC 不开机/不在同一 WiFi"的痛点。

## 目录

```
tools/serverless/
  handler.py     # 云函数入口 (兼容腾讯云 SCF + 阿里云 FC)
  deploy.py      # 打包 + 部署辅助脚本
  README.md      # 本文件
```

## 快速开始

### 1. 打包

```bash
cd tools/serverless
python deploy.py --platform scf    # 腾讯云
# 或
python deploy.py --platform fc     # 阿里云
# 或
python deploy.py                   # 只打包, 不指定平台
```

输出: `xiaoZhi-serverless.zip` (~15KB，纯标准库)

### 2. 配置环境变量

从 `tools/.env` 自动读取，部署脚本会输出清单。需在云函数控制台手动配置:

| 变量名 | 说明 |
|---|---|
| `QWEATHER_HOST` | 和风天气新版 API Host (如 `my7fc4dwmj.re.qweatherapi.com`) |
| `QWEATHER_KEY` | 和风天气 API Key |
| `ZHIPU_API_KEY` | 智谱 GLM-4-Flash API Key |

⚠️ **凭据不上传代码**，只在云平台控制台配环境变量。

### 3. 创建云函数

#### 腾讯云 SCF
- 运行环境: Python 3.9
- 执行方法: `handler.handler`
- 内存: 128MB
- 超时: **30 秒** (LLM 调用需要)
- 触发器: API 网关 → HTTP → 路径 `/{path+}` → 方法 ANY

#### 阿里云 FC
- 运行环境: Python 3.9
- Handler: `handler.handler`
- 内存: 128MB
- 超时: **30 秒**
- 触发器: HTTP 触发器 → 不认证 → GET/POST

### 4. 修改 ESP32 固件

`src/config/system_config.h`:
```cpp
#define KNOWLEDGE_SERVER_URL  "https://你的云函数域名"   // 原来是 http://192.168.1.49:8000
```

## 本地调试

```bash
cd tools/serverless
python handler.py
# → 启动本地 http://localhost:8000
# 行为与 knowledge_server.py 完全一致
```

## 已知坑 (C2 云函数特有)

| 坑 | 说明 | 应对 |
|---|---|---|
| **冷启动 3-5s** | 容器回收后首次调用需重新拉起 | 家用频率低, 可接受; 或配定时触发保活 |
| **12306 反爬** | 云函数出口 IP 是数据中心段, 比家宽更容易被 12306 限制 | 已在代码中用 CookieJar + init 页面拿 cookie; 如被限需加代理池 |
| **.env 不可用** | 云函数无文件系统写权限 | 改环境变量配置 |
| **无状态** | 内存缓存(_WX_CACHE/_train_cache)冷启动丢失 | 只是性能问题, 不影响功能 |

## 端点

| 端点 | 方法 | 说明 |
|---|---|---|
| `/` | GET | 状态说明页 |
| `/list` | GET | 音乐列表 JSON |
| `/weather?city=西安&days=1&clothes=1` | GET | 天气查询 |
| `/music?q=小星星&format=wav` | GET | 音乐 (返回 base64) |
| `/ask` | POST | LLM 问答 `{"text":"用户问题"}` |

## 音乐端点说明

云函数响应必须是文本/JSON，不能直接返回二进制 PCM。所以 `/music` 返回 **base64 编码** 的音频数据。ESP32 端 `music_client` 需要相应改造（增加 base64 解码）。如果暂时不用音乐功能，可以忽略此端点。
