"""
状态查询页 - Fluent 组件: 状态卡片 + 手动/自动刷新
"""
from __future__ import annotations

from PySide6.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QGridLayout
from qfluentwidgets import (PrimaryPushButton, PushButton, SwitchButton,
                            BodyLabel, SubtitleLabel, FluentIcon, CardWidget)

from app.protocol import build_sta


class StatusWidget(QWidget):
    def __init__(self, on_send, parent=None):
        super().__init__(parent)
        self._on_send = on_send

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(10)

        row = QHBoxLayout()
        self.btn_query = PrimaryPushButton(FluentIcon.SYNC, "查询状态", self)
        self.btn_query.clicked.connect(self.query)
        self.chk_auto = SwitchButton("自动刷新 (5s)", self)
        self.chk_auto.checkedChanged.connect(self._on_auto_toggle)
        row.addWidget(self.btn_query)
        row.addWidget(self.chk_auto)
        row.addStretch(1)
        root.addLayout(row)

        card = CardWidget(self)
        grid = QGridLayout(card)
        self._fields = {
            "state": BodyLabel("-"), "ssid": BodyLabel("-"), "ip": BodyLabel("-"),
            "rssi": BodyLabel("-"), "time": BodyLabel("-"), "city": BodyLabel("-"),
            "wx_api": BodyLabel("-"),
        }
        labels = [("WiFi 状态", "state"), ("SSID", "ssid"), ("IP 地址", "ip"),
                  ("信号强度", "rssi"), ("时间", "time"), ("城市", "city"),
                  ("天气源", "wx_api")]
        for i, (label, key) in enumerate(labels):
            grid.addWidget(BodyLabel(label + ":"), i, 0)
            grid.addWidget(self._fields[key], i, 1)
        root.addWidget(card)
        root.addStretch(1)

        self._timer = None

    def query(self):
        self._on_send(build_sta())

    def _on_auto_toggle(self, checked: bool):
        if checked and self._timer is None:
            from PySide6.QtCore import QTimer
            self._timer = QTimer(self)
            self._timer.timeout.connect(self.query)
            self._timer.start(5000)
            self.query()
        elif not checked and self._timer is not None:
            self._timer.stop()
            self._timer = None

    def on_status(self, status: dict):
        self._fields["state"].setText(status.get("state", "-"))
        self._fields["ssid"].setText(status.get("ssid", "-"))
        self._fields["ip"].setText(status.get("ip", "-"))
        self._fields["rssi"].setText(status.get("rssi", "-"))
        self._fields["time"].setText(status.get("time", "-"))
        self._fields["city"].setText(status.get("city", "-"))
        self._fields["wx_api"].setText(status.get("wx_api", "-"))

    def on_event(self, name: str, data: str):
        if name == "GOT_IP":
            self._fields["ip"].setText(data)
            self._fields["state"].setText("CONNECTED_IP_GOT")
        elif name == "WIFI_DISCONNECT":
            self._fields["state"].setText("DISCONNECT")
            self._fields["ssid"].setText(data)
