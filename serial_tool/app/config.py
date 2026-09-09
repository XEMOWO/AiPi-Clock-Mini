"""
配置持久化 - config.json

位置: exe 同目录(冻结时) 或 项目根目录(开发时)
"""
from __future__ import annotations

import json
import os
import sys

DEFAULTS = {
    "port": "COM3",
    "baud": 2000000,
    "sim_mode": False,
    "quick_buttons": [
        {"label": "PING", "text": "#XPING"},
        {"label": "状态", "text": "#XSTA"},
        {"label": "版本", "text": "#XVER"},
    ],
    "last_ssid": "",
    "last_city": "440306",
    "auto_refresh": False,
    "mon_on": False,
    "mon_file": "",
    "last_wxapi": "amap",
    "last_amap_key": "c01d70381da92dee9c4f16320555d685",
    "last_qw_key": "61622d11bffc476aa10b36e08c0f0f4a",
    "last_qw_cred": "HE2301271524131032",
    "last_wall_sec": 5,
    "last_wall_clock_sec": 5,
    "last_bg_c1": 13427448,   # 0xCDE6F8 淡蓝
    "last_bg_c2": 15329526,   # 0xE9DCF6 淡紫
    "last_bg_dir": 2,         # 垂直渐变
}


def _config_dir() -> str:
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _path() -> str:
    return os.path.join(_config_dir(), "config.json")


def load() -> dict:
    cfg = dict(DEFAULTS)
    try:
        with open(_path(), "r", encoding="utf-8") as f:
            saved = json.load(f)
        if isinstance(saved, dict):
            cfg.update(saved)
    except Exception:
        pass
    return cfg


def save(cfg: dict):
    try:
        with open(_path(), "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)
    except Exception:
        pass
