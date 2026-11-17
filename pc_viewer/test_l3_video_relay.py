"""No SDK/hardware: pipe synthetic video through the onboard relay and reconnect."""
import json
from pathlib import Path
import selectors
import subprocess
import tempfile
import threading
import time
import unittest

import numpy as np

from l3_stream import StreamReceiver
from test_l3_stream import video


class VideoRelayTest(unittest.TestCase):
    def test_pipe_decode_reconnect_and_idle_stop(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as work:
            binary = str(Path(work) / "relay")
            subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                            str(root / "onboard/l3_video_relay.c"),
                            str(root / "onboard/l3_stream.c"), "-o", binary], check=True, timeout=20)
            fixture = video([(20, 80, 180)] * 3)
            done = threading.Event()
            readers = []
            with subprocess.Popen([binary, "0", "10"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE) as relay:
                writer = None
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(relay.stdout, selectors.EVENT_READ)
                        self.assertTrue(selector.select(2))
                    ready = json.loads(relay.stdout.readline())
                    self.assertEqual(ready["event"], "video_relay_ready")

                    def feed():
                        try:
                            while not done.is_set():
                                relay.stdin.write(fixture)
                                relay.stdin.flush()
                                done.wait(0.1)
                        except (BrokenPipeError, ValueError):
                            pass

                    writer = threading.Thread(target=feed)
                    writer.start()
                    for _ in range(2):
                        receiver = StreamReceiver("127.0.0.1", video_port=ready["video_port"],
                                                  streams="video", io_timeout=1).start()
                        readers.append(receiver)
                        deadline = time.monotonic() + 3
                        while receiver.stats()["video_frames"] < 3 and time.monotonic() < deadline:
                            time.sleep(0.02)
                        self.assertGreaterEqual(receiver.stats()["video_frames"], 3)
                        np.testing.assert_allclose(receiver.latest_video().image.mean(axis=(0, 1)),
                                                   [20, 80, 180], atol=15)
                        self.assertEqual(receiver.stats()["decode_errors"], 0)
                        self.assertEqual(receiver.stats()["point_connections"], 0)
                        receiver.stop()
                    done.set()
                    writer.join(2)
                    self.assertFalse(writer.is_alive())
                    # Keep stdin open but idle. SIGTERM must interrupt the poll.
                    relay.terminate()
                    self.assertEqual(relay.wait(timeout=2), 0)
                    result = json.loads(relay.stdout.readline())
                    self.assertGreaterEqual(result["connections"], 2)
                    self.assertGreater(result["video_bytes"], 0)
                finally:
                    done.set()
                    for receiver in readers:
                        receiver.stop()
                    if relay.poll() is None:
                        relay.kill()
                    relay.wait(timeout=2)
                    if writer is not None:
                        writer.join(2)


if __name__ == "__main__":
    unittest.main()
