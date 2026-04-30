import threading
import socket
import json
import argparse
from pathlib import Path
import tkinter as tk
from tkinter import messagebox, scrolledtext
from datetime import datetime, timedelta

import serial
from serial.tools import list_ports


class Nrf24ControllerApp:
    def __init__(self, root: tk.Tk, initial_config: dict | None = None) -> None:
        self.root = root
        self.root.title("NRF24 上位机控制台")
        self.root.geometry("980x820")
        self.root.minsize(920, 720)

        self.config_path = Path.home() / ".nrf24_controller_gui.json"

        self.ser = None
        self.sock = None
        self.conn_type = None
        self.reader_stop = threading.Event()
        self.reader_thread = None

        self.conn_mode_var = tk.StringVar(value="SERIAL")
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.host_var = tk.StringVar(value="192.168.4.1")
        self.tcp_port_var = tk.StringVar(value="3333")
        self.token_var = tk.StringVar(value="nrf24")
        self.enable_var = tk.BooleanVar(value=True)
        self.mac_mode_var = tk.StringVar(value="ALOHA")
        self.mac_q_var = tk.StringVar(value="100")
        self.slot_ms_var = tk.StringVar(value="20")
        self.csma_win_var = tk.StringVar(value="1")
        self.slot_limit_var = tk.StringVar(value="0")

        self.count_var = tk.StringVar(value="10")
        self.interval_var = tk.StringVar(value="0")
        self.mode_var = tk.StringVar(value="ASCII")
        self.payload_var = tk.StringVar(value="HELLO_NRF24")
        self.auto_poll_var = tk.BooleanVar(value=True)
        self.poll_interval_ms = 1000
        self.schedule_time_var = tk.StringVar(value="11:05:00")
        self.scheduled_send_job = None

        self._load_persisted_config()
        if initial_config:
            self._apply_initial_config(initial_config)

        self.stat_vars = {
            "role": tk.StringVar(value="-"),
            "ack_ok": tk.StringVar(value="0"),
            "ack_fail": tk.StringVar(value="0"),
            "sent": tk.StringVar(value="0"),
            "retries_sum": tk.StringVar(value="0"),
            "retries_max": tk.StringVar(value="0"),
            "rx_pkt": tk.StringVar(value="0"),
            "frame_ok": tk.StringVar(value="0"),
            "crc_fail": tk.StringVar(value="0"),
            "gap": tk.StringVar(value="0"),
            "dup": tk.StringVar(value="0"),
            "ooo": tk.StringVar(value="0"),
            "mac": tk.StringVar(value="-"),
            "q": tk.StringVar(value="-"),
            "slot_ms": tk.StringVar(value="-"),
            "csma_win": tk.StringVar(value="-"),
            "slot_limit": tk.StringVar(value="-"),
        }

        # 新增以下代码：
        # 用于保存时隙统计的列表
        self.slot_statistics = [] 
        # 用于标记是否正在进行X包统计任务
        self.is_collecting = False 
        # （可选）定义一个用于在UI上显示统计摘要的变量
        self.xmit_summary = tk.StringVar(value="等待开始...")

        self._build_ui()
        self._refresh_ports()

    def _apply_initial_config(self, config: dict) -> None:
        for key, var in (
            ("conn_mode", self.conn_mode_var),
            ("port", self.port_var),
            ("baud", self.baud_var),
            ("host", self.host_var),
            ("tcp_port", self.tcp_port_var),
            ("token", self.token_var),
            ("count", self.count_var),
            ("interval", self.interval_var),
            ("mode", self.mode_var),
            ("payload", self.payload_var),
            ("schedule_time", self.schedule_time_var),
            ("mac_mode", self.mac_mode_var),
            ("mac_q", self.mac_q_var),
            ("slot_ms", self.slot_ms_var),
            ("csma_win", self.csma_win_var),
            ("slot_limit", self.slot_limit_var),
        ):
            value = config.get(key)
            if value is not None:
                var.set(str(value))

        if "auto_poll" in config:
            self.auto_poll_var.set(bool(config["auto_poll"]))
        if "enable_tx" in config:
            self.enable_var.set(bool(config["enable_tx"]))

    def _load_persisted_config(self) -> None:
        try:
            if self.config_path.exists():
                data = json.loads(self.config_path.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    self._apply_initial_config(data)
        except Exception:
            pass

    def _save_persisted_config(self) -> None:
        data = {
            "conn_mode": self.conn_mode_var.get(),
            "port": self.port_var.get(),
            "baud": self.baud_var.get(),
            "host": self.host_var.get(),
            "tcp_port": self.tcp_port_var.get(),
            "token": self.token_var.get(),
            "count": self.count_var.get(),
            "interval": self.interval_var.get(),
            "mode": self.mode_var.get(),
            "payload": self.payload_var.get(),
            "schedule_time": self.schedule_time_var.get(),
            "mac_mode": self.mac_mode_var.get(),
            "mac_q": self.mac_q_var.get(),
            "slot_ms": self.slot_ms_var.get(),
            "csma_win": self.csma_win_var.get(),
            "slot_limit": self.slot_limit_var.get(),
            "auto_poll": self.auto_poll_var.get(),
            "enable_tx": self.enable_var.get(),
        }
        try:
            self.config_path.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        except Exception:
            pass

    def _build_ui(self) -> None:
        top = tk.Frame(self.root)
        top.pack(fill=tk.X, padx=10, pady=8)

        tk.Label(top, text="串口").grid(row=0, column=0, sticky="w")
        self.port_menu = tk.OptionMenu(top, self.port_var, "")
        self.port_menu.config(width=15)
        self.port_menu.grid(row=0, column=1, padx=6)

        tk.Button(top, text="刷新", command=self._refresh_ports, width=10).grid(row=0, column=2, padx=6)

        tk.Label(top, text="波特率").grid(row=0, column=3, sticky="w")
        tk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, padx=6)

        tk.Label(top, text="方式").grid(row=1, column=0, sticky="w")
        tk.OptionMenu(top, self.conn_mode_var, "SERIAL", "TCP").grid(row=1, column=1, padx=6, sticky="w")

        tk.Label(top, text="主机").grid(row=1, column=2, sticky="w")
        tk.Entry(top, textvariable=self.host_var, width=12).grid(row=1, column=3, padx=6)

        tk.Label(top, text="端口").grid(row=1, column=4, sticky="w")
        tk.Entry(top, textvariable=self.tcp_port_var, width=10).grid(row=1, column=5, padx=6)

        tk.Label(top, text="Token").grid(row=1, column=6, sticky="w")
        tk.Entry(top, textvariable=self.token_var, width=12).grid(row=1, column=7, padx=6)

        tk.Button(top, text="连接", command=self._connect, width=12).grid(row=0, column=5, padx=6)
        tk.Button(top, text="断开", command=self._disconnect, width=12).grid(row=0, column=6, padx=6)
        tk.Button(top, text="Help", command=self._show_help, width=10).grid(row=0, column=7, padx=6)

        # ===== 新增：X包发送统计面板 =====
        stats_frame = tk.LabelFrame(self.root, text="发送统计详情 (X包任务)")
        stats_frame.pack(fill=tk.X, expand=False, padx=10, pady=6)

        # 摘要标签（放在面板顶部）
        tk.Label(stats_frame, textvariable=self.xmit_summary, fg="blue").pack(anchor="w", padx=4, pady=2)

        # 滚动文本框（用于显示详细的时隙记录）
        self.stats_text = scrolledtext.ScrolledText(stats_frame, height=6, state='disabled')
        self.stats_text.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)
        # ==================================

        
        slot = tk.LabelFrame(self.root, text="时隙与MAC设置")
        slot.pack(fill=tk.X, padx=10, pady=6)

        tk.Label(slot, text="时隙(ms)").grid(row=0, column=0, padx=6, pady=8, sticky="w")
        tk.Entry(slot, textvariable=self.slot_ms_var, width=8).grid(row=0, column=1, padx=6, sticky="w")
        tk.Label(slot, text="CSMA窗口(时隙)").grid(row=0, column=2, padx=6, sticky="w")
        tk.Entry(slot, textvariable=self.csma_win_var, width=8).grid(row=0, column=3, padx=6, sticky="w")
        tk.Button(slot, text="应用时隙设置", command=self._apply_slot_config, width=12).grid(row=0, column=4, padx=6, sticky="w")

        tk.Label(slot, text="任务时隙上限(T)").grid(row=0, column=5, padx=6, sticky="w")
        tk.Entry(slot, textvariable=self.slot_limit_var, width=8).grid(row=0, column=6, padx=6, sticky="w")
        tk.Button(slot, text="应用时隙上限", command=self._apply_slot_limit, width=12).grid(row=0, column=7, padx=6, sticky="w")

        tk.Label(slot, text="MAC模式").grid(row=1, column=0, padx=6, pady=8, sticky="w")
        tk.OptionMenu(slot, self.mac_mode_var, "ALOHA", "CSMA").grid(row=1, column=1, padx=6, sticky="w")
        tk.Label(slot, text="q(0-100)").grid(row=1, column=2, padx=6, sticky="w")
        tk.Entry(slot, textvariable=self.mac_q_var, width=8).grid(row=1, column=3, padx=6, sticky="w")
        tk.Button(slot, text="应用MAC设置", command=self._apply_mac_config, width=12).grid(row=1, column=4, padx=6, sticky="w")

        cmd = tk.LabelFrame(self.root, text="发送设置")
        cmd.pack(fill=tk.X, padx=10, pady=6)

        tk.Label(cmd, text="连续包数").grid(row=0, column=0, padx=6, pady=8, sticky="w")
        tk.Entry(cmd, textvariable=self.count_var, width=8).grid(row=0, column=1, padx=6)

        tk.Label(cmd, text="模式").grid(row=0, column=2, padx=6, sticky="w")
        tk.OptionMenu(cmd, self.mode_var, "ASCII", "HEX").grid(row=0, column=3, padx=6)

        tk.Label(cmd, text="内容").grid(row=1, column=0, padx=6, pady=8, sticky="w")
        tk.Entry(cmd, textvariable=self.payload_var, width=70).grid(row=1, column=1, columnspan=5, padx=6, sticky="we")

        btns = tk.Frame(self.root)
        btns.pack(fill=tk.X, padx=10, pady=6)

        tk.Checkbutton(
            btns,
            text="启用发送",
            variable=self.enable_var,
            command=self._toggle_enable,
        ).pack(side=tk.LEFT, padx=6)

        tk.Button(btns, text="发送突发包", command=self._send_burst, width=14).pack(side=tk.LEFT, padx=6)
        tk.Button(btns, text="停止发送", command=self._stop_burst, width=10).pack(side=tk.LEFT, padx=6)
        tk.Button(btns, text="查询状态", command=self._query_status, width=12).pack(side=tk.LEFT, padx=6)
        tk.Button(btns, text="重置统计", command=self._reset_stats, width=12).pack(side=tk.LEFT, padx=6)
        tk.Checkbutton(btns, text="自动轮询", variable=self.auto_poll_var).pack(side=tk.LEFT, padx=6)

        sched = tk.LabelFrame(self.root, text="定时发送")
        sched.pack(fill=tk.X, padx=10, pady=6)
        tk.Label(sched, text="开始时间").grid(row=0, column=0, padx=6, pady=8, sticky="w")
        tk.Entry(sched, textvariable=self.schedule_time_var, width=12).grid(row=0, column=1, padx=6)
        tk.Label(sched, text="格式: HH:MM 或 HH:MM:SS").grid(row=0, column=2, padx=6, sticky="w")
        tk.Button(sched, text="定时发送突发包", command=self._schedule_burst_at_time, width=16).grid(row=0, column=3, padx=8)
        tk.Button(sched, text="取消定时", command=self._cancel_scheduled_burst, width=10).grid(row=0, column=4, padx=8)

        stats = tk.LabelFrame(self.root, text="ACK / RX 可靠性统计")
        stats.pack(fill=tk.X, padx=10, pady=6)

        tk.Label(stats, text="角色").grid(row=0, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["role"], width=8, anchor="w").grid(row=0, column=1, padx=6)
        tk.Label(stats, text="MAC").grid(row=0, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["mac"], width=8, anchor="w").grid(row=0, column=3, padx=6)
        tk.Label(stats, text="q").grid(row=0, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["q"], width=6, anchor="w").grid(row=0, column=5, padx=6)
        tk.Label(stats, text="已发送").grid(row=0, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["sent"], width=8, anchor="w").grid(row=0, column=7, padx=6)

        tk.Label(stats, text="时隙(ms)").grid(row=1, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["slot_ms"], width=8, anchor="w").grid(row=1, column=1, padx=6)
        tk.Label(stats, text="CSMA窗口").grid(row=1, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["csma_win"], width=8, anchor="w").grid(row=1, column=3, padx=6)

        tk.Label(stats, text="时隙上限").grid(row=1, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["slot_limit"], width=8, anchor="w").grid(row=1, column=5, padx=6)

        tk.Label(stats, text="重传总数").grid(row=2, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["retries_sum"], width=8, anchor="w").grid(row=2, column=1, padx=6)
        tk.Label(stats, text="单包最大重传").grid(row=2, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["retries_max"], width=8, anchor="w").grid(row=2, column=3, padx=6)
        tk.Label(stats, text="ACK_OK").grid(row=2, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ack_ok"], width=8, anchor="w").grid(row=2, column=5, padx=6)
        tk.Label(stats, text="ACK_FAIL").grid(row=2, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ack_fail"], width=8, anchor="w").grid(row=2, column=7, padx=6)

        tk.Label(stats, text="接收包数").grid(row=3, column=0, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["rx_pkt"], width=8, anchor="w").grid(row=3, column=1, padx=6)
        tk.Label(stats, text="有效帧数").grid(row=3, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["frame_ok"], width=8, anchor="w").grid(row=3, column=3, padx=6)
        tk.Label(stats, text="CRC失败").grid(row=3, column=4, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["crc_fail"], width=8, anchor="w").grid(row=3, column=5, padx=6)
        tk.Label(stats, text="序号丢失").grid(row=3, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["gap"], width=8, anchor="w").grid(row=3, column=7, padx=6)

        tk.Label(stats, text="重复包").grid(row=4, column=0, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["dup"], width=8, anchor="w").grid(row=4, column=1, padx=6)
        tk.Label(stats, text="乱序包").grid(row=4, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ooo"], width=8, anchor="w").grid(row=4, column=3, padx=6)

        guide = tk.Label(
            self.root,
            text="提示：先连接串口，再点“发送突发包”。勾选“自动轮询”可自动刷新统计。",
            anchor="w",
            fg="#1f4f8f",
        )
        guide.pack(fill=tk.X, padx=12, pady=2)

        log_frame = tk.LabelFrame(self.root, text="日志终端")
        log_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=8)
        self.log = scrolledtext.ScrolledText(log_frame, height=20)
        self.log.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    def _refresh_ports(self) -> None:
        ports = [p.device for p in list_ports.comports()]
        menu = self.port_menu["menu"]
        menu.delete(0, "end")

        if not ports:
            ports = [""]

        for p in ports:
            menu.add_command(label=p, command=tk._setit(self.port_var, p))

        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _log(self, message: str) -> None:
        self.log.insert(tk.END, message + "\n")
        self.log.see(tk.END)

    def _close_transport(self) -> None:
        if self.conn_type == "SERIAL" and self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        if self.conn_type == "TCP" and self.sock is not None:
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

# 它们最终会以 "指令 + 换行符" 的形式通过串口或 TCP 发送给你的设备。在以下_send_line函数里面给出
    def _send_line(self, line: str) -> None:
        if self.conn_type is None:
            messagebox.showerror("错误", "连接未建立")
            return

        if self.conn_type == "SERIAL" and self.ser is not None:
            self.ser.write((line + "\n").encode("utf-8"))
        elif self.conn_type == "TCP" and self.sock is not None:
            self.sock.sendall((line + "\n").encode("utf-8"))
        else:
            messagebox.showerror("错误", "连接状态无效")
            return
        self._log("=> " + line)

    def _connect(self) -> None:
        if self.conn_type is not None:
            self._log("已连接，无需重复连接")
            return

        mode = self.conn_mode_var.get().strip().upper()
        if mode == "TCP":
            self._connect_tcp()
        else:
            self._connect_serial()

    def _connect_serial(self) -> None:
        port = self.port_var.get().strip()
        if not port:
            messagebox.showerror("错误", "请选择串口号")
            return

        try:
            baud = int(self.baud_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "波特率格式无效")
            return

        try:
            self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
        except Exception as exc:
            messagebox.showerror("连接失败", str(exc))
            return

        self.conn_type = "SERIAL"
        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        self._log(f"串口连接成功: {port} @ {baud}")
        self.root.after(200, self._query_status)
        self.root.after(self.poll_interval_ms, self._poll_status_tick)

    def _connect_tcp(self) -> None:
        host = self.host_var.get().strip()
        if not host:
            messagebox.showerror("错误", "请输入主机地址")
            return

        try:
            port = int(self.tcp_port_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "TCP 端口格式无效")
            return

        try:
            self.sock = socket.create_connection((host, port), timeout=5)
            self.sock.settimeout(0.2)
        except Exception as exc:
            messagebox.showerror("连接失败", str(exc))
            self.sock = None
            return

        self.conn_type = "TCP"
        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

        token = self.token_var.get().strip()
        self._log(f"无线连接成功: {host}:{port}")
        self._send_line(f"AUTH {token}")
        self.root.after(300, self._query_status)
        self.root.after(self.poll_interval_ms, self._poll_status_tick)

    def _disconnect(self) -> None:
        if self.conn_type is None:
            return

        self.reader_stop.set()
        self._close_transport()
        self._save_persisted_config()
        self._log("连接已断开")

    def _reader_loop(self) -> None:
        while not self.reader_stop.is_set():
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
                text = data.decode("utf-8", errors="replace").strip() if isinstance(data, bytes) else str(data).strip()
                if text:
                    self.root.after(0, self._log, "<= " + text)
                    stat_line = self._extract_stat_line(text)
                    if stat_line is not None:
                        self.root.after(0, self._update_stats_from_line, stat_line)
                
                if "GUI_STAT:" in text:
                    # 这是统计结果行
                    self.root.after(0, self._display_slot_stats, text)
                    continue # 避免被当成普通日志刷屏

            except Exception as exc:
                self.root.after(0, self._log, f"Reader error: {exc}")
                break


    # 然后在类中定义 _display_slot_stats 函数： 用于_reader_loop中处理包含统计信息的日志行。它会解析日志行中的统计数据，并更新UI显示。同时，它还会检测任务完成的标志，以更新摘要信息并停止统计收集。
    def _display_slot_stats(self, log_line):
        if not self.is_collecting:
            return
            
        # 解析并显示
        self.stats_text.config(state='normal')
        self.stats_text.insert(tk.END, log_line + "\n")
        self.stats_text.see(tk.END) # 滚动到底部
        self.stats_text.config(state='disabled')
        
        # 如果检测到任务结束，更新摘要
        if "in" in log_line and "slots" in log_line:
            self.xmit_summary.set(f"任务完成！{log_line}")
            self.is_collecting = False
            
    def _socket_read_line(self):
        if self.sock is None:
            return b""

        chunks = []
        while not self.reader_stop.is_set():
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

    def _extract_stat_line(self, text: str):
        idx = text.find("STAT ")
        if idx < 0:
            return None
        return text[idx:]

    def _update_stats_from_line(self, text: str) -> None:
        parts = text.split()
        kv = {}
        for item in parts[1:]:
            if "=" not in item:
                continue
            k, v = item.split("=", 1)
            kv[k.strip()] = v.strip()

        if "role" in kv:
            self.stat_vars["role"].set(kv["role"])

        mapping = {
            "sent": "sent",
            "ack_ok": "ack_ok",
            "tx_ok": "ack_ok",
            "ack_fail": "ack_fail",
            "tx_fail": "ack_fail",
            "retries_sum": "retries_sum",
            "retries_max": "retries_max",
            "rx_pkt": "rx_pkt",
            "frame_ok": "frame_ok",
            "crc_fail": "crc_fail",
            "gap": "gap",
            "seq_gap": "gap",
            "dup": "dup",
            "seq_dup": "dup",
            "ooo": "ooo",
            "seq_out_of_order": "ooo",
            "mac": "mac",
            "q": "q",
            "slot_ms": "slot_ms",
            "csma_win": "csma_win",
            "slot_limit": "slot_limit",
        }
        for src_key, dst_key in mapping.items():
            if src_key in kv and dst_key in self.stat_vars:
                self.stat_vars[dst_key].set(kv[src_key])

    def _toggle_enable(self) -> None:
        cmd = "ENABLE 1" if self.enable_var.get() else "ENABLE 0"
        self._send_line(cmd)

    def _send_burst(self) -> None:
        try:
            if self.conn_type is None:
                messagebox.showerror("错误", "请先建立连接")
                return
            if not self._apply_mac_config():
                return
            if not self._apply_slot_config():
                return
            if not self._apply_slot_limit():
                return
            count = int(self.count_var.get().strip())

            # --- 新增逻辑：开启统计 ---
            # 在发送前，重置本地统计数据
            self.slot_statistics = []
            self.is_collecting = True
            # 更新UI显示
            self.xmit_summary.set(f"正在发送 {count} 个包，请等待...")
            self.stats_text.config(state='normal')
            self.stats_text.delete(1.0, tk.END)
            self.stats_text.insert(tk.END, f"开始发送任务：目标 {count} 包\n")
            self.stats_text.config(state='disabled')
            # --- 统计逻辑结束 ---

        except ValueError:
            messagebox.showerror("错误", "发送次数必须是整数")
            return

        if count <= 0:
            messagebox.showerror("错误", "发送次数必须大于 0")
            return

        interval = 0

        payload = self.payload_var.get().strip()
        if not payload:
            messagebox.showerror("错误", "发送内容不能为空")
            return

        mode = self.mode_var.get().strip().upper()
        if mode == "HEX":
            cmd = f"BURSTHEX {count} {interval} {payload}"
        else:
            cmd = f"BURST {count} {interval} {payload}"

        self._send_line(cmd)

    def _apply_mac_config(self) -> bool:
        mode = self.mac_mode_var.get().strip().upper()
        try:
            q = int(self.mac_q_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "q 必须是 0-100 的整数")
            return False

        if mode not in {"ALOHA", "CSMA"}:
            messagebox.showerror("错误", "MAC 模式必须是 ALOHA 或 CSMA")
            return False
        if q < 0 or q > 100:
            messagebox.showerror("错误", "q 必须在 0-100 之间")
            return False

        self._send_line(f"MAC {mode} {q}")
        return True

    def _apply_slot_config(self) -> bool:
        try:
            slot_ms = int(self.slot_ms_var.get().strip())
            win = int(self.csma_win_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "时隙和窗口必须是整数")
            return False

        if slot_ms <= 0:
            messagebox.showerror("错误", "时隙必须 >= 1 ms")
            return False
        if win <= 0:
            messagebox.showerror("错误", "CSMA 窗口必须 >= 1")
            return False

        self._send_line(f"SLOT {slot_ms} {win}")
        return True

    def _apply_slot_limit(self) -> bool:
        try:
            limit = int(self.slot_limit_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "时隙上限必须是整数")
            return False

        if limit < 0:
            messagebox.showerror("错误", "时隙上限必须 >= 0")
            return False

        self._send_line(f"SLOTLIMIT {limit}")
        return True

    def _stop_burst(self) -> None:
        self._send_line("STOP")

    def _query_status(self) -> None:
        self._send_line("STATUS")

    def _poll_status_tick(self) -> None:
        if self.conn_type is not None and self.auto_poll_var.get():
            self._query_status()
        self.root.after(self.poll_interval_ms, self._poll_status_tick)

    def _reset_stats(self) -> None:
        self._send_line("RESETSTATS")

    def _parse_schedule_datetime(self, text: str):
        text = text.strip()
        now = datetime.now()

        for fmt in ("%H:%M:%S", "%H:%M"):
            try:
                t = datetime.strptime(text, fmt).time()
                target = datetime.combine(now.date(), t)
                if target <= now:
                    target = target + timedelta(days=1)
                return target
            except ValueError:
                continue
        return None

    def _schedule_burst_at_time(self) -> None:
        target = self._parse_schedule_datetime(self.schedule_time_var.get())
        if target is None:
            messagebox.showerror("错误", "时间格式无效，请输入 HH:MM 或 HH:MM:SS")
            return

        if self.conn_type is None:
            messagebox.showerror("错误", "请先建立连接后再定时")
            return

        self._cancel_scheduled_burst(log_cancel=False)

        delay_sec = (target - datetime.now()).total_seconds()
        if delay_sec < 0:
            delay_sec = 0

        delay_ms = int(delay_sec * 1000)
        self.scheduled_send_job = self.root.after(delay_ms, self._run_scheduled_burst)
        self._log(f"已设置定时发送: {target.strftime('%Y-%m-%d %H:%M:%S')} (约 {delay_sec:.1f}s 后触发)")

    def _run_scheduled_burst(self) -> None:
        self.scheduled_send_job = None
        self._log(f"触发定时发送: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
        self._send_burst()

    def _cancel_scheduled_burst(self, log_cancel: bool = True) -> None:
        if self.scheduled_send_job is None:
            return
        try:
            self.root.after_cancel(self.scheduled_send_job)
        except Exception:
            pass
        self.scheduled_send_job = None
        if log_cancel:
            self._log("已取消定时发送")

    def _show_help(self) -> None:
        text = (
            "功能说明\n"
            "1. 连接: 可选择串口或无线 TCP 连接设备。\n"
            "2. 启用发送: 控制 TX 端是否允许发包。\n"
            "3. 发送突发包: 按设置连续发送 count 个包，间隔 interval(ms)。\n"
            "4. 停止发送: 中止当前突发发送任务。\n"
            "5. 查询状态: 主动发送 STATUS，刷新 ACK/RX 统计。\n"
            "6. 重置统计: 发送 RESETSTATS，清零统计计数。\n"
            "7. 自动轮询: 每秒自动查询 STATUS，不会自动发送业务载荷。\n"
            "8. MAC 设置: 选择 ALOHA/CSMA 和 q，再点“应用MAC设置”。\n\n"
            "9. 时隙设置: 配置时隙长度与 CSMA 窗口，再点“应用时隙设置”。\n"
            "10. 时隙上限: 设置发送任务最多运行 T 个时隙，超过后自动停止。\n\n"
            "无线连接\n"
            "- 设备会开启 SoftAP，默认 SSID 为 NRF24_CTRL。\n"
            "- 无线模式下先自动发送 AUTH token，再执行命令。\n"
            "- 默认主机地址是 192.168.4.1，端口是 3333。\n\n"
            "定时发送\n"
            "- 在“开始时间”输入 HH:MM 或 HH:MM:SS，例如 11:05 或 11:05:00。\n"
            "- 点击“定时发送突发包”后，到点会自动执行一次“发送突发包”。\n"
            "- 若该时间今天已过，会自动安排到明天同一时间。\n"
            "- 可用“取消定时”取消未触发的任务。\n\n"
            "模式说明\n"
            "- ASCII: 按文本发送，如 HELLO。\n"
            "- HEX: 按十六进制发送，如 101010 或 A1B2C3（必须偶数位）。\n\n"
            "建议流程\n"
            "连接 -> 启用发送 -> 设置参数 -> 发送突发包 -> 观察统计。"
        )
        messagebox.showinfo("帮助", text)


def main() -> None:
    parser = argparse.ArgumentParser(description="NRF24 upper computer controller")
    parser.add_argument("--mode", choices=["SERIAL", "TCP"], help="default connection mode")
    parser.add_argument("--host", help="TCP host")
    parser.add_argument("--port", help="TCP port")
    parser.add_argument("--token", help="TCP auth token")
    parser.add_argument("--baud", help="serial baud rate")
    parser.add_argument("--schedule", help="default schedule time, HH:MM or HH:MM:SS")
    parser.add_argument("--payload", help="default payload")
    args = parser.parse_args()

    initial_config = {}
    for key, value in (
        ("conn_mode", args.mode),
        ("host", args.host),
        ("tcp_port", args.port),
        ("token", args.token),
        ("baud", args.baud),
        ("schedule_time", args.schedule),
        ("payload", args.payload),
    ):
        if value is not None:
            initial_config[key] = value

    root = tk.Tk()
    app = Nrf24ControllerApp(root, initial_config=initial_config)

    def on_close() -> None:
        app._cancel_scheduled_burst(log_cancel=False)
        app._save_persisted_config()
        app._disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
