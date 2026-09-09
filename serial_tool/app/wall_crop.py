"""
裁剪对话框 — 微信头像式调整: 固定比例裁剪区(壁纸 320:190 / 开机图 320:220),
滚轮缩放图片、拖动平移, 框内即上传内容; 确认后导出 RGB565。

裁剪区 = 整个视口 (输出尺寸 × 1.5), 图片在其内自由缩放/移动,
导出时按当前变换从原图裁出输出尺寸(越界处补黑)。
"""
from __future__ import annotations

import math

from PIL import Image
from PySide6.QtCore import Qt, QRectF
from PySide6.QtGui import QColor, QImage, QPen, QPixmap
from PySide6.QtWidgets import (
    QDialog,
    QGraphicsPixmapItem,
    QGraphicsRectItem,
    QGraphicsScene,
    QGraphicsView,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
)

OUT_W, OUT_H = 320, 190     # 输出(上传)尺寸(壁纸默认)


class CropView(QGraphicsView):
    def __init__(self, scene, parent=None):
        super().__init__(scene, parent)
        self.zoom_cb = None
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.setDragMode(QGraphicsView.NoDrag)   # item 自带 ItemIsMovable

    def wheelEvent(self, e):
        factor = 1.12 ** (e.angleDelta().y() / 120)
        if self.zoom_cb:
            self.zoom_cb(factor)
        e.accept()


class WallCropDialog(QDialog):
    """裁剪区(输出尺寸 × 1.5): 滚轮缩放 + 拖动平移, export() 返回 RGB565 字节
    支持任意输出尺寸(壁纸 320x190 / 开机图 320x220)"""

    def __init__(self, path: str, out_w: int = OUT_W, out_h: int = OUT_H,
                 parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"调整图片 {out_w}x{out_h} (滚轮缩放 / 拖动移动)")
        self.resize(520, 420)
        self._path = path
        self._out_w, self._out_h = out_w, out_h
        self._cw, self._ch = int(out_w * 1.5), int(out_h * 1.5)   # 裁剪区场景尺寸
        with Image.open(path) as im:
            self._orig_w, self._orig_h = im.size

        # 初始 contain 显示尺寸(完整显示, 用户再放大裁切)
        self._scale0 = min(self._cw / self._orig_w, self._ch / self._orig_h)
        self._iw0 = self._orig_w * self._scale0
        self._ih0 = self._orig_h * self._scale0
        self._zoom = 1.0

        # 场景: 黑底 + 裁剪区(白框 + 三等分辅助线)
        self._scene = QGraphicsScene(0, 0, self._cw, self._ch)
        self._scene.setBackgroundBrush(QColor(20, 20, 20))
        self._view = CropView(self._scene)
        self._view.setFixedSize(self._cw + 4, self._ch + 4)
        self._view.setSceneRect(QRectF(0, 0, self._cw, self._ch))
        self._view.zoom_cb = self._apply_zoom

        # 图片 item(缩放绕 item 中心, 拖拽平移)
        disp = QImage(path).scaled(
            max(1, int(round(self._iw0))), max(1, int(round(self._ih0))),
            Qt.KeepAspectRatio, Qt.SmoothTransformation)
        self._item = QGraphicsPixmapItem(QPixmap.fromImage(disp))
        self._item.setPos(self._cw / 2 - self._iw0 / 2,
                          self._ch / 2 - self._ih0 / 2)
        self._item.setTransformOriginPoint(self._iw0 / 2, self._ih0 / 2)
        self._item.setFlag(QGraphicsPixmapItem.ItemIsMovable, True)
        self._scene.addItem(self._item)

        # 裁剪框: 白框 + 三等分线(构图辅助)
        frame = QGraphicsRectItem(0, 0, self._cw, self._ch)
        frame.setPen(QPen(QColor(255, 255, 255, 200), 2))
        frame.setZValue(10)
        self._scene.addItem(frame)
        for gx in (self._cw // 3, self._cw * 2 // 3):
            l1 = QGraphicsRectItem(gx, 0, 1, self._ch)
            l1.setPen(QPen(QColor(255, 255, 255, 60)))
            l1.setZValue(10)
            self._scene.addItem(l1)
        for gy in (self._ch // 3, self._ch * 2 // 3):
            l2 = QGraphicsRectItem(0, gy, self._cw, 1)
            l2.setPen(QPen(QColor(255, 255, 255, 60)))
            l2.setZValue(10)
            self._scene.addItem(l2)

        tip = QLabel("滚轮缩放 · 拖动移动 · 框内即上传内容")
        tip.setAlignment(Qt.AlignCenter)
        btn_ok = QPushButton("确定")
        btn_cancel = QPushButton("取消")
        btn_ok.setDefault(True)
        btn_ok.clicked.connect(self.accept)
        btn_cancel.clicked.connect(self.reject)
        row = QHBoxLayout()
        row.addStretch(1)
        row.addWidget(btn_cancel)
        row.addWidget(btn_ok)

        root = QVBoxLayout(self)
        root.addWidget(tip)
        root.addWidget(self._view, alignment=Qt.AlignCenter)
        root.addLayout(row)

    def _apply_zoom(self, factor: float):
        self._zoom = max(0.05, min(20.0, self._zoom * factor))
        self._item.setScale(self._zoom)

    def export(self) -> bytes:
        """当前裁剪区 → out_w×out_h RGB565 小端字节流(越界处补黑)"""
        ow, oh = self._out_w, self._out_h
        z = self._zoom
        cw, ch = self._iw0 * z, self._ih0 * z
        cx = self._item.pos().x() + self._iw0 / 2   # item 中心(缩放绕中心)
        cy = self._item.pos().y() + self._ih0 / 2
        k = self._scale0 * z                        # 场景像素 → 原图像素比
        left, top = cx - cw / 2, cy - ch / 2
        x0, y0 = -left / k, -top / k
        x1, y1 = (self._cw - left) / k, (self._ch - top) / k

        # 原图放进扩黑画布(允许负坐标/超界), 再裁出目标区域
        ix0, iy0 = math.floor(x0), math.floor(y0)
        w, h = math.ceil(x1) - ix0, math.ceil(y1) - iy0
        canvas = Image.new("RGB", (max(1, w), max(1, h)), (0, 0, 0))
        img = Image.open(self._path).convert("RGB")
        canvas.paste(img, (-ix0, -iy0))
        region = canvas.crop((x0 - ix0, y0 - iy0, x1 - ix0, y1 - iy0))
        region = region.resize((ow, oh), Image.LANCZOS)

        raw = region.tobytes()
        out = bytearray(ow * oh * 2)
        for i in range(ow * oh):
            r, g, b = raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2]
            v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)   # B 占 5bit
            out[i * 2] = v & 0xFF
            out[i * 2 + 1] = v >> 8
        return bytes(out)


def rgb565_to_qimage(data: bytes, w: int = OUT_W, h: int = OUT_H) -> QImage:
    """RGB565 小端字节 → QImage RGB888(上传前回显预览)"""
    buf = bytearray(w * h * 3)
    for i in range(w * h):
        v = data[i * 2] | (data[i * 2 + 1] << 8)
        buf[i * 3] = ((v >> 11) & 0x1F) << 3
        buf[i * 3 + 1] = ((v >> 5) & 0x3F) << 2
        buf[i * 3 + 2] = (v & 0x1F) << 3
    img = QImage(bytes(buf), w, h, w * 3, QImage.Format_RGB888)
    return img.copy()
