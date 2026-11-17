"""Receive L3P1 point packets and raw H.264 over two independent TCP streams.

No DJI SDK or aircraft commands run here. Source timestamp/XYZ values remain
unconverted; received_monotonic_ns is this computer's receive/decode time.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import importlib
import json
import math
import os
import socket
import struct
import threading
import time

import numpy as np

try:
    from .l3_points import Packet, read_packet
except ImportError:
    from l3_points import Packet, read_packet


POINT_DTYPE = np.dtype([(name, "<f4") for name in ("x", "y", "z")] +
                       [(name, "u1") for name in ("intensity", "r", "g", "b")])
MAX_VIDEO_PACKET = 8 * 1024 * 1024
MAX_PIXELS = 32 * 1024 * 1024


@dataclass(frozen=True)
class VideoFrame:
    image: np.ndarray  # Read-only uint8 BGR; copy before changing it.
    sequence: int      # Local decoded-frame count, not a sensor sequence.
    received_monotonic_ns: int


@dataclass(frozen=True)
class PointFrame:
    points: np.ndarray  # Read-only packed x/y/z/intensity/r/g/b records.
    sequence: int
    source_timestamp_raw: int
    crc_header_raw: int
    crc_rest_raw: int
    received_monotonic_ns: int

    @property
    def xyz(self) -> np.ndarray:
        """Return an Nx3 array in the source's still-unverified units/axes."""
        return np.column_stack([self.points[name] for name in ("x", "y", "z")])


def point_frame(packet: Packet) -> PointFrame:
    points = np.frombuffer(packet.points, dtype=POINT_DTYPE)
    if not all(np.isfinite(points[name]).all() for name in ("x", "y", "z")):
        raise ValueError("non-finite XYZ coordinate")
    return PointFrame(points, packet.sequence, packet.source_timestamp_raw,
                      packet.crc_header_raw, packet.crc_rest_raw, time.monotonic_ns())


