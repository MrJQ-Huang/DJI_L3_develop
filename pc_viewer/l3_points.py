"""Inspect complete L3 PSDK packets and verify a TCP point-data bridge.

The bridge envelope is !4sI (L3P1, byte count) + one unchanged DJI packet.
This is not the DJI RF or .ldrt file protocol. No aircraft commands are sent.
"""
from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ENVELOPE = struct.Struct("!4sI")
MAGIC = b"L3P1"
# DJI PSDK packed header, followed by the two CRC fields and packed points.
# Little endian is the initial ARM64 interpretation; verify against real data.
HEADER = struct.Struct("<IIQIII")
POINT = struct.Struct("<fffBBBB")
MAX_PACKET = 512 * 1024  # Initial cap follows the official PSDK sample.


@dataclass(frozen=True)
class Packet:
    sequence: int
    source_timestamp_raw: int
    crc_header_raw: int
    crc_rest_raw: int
    points: memoryview

    @property
    def point_count(self) -> int:
        return len(self.points) // POINT.size

    def iter_points(self):
        """Yield (x, y, z, intensity, r, g, b), without coordinate conversion."""
        return POINT.iter_unpack(self.points)


def parse_packet(raw: bytes) -> Packet:
    if not HEADER.size + POINT.size <= len(raw) <= MAX_PACKET:
        raise ValueError("invalid point packet size")
    flag, sequence, stamp, size, crc_header, crc_rest = HEADER.unpack_from(raw)
    if flag != 0xFFFFFFFF:
        raise ValueError("invalid DJI packet flag")
    if size != len(raw) - HEADER.size or size % POINT.size:
        raise ValueError("point payload length does not match the packed records")
    return Packet(sequence, stamp, crc_header, crc_rest, memoryview(raw)[HEADER.size:])


def summarize(packet: Packet) -> dict:
    low, high = [math.inf] * 3, [-math.inf] * 3
    for x, y, z, *_ in packet.iter_points():
        for axis, value in enumerate((x, y, z)):
            if not math.isfinite(value):
                raise ValueError("non-finite XYZ coordinate")
            low[axis] = min(low[axis], value)
            high[axis] = max(high[axis], value)
    return {"sequence": packet.sequence, "source_timestamp_raw": packet.source_timestamp_raw,
            "points": packet.point_count, "xyz_min_raw": low, "xyz_max_raw": high}


def frame_packet(raw: bytes) -> bytes:
    parse_packet(raw)
    return ENVELOPE.pack(MAGIC, len(raw)) + raw


def _read_exact(sock: socket.socket, count: int, deadline: float) -> bytes:
    data = bytearray(count)
    view = memoryview(data)
    done = 0
    while done < count:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("point receive deadline exceeded")
        sock.settimeout(remaining)
        received = sock.recv_into(view[done:])
        if received == 0:
            raise EOFError("peer closed before the requested packet count")
        done += received
    return bytes(data)


def read_packet(sock: socket.socket, deadline: float) -> Packet:
    magic, length = ENVELOPE.unpack(_read_exact(sock, ENVELOPE.size, deadline))
    if magic != MAGIC or not HEADER.size + POINT.size <= length <= MAX_PACKET:
        raise ValueError("invalid L3P1 envelope")
    return parse_packet(_read_exact(sock, length, deadline))


def listen(bind: str, port: int, count: int, timeout: float) -> None:
    with socket.socket() as server:
        server.bind((bind, port))
        server.listen(1)
        deadline = time.monotonic() + timeout
        server.settimeout(timeout)
        print(json.dumps({"event": "listening", "address": server.getsockname()}), flush=True)
        peer, _ = server.accept()
        with peer:
            points = 0
            for _ in range(count):
                summary = summarize(read_packet(peer, deadline))
                points += summary["points"]
                print(json.dumps({"event": "packet", **summary}), flush=True)
        print(json.dumps({"event": "complete", "packets": count, "points": points}), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser("inspect", help="inspect one raw DJI MOP packet file")
    inspect.add_argument("packet", type=Path)
    receive = commands.add_parser("listen", help="receive a bounded batch over TCP")
    receive.add_argument("--bind", default="127.0.0.1")
    receive.add_argument("--port", type=int, default=9010)
    receive.add_argument("--count", type=int, default=30)
    receive.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    try:
        if args.command == "inspect":
            with args.packet.open("rb") as file:
                raw = file.read(MAX_PACKET + 1)
            print(json.dumps(summarize(parse_packet(raw))))
        else:
            if not (0 <= args.port <= 65535 and 1 <= args.count <= 10000 and 0 < args.timeout <= 300):
                parser.error("port 0..65535, count 1..10000, timeout >0..300 seconds required")
            listen(args.bind, args.port, args.count, args.timeout)
    except (OSError, EOFError, ValueError) as error:
        print(f"L3 point probe: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
