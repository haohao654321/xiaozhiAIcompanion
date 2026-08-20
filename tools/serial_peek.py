"""快速串口读取 — 抓最近 N 秒日志, 发 CMD 看响应"""
import serial, sys, time

PORT = "COM5"
BAUD = 115200
CMD = "FACELIST"  # 查人脸注册列表
READ_SEC = 5      # 读取时长

try:
    s = serial.Serial(PORT, BAUD, timeout=0.5)
except Exception as e:
    print(f"[ERR] open {PORT}: {e}")
    sys.exit(1)

# 发命令
time.sleep(0.2)
s.write((CMD + "\n").encode())
s.flush()

start = time.time()
buf = []
while time.time() - start < READ_SEC:
    chunk = s.read(4096)
    if chunk:
        try:
            text = chunk.decode("utf-8", errors="replace")
        except:
            text = repr(chunk)
        buf.append(text)
        print(text, end="", flush=True)
    time.sleep(0.05)

s.close()
print("\n\n[done]")
