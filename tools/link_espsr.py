"""
extra_script.py — 把 lib/esp-sr/lib 下的预编译静态库链接进固件

背景:
  esp-sr 官方只发布 ESP-IDF 组件, 本项目是 Arduino/PlatformIO 框架。
  做法: 只取 WakeNet 相关的预编译 .a + 自编译 model_path.c + esp-dsp 子集源码。

  工具链在 Windows 下无法从含中文的项目路径读取 .a (链接报 cannot find -lxxx),
  所以先把 .a 拷到 build_dir (纯 ASCII 路径) 再用 -L 指过去。

P8d: 仅保留 WakeNet 唤醒词所需库 (multinet/fst 已随命令词模块一并移除 —
  它们只服务于 MultiNet 命令词, 现已改 STT 文本路由)。
链接顺序: 依赖方在前 (wakenet → c_speech_features → dl_lib → hufzip)
"""
Import("env")

import os
import shutil

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")

src_dir = os.path.join(project_dir, "lib", "esp-sr", "lib")
dst_dir = os.path.join(build_dir, "espsr_libs")

LIBS = ["wakenet", "c_speech_features", "dl_lib", "hufzip"]


def copy_static_libs(source, target, env):
    os.makedirs(dst_dir, exist_ok=True)
    for lib in LIBS:
        src = os.path.join(src_dir, "lib%s.a" % lib)
        dst = os.path.join(dst_dir, "lib%s.a" % lib)
        if not os.path.exists(src):
            continue
        shutil.copy2(src, dst)
        print("espsr link: copied lib%s.a -> %s" % (lib, dst_dir))


env.Append(LIBPATH=[dst_dir])
env.Append(LIBS=LIBS)

# 链接前执行拷贝
env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", copy_static_libs)
