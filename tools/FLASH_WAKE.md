# P4-1 唤醒词烧录指引

固件编译已通过。真机需要烧 **两样东西**：固件（含新分区表）+ 唤醒词模型。

## 1. 烧固件（VSCode 里 pio upload 即可，会自动带上新分区表）

分区表已变更（factory 11MB→5MB，新增 model 分区 @ 0x520000），
upload 会自动重烧 partitions.bin，无需手动擦 flash。

## 2. 烧唤醒词模型（只烧一次，换模型才需要重烧）

在项目目录打开终端（COM 口按实际改）:

```
C:/Users/123/.platformio/packages/tool-esptoolpy/esptool.exe --chip esp32s3 --port COM5 --baud 921600 --before default_reset --after hard_reset write_flash 0x520000 tools/srmodels.bin
```

## 3. 验证

monitor 里看:

```
[WAKE] WakeNet9 OK: model=wn9_nihaoxiaozhi word='你好小智' chunk=480 (30ms) rate=16000
```

然后喊 **"你好小智"**，预期日志:

```
[WAKE] **'你好小智' DETECTED** (#1) armed 10s
[CONV] Wake word trigger
[CONV] -> RECORDING
```

唤醒后 10 秒内说话都会进入对话（VAD 辅助触发）。
没烧模型时自动降级回原来的纯 VAD 触发，功能不受影响（日志会有 FAIL 提示）。

## 备注

- 唤醒词: "你好小智" (乐鑫内置 wn9_nihaoxiaozhi 模型)
- 检测模式 DET_MODE_95（命中率高; 误唤醒多改 system_config.h 的 WAKE_DETECT_MODE 为 DET_MODE_90）
- 喇叭提示音/说话播放期间不喂唤醒词（防自激，与 VAD 同一套门控）
- 自定义唤醒词"伙计"需乐鑫官方训练（TTS Pipeline，数周），链路验证后再考虑
