#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小智 · 知识服务网关 (P8e)
=========================
统一 HTTP 服务, 合并音乐合成 + 天气查询 + LLM 联网问答。

端点:
  GET /                                → HTML 说明页
  GET /list                            → JSON 曲目列表
  GET /music?q=小星星                  → 裸 PCM (16kHz/16bit/mono)
  GET /music?q=小星星&format=wav       → WAV (浏览器试听)
  GET /weather?city=北京&days=1&clothes=0 → 纯文本天气播报 (≤50字, UTF-8)
  POST /ask                            → LLM 联网问答 (B+C 合一, 字数分级)

P8e: POST /ask — "问什么都能答"
  请求: POST /ask  {"text":"用户STT文本"}
  机制 B (web_search 兜底): 调 GLM-4-Flash + tools=[web_search(enable=true)]
    LLM 自动联网搜索后直接生成回复 → 火车/新闻/百科/人物 全覆盖
  机制 C (function calling 精确层): 同时传 get_weather function tool
    LLM 选了 function → 执行 get_weather → 回传结果 → LLM 生成自然语言播报
    比 web_search 纯搜索引擎更精准 (天气有结构化真数据)
  响应: 分级字数纯文本 (UTF-8), 直接 TTS 播报
    2026-08-19 C+B: prompt 强制 ≤40字核心结论; 超50字兜底截 48 + "……大概就是这样"
    中等 (25~50) → 原样
    简单 (≤24)  → 原样
    铁律: ≤100 字 (ESP32 TTS 缓冲区)

