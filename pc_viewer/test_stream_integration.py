"""SDK-free end-to-end test: real generated H.264, C fan-out, Python decoding."""
from pathlib import Path
import json
import selectors
import subprocess
import tempfile
import time
import unittest

import numpy as np

from l3_stream import StreamReceiver
from test_l3_stream import video


class StreamIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix="l3-stream-integration-")
        root = Path(__file__).resolve().parents[1]
        cls.binary = Path(cls.work.name) / "stream_replay"
        command = ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                   "-I", str(root / "onboard"), str(root / "onboard/l3_stream.c"),
                   str(root / "onboard/tests/stream_replay.c"), "-o", str(cls.binary)]
        subprocess.run(command, check=True, capture_output=True, text=True, timeout=20)
        cls.fixture = Path(cls.work.name) / "generated.h264"
        cls.fixture.write_bytes(video([(8, 30, 220)] * 6))

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def wait_for(self, predicate, source, receivers, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if source.poll() is not None:
                self.fail("C replay source exited before receive condition")
            time.sleep(0.01)
        self.fail("receive condition timed out: " + repr([receiver.stats() for receiver in receivers]))

    def test_two_receivers_decode_and_one_can_rejoin(self):
        receivers = []
        with subprocess.Popen([str(self.binary), str(self.fixture), "8"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as source:
            try:
                with selectors.DefaultSelector() as ready:
                    ready.register(source.stdout, selectors.EVENT_READ)
                    self.assertTrue(ready.select(3), "C source did not announce its ports")
                ports = json.loads(source.stdout.readline())
                self.assertEqual(ports["event"], "synthetic_replay")

                def connect():
                    receiver = StreamReceiver("127.0.0.1", ports["point_port"],
                                              ports["video_port"], io_timeout=1).start()
                    receivers.append(receiver)
                    return receiver

                first, second = connect(), connect()
                self.wait_for(lambda: all(r.stats()["video_frames"] >= 3 and
                                         r.stats()["point_packets"] >= 3 for r in (first, second)),
                              source, receivers)
                for receiver in (first, second):
                    frame = receiver.latest_video()
                    self.assertEqual(frame.image.shape, (24, 32, 3))
                    np.testing.assert_allclose(frame.image.mean(axis=(0, 1)), [8, 30, 220], atol=15)
                    cloud = receiver.latest_points()
                    np.testing.assert_array_equal(cloud.xyz, [[1.25, -2.5, 3.5]])
                    self.assertEqual(cloud.points["intensity"].item(), 95)
                    self.assertEqual(tuple(cloud.points[name].item() for name in ("r", "g", "b")),
                                     (20, 60, 110))
                    self.assertEqual(receiver.stats()["decode_errors"], 0)

                # Disconnect one subscriber while the other keeps receiving.
                first.stop()
                before = second.stats()
                self.wait_for(lambda: second.stats()["point_packets"] >= before["point_packets"] + 3 and
                              second.stats()["video_frames"] >= before["video_frames"] + 3,
                              source, receivers)
                replacement = connect()
                self.wait_for(lambda: replacement.stats()["point_packets"] >= 2 and
                              replacement.stats()["video_frames"] >= 3, source, receivers)
                self.assertEqual(replacement.stats()["decode_errors"], 0)
                self.assertEqual(second.stats()["point_errors"], 0)
            finally:
                for receiver in receivers:
                    receiver.stop()
                if source.poll() is None:
                    source.terminate()
                source.communicate(timeout=3)
            self.assertEqual(source.returncode, 0)


if __name__ == "__main__":
    unittest.main()
