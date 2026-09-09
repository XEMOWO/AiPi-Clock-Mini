"""
WiFi 配置页 - Fluent 组件: 配置表单 + Claude Code 状态监控开关
"""
from __future__ import annotations

import json
import urllib.parse
import urllib.request

from PySide6.QtCore import QTimer, QThread, Signal, Qt
from PySide6.QtGui import QPixmap, QColor
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QLineEdit,
                               QFrame, QLabel, QProgressBar, QDialog,
                               QSpinBox, QColorDialog)
from qfluentwidgets import (LineEdit, ComboBox, PrimaryPushButton, PushButton,
                            TextEdit, BodyLabel, SwitchButton, FluentIcon,
                            InfoBar, InfoBarPosition, CardWidget, SmoothScrollArea)

from app.config import load, save
from app.protocol import (build_cfg, build_city, build_mon, build_lamp,
                          build_wxapi, build_wxkey, build_clock, build_wall,
                          build_bg)
from app.wall_upload import (CHUNK, AckWaiter, WallUploadThread, process_image,
                             build_test_image, BOOT_W, BOOT_H)
from app.wall_crop import WallCropDialog, rgb565_to_qimage
from app.cities import CITIES

WX_PROVIDERS = [("amap", "高德天气 amap"), ("qweather", "和风天气 qweather"),
                ("openmeteo", "Open-Meteo (免费免Key)")]

# 高德候选城市(adcode 前缀), 切回 amap/qweather 时恢复
AMAP_CITY_ITEMS = ["440306 宝安区", "440305 南山区", "440304 福田区",
                   "440303 罗湖区", "440300 深圳市", "110000 北京市",
                   "310000 上海市", "440100 广州市"]

# 壁纸/时钟切换过渡动画 (与固件 g_cfg.wall_anim 一一对应)
WALL_ANIMS = ["无动画", "淡入淡出", "左滑推入", "右滑推入", "上滑推入", "缩放淡入", "随机"]

# 时钟屏背景样式 (与固件 g_cfg.bg_dir 一一对应); 默认 2=垂直渐变
BG_DIRS = ["纯色", "水平渐变", "垂直渐变"]
DEFAULT_BG_C1 = 0xCDE6F8   # 淡蓝
DEFAULT_BG_C2 = 0xE9DCF6   # 淡紫


class CityQueryThread(QThread):
    """高德 geocode 城市查询(后台线程, 避免卡 UI)。
    done: (items, error) — items=["adcode 名称", ...], error="" 表示成功"""
    done = Signal(list, str)

    def __init__(self, name: str, key: str, parent=None):
        super().__init__(parent)
        self._name = name
        self._key = key

    def run(self):
        try:
            url = ("https://restapi.amap.com/v3/geocode/geo?address="
                   + urllib.parse.quote(self._name) + "&key=" + self._key)
            req = urllib.request.Request(url, headers={"User-Agent": "WB2SerialTool"})
            with urllib.request.urlopen(req, timeout=10) as r:
                data = json.load(r)
            if data.get("status") != "1":
                self.done.emit([], data.get("info") or "查询失败")
                return
            items = []
            for g in data.get("geocodes", [])[:8]:
                ad = g.get("adcode") or ""
                fa = g.get("formatted_address") or "?"
                items.append(f"{ad} {fa}")
            self.done.emit(items, "")
        except Exception as e:
            self.done.emit([], str(e))


