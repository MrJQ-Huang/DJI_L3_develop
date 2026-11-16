import json
import socket
import subprocess
import sys
import unittest
from pathlib import Path

from l3_points import HEADER, POINT, frame_packet, parse_packet, summarize


def sample(sequence=17):
    points = POINT.pack(1.25, -2.5, 3.5, 95, 20, 60, 110) + POINT.pack(-4, 0, 9, 10, 1, 2, 3)
    return HEADER.pack(0xFFFFFFFF, sequence, 1 << 50, len(points), 0x12345678, 0x23456789) + points


class PointPacketTest(unittest.TestCase):
    def test_signed_xyz_intensity_and_rgb(self):
        packet = parse_packet(sample())
        self.assertEqual(list(packet.iter_points())[0], (1.25, -2.5, 3.5, 95, 20, 60, 110))
        self.assertEqual(packet.source_timestamp_raw, 1 << 50)
        self.assertEqual(summarize(packet)["xyz_min_raw"], [-4, -2.5, 3.5])

    def test_corrupt_flag_length_and_nonfinite_coordinate(self):
        raw = sample()
        for invalid in [raw[:-1], raw + b"\x00", b"\x00" + raw[1:]]:
            with self.assertRaises(ValueError):
                parse_packet(invalid)
        with self.assertRaises(ValueError):
            summarize(parse_packet(raw[:HEADER.size] + POINT.pack(float("nan"), 0, 0, 0, 0, 0, 0) * 2))

    def test_cli_receives_two_packets_over_real_tcp(self):
        command = [sys.executable, str(Path(__file__).with_name("l3_points.py")), "listen",
                   "--port", "0", "--count", "2", "--timeout", "5"]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as proc:
            try:
                ready = json.loads(proc.stdout.readline())
                with socket.create_connection(tuple(ready["address"]), timeout=2) as peer:
                    framed = frame_packet(sample(17)) + frame_packet(sample(18))
                    for offset in range(0, len(framed), 7):
                        peer.sendall(framed[offset:offset + 7])
                out, err = proc.communicate(timeout=6)
                self.assertEqual(proc.returncode, 0, err)
                stats = [json.loads(line) for line in out.splitlines()]
                self.assertEqual([record["sequence"] for record in stats[:-1]], [17, 18])
                self.assertEqual(stats[-1], {"event": "complete", "packets": 2, "points": 4})
            finally:
                if proc.poll() is None:
                    proc.kill()


if __name__ == "__main__":
    unittest.main()
