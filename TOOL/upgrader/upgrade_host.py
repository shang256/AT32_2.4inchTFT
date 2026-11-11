# -*- coding: utf-8 -*-
"""
AT32 固件升级上位机(TypeC 虚拟串口)

用法:
    python upgrade_host.py [固件.bin]        # 可带文件参数启动, 也可拖入/按钮选择

功能流程:
    1. 自动扫描 VID/PID 匹配的虚拟串口并发送 HELLO 握手
    2. 设备运行 APP 时自动发送 REBOOT 命令, 等待重枚举后进入 BOOT
    3. ERASE -> DATA(1KB/包) -> VERIFY(CRC32) -> DONE 完成升级
    4. 弹窗提示"升级中"进度与"升级成功"消息

协议(与固件 Software/Shared/boot_shared.h 一致):
    主机->设备: [0xA5][cmd][len16LE][payload]
    设备->主机: [0x5A][cmd][status][len16LE][payload]

依赖: pip install -r requirements.txt (pyserial 必需, tkinterdnd2 可选拖拽)
"""

import os
import sys
import time
import zlib
import queue
import threading

import serial
from serial.tools import list_ports

# ==================== 协议常量(与 boot_shared.h 同步) ====================
VID = 0x2E3C
PID = 0x5741
HEAD_HOST = 0xA5
HEAD_DEV = 0x5A
CMD_HELLO = 0x01
CMD_REBOOT = 0x02
CMD_ERASE = 0x10
CMD_DATA = 0x11
CMD_VERIFY = 0x12
CMD_DONE = 0x13
CMD_ABORT = 0x14
STATUS_OK = 0x00
MODE_BOOT = 0x00
MODE_APP = 0x01
DATA_MAX_LEN = 1024

# 超时参数(秒)
TMO_ACK = 3.0          # 常规应答
TMO_ERASE = 90.0       # 全区擦除(约 10s, 留足余量)
TMO_REENUM = 25.0      # REBOOT 后等待重枚举
RETRY_MAX = 3          # 数据包重试次数


# ==================== 协议帧收发 ====================
def frame_pack(cmd, payload=b""):
    """组主机请求帧 [0xA5][cmd][len16LE][payload]"""
    return bytes([HEAD_HOST, cmd, len(payload) & 0xFF, len(payload) >> 8]) + payload


