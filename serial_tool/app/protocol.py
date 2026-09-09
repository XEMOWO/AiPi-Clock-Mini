"""
WB2 串口工具 - 协议编解码模块

与固件端 xcmd.c 的 #X 行协议一一对应:
    命令: #XPING / #XVER / #XCFG,<ssid_hex>,<pwd_hex>,<city_hex> / #XCITY,<city_hex> / #XSTA
          / #XWXAPI,<hex(amap|qweather)> 切换天气源
          / #XWXKEY,<hex(provider)>,<hex(key)>[,<hex(cred)>] 下发天气 API Key
    响应: #XA,OK|ERR,<CMD>[,<data>]
    状态: #XST,<STATE>,<ssid_hex>,<ip>,<rssi>,<time>,<city_hex>[,<wx_api_hex>]
    事件: #XEV,<NAME>[,<data>]
值字段为 UTF-8 字节的小写 hex 编码。
"""
from __future__ import annotations


def hex_of(s: str) -> str:
    """字符串 -> hex(UTF-8 字节)"""
    return s.encode("utf-8").hex()


def str_of(h: str) -> str:
    """hex -> 字符串(非法 hex 容错)"""
    try:
        return bytes.fromhex(h).decode("utf-8", "replace")
    except ValueError:
        return h


# ---------- 命令构造 ----------

def build_ping() -> bytes:
    return b"#XPING\r\n"


def build_ver() -> bytes:
    return b"#XVER\r\n"


def build_sta() -> bytes:
    return b"#XSTA\r\n"


def build_cfg(ssid: str, pwd: str, city: str = "") -> bytes:
    """空字段表示保持原值"""
    return f"#XCFG,{hex_of(ssid)},{hex_of(pwd)},{hex_of(city)}\r\n".encode("utf-8")


def build_city(city: str) -> bytes:
    return f"#XCITY,{hex_of(city)}\r\n".encode("utf-8")


def build_mon(on: bool) -> bytes:
    """Claude Code 状态监控开关: #XMON,1 / #XMON,0"""
    return b"#XMON,1\r\n" if on else b"#XMON,0\r\n"


def build_lamp(status: int) -> bytes:
    """Claude Code 状态灯: #XLAMP,<0-4> (0呼吸 1/2黄 3绿 4红)"""
    return f"#XLAMP,{int(status) & 0xf}\r\n".encode("utf-8")


def build_clock(anim: int) -> bytes:
    """时钟显示样式: #XCLOCK,<hex("1"=滚动翻页|"0"=直接切换)>"""
    return f"#XCLOCK,{hex_of('1' if anim else '0')}\r\n".encode("utf-8")


def build_imgstart(total: int, img_type: int = 0) -> bytes:
    """开始图片上传: #XIMGSTART,<hex(4字节大端 总字节数)>[,<hex(type)>]
    type: 0=壁纸(默认) 1=开机图"""
    s = f"#XIMGSTART,{total.to_bytes(4, 'big').hex()}"
    if img_type:
        s += f",{img_type:02x}"
    return (s + "\r\n").encode("utf-8")


def build_img(seq: int, chunk: bytes) -> bytes:
    """一块数据: #XIMG,<hex(seq 2B小端)>,<hex(len 2B小端)>,<hex(data)>,<hex(xor)>
    xor = data 逐字节异或: 2M 丢字节致 hex 奇偶错位时固件可检出并重发 (len≤1000)"""
    chk = 0
    for b in chunk:
        chk ^= b
    return (f"#XIMG,{seq.to_bytes(2, 'little').hex()},"
            f"{len(chunk).to_bytes(2, 'little').hex()},{chunk.hex()},"
            f"{chk:02x}\r\n").encode("utf-8")


def build_imgend() -> bytes:
    """结束壁纸上传: #XIMGEND (固件校验总量并切换显示)"""
    return b"#XIMGEND\r\n"


