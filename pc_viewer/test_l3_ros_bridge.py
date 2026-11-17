"""Synthetic TCP -> receiver -> actual local ROS 2 publication/subscription.

Run after sourcing /opt/ros/humble/setup.bash, with the repository .venv.
No aircraft, real imagery, SDK calls, or external ROS participants are used.
"""
from dataclasses import replace
import json
import os
from pathlib import Path
import subprocess
import time
import unittest

os.environ["ROS_LOCALHOST_ONLY"] = "1"

import numpy as np
from rclpy.context import Context
from rclpy.executors import SingleThreadedExecutor
from rclpy.node import Node
from sensor_msgs.msg import Image, PointCloud2

from l3_points import frame_packet, parse_packet
from l3_ros_bridge import L3RosBridge, LIVE_QOS, ROS_POINT_DTYPE, image_message, point_message
from l3_stream import StreamReceiver, VideoFrame, point_frame
from test_l3_stream import Source, points, video


class MessageTest(unittest.TestCase):
    def test_point_fields_scale_color_and_receive_clock(self):
        source = point_frame(parse_packet(points(7)))
        stamp = 1_700_000_123_456_789_012
        source = replace(source, received_system_ns=stamp)
        original = source.points.tobytes()
        message = point_message(source, scale=0.01, frame_id="l3_local")
        self.assertEqual(message.header.frame_id, "l3_local")
        self.assertEqual(message.header.stamp.sec, 1_700_000_123)
        self.assertEqual(message.header.stamp.nanosec, 456_789_012)
        self.assertEqual((message.height, message.width, message.point_step, message.row_step), (1, 1, 20, 20))
        self.assertEqual([field.offset for field in message.fields], [0, 4, 8, 12, 16])
        decoded = np.frombuffer(message.data, dtype=ROS_POINT_DTYPE)
        self.assertAlmostEqual(float(decoded["x"][0]), 0.0125)
        self.assertAlmostEqual(float(decoded["y"][0]), -0.025)
        self.assertAlmostEqual(float(decoded["z"][0]), 0.035)
        self.assertEqual(float(decoded["intensity"][0]), 95)
        self.assertEqual(int(decoded["rgb"][0]), 20 << 16 | 60 << 8 | 110)
        self.assertEqual(source.points.tobytes(), original)
        self.assertEqual(source.source_timestamp_raw, 1 << 50)
        self.assertFalse(message.is_bigendian)
        self.assertTrue(message.is_dense)

    def test_scaling_rejects_nonfinite_output(self):
        frame = point_frame(parse_packet(points(1)))
        for scale in (0, -1, float("nan"), float("inf"), 1e38):
            with self.subTest(scale=scale), self.assertRaises(ValueError):
                point_message(frame, scale=scale, frame_id="l3_local")

    def test_bgr_image_noncontiguous_rows_and_local_clock(self):
        image = np.arange(4 * 6 * 3, dtype=np.uint8).reshape(4, 6, 3)[:, ::2]
        frame = VideoFrame(image, 1, 999, 1_700_000_000_000_000_001)
        message = image_message(frame)
        self.assertEqual((message.height, message.width, message.step), (4, 3, 9))
        self.assertEqual(message.encoding, "bgr8")
        self.assertEqual(message.header.frame_id, "l3_camera_optical")
        self.assertEqual(message.header.stamp.sec, 1_700_000_000)
        self.assertEqual(message.header.stamp.nanosec, 1)
        np.testing.assert_array_equal(np.frombuffer(message.data, np.uint8).reshape(4, 3, 3), image)


