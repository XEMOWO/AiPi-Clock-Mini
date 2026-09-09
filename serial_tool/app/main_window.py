"""
主窗口 - Fluent Win11 风格: 顶部连接栏 + 分段导航 + 三个页面
"""
from __future__ import annotations

from PySide6.QtWidgets import (QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
                               QStackedWidget)
from PySide6.QtCore import QTimer
from qfluentwidgets import (ComboBox, PrimaryPushButton, PushButton,
                            SegmentedWidget, BodyLabel, setTheme, Theme,
                            InfoBar, InfoBarPosition, FluentIcon)

from app.config import load, save
from app.protocol import parse_line, build_ping
from app.widgets.monitor_widget import MonitorWidget
from app.widgets.wifi_widget import WifiWidget
from app.widgets.status_widget import StatusWidget


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        setTheme(Theme.DARK)      # 深色主题
        self.setWindowTitle("WB2 串口工具")
        self.resize(840, 620)

        self._cfg = load()
        self._worker = None

        central = QWidget(self)
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(12, 12, 12, 8)
        root.setSpacing(10)

        # ===== 顶部连接栏 =====
        bar = QHBoxLayout()
        bar.addWidget(BodyLabel("端口:"))
        self.port_combo = ComboBox(self)
        self.port_combo.setMinimumWidth(140)
        self.port_combo.addItem("模拟设备")
        self._last_ports = None
        self._refresh_ports()
        bar.addWidget(self.port_combo)

        self.btn_refresh = PushButton(FluentIcon.SYNC, "刷新", self)
        self.btn_refresh.setToolTip("刷新串口列表")
        self.btn_refresh.clicked.connect(self._refresh_ports)
        bar.addWidget(self.btn_refresh)

        bar.addWidget(BodyLabel("波特率:"))
        self.baud_combo = ComboBox(self)
        for b in ("115200", "921600", "2000000", "57600", "9600"):
            self.baud_combo.addItem(b)
        self.baud_combo.setCurrentText(str(self._cfg.get("baud", 2000000)))
        bar.addWidget(self.baud_combo)

        self.btn_conn = PrimaryPushButton("连接", self)
        self.btn_conn.clicked.connect(self._toggle_conn)
        bar.addWidget(self.btn_conn)

        self.lbl_status = BodyLabel("未连接", self)
        self.lbl_status.setStyleSheet("color: #808080;")
        bar.addWidget(self.lbl_status)

        self.lbl_ping = BodyLabel("协议: ○", self)
        self.lbl_ping.setStyleSheet("color: #808080;")
        bar.addWidget(self.lbl_ping)
        bar.addStretch(1)
        root.addLayout(bar)

        # ===== 分段导航 + 页面堆栈 =====
        self.nav = SegmentedWidget(self)
        self.stack = QStackedWidget(self)
        self.monitor = MonitorWidget(self._send, self)
        self.wifi = WifiWidget(self._send, self)
        self.status = StatusWidget(self._send, self)
        self.monitor.setObjectName("monitor")
        self.wifi.setObjectName("wifi")
        self.status.setObjectName("status")
        self.stack.addWidget(self.monitor)
        self.stack.addWidget(self.wifi)
        self.stack.addWidget(self.status)
        self.nav.addItem(routeKey="monitor", text="串口监控",
                         onClick=lambda: self.stack.setCurrentWidget(self.monitor))
        self.nav.addItem(routeKey="wifi", text="WiFi 配置",
                         onClick=lambda: self.stack.setCurrentWidget(self.wifi))
        self.nav.addItem(routeKey="status", text="状态查询",
                         onClick=lambda: self.stack.setCurrentWidget(self.status))
        root.addWidget(self.nav)
        root.addWidget(self.stack, 1)

        # 自动刷新串口列表 (2s)
        self._port_timer = QTimer(self)
        self._port_timer.setInterval(2000)
        self._port_timer.timeout.connect(self._refresh_ports)
        self._port_timer.start()

    # ---- 端口列表 ----
    def _refresh_ports(self):
        current = self.port_combo.currentText()
        ports = ["模拟设备"]
        try:
            import serial.tools.list_ports as lp
            for p in lp.comports():
                if p.device not in ports:
                    ports.append(p.device)
        except Exception:
            pass
        if ports == self._last_ports:
            return
        self._last_ports = list(ports)
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if current:
            idx = self.port_combo.findText(current)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)
        elif self._cfg.get("port"):
            idx = self.port_combo.findText(self._cfg["port"])
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)

    # ---- 连接 ----
    def _toggle_conn(self):
        if self._worker is not None:
            self._worker.stop()
            self._worker.wait(2000)
            self._worker = None
            self._set_conn_state(False)
            return

        from app.serial_worker import SerialWorker
        port = self.port_combo.currentText().strip()
        baud = int(self.baud_combo.currentText())
        self._cfg["port"] = port
        self._cfg["baud"] = baud
        save(self._cfg)

        self._worker = SerialWorker(port, baud, self)
        self._worker.line_received.connect(self._on_line)
        self._worker.conn_changed.connect(self._on_conn_changed)
        self._worker.error_occurred.connect(self._on_error)
        self._worker.start()
        self.btn_conn.setText("断开")
        self.lbl_status.setText("连接中...")
        self.lbl_status.setStyleSheet("color: #ffcc00;")

    def _set_conn_state(self, connected: bool):
        self.btn_conn.setText("断开" if connected else "连接")
        self.lbl_status.setText("已连接" if connected else "未连接")
        self.lbl_status.setStyleSheet("color: #4CAF50;" if connected else "color: #808080;")
        self.lbl_ping.setText("协议: ○")
        self.lbl_ping.setStyleSheet("color: #808080;")
        if connected and self._worker is not None:
            self._worker.send(build_ping())

    def _on_conn_changed(self, ok: bool):
        if ok:
            self.lbl_status.setText("已连接")
            self.lbl_status.setStyleSheet("color: #4CAF50;")
            if self._worker is not None:
                self._worker.send(build_ping())
            InfoBar.success("连接成功", f"串口已打开", parent=self)
        else:
            self.lbl_status.setText("未连接")
            self.lbl_status.setStyleSheet("color: #808080;")
            self.btn_conn.setText("连接")
            self.lbl_ping.setText("协议: ○")
            self.lbl_ping.setStyleSheet("color: #808080;")

    def _on_error(self, msg: str):
        self.monitor.append(f"[工具] 错误: {msg}")
        self.lbl_status.setText("连接失败")
        self.lbl_status.setStyleSheet("color: #F44336;")
        InfoBar.error("连接失败", msg, parent=self)

    # ---- 发送 ----
    def _send(self, data: bytes):
        if self._worker is None:
            self.monitor.append("[工具] 未连接")
            return
        self._worker.send(data)

    # ---- 行分发 ----
    def _on_line(self, line: str):
        if not line:
            return
        parsed = parse_line(line)
        kind = parsed["kind"]
        if kind == "log":
            self.monitor.append(line)
        else:
            self.monitor.append(line)
            if kind == "ack":
                if parsed["cmd"] == "PING" and parsed["result"] == "OK":
                    self.lbl_ping.setText("协议: ● 已识别")
                    self.lbl_ping.setStyleSheet("color: #4CAF50;")
                self.wifi.on_ack(parsed["result"], parsed["cmd"], parsed["data"])
            elif kind == "status":
                self.status.on_status(parsed)
            elif kind == "event":
                self.status.on_event(parsed["name"], parsed["data"])
                if parsed["name"] == "GOT_IP":
                    self.lbl_ping.setText("协议: ● 已识别")
                    self.lbl_ping.setStyleSheet("color: #4CAF50;")

    def closeEvent(self, event):
        if self._worker is not None:
            self._worker.stop()
            self._worker.wait(2000)
        super().closeEvent(event)
