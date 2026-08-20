#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小智 · 知识服务网关 — 云函数部署辅助脚本
=============================================
用法:
  cd tools/serverless
  python deploy.py --platform scf   # 腾讯云 SCF
  python deploy.py --platform fc    # 阿里云 FC
  python deploy.py                  # 只打包, 不指定平台

功能:
  1. 自动打包 knowledge_server.py + handler.py 成 zip
  2. 读取 ../.env 输出环境变量配置清单
  3. 输出对应平台的创建/配置命令

注意:
  - 凭据 (API KEY) 不上传代码, 只在控制台配环境变量
  - .env 文件不要打包进 zip (已自动排除)
"""

import argparse
import os
import sys
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_PROJECT_ROOT = os.path.dirname(_HERE)


def _read_dotenv():
    """读取 ../.env, 返回 key:value 字典"""
    env_path = os.path.join(_PROJECT_ROOT, ".env")
    env = {}
    if not os.path.exists(env_path):
        print("[WARN] 找不到 %s, 请确保已配置凭据" % env_path)
        return env
    with open(env_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, _, v = line.partition("=")
            k = k.strip()
            v = v.strip().strip('"').strip("'")
            if k:
                env[k] = v
    return env


def _pack_zip(output_name="xiaoZhi-serverless.zip"):
    """打包部署 zip (排除 .env / __pycache__ / .git)"""
    out_path = os.path.join(_HERE, output_name)
    files_to_pack = [
        (os.path.join(_PROJECT_ROOT, "knowledge_server.py"), "knowledge_server.py"),
        (os.path.join(_HERE, "handler.py"), "handler.py"),
    ]
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for src, arcname in files_to_pack:
            if not os.path.exists(src):
                print("[ERROR] 找不到 %s, 打包失败" % src)
                sys.exit(1)
            zf.write(src, arcname)
            print("[PACK] %s → %s" % (src, arcname))
    size = os.path.getsize(out_path)
    print("[PACK] 输出: %s (%.1f KB)" % (out_path, size / 1024))
    return out_path


def _print_env_vars(env):
    """输出环境变量配置清单"""
    print("\n" + "=" * 50)
    print("环境变量配置清单 (复制到云函数控制台)")
    print("=" * 50)
    required = ["QWEATHER_HOST", "QWEATHER_KEY", "ZHIPU_API_KEY"]
    for k in required:
        v = env.get(k, "")
        mask = v[:4] + "****" + v[-4:] if len(v) > 8 else "(未设置)"
        print("  %s = %s" % (k, mask))
    if not env:
        print("  [WARN] .env 为空, 请先在 tools/.env 配置凭据")
    print("=" * 50 + "\n")


def _print_scf_guide():
    print("【腾讯云 SCF 部署步骤】")
    print("1. 登录 https://console.cloud.tencent.com/scf")
    print("2. 新建函数 → 自定义创建 → 事件函数")
    print("3. 基础配置:")
    print("   - 函数名称: xiaoZhi-knowledge-server")
    print("   - 运行环境: Python 3.9 (或 3.10)")
    print("   - 内存: 128MB")
    print("   - 超时: 30 秒 (LLM 调用需要)")
    print("4. 函数代码: 上传 zip (xiaoZhi-serverless.zip)")
    print("   - 执行方法: handler.handler  (文件.handler函数)")
    print("5. 环境变量: 粘贴上面的清单")
    print("6. 触发器: API 网关触发器")
    print("   - 协议: HTTP")
    print("   - 路径: /{path+}  (透传所有路径)")
    print("   - 方法: ANY")
    print("7. 访问地址: 触发器自动生成公网 URL")
    print("8. ESP32 system_config.h 中 KNOWLEDGE_SERVER_URL 改成该 URL")
    print()


def _print_fc_guide():
    print("【阿里云 FC 部署步骤】")
    print("1. 登录 https://fcnext.console.aliyun.com")
    print("2. 新建函数 → 使用标准 Runtime → Python 3.9/3.10")
    print("3. 函数配置:")
    print("   - 函数名称: xiaoZhi-knowledge-server")
    print("   - 内存: 128MB")
    print("   - 超时: 30 秒")
    print("4. 代码: 上传 zip (xiaoZhi-serverless.zip)")
    print("   - Handler: handler.handler")
    print("5. 环境变量: 粘贴上面的清单")
    print("6. 触发器: HTTP 触发器")
    print("   - 认证方式: 不认证 (或函数计算认证 + ESP32 带签名)")
    print("   - 请求方式: GET/POST/ANY")
    print("7. 访问地址: 触发器自动生成公网 URL (格式: https://xxx.fcapp.run)")
    print("8. ESP32 system_config.h 中 KNOWLEDGE_SERVER_URL 改成该 URL")
    print()


def main():
    ap = argparse.ArgumentParser(description="小智知识服务网关 — 云函数部署打包")
    ap.add_argument("--platform", choices=["scf", "fc"], default=None,
                    help="云平台: scf=腾讯云, fc=阿里云")
    ap.add_argument("--output", default="xiaoZhi-serverless.zip",
                    help="输出 zip 文件名")
    args = ap.parse_args()

    env = _read_dotenv()
    zip_path = _pack_zip(args.output)
    _print_env_vars(env)

    if args.platform == "scf":
        _print_scf_guide()
    elif args.platform == "fc":
        _print_fc_guide()
    else:
        print("【通用部署说明】")
        print(" zip 已打包: %s" % zip_path)
        print(" 请任选一家云平台 (腾讯云 SCF / 阿里云 FC) 上传部署")
        print(" 运行时: Python 3.9+, 入口函数: handler.handler")
        print(" 环境变量见上方清单")
        print()
        print(" 加 --platform scf 或 --platform fc 可查看详细步骤")

    print("[DONE] 打包完成, 请按上述步骤上传到云平台")


if __name__ == "__main__":
    main()
