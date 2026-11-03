"""Receive DJI L3 camera JPEG frames; the decoded BGR array is Frame.image."""
from __future__ import annotations

import argparse
import json
import signal
import socket
import struct
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


HEADER = struct.Struct(">4sIIIQQ")
MAGIC = b"L3F1"
MAX_PAYLOAD = 8 * 1024 * 1024
MAX_DIMENSION = 8192
MAX_PIXELS = 32 * 1024 * 1024
WINDOW = "DJI L3 Camera"


@dataclass(frozen=True)
class Frame:
    # Actual decoded pixels, before any display overlays. Shape: (height,width,3).
    image: np.ndarray
    sequence: int
    remote_callback_monotonic_ns: int
    received_monotonic_ns: int


def read_exact(sock: socket.socket, length: int, stop: threading.Event) -> bytes:
    """TCP may split any header/payload. Timeouts only check for cancellation."""
    data = bytearray(length)
    view = memoryview(data)
    offset = 0
    while offset < length:
        if stop.is_set():
            raise InterruptedError("receiver stopped")
        try:
            count = sock.recv_into(view[offset:])
        except socket.timeout:
            continue
        if count == 0:
            raise EOFError("peer disconnected")
        offset += count
    return bytes(data)


def jpeg_dimensions(data: bytes) -> tuple[int, int]:
    """Inspect SOF before OpenCV allocates the decoded image."""
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        raise ValueError("payload is not JPEG")
    offset = 2
    sof = {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
           0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}
    while offset < len(data):
        if data[offset] != 0xFF:
            raise ValueError("invalid JPEG marker")
        while offset < len(data) and data[offset] == 0xFF:
            offset += 1
        if offset >= len(data):
            break
        marker = data[offset]
        offset += 1
        if marker in (0xD9, 0xDA):
            break
        if marker == 0x01 or 0xD0 <= marker <= 0xD7:
            continue
        if offset + 2 > len(data):
            break
        segment_length = int.from_bytes(data[offset:offset + 2], "big")
        if segment_length < 2 or offset + segment_length > len(data):
            raise ValueError("truncated JPEG segment")
        if marker in sof:
            if segment_length < 8:
                raise ValueError("invalid JPEG SOF")
            height = int.from_bytes(data[offset + 3:offset + 5], "big")
            width = int.from_bytes(data[offset + 5:offset + 7], "big")
            return width, height
        offset += segment_length
    raise ValueError("JPEG has no supported dimension marker")


class Receiver:
    """Background TCP receiver, retaining only the newest valid Frame.

    Call latest_frame() from an OpenCV/AI consumer to obtain the BGR ndarray.
    Treat Frame.image as read-only, or copy it before modifying it.
    Remote monotonic timestamps describe RC callback time, not exposure time;
    they must not be subtracted from the PC clock to claim one-way latency.
    """

    def __init__(self, host="127.0.0.1", port=9000, max_frames=0, save_first=None):
        self.host, self.port = host, port
        self.max_frames = max_frames
        self.save_first = Path(save_first) if save_first else None
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self._lock = threading.Lock()
        self._latest: Frame | None = None
        self._last_taken_sequence: int | None = None
        self._listener: socket.socket | None = None
        self._connection: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self.fatal_error: str | None = None
        self.started = time.monotonic()
        self._stats = dict(connections=0, connected=False, frames_received=0,
                           protocol_errors=0, superseded_frames=0,
                           first_frame_saved=False)

    def start(self):
        self._thread = threading.Thread(target=self._run, name="l3-receiver", daemon=True)
        self._thread.start()
        if not self.ready.wait(5):
            self.stop()
            raise RuntimeError("listener did not start within 5 seconds")
        if self.fatal_error:
            raise RuntimeError(self.fatal_error)

    def stop(self):
        self.stop_event.set()
        for sock in (self._connection, self._listener):
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=2)

    def latest_frame(self) -> Frame | None:
        with self._lock:
            frame = self._latest
            if frame:
                self._last_taken_sequence = frame.sequence
            return frame

    def stats(self):
        with self._lock:
            result = dict(self._stats)
            frame = self._latest
        elapsed = max(time.monotonic() - self.started, 1e-6)
        result.update(runtime_seconds=round(elapsed, 3),
                      average_receive_fps=round(result["frames_received"] / elapsed, 3),
                      listen_host=self.host, listen_port=self.port)
        if frame:
            height, width = frame.image.shape[:2]
            result.update(width=width, height=height, sequence=frame.sequence,
                          remote_callback_monotonic_ns=frame.remote_callback_monotonic_ns,
                          last_frame_age_seconds=round((time.monotonic_ns() - frame.received_monotonic_ns) / 1e9, 3))
        if self.fatal_error:
            result["fatal_error"] = self.fatal_error
        return result

    def _receive_frame(self, connection):
        magic, length, width, height, sequence, callback_ns = HEADER.unpack(
            read_exact(connection, HEADER.size, self.stop_event))
        if magic != MAGIC:
            raise ValueError("invalid protocol magic")
        if not 0 < length <= MAX_PAYLOAD:
            raise ValueError("JPEG payload length outside 1..8 MiB")
        if not (0 < width <= MAX_DIMENSION and 0 < height <= MAX_DIMENSION
                and width * height <= MAX_PIXELS):
            raise ValueError("invalid or excessive frame dimensions")
        payload = read_exact(connection, length, self.stop_event)
        if jpeg_dimensions(payload) != (width, height):
            raise ValueError("JPEG dimensions do not match protocol header")
        image = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
        if image is None or image.shape != (height, width, 3):
            raise ValueError("JPEG decoder returned invalid image")
        frame = Frame(image, sequence, callback_ns, time.monotonic_ns())
        with self._lock:
            if self._latest and self._latest.sequence != self._last_taken_sequence:
                self._stats["superseded_frames"] += 1
            self._latest = frame
            self._stats["frames_received"] += 1
            count = self._stats["frames_received"]
        if self.save_first and count == 1:
            self.save_first.parent.mkdir(parents=True, exist_ok=True)
            # OpenCV's Windows file API can reject Chinese paths. Encode in
            # memory and let Python's Unicode-aware file API write the bytes.
            encoded_ok, encoded = cv2.imencode(self.save_first.suffix or ".jpg", image)
            if not encoded_ok:
                raise RuntimeError("could not save first frame")
            self.save_first.write_bytes(encoded.tobytes())
            with self._lock:
                self._stats["first_frame_saved"] = True
        if self.max_frames and count >= self.max_frames:
            self.stop_event.set()

    def _run(self):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
                self._listener = listener
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind((self.host, self.port))
                self.port = listener.getsockname()[1]
                listener.listen(1)
                listener.settimeout(0.25)
                self.ready.set()
                while not self.stop_event.is_set():
                    try:
                        connection, _ = listener.accept()
                    except socket.timeout:
                        continue
                    except OSError:
                        if self.stop_event.is_set():
                            break
                        raise
                    self._connection = connection
                    with connection:
                        connection.settimeout(0.25)
                        with self._lock:
                            self._stats["connections"] += 1
                            self._stats["connected"] = True
                        try:
                            while not self.stop_event.is_set():
                                self._receive_frame(connection)
                        except (EOFError, ConnectionError, InterruptedError):
                            pass
                        except (ValueError, struct.error, cv2.error) as error:
                            with self._lock:
                                self._stats["protocol_errors"] += 1
                            print(json.dumps({"event": "invalid_frame", "error": str(error)}), flush=True)
                        except OSError:
                            if not self.stop_event.is_set():
                                print(json.dumps({"event": "connection_lost"}), flush=True)
                        finally:
                            self._connection = None
                            with self._lock:
                                self._stats["connected"] = False
        except Exception as error:
            self.fatal_error = f"{type(error).__name__}: {error}"
            self.stop_event.set()
        finally:
            self.ready.set()


