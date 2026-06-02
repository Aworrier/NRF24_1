# -*- coding: utf-8 -*-
import re
import threading
import socket
import json
import argparse
from pathlib import Path
import tkinter as tk
from tkinter import ttk, messagebox, scrolledtext
from datetime import datetime, timedelta

import serial
from serial.tools import list_ports

# ============================================================
# 颜色与样式配置
# ============================================================
_C_BG = "#f5f6fa"
_C_CARD_BG = "#ffffff"
_C_HEADER_BG = "#2c3e50"
_C_HEADER_FG = "#ffffff"
_C_ACCENT = "#2980b9"
_C_ACCENT_HOVER = "#3498db"
_C_SUCCESS = "#27ae60"
_C_DANGER = "#c0392b"
_C_DANGER_HOVER = "#e74c3c"
_C_TEXT_PRI = "#2d3436"
_C_TEXT_SEC = "#636e72"
_C_BORDER = "#dcdde1"
_C_INPUT_BG = "#ffffff"
_C_STAT_BG = "#f8f9fa"
_C_LOG_BG = "#1e1e1e"
_C_LOG_FG = "#dfe6e9"

DEV_RX = "rx"
DEV_JAM = "jam"


class DeviceConnection:
    """管理与单个 NRF24 设备的串口或 TCP 物理连接"""

    def __init__(self, name: str, on_line, on_error, on_disconnect):
        self.name = name
        self.on_line = on_line          # 回调: (text: str) -> None
        self.on_error = on_error        # 回调: (msg: str) -> None
        self.on_disconnect = on_disconnect  # 回调: () -> None
        self.ser = None
        self.sock = None
        self.conn_type = None           # "SERIAL" 或 "TCP"
        self._stop = threading.Event()
        self._thread = None

    def connect_serial(self, port: str, baud: int) -> None:
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
        self.conn_type = "SERIAL"
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def connect_tcp(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.2)
        self.conn_type = "TCP"
        self._stop.clear()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def disconnect(self) -> None:
        self._stop.set()
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except Exception:
                pass
            try:
                self.sock.close()
            except Exception:
                pass
        self.ser = None
        self.sock = None
        self.conn_type = None

    def send_line(self, line: str) -> None:
        # 统一使用 utf-8 编码发送，防止中文字符集转换异常
        data = (line + "\n").encode("utf-8")
        if self.conn_type == "SERIAL" and self.ser is not None:
            self.ser.write(data)
        elif self.conn_type == "TCP" and self.sock is not None:
            self.sock.sendall(data)

    def is_connected(self) -> bool:
        return self.conn_type is not None

    def _reader_loop(self) -> None:
        reconnect_delay_s = 2.0
        intentional = False
        while not self._stop.is_set():
            try:
                if self.conn_type == "SERIAL":
                    if self.ser is None:
                        break
                    data = self.ser.readline()
                elif self.conn_type == "TCP":
                    if self.sock is None:
                        break
                    data = self._socket_read_line()
                else:
                    break
                if not data:
                    continue
                # 统一以 utf-8 解码，无法解析的字符替换为替代字符，防止中断运行
                text = data.decode("utf-8", errors="replace").strip()
                if text:
                    self.on_line(text)
            except serial.SerialException as exc:
                self.on_error(f"串口错误: {exc}")
                if self._stop.is_set():
                    break
                self._stop.wait(timeout=reconnect_delay_s)
            except OSError as exc:
                self.on_error(f"I/O 错误: {exc}")
                if self._stop.is_set():
                    break
                self._stop.wait(timeout=2.0)
            except Exception as exc:
                self.on_error(f"读取线程异常: {exc}")
                break
        else:
            intentional = True
        if not intentional:
            self.on_disconnect()

    def _socket_read_line(self) -> bytes:
        if self.sock is None:
            return b""
        chunks = []
        while not self._stop.is_set():
            try:
                data = self.sock.recv(1)
            except socket.timeout:
                if chunks:
                    continue
                return b""
            except Exception:
                return b""
            if not data:
                return b""
            ch = data.decode("utf-8", errors="ignore")
            if ch == "\r":
                continue
            if ch == "\n":
                return "".join(chunks).encode("utf-8")
            chunks.append(ch)
        return b""


class ScrollableFrame(ttk.Frame):
    """支持垂直滚动的容器组件"""
    def __init__(self, container, *args, **kwargs):
        super().__init__(container, *args, **kwargs)
        self.canvas = tk.Canvas(self, borderwidth=0, background=_C_BG, highlightthickness=0)
        self.scrollbar = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.scrollable_frame = ttk.Frame(self.canvas, style="Tab.TFrame")

        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        self.canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")
        self.canvas.configure(yscrollcommand=self.scrollbar.set)

        self.canvas.pack(side="left", fill="both", expand=True)
        self.scrollbar.pack(side="right", fill="y")


