"""
协议编解码往返测试: pytest tests/test_protocol.py
"""
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from app.protocol import (hex_of, str_of, build_cfg, build_city, build_sta,
                          build_ping, build_ver, parse_line)


def test_hex_roundtrip():
    assert str_of(hex_of("FAE@Seahi")) == "FAE@Seahi"


def test_hex_chinese():
    s = "中文SSID"
    assert str_of(hex_of(s)) == s


def test_build_cfg():
    cmd = build_cfg("FAE@Seahi", "fae12345678", "440306")
    assert cmd == (b"#XCFG,464145405365616869,6661653132333435363738,"
                   b"343430333036\r\n")


def test_build_cfg_empty_field_keeps():
    cmd = build_cfg("", "", "440306")
    assert cmd == b"#XCFG,,,343430333036\r\n"


def test_build_sta_ping_ver():
    assert build_sta() == b"#XSTA\r\n"
    assert build_ping() == b"#XPING\r\n"
    assert build_ver() == b"#XVER\r\n"


def test_parse_ack_ok():
    r = parse_line("#XA,OK,CFG,464145405365616869,343430333036")
    assert r["kind"] == "ack"
    assert r["result"] == "OK"
    assert r["cmd"] == "CFG"
    assert r["data"] == ["464145405365616869", "343430333036"]


def test_parse_ack_err():
    r = parse_line("#XA,ERR,CFG,626164206c656e")
    assert r["kind"] == "ack"
    assert r["result"] == "ERR"
    assert str_of(r["data"][0]) == "bad len"


def test_parse_status():
    r = parse_line("#XST,CONNECTED_IP_GOT,464145405365616869,192.168.16.16,"
                   "-45,2026-08-04 09:00:00,343430333036")
    assert r["kind"] == "status"
    assert r["state"] == "CONNECTED_IP_GOT"
    assert r["ssid"] == "FAE@Seahi"
    assert r["ip"] == "192.168.16.16"
    assert r["rssi"] == "-45"
    assert r["time"] == "2026-08-04 09:00:00"
    assert r["city"] == "440306"


def test_parse_event():
    r = parse_line("#XEV,GOT_IP,192.168.16.16")
    assert r["kind"] == "event"
    assert r["name"] == "GOT_IP"
    assert r["data"] == "192.168.16.16"


def test_parse_log_not_protocol():
    r = parse_line("[WiFi] GOT IP")
    assert r["kind"] == "log"
    r2 = parse_line("TEST4 start, heap=104552")
    assert r2["kind"] == "log"


def test_parse_log_starts_with_hash():
    """日志行以 # 开头但不是 #X 协议 → 按日志处理"""
    r = parse_line("# comment in log")
    assert r["kind"] == "log"
