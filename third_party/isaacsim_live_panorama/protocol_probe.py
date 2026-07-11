"""Receive and validate one panorama frame without starting Isaac Sim."""

import socket
import struct

import cv2
import numpy as np


HEADER = struct.Struct("!4sHHQQIII")


def recv_exact(conn: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = conn.recv(remaining)
        if not chunk:
            raise ConnectionError("client disconnected")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("0.0.0.0", 5002))
    server.listen(1)
    print("PROBE_LISTENING 0.0.0.0:5002", flush=True)
    conn, addr = server.accept()

    with conn:
        header = recv_exact(conn, HEADER.size)
        magic, version, codec, frame_id, _, width, height, payload_size = HEADER.unpack(header)
        payload = recv_exact(conn, payload_size)

image = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
if magic != b"PANO" or version != 1 or codec != 1 or image is None:
    raise RuntimeError("invalid panorama frame")
if image.shape[1] != width or image.shape[0] != height:
    raise RuntimeError(f"dimension mismatch: header={width}x{height}, image={image.shape}")

print(
    f"PROBE_OK client={addr[0]} frame={frame_id} size={width}x{height} jpeg={payload_size}",
    flush=True,
)