def emit_stats(receiver, stats_path, event="stats"):
    result = dict(event=event, **receiver.stats())
    print(json.dumps(result, ensure_ascii=False), flush=True)
    if stats_path:
        path = Path(stats_path)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--no-display", action="store_true")
    parser.add_argument("--max-frames", type=int, default=0, help="exit after N valid decoded frames; 0 means unlimited")
    parser.add_argument("--save-first", metavar="IMAGE_PATH", help="save the first decoded frame only")
    parser.add_argument("--stats-json", metavar="JSON_PATH", help="update this file once per second and at exit")
    args = parser.parse_args()
    if args.max_frames < 0 or not 0 <= args.port <= 65535:
        parser.error("max-frames must be nonnegative; port must be 0..65535")
    receiver = Receiver(args.host, args.port, args.max_frames, args.save_first)
    signal.signal(signal.SIGINT, lambda *_: receiver.stop_event.set())
    signal.signal(signal.SIGTERM, lambda *_: receiver.stop_event.set())
    latest_rendered_ns = None
    window_created = False
    last_stats = time.monotonic()
    fps_start = last_stats
    fps_count = 0
    display_fps = 0.0
    try:
        receiver.start()
        emit_stats(receiver, args.stats_json, "listening")
        if not args.no_display:
            cv2.namedWindow(WINDOW, cv2.WINDOW_NORMAL)
            window_created = True
            canvas = np.zeros((540, 960, 3), np.uint8)
            cv2.putText(canvas, "Waiting for DJI L3 Camera...", (35, 70),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2)
            cv2.imshow(WINDOW, canvas)
        while not receiver.stop_event.is_set():
            frame = receiver.latest_frame()
            if frame is not None and frame.received_monotonic_ns != latest_rendered_ns:
                latest_rendered_ns = frame.received_monotonic_ns
                fps_count += 1
                now = time.monotonic()
                if now - fps_start >= 1:
                    display_fps = fps_count / (now - fps_start)
                    fps_count, fps_start = 0, now
                if not args.no_display:
                    canvas = frame.image.copy()  # keep programmatic BGR pixels unmodified
                    height, width = canvas.shape[:2]
                    label = f"{width}x{height}  {display_fps:.1f} fps  seq={frame.sequence}"
                    cv2.rectangle(canvas, (0, 0), (min(width, 650), 36), (0, 0, 0), -1)
                    cv2.putText(canvas, label, (8, 25), cv2.FONT_HERSHEY_SIMPLEX,
                                0.65, (0, 255, 0), 1, cv2.LINE_AA)
                    cv2.imshow(WINDOW, canvas)
            if args.no_display:
                receiver.stop_event.wait(0.02)
            else:
                if cv2.waitKey(15) & 0xFF == 27:
                    break
                if cv2.getWindowProperty(WINDOW, cv2.WND_PROP_VISIBLE) < 1:
                    break
            now = time.monotonic()
            if now - last_stats >= 1:
                emit_stats(receiver, args.stats_json)
                last_stats = now
    except (RuntimeError, OSError, cv2.error) as error:
        print(json.dumps({"event": "error", "error": str(error)}), flush=True)
        return_code = 1
    else:
        return_code = 1 if receiver.fatal_error else 0
    finally:
        receiver.stop()
        if window_created:
            cv2.destroyAllWindows()
        emit_stats(receiver, args.stats_json, "stopped")
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
