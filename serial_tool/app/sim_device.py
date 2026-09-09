"""
模拟设备 - 无开发板时调试整个工具管线

暴露与 pyserial 相同的 read/write/close 接口, 按 #X 协议回包。
"""
from __future__ import annotations

import threading
import time


class SimDevice:
    def __init__(self, port: str = "", baudrate: int = 115200):
        self._rx = b""          # 主机 -> 设备方向缓冲
        self._logs = [          # 模拟日志(每 3s 推一条)
            "[WiFi] GOT IP",
            "[SNTP] time synced 2026-08-04 09:00:00",
            "[Weather] temp=32 humi=65",
        ]
        self._log_i = 0
        self._last_log = time.time()
        self._connected = True
        self._ssid = "FAE@Seahi"
        self._pwd = "fae12345678"
        self._city = "440306"
        self._ip = "192.168.16.16"
        self._wx_api = "amap"
        self._lock = threading.Lock()

    # ---- 串口兼容接口 ----
    def write(self, data: bytes) -> int:
        with self._lock:
            self._rx += data
            return len(data)

    def read(self, size: int = 1) -> bytes:
        with self._lock:
            now = time.time()
            out = b""
            # 处理命令
            while b"\n" in self._rx:
                line, self._rx = self._rx.split(b"\n", 1)
                out += self._handle(line.strip(b"\r\n"))
            # 定时推模拟日志
            if now - self._last_log > 3.0 and not out:
                self._last_log = now
                out += (self._logs[self._log_i % len(self._logs)] + "\r\n").encode()
                self._log_i += 1
            return out[:size] if size else out

    def close(self):
        self._connected = False

    def reset_input_buffer(self):
        with self._lock:
            self._rx = b""

    def in_waiting(self) -> int:
        return 0

    # ---- 协议处理 ----
    def _handle(self, line: bytes) -> bytes:
        text = line.decode("utf-8", "replace")
        if not text.startswith("#X"):
            return b""
        cmd = text[2:]
        if cmd == "PING":
            return b"#XA,OK,PING\r\n"
        if cmd == "VER":
            return b"#XA,OK,VER,58454d4f574f5f544553543420312e30\r\n"  # "XEMOWO_TEST4 1.0"
        if cmd == "STA":
            import app.protocol as p
            return (f"#XST,CONNECTED_IP_GOT,{p.hex_of(self._ssid)},{self._ip},-45,"
                    f"2026-08-04 09:00:00,{p.hex_of(self._city)},"
                    f"{p.hex_of(self._wx_api)}\r\n").encode()
        if cmd.startswith("CFG"):
            import app.protocol as p
            parts = cmd.split(",")[1:] if "," in cmd else []
            if len(parts) >= 2:
                ssid, pwd = p.str_of(parts[0]), p.str_of(parts[1])
                if ssid:
                    self._ssid = ssid
                if pwd:
                    self._pwd = pwd
                if len(parts) >= 3 and parts[2]:
                    self._city = p.str_of(parts[2])
            return (f"#XA,OK,CFG,{p.hex_of(self._ssid)},{p.hex_of(self._city)}\r\n"
                    f"#XEV,GOT_IP,{self._ip}\r\n").encode()
        if cmd.startswith("CITY"):
            import app.protocol as p
            parts = cmd.split(",")[1:] if "," in cmd else []
            if parts:
                self._city = p.str_of(parts[0])
            return f"#XA,OK,CITY,{p.hex_of(self._city)}\r\n".encode()
        if cmd.startswith("MON"):
            parts = cmd.split(",")[1:] if "," in cmd else []
            on = parts and parts[0] == "1"
            return (f"#XA,OK,MON,{'on' if on else 'off'}\r\n").encode()
        if cmd.startswith("LAMP"):
            parts = cmd.split(",")[1:] if "," in cmd else []
            val = parts[0] if parts else "?"
            return (f"#XA,OK,LAMP,{val}\r\n").encode()
        if cmd.startswith("IMGSTART"):
            return b"#XA,OK,IMGSTART\r\n"
        if cmd.startswith("IMGEND"):
            return b"#XA,OK,IMGEND\r\n"
        if cmd.startswith("IMG,"):
            return b"#XA,OK,IMG\r\n"
        if cmd.startswith("CLOCK"):
            return b"#XA,OK,CLOCK,31\r\n"
        if cmd.startswith("WALL"):
            parts = cmd.split(",")[1:] if "," in cmd else []
            return f"#XA,OK,WALL,{parts[0]},{parts[1] if len(parts) > 1 else '05'}\r\n".encode()
        if cmd.startswith("WXAPI"):
            import app.protocol as p
            parts = cmd.split(",")[1:] if "," in cmd else []
            if parts and p.str_of(parts[0]) in ("amap", "qweather"):
                self._wx_api = p.str_of(parts[0])
                return (f"#XA,OK,WXAPI,{p.hex_of(self._wx_api)}\r\n").encode()
            return b"#XA,ERR,WXAPI,626164206170692028616d61707c717765617468657229\r\n"
        if cmd.startswith("WXKEY"):
            import app.protocol as p
            parts = cmd.split(",")[1:] if "," in cmd else []
            if parts and p.str_of(parts[0]) in ("amap", "qweather"):
                return b"#XA,OK,WXKEY\r\n"
            return b"#XA,ERR,WXKEY,6261642070726f7669646572\r\n"
        return b"#XA,ERR,UNKNOWN\r\n"