class WifiWidget(QWidget):
    def __init__(self, on_send, parent=None):
        super().__init__(parent)
        self._on_send = on_send
        self._cfg = load()
        self._city_thread: CityQueryThread | None = None
        self._wall_waiter = AckWaiter()      # 壁纸上传的 ACK 等待器
        self._wall_thread: WallUploadThread | None = None
        self._wall_data: bytes | None = None  # 裁剪对话框导出的 RGB565(优先于自动 contain)

        # 内容包进滚动容器: 卡片多, 窗口放不下时可滚动查看
        scroll = SmoothScrollArea(self)
        scroll.setWidgetResizable(True)
        content = QWidget(scroll)
        root = QVBoxLayout(content)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(10)

        # ===== 卡片 1: WiFi 配置 =====
        card = CardWidget(self)
        form = QVBoxLayout(card)

        form.addWidget(BodyLabel("WiFi SSID:"))
        self.ssid_edit = LineEdit(card)
        self.ssid_edit.setPlaceholderText("路由器名称, 留空 = 不修改")
        self.ssid_edit.setText(self._cfg.get("last_ssid", ""))
        form.addWidget(self.ssid_edit)

        form.addWidget(BodyLabel("WiFi 密码:"))
        self.pwd_edit = LineEdit(card)
        self.pwd_edit.setPlaceholderText("路由器密码, 留空 = 不修改")
        self.pwd_edit.setEchoMode(QLineEdit.Password)
        form.addWidget(self.pwd_edit)

        form.addWidget(BodyLabel("天气城市 (高德 adcode):"))
        row = QHBoxLayout()
        self.city_edit = LineEdit(card)
        self.city_edit.setPlaceholderText("如 440306")
        self.city_edit.setText(self._cfg.get("last_city", "440306"))
        self.city_combo = ComboBox(card)
        self.city_combo.addItems(AMAP_CITY_ITEMS)
        self.city_combo.currentIndexChanged.connect(self._on_city_combo_changed)
        row.addWidget(self.city_edit, 1)
        row.addWidget(self.city_combo)
        form.addLayout(row)

        # 城市名 → 代码 查询行: 输入"深圳"查高德 geocode, 结果填入下拉框
        query_row = QHBoxLayout()
        self.city_query_edit = LineEdit(card)
        self.city_query_edit.setPlaceholderText("输入城市名, 如 深圳 / 宝安 / 上海")
        self.city_query_edit.returnPressed.connect(self._query_city)
        self.btn_city_query = PushButton(FluentIcon.SEARCH, "查询", card)
        self.btn_city_query.clicked.connect(self._query_city)
        query_row.addWidget(BodyLabel("城市查询:"))
        query_row.addWidget(self.city_query_edit, 1)
        query_row.addWidget(self.btn_city_query)
        form.addLayout(query_row)
        query_hint = BodyLabel("查询结果自动填入上方下拉框, 点选后按\"仅改城市\"下发")
        query_hint.setStyleSheet("color: #808080;")
        form.addWidget(query_hint)

        btn_row = QHBoxLayout()
        self.btn_cfg = PrimaryPushButton(FluentIcon.SEND, "保存并下发 (WiFi+城市)", card)
        self.btn_city = PushButton("仅改城市", card)
        self.btn_cfg.clicked.connect(self._send_cfg)
        self.btn_city.clicked.connect(self._send_city)
        btn_row.addWidget(self.btn_cfg)
        btn_row.addWidget(self.btn_city)
        form.addLayout(btn_row)
        root.addWidget(card)

        # ===== 卡片 2: 时钟显示样式 =====
        card_clk = CardWidget(self)
        form_clk = QVBoxLayout(card_clk)

        clk_row = QHBoxLayout()
        self.clock_combo = ComboBox(card_clk)
        self.clock_combo.addItems(["滚动翻页 (默认)", "直接切换 (无动画)"])
        self._set_clock_combo(self._cfg.get("last_clock_anim", "1"))
        self.btn_clock = PrimaryPushButton(FluentIcon.SEND, "下发时钟样式", card_clk)
        self.btn_clock.clicked.connect(self._send_clock)
        clk_row.addWidget(BodyLabel("时间显示样式:"))
        clk_row.addWidget(self.clock_combo, 1)
        clk_row.addWidget(self.btn_clock)
        form_clk.addLayout(clk_row)
        clk_hint = BodyLabel("滚动翻页: 数字上下滚动切换; 直接切换: 数字瞬间变化")
        clk_hint.setStyleSheet("color: #808080;")
        form_clk.addWidget(clk_hint)
        root.addWidget(card_clk)

        # ===== 卡片 3: Claude Code 状态监控 =====
        card2 = CardWidget(self)
        form2 = QVBoxLayout(card2)

        row2 = QHBoxLayout()
        self.chk_mon = SwitchButton("Claude Code 状态监控 (label_1 状态灯)", card2)
        self.chk_mon.checkedChanged.connect(self._toggle_mon)
        row2.addWidget(self.chk_mon)
        row2.addStretch(1)
        form2.addLayout(row2)

        file_row = QHBoxLayout()
        file_row.addWidget(BodyLabel("状态文件:"))
        self.mon_file = LineEdit(card2)
        self.mon_file.setReadOnly(True)
        self.mon_file.setPlaceholderText("选择内容为状态数字(0-4)的 txt 文件")
        self.btn_mon_file = PushButton(FluentIcon.FOLDER, "选择 txt 文件", card2)
        self.btn_mon_file.clicked.connect(self._pick_file)
        file_row.addWidget(self.mon_file, 1)
        file_row.addWidget(self.btn_mon_file)
        form2.addLayout(file_row)
        root.addWidget(card2)

        # ===== 卡片 4: 天气 API (高德/和风 双源) =====
        card3 = CardWidget(self)
        form3 = QVBoxLayout(card3)

        src_row = QHBoxLayout()
        self.wx_combo = ComboBox(card3)
        for _, label in WX_PROVIDERS:
            self.wx_combo.addItem(label)
        self._set_wx_combo(self._cfg.get("last_wxapi", "amap"))
        self.btn_wxapi = PushButton("切换天气源", card3)
        self.btn_wxapi.clicked.connect(self._send_wxapi)
        src_row.addWidget(BodyLabel("天气源:"))
        src_row.addWidget(self.wx_combo, 1)
        src_row.addWidget(self.btn_wxapi)
        form3.addLayout(src_row)
        self.wx_combo.currentIndexChanged.connect(self._on_wx_provider_changed)

        key_row = QHBoxLayout()
        self.wx_key_label = BodyLabel("API Key:", card3)
        self.wx_key_edit = LineEdit(card3)
        self.wx_key_edit.setPlaceholderText("API Key (必填)")
        self.wx_key_edit.setEchoMode(QLineEdit.Password)
        self.wx_key_edit.setText(
            self._cfg.get("last_amap_key", "") if self._current_provider() == "amap"
            else self._cfg.get("last_qw_key", ""))
        self.btn_wxkey = PrimaryPushButton(FluentIcon.SEND, "下发 API Key", card3)
        self.btn_wxkey.clicked.connect(self._send_wxkey)
        key_row.addWidget(self.wx_key_label)
        key_row.addWidget(self.wx_key_edit, 1)
        key_row.addWidget(self.btn_wxkey)
        form3.addLayout(key_row)
        self.wx_key_edit.textChanged.connect(self._on_wx_key_edited)
        self._on_wx_provider_changed(0)      # 初始化输入行状态

        cred_row = QHBoxLayout()
        self.wx_cred_edit = LineEdit(card3)
        self.wx_cred_edit.setPlaceholderText("和风凭据ID (可选, 旧版备用)")
        self.wx_cred_edit.setText(self._cfg.get("last_qw_cred", ""))
        cred_row.addWidget(BodyLabel("凭据ID:"))
        cred_row.addWidget(self.wx_cred_edit, 1)
        form3.addLayout(cred_row)
        root.addWidget(card3)

        # ===== 卡片 5: 壁纸上传 =====
        card_wall = CardWidget(self)
        form_wall = QVBoxLayout(card_wall)

        wall_top = QHBoxLayout()
        self.wall_file = LineEdit(card_wall)
        self.wall_file.setReadOnly(True)
        self.wall_file.setPlaceholderText("选择图片 (JPG/PNG), 裁剪框内缩放/移动后上传")
        self.btn_wall_pick = PushButton(FluentIcon.PHOTO, "选择图片", card_wall)
        self.btn_wall_pick.clicked.connect(self._pick_wall)
        wall_top.addWidget(self.wall_file, 1)
        wall_top.addWidget(self.btn_wall_pick)
        form_wall.addLayout(wall_top)

        self.wall_preview = QLabel(card_wall)
        self.wall_preview.setFixedSize(200, 119)   # 320:190 等比缩到 200 宽
        self.wall_preview.setAlignment(Qt.AlignCenter)
        self.wall_preview.setStyleSheet("border: 1px dashed #555; color: #666;")
        self.wall_preview.setText("预览 320x190")
        self.wall_preview.setScaledContents(True)
        form_wall.addWidget(self.wall_preview, alignment=Qt.AlignCenter)

        wall_btn_row = QHBoxLayout()
        self.btn_wall_test = PushButton(FluentIcon.PHOTO, "测试图", card_wall)
        self.btn_wall_test.setToolTip("上传白底黑框测试图, 用于定位屏幕可视区边界和色偏")
        self.btn_wall_test.clicked.connect(self._start_wall_test)
        self.btn_wall_up = PrimaryPushButton(FluentIcon.UP, "上传到设备", card_wall)
        self.btn_wall_up.setEnabled(False)
        self.btn_wall_up.clicked.connect(self._start_wall_upload)
        self.wall_progress = QProgressBar(card_wall)
        self.wall_progress.setRange(0, 100)
        self.wall_progress.setValue(0)
        self.wall_progress.setFixedWidth(160)
        wall_btn_row.addWidget(self.btn_wall_test)
        wall_btn_row.addWidget(self.btn_wall_up)
        wall_btn_row.addWidget(self.wall_progress)
        wall_btn_row.addStretch(1)
        form_wall.addLayout(wall_btn_row)
        wall_hint = BodyLabel("上传约 2-4 秒 (擦除+写入 flash); 完成后设备立即显示, 重启后自动保留")
        wall_hint.setStyleSheet("color: #808080;")
        form_wall.addWidget(wall_hint)

        # 图播放: 时钟/壁纸定时交替 + 过渡动画 (两个时长各自独立)
        wall_play_row = QHBoxLayout()
        self.chk_wall = SwitchButton("图播放 (时钟/壁纸交替显示)", card_wall)
        self.chk_wall.checkedChanged.connect(self._send_wall_play)
        self.wall_sec_spin = QSpinBox(card_wall)
        self.wall_sec_spin.setRange(1, 60)
        self.wall_sec_spin.setValue(5)                  # 默认图片 5 秒
        self.wall_sec_spin.setToolTip("壁纸图片显示时长 (1-60 秒)")
        self.wall_clock_spin = QSpinBox(card_wall)
        self.wall_clock_spin.setRange(1, 60)
        self.wall_clock_spin.setValue(5)                # 默认时钟 5 秒
        self.wall_clock_spin.setToolTip("时钟页面显示时长 (1-60 秒)")
        self.btn_wall_play = PushButton(FluentIcon.SEND, "下发设置", card_wall)
        self.btn_wall_play.clicked.connect(self._send_wall_play)
        wall_play_row.addWidget(self.chk_wall)
        wall_play_row.addWidget(BodyLabel("图片:"))
        wall_play_row.addWidget(self.wall_sec_spin)
        wall_play_row.addWidget(BodyLabel("时钟:"))
        wall_play_row.addWidget(self.wall_clock_spin)
        wall_play_row.addWidget(self.btn_wall_play)
        wall_play_row.addStretch(1)
        form_wall.addLayout(wall_play_row)

        wall_anim_row = QHBoxLayout()
        self.wall_anim_combo = ComboBox(card_wall)
        self.wall_anim_combo.addItems(WALL_ANIMS)
        wall_anim_row.addWidget(BodyLabel("过渡动画:"))
        wall_anim_row.addWidget(self.wall_anim_combo, 1)
        form_wall.addLayout(wall_anim_row)

        # 时钟背景: 渐变色块(点击调色) + 样式
        bg_row = QHBoxLayout()
        self.btn_bg_c1 = PushButton(card_wall)
        self.btn_bg_c1.setFixedSize(52, 26)
        self.btn_bg_c1.setToolTip("点击选择渐变起色 (左/上)")
        self.btn_bg_c1.clicked.connect(lambda: self._pick_bg_color(1))
        self.btn_bg_c2 = PushButton(card_wall)
        self.btn_bg_c2.setFixedSize(52, 26)
        self.btn_bg_c2.setToolTip("点击选择渐变止色 (右/下)")
        self.btn_bg_c2.clicked.connect(lambda: self._pick_bg_color(2))
        self.bg_dir_combo = ComboBox(card_wall)
        self.bg_dir_combo.addItems(BG_DIRS)
        self.btn_bg_send = PushButton(FluentIcon.SEND, "下发背景", card_wall)
        self.btn_bg_send.clicked.connect(self._send_bg)
        bg_row.addWidget(BodyLabel("时钟背景:"))
        bg_row.addWidget(self.btn_bg_c1)
        bg_row.addWidget(QLabel("→", card_wall))
        bg_row.addWidget(self.btn_bg_c2)
        bg_row.addWidget(self.bg_dir_combo)
        bg_row.addWidget(self.btn_bg_send)
        bg_row.addStretch(1)
        form_wall.addLayout(bg_row)
        wall_play_hint = BodyLabel("开启后: 时钟显示 N 秒 → 按所选动画切到壁纸 N 秒 → 再切回时钟, 循环交替; 需先上传壁纸")
        wall_play_hint.setStyleSheet("color: #808080;")
        form_wall.addWidget(wall_play_hint)
        root.addWidget(card_wall)

        # ===== 卡片 5b: 开机图上传 =====
        card_boot = CardWidget(self)
        form_boot = QVBoxLayout(card_boot)

        boot_top = QHBoxLayout()
        self.chk_boot = SwitchButton("开机图 (上电显示 2 秒)", card_boot)
        self.chk_boot.setChecked(bool(self._cfg.get("last_boot_on", True)))
        self.chk_boot.checkedChanged.connect(self._send_boot_on)
        boot_top.addWidget(self.chk_boot)
        boot_top.addStretch(1)
        form_boot.addLayout(boot_top)

        boot_file_row = QHBoxLayout()
        self.boot_file = LineEdit(card_boot)
        self.boot_file.setReadOnly(True)
        self.boot_file.setPlaceholderText("选择图片 (JPG/PNG), 裁剪框内缩放/移动后上传")
        self.btn_boot_pick = PushButton(FluentIcon.PHOTO, "选择图片", card_boot)
        self.btn_boot_pick.clicked.connect(self._pick_boot)
        boot_file_row.addWidget(self.boot_file, 1)
        boot_file_row.addWidget(self.btn_boot_pick)
        form_boot.addLayout(boot_file_row)

        self.boot_preview = QLabel(card_boot)
        self.boot_preview.setFixedSize(200, 138)   # 320:220 等比缩到 200 宽
        self.boot_preview.setAlignment(Qt.AlignCenter)
        self.boot_preview.setStyleSheet("border: 1px dashed #555; color: #666;")
        self.boot_preview.setText("预览 320x220")
        self.boot_preview.setScaledContents(True)
        form_boot.addWidget(self.boot_preview, alignment=Qt.AlignCenter)

        boot_btn_row = QHBoxLayout()
        self.btn_boot_clear = PushButton(FluentIcon.DELETE, "清除开机图", card_boot)
        self.btn_boot_clear.clicked.connect(self._clear_boot)
        self.btn_boot_up = PrimaryPushButton(FluentIcon.UP, "上传开机图", card_boot)
        self.btn_boot_up.setEnabled(False)
        self.btn_boot_up.clicked.connect(self._start_boot_upload)
        self.boot_progress = QProgressBar(card_boot)
        self.boot_progress.setRange(0, 100)
        self.boot_progress.setValue(0)
        self.boot_progress.setFixedWidth(160)
        boot_btn_row.addWidget(self.btn_boot_clear)
        boot_btn_row.addWidget(self.btn_boot_up)
        boot_btn_row.addWidget(self.boot_progress)
        boot_btn_row.addStretch(1)
        form_boot.addLayout(boot_btn_row)
        boot_hint = BodyLabel("上传约 2-4 秒; 开机图在时钟之前全屏显示 2 秒, 开关可随时启用/停用")
        boot_hint.setStyleSheet("color: #808080;")
        form_boot.addWidget(boot_hint)
        root.addWidget(card_boot)

        # ===== 下发结果 =====
        root.addWidget(BodyLabel("下发结果:"))
        self.result = TextEdit(self)
        self.result.setReadOnly(True)
        self.result.document().setMaximumBlockCount(500)
        self.result.setStyleSheet("font-family: 'Cascadia Mono', Consolas, monospace;")
        root.addWidget(self.result, 1)

        scroll.setWidget(content)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(scroll)

        # 定时读取状态文件 (1s)
        self._mon_timer = QTimer(self)
        self._mon_timer.setInterval(1000)
        self._mon_timer.timeout.connect(self._poll_status_file)
        self._last_status: int | None = None

        # 恢复上次状态 (放在 result 创建之后, 避免 setChecked 触发回调时访问未创建控件)
        if self._cfg.get("mon_file"):
            self.mon_file.setText(self._cfg["mon_file"])
        if self._cfg.get("mon_on"):
            self.chk_mon.setChecked(True)
        if self._cfg.get("last_wall_sec"):
            self.wall_sec_spin.setValue(max(1, min(60, int(self._cfg["last_wall_sec"]))))
        if self._cfg.get("last_wall_clock_sec"):
            self.wall_clock_spin.setValue(max(1, min(60, int(self._cfg["last_wall_clock_sec"]))))
        if self._cfg.get("last_wall_anim"):
            idx = int(self._cfg["last_wall_anim"])
            if 0 <= idx < len(WALL_ANIMS):
                self.wall_anim_combo.setCurrentIndex(idx)
        if self._cfg.get("last_wall_mode"):
            self.chk_wall.setChecked(True)      # 触发 checkedChanged → 立即下发恢复

        # 背景状态恢复 (默认 = 固件默认: 淡蓝→淡紫垂直渐变)
        self._bg_c1 = int(self._cfg.get("last_bg_c1", DEFAULT_BG_C1))
        self._bg_c2 = int(self._cfg.get("last_bg_c2", DEFAULT_BG_C2))
        d = int(self._cfg.get("last_bg_dir", 2))
        if 0 <= d < len(BG_DIRS):
            self.bg_dir_combo.setCurrentIndex(d)
        self._set_bg_btn(self.btn_bg_c1, self._bg_c1)
        self._set_bg_btn(self.btn_bg_c2, self._bg_c2)

    # ---- WiFi ----
    def _send_cfg(self):
        ssid = self.ssid_edit.text().strip()
        pwd = self.pwd_edit.text().strip()
        city = self.city_edit.text().strip()
        if self._current_provider() == "openmeteo":
            # 统一以"城市名"输入框为准(城市卡片是只读同步显示)
            city = self.wx_key_edit.text().strip() or city
        if not ssid and not pwd and not city:
            self.result.append("[工具] 至少填写一项才能下发")
            return
        if city and self._current_provider() == "openmeteo" and city.isdigit():
            self.result.append("[工具] Open-Meteo 需要中文城市名(如\"深圳\"), 数字 adcode 不识别")
            InfoBar.error("城市格式错误", "Open-Meteo 请输入中文城市名", parent=self,
                          position=InfoBarPosition.TOP_RIGHT)
            return
        self._cfg["last_ssid"] = ssid
        self._cfg["last_city"] = city
        save(self._cfg)
        self._on_send(build_cfg(ssid, pwd, city))
        self.result.append(
            f"TX→ #XCFG (ssid={ssid or '(不变)'}, pwd={'*' * len(pwd) if pwd else '(不变)'}, city={city or '(不变)'})")

    def _send_city(self):
        city = self.city_edit.text().strip()
        if self._current_provider() == "openmeteo":
            # 统一以"城市名"输入框为准(城市卡片是只读同步显示)
            city = self.wx_key_edit.text().strip() or city
        if not city:
            self.result.append("[工具] 城市不能为空")
            return
        if self._current_provider() == "openmeteo" and city.isdigit():
            self.result.append("[工具] Open-Meteo 需要中文城市名(如\"深圳\"), 数字 adcode 不识别")
            InfoBar.error("城市格式错误", "Open-Meteo 请输入中文城市名", parent=self,
                          position=InfoBarPosition.TOP_RIGHT)
            return
        self._cfg["last_city"] = city
        save(self._cfg)
        self._on_send(build_city(city))
        self.result.append(f"TX→ #XCITY ({city})")

    # ---- 时钟显示样式 ----
    def _set_clock_combo(self, v):
        self.clock_combo.setCurrentIndex(1 if str(v) == "0" else 0)

    def _send_clock(self):
        anim = 0 if self.clock_combo.currentIndex() == 1 else 1
        self._cfg["last_clock_anim"] = str(anim)
        save(self._cfg)
        self._on_send(build_clock(anim))
        self.result.append(f"TX→ #XCLOCK ({'滚动翻页' if anim else '直接切换'})")

    def _on_city_combo_changed(self, _i: int):
        """下拉选城市 → 填代码到输入框(空下拉时忽略, 防 clear 触发)"""
        text = self.city_combo.currentText()
        if text:
            self.city_edit.setText(text.split()[0])

    def _query_city(self):
        """城市名 → 高德 geocode 查询, 结果更新下拉框候选"""
        name = self.city_query_edit.text().strip()
        if not name:
            self.result.append("[工具] 请输入城市名")
            return
        if self._current_provider() == "openmeteo":
            self.result.append("[工具] Open-Meteo 免 Key: 设备联网后自动反查城市, 无需手动查询")
            return
        key = ""
        if self._current_provider() == "amap":
            key = self.wx_key_edit.text().strip()
        if not key:
            key = self._cfg.get("last_amap_key", "")
        if not key:
            self.result.append("[工具] 未填写高德 Key: 先在天气 API 卡片填写并下发")
            InfoBar.error("缺少高德 Key", "请先在天气 API 卡片填写 API Key 并下发",
                          parent=self, position=InfoBarPosition.TOP_RIGHT)
            return
        self.btn_city_query.setEnabled(False)
        self.btn_city_query.setText("查询中...")
        self._city_thread = CityQueryThread(name, key, self)
        self._city_thread.done.connect(self._on_city_queried)
        self._city_thread.start()

    def _on_city_queried(self, items: list, error: str):
        self.btn_city_query.setEnabled(True)
        self.btn_city_query.setText("查询")
        if error:
            self.result.append(f"[工具] 城市查询失败: {error}")
            InfoBar.error("城市查询失败", error, parent=self,
                          position=InfoBarPosition.TOP_RIGHT)
            return
        if not items:
            self.result.append("[工具] 未找到匹配城市")
            InfoBar.warning("无匹配", "换个更具体的城市名试试", parent=self,
                            position=InfoBarPosition.TOP_RIGHT)
            return
        self.city_combo.blockSignals(True)
        self.city_combo.clear()
        self.city_combo.addItems(items)
        self.city_combo.setCurrentIndex(0)
        self.city_combo.blockSignals(False)
        self._on_city_combo_changed(0)              # 自动填第一个候选
        self.result.append(f"[工具] 查询到 {len(items)} 个候选: {', '.join(items)}")
        InfoBar.success(f"查询到 {len(items)} 个候选",
                        "点选下拉框或直接\"仅改城市\"下发", parent=self,
                        position=InfoBarPosition.TOP_RIGHT)
        self.city_combo.showPopup()                 # 展开下拉供选择

    # ---- 壁纸上传 ----
    def _pick_wall(self):
        from PySide6.QtWidgets import QFileDialog
        path, _ = QFileDialog.getOpenFileName(self, "选择壁纸图片", "",
                                              "图片 (*.png *.jpg *.jpeg *.bmp);;所有文件 (*)")
        if not path:
            return
        self.wall_file.setText(path)
        self._cfg["last_wall"] = path
        save(self._cfg)
        try:
            dlg = WallCropDialog(path, self)
            if dlg.exec() != QDialog.Accepted:
                self.result.append("[工具] 已取消裁剪, 未选壁纸")
                return
            data = dlg.export()                 # 框内内容 → RGB565
            self._wall_data = data
            self.wall_preview.setPixmap(QPixmap.fromImage(rgb565_to_qimage(data)))
            self.btn_wall_up.setEnabled(True)
            self.result.append(f"[工具] 已调整图片: {path} ({len(data)} 字节 RGB565)")
        except Exception as e:
            self.result.append(f"[工具] 图片读取失败: {e}")
            InfoBar.error("图片读取失败", str(e), parent=self,
                          position=InfoBarPosition.TOP_RIGHT)

    def _start_wall_upload(self):
        if self._wall_data is not None:         # 优先: 裁剪框导出的内容
            self._start_wall_upload_data(self._wall_data, "壁纸")
            return
        path = self.wall_file.text().strip()    # 兜底: 未走裁剪(旧数据)自动 contain
        if not path:
            return
        try:
            data = process_image(path)
        except Exception as e:
            self.result.append(f"[工具] 图片处理失败: {e}")
            InfoBar.error("图片处理失败", str(e), parent=self,
                          position=InfoBarPosition.TOP_RIGHT)
            return
        self._start_wall_upload_data(data, "壁纸")

    def _start_wall_test(self):
        data = build_test_image()
        self._start_wall_upload_data(data, "测试图")

    def _start_wall_upload_data(self, data: bytes, name: str):
        self.btn_wall_up.setEnabled(False)
        self.wall_progress.setValue(0)
        self.result.append(f"[工具] 开始上传{name} ({len(data)} 字节, {(len(data) + CHUNK - 1) // CHUNK} 块)...")
        self._wall_thread = WallUploadThread(data, self._on_send,
                                             self._wall_waiter, self)
        self._wall_thread.progress.connect(self._on_wall_progress)
        self._wall_thread.succeeded.connect(self._on_wall_done)
        self._wall_thread.failed.connect(self._on_wall_failed)
        self._wall_thread.retry_note.connect(
            lambda m: self.result.append(f"[工具] {m}"))
        self._wall_thread.start()

    def _on_wall_progress(self, done: int, total: int):
        self.wall_progress.setValue(int(done * 100 / total) if total else 0)

    def _on_wall_done(self):
        self.btn_wall_up.setEnabled(True)
        self.result.append("[工具] 壁纸上传完成, 设备已切换显示")
        InfoBar.success("壁纸上传完成", "设备已显示新壁纸", parent=self,
                        position=InfoBarPosition.TOP_RIGHT)

    def _on_wall_failed(self, msg: str):
        self.btn_wall_up.setEnabled(True)
        self.result.append(f"[工具] 壁纸上传失败: {msg}")
        InfoBar.error("壁纸上传失败", msg, parent=self,
                      position=InfoBarPosition.TOP_RIGHT)

    # ---- 开机图上传 (320x220 全屏, 裁剪框调整) ----
    def _pick_boot(self):
        from PySide6.QtWidgets import QFileDialog, QDialog
        from app.wall_crop import WallCropDialog, rgb565_to_qimage
        path, _ = QFileDialog.getOpenFileName(self, "选择开机图", "",
                                              "图片 (*.png *.jpg *.jpeg *.bmp);;所有文件 (*)")
        if not path:
            return
        self.boot_file.setText(path)
        self._cfg["last_boot"] = path
        save(self._cfg)
        try:
            dlg = WallCropDialog(path, BOOT_W, BOOT_H, self)
            if dlg.exec() != QDialog.Accepted:
                self.result.append("[工具] 已取消裁剪, 未选开机图")
                return
            data = dlg.export()
            self._boot_data = data
            self.boot_preview.setPixmap(
                QPixmap.fromImage(rgb565_to_qimage(data, BOOT_W, BOOT_H)))
            self.btn_boot_up.setEnabled(True)
            self.result.append(f"[工具] 已调整开机图: {path} ({len(data)} 字节 RGB565)")
        except Exception as e:
            self.result.append(f"[工具] 图片读取失败: {e}")
            InfoBar.error("图片读取失败", str(e), parent=self,
                          position=InfoBarPosition.TOP_RIGHT)

    def _send_boot_on(self, on: bool):
        from app.protocol import build_bootimg
        self._cfg["last_boot_on"] = on
        save(self._cfg)
        self._on_send(build_bootimg(on))
        self.result.append(f"[工具] 开机图{'已开启' if on else '已关闭'}")

    def _start_boot_upload(self):
        if getattr(self, "_boot_data", None) is None:
            self.result.append("[工具] 请先选择开机图")
            return
        self.btn_boot_up.setEnabled(False)
        self.boot_progress.setValue(0)
        self.result.append(f"[工具] 开始上传开机图 ({len(self._boot_data)} 字节, "
                           f"{(len(self._boot_data) + CHUNK - 1) // CHUNK} 块)...")
        self._boot_thread = WallUploadThread(self._boot_data, self._on_send,
                                             self._wall_waiter, img_type=1, parent=self)
        self._boot_thread.progress.connect(
            lambda d, t: self.boot_progress.setValue(int(d * 100 / t) if t else 0))
        self._boot_thread.succeeded.connect(self._on_boot_done)
        self._boot_thread.failed.connect(self._on_boot_failed)
        self._boot_thread.retry_note.connect(
            lambda m: self.result.append(f"[工具] {m}"))
        self._boot_thread.start()

    def _on_boot_done(self):
        self.btn_boot_up.setEnabled(True)
        self.result.append("[工具] 开机图上传完成, 下次上电显示 2 秒")
        InfoBar.success("开机图上传完成", "下次上电时显示 2 秒", parent=self,
                        position=InfoBarPosition.TOP_RIGHT)

    def _on_boot_failed(self, msg: str):
        self.btn_boot_up.setEnabled(True)
        self.result.append(f"[工具] 开机图上传失败: {msg}")
        InfoBar.error("开机图上传失败", msg, parent=self,
                      position=InfoBarPosition.TOP_RIGHT)

    def _clear_boot(self):
        from app.protocol import build_bootclr
        self._on_send(build_bootclr())
        self.result.append("[工具] 已发送清除开机图命令")

    # ---- 图播放 ----
    def _send_wall_play(self, *_args):
        mode = 1 if self.chk_wall.isChecked() else 0
        sec = self.wall_sec_spin.value()
        clock_sec = self.wall_clock_spin.value()
        anim = self.wall_anim_combo.currentIndex()
        self._cfg["last_wall_mode"] = mode
        self._cfg["last_wall_sec"] = sec
        self._cfg["last_wall_clock_sec"] = clock_sec
        self._cfg["last_wall_anim"] = anim
        save(self._cfg)
        self._on_send(build_wall(mode, sec, anim, clock_sec))
        self.result.append(
            f"TX→ #XWALL (图播放={'开' if mode else '关'}, 图片 {sec} 秒, 时钟 {clock_sec} 秒, "
            f"动画: {WALL_ANIMS[anim]})")

    # ---- 时钟背景 ----
    @staticmethod
    def _set_bg_btn(btn, color: int):
        btn.setStyleSheet(f"background-color: #{color & 0xFFFFFF:06x}; border: 1px solid #888;")

    def _pick_bg_color(self, which: int):
        cur = self._bg_c1 if which == 1 else self._bg_c2
        col = QColorDialog.getColor(QColor(cur), self, "选择颜色")
        if not col.isValid():
            return
        rgb = col.rgb() & 0xFFFFFF
        if which == 1:
            self._bg_c1 = rgb
            self._set_bg_btn(self.btn_bg_c1, rgb)
        else:
            self._bg_c2 = rgb
            self._set_bg_btn(self.btn_bg_c2, rgb)

    def _send_bg(self, *_args):
        grad = self.bg_dir_combo.currentIndex()
        self._cfg["last_bg_c1"] = self._bg_c1
        self._cfg["last_bg_c2"] = self._bg_c2
        self._cfg["last_bg_dir"] = grad
        save(self._cfg)
        self._on_send(build_bg(self._bg_c1, self._bg_c2, grad))
        self.result.append(
            f"TX→ #XBG (时钟背景: #{self._bg_c1:06x} → #{self._bg_c2:06x}, {BG_DIRS[grad]})")

    # ---- 天气 API ----
    def _current_provider(self) -> str:
        idx = self.wx_combo.currentIndex()
        return WX_PROVIDERS[idx][0] if 0 <= idx < len(WX_PROVIDERS) else "amap"

    def _set_wx_combo(self, provider: str):
        for i, (p, _) in enumerate(WX_PROVIDERS):
            if p == provider:
                self.wx_combo.setCurrentIndex(i)
                return

    def _on_wx_provider_changed(self, _i: int):
        """切源时: 下方输入行按源切换 — Open-Meteo 变\"城市名\", 高德/和风变 API Key;
        城市下拉: Open-Meteo 用 334 地级市中文名, 高德/和风用 adcode 候选"""
        prov = self._current_provider()
        if prov == "openmeteo":
            self.wx_key_label.setText("城市名:")
            self.wx_key_edit.setEnabled(True)
            self.wx_key_edit.setEchoMode(QLineEdit.Normal)
            self.wx_key_edit.setPlaceholderText("输入城市名, 如 深圳")
            if self.wx_key_edit.text().strip().isdigit():
                self.wx_key_edit.clear()        # 旧 adcode 数字对 Open-Meteo 无效
            self.btn_wxkey.setText("下发城市")
            self.btn_city_query.setEnabled(False)
            self.city_edit.setReadOnly(True)    # 统一以"城市名"框为准
            self.city_edit.setPlaceholderText("城市名 (同步上方输入)")
            self.city_combo.blockSignals(True)
            self.city_combo.clear()
            self.city_combo.addItems(CITIES)
            self.city_combo.blockSignals(False)
            if self.city_edit.text().strip().isdigit():
                self.city_edit.clear()
            if self.wx_key_edit.text().strip() and not self.wx_key_edit.text().strip().isdigit():
                self.city_edit.setText(self.wx_key_edit.text().strip())
        else:
            self.wx_key_label.setText("API Key:")
            self.wx_key_edit.setEnabled(True)
            self.wx_key_edit.setEchoMode(QLineEdit.Password)
            self.wx_key_edit.setPlaceholderText("API Key (必填)")
            key = (self._cfg.get("last_amap_key", "") if prov == "amap"
                   else self._cfg.get("last_qw_key", ""))
            self.wx_key_edit.setText(key)
            self.btn_wxkey.setText("下发 API Key")
            self.btn_city_query.setEnabled(True)
            self.city_edit.setReadOnly(False)
            self.city_edit.setPlaceholderText("如 440306")
            self.city_combo.blockSignals(True)
            self.city_combo.clear()
            self.city_combo.addItems(AMAP_CITY_ITEMS)
            self.city_combo.blockSignals(False)

    def _send_wxapi(self):
        prov = self._current_provider()
        self._cfg["last_wxapi"] = prov
        save(self._cfg)
        self._on_send(build_wxapi(prov))
        self.result.append(f"TX→ #XWXAPI ({prov})")

    def _on_wx_key_edited(self, text: str):
        """Open-Meteo 模式: 城市名输入框与上方城市卡片实时同步"""
        if self._current_provider() == "openmeteo":
            self.city_edit.setText(text.strip())

    def _send_wxkey(self):
        prov = self._current_provider()
        key = self.wx_key_edit.text().strip()
        if not key:
            self.result.append("[工具] API Key 不能为空")
            return
        if prov == "openmeteo":
            # Open-Meteo 免 Key: 该输入行变城市名, 直接下发 #XCITY
            if key.isdigit():
                self.result.append("[工具] Open-Meteo 需要中文城市名(如\"深圳\"), 数字 adcode 不识别")
                InfoBar.error("城市格式错误", "请输入中文城市名", parent=self,
                              position=InfoBarPosition.TOP_RIGHT)
                return
            self.city_edit.setText(key)
            self._cfg["last_city"] = key
            save(self._cfg)
            self._on_send(build_city(key))
            self.result.append(f"TX→ #XCITY ({key})")
            return
        cred = self.wx_cred_edit.text().strip()
        if prov == "amap":
            self._cfg["last_amap_key"] = key
        else:
            self._cfg["last_qw_key"] = key
        if cred:
            self._cfg["last_qw_cred"] = cred
        save(self._cfg)
        self._on_send(build_wxkey(prov, key, cred))
        self.result.append(f"TX→ #XWXKEY ({prov}, key=***, cred={cred or '(无)'})")

    # ---- Claude Code 监控 ----
    def _pick_file(self):
        from PySide6.QtWidgets import QFileDialog
        path, _ = QFileDialog.getOpenFileName(self, "选择状态 txt 文件", "",
                                              "文本文件 (*.txt);;所有文件 (*)")
        if path:
            self.mon_file.setText(path)
            self._cfg["mon_file"] = path
            save(self._cfg)

    def _poll_status_file(self):
        """定时读取状态文件, 内容为 0-4 数字, 变化时下发 #XLAMP"""
        path = self.mon_file.text().strip()
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read().strip()
        except Exception:
            return
        status = None
        for ch in content:
            if ch in "01234":
                status = int(ch)
                break
        if status is None:
            return
        if status != self._last_status:
            self._last_status = status
            self._on_send(build_lamp(status))
            self.result.append(f"TX→ #XLAMP,{status}")

    def _toggle_mon(self, checked: bool):
        if checked:
            path = self.mon_file.text().strip()
            if not path:
                self._pick_file()                    # 没选过文件: 自动弹出选择框
                path = self.mon_file.text().strip()
            if not path:
                self.result.append("[工具] 未选择状态文件, 监控未开启")
                self.chk_mon.setChecked(False)
                return
            self._cfg["mon_on"] = True
            self._cfg["mon_file"] = path
            save(self._cfg)
            self._on_send(build_mon(True))
            self.result.append("TX→ #XMON,1  (Claude Code 状态监控开启)")
            InfoBar.success("监控已开启", path, parent=self, position=InfoBarPosition.TOP_RIGHT)
            self._last_status = None
            self._poll_status_file()
            self._mon_timer.start()
        else:
            self._cfg["mon_on"] = False
            save(self._cfg)
            self._on_send(build_mon(False))
            self.result.append("TX→ #XMON,0  (Claude Code 状态监控关闭)")
            self._mon_timer.stop()
            self._last_status = None

    # ---- 来自主窗口的行分发 ----
    def on_ack(self, result: str, cmd: str, data: list[str]):
        if cmd in ("CFG", "CITY", "MON", "LAMP", "WXAPI", "WXKEY",
                   "WXCITY", "CITYLIST", "CLOCK", "WALL"):
            self.result.append(f"RX← #XA,{result},{cmd}")
            if result == "OK":
                InfoBar.success(f"{cmd} 下发成功", "", parent=self,
                                position=InfoBarPosition.TOP_RIGHT)
            else:
                InfoBar.error(f"{cmd} 下发失败", "", parent=self,
                              position=InfoBarPosition.TOP_RIGHT)
        elif cmd in ("IMGSTART", "IMG", "IMGEND"):
            self._wall_waiter.notify(cmd, result, data)   # 唤醒上传线程(带 ERR 详情)
            if cmd in ("IMGSTART", "IMGEND"):
                self.result.append(f"RX← #XA,{result},{cmd}")
