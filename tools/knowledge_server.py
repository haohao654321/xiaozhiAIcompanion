#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
小智 · 知识服务网关 (P8a)
=========================
统一 HTTP 服务, 合并音乐合成 + 天气查询。

端点:
  GET /                                → HTML 说明页
  GET /list                            → JSON 曲目列表
  GET /music?q=小星星                  → 裸 PCM (16kHz/16bit/mono)
  GET /music?q=小星星&format=wav       → WAV (浏览器试听)
  GET /weather?city=北京&days=1&clothes=0 → 纯文本天气播报 (≤50字, UTF-8)

天气数据源: 和风天气 (https://dev.qweather.com)
  - 城市名 → GeoAPI 查 location ID (带缓存)
  - 实时 / 3天预报 / 穿衣指数 → 格式化为口语化短句
  - 天气结果缓存 1 小时 (防超免费额度 1000次/日)

和风天气 (新版 API): 环境变量 QWEATHER_HOST(专属域名) + QWEATHER_KEY(API KEY)
  Windows:  set QWEATHER_HOST=my7fc4dwmj.re.qweatherapi.com  &&  set QWEATHER_KEY=你的KEY  &&  python knowledge_server.py
  Linux:    QWEATHER_HOST=... QWEATHER_KEY=... python knowledge_server.py
  - 新版认证: X-QW-Api-Key 请求头 + gzip 压缩响应 (不再用 ?key= 参数)
  - 兼容旧版: 只设 QWEATHER_KEY 时自动走 devapi.qweather.com + ?key=

真机联调:
  ESP32 天气: http://<PC_IP>:8000/weather?city=西安&clothes=1
  ESP32 音乐: http://<PC_IP>:8000/music?q=小星星

零第三方依赖 (纯 Python 标准库), 本地 PC / 云函数 / VPS 都能跑。
"""

import argparse
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
    n = int(SAMPLE_RATE * dur_s)
    if freq is None or n <= 0:
        return []
    out = []
    harmonics = [(1.0, 1.00), (2.0, 0.35), (3.0, 0.18), (4.0, 0.09)]
    decay = 1.8 + 2.2 * (freq / 1000.0)
    fade_n = int(0.012 * SAMPLE_RATE)
    for i in range(n):
        t = i / SAMPLE_RATE
        env = math.exp(-decay * t)
        s = 0.0
        for mult, w in harmonics:
            s += w * math.sin(2.0 * math.pi * freq * mult * t)
        v = s / sum(w for _, w in harmonics)
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

def render_pcm(song, bpm=None):
    bpm = bpm or song['bpm']
    beat = 60.0 / bpm
    samples = []
    for note in song['notes']:
        name, beats = note[0], note[1]
        rest = len(note) > 2 and note[2] == 'r'
        dur = beats * beat
        if rest:
            samples.extend([0] * int(SAMPLE_RATE * dur))
        else:
            samples.extend(synth_note(note_freq(name), dur))
    return struct.pack('<%dh' % len(samples), *samples), len(samples)

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
                '/', '/list', '/music?q=', '/weather?city=']}, 404)

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
<h1>小智 · 知识服务网关 (P8a)</h1>
<p>ESP32 查真数据走这里。零依赖，本地/云端都能跑。</p>
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
<p>和风天气: 环境变量 <code>QWEATHER_HOST</code>(专属域名) + <code>QWEATHER_KEY</code>(API KEY)</p>
</body></html>""" % (
    'ok' if QWEATHER_KEY else 'no',
    ('新版API' if _USE_NEW_API else ('已配置(旧版)' if QWEATHER_KEY else '未配置')))

def main():
    ap = argparse.ArgumentParser(description='小智知识服务网关 (天气+音乐)')
    ap.add_argument('--host', default='0.0.0.0')
    ap.add_argument('--port', type=int, default=8000)
    args = ap.parse_args()

    print('=' * 56)
    print('  小智 · 知识服务网关 (P8a)')
    if QWEATHER_KEY:
        print('  天气: %s (%s)' % ('和风天气已配置' if QWEATHER_KEY else '未配置',
              ('新版API ' + QWEATHER_HOST if _USE_NEW_API else '旧版API')))
    else:
        print('  天气: 未配置 QWEATHER_HOST/QWEATHER_KEY (天气不可用)')
    print('  音乐: %s 首曲' % len(SONGS))
    print('  监听: http://%s:%d' % (args.host, args.port))
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
