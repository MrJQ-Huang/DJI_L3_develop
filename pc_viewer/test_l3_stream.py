"""Offline receiver tests: generated H.264 and synthetic DJI-layout points."""
from fractions import Fraction
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

import av
import numpy as np

from l3_points import HEADER, POINT, frame_packet, parse_packet
from l3_stream import POINT_DTYPE, StreamReceiver, point_frame


def points(sequence):
    payload = POINT.pack(1.25, -2.5, 3.5, 95, 20, 60, 110)
    return HEADER.pack(0xFFFFFFFF, sequence, 1 << 50, len(payload), 0x12345678, 0x23456789) + payload


def video(colors):
    encoder = av.CodecContext.create("libx264", "w")
    encoder.width, encoder.height = 32, 24
    encoder.pix_fmt = "yuv420p"
    encoder.time_base = Fraction(1, 10)
    encoder.framerate = Fraction(10, 1)
    encoder.options = {"preset": "ultrafast", "tune": "zerolatency",
                       "x264-params": "keyint=1:repeat-headers=1:scenecut=0"}
    packets = []
    for sequence, color in enumerate(colors):
        image = np.empty((24, 32, 3), dtype=np.uint8)
        image[:] = color
        frame = av.VideoFrame.from_ndarray(image, format="bgr24")
        frame.pts = sequence
        packets.extend(encoder.encode(frame))
    packets.extend(encoder.encode(None))
    return b"".join(bytes(packet) for packet in packets)


class Source:
    """One payload per connection; deliberately split both wire formats."""

    def __init__(self, sessions, fragment=17, delay=0, hold_open=False):
        self.sessions = sessions
        self.fragment, self.delay = fragment, delay
        self.hold_open = hold_open
        self.stop = threading.Event()
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(8)
        self.listener.settimeout(0.1)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        for payload in self.sessions:
            while not self.stop.is_set():
                try:
                    peer, _ = self.listener.accept()
                    break
                except socket.timeout:
                    continue
                except OSError:
                    return
            else:
                return
            with peer:
                peer.settimeout(0.5)
                try:
                    for offset in range(0, len(payload), self.fragment):
                        if self.stop.is_set():
                            return
                        peer.sendall(payload[offset:offset + self.fragment])
                        if self.delay:
                            self.stop.wait(self.delay)
                    if self.hold_open:
                        self.stop.wait(10)
                    peer.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
        self.stop.wait(10)

    def close(self):
        self.stop.set()
        self.listener.close()
        self.thread.join(timeout=2)


class SocksSource(Source):
    def __init__(self, payload, reject=False):
        self.reject = reject
        self.requests = []
        super().__init__([payload])

    def run(self):
        while not self.stop.is_set():
            try:
                peer, _ = self.listener.accept()
                break
            except socket.timeout:
                continue
            except OSError:
                return
        else:
            return
        with peer:
            peer.settimeout(1)

            def exact(count):
                data = b""
                while len(data) < count:
                    chunk = peer.recv(count - len(data))
                    if not chunk:
                        raise EOFError
                    data += chunk
                return data

            try:
                if exact(3) != b"\x05\x01\x00":
                    return
                peer.sendall(b"\x05")
                peer.sendall(b"\x00")
                request = exact(4)
                if request[:3] != b"\x05\x01\x00":
                    return
                if request[3] == 1:
                    host = socket.inet_ntoa(exact(4))
                elif request[3] == 3:
                    host = exact(exact(1)[0]).decode("ascii")
                else:
                    return
                self.requests.append((host, struct.unpack("!H", exact(2))[0]))
                response = b"\x05" + bytes([5 if self.reject else 0]) + b"\x00\x01\x7f\x00\x00\x01\x00\x00"
                for byte in response:
                    peer.sendall(bytes([byte]))
                if not self.reject:
                    peer.sendall(self.sessions[0])
            except (OSError, EOFError):
                pass
        self.stop.wait(10)


def wait_for(test, timeout=4):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if test():
            return
        time.sleep(0.01)
    raise AssertionError("receiver did not reach expected state")