class RosIntegrationTest(unittest.TestCase):
    def setUp(self):
        self.context = Context()
        self.context.init(args=[], domain_id=160 + os.getpid() % 40)
        self.executor = SingleThreadedExecutor(context=self.context)
        self.subscriber = Node("l3_synthetic_assertions", context=self.context)
        self.executor.add_node(self.subscriber)
        self.bridge = self.receiver = None
        self.sources = []

    def tearDown(self):
        if self.receiver is not None:
            self.receiver.stop()
        for source in self.sources:
            source.close()
        self.executor.shutdown(timeout_sec=1)
        if self.bridge is not None:
            self.bridge.destroy_node()
        self.subscriber.destroy_node()
        self.context.shutdown()

    def spin_until(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.executor.spin_once(timeout_sec=0.02)
            if condition():
                return
        self.fail("local ROS condition timed out")

    def spin_for(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.executor.spin_once(timeout_sec=0.02)

    def start_bridge(self, streams, cloud, visible=None):
        self.receiver = StreamReceiver("127.0.0.1", cloud.port,
                                       visible.port if visible else 9102, streams,
                                       io_timeout=10, reconnect_delay=0.4)
        self.bridge = L3RosBridge(self.receiver, raw_coordinates=True, context=self.context)
        self.executor.add_node(self.bridge)
        self.spin_until(lambda: self.bridge.point_publisher.get_subscription_count() == 1 and
                         (self.bridge.image_publisher is None or
                          self.bridge.image_publisher.get_subscription_count() == 1))
        self.receiver.start()

    def test_points_only_actual_tcp_ros_and_no_duplicate_old_packet(self):
        received = []
        self.subscriber.create_subscription(PointCloud2, "/l3/points", received.append, LIVE_QOS)
        cloud = Source([frame_packet(points(13))], fragment=3, hold_open=True)
        self.sources.append(cloud)
        before = time.time_ns()
        self.start_bridge("points", cloud)
        self.spin_until(lambda: len(received) == 1)
        message = received[0]
        self.assertEqual(message.header.frame_id, "l3_raw")
        stamp = message.header.stamp.sec * 1_000_000_000 + message.header.stamp.nanosec
        self.assertLessEqual(before, stamp)
        self.assertLessEqual(stamp, time.time_ns())
        decoded = np.frombuffer(message.data, dtype=ROS_POINT_DTYPE)
        self.assertEqual(tuple(float(decoded[name][0]) for name in ("x", "y", "z")), (1.25, -2.5, 3.5))
        self.assertIsNone(self.bridge.image_publisher)
        self.assertEqual(self.receiver.stats()["video_connections"], 0)
        self.spin_for(0.3)
        self.assertEqual(len(received), 1, "idle source must not repeatedly publish an old point packet")
        self.assertEqual(self.bridge.point_messages, 1)

    def test_both_real_ros_types_and_restart_repeated_source_sequence(self):
        clouds, images = [], []
        self.subscriber.create_subscription(PointCloud2, "/l3/points", clouds.append, LIVE_QOS)
        self.subscriber.create_subscription(Image, "/l3/camera/image_raw", images.append, LIVE_QOS)
        # A sender restart may reuse the sequence; both newly received packets count.
        cloud = Source([frame_packet(points(7)), frame_packet(points(7))], fragment=5)
        visible = Source([video([(8, 30, 220)] * 3)], fragment=11, hold_open=True)
        self.sources.extend([cloud, visible])
        self.start_bridge("both", cloud, visible)
        self.spin_until(lambda: len(clouds) == 2 and len(images) >= 1)
        self.assertEqual(self.receiver.stats()["point_packets"], 2)
        self.assertGreaterEqual(self.receiver.stats()["point_connections"], 2)
        frame = images[-1]
        decoded = np.frombuffer(frame.data, np.uint8).reshape(frame.height, frame.width, 3)
        self.assertEqual(decoded.shape, (24, 32, 3))
        np.testing.assert_allclose(decoded.mean(axis=(0, 1)), [8, 30, 220], atol=15)
        self.assertEqual(frame.encoding, "bgr8")
        self.assertEqual(self.bridge.publish_errors, 0)
        counts = len(clouds), len(images)
        self.spin_for(0.3)
        self.assertEqual((len(clouds), len(images)), counts)


class LauncherTest(unittest.TestCase):
    def test_local_only_headless_bounded_points_default(self):
        cloud = Source([frame_packet(points(1))], fragment=3, hold_open=True)
        root = Path(__file__).resolve().parents[1]
        command = ["bash", str(root / "scripts/start_l3_rviz.sh"), "--no-rviz", "--host", "127.0.0.1",
                   "--point-port", str(cloud.port), "--raw-coordinates", "--seconds", "0.6"]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=8,
                                    env={**os.environ, "ROS_DOMAIN_ID": "159"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            summary = next(json.loads(line) for line in result.stdout.splitlines()
                           if line.startswith('{"event": "ros_complete"'))
            self.assertTrue(summary["published_requested_streams"])
            self.assertEqual(summary["ros_point_messages"], 1)
            self.assertEqual(summary["video_connections"], 0)
            self.assertEqual(summary["point_frame_id"], "l3_raw")
        finally:
            cloud.close()


if __name__ == "__main__":
    unittest.main()
