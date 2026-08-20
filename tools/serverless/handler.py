#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小智 · 知识服务网关 — Serverless 适配层 (C2 云函数)
========================================================
兼容: 腾讯云 SCF (API 网关触发) / 阿里云 FC (HTTP 触发器)

用法:
  1. 把本文件 + knowledge_server.py 一起打包上传云函数
  2. 入口函数配: serverless.handler (或 index.handler, 看平台)
  3. 环境变量在控制台手动配置 (QWEATHER_HOST/QWEATHER_KEY/ZHIPU_API_KEY)
  4. 触发器: API 网关 / HTTP 触发器, 路径映射 /*
  5. 超时建议 30s (LLM 调用慢), 内存 128MB 够用

本地调试用:
  python handler.py          # 启动本地 8000 端口, 行为与 knowledge_server.py 一致
"""

import json
import sys
import os
import base64

# 把上级目录加入路径, 以便 import knowledge_server
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(_HERE))

# knowledge_server 是纯标准库, import 安全 (不会启动 HTTP 服务)
from knowledge_server import (
    fetch_weather_text,
    fetch_trains_text,
    ask_llm,
    render_pcm,
    find_song,
    build_wav,
    SONGS,
)

# ════════════════════════════════════════════════════════════
#  Event 解析 (兼容腾讯云 SCF / 阿里云 FC)
# ════════════════════════════════════════════════════════════
def _parse_event(event):
    """把各云平台的 event 统一成 (method, path, query_dict, headers_dict, body_str)"""
    # 阿里云 FC 3.0: WSGI environ 风格 (HTTP 触发器经 wsgi_wrapper 调用)
    if isinstance(event, dict) and ("REQUEST_METHOD" in event or "wsgi.input" in event):
        method = event.get("REQUEST_METHOD", "GET")
        path = event.get("PATH_INFO", "/") or "/"
        query = {}
        qs = event.get("QUERY_STRING", "") or ""
        if qs:
            from urllib.parse import parse_qs
            query = {k: v[0] for k, v in parse_qs(qs).items()}
        headers = {}
        for k, v in event.items():
            if k.startswith("HTTP_") and isinstance(v, str):
                headers[k[5:].lower().replace("_", "-")] = v
        body = ""
        wsgi_input = event.get("wsgi.input")
        if wsgi_input is not None:
            try:
                body = wsgi_input.read().decode("utf-8")
            except Exception:
                body = ""
        return method, path, query, headers, body

    # 腾讯云 SCF (API 网关 V2 触发器)
    if "requestContext" in event and "http" in event.get("requestContext", {}):
        ctx = event["requestContext"]["http"]
        method = ctx.get("method", "GET")
        path = ctx.get("path", "/")
        headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
        q = event.get("queryStringParameters") or {}
        query = {k: (v[0] if isinstance(v, list) else v) for k, v in q.items()}
        body = event.get("body", "") or ""
        if event.get("isBase64Encoded") and body:
            body = base64.b64decode(body).decode("utf-8")
        return method, path, query, headers, body

    # 腾讯云 SCF (API 网关 V1 触发器)
    if "httpMethod" in event and "requestContext" in event:
        method = event.get("httpMethod", "GET")
        path = event.get("path", "/")
        headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
        q = event.get("queryString") or {}
        query = {k: (v[0] if isinstance(v, list) else v) for k, v in q.items()}
        body = event.get("body", "") or ""
        if event.get("isBase64Encoded") and body:
            body = base64.b64decode(body).decode("utf-8")
        return method, path, query, headers, body

    # 阿里云 FC (HTTP 触发器)
    if "httpMethod" in event or "requestURI" in event:
        method = event.get("httpMethod", "GET")
        path = event.get("path", "/")
        if not path and "requestURI" in event:
            path = event["requestURI"].split("?")[0]
        headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
        # 阿里云 queryParameters 格式: {"key": {"value": "v"}} 或 {"key": "v"}
        raw_q = event.get("queryParameters") or {}
        query = {}
        for k, v in raw_q.items():
            query[k] = v.get("value") if isinstance(v, dict) else v
        body = event.get("body", "") or ""
        return method, path, query, headers, body

    # 兜底: 当作根路径 GET
    return "GET", "/", {}, {}, ""


def _is_aliyun_fc():
    """检测阿里云 FC 运行环境 (FC 3.0 HTTP 触发器只能返回字符串, 不能返回 dict)"""
    return "FC_FUNCTION_NAME" in os.environ or "FC_SERVICE_NAME" in os.environ


def _resp(body, status=200, content_type="text/plain; charset=utf-8",
          extra_headers=None, is_base64=False):
    """构造云平台通用响应 dict

    ⚠️ 阿里云 FC 3.0 (fcapp.run 域名): handler 只能返回字符串, dict 会被
    运行时当可迭代对象拼接成乱码 → 直接返回 body 文本 (HTTP 固定 200)
    """
    if _is_aliyun_fc():
        return body
    headers = {"Content-Type": content_type}
    if extra_headers:
        headers.update(extra_headers)
    return {
        "isBase64Encoded": is_base64,
        "statusCode": status,
        "headers": headers,
        "body": body,
    }


# ════════════════════════════════════════════════════════════
#  云函数主入口
# ════════════════════════════════════════════════════════════
def handler(event, context):
    """云函数统一入口 (腾讯云 SCF / 阿里云 FC 均可)"""
    method, path, query, headers, body = _parse_event(event)

    # ── 根路径: 简单说明 ──
    if path == "/" or path == "":
        html = (
            "<h1>小智知识服务网关 (云函数版)</h1>"
            "<p>端点: /weather /music /ask /list</p>"
            "<p>部署: C2 云函数 | 状态: 运行中</p>"
        )
        return _resp(html, content_type="text/html; charset=utf-8")

    # ── GET /list ──
    if path == "/list" and method == "GET":
        songs = [s["name"] for s in SONGS.values()]
        return _resp(json.dumps(songs, ensure_ascii=False),
                     content_type="application/json")

    # ── GET /weather ──
    if path == "/weather" and method == "GET":
        city = query.get("city", "西安")
        try:
            days = int(query.get("days", 1))
        except Exception:
            days = 1
        try:
            clothes = int(query.get("clothes", 0))
        except Exception:
            clothes = 0
        text = fetch_weather_text(city, days, bool(clothes))
        return _resp(text)

    # ── GET /music ──
    if path == "/music" and method == "GET":
        q = query.get("q", "")
        song = find_song(q)
        if not song:
            return _resp("找不到这首歌", status=404)
        pcm_bytes, _ = render_pcm(song)
        fmt = query.get("format", "pcm")
        if fmt == "wav":
            wav_bytes = build_wav(pcm_bytes)
            b64 = base64.b64encode(wav_bytes).decode("ascii")
            return _resp(b64, content_type="text/plain", is_base64=False)
        # PCM 直接 base64 (ESP32 端需解码)
        b64 = base64.b64encode(pcm_bytes).decode("ascii")
        return _resp(b64, content_type="text/plain")

    # ── POST /ask ──
    if path == "/ask" and method == "POST":
        try:
            data = json.loads(body) if body else {}
        except Exception:
            return _resp("Invalid JSON", status=400)
        text = data.get("text", "")
        if not text:
            return _resp("text field required", status=400)
        # v1w: 透传对话历史 (ESP32 端维护 2 轮, 网关指代消解/追问依赖它)
        history = data.get("history", []) or []
        reply = ask_llm(text, history)
        if reply is None:
            reply = "这个问题我想不起来了"
        return _resp(reply)

    # ── 404 ──
    return _resp("Not Found: %s %s" % (method, path), status=404)


# ════════════════════════════════════════════════════════════
#  WSGI 入口 (腾讯云 SCF Web 函数专用)
#  Web 函数自带公网 URL, 不需要 API 网关 (经典 API 网关已停售!)
#  执行方法填: handler.wsgi_app
# ════════════════════════════════════════════════════════════
def wsgi_app(environ, start_response):
    """WSGI 应用 — 把 WSGI environ 转成 event dict, 复用 handler()"""
    import urllib.parse

    method = environ.get("REQUEST_METHOD", "GET")
    path = environ.get("PATH_INFO", "/")
    qs = environ.get("QUERY_STRING", "")
    query = {k: v[0] for k, v in urllib.parse.parse_qs(qs).items()}

    # environ 里 HTTP_ 前缀的键转成 headers dict
    headers = {}
    for k, v in environ.items():
        if k.startswith("HTTP_"):
            headers[k[5:].lower().replace("_", "-")] = v

    # 读取请求体
    try:
        cl = int(environ.get("CONTENT_LENGTH") or 0)
        body = environ["wsgi.input"].read(cl).decode("utf-8", "replace") if cl > 0 else ""
    except Exception:
        body = ""

    event = {
        "httpMethod": method,
        "path": path,
        "queryStringParameters": query,
        "headers": headers,
        "body": body,
    }
    r = handler(event, None)

    status = "%d %s" % (r["statusCode"], "OK" if r["statusCode"] < 400 else "Error")
    body_bytes = r["body"].encode("utf-8")
    resp_headers = [(k, v) for k, v in r["headers"].items()]
    resp_headers.append(("Content-Length", str(len(body_bytes))))
    start_response(status, resp_headers)
    return [body_bytes]


# ════════════════════════════════════════════════════════════
#  本地调试入口 (直接运行本文件 → 本地 HTTP 服务)
# ════════════════════════════════════════════════════════════
if __name__ == "__main__":
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    import urllib.parse

    class LocalHandler(BaseHTTPRequestHandler):
        def _send(self, status, body, content_type="text/plain; charset=utf-8"):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.end_headers()
            self.wfile.write(body.encode("utf-8"))

        def do_GET(self):
            p = urllib.parse.urlparse(self.path)
            path = p.path
            q = urllib.parse.parse_qs(p.query)
            query = {k: v[0] for k, v in q.items()}
            event = {
                "httpMethod": "GET",
                "path": path,
                "queryStringParameters": query,
                "headers": dict(self.headers),
                "body": "",
            }
            r = handler(event, None)
            self._send(r["statusCode"], r["body"], r["headers"].get("Content-Type", "text/plain"))

        def do_POST(self):
            p = urllib.parse.urlparse(self.path)
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length).decode("utf-8") if length > 0 else ""
            event = {
                "httpMethod": "POST",
                "path": p.path,
                "queryStringParameters": {},
                "headers": dict(self.headers),
                "body": body,
            }
            r = handler(event, None)
            self._send(r["statusCode"], r["body"], r["headers"].get("Content-Type", "text/plain"))

    port = int(os.environ.get("PORT", 8000))
    srv = ThreadingHTTPServer(("0.0.0.0", port), LocalHandler)
    print("[Local] 云函数适配层本地调试: http://localhost:%d" % port)
    print("[Local] POST /ask body={\"text\":\"查火车\"}")
    print("[Local] GET  /weather?city=西安&clothes=1")
    srv.serve_forever()
