"""Publish received L3 streams to local ROS 2; never control the aircraft.

Point coordinates require an explicit choice: raw diagnostic values (l3_raw),
or a user-confirmed source-unit-to-metre scale (l3_local). Axes stay unchanged.
ROS stamps are local system receive/decode times, not sensor acquisition times.
"""
from __future__ import annotations

import argparse
import json
import math
import time

import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import Image, PointCloud2, PointField

try:
    from .l3_stream import PointFrame, StreamReceiver, VideoFrame
except ImportError:
    from l3_stream import PointFrame, StreamReceiver, VideoFrame


ROS_POINT_DTYPE = np.dtype([(name, "<f4") for name in ("x", "y", "z", "intensity")] +
                           [("rgb", "<u4")])
LIVE_QOS = QoSProfile(depth=1, reliability=ReliabilityPolicy.BEST_EFFORT)


def set_header(message, received_system_ns: int, frame_id: str) -> None:
    message.header.stamp.sec, message.header.stamp.nanosec = divmod(received_system_ns, 1_000_000_000)
    message.header.frame_id = frame_id


def image_message(frame: VideoFrame) -> Image:
    image = frame.image
    if image.dtype != np.uint8 or image.ndim != 3 or image.shape[2] != 3:
        raise ValueError("expected uint8 H x W x 3 BGR image")
    message = Image()
    set_header(message, frame.received_system_ns, "l3_camera_optical")
    message.height, message.width = image.shape[:2]
    message.encoding = "bgr8"
    message.is_bigendian = 0
    message.step = message.width * 3
    message.data = image.tobytes(order="C")
    return message


def point_message(frame: PointFrame, *, scale: float, frame_id: str) -> PointCloud2:
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError("coordinate scale must be finite and positive")
    records = np.empty(len(frame.points), dtype=ROS_POINT_DTYPE)
    with np.errstate(over="ignore", invalid="ignore"):
        for axis in ("x", "y", "z"):
            records[axis] = frame.points[axis].astype(np.float64) * scale
            if not np.isfinite(records[axis]).all():
                raise ValueError("scaled XYZ exceeds finite FLOAT32 range")
    records["intensity"] = frame.points["intensity"]
    records["rgb"] = (frame.points["r"].astype(np.uint32) << 16 |
                      frame.points["g"].astype(np.uint32) << 8 |
                      frame.points["b"].astype(np.uint32))
    message = PointCloud2()
    set_header(message, frame.received_system_ns, frame_id)
    message.height, message.width = 1, len(records)
    message.fields = [PointField(name=name, offset=offset * 4,
                                datatype=PointField.UINT32 if name == "rgb" else PointField.FLOAT32,
                                count=1)
                      for offset, name in enumerate(ROS_POINT_DTYPE.names)]
    message.is_bigendian = False
    message.point_step = ROS_POINT_DTYPE.itemsize
    message.row_step = message.point_step * message.width
    message.is_dense = True
    message.data = records.tobytes()
    return message


