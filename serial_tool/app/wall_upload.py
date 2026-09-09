"""
壁纸上传 - 图片处理(缩放/裁剪/RGB565) + 串口分块上传线程

PC 端做全部重活: Pillow 缩放裁剪 → RGB565 小端 → 分块 hex → 顺序发送;
MCU 端只做收块→写 flash→XIP 显示(固件侧 xcmd 的 #XIMGSTART/#XIMG/#XIMGEND)。
"""
from __future__ import annotations

import time

from PySide6.QtCore import QThread, Signal, QMutex, QWaitCondition

from app.protocol import build_imgstart, build_img, build_imgend, str_of

IMG_W = 320          # 可视区宽 (面板 172x320 竖屏横用, 逻辑列 0-319 全有效)
IMG_H = 190          # 可视区高 (OY=30 起点对齐, 可见 190 行)
BOOT_W = 320         # 开机图宽 (全屏)
BOOT_H = 220         # 开机图高 (全屏)
CHUNK = 1000         # 每块数据字节数 (hex 2000 字符 + 帧头 < 2048 固件行缓冲)


def process_image(path: str, w: int = IMG_W, h: int = IMG_H) -> bytes:
    """图片 → w×h RGB565 小端字节流 (contain 缩放居中: 完整显示, 不足处黑边)

    与预览(KeepAspectRatio 完整显示)一致: cover 会把比例不符的图左右/上下
    留白裁掉, 实机显示与预览对不上; contain 保留全部内容, 缺的部分补黑边。
    默认壁纸 320x190; 开机图传 w=BOOT_W, h=BOOT_H。
    """
    from PIL import Image

    img = Image.open(path).convert("RGB")
    scale = min(w / img.width, h / img.height)
    nw, nh = max(1, round(img.width * scale)), max(1, round(img.height * scale))
    img = img.resize((nw, nh), Image.LANCZOS)
    canvas = Image.new("RGB", (w, h), (0, 0, 0))
    canvas.paste(img, ((w - nw) // 2, (h - nh) // 2))
    img = canvas

    raw = img.tobytes()                     # 每像素 R,G,B
    out = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)   # B 占 5bit, 取高 5 位须 >>3 (之前 >>5 白色变黄)
        out[i * 2] = v & 0xFF               # 小端: 低字节在前
        out[i * 2 + 1] = v >> 8
    return bytes(out)


def build_test_image() -> bytes:
    """测试图: 白底 + 四边定位条 + 中央 80x80 黑方块 + 左侧每 10 行粗刻度。
    四边: 外 4px 红 (0xF800) + 内 4px 黑 — 上传后看四条红/黑边是否完整可见,
    定位水平偏移 (OX) 与垂直偏移 (OY): 哪条边没了说明该方向被裁。"""
    w, h = IMG_W, IMG_H
    out = bytearray(w * h * 2)
    cx1, cx2 = w // 2 - 40, w // 2 + 40   # 中央黑方块: 宽 80
    cy1, cy2 = h // 2 - 40, h // 2 + 40   # 中央黑方块: 高 80
    for y in range(h):
        row = y * w
        for x in range(w):
            red = (x < 4 or x >= w - 4 or y < 4 or y >= h - 4)
            black = (x < 8 or x >= w - 8 or y < 8 or y >= h - 8)
            tick = (x < 10 and y % 10 < 5)          # 每 10 行一个 10x5 黑块
            center = (cx1 <= x < cx2 and cy1 <= y < cy2)
            if red:
                v = 0xF800
            elif black or tick or center:
                v = 0x0000
            else:
                v = 0xFFFF
            i = (row + x) * 2
            out[i] = v & 0xFF
            out[i + 1] = v >> 8
    return bytes(out)


class AckWaiter:
    """跨线程 ACK 等待: 上传线程 send 后阻塞等 GUI 线程的串口应答通知"""

    def __init__(self):
        self._mutex = QMutex()
        self._cond = QWaitCondition()
        self._result: tuple | None = None

    def notify(self, cmd: str, result: str, data: list[str] | None = None):
        self._mutex.lock()
        self._result = (cmd, result, data or [])
        self._cond.wakeAll()
        self._mutex.unlock()

    def wait(self, cmd: str, timeout_ms: int = 8000) -> tuple | None:
        """等指定命令的 ACK, 返回 (cmd, result, data); 超时返回 None; 不匹配的 ACK 丢弃继续等"""
        self._mutex.lock()
        try:
            deadline = time.monotonic() + timeout_ms / 1000
            while True:
                if self._result and self._result[0] == cmd:
                    r = self._result
                    self._result = None
                    return r
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self._result = None
                    return None
                self._cond.wait(self._mutex, int(remaining * 1000))
        finally:
            self._mutex.unlock()


class WallUploadThread(QThread):
    progress = Signal(int, int)     # 已发块, 总块
    succeeded = Signal()
    failed = Signal(str)
    retry_note = Signal(str)        # 重试提示(2M 偶发丢字节, 重发覆盖写)

    def __init__(self, data: bytes, send, waiter: AckWaiter, img_type: int = 0,
                 parent=None):
        super().__init__(parent)
        self._data = data
        self._send = send           # 线程安全投递到串口队列
        self._waiter = waiter
        self._img_type = img_type   # 0=壁纸 1=开机图

    def run(self):
        try:
            data = self._data
            pad = (-len(data)) % CHUNK
            if pad:                             # 补零到块等长: 固件按 seq*len 定位偏移,
                data = data + b"\x00" * pad     # 最后一块不足 CHUNK 会算错偏移(之前 600B 尾部
            self._send(build_imgstart(len(data), self._img_type))  # 被写到 72600 处覆盖第 72 块, 导致 finish 校验失败)。
            if not self._expect("IMGSTART"):    # 补的 0x00 落在 190 行可视区外, 被屏幕裁掉
                return
            n = (len(data) + CHUNK - 1) // CHUNK
            for i in range(n):
                chunk = data[i * CHUNK:(i + 1) * CHUNK]
                if not self._send_img(i, chunk):
                    return
                self.progress.emit(i + 1, n)
            self._send(build_imgend())
            if not self._expect("IMGEND", 3000):
                return
            self.succeeded.emit()
        except Exception as e:
            self.failed.emit(str(e))

    def _send_img(self, seq: int, chunk: bytes, retries: int = 5) -> bool:
        """发送一块数据: 超时/固件 ERR(2M 下偶发丢字节致 hex 错位)重发同 seq,
        固件按 seq 定位 offset 覆盖写, 幂等安全"""
        err = "未知错误"
        for attempt in range(1, retries + 1):
            self._send(build_img(seq, chunk))
            r = self._waiter.wait("IMG", 2000)   # 固件处理一帧 <100ms, 2s 足够
            if r is not None and r[1] == "OK":
                return True
            err = r[1] if r is not None else "超时"
            if attempt < retries:
                self.retry_note.emit(f"块 {seq} 重发 {attempt}/{retries}: {err}")
        self.failed.emit(f"IMG 块 {seq} 重试 {retries} 次仍失败: {err}")
        return False

    def _expect(self, cmd: str, timeout_ms: int = 8000) -> bool:
        """等 ACK(单次; IMGSTART 含整区擦除较慢用默认 8s, IMGEND 传 3000)"""
        r = self._waiter.wait(cmd, timeout_ms)
        if r is None:
            self.failed.emit(f"等待 {cmd} 应答超时(设备未连接?)")
            return False
        got, result, data = r
        if got == cmd and result == "OK":
            return True
        detail = str_of(data[0]) if data else ""
        self.failed.emit(f"{cmd} 返回 {result}" + (f" ({detail})" if detail else ""))
        return False