天气数据源: 和风天气 (https://dev.qweather.com)
  - 城市名 → GeoAPI 查 location ID (带缓存)
  - 实时 / 3天预报 / 穿衣指数 → 格式化为口语化短句
  - 天气结果缓存 1 小时 (防超免费额度 1000次/日)

和风天气 (新版 API): 环境变量 QWEATHER_HOST(专属域名) + QWEATHER_KEY(API KEY)
  Windows:  set QWEATHER_HOST=my7fc4dwmj.re.qweatherapi.com  &&  set QWEATHER_KEY=你的KEY  &&  python knowledge_server.py
  Linux:    QWEATHER_HOST=... QWEATHER_KEY=... python knowledge_server.py
  - 新版认证: X-QW-Api-Key 请求头 + gzip 压缩响应 (不再用 ?key= 参数)
  - 兼容旧版: 只设 QWEATHER_KEY 时自动走 devapi.qweather.com + ?key=

智谱 GLM-4-Flash: 环境变量 ZHIPU_API_KEY (或硬编码在 config)
  免费, 支持 web_search + function calling
  POST https://open.bigmodel.cn/api/paas/v4/chat/completions

真机联调:
  ESP32 天气: http://<PC_IP>:8000/weather?city=西安&clothes=1
  ESP32 音乐: http://<PC_IP>:8000/music?q=小星星
  ESP32 问答: POST http://<PC_IP>:8000/ask  body={"text":"查一下今晚西安北站的火车车次"}

零第三方依赖 (纯 Python 标准库), 本地 PC / 云函数 / VPS 都能跑。
"""

import argparse
import array
import datetime
import gzip
import json
import math
import os
import ssl
import struct
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ════════════════════════════════════════════════════════════
#  .env 自动加载 (纯标准库, 不引入 python-dotenv 依赖)
#  仅当对应环境变量为空时, 从同目录 .env 文件读取 (不覆盖已有的)
# ════════════════════════════════════════════════════════════
def _load_dotenv():
    env_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
    if not os.path.exists(env_path):
        return
    with open(env_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key = key.strip()
            val = val.strip()
            # 去掉值两端的引号 (如果有的话)
            if len(val) >= 2 and val[0] == val[-1] and val[0] in ('"', "'"):
                val = val[1:-1]
            if key and not os.environ.get(key):
                os.environ[key] = val
_load_dotenv()

# ════════════════════════════════════════════════════════════
#  音乐合成 (从 music_server.py 合并, 自包含)
# ════════════════════════════════════════════════════════════
SAMPLE_RATE = 16000
AMPLITUDE = 0.45

NOTE_FREQ = {
    'C0':16.35,'Cs0':17.32,'D0':18.35,'Ds0':19.45,'E0':20.60,'F0':21.83,'Fs0':23.12,'G0':24.50,'Gs0':25.96,'A0':27.50,'As0':29.14,'B0':30.87,
    'C1':32.70,'Cs1':34.65,'D1':36.71,'Ds1':38.89,'E1':41.20,'F1':43.65,'Fs1':46.25,'G1':49.00,'Gs1':51.91,'A1':55.00,'As1':58.27,'B1':61.74,
    'C2':65.41,'Cs2':69.30,'D2':73.42,'Ds2':77.78,'E2':82.41,'F2':87.31,'Fs2':92.50,'G2':98.00,'Gs2':103.83,'A2':110.00,'As2':116.54,'B2':123.47,
    'C3':130.81,'Cs3':138.59,'D3':146.83,'Ds3':155.56,'E3':164.81,'F3':174.61,'Fs3':185.00,'G3':196.00,'Gs3':207.65,'A3':220.00,'As3':233.08,'B3':246.94,
    'C4':261.63,'Cs4':277.18,'D4':293.66,'Ds4':311.13,'E4':329.63,'F4':349.23,'Fs4':369.99,'G4':392.00,'Gs4':415.30,'A4':440.00,'As4':466.16,'B4':493.88,
    'C5':523.25,'Cs5':554.37,'D5':587.33,'Ds5':622.25,'E5':659.26,'F5':698.46,'Fs5':739.99,'G5':783.99,'Gs5':830.61,'A5':880.00,'As5':932.33,'B5':987.77,
    'C6':1046.50,'Cs6':1108.73,'D6':1174.66,'Ds6':1244.51,'E6':1318.51,'F6':1396.91,'Fs6':1479.98,'G6':1567.98,'Gs6':1661.22,'A6':1760.00,'As6':1864.66,'B6':1975.53,
    'C7':2093.00,'Cs7':2217.46,'D7':2349.32,'Ds7':2489.02,'E7':2637.02,'F7':2793.83,'Fs7':2959.96,'G7':3135.96,'Gs7':3322.44,'A7':3520.00,'As7':3729.31,'B7':3951.07,
    'C8':4186.01,
}

def _solfege(note_str, octave_base=4):
    n = note_str[0]
    if n.isdigit():
        names = ['C', 'D', 'E', 'F', 'G', 'A', 'B']
        note = names[(int(n) - 1) % 7]
        octave = octave_base
        sharp = '#' if len(note_str) > 1 and note_str[1] == '#' else ''
        return note + sharp + str(octave)
    return note_str

def note_freq(note_name):
    if note_name in NOTE_FREQ:
        return NOTE_FREQ[note_name]
    if note_name and note_name[0].isdigit():
        return NOTE_FREQ[_solfege(note_name)]
    return None

def synth_note(freq, dur_s, amp=AMPLITUDE):
    """合成单音符 → array('h') (16bit PCM)
    ⚠️ 2026-08-20: list of int → array('h'), 内存降 14 倍 (云函数 128MB OOM 修复:
    list 每样本 28B, 小星星 44 万样本 = 12MB+; array 仅 880KB)
    """
    n = int(SAMPLE_RATE * dur_s)
    if freq is None or n <= 0:
        return array.array('h')
    out = array.array('h')
    harmonics = [(1.0, 1.00), (2.0, 0.35), (3.0, 0.18), (4.0, 0.09)]
    decay = 1.8 + 2.2 * (freq / 1000.0)
    fade_n = int(0.012 * SAMPLE_RATE)
    w_sum = sum(w for _, w in harmonics)
    for i in range(n):
        t = i / SAMPLE_RATE
        env = math.exp(-decay * t)
        s = 0.0
        for mult, w in harmonics:
            s += w * math.sin(2.0 * math.pi * freq * mult * t)
        v = s / w_sum
        v *= env * amp
        attack = min(1.0, i / (0.005 * SAMPLE_RATE))
        v *= attack
        if i >= n - fade_n:
            v *= (n - i) / float(fade_n)
        out.append(int(v * 32767))
    return out

SONGS = {}
def _register(name, aliases, notes, bpm=100):
    SONGS[name] = {'name': name, 'aliases': aliases, 'notes': notes, 'bpm': bpm}

_register('小星星', ['twinkle', '一闪一闪亮晶晶', '小星星变奏曲'],
    [('1',1),('1',1),('5',1),('5',1),('6',1),('6',1),('5',2),
     ('4',1),('4',1),('3',1),('3',1),('2',1),('2',1),('1',2),
     ('5',1),('5',1),('4',1),('4',1),('3',1),('3',1),('2',2),
     ('5',1),('5',1),('4',1),('4',1),('3',1),('3',1),('2',2),
     ('1',1),('1',1),('5',1),('5',1),('6',1),('6',1),('5',2),
     ('4',1),('4',1),('3',1),('3',1),('2',1),('2',1),('1',2)], bpm=96)

_register('致爱丽丝', ['fur elise', '爱丽丝', '给爱丽丝'],
    [('E5',1,'r'),('D#5',1),('E5',1),('D#5',1),('E5',1),
     ('B4',1),('D5',1),('C5',1),('A4',2),
     ('C4',1),('E4',1),('A4',1),('B4',1),
     ('E4',1),('G#4',1),('B4',1),('C5',1),
     ('E4',1),('E5',1),('D#5',1),('E5',1),('D#5',1),('E5',1),
     ('B4',1),('D5',1),('C5',1),('A4',2),
     ('C4',1),('E4',1),('A4',1),('B4',1),
     ('E4',1),('C5',1),('B4',1),('A4',2)], bpm=110)

_register('欢乐颂', ['ode to joy', '贝多芬', '欢乐颂主题'],
    [('3',1),('3',1),('4',1),('5',1),('5',1),('4',1),('3',1),('2',1),
     ('1',1),('1',1),('2',1),('3',1),('3',2),('2',2),
     ('3',1),('3',1),('4',1),('5',1),('5',1),('4',1),('3',1),('2',1),
     ('1',1),('1',1),('2',1),('3',1),('2',2),('1',2),
     ('2',1),('2',1),('3',1),('1',1),('2',1),('3',1),('4',2),('3',1),('1',1),
     ('2',1),('3',1),('4',2),('3',2),('2',1),('1',1),('2',1),('5',2),
     ('3',1),('3',1),('4',1),('5',1),('5',1),('4',1),('3',1),('2',1),
     ('1',1),('1',1),('2',1),('3',1),('2',2),('1',2)], bpm=110)

_register('两只老虎', ['two tigers', '老虎'],
    [('1',1),('2',1),('3',1),('1',1),('1',1),('2',1),('3',1),('1',1),
     ('3',1),('4',1),('5',2),('3',1),('4',1),('5',2),
     ('5',0.5),('6',0.5),('5',0.5),('4',0.5),('3',1),('1',1),
     ('5',0.5),('6',0.5),('5',0.5),('4',0.5),('3',1),('1',1),
     ('1',1),('5',1),('1',2),('1',1),('5',1),('1',2)], bpm=120)

_register('生日快乐', ['happy birthday', '生日歌'],
    [('5',0.75),('5',0.25),('6',1),('5',1),('1',1),('7',2),
     ('5',0.75),('5',0.25),('6',1),('5',1),('2',1),('1',2),
     ('5',0.75),('5',0.25),('5',1),('3',1),('1',1),('7',1),('6',2),
     ('4',0.75),('4',0.25),('3',1),('1',1),('2',1),('1',2)], bpm=100)

_register('天空之城', ['castle in the sky', '天空之城主题', '君をのせて'],
    [('6',1),('5',1),('4',1),('3',1),('4',1),('5',1),('6',1),('7',1),
     ('1',2),('7',1),('6',1),('5',2),('4',1),('3',1),
     ('2',1),('3',1),('4',1),('5',1),('4',1),('3',1),('2',1),('1',1),
     ('2',2),('3',1),('2',1),('1',2),('7',1),('6',1),
     ('6',1),('5',1),('4',1),('3',1),('4',1),('5',1),('6',1),('7',1),
     ('1',2),('7',1),('6',1),('5',2),('4',1),('3',1),
     ('2',1),('3',1),('4',1),('5',1),('4',1),('3',1),('2',1),('1',1),
     ('2',2),('3',1),('2',1),('1',2),('7',1),('6',1)], bpm=90)

def render_pcm(song, bpm=None, max_seconds=None):
    """整曲渲染 → (bytes PCM, sample数)
    ⚠️ 2026-08-20: list+struct.pack(*samples) 展开 44 万参数 → 云函数 128MB OOM;
    改 array('h') 拼接 + tobytes, 峰值内存 <3MB
    ⚠️ 2026-08-20: FC 3.0 WSGI 响应超 1MB 报 invalid response type —
    30s 小星星 base64=1.25MB 超限 → 加 max_seconds 截断 (15s 旋律足够听, 纯云端不烧固件)
    """
    bpm = bpm or song['bpm']
    beat = 60.0 / bpm
    samples = array.array('h')
    max_n = int((max_seconds or 999) * SAMPLE_RATE)
    for note in song['notes']:
        name, beats = note[0], note[1]
        rest = len(note) > 2 and note[2] == 'r'
        dur = beats * beat
        n = int(SAMPLE_RATE * dur)
        if max_seconds and len(samples) + n > max_n:
            break                        # 超时截断, 保证音符完整 (断在音符边界)
        if rest:
            samples.extend(array.array('h', [0]) * n)
        else:
            samples.extend(synth_note(note_freq(name), dur))
    return samples.tobytes(), len(samples)

def find_song(query):
    q = query.strip().lower()
    if not q:
        return None
    for song in SONGS.values():
        if q == song['name'] or q in [a.lower() for a in song['aliases']]:
            return song
        if song['name'] in q or q in song['name']:
            return song
    return None

def build_wav(pcm_bytes, sample_rate=SAMPLE_RATE):
    n = len(pcm_bytes)
    header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + n, b'WAVE',
        b'fmt ', 16, 1, 1, sample_rate, sample_rate * 2, 2, 16,
        b'data', n)
    return header + pcm_bytes

# ════════════════════════════════════════════════════════════
#  天气查询 (和风天气)
# ════════════════════════════════════════════════════════════
# 新版 API: QWEATHER_HOST(专属域名如 my7fc4dwmj.re.qweatherapi.com) + QWEATHER_KEY(API KEY)
#   - 认证: X-QW-Api-Key 请求头, 响应 gzip 压缩
# 旧版兼容: 只设 QWEATHER_KEY → devapi.qweather.com + ?key= 参数
QWEATHER_HOST = os.environ.get("QWEATHER_HOST", "").strip()
QWEATHER_KEY = os.environ.get("QWEATHER_KEY", "").strip()
_USE_NEW_API = bool(QWEATHER_HOST and QWEATHER_KEY)
_ssl_ctx = ssl.create_default_context()

_GEO_CACHE = {}    # city_name -> location_id (永不过期, 城市不变)
_WX_CACHE = {}     # cache_key -> (text, timestamp)
_WX_TTL = 3600     # 天气缓存 1 小时

def _wx_headers():
    """新版 API 认证头; 旧版无额外头 (key 走 URL 参数)"""
    return {"X-QW-Api-Key": QWEATHER_KEY} if _USE_NEW_API else {}

def _wx_url(path, query):
    """按 API 版本拼 URL"""
    if _USE_NEW_API:
        return "https://%s%s?%s" % (QWEATHER_HOST, path, query)
    return "https://devapi.qweather.com%s?%s&key=%s" % (path, query, QWEATHER_KEY)

def _http_get_json(url, headers=None):
    h = {"User-Agent": "xiaoZhi-KS/1.0", "Accept-Encoding": "gzip"}
    if headers:
        h.update(headers)
    req = urllib.request.Request(url, headers=h)
    with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx) as r:
        raw = r.read()
        if r.headers.get("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
        return json.loads(raw.decode("utf-8"))

def _geocode(city):
    """城市名 → 和风 location_id (带永久缓存)"""
    if city in _GEO_CACHE:
        return _GEO_CACHE[city]
    if not QWEATHER_KEY:
        return None
    q = urllib.parse.quote(city)
    if _USE_NEW_API:
        url = "https://%s/geo/v2/city/lookup?location=%s&number=1" % (QWEATHER_HOST, q)
    else:
        url = ("https://geoapi.qweather.com/v2/city/lookup"
               "?location=%s&key=%s&number=1" % (q, QWEATHER_KEY))
    try:
        d = _http_get_json(url, _wx_headers())
        if d.get("code") == "200" and d.get("location"):
            cid = d["location"][0]["id"]
            _GEO_CACHE[city] = cid
            print("[Weather] geocode %s -> %s" % (city, cid))
            return cid
    except Exception as e:
        print("[Weather] geocode fail (%s): %s" % (city, e))
    return None

def _fetch_now(cid):
    return _http_get_json(_wx_url("/v7/weather/now", "location=%s" % cid), _wx_headers())

def _fetch_3d(cid):
    return _http_get_json(_wx_url("/v7/weather/3d", "location=%s" % cid), _wx_headers())

def _fetch_clothes(cid):
    return _http_get_json(_wx_url("/v7/indices/1d", "type=3&location=%s" % cid), _wx_headers())

def _format_weather(city, days, clothes, cid):
    """查和风 API → 格式化 ≤50 字播报文本"""
    now_d = _fetch_now(cid)
    if now_d.get("code") != "200":
        return city + "天气查询失败"
    now = now_d.get("now", {})
    cur_text = now.get("text", "未知")
    cur_temp = now.get("temp", "?")

    daily = []
    if days >= 1:
        try:
            dd = _fetch_3d(cid)
            if dd.get("code") == "200":
                daily = dd.get("daily", [])
        except Exception as e:
            print("[Weather] 3d fail: %s" % e)

    # 组装主体
    if days == 0:
        out = "%s现在%s，气温%s度" % (city, cur_text, cur_temp)
    elif days == 1 and daily:
        d = daily[0]
        out = "%s今天%s，%s到%s度" % (city, d.get("textDay", "未知"),
                                      d.get("tempMin", "?"), d.get("tempMax", "?"))
    elif days == 2 and len(daily) >= 2:
        d = daily[1]
        out = "%s明天%s，%s到%s度" % (city, d.get("textDay", "未知"),
                                      d.get("tempMin", "?"), d.get("tempMax", "?"))
    elif days == 3 and len(daily) >= 3:
        d = daily[2]
        out = "%s后天%s，%s到%s度" % (city, d.get("textDay", "未知"),
                                      d.get("tempMin", "?"), d.get("tempMax", "?"))
    else:
        out = "%s现在%s，气温%s度" % (city, cur_text, cur_temp)

    # 穿衣建议
    if clothes:
        try:
            id_ = _fetch_clothes(cid)
            if id_.get("code") == "200" and id_.get("daily"):
                cat = id_["daily"][0].get("category", "")
                if cat:
                    out += "，建议" + _clothes_short(cat, cur_temp)
        except Exception as e:
            print("[Weather] clothes fail: %s" % e)

    # 截断 ≤50 字符 (TTS 限制)
    if len(out) > 50:
        out = out[:50]
    return out

def _clothes_short(category, temp):
    """穿衣指数 category → 精简口语建议"""
    mapping = {
        "寒冷": "穿厚羽绒服",
        "冷": "穿厚外套",
        "较冷": "穿外套加毛衣",
        "较舒适": "穿长袖加薄外套",
        "舒适": "穿长袖就行",
        "热": "穿短袖薄衫",
        "炎热": "穿短袖短裤",
    }
    return mapping.get(category, "穿短袖")

def fetch_weather_text(city, days=1, clothes=False):
    """主入口: 城市 + 天数 + 穿衣 → ≤50字文本"""
    if not QWEATHER_KEY:
        return "天气服务未配置，请设置和风天气key"
    if _USE_NEW_API:
        pass  # 新版: HOST + KEY 齐备
    cid = _geocode(city)
    if not cid:
        return "找不到%s的天气" % city

    ck = "%s:%d:%d" % (cid, days, clothes)
    now = time.time()
    cached = _WX_CACHE.get(ck)
    if cached and now - cached[1] < _WX_TTL:
        print("[Weather] cache hit: %s" % ck)
        return cached[0]

    try:
        text = _format_weather(city, days, clothes, cid)
        _WX_CACHE[ck] = (text, now)
        print("[Weather] %s -> %s" % (ck, text))
        return text
    except Exception as e:
        print("[Weather] fetch fail: %s" % e)
        return "%s天气查询失败" % city

# ════════════════════════════════════════════════════════════
#  P8e: LLM 联网问答 (B+C 合一 — web_search 兜底 + function calling 精确层)
# ════════════════════════════════════════════════════════════
ZHIPU_API_KEY = os.environ.get("ZHIPU_API_KEY", "").strip()
if not ZHIPU_API_KEY:
    # 兼容: ESP32 固件里硬编码了 key, 网关也硬编码一份 (同一 key)
    ZHIPU_API_KEY = "0d90e4ca283f4d629649ec59b579c219.UYGhpdb7Gb8F2HbA"
ZHIPU_LLM_URL = "https://open.bigmodel.cn/api/paas/v4/chat/completions"
ZHIPU_MODEL = "glm-4-flash"

# 系统提示 — 和 ESP32 端 llm_client.cpp 的 LLM_SYSTEM_PROMPT 同款约束 (≤50字/不emoji/口语化)
# 2026-08-19 用户定 C+B: 强制 LLM 主动短答 (≤40字核心结论), 截断尾巴换自然收尾
# ── 地名听错纠错表 (STT 同音错词 → 正确地名, 2026-08-19 真机实测) ──
# 百度 STT 常把"扎尕那"听成"扎嘎纳/扎塔娜"。请求进 LLM 前先替换,
# 保证 web_search / function calling 拿到正确的地名 (治本, 不依赖百度热词)
_PLACE_FIX = {
    "扎嘎纳": "扎尕那",
    "扎塔娜": "扎尕那",
    "扎喀纳": "扎尕那",
    "扎噶纳": "扎尕那",
    "札格纳": "扎尕那",   # 2026-08-19 真机实测: STT 听出"札格纳"
    "札嘎那": "扎尕那",
    "扎尕那": "扎尕那",   # 正确写法兜底 (无变化)
    # 随实测补充更多: 五彩丹霞→张掖丹霞 等
}

# ── 读音近似匹配: 常用同音字组 (声母韵母相同/极近) ──
# 覆盖候选地名里每个字的常见 STT 错字。命中任意变体都能纠正, 不用穷举错词
_HOMOPHONE_GROUPS = {
    "扎": "扎渣札",   "尕": "尕嘎噶尬伽格", "那": "那娜纳哪拿",
    "五": "五武舞",   "彩": "彩采菜",     "丹": "丹单担",   "霞": "霞侠狭",
    "张": "张章彰樟", "掖": "掖页夜液业",
    "迭": "迭叠蝶碟", "部": "部布步不",
    "嘉": "嘉佳加家", "峪": "峪欲玉育",   "关": "关观官冠",
    "敦": "敦蹲吨",   "煌": "煌皇黄簧",
    "莫": "莫漠寞墨", "高": "高膏糕",     "窟": "窟哭枯",
    "鸣": "鸣明名",   "沙": "沙纱砂鲨",   "山": "山衫删",
    "月": "月越阅悦", "牙": "牙芽呀鸦",   "泉": "泉全权拳",
    "西": "西希吸溪", "安": "安按案暗",
    "兵": "兵冰宾",   "马": "马码玛",     "俑": "俑勇涌永",
    "大": "大达答",   "雁": "雁燕艳焰",   "塔": "塔他她它",
    "华": "华花划滑", "清": "清青轻倾",   "池": "池迟持尺",
    "法": "法发罚",   "门": "门们闷",     "寺": "寺四似伺",
    "壶": "壶胡湖糊", "口": "口扣寇",     "瀑": "瀑铺扑",   "布": "布部不步",
    "延": "延严言盐",
    "宝": "宝保堡饱", "鸡": "鸡机基积",
    "咸": "咸闲贤衔", "阳": "阳杨羊洋",
    "渭": "渭位为未", "南": "南男难楠兰蓝篮",
    "汉": "汉汗翰瀚", "中": "中忠钟终",
    "康": "康扛慷",
    "商": "商伤上赏", "洛": "洛落络乐",
    "铜": "铜同童彤", "川": "川穿传串",
    "陇": "陇龙笼隆",
    "天": "天田填甜", "水": "水谁睡税",
    "平": "平评瓶萍", "凉": "凉良量梁",
    "庆": "庆轻清青",
    "定": "定丁钉顶",
    "甘": "甘干竿",
    "临": "临林邻淋", "夏": "夏下吓",
    "青": "青清轻倾", "海": "海嗨害",     "湖": "湖胡壶糊",
    "茶": "茶查察叉", "卡": "卡咔咖",     "盐": "盐言严沿",
    "祁": "祁奇齐棋", "连": "连联莲帘",
    "源": "源元圆原园",
    "格": "格个各哥", "尔": "尔二耳儿",   "木": "木目穆幕",
    "玉": "玉欲育愈", "树": "树数术束",
    "可": "可克客刻", "里": "里理李礼",
    "肃": "肃速宿苏",
    "和": "和河何合", "政": "政正郑证", "临": "临林邻淋",
}

# ── 景区/景点 → 行政区映射 (和风天气 GeoAPI 只认城市/区县, 不认景区名) ──
# 2026-08-19: "扎尕那天气" → get_weather(city=扎尕那) → GeoAPI 400 → 查不到
# 映射后查所属县/市, 天气数据才拿得到
_SCENIC_TO_CITY = {
    "扎尕那": "迭部",
    "迭部": "迭部",
    "五彩丹霞": "临泽",
    "七彩丹霞": "临泽",
    "丹霞": "临泽",
    "莫高窟": "敦煌",
    "鸣沙山": "敦煌",
    "月牙泉": "敦煌",
    "兵马俑": "临潼",
    "秦始皇陵": "临潼",
    "大雁塔": "西安",
    "华清池": "临潼",
    "华山": "华阴",
    "法门寺": "扶风",
    "壶口瀑布": "吉县",
    "茶卡盐湖": "乌兰",
    "青海湖": "共和",
    "祁连山": "祁连",
    "门源": "门源",
    "可可西里": "格尔木",
    "玉树": "玉树",
}

# 候选地名 (与固件 BAIDU_HOTWORDS 对齐; 读音匹配的比对目标)
_PLACE_CANDIDATES = [
    "扎尕那", "五彩丹霞", "张掖", "迭部", "嘉峪关", "敦煌", "莫高窟",
    "鸣沙山", "月牙泉", "西安", "兵马俑", "大雁塔", "华清池", "华山",
    "法门寺", "壶口瀑布", "延安", "宝鸡", "咸阳", "渭南", "汉中",
    "安康", "商洛", "铜川", "陇南", "天水", "平凉", "庆阳", "定西",
    "甘南", "临夏", "青海湖", "茶卡盐湖", "祁连山", "门源", "格尔木",
    "玉树", "可可西里", "和政", "临潭", "卓尼", "玛曲", "碌曲", "夏河",
    "积石山", "东乡", "广河", "康乐", "永靖",
]

def _homophone(a, b):
    """两字是否同音: 相同 或 在同一个同音字组里"""
    if a == b:
        return True
    ga = _HOMOPHONE_GROUPS.get(a, "")
    gb = _HOMOPHONE_GROUPS.get(b, "")
    return (b in ga) or (a in gb)

def _place_similarity(text_word, cand):
    """词与候选地名的读音相似度 0.0~1.0 (逐字同音比例, 长度必须一致)"""
    if len(text_word) != len(cand) or len(text_word) < 2:
        return 0.0
    hit = sum(1 for a, b in zip(text_word, cand) if _homophone(a, b))
    return hit / len(cand)

def _fix_place_names(text):
    """把用户文本里的听错地名替换成正确的 — 双引擎:
    1) 精确纠错表 (已知错词)
    2) 读音近似匹配 (任意同音变体, 如"扎噶娜"→"扎尕那")
    """
    # 引擎1: 精确纠错表
    for wrong, right in _PLACE_FIX.items():
        if wrong in text:
            fixed = text.replace(wrong, right)
            print("[Ask] 地名纠错(精确): %s -> %s" % (wrong, right))
            return fixed
    # 引擎2: 读音近似匹配 — 文本里每个与候选等长的连续片段, 逐字比同音
    best = None  # (score, cand, start, seg)
    n = len(text)
    for cand in _PLACE_CANDIDATES:
        clen = len(cand)
        if clen < 2:
            continue
        for i in range(n - clen + 1):
            seg = text[i:i + clen]
            if seg == cand:      # 正确写法, 无需替换
                continue
            sim = _place_similarity(seg, cand)
            if sim >= 0.6 and (best is None or sim > best[0]):
                best = (sim, cand, i, seg)
    if best:
        sim, cand, i, seg = best
        fixed = text[:i] + cand + text[i + len(seg):]
        print("[Ask] 地名纠错(读音 %.2f): %s -> %s" % (sim, seg, cand))
        return fixed
    return text

# ── 指代消解: "这个地方/那儿/这边" → 历史里刚讨论的地名 (2026-08-19 真机实测) ──
# 用户问完"XX在什么地方"后追问"这个地方的天气怎么样", LLM 常无法消解指代。
# 网关直接从最近的 history 内容里提取候选地名, 程序化替换, 不依赖 LLM。
_REFER_WORDS = ("这个地方", "那个地方", "这儿", "那边", "这边", "那地方")

def _resolve_place_reference(text, history):
    """把文本里的指代词替换成历史中最近提到的地名。返回 (新文本, 是否替换)"""
    if not any(k in text for k in _REFER_WORDS):
        return text, False
    if not history or not isinstance(history, list):
        return text, False
    # 从最近的对话往前找: 先看 assistant 回复, 再看 user 问题
    for pair in reversed(history):
        for content in reversed(pair):
            if not isinstance(content, str):
                continue
            for cand in _PLACE_CANDIDATES:
                if cand in content:
                    new_text = text
                    for ref in _REFER_WORDS:
                        new_text = new_text.replace(ref, cand)
                    print("[Ask] 指代消解: %r -> %s" % (text, cand))
                    return new_text, True
    return text, False

# 指代问词: "列举一下哪十个""有哪些""那几个" 等 (无明确地名时的追问)
_REFER_QUERY_WORDS = ("哪", "几个", "哪些", "那些", "这些", "列举", "都有", "说说")

def _inject_place_from_history(text, history):
    """文本无候选地名但含指代问词 → 从 history 最近对话提取地名+主题拼进文本。
    修"西安有几个区" → "列举一下哪十个?" LLM 答"不确定" (它不敢列又不搜索)。
    补全成 "关于西安的区：列举一下哪十个?" → 含地名 → 丢 history → 强制搜索 → 列区名。
    """
    import re
    if any(cand in text for cand in _PLACE_CANDIDATES):
        return text
    if not any(k in text for k in _REFER_QUERY_WORDS):
        return text
    if not history or not isinstance(history, list):
        return text
    for pair in reversed(history):
        for content in reversed(pair):
            if not isinstance(content, str):
                continue
            for cand in _PLACE_CANDIDATES:
                if cand in content:
                    # 提取主题词: "西安有10个区" → "区"; "有3个机场" → "机场"
                    topic = ""
                    m = re.search(r'\d+\s*个([^\s，。！？]{1,4})', content)
                    if m:
                        topic = m.group(1)
                    full = "关于%s" % cand
                    if topic and topic not in text:
                        full += "的%s" % topic
                    print("[Ask] 指代补全地名: %r -> %s" % (text, full))
                    return full + "：" + text
    return text

_ASK_SYSTEM_PROMPT = (
    '你是"小智"，一个桌面AI情感伴侣。回复必须精炼：'
    '  · 无论问题多复杂（知识解释/评价/列举/对比），先给不超过40字的核心结论，禁止展开长篇。'
    '  · 一句话能说完就别用两句；列要点最多2条，每条不超过10字。'
    '  · 铁律上限：任何情况下绝对不能超过100字（ESP32 TTS 缓冲区限制）。'
    '语气口语化、自然、俏皮。绝对禁止emoji和表情符号。不要解释你是AI，不要长篇大论。'
    '【重要规则】搜索不到或不确定的信息，直接说"这个我不确定"或"没查到这个地方"，绝不编造地名、城市、数字、天气。'
    '【指代消解】如果用户说"这个地方""那儿""这边"等指代词，指的是对话历史中刚讨论的地点，直接用该地名回答问题（如查天气、查位置），不要反问"哪个地方"。'
    '【新地名优先】用户明确说出一个地名（如"和政""兰州"）时，即使它与历史中的地点不同，也必须以用户说的地名为准查询（get_weather/web_search），禁止用历史里的地点（如迭部）替代。用户说新地名就是换话题了。'
    '【列举/展开请求】用户要求"列举""有哪些""哪几个""说说""展开"时，必须调用web_search搜索后给出具体内容列表，禁止用"不确定""不知道"敷衍。只有搜索完全无结果才能说"不确定"。列举最多列5项，用顿号分隔，务必控制在100字内。'
    '【地名听错处理】当用户问的地点/景区听起来像听错的（如「扎嘎纳」可能其实是「扎尕那」），禁止反问！'
    '  直接调用web_search搜索用户说的原词（哪怕听上去不像真地名），搜索结果会给你正确的地名和答案。'
    '  搜到了就直接回答正确信息（如"扎尕那在甘肃迭部"）。只有搜索完全无结果时才说"没查到这个地方"。'
    '【否定追问处理】如果用户否定你上一轮的猜测（说"不是""不对""说错了"），不要重复同一个猜测！'
    '  结合对话历史，用web_search重新搜索用户最初问的原话；或者直接说"那你说的是哪里，我再查查"。'
    '【新闻查询】用户问"最近新闻""最近发生了什么""热点""时事"等，必须调用web_search搜索最新信息后给出口语化摘要（1到2条核心即可），禁止说"我不关注新闻"或"请去看新闻App"。'
    '【百科查询】用户问"XX是什么""XX什么意思""介绍一下XX"等，必须调用web_search搜索后给出简洁解释（一句话定义+一句话特点），禁止说"我不知道"或"请去查百科"。'
    '【景点查询】用户问"XX景区门票""XX开放时间""XX好玩吗""XX攻略"等，必须调用web_search搜索后给出核心实用信息（门票/开放时间/特色），禁止敷衍。'
    '【股票查询】用户问股价/大盘/涨跌/行情时，必须调用get_stock工具获取实时数据，禁止凭记忆编造数字。'
    '【重要规则】当有可用的function工具时，必须优先调用工具获取具体数据，而不是用web_search泛泛搜索。'
    '禁止回复"请查询XX官网""请自行查"等推脱用语——用户要的是直接的数据结果，不是指路。'
    '工具返回的数据要直接播报给用户，不要添加"具体请去官网核实"之类的尾巴。'
)

# Function calling: 天气查询 tool 定义 (C 层 — 精确真数据, 比 web_search 搜天气更准)
_WEATHER_TOOL = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "【查天气/穿衣/几度/下雨/冷热时必须优先调用此工具】获取指定城市的天气信息，包括今天/明天/后天的预报和穿衣建议。当用户问天气相关问题时，必须调用此工具获取真实气象数据，禁止用web_search泛泛搜索。",
        "parameters": {
            "type": "object",
            "properties": {
                "city": {"type": "string", "description": "城市名称，如北京、西安、上海"},
                "days": {"type": "integer", "description": "预报天数：1=今天 2=明天 3=后天，默认1"},
                "clothes": {"type": "boolean", "description": "是否需要穿衣建议，默认false"}
            },
            "required": ["city"]
        }
    }
}

# ── 12306 火车查询 (C 层 — 精确车次, 比 web_search 搜火车更准) ──

# 常用车站 电报码 (城市名→主要站; 12306 用电报码而非城市名查询)
_STATION_MAP = {
    "西安": "XAY",  "西安北": "EAY",
    "北京": "BJP",  "北京西": "BXP", "北京南": "VNP", "北京北": "VAP",
    "上海": "SHH",  "上海虹桥": "AOH", "上海南": "SNH",
    "广州": "IZQ",  "广州南": "IZW",
    "深圳": "SZQ",  "深圳北": "IOQ",
    "成都": "ICW",  "成都东": "ICW",
    "重庆": "CUW",  "重庆北": "WUW",
    "武汉": "WUH",  "汉口": "HKN",
    "郑州": "ZZF",  "郑州东": "ZAF",
    "杭州": "HZH",  "杭州东": "HGH",
    "南京": "NJS",  "南京南": "NKH",
    "天津": "TJP",  "天津西": "TXP",
    "长沙": "CWQ",  "石家庄": "SJP",
    "兰州": "LAJ",  "宝鸡": "BJY2",
    "咸阳": "XYY2", "宝鸡南": "BNY2",
}

# 日期缓存 (避免12306频繁请求; 查询结果缓存10分钟)
_train_cache = {}

def _resolve_station(name):
    """城市名/站名 → 12306 电报码"""
    name = name.strip()
    if name in _STATION_MAP:
        return _STATION_MAP[name]
    # 去掉"站"字再试
    if name.endswith("站"):
        short = name[:-1]
        if short in _STATION_MAP:
            return _STATION_MAP[short]
    # 去掉"市"字再试
    if name.endswith("市"):
        short = name[:-1]
        if short in _STATION_MAP:
            return _STATION_MAP[short]
    return None

_12306_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
_12306_INIT_URL = "https://kyfw.12306.cn/otn/leftTicket/init"
# 复用同一个 opener + cookie jar (进程内多次调用共享 cookie, 避免反复拿)
_12306_opener = None

def _get_12306_opener():
    """首次访问 init 拿 JSESSIONID, 后续用带 cookie 的 opener 查询
    12306 反爬: 不带 cookie 的 queryZ 被重定向到 error.html"""
    global _12306_opener
    if _12306_opener:
        return _12306_opener
    import http.cookiejar
    cj = http.cookiejar.CookieJar()
    op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
    op.addheaders = [
        ("User-Agent", _12306_UA),
        ("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
        ("Accept-Language", "zh-CN,zh;q=0.9"),
    ]
    try:
        op.open(_12306_INIT_URL, timeout=10)
        print("[Train] init ok, cookies: %s" % [(c.name, c.value[:16]) for c in cj])
    except Exception as e:
        print("[Train] init err: %s (将继续尝试)" % e)
    _12306_opener = op
    return op

def fetch_trains_text(from_city, to_city, date_str=""):
    """查 12306 车次 → ≤50字纯文本播报
    from_city/to_city: 城市名或站名 (如 '西安' / '北京' / '西安北')
    date_str: YYYY-MM-DD 或空(默认今天)
    """
    from_code = _resolve_station(from_city)
    to_code = _resolve_station(to_city)
    if not from_code:
        return "找不到出发站 %s" % from_city
    if not to_code:
        return "找不到目的地 %s" % to_city

    if not date_str:
        date_str = "today"

    # 自然语言日期 → YYYY-MM-DD (LLM 传的 date 可能是 "tomorrow"/"明天"/"后天" 或 YYYY-MM-DD)
    today = datetime.datetime.now()
    date_lower = str(date_str).lower().strip()
    if date_lower in ("today", "今天", "now", ""):
        date_str = today.strftime("%Y-%m-%d")
    elif date_lower in ("tomorrow", "明天"):
        date_str = (today + datetime.timedelta(days=1)).strftime("%Y-%m-%d")
    elif date_lower in ("after_tomorrow", "后天", "後天"):
        date_str = (today + datetime.timedelta(days=2)).strftime("%Y-%m-%d")
    else:
        # 尝试直接用 (YYYY-MM-DD), 失败再尝试其他格式
        try:
            datetime.datetime.strptime(date_str, "%Y-%m-%d")
        except ValueError:
            print("[Train] cannot parse date '%s', falling back to today" % date_str)
            date_str = today.strftime("%Y-%m-%d")

    # 缓存 key (10分钟有效)
    cache_key = "%s-%s-%s" % (date_str, from_code, to_code)
    now = time.time()
    if cache_key in _train_cache and now - _train_cache[cache_key]["ts"] < 600:
        print("[Train] cache hit: %s" % cache_key)
        return _train_cache[cache_key]["text"]

    url = ("https://kyfw.12306.cn/otn/leftTicket/queryZ"
           "?leftTicketDTO.train_date=%s&leftTicketDTO.from_station=%s"
           "&leftTicketDTO.to_station=%s&purpose_codes=ADULT"
           % (date_str, from_code, to_code))
    print("[Train] 12306: %s → %s (%s)" % (from_city, to_city, date_str))

    # 用带 cookie 的 opener (反爬规避)
    op = _get_12306_opener()
    op.addheaders = [
        ("User-Agent", _12306_UA),
        ("Accept", "*/*"),
        ("Accept-Language", "zh-CN,zh;q=0.9"),
        ("Referer", _12306_INIT_URL),
        ("X-Requested-With", "XMLHttpRequest"),
    ]
    try:
        with op.open(url, timeout=12) as r:
            raw = r.read()
            if r.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            data = json.loads(raw.decode("utf-8"))
    except Exception as e:
        print("[Train] 12306 error: %s" % e)
        # 如果失败, 重置 opener 下次重新拿 cookie
        global _12306_opener
        _12306_opener = None
        return "查火车出了点问题，网络不太好"

    results = data.get("data", {}).get("result", [])
    if not results:
        return "没有 %s 到 %s 的直达车" % (from_city, to_city)

    # 解析车次, 取前 5 趟, 格式: 车次 出发→到达 历时
    # 12306 返回格式: 用 | 分隔的长字符串, 各字段位置固定
    # [3]=车次 [8]=出发时间 [9]=到达时间 [10]=历时
    trains = []
    for row in results:
        parts = row.split("|")
        if len(parts) < 11:
            continue
        code = parts[3]       # 车次 (G123/D4567/K89...)
        dep   = parts[8]      # 出发时间
        arr   = parts[9]      # 到达时间
        dur   = parts[10]     # 历时
        if not dep or not arr or dep == "----":
            continue
        trains.append("%s %s-%s %s" % (code, dep, arr, dur))
        if len(trains) >= 3:
            break

    if not trains:
        return "%s到%s没有可订车次" % (from_city, to_city)

    # 拼成口语化短句 (≤100字: 车次格式每条约15字, 3条=45字+前后缀)
    text = "%s到%s有" % (from_city, to_city)
    for i, t in enumerate(trains):
        text += t
        if i < len(trains) - 1:
            text += "，"
    text += "等"
    # 超长截断 (保持 UTF-8 完整)
    if len(text) > 100:
        text = text[:98] + "等"
    print("[Train] Result: %s" % text)
    _train_cache[cache_key] = {"ts": now, "text": text}
    return text

# ════════════════════════════════════════════════════════════
#  股票查询 (腾讯财经接口, 免费无认证)
# ════════════════════════════════════════════════════════════
_STOCK_MAP = {
    "上证指数": "sh000001", "大盘": "sh000001", "沪市": "sh000001",
    "深证成指": "sz399001", "深市": "sz399001",
    "创业板指": "sz399006", "创业板": "sz399006",
    "贵州茅台": "sh600519", "茅台": "sh600519",
    "中国平安": "sh601318", "平安": "sh601318",
    "比亚迪": "sz002594",
    "宁德时代": "sz300750",
    "腾讯": "hk00700", "腾讯控股": "hk00700",
    "阿里巴巴": "hk09988", "阿里": "hk09988",
    "美团": "hk03690",
}

def _resolve_stock_code(name):
    """股票名/别名 → 腾讯财经代码 (sh/sz/hk/us 前缀)"""
    name = name.strip()
    if name in _STOCK_MAP:
        return _STOCK_MAP[name]
    # 直接传带前缀代码 (sh600519 / sz000001 / hk00700 / usAAPL)
    import re as _re
    if _re.fullmatch(r"(?:sh|sz|hk|us)[0-9A-Za-z]+", name.lower()):
        return name.lower()
    # 如果用户直接给代码如 "600519"，尝试加前缀
    if name.isdigit():
        if len(name) == 6:
            # 6位数字: 上海(sh) 或 深圳(sz)；简单规则: 6开头=上海, 0/3开头=深圳
            if name.startswith('6'):
                return "sh" + name
            else:
                return "sz" + name
    # 去掉"股票""股份"后缀再试
    for suffix in ("股票", "股份", "集团", "控股"):
        if name.endswith(suffix):
            short = name[:-len(suffix)]
            if short in _STOCK_MAP:
                return _STOCK_MAP[short]
    return None

# 指数代码 (sh000001 上证 / sz399001 深成 / sz399006 创业板) — 播报用"点"不用"元"
_INDEX_CODES = {"sh000001", "sz399001", "sz399006", "sh000300", "sz399905"}

def fetch_stock_text(query):
    """查股票实时行情 → ≤50字纯文本播报
    query: 股票名称或代码 (如 '茅台' / '比亚迪' / 'sh600519')
    """
    code = _resolve_stock_code(query)
    if not code:
        return "找不到股票 %s" % query

    url = "https://qt.gtimg.cn/q=%s" % code
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "xiaoZhi-KS/1.0"})
        with urllib.request.urlopen(req, timeout=10, context=_ssl_ctx) as r:
            raw = r.read().decode("gbk", errors="replace")  # 腾讯接口返回 GBK
    except Exception as e:
        print("[Stock] fetch err: %s" % e)
        return "股票查询失败，网络不太好"

    # 解析: v_sh600519="1~贵州茅台~600519~1675.00~1660.00~...";
    for line in raw.split(";"):
        line = line.strip()
        if not line or "~" not in line:
            continue
        # 去掉 v_sh600519=" ... " 外壳
        body = line.split('="', 1)[-1].rstrip('"')
        parts = body.split("~")
        if len(parts) < 40:
            continue
        name = parts[1]           # 股票名称
        # parts[3]=当前价 parts[4]=昨收
        try:
            price = float(parts[3])
            close = float(parts[4])
            change_pct = (price - close) / close * 100.0 if close else 0.0
        except (ValueError, IndexError):
            continue
        direction = "涨" if change_pct >= 0 else "跌"
        unit = "点" if code in _INDEX_CODES else "元"
        text = "%s%.2f%s，%s%.2f%%" % (name, price, unit, direction, abs(change_pct))
        print("[Stock] %s -> %s" % (query, text))
        return text
    return "没查到 %s 的行情" % query

# Function calling: 火车查询 tool 定义 (C 层 — 精确车次, 比 web_search 搜火车更准)
_TRAIN_TOOL = {
    "type": "function",
    "function": {
        "name": "get_trains",
        "description": "【火车/车次/买票/出行问题,必须优先调此工具,禁止说请去12306查】查询12306官方火车车次,获取两站之间直达车次、出发到达时间和历时。用户说'查火车''有什么车''车次''去XX坐什么车'时,必须调用此工具直接返回车次列表。date参数支持英文today/tomorrow/中文今天/明天/后天/或者YYYY-MM-DD。",
        "parameters": {
            "type": "object",
            "properties": {
                "from_city": {"type": "string", "description": "出发城市或车站名称，如西安、北京、上海、广州"},
                "to_city": {"type": "string", "description": "目的城市或车站名称，如北京、上海、西安"},
                "date": {"type": "string", "description": "出发日期 YYYY-MM-DD 格式，省略则查询今天"}
            },
            "required": ["from_city", "to_city"]
        }
    }
}

# Function calling: 股票查询 tool 定义 (C 层 — 精确实时行情)
_STOCK_TOOL = {
    "type": "function",
    "function": {
        "name": "get_stock",
        "description": "【股票/股价/大盘/涨跌/行情问题,必须优先调此工具,禁止编造数字】查询股票实时行情,获取当前价格、涨跌幅度。支持股票名称(如茅台、比亚迪)或代码(如sh600519)。",
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "股票名称或代码，如茅台、比亚迪、上证指数、sh600519"}
            },
            "required": ["query"]
        }
    }
}

# Web search tool 定义 (B 层 — 兜底, 什么都搜得到)
_WEB_SEARCH_TOOL = {
    "type": "web_search",
    "web_search": {"enable": True}
}

def _glm_post(data):
    """调 GLM-4-Flash API (POST JSON)"""
    body = json.dumps(data, ensure_ascii=False).encode("utf-8")
    headers = {
        "Content-Type": "application/json",
        "Authorization": "Bearer " + ZHIPU_API_KEY,
    }
    req = urllib.request.Request(ZHIPU_LLM_URL, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30, context=_ssl_ctx) as r:
            raw = r.read()
            if r.headers.get("Content-Encoding") == "gzip":
                raw = gzip.decompress(raw)
            return json.loads(raw.decode("utf-8"))
    except Exception as e:
        print("[Ask] GLM POST fail: %s" % e)
        return None

def ask_llm(user_text, history=None, memory=""):
    """B+C 合一: web_search 兜底 + get_weather function calling
    返回 ≤50 字纯文本 (UTF-8)
    history: ESP32 传来的对话历史, [[user1, assistant1], [user2, assistant2], ...]
    memory:  P10 长期记忆 (ESP32 SD 卡"记住XXX"条目, 多行), 注入 system prompt
    """
    if not user_text or not user_text.strip():
        return "我没听清，请再说一遍"

    # v1w: 地名听错纠错 — 进 LLM 前先把"扎嘎纳"这类错词替换成"扎尕那"
    user_text = _fix_place_names(user_text)
    # v1w: 指代消解 — "这个地方的天气" → "扎尕那的天气" (结合历史)
    user_text, _ = _resolve_place_reference(user_text, history)
    # v1w: 指代补全 — "列举一下哪十个?" → "关于西安：列举一下哪十个?" (无地名追问)
    user_text = _inject_place_from_history(user_text, history)
    # v1w(修): 地名已明确 → 丢弃 history, 防 LLM 被旧话题带偏!
    #   "本周日临夏天气" 曾答成 history 里的"和政" — prompt 拦不住, 直接不给它历史
    #   指代词消解后文本已含地名, 同样不需要 history; 只有"不是""那天气呢"这种无地名
    #   追问才保留 history (LLM 需结合上文)
    if history:
        if any(cand in user_text for cand in _PLACE_CANDIDATES):
            print("[Ask] 文本含明确地名, 丢弃 history 防干扰")
            history = None

    # P10: 长期记忆注入 system prompt (ESP32 SD 卡条目)
    sys_prompt = _ASK_SYSTEM_PROMPT
    if memory and str(memory).strip():
        sys_prompt += ("【长期记忆】以下是你长期记住的、用户亲口告诉你的信息，"
                       "回答时作为事实参考（例如称呼用户、用户的偏好等），不要质疑：\n"
                       + str(memory).strip()[:800])   # 上限 800 字符防 prompt 爆炸
        print("[Ask] memory injected (%d chars)" % len(str(memory).strip()))

    messages = [
        {"role": "system", "content": sys_prompt},
    ]

    # v1w: 注入历史对话 (最多 2 轮, ESP32 端已做淘汰)
    if history and isinstance(history, list):
        for pair in history:
            if isinstance(pair, (list, tuple)) and len(pair) >= 2:
                messages.append({"role": "user", "content": str(pair[0])})
                messages.append({"role": "assistant", "content": str(pair[1])})

    messages.append({"role": "user", "content": user_text})

    # 同时传 web_search (兜底) + get_weather (精确天气) + get_trains (精确车次) — LLM 自行选择
    # 智能优先级: 用户文本命中 function 关键词时, 第一轮只放 function tool 强迫 LLM 调
    #   (LLM 在 web_search 和 function 同时可选时不稳定, 时常选 web_search 搜出泛泛结论)
    #   没命中关键词的, 按常规全放 (function + web_search) — 真问别的就 web_search 兜底
    text_lower = user_text.lower()
    # ⚠️ 不能用裸"票" — "门票/发票/机票"全误命中火车 (2026-08-20 真机发现)
    has_train_kw = any(k in user_text for k in ("火车", "车次", "高铁", "动车", "买票", "坐火车", "K车", "坐什么", "出行"))
    has_weather_kw = any(k in user_text for k in ("天气", "几度", "下雨", "冷", "热", "穿什么", "穿衣", "气温", "预报"))
    # v1w: 地点/景区查询 (在哪/哪里/什么地方/哪个城市) — 强制 web_search 兜底
    #   STT 常听错地名, 只有 web_search 能搜到真实地点; 不加这句 LLM 会凭猜反问
    has_place_kw = any(k in user_text for k in ("在哪", "哪里", "什么地方", "哪个城市", "位于", "位置", "景区", "景点", "在什么"))
    # v1w: 列举/展开请求 (有哪些/哪几个/列举/说说) — 强制 web_search, 防"不确定"敷衍
    has_list_kw = any(k in user_text for k in ("列举", "有哪些", "哪几个", "哪些", "说说", "展开", "都有什么", "都有哪些"))
    # P8b/P8c: 新闻/百科/景点/股票 关键词路由
    has_news_kw = any(k in user_text for k in ("新闻", "热点", "时事", "头条", "最近发生", "发生了什么", "最近怎么样", "最新消息"))
    has_baike_kw = any(k in user_text for k in ("是什么", "什么意思", "介绍一下", "百科", "定义", "解释", "简介", "啥是"))
    has_scenic_kw = any(k in user_text for k in ("门票", "开放时间", "景区", "景点", "旅游", "好玩", "攻略", "值得去", "游记"))
    has_stock_kw = any(k in user_text for k in ("股票", "股价", "大盘", "涨跌", "指数", "A股", "港股", "美股", "行情", "茅台", "比亚迪"))
    if has_stock_kw:
        tools = [_STOCK_TOOL]  # 强制只股票 function
        print("[Ask] forced get_stock-only (stock keyword detected)")
    elif has_train_kw and not has_weather_kw:
        tools = [_TRAIN_TOOL]  # 强制只火车 function, 不放 web_search
        print("[Ask] forced get_trains-only (train keyword detected)")
    elif has_weather_kw and not has_train_kw:
        tools = [_WEATHER_TOOL]
        print("[Ask] forced get_weather-only (weather keyword detected)")
    elif has_news_kw:
        tools = [_WEB_SEARCH_TOOL]  # 新闻: 只放 web_search, 搜最新信息
        print("[Ask] forced web_search-only (news keyword detected)")
    elif has_baike_kw:
        tools = [_WEB_SEARCH_TOOL]  # 百科: 只放 web_search, 搜解释
        print("[Ask] forced web_search-only (baike keyword detected)")
    elif has_scenic_kw:
        tools = [_WEB_SEARCH_TOOL]  # 景点: 只放 web_search, 搜实用信息
        print("[Ask] forced web_search-only (scenic keyword detected)")
    elif has_place_kw:
        tools = [_WEB_SEARCH_TOOL]  # 地点查询: 只放 web_search, 强迫搜索原词
        print("[Ask] forced web_search-only (place keyword detected)")
    elif has_list_kw:
        tools = [_WEB_SEARCH_TOOL]  # 列举请求: 只放 web_search, 必须搜出具体内容
        print("[Ask] forced web_search-only (list keyword detected)")
    else:
        tools = [_WEB_SEARCH_TOOL, _WEATHER_TOOL, _TRAIN_TOOL, _STOCK_TOOL]

    data = {
        "model": ZHIPU_MODEL,
        "messages": messages,
        "tools": tools,
        "tool_choice": "auto",
        "temperature": 0.8,
        "max_tokens": 200,
    }

    print("[Ask] Q: %s (tools: %s)" % (user_text[:80], [t.get("type") if t.get("type") != "function" else t["function"]["name"] for t in tools]))
    resp = _glm_post(data)
    if not resp:
        return "这个问题我想不起来了，网络可能不太好"

    # 检查 API 错误
    if resp.get("error"):
        msg = resp["error"].get("message", "unknown")
        print("[Ask] GLM API error: %s" % msg)
        return "这个问题我答不上来"

    choices = resp.get("choices", [])
    if not choices:
        return "这个问题我答不上来"

    msg = choices[0].get("message", {})
    finish_reason = choices[0].get("finish_reason", "")

    # C 层: 模型选了 function calling → 执行后回传 → 再请求一轮
    if finish_reason == "tool_calls" and msg.get("tool_calls"):
        tool_calls = msg["tool_calls"]
        # 把 assistant 的 tool_calls 消息加回 history
        messages.append({
            "role": "assistant",
            "content": msg.get("content", ""),
            "tool_calls": tool_calls,
        })

        for tc in tool_calls:
            fn = tc.get("function", {})
            fn_name = fn.get("name", "")
            fn_args_raw = fn.get("arguments", "{}")
            tc_id = tc.get("id", "")
            print("[Ask] Function call: %s(%s)" % (fn_name, fn_args_raw))

            try:
                fn_args = json.loads(fn_args_raw) if isinstance(fn_args_raw, str) else (fn_args_raw or {})
            except Exception:
                fn_args = {}

            # 执行本地已知函数
            if fn_name == "get_weather":
                city = fn_args.get("city", "西安")
                days = int(fn_args.get("days", 1))
                clothes = bool(fn_args.get("clothes", False))
                # v1w: 景区→行政区映射 — 和风 GeoAPI 只认城市/区县, 不认景区名
                # 用户问"扎尕那天气"→ LLM 调 get_weather(city=扎尕那) → 映射到"迭部"才能查到
                if city in _SCENIC_TO_CITY:
                    print("[Weather] 景区映射: %s -> %s" % (city, _SCENIC_TO_CITY[city]))
                    city = _SCENIC_TO_CITY[city]
                tool_result = fetch_weather_text(city, days, clothes)
            elif fn_name == "get_trains":
                from_city = fn_args.get("from_city", "西安")
                to_city = fn_args.get("to_city", "北京")
                date_str = fn_args.get("date", "")
                tool_result = fetch_trains_text(from_city, to_city, date_str)
            elif fn_name == "get_stock":
                query = fn_args.get("query", "")
                tool_result = fetch_stock_text(query)
            else:
                tool_result = "未知的函数: %s" % fn_name

            print("[Ask] Function result: %s -> %s" % (fn_name, tool_result[:50]))
            messages.append({
                "role": "tool",
                "content": tool_result,
                "tool_call_id": tc_id,
            })

        # 第二轮请求: 把 tool 结果给 LLM → 生成自然语言播报
        data2 = {
            "model": ZHIPU_MODEL,
            "messages": messages,
            "tools": tools,
            "tool_choice": "auto",
            "temperature": 0.8,
            "max_tokens": 200,
        }
        resp2 = _glm_post(data2)
        if not resp2 or resp2.get("error"):
            return "查不到了，网络可能不太好"
        choices2 = resp2.get("choices", [])
        if not choices2:
            return "查不到了"
        reply = choices2[0].get("message", {}).get("content", "").strip()
    else:
        # B 层: web_search 兜底, 模型直接搜了生成回复
        reply = msg.get("content", "").strip()

    if not reply:
        return "这个问题我答不上来"

    reply = _strip_emoji(reply)
    reply = _strip_evade_phrase(reply)

    # ★ 字数兜底 (2026-08-19 用户定 C+B: prompt 已强制 ≤40字, 这里只做兜底):
    #   超 50 字 → 截到 48 + "……大概就是这样" (自然收尾, 不再问"换种说法")
    #   铁律: 全部 ≤100 字 (ESP32 TTS 缓冲区 CONV_TTS_MAX_CHARS=100)
    n = len(reply)
    if n > 50:
        reply = reply[:48] + "……大概就是这样"
    if len(reply) > 100:
        # 铁律兜底: ≥100 仍截 (防御性, 正常不会到这)
        reply = reply[:100]
    print("[Ask] A: %s" % reply[:60])
    return reply

def _strip_evade_phrase(text):
    """检测 LLM 的推脱尾巴 ("请去XX查""具体请查看官网") → 只砍尾巴, 保留前面的数据

    例: "明天西安到北京有K546、Z180等车次，具体时间表请查看12306官网"
        → "明天西安到北京有K546、Z180等车次"
    纯推脱无数据 → "这个我没查到具体数据，换个说法再问问我"
    """
    import re
    # 匹配推脱模式: "请查询XX官网" / "请去XX查" / "具体请查" / "请自行查"
    evade_patterns = [
        r"请.{0,4}查询.{0,2}官网",
        r"请.{0,2}查看.{0,6}官网",
        r"请.{0,2}去.{0,4}查",
        r"请.{0,2}自行.{0,2}查",
        r"请.{0,2}前往.{0,4}查",
        r"建议.{0,4}查询.{0,2}官网",
        r"具体.{0,6}请.{0,4}查",
        r"请.{0,2}访问.{0,4}查询",
        r"具体.{0,6}查.{0,2}官网",
        r"[可请建].{0,2}[登录|查询].{0,4}app",
    ]
    for p in evade_patterns:
        m = re.search(p, text)
        if m:
            # 砍掉从匹配处到句尾的尾巴, 保留前面数据
            cut = text[:m.start()].rstrip("，。；,.;！! ")
            if cut:
                return cut
            return "这个我没查到具体数据，换个说法再问问我"
    return text

def _strip_emoji(text):
    """剥除 emoji / 零宽字符 — 与 ESP32 端 stripEmoji() 同款逻辑"""
    out = []
    i = 0
    while i < len(text):
        c = ord(text[i])
        # 辅助平面 emoji (U+1F000~U+1FAFF)
        if 0x1F000 <= c <= 0x1FAFF:
            i += 1
            continue
        # 变体选择符 / 零宽字符
        if c in (0xFE0F, 0xFE0E, 0x200B, 0x200C, 0x200D, 0x2060):
            i += 1
            continue
        # 旗帜 emoji (U+1F1E6~U+1F1FF)
        if 0x1F1E6 <= c <= 0x1F1FF:
            i += 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)

# ════════════════════════════════════════════════════════════
#  HTTP 服务
# ════════════════════════════════════════════════════════════
class KSHandler(BaseHTTPRequestHandler):
    server_version = 'KnowledgeServer/1.0'

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _text(self, text, code=200, ctype='text/plain; charset=utf-8'):
        body = text.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _pcm(self, pcm_bytes, song_name):
        self.send_response(200)
        self.send_header('Content-Type', 'audio/x-pcm;rate=16000')
        self.send_header('Content-Length', str(len(pcm_bytes)))
        self.send_header('X-Song-Name', song_name.encode('utf-8'))
        self.end_headers()
        self.wfile.write(pcm_bytes)

    def _wav(self, pcm_bytes, song_name):
        wav = build_wav(pcm_bytes)
        self.send_response(200)
        self.send_header('Content-Type', 'audio/wav')
        self.send_header('Content-Length', str(len(wav)))
        self.send_header('X-Song-Name', song_name.encode('utf-8'))
        self.end_headers()
        self.wfile.write(wav)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        params = urllib.parse.parse_qs(parsed.query)

        if path == '/':
            self._text(_HTML, ctype='text/html; charset=utf-8')
        elif path == '/list':
            items = [{'name': s['name'], 'aliases': s['aliases'], 'bpm': s['bpm']}
                     for s in SONGS.values()]
            self._json({'count': len(items), 'songs': items,
                        'weather': 'available' if QWEATHER_KEY else 'no_key',
                        'weather_mode': 'new' if _USE_NEW_API else ('old' if QWEATHER_KEY else 'none')})
        elif path == '/music':
            q = params.get('q', [''])[0]
            song = find_song(q)
            if not song:
                self._json({'error': 'song not found', 'query': q,
                            'available': [s['name'] for s in SONGS.values()]}, 404)
                return
            pcm, _ = render_pcm(song)
            fmt = params.get('format', [''])[0].lower()
            if fmt == 'wav':
                self._wav(pcm, song['name'])
            else:
                self._pcm(pcm, song['name'])
        elif path == '/weather':
            city = params.get('city', ['西安'])[0]
            days = int(params.get('days', ['1'])[0])
            clothes = params.get('clothes', ['0'])[0] == '1'
            text = fetch_weather_text(city, days, clothes)
            self._text(text)
        else:
            self._json({'error': 'not found', 'endpoints': [
                '/', '/list', '/music?q=', '/weather?city=', 'POST /ask']}, 404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == '/ask':
            # P8e: LLM 联网问答 (B+C 合一)
            length = int(self.headers.get('Content-Length', 0))
            if length == 0:
                self._text("没收到问题", 400)
                return
            raw = self.rfile.read(length)
            try:
                data = json.loads(raw.decode('utf-8'))
            except Exception:
                self._text("请求格式不对", 400)
                return
            user_text = data.get('text', '').strip()
            if not user_text:
                self._text("没听清，请再说一遍", 400)
                return
            # v1w: 接收对话历史 (ESP32 端维护最近 2 轮)
            history = data.get('history', [])
            reply = ask_llm(user_text, history)
            self._text(reply)
        else:
            self._json({'error': 'not found'}, 404)

    def log_message(self, fmt, *args):
        print('[KS] %s - %s' % (self.client_address[0], fmt % args))

_HTML = """<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小智 · 知识服务网关</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:720px;margin:40px auto;padding:0 16px;color:#2c3e50}
h1{color:#1a73e8}h2{color:#34495e;border-bottom:2px solid #eef;padding-bottom:6px}
code{background:#f0f4ff;padding:2px 8px;border-radius:4px;color:#1a73e8}
li{margin:8px 0}.badge{display:inline-block;padding:2px 10px;border-radius:12px;font-size:12px;color:#fff}
.ok{background:#34a853}.no{background:#ea4335}
</style></head><body>
<h1>小智 · 知识服务网关 (P8e)</h1>
<p>ESP32 查真数据走这里。零依赖，本地/云端都能跑。</p>
<h2>LLM 联网问答 (B+C 合一) <span class="badge ok">GLM-4-Flash</span></h2>
<ul>
<li><code>POST /ask</code> body <code>{"text":"查火车"}</code> → ≤50字纯文本 (web_search 兜底 + get_weather function calling)</li>
</ul>
<h2>天气查询 <span class="badge %s">%s</span></h2>
<ul>
<li><code>/weather?city=北京</code> → 今天预报</li>
<li><code>/weather?city=西安&clothes=1</code> → 天气+穿衣建议</li>
<li><code>/weather?city=兰州&days=2</code> → 明天天气</li>
</ul>
<h2>音乐合成</h2>
<ul>
<li><code>/music?q=小星星</code> → 裸 PCM</li>
<li><code>/music?q=致爱丽丝&format=wav</code> → 浏览器试听</li>
</ul>
<p>和风天气: <code>QWEATHER_HOST</code> + <code>QWEATHER_KEY</code> | GLM: <code>ZHIPU_API_KEY</code></p>
</body></html>""" % (
    'ok' if QWEATHER_KEY else 'no',
    ('新版API' if _USE_NEW_API else ('已配置(旧版)' if QWEATHER_KEY else '未配置')))

def main():
    ap = argparse.ArgumentParser(description='小智知识服务网关 (天气+音乐+LLM问答)')
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=8000)
    args = ap.parse_args()

    print('=' * 56)
    print('  小智 · 知识服务网关 (P8e)')
    print('  问答: GLM-4-Flash + web_search + get_weather function (B+C)')
    if QWEATHER_KEY:
        print('  天气: %s (%s)' % ('和风天气已配置' if QWEATHER_KEY else '未配置',
              ('新版API ' + QWEATHER_HOST if _USE_NEW_API else '旧版API')))
    else:
        print('  天气: 未配置 QWEATHER_HOST/QWEATHER_KEY (天气不可用)')
    print('  音乐: %s 首曲' % len(SONGS))
    print('  监听: http://%s:%d' % (args.host, args.port))
    print('  问答: POST http://localhost:%d/ask  body={"text":"你好"}' % args.port)
    print('  天气: http://localhost:%d/weather?city=西安&clothes=1' % args.port)
    print('  音乐: http://localhost:%d/music?q=小星星&format=wav' % args.port)
    print('=' * 56)
    server = ThreadingHTTPServer((args.host, args.port), KSHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\n[KS] bye')

if __name__ == '__main__':
    main()
