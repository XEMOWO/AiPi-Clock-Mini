"""
串口读写线程 - QThread + pyserial

串口对象只在本线程内使用, GUI 线程通过 send() 投递写数据, 通过信号收数据。
支持"模拟设备"模式(不打开真实串口)。
"""
from __future__ import annotations

import queue
import threading

from PySide6.QtCore import QThread, Signal


class SerialWorker(QThread):
    line_received = Signal(str)     # 已按 \n 切好的行
    conn_changed = Signal(bool)     # 连接状态变化
    error_occurred = Signal(str)    # 错误信息

    def __init__(self, port: str, baud: int, parent=None):
        super().__init__(parent)
        self._port = port
        self._baud = baud
        self._q: queue.Queue[bytes] = queue.Queue()
        self._stop = threading.Event()

    def send(self, data: bytes):
        """GUI 线程调用, 线程安全"""
        self._q.put(data)

    def stop(self):
        self._stop.set()
        self._q.put(b"")  # 唤醒阻塞的读

    def run(self):
        dev = None
        try:
            if self._port == "模拟设备":
                from app.sim_device import SimDevice
                dev = SimDevice()
                self.conn_changed.emit(True)
            else:
                import serial
                dev = serial.Serial(self._port, self._baud, timeout=0.2)
                self.conn_changed.emit(True)

            buf = b""
            while not self._stop.is_set():
                # 写: 排空发送队列
                sent_any = False
                while not self._q.empty():
                    data = self._q.get()
                    if data:
                        dev.write(data)
                        sent_any = True
                # 读: 200ms 超时轮询
                try:
                    chunk = dev.read(4096)
                except Exception:
                    chunk = b""
                buf += chunk
                # 按 \n 切行
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    try:
                        self.line_received.emit(line.decode("utf-8", "replace").rstrip("\r"))
                    except Exception:
                        pass
        except Exception as e:
            self.error_occurred.emit(str(e))
        finally:
            if dev is not None:
                try:
                    dev.close()
                except Exception:
                    pass
            self.conn_changed.emit(False)