class L3RosBridge(Node):
    def __init__(self, receiver: StreamReceiver, *, raw_coordinates: bool = False,
                 xyz_scale_to_meters: float | None = None, context=None):
        has_points = receiver.streams != "video"
        if raw_coordinates and xyz_scale_to_meters is not None:
            raise ValueError("choose raw diagnostic coordinates or a confirmed unit scale")
        if has_points and not raw_coordinates and xyz_scale_to_meters is None:
            raise ValueError("point coordinates require --raw-coordinates or --xyz-scale-to-meters")
        if xyz_scale_to_meters is not None and (not math.isfinite(xyz_scale_to_meters) or
                                               xyz_scale_to_meters <= 0):
            raise ValueError("coordinate scale must be finite and positive")
        super().__init__("l3_stream_bridge", context=context)
        self.receiver = receiver
        self.scale = 1.0 if raw_coordinates or not has_points else xyz_scale_to_meters
        self.point_frame_id = "l3_raw" if raw_coordinates else "l3_local"
        self.point_publisher = self.create_publisher(PointCloud2, "/l3/points", LIVE_QOS) if has_points else None
        self.image_publisher = (self.create_publisher(Image, "/l3/camera/image_raw", LIVE_QOS)
                                if receiver.streams != "points" else None)
        self._last_points = self._last_video = None
        self.point_messages = self.image_messages = self.publish_errors = 0
        self.create_timer(0.02, self.publish_latest)
        if has_points and raw_coordinates:
            self.get_logger().warning("l3_raw: diagnostic source coordinates; units/origin/axes are unverified. "
                                      "RViz distances are raw values, not confirmed metres.")
        elif has_points:
            self.get_logger().info(f"l3_local: user-supplied scale {self.scale:g} to metres; source axes unchanged.")
        self.get_logger().info("Header stamps use local system receive/decode time; no hardware synchronization.")

    def publish_latest(self) -> None:
        # Identity, not source sequence: a source may restart with the same seq.
        if self.point_publisher is not None:
            frame = self.receiver.latest_points()
            if frame is not None and frame is not self._last_points:
                self._last_points = frame
                try:
                    self.point_publisher.publish(point_message(frame, scale=self.scale,
                                                               frame_id=self.point_frame_id))
                    self.point_messages += 1
                except ValueError as error:
                    self.publish_errors += 1
                    self.get_logger().error(str(error))
        if self.image_publisher is not None:
            frame = self.receiver.latest_video()
            if frame is not None and frame is not self._last_video:
                self._last_video = frame
                try:
                    self.image_publisher.publish(image_message(frame))
                    self.image_messages += 1
                except ValueError as error:
                    self.publish_errors += 1
                    self.get_logger().error(str(error))

    def stats(self) -> dict:
        return {**self.receiver.stats(), "ros_point_messages": self.point_messages,
                "ros_image_messages": self.image_messages, "ros_publish_errors": self.publish_errors,
                "point_frame_id": self.point_frame_id,
                "timestamp_basis": "local_system_receive_decode_time"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="Manifold Tailnet/USB host")
    parser.add_argument("--streams", choices=("points", "both", "video"), default="points")
    parser.add_argument("--point-port", type=int, default=9101)
    parser.add_argument("--video-port", type=int, default=9102)
    parser.add_argument("--socks5", help="Explicit SOCKS5 endpoint, e.g. 127.0.0.1:1055")
    parser.add_argument("--io-timeout", type=float, default=10)
    parser.add_argument("--seconds", type=float, default=0, help="0: until Ctrl+C")
    coordinates = parser.add_mutually_exclusive_group()
    coordinates.add_argument("--raw-coordinates", action="store_true",
                             help="explicit raw-coordinate diagnostic mode; no metric/axis claim")
    coordinates.add_argument("--xyz-scale-to-meters", type=float,
                             help="source-unit-to-metre multiplier AFTER source units are verified; axes unchanged")
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds < 0:
        parser.error("--seconds must be finite and nonnegative")
    if args.streams != "video" and not args.raw_coordinates and args.xyz_scale_to_meters is None:
        parser.error("select --raw-coordinates for diagnosis or a verified --xyz-scale-to-meters")
    if args.xyz_scale_to_meters is not None and (not math.isfinite(args.xyz_scale_to_meters) or
                                                args.xyz_scale_to_meters <= 0):
        parser.error("--xyz-scale-to-meters must be finite and positive")
    try:
        receiver = StreamReceiver(args.host, args.point_port, args.video_port, args.streams,
                                  io_timeout=args.io_timeout, socks5=args.socks5)
    except (ImportError, ValueError) as error:
        parser.error(str(error))
    rclpy.init(args=[])
    node = L3RosBridge(receiver, raw_coordinates=args.raw_coordinates,
                       xyz_scale_to_meters=args.xyz_scale_to_meters)
    started = last_report = time.monotonic()
    interrupted = False
    receiver.start()
    try:
        while rclpy.ok():
            rclpy.spin_once(node, timeout_sec=0.1)
            now = time.monotonic()
            if now - last_report >= 1:
                print(json.dumps({"event": "ros_stats", **node.stats()}), flush=True)
                last_report = now
            if args.seconds and now - started >= args.seconds:
                break
    except KeyboardInterrupt:
        interrupted = True
    finally:
        receiver.stop()
        stats = node.stats()
        node.destroy_node()
        rclpy.try_shutdown()
    passed = ((args.streams == "video" or stats["ros_point_messages"] > 0) and
              (args.streams == "points" or stats["ros_image_messages"] > 0) and
              stats["ros_publish_errors"] == 0)
    print(json.dumps({"event": "ros_complete", "published_requested_streams": passed,
                      "elapsed_seconds": round(time.monotonic() - started, 3), **stats}), flush=True)
    return 130 if interrupted else 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