class Device:
    """升级设备(BOOT 或 APP 模式)的串口封装"""

    def __init__(self, port):
        self.ser = serial.Serial(port, 115200, timeout=0.05, write_timeout=5.0)
        self.rx_buf = b""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _read_more(self):
        """读一次串口并入缓冲"""
        data = self.ser.read(256)
        if data:
            self.rx_buf += data

    def send(self, cmd, payload=b""):
        """发送一帧请求"""
        self.ser.write(frame_pack(cmd, payload))
        self.ser.flush()

    def recv_frame(self, timeout):
        """等待并解析一帧应答, 返回 (cmd, status, payload) 或 None(超时)"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._read_more()
            while len(self.rx_buf) >= 5:
                if self.rx_buf[0] != HEAD_DEV:
                    # 逐字节重新同步
                    self.rx_buf = self.rx_buf[1:]
                    continue
                plen = self.rx_buf[3] | (self.rx_buf[4] << 8)
                if len(self.rx_buf) < 5 + plen:
                    break
                cmd = self.rx_buf[1]
                status = self.rx_buf[2]
                payload = self.rx_buf[5:5 + plen]
                self.rx_buf = self.rx_buf[5 + plen:]
                return cmd, status, payload
            time.sleep(0.005)
        return None

    def transact(self, cmd, payload=b"", timeout=TMO_ACK):
        """发送请求并等待同命令应答"""
        self.rx_buf = b""
        self.send(cmd, payload)
        deadline = time.time() + timeout
        while time.time() < deadline:
            rsp = self.recv_frame(max(0.05, deadline - time.time()))
            if rsp is None:
                continue
            rcmd, status, rpayload = rsp
            if rcmd == cmd:
                return status, rpayload
        return None, None

    def hello(self):
        """握手, 返回 (mode, app_valid) 或 None"""
        status, payload = self.transact(CMD_HELLO, timeout=1.0)
        if status == STATUS_OK and len(payload) >= 3:
            return payload[0], payload[2]
        return None


def find_device_ports():
    """扫描 VID/PID 匹配的虚拟串口列表"""
    ports = []
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            ports.append(p.device)
    return ports


# ==================== 升级流程 ====================
class UpgradeWorker(threading.Thread):
    """后台升级线程: 连接 -> 必要时切 BOOT -> 擦写 -> 校验 -> 完成"""

    def __init__(self, path, ui):
        super().__init__(daemon=True)
        self.path = path          # 固件路径
        self.ui = ui              # UI 回调集合
        self.stop_flag = False

    def run(self):
        try:
            self._run()
        except Exception as e:  # noqa: BLE001
            self.ui.fail("升级失败: %s" % str(e))
        finally:
            self.ui.finish()

    def _stop_pending(self):
        return self.stop_flag

    def _run(self):
        # ---- Step1: 读取固件 ----
        self.ui.status("正在读取固件...")
        with open(self.path, "rb") as f:
            fw = f.read()
        size = len(fw)
        crc = zlib.crc32(fw) & 0xFFFFFFFF
        self.ui.log("固件: %s (%d 字节, CRC32=%08X)" % (os.path.basename(self.path), size, crc))
        if size == 0:
            raise RuntimeError("固件文件为空")

        # ---- Step2: 连接设备(必要时命令 APP 重启进 BOOT) ----
        dev = self._connect_and_enter_boot()
        if dev is None:
            raise RuntimeError("未找到升级设备, 请确认设备已通过 TypeC 连接")

        try:
            # ---- Step3: 擦除 ----
            self.ui.status("正在擦除 APP 区(约 10 秒, 请稍候)...")
            self.ui.log("擦除 APP 区...")
            st, _ = dev.transact(CMD_ERASE, size.to_bytes(4, "little"), timeout=TMO_ERASE)
            if st != STATUS_OK:
                raise RuntimeError("擦除失败(status=%s)" % st)

            # ---- Step4: 逐包写入 ----
            total_pkts = (size + DATA_MAX_LEN - 1) // DATA_MAX_LEN
            for seq in range(total_pkts):
                if self._stop_pending():
                    dev.transact(CMD_ABORT, timeout=1.0)
                    raise RuntimeError("已中止")
                chunk = fw[seq * DATA_MAX_LEN:(seq + 1) * DATA_MAX_LEN]
                payload = seq.to_bytes(4, "little") + len(chunk).to_bytes(2, "little") + chunk
                st, rsp = self._send_with_retry(dev, payload)
                if st != STATUS_OK:
                    raise RuntimeError("第 %d 包写入失败" % seq)
                if seq % 4 == 0 or seq == total_pkts - 1:
                    self.ui.progress((seq + 1) / total_pkts)
                    self.ui.status("正在升级... %d/%d (%.0f%%)" % (
                        seq + 1, total_pkts, (seq + 1) * 100.0 / total_pkts))

            # ---- Step5: CRC 校验 ----
            self.ui.status("正在校验...")
            self.ui.log("CRC32 校验...")
            st, _ = dev.transact(CMD_VERIFY, size.to_bytes(4, "little") + crc.to_bytes(4, "little"),
                                 timeout=15.0)
            if st != STATUS_OK:
                raise RuntimeError("CRC32 校验失败, 请重试升级")

            # ---- Step6: 完成(BOOT 写参数页并跳转新 APP) ----
            self.ui.progress(1.0)
            st, _ = dev.transact(CMD_DONE, size.to_bytes(4, "little") + crc.to_bytes(4, "little"))
            if st != STATUS_OK:
                raise RuntimeError("完成命令失败")
            self.ui.log("升级成功, 设备正在启动新固件")
            self.ui.success("升级成功!")
        finally:
            dev.close()

    def _send_with_retry(self, dev, payload):
        """DATA 包发送(带重试)"""
        for _ in range(RETRY_MAX):
            if self._stop_pending():
                return None, None
            st, rsp = dev.transact(CMD_DATA, payload, timeout=5.0)
            if st == STATUS_OK:
                return st, rsp
        return st, rsp

    def _connect_and_enter_boot(self):
        """连接设备; APP 模式则命令其重启并等待 BOOT 重枚举"""
        self.ui.status("正在检测设备...")
        for port in find_device_ports():
            try:
                dev = Device(port)
            except Exception:
                continue
            hello = dev.hello()
            if hello is None:
                dev.close()
                continue
            mode, _valid = hello
            if mode == MODE_BOOT:
                self.ui.log("已连接 BOOT(端口 %s)" % port)
                return dev
            # APP 模式: 命令重启进 BOOT
            self.ui.log("设备运行中(端口 %s), 正在重启进入 BOOT..." % port)
            self.ui.status("正在重启设备进入升级模式...")
            dev.send(CMD_REBOOT)
            time.sleep(0.2)
            dev.close()
            return self._wait_boot_reenum()
        return None

    def _wait_boot_reenum(self):
        """等待设备重枚举为 BOOT(轮询新串口并握手)"""
        deadline = time.time() + TMO_REENUM
        while time.time() < deadline and not self._stop_pending():
            time.sleep(0.5)
            for port in find_device_ports():
                try:
                    dev = Device(port)
                except Exception:
                    continue
                hello = dev.hello()
                if hello is not None and hello[0] == MODE_BOOT:
                    self.ui.log("BOOT 已就绪(端口 %s)" % port)
                    return dev
                dev.close()
        return None


# ==================== 图形界面 ====================
class UpgraderUI:
    """tkinter 界面: 拖入/选择固件 -> 自动升级 -> 结果提示"""

    def __init__(self):
        import tkinter as tk
        from tkinter import ttk, filedialog, messagebox

        self.tk = tk
        self.ttk = ttk
        self.filedialog = filedialog
        self.messagebox = messagebox

        self.root = self._create_root()
        self.root.title("AT32 固件升级工具 (TypeC)")
        self.root.geometry("520x420")
        self.root.minsize(480, 380)

        self.msg_queue = queue.Queue()   # UI 线程事件队列
        self.worker = None               # 当前升级线程
        self.fw_path = None              # 已选固件路径

        self._build()
        self.root.after(80, self._poll_queue)

    def _create_root(self):
        """创建根窗口(优先启用拖拽支持)"""
        try:
            from tkinterdnd2 import TkinterDnD, DND_FILES
            root = TkinterDnD.Tk()
            self.dnd_files = DND_FILES
            self.dnd_ok = True
        except Exception:
            self.dnd_ok = False
            root = self.tk.Tk()
        return root

    def _build(self):
        """构建界面控件"""
        pad = {"padx": 12, "pady": 4}

        # 设备状态行
        bar = self.ttk.Frame(self.root)
        bar.pack(fill="x", **pad)
        self.ttk.Label(bar, text="设备:").pack(side="left")
        self.dev_label = self.ttk.Label(bar, text="未检测(选择固件后自动连接)", foreground="#666666")
        self.dev_label.pack(side="left", padx=8)

        # 拖放区
        self.drop_frame = self.ttk.LabelFrame(self.root, text=" 固件文件 ")
        self.drop_frame.pack(fill="both", expand=True, **pad)
        self.drop_label = self.tk.Label(self.drop_frame,
                                        text=("将 .bin 固件文件拖到此处\n\n或点击此处选择文件"),
                                        justify="center", fg="#555555", bg="#FAFAFA",
                                        cursor="hand2")
        self.drop_label.pack(fill="both", expand=True, padx=8, pady=8)
        self.drop_label.bind("<Button-1>", lambda e: self.choose_file())
        if self.dnd_ok:
            self.drop_label.drop_target_register(self.dnd_files)
            self.drop_label.dnd_bind("<<Drop>>", self._on_drop)

        # 进度条
        self.progress = self.ttk.Progressbar(self.root, maximum=1.0)
        self.progress.pack(fill="x", **pad)

        # 状态行
        self.status_var = self.tk.StringVar(value="等待固件文件")
        self.ttk.Label(self.root, textvariable=self.status_var, anchor="w").pack(fill="x", **pad)

        # 日志区
        self.log_text = self.tk.Text(self.root, height=7, state="disabled",
                                     bg="#1E1E1E", fg="#CCCCCC", font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, **pad)

    # ---------- 事件 ----------
    def _on_drop(self, event):
        """拖放回调: 取第一个文件路径"""
        path = self.root.tk.splitlist(event.data)
        if path:
            self.set_file(path[0].strip("{}"))

    def choose_file(self):
        """按钮选择固件文件"""
        path = self.filedialog.askopenfilename(
            title="选择 APP 固件", filetypes=[("固件文件", "*.bin"), ("所有文件", "*.*")])
        if path:
            self.set_file(path)

    def set_file(self, path):
        """设定固件并自动开始升级"""
        if self.worker is not None and self.worker.is_alive():
            self.messagebox.showwarning("忙", "正在升级中, 请等待完成")
            return
        if not os.path.isfile(path):
            self.messagebox.showerror("错误", "文件不存在: %s" % path)
            return
        self.fw_path = path
        self.drop_label.config(text=os.path.basename(path) + "\n\n(点击可重新选择)", fg="#0B6E0B")
        self.log("已选择固件: %s" % path)
        self.start_upgrade()

    def start_upgrade(self):
        """启动升级线程"""
        self.worker = UpgradeWorker(self.fw_path, self)
        self.worker.start()

    # ---------- 工作线程回调(经队列转发到 UI 线程) ----------
    def status(self, text):
        self.msg_queue.put(("status", text))

    def log(self, text):
        self.msg_queue.put(("log", text + "\n"))

    def progress(self, ratio):
        self.msg_queue.put(("progress", ratio))

    def dev_info(self, text):
        self.msg_queue.put(("dev", text))

    def success(self, text):
        self.msg_queue.put(("success", text))

    def fail(self, text):
        self.msg_queue.put(("fail", text))

    def finish(self):
        self.msg_queue.put(("finish", None))

    # ---------- UI 线程处理 ----------
    def _poll_queue(self):
        try:
            while True:
                kind, value = self.msg_queue.get_nowait()
                if kind == "status":
                    self.status_var.set(value)
                elif kind == "log":
                    self.log_text.config(state="normal")
                    self.log_text.insert("end", value)
                    self.log_text.see("end")
                    self.log_text.config(state="disabled")
                elif kind == "progress":
                    self.progress["value"] = value
                elif kind == "dev":
                    self.dev_label.config(text=value, foreground="#0B6E0B")
                elif kind == "success":
                    self.status_var.set("升级成功")
                    self.progress["value"] = 1.0
                    self.messagebox.showinfo("升级成功", value + "\n\n设备已启动新固件。")
                    self.dev_label.config(text="未检测", foreground="#666666")
                elif kind == "fail":
                    self.status_var.set("升级失败")
                    self.messagebox.showerror("升级失败", value)
                elif kind == "finish":
                    self.worker = None
        except queue.Empty:
            pass
        self.root.after(80, self._poll_queue)

    def run(self):
        self.root.mainloop()


def main():
    ui = UpgraderUI()
    # 命令行带固件参数: 启动后自动载入
    if len(sys.argv) > 1 and os.path.isfile(sys.argv[1]):
        ui.root.after(300, lambda: ui.set_file(sys.argv[1]))
    ui.run()


if __name__ == "__main__":
    main()
