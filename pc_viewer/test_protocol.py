"""Local synthetic JPEG integration checks; no DJI hardware/ADB is used."""
import json
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import cv2
import numpy as np

from receiver import HEADER, MAGIC, MAX_PAYLOAD, Receiver


def wait_for(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError("timed out waiting for condition")


def packet(jpeg, sequence, width=160, height=120):
    return HEADER.pack(MAGIC, len(jpeg), width, height, sequence,
                       123456789000 + sequence) + jpeg


def send(port, data, fragmented=False):
    with socket.create_connection(("127.0.0.1", port), timeout=2) as client:
        if fragmented:
            # Splits inside integer fields, the header/payload boundary and JPEG.
            for start in range(0, len(data), 7):
                client.sendall(data[start:start + 7])
        else:
            client.sendall(data)


def main():
    image = np.zeros((120, 160, 3), np.uint8)
    image[:] = (30, 100, 220)
    ok, encoded = cv2.imencode(".jpg", image)
    assert ok
    jpeg = encoded.tobytes()

    receiver = Receiver(port=0, max_frames=3)
    receiver.start()
    try:
        send(receiver.port, HEADER.pack(MAGIC, MAX_PAYLOAD + 1, 160, 120, 0, 0))
        wait_for(lambda: receiver.stats()["protocol_errors"] == 1)
        send(receiver.port, packet(jpeg, 0, width=161))
        wait_for(lambda: receiver.stats()["protocol_errors"] == 2)
        send(receiver.port, packet(jpeg, 1), fragmented=True)
        wait_for(lambda: receiver.stats()["frames_received"] == 1)
        frame = receiver.latest_frame()
        assert frame.image.shape == (120, 160, 3)
        assert frame.sequence == 1
        assert frame.remote_callback_monotonic_ns == 123456789001
        assert np.max(np.abs(frame.image.mean(axis=(0, 1)) - (30, 100, 220))) <= 3
        # Drop a connection mid-payload. A following client must still work.
        send(receiver.port, packet(jpeg, 99)[:48])
        send(receiver.port, packet(jpeg, 2) + packet(jpeg, 3))
        wait_for(lambda: receiver.stop_event.is_set())
        stats = receiver.stats()
        assert stats["frames_received"] == 3, stats
        assert stats["protocol_errors"] == 2, stats
        assert stats["sequence"] == 3, stats
        assert stats["connections"] == 5, stats
        assert stats["superseded_frames"] == 1, stats
        print("PASS: fragmented TCP, JPEG BGR pixels, uint64 timestamp, reconnect, partial disconnect, dimension and size rejection, bounded latest-frame slot")
    finally:
        receiver.stop()

    # Verify the actual CLI exits at its limit and emits parseable final stats.
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="protocol_test_", dir=Path(__file__).parent) as folder:
        stats_path = Path(folder) / "stats.json"
        image_path = Path(folder) / "first.jpg"
        command = [sys.executable, str(Path(__file__).with_name("receiver.py")),
                   "--port", str(port), "--no-display", "--max-frames", "1",
                   "--save-first", str(image_path), "--stats-json", str(stats_path)]
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8")
        try:
            wait_for(lambda: stats_path.exists())
            send(port, packet(jpeg, 42), fragmented=True)
            stdout, stderr = process.communicate(timeout=10)
            assert process.returncode == 0, (stdout, stderr)
            stats = json.loads(stats_path.read_text(encoding="utf-8"))
            assert stats["event"] == "stopped" and stats["frames_received"] == 1, stats
            assert stats["sequence"] == 42 and stats["first_frame_saved"], stats
            saved = cv2.imdecode(np.frombuffer(image_path.read_bytes(), np.uint8), cv2.IMREAD_COLOR)
            assert saved.shape == (120, 160, 3)
            for line in stdout.splitlines():
                json.loads(line)
            print("PASS: headless CLI, --max-frames, first-frame save, atomic JSON status, clean exit")
        finally:
            if process.poll() is None:
                process.terminate()
                process.communicate(timeout=5)

    receiver = Receiver(port=0)
    receiver.start()
    start = time.monotonic()
    receiver.stop()
    assert time.monotonic() - start < 2
    print("PASS: idle listener stops within 2 seconds")


if __name__ == "__main__":
    main()