class StreamReceiverTest(unittest.TestCase):
    def test_point_fields_and_raw_clock(self):
        frame = point_frame(parse_packet(points(17)))
        self.assertEqual(POINT_DTYPE.itemsize, 16)
        self.assertEqual(frame.sequence, 17)
        self.assertEqual(frame.source_timestamp_raw, 1 << 50)
        self.assertEqual(frame.crc_header_raw, 0x12345678)
        np.testing.assert_array_equal(frame.xyz, [[1.25, -2.5, 3.5]])
        self.assertEqual(frame.points["intensity"].item(), 95)
        self.assertEqual(tuple(frame.points[name].item() for name in ("r", "g", "b")), (20, 60, 110))
        self.assertFalse(frame.points.flags.writeable)
        invalid = bytearray(points(1))
        invalid[HEADER.size:HEADER.size + 4] = np.float32(float("nan")).tobytes()
        with self.assertRaises(ValueError):
            point_frame(parse_packet(bytes(invalid)))

    def test_fragmented_both_streams_decode_bgr_and_keep_one_slot(self):
        colors = [(0, 0, 230)] * 4 + [(225, 0, 0)]
        visible = Source([video(colors)], fragment=7)
        cloud = Source([b"".join(frame_packet(points(i)) for i in range(20))], fragment=3)
        receiver = StreamReceiver("127.0.0.1", cloud.port, visible.port, io_timeout=0.5).start()
        try:
            wait_for(lambda: receiver.stats()["video_frames"] >= len(colors) and
                     receiver.stats()["point_packets"] == 20)
            frame = receiver.latest_video()
            self.assertEqual(frame.image.shape, (24, 32, 3))
            self.assertEqual(frame.image.dtype, np.uint8)
            self.assertFalse(frame.image.flags.writeable)
            self.assertGreater(frame.image[:, :, 0].mean(), 200)
            self.assertLess(frame.image[:, :, 2].mean(), 20)
            self.assertEqual(receiver.latest_points().sequence, 19)
            stats = receiver.stats()
            self.assertEqual((stats["video_slots"], stats["point_slots"]), (1, 1))
            self.assertGreaterEqual(stats["video_superseded"], len(colors) - 1)
            self.assertEqual(stats["point_superseded"], 19)
            self.assertEqual(stats["decode_errors"], 0)
        finally:
            receiver.stop()
            visible.close()
            cloud.close()

    def test_reconnect_resets_video_parser_and_rejects_partial_point_packet(self):
        encoded = video([(0, 230, 0)] * 3)
        visible = Source([encoded[:len(encoded) // 3], encoded], fragment=13)
        framed = frame_packet(points(22))
        cloud = Source([framed[:12], framed], fragment=5)
        receiver = StreamReceiver("127.0.0.1", cloud.port, visible.port,
                                  io_timeout=0.4, reconnect_delay=0.05).start()
        try:
            wait_for(lambda: receiver.stats()["video_frames"] >= 3 and
                     receiver.stats()["point_packets"] == 1)
            self.assertGreaterEqual(receiver.stats()["video_connections"], 2)
            self.assertGreaterEqual(receiver.stats()["point_connections"], 2)
            self.assertEqual(receiver.latest_points().sequence, 22)
            self.assertGreater(receiver.latest_video().image[:, :, 1].mean(), 200)
        finally:
            receiver.stop()
            visible.close()
            cloud.close()

    def test_stop_interrupts_idle_reads_and_reconnect_wait(self):
        cloud = Source([b""], fragment=1, hold_open=True)
        receiver = StreamReceiver("127.0.0.1", cloud.port, streams="points",
                                  io_timeout=0.5, reconnect_delay=10).start()
        try:
            wait_for(lambda: receiver.stats()["point_connections"] >= 1)
            started = time.monotonic()
            receiver.stop()
            self.assertLess(time.monotonic() - started, 1)
            self.assertTrue(all(not t.is_alive() for t in receiver._threads))
            self.assertEqual(receiver.stats()["state"], "stopped")
        finally:
            receiver.stop()
            cloud.close()

    def test_headless_cli_receives_both_streams_and_exits(self):
        visible = Source([video([(0, 0, 230)] * 3)], fragment=11)
        cloud = Source([frame_packet(points(7))], fragment=7)
        command = [sys.executable, str(Path(__file__).with_name("l3_stream.py")),
                   "--host", "127.0.0.1", "--video-port", str(visible.port),
                   "--point-port", str(cloud.port), "--no-display", "--seconds", "0.7"]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            stats = json.loads(result.stdout.splitlines()[-1])
            self.assertEqual(stats["event"], "complete")
            self.assertTrue(stats["received_requested_streams"])
            self.assertEqual(stats["video_frames"], 3)
            self.assertEqual(stats["point_packets"], 1)
        finally:
            visible.close()
            cloud.close()

    def test_explicit_socks5_connect_supports_ip_and_dns_targets(self):
        for host in ("100.92.1.2", "manifold-3.test"):
            with self.subTest(host=host):
                proxy = SocksSource(frame_packet(points(71)))
                receiver = StreamReceiver(host, streams="points", io_timeout=0.4,
                                          socks5=f"127.0.0.1:{proxy.port}").start()
                try:
                    wait_for(lambda: receiver.stats()["point_packets"] == 1)
                    self.assertEqual(proxy.requests, [(host, 9101)])
                    self.assertEqual(receiver.latest_points().sequence, 71)
                finally:
                    receiver.stop()
                    proxy.close()

    def test_socks5_rejection_does_not_report_a_connected_stream(self):
        proxy = SocksSource(frame_packet(points(71)), reject=True)
        receiver = StreamReceiver("100.92.1.2", streams="points", io_timeout=0.4,
                                  socks5=f"127.0.0.1:{proxy.port}").start()
        try:
            wait_for(lambda: receiver.stats()["connection_errors"] >= 1)
            self.assertEqual(receiver.stats()["point_connections"], 0)
            self.assertEqual(receiver.stats()["point_packets"], 0)
            self.assertEqual(receiver.stats()["point_error"], "OSError")
        finally:
            receiver.stop()
            proxy.close()

    def test_socks5_handshake_has_a_deadline(self):
        proxy = Source([b""], hold_open=True)
        receiver = StreamReceiver("100.92.1.2", streams="points", io_timeout=0.15,
                                  socks5=f"127.0.0.1:{proxy.port}").start()
        try:
            wait_for(lambda: receiver.stats()["connection_errors"] >= 1, timeout=1)
            self.assertEqual(receiver.stats()["point_connections"], 0)
            started = time.monotonic()
            receiver.stop()
            self.assertLess(time.monotonic() - started, 0.5)
        finally:
            receiver.stop()
            proxy.close()


if __name__ == "__main__":
    unittest.main()