def build_bootclr() -> bytes:
    """清除开机图: #XBOOTCLR (移除图片, 上电不再显示)"""
    return b"#XBOOTCLR\r\n"


def build_bootimg(on: bool) -> bytes:
    """开机图显示开关: #XBOOTIMG,<hex("1"=显示|"0"=停用)>"""
    return f"#XBOOTIMG,{hex_of('1' if on else '0')}\r\n".encode("utf-8")


def build_wall(mode: int, sec: int, anim: int = 0, clock_sec: int | None = None) -> bytes:
    """图播放: #XWALL,<hex(mode 0|1)>,<hex(壁纸秒数)>[,<hex(动画 0-6)>][,<hex(时钟秒数)>]
    1=时钟/壁纸各自按秒数交替, 0=关(始终时钟); 动画: 0=无 1=淡入 2=左滑
    3=右滑 4=上滑 5=缩放淡入 6=随机; clock_sec = 时钟页面显示秒数
    (可选参数不传 = 固件保持原设置)"""
    s = f"#XWALL,{hex_of('1' if mode else '0')},{hex_of(str(int(sec)))}"
    if anim:
        s += f",{hex_of(str(int(anim)))}"
    if clock_sec is not None:
        s += f",{hex_of(str(int(clock_sec)))}"
    return (s + "\r\n").encode("utf-8")


def build_bg(c1: int, c2: int, grad_dir: int) -> bytes:
    """时钟屏背景: #XBG,<hex(0xRRGGBB 起色)>,<hex(0xRRGGBB 止色)>,<hex(样式)>
    样式: 0=纯色(用起色) 1=水平渐变 2=垂直渐变"""
    return (f"#XBG,{c1 & 0xFFFFFF:06x},{c2 & 0xFFFFFF:06x},"
            f"{hex_of(str(int(grad_dir)))}\r\n").encode("utf-8")


def build_wxapi(provider: str) -> bytes:
    """切换天气源: #XWXAPI,<hex("amap"|"qweather")>"""
    return f"#XWXAPI,{hex_of(provider)}\r\n".encode("utf-8")


def build_wxkey(provider: str, key: str, cred: str = "") -> bytes:
    """下发天气 API Key: #XWXKEY,<hex(provider)>,<hex(key)>[,<hex(cred)>]
    provider = amap | qweather; cred 为和风旧版凭据ID(可选, 仅存储备用)"""
    body = f"#XWXKEY,{hex_of(provider)},{hex_of(key)}"
    if cred:
        body += f",{hex_of(cred)}"
    return (body + "\r\n").encode("utf-8")


# ---------- 响应解析 ----------

def parse_line(line: str) -> dict:
    """解析一行串口数据, 返回 {kind, ...}:
        kind = ack / status / event / log
    """
    line = line.strip("\r\n")
    if not line.startswith("#X"):
        return {"kind": "log", "text": line}

    parts = line.split(",")

    if line.startswith("#XA,") and len(parts) >= 3:
        return {
            "kind": "ack",
            "result": parts[1],           # OK / ERR
            "cmd": parts[2],              # PING / VER / CFG / CITY ...
            "data": parts[3:] if len(parts) > 3 else [],
            "raw": line,
        }

    if line.startswith("#XST,") and len(parts) >= 7:
        st = {
            "kind": "status",
            "state": parts[1],
            "ssid": str_of(parts[2]),
            "ip": parts[3],
            "rssi": parts[4],
            "time": parts[5],
            "city": str_of(parts[6]),
            "raw": line,
        }
        if len(parts) > 7:                       # 新固件: 末尾带天气源
            st["wx_api"] = str_of(parts[7])
        return st

    if line.startswith("#XEV,") and len(parts) >= 2:
        return {
            "kind": "event",
            "name": parts[1],             # GOT_IP / WIFI_DISCONNECT ...
            "data": parts[2] if len(parts) > 2 else "",
            "raw": line,
        }

    return {"kind": "log", "text": line}
