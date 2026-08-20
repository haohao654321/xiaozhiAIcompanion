#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小智 · 阿里云 FC 自动部署脚本
=================================
一键完成: 创建服务 + 创建函数(上传代码) + 配环境变量 + 创建 HTTP 触发器 + 输出公网 URL

用法:
  python deploy_fc.py --access-key-id <AK> --access-key-secret <SK>
  python deploy_fc.py --access-key-id <AK> --access-key-secret <SK> --region cn-hangzhou

依赖: aliyun-python-sdk-core (pip install aliyun-python-sdk-core)
"""
import argparse
import base64
import json
import os
import sys
import time
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.dirname(_HERE)

SERVICE_NAME = "xiaoZhi-knowledge-server"
FUNCTION_NAME = "xiaoZhi-knowledge-server"
TRIGGER_NAME = "http-trigger"
_REGION_DEFAULT = "cn-hangzhou"

_ENV_KEYS = ["QWEATHER_HOST", "QWEATHER_KEY", "ZHIPU_API_KEY"]


def _load_dotenv():
    env_path = os.path.join(_PROJECT_ROOT, ".env")
    env = {}
    if os.path.exists(env_path):
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                env[k.strip()] = v.strip().strip('"').strip("'")
    return env


def _pack_zip():
    """打包 handler.py + knowledge_server.py (阿里云标准 Python 运行时)"""
    out_path = os.path.join(_HERE, "xiaoZhi-serverless-fc.zip")
    files_to_pack = [
        (os.path.join(_PROJECT_ROOT, "knowledge_server.py"), "knowledge_server.py"),
        (os.path.join(_HERE, "handler.py"), "handler.py"),
    ]
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, arcname in files_to_pack:
            if not os.path.exists(src):
                print("[ERROR] 找不到 %s" % src)
                sys.exit(1)
            zf.write(src, arcname)
    with open(out_path, "rb") as f:
        zip_b64 = base64.b64encode(f.read()).decode("ascii")
    print("[PACK] %s (%.1f KB)" % (out_path, os.path.getsize(out_path) / 1024))
    return zip_b64


def main():
    ap = argparse.ArgumentParser(description="阿里云 FC 自动部署")
    ap.add_argument("--access-key-id", default=os.environ.get("ALIBABA_CLOUD_ACCESS_KEY_ID", ""))
    ap.add_argument("--access-key-secret", default=os.environ.get("ALIBABA_CLOUD_ACCESS_KEY_SECRET", ""))
    ap.add_argument("--region", default=_REGION_DEFAULT)
    args = ap.parse_args()

    if not args.access_key_id or not args.access_key_secret:
        print("[ERROR] 需要提供阿里云 AccessKey ID 和 AccessKey Secret")
        print("  用法: python deploy_fc.py --access-key-id xxx --access-key-secret xxx")
        sys.exit(1)

    env_vars = _load_dotenv()
    missing = [k for k in _ENV_KEYS if not env_vars.get(k)]
    if missing:
        print("[ERROR] tools/.env 缺少: %s" % ", ".join(missing))
        sys.exit(1)

    zip_b64 = _pack_zip()

    try:
        from aliyunsdkcore.client import AcsClient
        from aliyunsdkcore.request import RoaRequest
    except ImportError:
        print("[ERROR] 未安装 aliyun-python-sdk-core: pip install aliyun-python-sdk-core")
        sys.exit(1)

    client = AcsClient(args.access_key_id, args.access_key_secret, args.region)

    # ════════════════════════════════════════════════════
    #  FC 2021-04-06 OpenAPI (ROA)
    #  ⚠️ endpoint 必须是 fc.{region}.aliyuncs.com (SDK 默认生成
    #     {region}.fc.aliyuncs.com 是错的!) 且必须走 HTTPS
    # ════════════════════════════════════════════════════
    fc_endpoint = "fc.%s.aliyuncs.com" % args.region

    class FCApi(RoaRequest):
        def __init__(self, action, method, uri):
            RoaRequest.__init__(self, "fc", "2021-04-06", action,
                                method=method, uri_pattern=uri,
                                protocol="https")
            self.set_accept_format("JSON")
            self.set_content_type("application/json")
            self.set_endpoint(fc_endpoint)

    def _call(req, body=None, expect=200):
        if body is not None:
            req.set_content(json.dumps(body).encode("utf-8"))
        try:
            resp = client.do_action_with_exception(req)
            return json.loads(resp) if resp else {}
        except Exception as e:
            msg = str(e)
            # 阿里云错误格式: "HTTP Status: 409 Error:ServiceAlreadyExists xxx"
            err_code = "Unknown"
            try:
                if "Error:" in msg:
                    err_code = msg.split("Error:", 1)[1].split(" ", 1)[0]
            except Exception:
                pass
            print("[ERROR] %s" % msg[:200])
            return {"_error": err_code, "_msg": msg}

    # ── 1. 创建服务 (已存在则跳过) ──
    print("[1/4] 创建服务 %s ..." % SERVICE_NAME)
    svc_req = FCApi("CreateService", "POST",
                    "/2021-04-06/services")
    r = _call(svc_req, {
        "serviceName": SERVICE_NAME,
        "description": "小智知识服务网关 (天气/火车/LLM问答)",
    })
    if r.get("_error"):
        if r["_error"] == "ServiceAlreadyExists":
            print("[OK] 服务已存在, 跳过")
        else:
            print("[FAIL] 服务创建失败, 中止")
            sys.exit(1)
    else:
        print("[OK] 服务创建成功: %s" % r.get("serviceName", SERVICE_NAME))

    # ── 2. 创建/更新函数 (幂等: 存在则 UpdateFunction 更新代码, 不存在则创建) ──
    fn_body = {
        "handler": "handler.handler",
        "memorySize": 128,
        "timeout": 30,
        "environmentVariables": env_vars,
        "code": {"zipFile": zip_b64},
        "description": "小智知识服务网关 (天气/火车/LLM问答)",
    }
    get_fn = FCApi("GetFunction", "GET",
                   "/2021-04-06/services/%s/functions/%s" % (SERVICE_NAME, FUNCTION_NAME))
    r = _call(get_fn)
    if r.get("_error") == "FunctionNotFound":
        print("[2/4] 创建函数 %s (Python3.9, 128MB, 30s)..." % FUNCTION_NAME)
        fn_body["functionName"] = FUNCTION_NAME
        fn_body["runtime"] = "python3.9"
        fn_req = FCApi("CreateFunction", "POST",
                       "/2021-04-06/services/%s/functions" % SERVICE_NAME)
        r = _call(fn_req, fn_body)
        if r.get("_error"):
            print("[FAIL] 函数创建失败, 中止")
            sys.exit(1)
        print("[OK] 函数创建成功")
    elif r.get("_error"):
        print("[FAIL] 查询函数失败, 中止")
        sys.exit(1)
    else:
        print("[2/4] 函数已存在, 更新代码+配置...")
        upd = FCApi("UpdateFunction", "PUT",
                    "/2021-04-06/services/%s/functions/%s" % (SERVICE_NAME, FUNCTION_NAME))
        r = _call(upd, fn_body)
        if r.get("_error"):
            print("[FAIL] 函数更新失败, 中止")
            sys.exit(1)
        print("[OK] 函数更新成功")

    # ── 3. 创建 HTTP 触发器 (幂等: 存在则跳过) ──
    print("[3/4] 检查 HTTP 触发器 (匿名, GET/POST)...")
    tr_get = FCApi("GetTrigger", "GET",
                   "/2021-04-06/services/%s/functions/%s/triggers/%s"
                   % (SERVICE_NAME, FUNCTION_NAME, TRIGGER_NAME))
    r = _call(tr_get)
    if not r.get("_error"):
        print("[OK] 触发器已存在, 跳过 (如需改配置请手动删了重建)")
    else:
        tr_req = FCApi("CreateTrigger", "POST",
                       "/2021-04-06/services/%s/functions/%s/triggers"
                       % (SERVICE_NAME, FUNCTION_NAME))
        tr_body = {
            "triggerName": TRIGGER_NAME,
            "triggerType": "http",
            # ⚠️ OpenAPI 2021-04-06 的 triggerConfig 是 JSON 字符串, 不是对象! (Go map 序列化报错)
            "triggerConfig": json.dumps({
                "authType": "anonymous",
                "methods": ["GET", "POST"],
            }),
            "description": "HTTP 匿名访问 (小智知识服务网关)",
        }
        r = _call(tr_req, tr_body)
        if r.get("_error"):
            print("[FAIL] 触发器创建失败, 中止")
            sys.exit(1)
        print("[OK] HTTP 触发器创建成功")

    # ── 4. 查询触发器拿官方公网 URL (urlInternet 字段) ──
    print("[4/4] 获取公网访问地址...")
    tr_get2 = FCApi("GetTrigger", "GET",
                    "/2021-04-06/services/%s/functions/%s/triggers/%s"
                    % (SERVICE_NAME, FUNCTION_NAME, TRIGGER_NAME))
    r = _call(tr_get2)
    url = r.get("urlInternet", "") if not r.get("_error") else ""
    if not url:
        print("[ERROR] 触发器信息里没有 urlInternet, 请到控制台查看")
        print("  https://fcnext.console.aliyun.com/cn-hangzhou/functions")
        sys.exit(1)

    print("[DONE] 部署完成!")
    print("=" * 60)
    print("  服务:    %s" % SERVICE_NAME)
    print("  函数:    %s" % FUNCTION_NAME)
    print("  地域:    %s" % args.region)
    print("  公网URL: %s" % url)
    print("=" * 60)
    print("  测试: %s/weather?city=西安" % url)
    print("  测试: curl -X POST %s/ask -d '{\"text\":\"查火车\"}'" % url)
    print()
    print("  ESP32 固件 KNOWLEDGE_SERVER_URL 改成上面的 %s (不含子路径)" % url)

    # ── 5. 部署后预热: 连发请求触发新实例创建 ──
    # FC 更新代码后旧实例不会立即回收 (约10-15分钟), 期间新旧代码并存。
    # 连发健康请求把流量打到新实例上, 降低用户命中旧实例的概率。
    print("[5/5] 预热新实例 (GET /list x5)...")
    import urllib.request as _urlreq
    for i in range(5):
        try:
            with _urlreq.urlopen(url.rstrip("/") + "/list", timeout=15) as r:
                _ = r.read(200)
            print("  [prewarm %d/5] OK" % (i + 1))
        except Exception as e:
            print("  [prewarm %d/5] ERR: %s" % (i + 1, str(e)[:80]))
        time.sleep(2)
    print("[DONE] 预热完成! 建议等 10-15 分钟旧实例全部回收后效果最稳定")


if __name__ == "__main__":
    main()