class Nrf24ControllerApp:
    def __init__(self, root: tk.Tk, initial_config: dict | None = None) -> None:
        self.root = root
        self.root.title("NRF24 多设备综合控制台")
        self.root.geometry("1440x900")
        self.root.minsize(1200, 800)
        self.root.configure(bg=_C_BG)

        self.config_path = Path.home() / ".nrf24_multi_controller_gui.json"
        self.poll_interval_ms = 1000

        self.devices_state = {}  # 存放所有设备的状态 (键名如 'rx', 'jam', 'tx_1', 'tx_2'...)
        self.tx_counter = 0

        self._setup_styles()
        self._build_ui()

        # 初始化默认设备（1个RX，1个JAM，默认创建1个TX）
        self._add_rx_state()
        self._add_jam_state()
        self._load_persisted_config_or_defaults()

        # 构建右侧静态面板和左侧TX列表
        self._build_rx_panel()
        self._build_jam_panel()
        self._render_all_tx_panels()

        self._start_global_poll()

    def _setup_styles(self) -> None:
        style = ttk.Style()
        style.theme_use("clam")

        style.configure("Header.TFrame", background=_C_HEADER_BG)
        style.configure("Header.TLabel",
                        background=_C_HEADER_BG, foreground=_C_HEADER_FG,
                        font=("Helvetica", 13, "bold"), padding=(0, 4))

        style.configure("Card.TLabelframe",
                        background=_C_CARD_BG, bordercolor=_C_BORDER,
                        borderwidth=1, relief="solid", padding=4)
        style.configure("Card.TLabelframe.Label",
                        foreground=_C_TEXT_PRI, background=_C_BG,
                        font=("Helvetica", 9, "bold"), padding=(4, 2))

        style.configure("CardContent.TFrame", background=_C_CARD_BG)
        style.configure("Tab.TFrame", background=_C_BG)

        style.configure("TButton",
                        background=_C_ACCENT, foreground="white",
                        borderwidth=0, focuscolor="none",
                        font=("Helvetica", 9), padding=(6, 3))
        style.map("TButton",
                  background=[("active", _C_ACCENT_HOVER), ("pressed", "#1f618d")],
                  foreground=[("active", "white")])

        style.configure("Secondary.TButton",
                        background="#eaedf1", foreground=_C_TEXT_PRI,
                        borderwidth=0, focuscolor="none",
                        font=("Helvetica", 9), padding=(6, 3))
        style.map("Secondary.TButton",
                  background=[("active", "#d5dbdb")])

        style.configure("Danger.TButton",
                        background=_C_DANGER, foreground="white",
                        borderwidth=0, focuscolor="none",
                        font=("Helvetica", 9, "bold"), padding=(6, 3))
        style.map("Danger.TButton",
                  background=[("active", _C_DANGER_HOVER)])

        style.configure("Field.TLabel",
                        foreground=_C_TEXT_SEC, background=_C_CARD_BG,
                        font=("Helvetica", 9))
        style.configure("Value.TLabel",
                        foreground=_C_TEXT_PRI, background=_C_STAT_BG,
                        font=("Helvetica", 9, "bold"),
                        relief="solid", bordercolor="#e9ecef", borderwidth=1)

    def _build_ui(self) -> None:
        # 顶端导航栏
        header = ttk.Frame(self.root, style="Header.TFrame")
        header.pack(fill=tk.X, side=tk.TOP)

        ttk.Label(header, text="  NRF24 多设备综合控制台 (多TX版)", style="Header.TLabel").pack(side=tk.LEFT, padx=10, pady=8)

        # 全局操作组
        global_btn_frame = ttk.Frame(header, style="Header.TFrame")
        global_btn_frame.pack(side=tk.RIGHT, padx=10, pady=4)

        ttk.Button(global_btn_frame, text="✚ 添加发端 (TX)", command=self._add_new_tx, style="TButton").pack(side=tk.LEFT, padx=5)
        ttk.Button(global_btn_frame, text="↻ 刷新可用串口", command=self._refresh_all_combos, style="Secondary.TButton").pack(side=tk.LEFT, padx=5)
        ttk.Button(global_btn_frame, text="保存当前配置", command=self._save_persisted_config, style="Secondary.TButton").pack(side=tk.LEFT, padx=5)

        # 主工作区：三栏布局 (修改 background 传参方式为 style)
        main_workspace = ttk.Frame(self.root, style="Tab.TFrame")
        main_workspace.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

        # 1. 左侧 TX 栏 (可支持多个 TX 面板垂直排列滚动)
        tx_outer_frame = ttk.LabelFrame(main_workspace, text=" 发送端 (TX) 设备列表 ", style="Card.TLabelframe")
        tx_outer_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=4, pady=4)

        self.tx_scroll_container = ScrollableFrame(tx_outer_frame)
        self.tx_scroll_container.pack(fill=tk.BOTH, expand=True)

        # 2. 中间 RX 栏
        self.rx_outer_frame = ttk.LabelFrame(main_workspace, text=" 接收端 (RX) 控制面板 ", style="Card.TLabelframe", width=340)
        self.rx_outer_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=False, padx=4, pady=4)
        self.rx_outer_frame.pack_propagate(False)

        # 3. 右侧 JAM 栏
        self.jam_outer_frame = ttk.LabelFrame(main_workspace, text=" 干扰源 (JAM) 控制面板 ", style="Card.TLabelframe", width=340)
        self.jam_outer_frame.pack(side=tk.LEFT, fill=tk.BOTH, expand=False, padx=4, pady=4)
        self.jam_outer_frame.pack_propagate(False)

        # 底部日志框
        log_frame = ttk.LabelFrame(self.root, text=" 综合系统日志终端 ", style="Card.TLabelframe")
        log_frame.pack(fill=tk.X, side=tk.BOTTOM, padx=12, pady=(2, 10))

        self.log = scrolledtext.ScrolledText(
            log_frame, height=9, font=("Courier New", 9),
            bg=_C_LOG_BG, fg=_C_LOG_FG, insertbackground="white",
            relief=tk.FLAT, borderwidth=0)
        self.log.pack(fill=tk.BOTH, expand=True, padx=6, pady=4)

        self.log.tag_config("system", foreground="#95a5a6")
        self.log.tag_config("rx", foreground="#2ecc71")
        self.log.tag_config("jam", foreground="#e74c3c")
        self.log.tag_config("tx", foreground="#3498db")

        self._system_log("多设备控制系统初始化完成。")

    # ============================================================
    # 设备状态生成
    # ============================================================
    def _make_base_state(self, dev_id: str, default_mode="ALOHA") -> dict:
        return {
            "conn": None,
            "conn_mode_var": tk.StringVar(value="SERIAL"),
            "port_var": tk.StringVar(),
            "baud_var": tk.StringVar(value="115200"),
            "host_var": tk.StringVar(value="192.168.4.1"),
            "tcp_port_var": tk.StringVar(value="3333"),
            "token_var": tk.StringVar(value="nrf24"),
            "enable_var": tk.BooleanVar(value=True),
            "mac_mode_var": tk.StringVar(value=default_mode),
            "mac_q_var": tk.StringVar(value="100"),
            "slot_ms_var": tk.StringVar(value="2"),
            "csma_win_var": tk.StringVar(value="4"),
            "slot_limit_var": tk.StringVar(value="0"),
            "count_var": tk.StringVar(value="10"),
            "interval_var": tk.StringVar(value="0"),
            "mode_var": tk.StringVar(value="ASCII"),
            "payload_var": tk.StringVar(value="HELLO_NRF24"),
            "auto_poll_var": tk.BooleanVar(value=True),
            "schedule_time_var": tk.StringVar(value="12:00:00"),
            "scheduled_send_job": None,
            "slot_statistics": [],
            "is_collecting": False,
            "xmit_summary": tk.StringVar(value="等待发送指令..."),
            "stat_vars": {
                "role": tk.StringVar(value="-"),
                "ack_ok": tk.StringVar(value="0"),
                "ack_fail": tk.StringVar(value="0"),
                "sent": tk.StringVar(value="0"),
                "retries_sum": tk.StringVar(value="0"),
                "retries_max": tk.StringVar(value="0"),
                "rx_pkt": tk.StringVar(value="0"),
                "frame_ok": tk.StringVar(value="0"),
                "crc_fail": tk.StringVar(value="0"),
                "magic_fail": tk.StringVar(value="0"),
                "len_fail": tk.StringVar(value="0"),
                "last_seq": tk.StringVar(value="-"),
                "gap": tk.StringVar(value="0"),
                "dup": tk.StringVar(value="0"),
                "ooo": tk.StringVar(value="0"),
                "mac": tk.StringVar(value="-"),
                "q": tk.StringVar(value="-"),
                "slot_ms": tk.StringVar(value="-"),
                "csma_win": tk.StringVar(value="-"),
                "slot_limit": tk.StringVar(value="-"),
            },
            "widgets": {},
        }

    def _add_rx_state(self) -> None:
        self.devices_state[DEV_RX] = self._make_base_state(DEV_RX)
        self.devices_state[DEV_RX]["conn"] = self._create_device_connection(DEV_RX)

    def _add_jam_state(self) -> None:
        self.devices_state[DEV_JAM] = self._make_base_state(DEV_JAM, "CSMA")
        self.devices_state[DEV_JAM]["conn"] = self._create_device_connection(DEV_JAM)

    def _add_new_tx(self) -> str:
        self.tx_counter += 1
        tx_id = f"tx_{self.tx_counter}"
        self.devices_state[tx_id] = self._make_base_state(tx_id)
        self.devices_state[tx_id]["conn"] = self._create_device_connection(tx_id)

        # 重新渲染左侧列表
        self._render_all_tx_panels()
        self._refresh_all_combos()
        self._system_log(f"添加新发送端: {tx_id.upper()}")
        return tx_id

    def _remove_tx(self, tx_id: str) -> None:
        if tx_id in self.devices_state:
            self._dev_disconnect(tx_id)
            del self.devices_state[tx_id]
            self._render_all_tx_panels()
            self._system_log(f"移除了发送端: {tx_id.upper()}")

    def _create_device_connection(self, dev_id: str) -> DeviceConnection:
        def on_line(text: str) -> None:
            self.root.after(0, self._log_device_output, dev_id, text)
            stat_line = self._extract_stat_line(text)
            if stat_line is not None:
                self.root.after(0, self._update_stats, dev_id, stat_line)
            if "GUI_STAT:" in text:
                self.root.after(0, self._display_slot_stats, dev_id, text)

        def on_error(msg: str) -> None:
            self.root.after(0, self._system_log, f"[{dev_id.upper()}] 错误: {msg}")

        def on_disconnect() -> None:
            self.root.after(0, self._on_device_disconnected, dev_id)

        return DeviceConnection(dev_id, on_line, on_error, on_disconnect)

    def _on_device_disconnected(self, dev_id: str) -> None:
        self._update_ui_dot_indicator(dev_id, False)
        self._system_log(f"[{dev_id.upper()}] 物理连接断开")

    # ============================================================
    # UI面板构造 (RX & JAM)
    # ============================================================
    def _build_rx_panel(self) -> None:
        parent = self.rx_outer_frame
        dev = self.devices_state[DEV_RX]

        # 1. 连接区
        self._build_compact_connection_ui(parent, DEV_RX)

        # 2. 控制指令
        ctrl_frame = ttk.LabelFrame(parent, text=" 接收端操作 ", style="Card.TLabelframe")
        ctrl_frame.pack(fill=tk.X, padx=5, pady=4)
        c_inner = ttk.Frame(ctrl_frame, style="CardContent.TFrame")
        c_inner.pack(fill=tk.X, padx=4, pady=4)

        ttk.Button(c_inner, text=" 查询状态 ", command=lambda: self._dev_query_status(DEV_RX)).pack(side=tk.LEFT, padx=3, expand=True, fill=tk.X)
        ttk.Button(c_inner, text=" 清空统计 ", command=lambda: self._dev_reset_stats(DEV_RX)).pack(side=tk.LEFT, padx=3, expand=True, fill=tk.X)
        ttk.Checkbutton(c_inner, text="自动轮询", variable=dev["auto_poll_var"]).pack(side=tk.LEFT, padx=5)

        # 3. 统计网格
        stat_frame = ttk.LabelFrame(parent, text=" 实时接收指标 ", style="Card.TLabelframe")
        stat_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=4)
        s_inner = ttk.Frame(stat_frame, style="CardContent.TFrame")
        s_inner.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        rx_spec = [
            ("设备角色", "role"), ("接收数据包", "rx_pkt"),
            ("完好帧数", "frame_ok"), ("CRC 校验失败", "crc_fail"),
            ("魔数/版本异常", "magic_fail"), ("帧长度异常", "len_fail"),
            ("序列号丢失", "gap"), ("重复数据包", "dup"),
            ("乱序数据包", "ooo"), ("末序列号", "last_seq"),
        ]
        for i, (label, key) in enumerate(rx_spec):
            ttk.Label(s_inner, text=label, style="Field.TLabel").grid(row=i, column=0, sticky="w", pady=3, padx=4)
            ttk.Label(s_inner, textvariable=dev["stat_vars"][key], width=12, style="Value.TLabel", anchor="center").grid(row=i, column=1, sticky="ew", pady=3, padx=4)
        s_inner.columnconfigure(1, weight=1)

    def _build_jam_panel(self) -> None:
        parent = self.jam_outer_frame
        dev = self.devices_state[DEV_JAM]

        # 1. 连接区
        self._build_compact_connection_ui(parent, DEV_JAM)

        # 2. 强干扰启停控制
        jam_ctrl = ttk.LabelFrame(parent, text=" 干扰模式设置 ", style="Card.TLabelframe")
        jam_ctrl.pack(fill=tk.X, padx=5, pady=4)
        jc_inner = ttk.Frame(jam_ctrl, style="CardContent.TFrame")
        jc_inner.pack(fill=tk.X, padx=4, pady=4)

        ttk.Button(jc_inner, text=" ⚡ 开启干扰 (JAM ON) ", style="Danger.TButton", command=lambda: self._dev_send_line(DEV_JAM, "JAM ON")).pack(fill=tk.X, pady=2, padx=4)
        ttk.Button(jc_inner, text=" 🛑 关闭干扰 (JAM OFF) ", style="Secondary.TButton", command=lambda: self._dev_send_line(DEV_JAM, "JAM OFF")).pack(fill=tk.X, pady=2, padx=4)

        # 3. 说明：jammer 绕过 MAC 层直接连续发送
        note_frame = ttk.LabelFrame(parent, text=" Jammer 说明 ", style="Card.TLabelframe")
        note_frame.pack(fill=tk.X, padx=5, pady=4)
        n_inner = ttk.Frame(note_frame, style="CardContent.TFrame")
        n_inner.pack(fill=tk.X, padx=4, pady=4)
        ttk.Label(n_inner, text="Jammer 绕过 MAC 层与时隙调度，\n直接连续发送满载数据包以产生\n最大信道干扰。干扰帧不等待 ACK。",
                  style="Field.TLabel", justify=tk.LEFT).pack(anchor="w", padx=4, pady=4)

        # 4. 指标展示
        stat_frame = ttk.LabelFrame(parent, text=" 干扰发送统计 ", style="Card.TLabelframe")
        stat_frame.pack(fill=tk.BOTH, expand=True, padx=5, pady=4)
        s_inner = ttk.Frame(stat_frame, style="CardContent.TFrame")
        s_inner.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

        jam_spec = [
            ("发送角色", "role"), ("已发干扰帧", "sent"),
            ("应答成功", "ack_ok"), ("应答失败", "ack_fail"),
        ]
        for i, (label, key) in enumerate(jam_spec):
            ttk.Label(s_inner, text=label, style="Field.TLabel").grid(row=i, column=0, sticky="w", pady=2, padx=4)
            ttk.Label(s_inner, textvariable=dev["stat_vars"][key], width=12, style="Value.TLabel", anchor="center").grid(row=i, column=1, sticky="ew", pady=2, padx=4)
        s_inner.columnconfigure(1, weight=1)

    # ============================================================
    # 动态渲染 TX 发送端组件
    # ============================================================
    def _render_all_tx_panels(self) -> None:
        # 清除现有左侧控制块
        for widget in self.tx_scroll_container.scrollable_frame.winfo_children():
            widget.destroy()

        # 过滤获取所有当前存在的 tx 设备
        tx_keys = [k for k in self.devices_state.keys() if k.startswith("tx_")]

        if not tx_keys:
            no_tx_lbl = ttk.Label(self.tx_scroll_container.scrollable_frame, text="无活跃发送端，请点击右上角按钮添加。", font=("Helvetica", 10), background=_C_BG, foreground=_C_TEXT_SEC)
            no_tx_lbl.pack(padx=20, pady=40)
            return

        for tx_id in tx_keys:
            self._build_single_tx_panel(self.tx_scroll_container.scrollable_frame, tx_id)

    def _build_single_tx_panel(self, parent: ttk.Frame, tx_id: str) -> None:
        dev = self.devices_state[tx_id]

        tx_card = ttk.LabelFrame(parent, text=f" 发送端 - {tx_id.upper()} ", style="Card.TLabelframe")
        tx_card.pack(fill=tk.X, padx=6, pady=6, expand=True)

        # 内部主容器
        inner = ttk.Frame(tx_card, style="CardContent.TFrame")
        inner.pack(fill=tk.X, padx=4, pady=4)

        # 第一行：物理连接控制
        conn_row = ttk.Frame(inner, style="CardContent.TFrame")
        conn_row.pack(fill=tk.X, pady=2)

        # 指示灯与状态
        indicator = tk.Canvas(conn_row, width=10, height=10, bg=_C_CARD_BG, highlightthickness=0)
        indicator.pack(side=tk.LEFT, padx=(4, 4))
        dev["widgets"]["dot"] = indicator
        self._draw_state_dot(tx_id, False)

        ttk.Label(conn_row, text="串口", style="Field.TLabel").pack(side=tk.LEFT, padx=2)
        port_combo = ttk.Combobox(conn_row, textvariable=dev["port_var"], width=10, state="readonly")
        port_combo.pack(side=tk.LEFT, padx=2)
        dev["widgets"]["port_combo"] = port_combo

        ttk.Label(conn_row, text="波特率", style="Field.TLabel").pack(side=tk.LEFT, padx=2)
        ttk.Entry(conn_row, textvariable=dev["baud_var"], width=7).pack(side=tk.LEFT, padx=2)

        ttk.Label(conn_row, text="连接", style="Field.TLabel").pack(side=tk.LEFT, padx=2)
        ttk.OptionMenu(conn_row, dev["conn_mode_var"], "SERIAL", "SERIAL", "TCP").pack(side=tk.LEFT, padx=2)

        ttk.Label(conn_row, text="IP", style="Field.TLabel").pack(side=tk.LEFT, padx=2)
        ttk.Entry(conn_row, textvariable=dev["host_var"], width=11).pack(side=tk.LEFT, padx=2)
        ttk.Entry(conn_row, textvariable=dev["tcp_port_var"], width=5).pack(side=tk.LEFT, padx=2)

        ttk.Button(conn_row, text="连接", command=lambda: self._dev_connect(tx_id), width=5).pack(side=tk.LEFT, padx=4)
        ttk.Button(conn_row, text="断开", command=lambda: self._dev_disconnect(tx_id), style="Secondary.TButton", width=5).pack(side=tk.LEFT, padx=2)
        ttk.Button(conn_row, text="移除设备", command=lambda: self._remove_tx(tx_id), style="Danger.TButton", width=8).pack(side=tk.RIGHT, padx=4)

        # 第二行：系统与信道参数
        param_row = ttk.Frame(inner, style="CardContent.TFrame")
        param_row.pack(fill=tk.X, pady=2)

        ttk.Label(param_row, text="时隙(ms)", style="Field.TLabel").grid(row=0, column=0, padx=2, sticky="w")
        ttk.Entry(param_row, textvariable=dev["slot_ms_var"], width=5).grid(row=0, column=1, padx=2)

        ttk.Label(param_row, text="CSMA窗口", style="Field.TLabel").grid(row=0, column=2, padx=4, sticky="w")
        ttk.Entry(param_row, textvariable=dev["csma_win_var"], width=5).grid(row=0, column=3, padx=2)

        ttk.Label(param_row, text="时隙限制", style="Field.TLabel").grid(row=0, column=4, padx=4, sticky="w")
        ttk.Entry(param_row, textvariable=dev["slot_limit_var"], width=5).grid(row=0, column=5, padx=2)

        ttk.Label(param_row, text="MAC协议", style="Field.TLabel").grid(row=0, column=6, padx=4, sticky="w")
        ttk.OptionMenu(param_row, dev["mac_mode_var"], "ALOHA", "ALOHA", "CSMA").grid(row=0, column=7, padx=2)

        ttk.Label(param_row, text="冲突概率q", style="Field.TLabel").grid(row=0, column=8, padx=4, sticky="w")
        ttk.Entry(param_row, textvariable=dev["mac_q_var"], width=4).grid(row=0, column=9, padx=2)

        # 第三行：发送数据载荷配置
        data_row = ttk.Frame(inner, style="CardContent.TFrame")
        data_row.pack(fill=tk.X, pady=2)

        ttk.Label(data_row, text="突发包数", style="Field.TLabel").pack(side=tk.LEFT, padx=2)
        ttk.Entry(data_row, textvariable=dev["count_var"], width=6).pack(side=tk.LEFT, padx=2)

        ttk.Label(data_row, text="帧间隔(ms)", style="Field.TLabel").pack(side=tk.LEFT, padx=4)
        ttk.Entry(data_row, textvariable=dev["interval_var"], width=6).pack(side=tk.LEFT, padx=2)

        ttk.Label(data_row, text="载荷格式", style="Field.TLabel").pack(side=tk.LEFT, padx=4)
        ttk.OptionMenu(data_row, dev["mode_var"], "ASCII", "ASCII", "HEX").pack(side=tk.LEFT, padx=2)

        ttk.Label(data_row, text="发送载荷", style="Field.TLabel").pack(side=tk.LEFT, padx=4)
        ttk.Entry(data_row, textvariable=dev["payload_var"], width=22).pack(side=tk.LEFT, padx=2, fill=tk.X, expand=True)

        ttk.Checkbutton(data_row, text="启用TX", variable=dev["enable_var"], command=lambda: self._dev_toggle_enable(tx_id)).pack(side=tk.RIGHT, padx=4)

        # 第四行：控制动作与定时发送
        action_row = ttk.Frame(inner, style="CardContent.TFrame")
        action_row.pack(fill=tk.X, pady=4)

        ttk.Button(action_row, text="🚀 发送突发包", command=lambda: self._dev_send_burst(tx_id)).pack(side=tk.LEFT, padx=2)
        ttk.Button(action_row, text="停止", command=lambda: self._dev_send_line(tx_id, "STOP"), style="Secondary.TButton").pack(side=tk.LEFT, padx=2)
        ttk.Button(action_row, text="读取状态", command=lambda: self._dev_query_status(tx_id), style="Secondary.TButton").pack(side=tk.LEFT, padx=2)
        ttk.Button(action_row, text="清除计数", command=lambda: self._dev_reset_stats(tx_id), style="Secondary.TButton").pack(side=tk.LEFT, padx=2)

        # 定时发送子组件
        ttk.Label(action_row, text="定时(HH:MM:SS)", style="Field.TLabel").pack(side=tk.LEFT, padx=(10, 2))
        ttk.Entry(action_row, textvariable=dev["schedule_time_var"], width=8).pack(side=tk.LEFT, padx=2)
        ttk.Button(action_row, text="设定任务", command=lambda: self._schedule_burst(tx_id)).pack(side=tk.LEFT, padx=2)
        ttk.Button(action_row, text="取消", command=lambda: self._cancel_scheduled_burst(tx_id), style="Secondary.TButton").pack(side=tk.LEFT, padx=2)

        # 下方：状态反馈展示区
        status_panel = ttk.Frame(inner, style="CardContent.TFrame")
        status_panel.pack(fill=tk.X, pady=2)

        # 指标展示子面板
        perf_frame = ttk.Frame(status_panel, style="CardContent.TFrame")
        perf_frame.pack(side=tk.LEFT, fill=tk.Y, padx=2)

        metrics = [
            ("发送计数", "sent"), ("应答成功", "ack_ok"), ("重传总数", "retries_sum"),
            ("应答失败", "ack_fail"), ("单包最大重传", "retries_max")
        ]
        for i, (m_label, m_key) in enumerate(metrics):
            r = i // 3
            c = (i % 3) * 2
            ttk.Label(perf_frame, text=m_label, style="Field.TLabel").grid(row=r, column=c, sticky="w", padx=(2, 2), pady=1)
            ttk.Label(perf_frame, textvariable=dev["stat_vars"][m_key], width=7, style="Value.TLabel", anchor="center").grid(row=r, column=c+1, padx=(1, 4), pady=1)

        # 时隙分析反馈文本区域
        txt_frame = ttk.Frame(status_panel, style="CardContent.TFrame")
        txt_frame.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=(8, 2))

        ttk.Label(txt_frame, textvariable=dev["xmit_summary"], font=("Helvetica", 8, "bold"), foreground=_C_ACCENT, background=_C_CARD_BG).pack(anchor="w")

        st_text = scrolledtext.ScrolledText(
            txt_frame, height=3, state="disabled", font=("Courier New", 8),
            bg="#fdfefe", fg=_C_TEXT_PRI, insertbackground=_C_TEXT_PRI,
            relief=tk.FLAT, borderwidth=1, highlightbackground=_C_BORDER)
        st_text.pack(fill=tk.BOTH, expand=True, pady=1)
        dev["widgets"]["stats_text"] = st_text

    # ============================================================
    # 通用物理连接组装函数
    # ============================================================
    def _build_compact_connection_ui(self, parent_frame: ttk.Frame, dev_id: str) -> None:
        dev = self.devices_state[dev_id]

        conn_frame = ttk.LabelFrame(parent_frame, text=" 物理层链路配置 ", style="Card.TLabelframe")
        conn_frame.pack(fill=tk.X, padx=5, pady=4)
        c_inner = ttk.Frame(conn_frame, style="CardContent.TFrame")
        c_inner.pack(fill=tk.X, padx=4, pady=4)

        # 行0: 串口/波特率/连接模式
        ttk.Label(c_inner, text="端口", style="Field.TLabel").grid(row=0, column=0, sticky="w", pady=2)
        port_combo = ttk.Combobox(c_inner, textvariable=dev["port_var"], width=10, state="readonly")
        port_combo.grid(row=0, column=1, padx=2)
        dev["widgets"]["port_combo"] = port_combo

        ttk.Label(c_inner, text="速率", style="Field.TLabel").grid(row=0, column=2, sticky="w", pady=2)
        ttk.Entry(c_inner, textvariable=dev["baud_var"], width=7).grid(row=0, column=3, padx=2)

        # 行1: 网络IP、端口、模式
        ttk.Label(c_inner, text="方式", style="Field.TLabel").grid(row=1, column=0, sticky="w", pady=2)
        ttk.OptionMenu(c_inner, dev["conn_mode_var"], "SERIAL", "SERIAL", "TCP").grid(row=1, column=1, sticky="ew")

        ttk.Label(c_inner, text="Token", style="Field.TLabel").grid(row=1, column=2, sticky="w", pady=2)
        ttk.Entry(c_inner, textvariable=dev["token_var"], width=7).grid(row=1, column=3, padx=2)

        ttk.Label(c_inner, text="IP", style="Field.TLabel").grid(row=2, column=0, sticky="w", pady=2)
        ttk.Entry(c_inner, textvariable=dev["host_var"], width=12).grid(row=2, column=1, padx=2)

        ttk.Label(c_inner, text="Port", style="Field.TLabel").grid(row=2, column=2, sticky="w", pady=2)
        ttk.Entry(c_inner, textvariable=dev["tcp_port_var"], width=7).grid(row=2, column=3, padx=2)

        # 控制物理通断
        btn_box = ttk.Frame(c_inner, style="CardContent.TFrame")
        btn_box.grid(row=3, column=0, columnspan=4, sticky="ew", pady=4)

        # 连接指示灯
        indicator = tk.Canvas(btn_box, width=10, height=10, bg=_C_CARD_BG, highlightthickness=0)
        indicator.pack(side=tk.LEFT, padx=4)
        dev["widgets"]["dot"] = indicator
        self._draw_state_dot(dev_id, False)

        ttk.Button(btn_box, text=" 建立连接 ", command=lambda: self._dev_connect(dev_id)).pack(side=tk.LEFT, expand=True, fill=tk.X, padx=2)
        ttk.Button(btn_box, text=" 断开 ", command=lambda: self._dev_disconnect(dev_id), style="Secondary.TButton").pack(side=tk.LEFT, expand=True, fill=tk.X, padx=2)

    # ============================================================
    # 连接指示灯图形化更新
    # ============================================================
    def _draw_state_dot(self, dev_id: str, connected: bool) -> None:
        dev = self.devices_state.get(dev_id)
        if not dev or "dot" not in dev["widgets"]:
            return
        dot = dev["widgets"]["dot"]
        dot.delete("all")
        color = _C_SUCCESS if connected else _C_DANGER
        dot.create_oval(1, 1, 9, 9, fill=color, outline="")

    def _update_ui_dot_indicator(self, dev_id: str, connected: bool) -> None:
        self._draw_state_dot(dev_id, connected)

    # ============================================================
    # 日志输出机制
    # ============================================================
    def _system_log(self, message: str) -> None:
        self.log.insert(tk.END, f"[{datetime.now().strftime('%H:%M:%S')}] [SYSTEM] {message}\n", "system")
        self.log.see(tk.END)

    def _log_device_output(self, dev_id: str, text: str) -> None:
        tag = "tx" if dev_id.startswith("tx_") else dev_id
        self.log.insert(tk.END, f"[{datetime.now().strftime('%H:%M:%S')}] [{dev_id.upper()}] {text}\n", tag)
        self.log.see(tk.END)

    # ============================================================
    # 物理串口检测刷新
    # ============================================================
    def _refresh_all_combos(self) -> None:
        ports = [p.device for p in list_ports.comports()]
        if not ports:
            ports = [""]

        for dev_id, dev in self.devices_state.items():
            combo = dev["widgets"].get("port_combo")
            if combo:
                combo["values"] = ports
                if not dev["port_var"].get() and ports[0]:
                    dev["port_var"].set(ports[0])
        self._system_log("可用串口刷新完毕。")

    # ============================================================
    # 设备物理连接流程
    # ============================================================
    def _dev_connect(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        conn = dev["conn"]
        if conn.is_connected():
            self._system_log(f"[{dev_id.upper()}] 连接已处于活跃状态。")
            return

        mode = dev["conn_mode_var"].get().strip().upper()
        if mode == "TCP":
            host = dev["host_var"].get().strip()
            if not host:
                messagebox.showerror("连接错误", f"[{dev_id.upper()}] 请输入合法的 IP 主机地址")
                return
            try:
                port = int(dev["tcp_port_var"].get().strip())
            except ValueError:
                messagebox.showerror("连接错误", f"[{dev_id.upper()}] TCP 端口非有效整数")
                return
            try:
                conn.connect_tcp(host, port)
            except Exception as exc:
                messagebox.showerror("网络建立失败", f"无法建立到 {host}:{port} 的连接\n{exc}")
                return
            token = dev["token_var"].get().strip()
            self._system_log(f"[{dev_id.upper()}] 网络层连接建立成功 {host}:{port}")
            self._update_ui_dot_indicator(dev_id, True)
            conn.send_line(f"AUTH {token}")
            self.root.after(300, lambda: self._dev_query_status(dev_id))
        else:
            port = dev["port_var"].get().strip()
            if not port:
                messagebox.showerror("串口错误", f"[{dev_id.upper()}] 请在下拉框选择串口号")
                return
            try:
                baud = int(dev["baud_var"].get().strip())
            except ValueError:
                messagebox.showerror("格式错误", f"[{dev_id.upper()}] 串口波特率无效")
                return
            try:
                conn.connect_serial(port, baud)
            except Exception as exc:
                messagebox.showerror("串口打开失败", f"无法打开端口 {port}\n{exc}")
                return
            self._system_log(f"[{dev_id.upper()}] 串口连接已开通: {port} @ {baud}")
            self._update_ui_dot_indicator(dev_id, True)
            self.root.after(200, lambda: self._dev_query_status(dev_id))

    def _dev_disconnect(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        self._cancel_scheduled_burst(dev_id)
        dev["conn"].disconnect()
        self._update_ui_dot_indicator(dev_id, False)
        self._system_log(f"[{dev_id.upper()}] 连接被切断")

    # ============================================================
    # 指令发送及基础下发接口
    # ============================================================
    def _dev_send_line(self, dev_id: str, line: str) -> None:
        dev = self.devices_state[dev_id]
        if not dev["conn"].is_connected():
            self._system_log(f"[{dev_id.upper()}] 命令发送终止: 设备处于未连接状态")
            return
        dev["conn"].send_line(line)

    def _dev_query_status(self, dev_id: str) -> None:
        self._dev_send_line(dev_id, "STATUS")

    def _dev_reset_stats(self, dev_id: str) -> None:
        self._dev_send_line(dev_id, "RESETSTATS")

    def _dev_toggle_enable(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        cmd = "ENABLE 1" if dev["enable_var"].get() else "ENABLE 0"
        self._dev_send_line(dev_id, cmd)

    def _apply_mac_and_slot_config(self, dev_id: str) -> bool:
        dev = self.devices_state[dev_id]
        mac_mode = dev["mac_mode_var"].get().strip().upper()
        try:
            q = int(dev["mac_q_var"].get().strip())
            slot_ms = int(dev["slot_ms_var"].get().strip())
            win = int(dev["csma_win_var"].get().strip())
        except ValueError:
            messagebox.showerror("参数越界", "时隙、退避窗口、MAC 协议权重必须配置为有效整数。")
            return False

        if q < 0 or q > 100:
            messagebox.showerror("参数越界", "q 权重参数范围应当限制在 0 至 100 以内。")
            return False

        # 命令串行下发
        self._dev_send_line(dev_id, f"MAC {mac_mode} {q}")
        self._dev_send_line(dev_id, f"SLOT {slot_ms} {win}")

        limit_val = dev["slot_limit_var"].get().strip()
        if limit_val:
            try:
                self._dev_send_line(dev_id, f"SLOTLIMIT {int(limit_val)}")
            except ValueError:
                pass
        return True

    def _dev_send_burst(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        if not dev["conn"].is_connected():
            messagebox.showerror("链路断开", "在发送突发数据流前，请确认设备处于连接状态。")
            return

        if not self._apply_mac_and_slot_config(dev_id):
            return

        try:
            count = int(dev["count_var"].get().strip())
            interval_ms = int(dev["interval_var"].get().strip())
        except ValueError:
            messagebox.showerror("格式错误", "发送总数和帧间隔必须填入有效整型数据。")
            return

        if interval_ms < 0:
            interval_ms = 0

        payload = dev["payload_var"].get().strip()
        if not payload:
            messagebox.showerror("空载荷", "突发载荷内容不能为空字符串")
            return

        dev["slot_statistics"] = []
        dev["is_collecting"] = True
        dev["xmit_summary"].set(f"正在连续发送 {count} 个包，等待遥测...")

        txt_widget = dev["widgets"].get("stats_text")
        if txt_widget:
            txt_widget.config(state="normal")
            txt_widget.delete(1.0, tk.END)
            txt_widget.insert(tk.END, "开始通信任务...\n")
            txt_widget.config(state="disabled")

        mode = dev["mode_var"].get().strip().upper()
        cmd_prefix = "BURSTHEX" if mode == "HEX" else "BURST"
        self._dev_send_line(dev_id, f"{cmd_prefix} {count} {interval_ms} {payload}")

    # ============================================================
    # 定时爆发处理逻辑 (仅限 TX 设备)
    # ============================================================
    def _schedule_burst(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        time_str = dev["schedule_time_var"].get().strip()
        target_time = self._parse_target_time(time_str)
        if target_time is None:
            messagebox.showerror("时间格式不正确", "时间解析失败，请检查并按照格式 'HH:MM:SS' 输入。")
            return

        if not dev["conn"].is_connected():
            messagebox.showerror("未连接", "定时任务创建失败，物理层目前未与该设备握手。")
            return

        self._cancel_scheduled_burst(dev_id)

        delta_sec = (target_time - datetime.now()).total_seconds()
        if delta_sec < 0:
            delta_sec = 0

        delay_ms = int(delta_sec * 1000)
        dev["scheduled_send_job"] = self.root.after(
            delay_ms, lambda: self._execute_scheduled_burst(dev_id)
        )
        self._system_log(f"[{dev_id.upper()}] 成功设定定时任务: {target_time.strftime('%H:%M:%S')} (将在 {delta_sec:.1f} 秒后触发)")

    def _execute_scheduled_burst(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        dev["scheduled_send_job"] = None
        self._system_log(f"[{dev_id.upper()}] 触发计划定时突发任务")
        self._dev_send_burst(dev_id)

    def _cancel_scheduled_burst(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        if dev["scheduled_send_job"] is not None:
            try:
                self.root.after_cancel(dev["scheduled_send_job"])
            except Exception:
                pass
            dev["scheduled_send_job"] = None
            self._system_log(f"[{dev_id.upper()}] 计划定时已撤销")

    def _parse_target_time(self, text: str) -> datetime | None:
        now = datetime.now()
        for fmt in ("%H:%M:%S", "%H:%M"):
            try:
                time_obj = datetime.strptime(text, fmt).time()
                combined = datetime.combine(now.date(), time_obj)
                if combined <= now:
                    combined += timedelta(days=1)
                return combined
            except ValueError:
                continue
        return None

    # ============================================================
    # 定时轮询与状态自动解析更新
    # ============================================================
    def _start_global_poll(self) -> None:
        def poll():
            for dev_id, dev in list(self.devices_state.items()):
                if dev["conn"].is_connected() and dev["auto_poll_var"].get():
                    self._dev_query_status(dev_id)
            self.root.after(self.poll_interval_ms, poll)
        poll()

    def _extract_stat_line(self, text: str) -> str | None:
        pos = text.find("STAT ")
        if pos < 0:
            return None
        return text[pos:]

    def _update_stats(self, dev_id: str, text: str) -> None:
        parts = text.split()
        kv = {}
        for item in parts[1:]:
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            kv[k.strip()] = v.strip()

        sv = self.devices_state[dev_id]["stat_vars"]
        if "role" in kv:
            sv["role"].set(kv["role"])

        mapping = {
            "sent": "sent", "ack_ok": "ack_ok", "tx_ok": "ack_ok",
            "ack_fail": "ack_fail", "tx_fail": "ack_fail",
            "retries_sum": "retries_sum", "retries_max": "retries_max",
            "rx_pkt": "rx_pkt", "frame_ok": "frame_ok", "crc_fail": "crc_fail",
            "magic_fail": "magic_fail", "len_fail": "len_fail", "last_seq": "last_seq",
            "gap": "gap", "seq_gap": "gap",
            "dup": "dup", "seq_dup": "dup",
            "ooo": "ooo", "seq_out_of_order": "ooo",
            "mac": "mac", "q": "q",
            "slot_ms": "slot_ms", "csma_win": "csma_win", "slot_limit": "slot_limit",
        }
        for src, dest in mapping.items():
            if src in kv and dest in sv:
                sv[dest].set(kv[src])

    def _display_slot_stats(self, dev_id: str, log_line: str) -> None:
        dev = self.devices_state[dev_id]
        if not dev["is_collecting"]:
            return

        slot_match = re.match(
            r"GUI_STAT:\s+slot=(\d+)\s+attempted=(\d+)\s+success=(\d+)\s+reason=(\d+)",
            log_line,
        )
        if slot_match:
            record = {
                "slot": int(slot_match.group(1)),
                "attempted": int(slot_match.group(2)),
                "success": int(slot_match.group(3)),
                "reason": int(slot_match.group(4)),
            }
            dev["slot_statistics"].append(record)
            self._render_statistics_terminal(dev_id)
            return

        summary_match = re.match(
            r"GUI_STAT:\s+Sent\s+(\d+)/(\d+)\s+packets\sin\s+(\d+)\s+slots",
            log_line,
        )
        if summary_match:
            sent = summary_match.group(1)
            target = summary_match.group(2)
            slot_count = summary_match.group(3)
            timeout = "timeout" in log_line
            status = "超时异常终止" if timeout else "发送任务正常完成"
            dev["xmit_summary"].set(f"{status}: {sent}/{target}包 耗时时隙:{slot_count}")
            dev["is_collecting"] = False
            self._render_statistics_terminal(dev_id)

    def _render_statistics_terminal(self, dev_id: str) -> None:
        dev = self.devices_state[dev_id]
        st = dev["widgets"].get("stats_text")
        if st is None or not dev["slot_statistics"]:
            return

        lines = [f"{'时隙':>5} | {'尝试':>4} | {'成功':>4} | {'事件原因描述':<8}", "-" * 34]
        reason_map = {0: "未开始/忙", 1: "MAX_RT超时", 2: "信道拥堵", 3: "其他底层故障"}

        for r in dev["slot_statistics"]:
            att = "是" if r["attempted"] else "否"
            suc = "是" if r["success"] else "否"
            reason_str = reason_map.get(r["reason"], str(r["reason"]))
            lines.append(f"{r['slot']:>5} | {att:>4} | {suc:>4} | {reason_str:<8}")

        total = len(dev["slot_statistics"])
        attempted_cnt = sum(1 for r in dev["slot_statistics"] if r["attempted"])
        success_cnt = sum(1 for r in dev["slot_statistics"] if r["success"])
        lines.append("-" * 34)
        lines.append(f"总计时隙: {total} | 尝试: {attempted_cnt} | 成功率: {(success_cnt/attempted_cnt*100) if attempted_cnt else 0:.1f}%")

        st.config(state="normal")
        st.delete(1.0, tk.END)
        st.insert(tk.END, "\n".join(lines))
        st.see(tk.END)
        st.config(state="disabled")

    # ============================================================
    # 持久化配置文件自动读取加载
    # ============================================================
    def _load_persisted_config_or_defaults(self) -> None:
        try:
            if self.config_path.exists():
                data = json.loads(self.config_path.read_text(encoding="utf-8"))
                if not isinstance(data, dict):
                    self._add_new_tx()
                    return

                # 读取固定的 RX & JAM
                for dev_id in (DEV_RX, DEV_JAM):
                    for k in ("conn_mode", "port", "baud", "host", "tcp_port", "token", "mac_mode", "mac_q", "slot_ms", "csma_win", "slot_limit"):
                        cfg_key = f"{dev_id}_{k}"
                        if cfg_key in data:
                            self.devices_state[dev_id][f"{k}_var"].set(str(data[cfg_key]))

                # 获取保存的动态发端 TX 设备列表
                tx_list = data.get("tx_list", [])
                if not tx_list:
                    self._add_new_tx()
                else:
                    for item in tx_list:
                        tx_id = self._add_new_tx()
                        for k in ("conn_mode", "port", "baud", "host", "tcp_port", "token", "count", "interval", "mode", "payload", "mac_mode", "mac_q", "slot_ms", "csma_win", "slot_limit", "schedule_time"):
                            cfg_key = f"{item}_{k}"
                            if cfg_key in data:
                                self.devices_state[tx_id][f"{k}_var"].set(str(data[cfg_key]))
            else:
                self._add_new_tx()
        except Exception as exc:
            self._system_log(f"加载配置文件异常，已降级初始化默认设置。{exc}")

    def _save_persisted_config(self) -> None:
        data = {}
        # 1. 保存固定设备 RX & JAM
        for dev_id in (DEV_RX, DEV_JAM):
            dev = self.devices_state[dev_id]
            for k in ("conn_mode", "port", "baud", "host", "tcp_port", "token", "mac_mode", "mac_q", "slot_ms", "csma_win", "slot_limit"):
                data[f"{dev_id}_{k}"] = dev[f"{k}_var"].get()

        # 2. 保存 TX 设备信息与列表
        tx_keys = [k for k in self.devices_state.keys() if k.startswith("tx_")]
        data["tx_list"] = tx_keys
        for tx_id in tx_keys:
            dev = self.devices_state[tx_id]
            for k in ("conn_mode", "port", "baud", "host", "tcp_port", "token", "count", "interval", "mode", "payload", "mac_mode", "mac_q", "slot_ms", "csma_win", "slot_limit", "schedule_time"):
                data[f"{tx_id}_{k}"] = dev[f"{k}_var"].get()

        try:
            self.config_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
            self._system_log("设备配置已成功保存至本地物理文件。")
        except Exception as exc:
            messagebox.showerror("写入异常", f"无法写入配置文件 {exc}")


def main() -> None:
    parser = argparse.ArgumentParser(description="NRF24 Multi-Device Comprehensive Controller Panel")
    parser.parse_args()

    root = tk.Tk()
    app = Nrf24ControllerApp(root)

    def on_close() -> None:
        # 断开所有可能正活跃的连接、关闭线程、取消任务
        for dev_id, dev in list(app.devices_state.items()):
            app._dev_disconnect(dev_id)
        app._save_persisted_config()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()