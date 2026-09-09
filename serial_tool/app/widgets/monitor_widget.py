"""
串口监控页 - Fluent 组件: 终端区 + 发送 + 快捷按钮 + 日志保存
"""
from __future__ import annotations

from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QLineEdit,
                               QDialog, QFormLayout)
from qfluentwidgets import (TextEdit, PushButton, PrimaryPushButton, BodyLabel,
                            FluentIcon, InfoBar, InfoBarPosition)

from app.config import load, save


class MonitorWidget(QWidget):
    def __init__(self, on_send, parent=None):
        """on_send: callable(bytes) 发送数据"""
        super().__init__(parent)
        self._on_send = on_send
        self._cfg = load()

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(8)

        # 终端区
        self.view = TextEdit(self)
        self.view.setReadOnly(True)
        self.view.document().setMaximumBlockCount(8000)   # 防高流量刷爆 UI
        self.view.setStyleSheet("font-family: 'Cascadia Mono', Consolas, monospace;")
        layout.addWidget(self.view, 1)

        # 工具栏
        bar = QHBoxLayout()
        self.btn_save = PushButton(FluentIcon.SAVE, "保存日志", self)
        self.btn_clear = PushButton(FluentIcon.DELETE, "清屏", self)
        self.btn_manage = PushButton(FluentIcon.ADD_TO, "管理快捷按钮", self)
        self.btn_save.clicked.connect(self._save_log)
        self.btn_clear.clicked.connect(self.view.clear)
        self.btn_manage.clicked.connect(self._manage_buttons)
        bar.addWidget(self.btn_save)
        bar.addWidget(self.btn_clear)
        bar.addStretch(1)
        bar.addWidget(self.btn_manage)
        layout.addLayout(bar)

        # 快捷按钮行
        self.btn_row = QHBoxLayout()
        layout.addLayout(self.btn_row)
        self._rebuild_buttons()

        # 发送行
        send_row = QHBoxLayout()
        self.send_edit = QLineEdit(self)
        self.send_edit.setPlaceholderText("输入要发送的内容, 回车发送 (支持 #X 协议命令)")
        self.send_edit.returnPressed.connect(self._do_send)
        self.btn_send = PrimaryPushButton(FluentIcon.SEND, "发送", self)
        self.btn_send.clicked.connect(self._do_send)
        send_row.addWidget(self.send_edit, 1)
        send_row.addWidget(self.btn_send)
        layout.addLayout(send_row)

    # ---- 日志 ----
    def append(self, text: str):
        self.view.append(text)

    def _save_log(self):
        from PySide6.QtWidgets import QFileDialog
        path, _ = QFileDialog.getSaveFileName(self, "保存日志", "wb2_log.txt", "文本文件 (*.txt)")
        if not path:
            return
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(self.view.toPlainText())
            self.append(f"[工具] 日志已保存: {path}")
            InfoBar.success("已保存", path, parent=self, position=InfoBarPosition.TOP_RIGHT)
        except Exception as e:
            InfoBar.error("保存失败", str(e), parent=self, position=InfoBarPosition.TOP_RIGHT)

    # ---- 发送 ----
    def _do_send(self):
        text = self.send_edit.text()
        if not text:
            return
        self._on_send((text + "\r\n").encode("utf-8"))
        self.append(f"TX→ {text}")

    def _send_quick(self, text: str):
        self._on_send((text + "\r\n").encode("utf-8"))
        self.append(f"TX→ {text}")

    # ---- 快捷按钮 ----
    def _rebuild_buttons(self):
        while self.btn_row.count():
            item = self.btn_row.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()
        for btn in self._cfg.get("quick_buttons", []):
            b = PushButton(btn.get("label", "?"), self)
            b.setToolTip(btn.get("text", ""))
            b.clicked.connect(lambda _=False, t=btn.get("text", ""): self._send_quick(t))
            self.btn_row.addWidget(b)
        self.btn_row.addStretch(1)

    def _manage_buttons(self):
        dlg = QDialog(self)
        dlg.setWindowTitle("管理快捷按钮")
        form = QFormLayout(dlg)
        edits: list[tuple[str, QLineEdit]] = []

        def _add_row():
            n = len(edits) + 1
            le = QLineEdit(dlg)
            le.setPlaceholderText("发送内容")
            form.addRow(f"按钮{n}", le)
            edits.append((f"按钮{n}", le))

        for btn in self._cfg.get("quick_buttons", []):
            le = QLineEdit(btn.get("text", ""), dlg)
            le.setPlaceholderText("发送内容")
            form.addRow(btn.get("label", "?"), le)
            edits.append((btn.get("label", "?"), le))

        row = QHBoxLayout()
        btn_add = PushButton("添加", dlg)
        btn_add.clicked.connect(_add_row)

        def _save_dlg():
            self._cfg["quick_buttons"] = [
                {"label": lbl, "text": le.text()} for lbl, le in edits if le.text()
            ]
            save(self._cfg)
            self._rebuild_buttons()
            dlg.accept()

        btn_ok = PrimaryPushButton("保存", dlg)
        btn_ok.clicked.connect(_save_dlg)
        row.addWidget(btn_add)
        row.addWidget(btn_ok)
        form.addRow(row)
        dlg.exec()
