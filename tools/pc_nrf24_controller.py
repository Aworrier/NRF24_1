import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext

import serial
from serial.tools import list_ports


class Nrf24ControllerApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("NRF24 上位机控制台")
        self.root.geometry("860x620")

        self.ser = None
        self.reader_stop = threading.Event()
        self.reader_thread = None

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.enable_var = tk.BooleanVar(value=True)

        self.count_var = tk.StringVar(value="10")
        self.interval_var = tk.StringVar(value="30")
        self.mode_var = tk.StringVar(value="ASCII")
        self.payload_var = tk.StringVar(value="HELLO_NRF24")
        self.auto_poll_var = tk.BooleanVar(value=True)
        self.poll_interval_ms = 1000

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
        }

        self._build_ui()
        self._refresh_ports()

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

        tk.Button(top, text="连接", command=self._connect, width=12).grid(row=0, column=5, padx=6)
        tk.Button(top, text="断开", command=self._disconnect, width=12).grid(row=0, column=6, padx=6)
        tk.Button(top, text="Help", command=self._show_help, width=10).grid(row=0, column=7, padx=6)

        cmd = tk.LabelFrame(self.root, text="发包命令")
        cmd.pack(fill=tk.X, padx=10, pady=6)

        tk.Label(cmd, text="发送次数").grid(row=0, column=0, padx=6, pady=8, sticky="w")
        tk.Entry(cmd, textvariable=self.count_var, width=8).grid(row=0, column=1, padx=6)

        tk.Label(cmd, text="间隔(ms)").grid(row=0, column=2, padx=6, sticky="w")
        tk.Entry(cmd, textvariable=self.interval_var, width=10).grid(row=0, column=3, padx=6)

        tk.Label(cmd, text="模式").grid(row=0, column=4, padx=6, sticky="w")
        tk.OptionMenu(cmd, self.mode_var, "ASCII", "HEX").grid(row=0, column=5, padx=6)

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

        stats = tk.LabelFrame(self.root, text="ACK / RX 可靠性统计")
        stats.pack(fill=tk.X, padx=10, pady=6)

        tk.Label(stats, text="角色").grid(row=0, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["role"], width=8, anchor="w").grid(row=0, column=1, padx=6)
        tk.Label(stats, text="已发送").grid(row=0, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["sent"], width=8, anchor="w").grid(row=0, column=3, padx=6)
        tk.Label(stats, text="ACK_OK").grid(row=0, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ack_ok"], width=8, anchor="w").grid(row=0, column=5, padx=6)
        tk.Label(stats, text="ACK_FAIL").grid(row=0, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ack_fail"], width=8, anchor="w").grid(row=0, column=7, padx=6)

        tk.Label(stats, text="重传总数").grid(row=1, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["retries_sum"], width=8, anchor="w").grid(row=1, column=1, padx=6)
        tk.Label(stats, text="单包最大重传").grid(row=1, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["retries_max"], width=8, anchor="w").grid(row=1, column=3, padx=6)
        tk.Label(stats, text="接收包数").grid(row=1, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["rx_pkt"], width=8, anchor="w").grid(row=1, column=5, padx=6)
        tk.Label(stats, text="有效帧数").grid(row=1, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["frame_ok"], width=8, anchor="w").grid(row=1, column=7, padx=6)

        tk.Label(stats, text="CRC失败").grid(row=2, column=0, sticky="w", padx=6, pady=2)
        tk.Label(stats, textvariable=self.stat_vars["crc_fail"], width=8, anchor="w").grid(row=2, column=1, padx=6)
        tk.Label(stats, text="序号丢失").grid(row=2, column=2, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["gap"], width=8, anchor="w").grid(row=2, column=3, padx=6)
        tk.Label(stats, text="重复包").grid(row=2, column=4, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["dup"], width=8, anchor="w").grid(row=2, column=5, padx=6)
        tk.Label(stats, text="乱序包").grid(row=2, column=6, sticky="w", padx=6)
        tk.Label(stats, textvariable=self.stat_vars["ooo"], width=8, anchor="w").grid(row=2, column=7, padx=6)

        guide = tk.Label(
            self.root,
            text="提示：先连接串口，再点“发送突发包”。勾选“自动轮询”可自动刷新统计。",
            anchor="w",
            fg="#1f4f8f",
        )
        guide.pack(fill=tk.X, padx=12, pady=2)

        self.log = scrolledtext.ScrolledText(self.root, height=18)
        self.log.pack(fill=tk.BOTH, expand=True, padx=10, pady=8)

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

    def _send_line(self, line: str) -> None:
        if self.ser is None:
            messagebox.showerror("错误", "串口未连接")
            return

        self.ser.write((line + "\n").encode("utf-8"))
        self._log("=> " + line)

    def _connect(self) -> None:
        if self.ser is not None:
            self._log("已连接，无需重复连接")
            return

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

        self.reader_stop.clear()
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()
        self._log(f"连接成功: {port} @ {baud}")
        self.root.after(200, self._query_status)
        self.root.after(self.poll_interval_ms, self._poll_status_tick)

    def _disconnect(self) -> None:
        if self.ser is None:
            return

        self.reader_stop.set()
        try:
            self.ser.close()
        except Exception:
            pass
        self.ser = None
        self._log("串口已断开")

    def _reader_loop(self) -> None:
        while not self.reader_stop.is_set():
            try:
                if self.ser is None:
                    break
                data = self.ser.readline()
                if not data:
                    continue
                text = data.decode("utf-8", errors="replace").strip()
                if text:
                    self.root.after(0, self._log, "<= " + text)
                    stat_line = self._extract_stat_line(text)
                    if stat_line is not None:
                        self.root.after(0, self._update_stats_from_line, stat_line)
            except Exception as exc:
                self.root.after(0, self._log, f"Reader error: {exc}")
                break

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
        }
        for src_key, dst_key in mapping.items():
            if src_key in kv and dst_key in self.stat_vars:
                self.stat_vars[dst_key].set(kv[src_key])

    def _toggle_enable(self) -> None:
        cmd = "ENABLE 1" if self.enable_var.get() else "ENABLE 0"
        self._send_line(cmd)

    def _send_burst(self) -> None:
        try:
            count = int(self.count_var.get().strip())
            interval = int(self.interval_var.get().strip())
        except ValueError:
            messagebox.showerror("错误", "发送次数和间隔必须是整数")
            return

        if count <= 0:
            messagebox.showerror("错误", "发送次数必须大于 0")
            return
        if interval < 0:
            messagebox.showerror("错误", "间隔必须大于等于 0")
            return

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

    def _stop_burst(self) -> None:
        self._send_line("STOP")

    def _query_status(self) -> None:
        self._send_line("STATUS")

    def _poll_status_tick(self) -> None:
        if self.ser is not None and self.auto_poll_var.get():
            self._query_status()
        self.root.after(self.poll_interval_ms, self._poll_status_tick)

    def _reset_stats(self) -> None:
        self._send_line("RESETSTATS")

    def _show_help(self) -> None:
        text = (
            "功能说明\n"
            "1. 连接: 选择串口和波特率后连接设备。\n"
            "2. 启用发送: 控制 TX 端是否允许发包。\n"
            "3. 发送突发包: 按设置连续发送 count 个包，间隔 interval(ms)。\n"
            "4. 停止发送: 中止当前突发发送任务。\n"
            "5. 查询状态: 主动发送 STATUS，刷新 ACK/RX 统计。\n"
            "6. 重置统计: 发送 RESETSTATS，清零统计计数。\n"
            "7. 自动轮询: 每秒自动查询 STATUS，不会自动发送业务载荷。\n\n"
            "模式说明\n"
            "- ASCII: 按文本发送，如 HELLO。\n"
            "- HEX: 按十六进制发送，如 101010 或 A1B2C3（必须偶数位）。\n\n"
            "建议流程\n"
            "连接 -> 启用发送 -> 设置参数 -> 发送突发包 -> 观察统计。"
        )
        messagebox.showinfo("帮助", text)


def main() -> None:
    root = tk.Tk()
    app = Nrf24ControllerApp(root)

    def on_close() -> None:
        app._disconnect()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
