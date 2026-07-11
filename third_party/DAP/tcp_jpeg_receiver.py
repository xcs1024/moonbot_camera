import argparse
import os
import socket
import struct
import time

import cv2
import numpy as np

HEADER_FMT = "!4sHHQQIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
MAGIC = b"PANO"
VERSION = 1
CODEC_JPEG = 1
MAX_PAYLOAD_SIZE = 20 * 1024 * 1024


def recv_exact(conn: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = conn.recv(remaining)
        if not chunk:
            raise ConnectionError("client disconnected")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def on_frame(frame_bgr: np.ndarray, frame_id: int, timestamp_us: int, save_dir: str) -> None:
    # 当前先验证 TCP/JPEG 链路；后续 DAP 推理从这里接入。
    latest_path = os.path.join(save_dir, "latest.jpg")
    cv2.imwrite(latest_path, frame_bgr)

    if frame_id % 30 == 0:
        snapshot_path = os.path.join(save_dir, f"{frame_id:08d}.jpg")
        cv2.imwrite(snapshot_path, frame_bgr)

    print(
        f"frame={frame_id} ts={timestamp_us} "
        f"shape={frame_bgr.shape} latest={latest_path}",
        flush=True,
    )


def handle_client(conn: socket.socket, addr, save_dir: str) -> None:
    print(f"client connected: {addr}", flush=True)
    while True:
        header = recv_exact(conn, HEADER_SIZE)
        magic, version, codec, frame_id, timestamp_us, width, height, payload_len = struct.unpack(
            HEADER_FMT,
            header,
        )

        if magic != MAGIC:
            raise ValueError(f"bad magic: {magic!r}")
        if version != VERSION:
            raise ValueError(f"bad version: {version}")
        if codec != CODEC_JPEG:
            raise ValueError(f"unsupported codec: {codec}")
        if payload_len <= 0 or payload_len > MAX_PAYLOAD_SIZE:
            raise ValueError(f"bad payload_len: {payload_len}")

        payload = recv_exact(conn, payload_len)
        jpg = np.frombuffer(payload, dtype=np.uint8)
        frame_bgr = cv2.imdecode(jpg, cv2.IMREAD_COLOR)
        if frame_bgr is None:
            print(f"decode failed: frame={frame_id}", flush=True)
            continue

        decoded_height, decoded_width = frame_bgr.shape[:2]
        if decoded_width != width or decoded_height != height:
            print(
                f"size mismatch: header={width}x{height}, "
                f"decoded={decoded_width}x{decoded_height}",
                flush=True,
            )

        on_frame(frame_bgr, frame_id, timestamp_us, save_dir)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--save-dir", default="/tmp/dap_tcp")
    args = parser.parse_args()

    os.makedirs(args.save_dir, exist_ok=True)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)

    print(f"listening on {args.host}:{args.port}", flush=True)

    while True:
        conn, addr = server.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            handle_client(conn, addr, args.save_dir)
        except Exception as exc:
            print(f"client closed/error: {exc}", flush=True)
        finally:
            conn.close()
            time.sleep(0.5)


if __name__ == "__main__":
    main()
