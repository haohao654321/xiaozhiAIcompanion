#!/usr/bin/env python3
"""测试 12306 直连: 先拿 Cookie 再查车次"""
import urllib.request, ssl, gzip, json, http.cookiejar

ssl_ctx = ssl.create_default_context()
ssl_ctx.check_hostname = False
ssl_ctx.verify_mode = ssl.CERT_NONE

UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"
INIT_URL = "https://kyfw.12306.cn/otn/leftTicket/init"

# 1) 访问 init 拿 cookie
cj = http.cookiejar.CookieJar()
opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
opener.addheaders = [
    ("User-Agent", UA),
    ("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
    ("Accept-Language", "zh-CN,zh;q=0.9"),
]
try:
    r = opener.open(INIT_URL, timeout=10)
    print("init:", r.status)
    print("cookies:", [(c.name, c.value[:20]) for c in cj])
except Exception as e:
    print("init err:", e)

# 2) 查 queryZ
opener.addheaders = [
    ("User-Agent", UA),
    ("Accept", "*/*"),
    ("Accept-Language", "zh-CN,zh;q=0.9"),
    ("Referer", INIT_URL),
    ("X-Requested-With", "XMLHttpRequest"),
]
url = ("https://kyfw.12306.cn/otn/leftTicket/queryZ"
       "?leftTicketDTO.train_date=2026-08-18&leftTicketDTO.from_station=XAY"
       "&leftTicketDTO.to_station=BJP&purpose_codes=ADULT")
try:
    r = opener.open(url, timeout=12)
    raw = r.read()
    if r.headers.get("Content-Encoding") == "gzip":
        raw = gzip.decompress(raw)
    text = raw.decode("utf-8", "replace")
    print("queryZ status:", r.status, "len:", len(text))
    # 解析 JSON
    data = json.loads(text)
    print("httpstatus:", data.get("httpstatus"))
    results = data.get("data", {}).get("result", [])
    print("trains found:", len(results))
    for i, row in enumerate(results[:5]):
        parts = row.split("|")
        if len(parts) > 10:
            print("  #%d: %s %s→%s %s" % (i+1, parts[3], parts[8], parts[9], parts[10]))
except Exception as e:
    print("queryZ err:", e)
