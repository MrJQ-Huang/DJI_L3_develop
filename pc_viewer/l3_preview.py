#!/usr/bin/env python3
"""Manifold-only HD video window. Forward original H.264; never start recording."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import time

from l3_stream import StreamReceiver

SOURCE_NAMES = {1: "可见光", 14: "点云画面（二维）"}
UNSUPPORTED = "0x00000021000000e0"


@dataclass
class PreviewState:
    current: int | None = None
    pending: bool = False
    message: str = "正在连接妙算3…"
    generation: int = 0

    def apply(self, event: dict[str, str]) -> None:
        if event.get("event") != "video_control" or event.get("action") not in ("status", "switch"):
            return
        try:
            current = int(event["current"])
        except (ValueError, KeyError):
            return
        current = current if current >= 0 else None
        # The first source observation is metadata for the startup connection,
        # not a switch. Reconnecting here would discard L3's startup IDR.
        if current != self.current and (self.current is not None or self.generation > 0):
            self.generation += 1
        self.current = current
        if event["action"] == "switch":
            self.pending = False
            if event.get("success") == "yes" and str(current) == event.get("requested"):
                self.message = "设备已确认画面源，等待新视频帧"
            elif event.get("code") == UNSUPPORTED:
                self.message = "设备拒绝此次切源；仍显示实际选中的画面"
            else:
                self.message = "切换未确认：" + event.get("code", "未知错误")
        elif current is None:
            self.message = "无法读取设备当前画面源"
        elif self.message == "正在连接妙算3…":
            self.message = "原始 H.264 回传 · 不降分辨率、不重新编码"


def parse_event(line: str) -> dict[str, str]:
    if not line.startswith("event="):
        return {}
    return dict(part.split("=", 1) for part in line.split() if "=" in part)


def ssh_command(args) -> list[str]:
    remote = ["bash", args.remote_root + "/scripts/run_l3_preview_onboard.sh",
              "--bin-dir", args.remote_root + "/build", "--config", args.remote_config,
              "--seconds", str(args.seconds), "--mount", str(args.mount),
              "--video-port", str(args.video_port)]
    if args.expected_source is not None:
        remote += ["--expected-source", str(args.expected_source)]
    command = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
               "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"]
    if args.identity:
        command += ["-i", args.identity]
    return command + [args.ssh_host, shlex.join(remote)]


class PreviewSession:
    def __init__(self, args):
        self.args = args
        self.state = PreviewState()
        self.lock = threading.Lock()
        self.ready = threading.Event()
        self.finished = threading.Event()
        self.process = None
        self.returncode = None
        root = Path(__file__).resolve().parents[1] / ".validation"
        root.mkdir(exist_ok=True, mode=0o700)
        self.log_dir = Path(tempfile.mkdtemp(prefix="hd-preview-", dir=root))
        self.worker = threading.Thread(target=self._run, daemon=True)

    def start(self):
        self.worker.start()

    def snapshot(self) -> PreviewState:
        with self.lock:
            return PreviewState(**vars(self.state))

    def _run(self):
        try:
            log_fd = os.open(self.log_dir / "onboard.private.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(log_fd, "w", encoding="utf-8") as log:
                self.process = subprocess.Popen(ssh_command(self.args), stdin=subprocess.PIPE,
                                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                                text=True, encoding="utf-8", errors="replace", bufsize=1)
                logged = 0
                # readline's bound also prevents an unexpected SDK line from growing RAM.
                while line := self.process.stdout.readline(8192):
                    if logged < 4 * 1024 * 1024:
                        log.write(line)
                        log.flush()
                        logged += len(line.encode("utf-8"))
                    event = parse_event(line)
                    with self.lock:
                        self.state.apply(event)
                    if event.get("phase") in ("video_listen", "video_hold"):
                        self.ready.set()
                self.returncode = self.process.wait()
        except (OSError, ValueError) as error:
            with self.lock:
                self.state.message = "连接失败：" + type(error).__name__
            self.returncode = 1
        finally:
            self.finished.set()

    def send(self, command: str) -> bool:
        if command not in ("status", "visible", "point", "stop"):
            raise ValueError("unknown preview command")
        with self.lock:
            if self.process is None or self.process.poll() is not None:
                return False
            if command in ("visible", "point"):
                if self.state.pending or self.state.current not in SOURCE_NAMES:
                    return False
                self.state.pending = True
                self.state.message = "正在等待设备确认切换…"
            try:
                self.process.stdin.write(command + "\n")
                self.process.stdin.flush()
                return True
            except (OSError, ValueError):
                self.state.pending = False
                self.state.message = "控制连接已断开"
                return False

    def close(self):
        self.send("stop")
        if self.process is not None:
            try:
                self.process.stdin.close()  # EOF also requests clean video-only shutdown.
            except OSError:
                pass
        self.worker.join(timeout=8)
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        # Remote timeout remains mandatory if an SDK call blocks cleanup.


class PreviewApp:
    def __init__(self, args):
        self.args = args
        self.session = PreviewSession(args)
        self.receiver = None
        self.started = time.monotonic()
        self.generation = 0
        self.last_report = self.started
        self.last_status = self.started
        self.previous_bytes = self.previous_frames = 0
        self.shown = 0
        self.received_frames = 0
        self.restarts = 0
        self.recovering = False
        self.active_source = None
        self.restart_source = None
        self.last_frame_ns = None
        self.final_frame_age = None
        self.last_stats = {}
        self.closed = False
        self.root = None
        if not args.no_display:
            import tkinter as tk
            from tkinter import ttk
            self.tk = tk
            self.root = tk.Tk()
            self.root.title("L3 高清实时预览")
            self.root.geometry("1280x800")
            self.root.minsize(640, 420)
            bar = ttk.Frame(self.root, padding=10)
            bar.pack(fill="x")
            self.source_text = tk.StringVar(value="当前画面：等待设备确认")
            ttk.Label(bar, textvariable=self.source_text).pack(side="left")
            self.toggle = ttk.Button(bar, text="切换画面", command=self.switch, state="disabled")
            self.toggle.pack(side="right", padx=6)
            ttk.Button(bar, text="停止预览", command=self.close).pack(side="right")
            self.message_text = tk.StringVar(value="正在连接妙算3…")
            ttk.Label(self.root, textvariable=self.message_text, padding=(10, 4)).pack(fill="x")
            self.canvas = tk.Canvas(self.root, background="#10151c", highlightthickness=0)
            self.canvas.pack(fill="both", expand=True)
            self.image_item = self.canvas.create_image(0, 0, anchor="center")
            self.metrics_text = tk.StringVar(value="原始高清 H.264 · 等待视频帧")
            ttk.Label(self.root, textvariable=self.metrics_text, padding=10).pack(fill="x")
            self.root.protocol("WM_DELETE_WINDOW", self.close)
            self.root.bind("<Escape>", lambda _event: self.close())

    def new_receiver(self):
        self.receiver = StreamReceiver(self.args.host, video_port=self.args.video_port,
                                       streams="video", socks5=self.args.socks5, io_timeout=10).start()
        self.shown = self.previous_bytes = self.previous_frames = 0

    def request_recovery(self, now, source=None):
        if self.recovering or now - self.started >= self.args.seconds - 10:
            return
        # A new TCP decoder cannot join L3's P-only continuation: it needs the
        # startup IDR. Stop this subscription and await full SDK cleanup before
        # starting a fresh process. This never changes the encoder or camera mode.
        self.recovering = True
        self.restart_source = source if source is not None else self.active_source
        self.receiver.stop()
        self.received_frames += self.receiver.stats()["video_frames"]
        self.receiver = None
        self.session.send("stop")
        with self.session.lock:
            self.session.state.message = ("正在切换画面，重新订阅原始高清流…" if source is not None else
                                          "视频连接中断，正在重新订阅原始高清流…")

    def can_switch(self, state):
        return (not self.recovering and not state.pending and self.session.ready.is_set() and
                not self.session.finished.is_set() and state.current in SOURCE_NAMES and
                time.monotonic() - self.started < self.args.seconds - 10)

    def switch(self):
        state = self.session.snapshot()
        if self.can_switch(state):
            self.session.send("visible" if state.current == 14 else "point")

    def tick(self):
        now = time.monotonic()
        state = self.session.snapshot()
        if self.root:
            label = SOURCE_NAMES.get(state.current, "未知（不能确认画面类型）")
            self.source_text.set("当前画面：" + label)
            self.message_text.set(state.message)
            self.toggle.configure(text="切到可见光" if state.current == 14 else "切到点云画面",
                                  state="normal" if self.can_switch(state) else "disabled")
        if self.session.ready.is_set() and self.receiver is None and not self.recovering:
            self.generation = state.generation
            self.active_source = state.current
            self.new_receiver()
        if self.receiver is not None:
            if state.current in SOURCE_NAMES:
                if self.active_source is None:
                    self.active_source = state.current
                elif state.current != self.active_source:
                    if self.root:
                        self.canvas.itemconfigure(self.image_item, image="")
                    self.request_recovery(now, source=state.current)
        if self.receiver is not None:
            video = self.receiver.latest_video()
            stats = self.receiver.stats()
            self.last_stats = stats
            if video is not None and video.sequence != self.shown:
                self.shown = video.sequence
                self.last_frame_ns = video.received_monotonic_ns
                if self.root:
                    import cv2
                    height, width = video.image.shape[:2]
                    cw, ch = max(self.canvas.winfo_width(), 1), max(self.canvas.winfo_height(), 1)
                    factor = min(cw / width, ch / height, 1)
                    # Fit only the local window. Network/decoder retain source resolution.
                    rgb = cv2.cvtColor(video.image, cv2.COLOR_BGR2RGB)
                    if factor < 1:
                        rgb = cv2.resize(rgb, (max(1, int(width * factor)), max(1, int(height * factor))))
                    h, w = rgb.shape[:2]
                    self.photo = self.tk.PhotoImage(data=f"P6\n{w} {h}\n255\n".encode() + rgb.tobytes(), format="PPM")
                    self.canvas.itemconfigure(self.image_item, image=self.photo)
                    self.canvas.coords(self.image_item, cw // 2, ch // 2)
            if now - self.last_report >= 1:
                elapsed = now - self.last_report
                mbps = (stats["video_bytes"] - self.previous_bytes) * 8 / elapsed / 1e6
                fps = (stats["video_frames"] - self.previous_frames) / elapsed
                dimensions = f"{video.image.shape[1]}×{video.image.shape[0]}" if video is not None else "等待首帧"
                age = (time.monotonic_ns() - video.received_monotonic_ns) / 1e9 if video is not None else 0
                stale = f" · {age:.1f}s 未收到新帧" if age > 2 else ""
                if self.root:
                    self.metrics_text.set(f"{dimensions} · 接收 {mbps:.1f} Mbps · 解码 {fps:.1f} fps · 原码流回传{stale}")
                print(json.dumps({"event": "hd_stats", "source": state.current, "dimensions": dimensions,
                                  "mbps": round(mbps, 2), "fps": round(fps, 2), **stats}), flush=True)
                self.previous_bytes, self.previous_frames = stats["video_bytes"], stats["video_frames"]
                self.last_report = now
            # The device resets video before its switch ACK reaches SSH. Let
            # that ACK establish the actual source before choosing a restart.
            if stats["video_disconnects"] > 0 and not state.pending and not self.session.finished.is_set():
                self.request_recovery(now)
        if now - self.last_status >= 3 and self.session.ready.is_set() and not state.pending and not self.recovering:
            self.session.send("status")
            self.last_status = now
        if self.session.finished.is_set() and self.recovering and self.session.returncode == 0:
            remaining = self.args.seconds - int(now - self.started)
            if remaining >= 5:
                self.session.close()
                session_args = argparse.Namespace(**vars(self.args))
                session_args.seconds = remaining
                # A switch result can arrive while the old session drains.
                # Its final readback takes precedence over the earlier snapshot.
                final_source = self.session.snapshot().current
                restart_source = final_source if final_source in SOURCE_NAMES else self.restart_source
                if restart_source is not None:
                    session_args.expected_source = restart_source
                self.session = PreviewSession(session_args)
                self.session.start()
                self.restarts += 1
                self.recovering = False
                print(json.dumps({"event": "hd_restart", "attempt": self.restarts,
                                  "remaining_seconds": remaining, "reencode": False}), flush=True)
        if self.session.finished.is_set() or now - self.started > self.args.seconds + 40:
            self.close()

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.final_frame_age = (time.monotonic_ns() - self.last_frame_ns) / 1e9 if self.last_frame_ns is not None else None
        if self.receiver:
            self.receiver.stop()
            self.last_stats = self.receiver.stats()
            self.received_frames += self.last_stats["video_frames"]
        self.session.close()
        if self.root:
            self.root.destroy()

    def run(self) -> int:
        self.session.start()
        try:
            if self.root:
                def update():
                    if not self.closed:
                        try:
                            self.tick()
                        except Exception:
                            self.close()
                            raise
                        if not self.closed:
                            self.root.after(30, update)
                self.root.after(30, update)
                self.root.mainloop()
            else:
                while not self.closed:
                    self.tick()
                    time.sleep(0.03)
        except KeyboardInterrupt:
            pass
        finally:
            self.close()
        print(json.dumps({"event": "hd_complete", "onboard_rc": self.session.returncode,
                          "restarts": self.restarts,
                          "last_frame_age_seconds": round(self.final_frame_age, 2) if self.final_frame_age is not None else None,
                          "received_frames": self.received_frames, "last_stats": self.last_stats,
                          "logs": str(self.session.log_dir)}), flush=True)
        fresh = self.final_frame_age is not None and self.final_frame_age < 3
        return 0 if self.session.returncode == 0 and self.received_frames > 0 and fresh else 1


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Manifold private/Tailnet host")
    parser.add_argument("--ssh-host", default="l3-manifold")
    parser.add_argument("--identity")
    parser.add_argument("--socks5", default="127.0.0.1:1055", help="Explicit SOCKS5 host:port; empty for direct TCP")
    parser.add_argument("--remote-root", default="/open_app/workspaces/dji/l3-dev/repair-20261117")
    parser.add_argument("--remote-config", default="/open_app/workspaces/dji/l3-dev/private/psdk.conf")
    parser.add_argument("--seconds", type=int, default=300, help="Bounded preview duration, 1..300 seconds")
    parser.add_argument("--mount", type=int, default=1)
    parser.add_argument("--video-port", type=int, default=9102)
    parser.add_argument("--expected-source", type=int)
    parser.add_argument("--no-display", action="store_true")
    args = parser.parse_args(argv)
    if (not 1 <= args.seconds <= 300 or args.mount not in (1, 2, 3) or
            not 1 <= args.video_port <= 65535 or args.video_port == 19101 or
            not args.ssh_host or args.ssh_host.startswith("-") or not args.host or
            not args.remote_root.startswith("/") or not args.remote_config.startswith("/") or
            args.expected_source is not None and not 0 <= args.expected_source <= 255):
        parser.error("invalid duration, mount, port, host, source or absolute remote path")
    args.socks5 = args.socks5 or None
    # Validate networking/decode prerequisites before starting the remote SDK.
    try:
        StreamReceiver(args.host, video_port=args.video_port, streams="video", socks5=args.socks5)
    except (ValueError, ImportError) as error:
        parser.error(str(error))
    return args


if __name__ == "__main__":
    os.umask(0o077)
    raise SystemExit(PreviewApp(arguments()).run())