class StreamReceiver:
    """Two receiving workers, with one latest-value slot per stream.

    Use start(), latest_video()/latest_points(), stats(), then stop(). Readers
    may skip old frames without making the network/decoder queue grow. TCP
    reconnects create a new H.264 parser; the sender must supply SPS/PPS + IDR.
    """

    def __init__(self, host: str, point_port: int = 9101, video_port: int = 9102,
                 streams: str = "both", reconnect_delay: float = 0.25,
                 io_timeout: float = 2.0, socks5: str | None = None):
        if not host or streams not in ("both", "video", "points"):
            raise ValueError("host and streams=both|video|points are required")
        if not all(1 <= port <= 65535 for port in (point_port, video_port)):
            raise ValueError("ports must be in 1..65535")
        if not all(math.isfinite(v) and v > 0 for v in (io_timeout, reconnect_delay)):
            raise ValueError("timeouts must be finite and positive")
        self.host, self.point_port, self.video_port = host, point_port, video_port
        self.streams, self.reconnect_delay, self.io_timeout = streams, reconnect_delay, io_timeout
        self._proxy = None
        if socks5 is not None:
            try:
                proxy_host, proxy_port = socks5.rsplit(":", 1)
                if not proxy_host or not 1 <= int(proxy_port) <= 65535:
                    raise ValueError
                self._proxy = proxy_host, int(proxy_port)
            except (ValueError, AttributeError) as error:
                raise ValueError("socks5 must be an explicit host:port") from error
        self._av = importlib.import_module("av") if streams != "points" else None
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._sockets: set[socket.socket] = set()
        self._threads: list[threading.Thread] = []
        self._started = False
        self._video: VideoFrame | None = None
        self._points: PointFrame | None = None
        self._video_observed = self._points_observed = 0
        self._point_updates = 0
        self._stats = dict(video_connections=0, point_connections=0,
                           video_disconnects=0, point_disconnects=0,
                           video_connected=False, point_connected=False,
                           video_bytes=0, video_frames=0, point_packets=0,
                           points_received=0, decode_errors=0, point_errors=0,
                           connection_errors=0, video_superseded=0, point_superseded=0,
                           video_error="", point_error="")

    def start(self) -> StreamReceiver:
        with self._lock:
            if self._started or self._stop.is_set():
                raise RuntimeError("a receiver can only be started once")
            self._started = True
        for name in ("video", "point"):
            if (name == "video" and self.streams == "points" or
                    name == "point" and self.streams == "video"):
                continue
            thread = threading.Thread(target=self._receive, args=(name,),
                                      name="l3-" + name, daemon=True)
            self._threads.append(thread)
            thread.start()
        return self

    def stop(self) -> None:
        self._stop.set()
        with self._lock:
            sockets = list(self._sockets)
        for peer in sockets:
            try:
                peer.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            peer.close()
        for thread in self._threads:
            if thread is not threading.current_thread():
                thread.join(timeout=self.io_timeout + 1)

    def latest_video(self) -> VideoFrame | None:
        with self._lock:
            if self._video is not None:
                self._video_observed = self._video.sequence
            return self._video

    def latest_points(self) -> PointFrame | None:
        with self._lock:
            self._points_observed = self._point_updates
            return self._points

    def stats(self) -> dict:
        with self._lock:
            return {**self._stats, "state": "stopped" if self._stop.is_set() else "running",
                    "video_slots": int(self._video is not None),
                    "point_slots": int(self._points is not None)}

    def _receive(self, name: str) -> None:
        port = self.video_port if name == "video" else self.point_port
        while not self._stop.is_set():
            peer = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            peer.settimeout(self.io_timeout)
            connected = False
            with self._lock:
                if self._stop.is_set():
                    peer.close()
                    break
                self._sockets.add(peer)
            try:
                self._connect(peer, port)
                connected = True
                with self._lock:
                    self._stats[name + "_connections"] += 1
                    self._stats[name + "_connected"] = True
                    self._stats[name + "_error"] = ""
                if name == "video":
                    self._receive_video(peer)
                else:
                    self._receive_points(peer)
            except (OSError, EOFError, ValueError) as error:
                if not self._stop.is_set():
                    with self._lock:
                        if not connected:
                            self._stats["connection_errors"] += 1
                        elif isinstance(error, ValueError):
                            self._stats["point_errors" if name == "point" else "decode_errors"] += 1
                        self._stats[name + "_error"] = type(error).__name__
            finally:
                with self._lock:
                    self._sockets.discard(peer)
                    self._stats[name + "_connected"] = False
                    if connected:
                        self._stats[name + "_disconnects"] += 1
                peer.close()
            self._stop.wait(self.reconnect_delay)

    def _connect(self, peer: socket.socket, port: int) -> None:
        """Direct TCP or explicit no-auth SOCKS5; never use environment proxies."""
        if self._proxy is None:
            peer.connect((self.host, port))
            return
        deadline = time.monotonic() + self.io_timeout

        def remaining_timeout():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("SOCKS5 connect deadline exceeded")
            peer.settimeout(remaining)

        def exact(length):
            data = bytearray()
            while len(data) < length:
                remaining_timeout()
                chunk = peer.recv(length - len(data))
                if not chunk:
                    raise EOFError("SOCKS5 closed during handshake")
                data.extend(chunk)
            return bytes(data)

        remaining_timeout()
        peer.connect(self._proxy)
        remaining_timeout()
        peer.sendall(b"\x05\x01\x00")
        if exact(2) != b"\x05\x00":
            raise OSError("SOCKS5 proxy requires unsupported authentication")
        try:
            target = b"\x01" + socket.inet_aton(self.host)
        except OSError:
            name = self.host.encode("idna")
            if not 1 <= len(name) <= 255:
                raise ValueError("SOCKS5 target name length outside 1..255")
            target = b"\x03" + bytes([len(name)]) + name
        remaining_timeout()
        peer.sendall(b"\x05\x01\x00" + target + struct.pack("!H", port))
        version, reply, reserved, kind = exact(4)
        if version != 5 or reply != 0 or reserved != 0:
            raise OSError("SOCKS5 CONNECT rejected")
        if kind == 1:
            exact(4)
        elif kind == 3:
            exact(exact(1)[0])
        elif kind == 4:
            exact(16)
        else:
            raise OSError("invalid SOCKS5 bound address")
        exact(2)
        peer.settimeout(self.io_timeout)

    def _receive_points(self, peer: socket.socket) -> None:
        while not self._stop.is_set():
            frame = point_frame(read_packet(peer, time.monotonic() + self.io_timeout))
            with self._lock:
                if self._point_updates > self._points_observed:
                    self._stats["point_superseded"] += 1
                self._point_updates += 1
                self._points = frame
                self._stats["point_packets"] += 1
                self._stats["points_received"] += len(frame.points)

    def _receive_video(self, peer: socket.socket) -> None:
        codec = self._av.CodecContext.create("h264", "r")
        codec.thread_count = 1
        pending = 0
        try:
            while not self._stop.is_set():
                chunk = peer.recv(64 * 1024)
                if not chunk:
                    for packet in codec.parse(None):
                        self._decode(codec, packet)
                    self._decode(codec, None)
                    return
                with self._lock:
                    self._stats["video_bytes"] += len(chunk)
                pending += len(chunk)
                packets = codec.parse(chunk)
                if packets:
                    pending = 0
                if pending > MAX_VIDEO_PACKET:
                    raise ValueError("H.264 parser packet exceeds 8 MiB")
                for packet in packets:
                    self._decode(codec, packet)
        except self._av.error.FFmpegError as error:
            raise ValueError("invalid H.264 stream") from error

    def _decode(self, codec, packet) -> None:
        for decoded in codec.decode(packet):
            if getattr(decoded, "is_corrupt", False):
                raise ValueError("corrupt decoded frame")
            if (decoded.width <= 0 or decoded.height <= 0 or decoded.width > 8192 or
                    decoded.height > 8192 or decoded.width * decoded.height > MAX_PIXELS):
                raise ValueError("decoded image exceeds size limit")
            image = decoded.to_ndarray(format="bgr24")
            image.flags.writeable = False
            with self._lock:
                if self._video is not None and self._video.sequence > self._video_observed:
                    self._stats["video_superseded"] += 1
                self._stats["video_frames"] += 1
                self._video = VideoFrame(image, self._stats["video_frames"], time.monotonic_ns())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Manifold USB or Tailnet IPv4 address")
    parser.add_argument("--point-port", type=int, default=9101)
    parser.add_argument("--video-port", type=int, default=9102)
    parser.add_argument("--streams", choices=("both", "video", "points"), default="both")
    parser.add_argument("--socks5", help="Explicit no-auth SOCKS5 host:port, e.g. 127.0.0.1:1055")
    parser.add_argument("--io-timeout", type=float, default=10,
                        help="TCP/initial-keyframe wait in seconds (default: 10)")
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--seconds", type=float, default=0, help="0: until Ctrl+C/ESC")
    parser.add_argument("--max-frames", type=int, default=0)
    args = parser.parse_args()
    if (not math.isfinite(args.seconds) or args.seconds < 0 or args.max_frames < 0 or
            (args.streams == "points" and args.max_frames)):
        parser.error("duration/frame limit must be nonnegative; points has no video frames")
    display = not args.no_display and args.streams != "points"
    if display and os.name != "nt" and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        parser.error("no graphical display; use --no-display")
    try:
        receiver = StreamReceiver(args.host, args.point_port, args.video_port, args.streams,
                                  io_timeout=args.io_timeout, socks5=args.socks5)
    except (ValueError, ImportError) as error:
        parser.error(str(error) + "; install requirements.txt if a dependency is missing")
    if display:
        import cv2
    started = last_report = time.monotonic()
    shown = 0
    interrupted = False
    receiver.start()
    try:
        while True:
            now = time.monotonic()
            stats = receiver.stats()
            if args.seconds and now - started >= args.seconds:
                break
            if args.max_frames and stats["video_frames"] >= args.max_frames:
                break
            if display:
                video = receiver.latest_video()
                if video is not None and video.sequence != shown:
                    cv2.imshow("L3 visible light", video.image)
                    shown = video.sequence
                if cv2.waitKey(1) & 0xFF == 27:
                    break
                if shown and cv2.getWindowProperty("L3 visible light", cv2.WND_PROP_VISIBLE) < 1:
                    break
            if now - last_report >= 1:
                print(json.dumps({"event": "stats", **stats}), flush=True)
                last_report = now
            time.sleep(0.02)
    except KeyboardInterrupt:
        interrupted = True
    finally:
        receiver.stop()
        if display:
            cv2.destroyAllWindows()
    stats = receiver.stats()
    passed = ((args.streams == "points" or stats["video_frames"] > 0) and
              (args.streams == "video" or stats["point_packets"] > 0))
    print(json.dumps({"event": "complete", "received_requested_streams": passed,
                      "elapsed_seconds": round(time.monotonic() - started, 3), **stats}), flush=True)
    return 130 if interrupted else 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
