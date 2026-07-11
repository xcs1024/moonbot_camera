import argparse
import os
import socket
import struct
import sys
import time
from pathlib import Path

import cv2
import numpy as np
import torch
import yaml

PROJECT_ROOT = Path(__file__).resolve().parent
sys.path.append(str(PROJECT_ROOT / "test"))
from infer import infer_raw, load_model  # noqa: E402

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


def erp_points_from_depth(depth: np.ndarray, rgb: np.ndarray, stride: int, depth_scale: float, max_depth_m: float):
    # ERP 全景深度转方向向量；这里不依赖 Open3D，便于 ARM64 部署。
    depth_m = depth.astype(np.float32) * float(depth_scale)
    h, w = depth_m.shape

    ys = np.arange(0, h, stride, dtype=np.float32)
    xs = np.arange(0, w, stride, dtype=np.float32)
    grid_x, grid_y = np.meshgrid(xs, ys)

    sampled_depth = depth_m[::stride, ::stride]
    sampled_rgb = rgb[::stride, ::stride]

    u = grid_x / float(w)
    v = grid_y / float(h)
    theta = (1.0 - u) * (2.0 * np.pi)
    phi = v * np.pi

    sin_phi = np.sin(phi)
    dirs = np.stack(
        [
            sin_phi * np.cos(theta),
            sin_phi * np.sin(theta),
            np.cos(phi),
        ],
        axis=-1,
    ).astype(np.float32)

    points = dirs * sampled_depth[..., None]
    mask = np.isfinite(sampled_depth) & (sampled_depth > 0.01) & (sampled_depth <= max_depth_m)

    return points[mask].astype(np.float32), sampled_rgb[mask].astype(np.uint8)


def atomic_write_bytes(path: Path, data: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("wb") as f:
        f.write(data)
    tmp.replace(path)


def atomic_imwrite(path: Path, image: np.ndarray) -> None:
    # OpenCV 依赖扩展名选择编码器，所以不能直接写 latest.jpg.tmp。
    ok, encoded = cv2.imencode(path.suffix, image)
    if not ok:
        raise RuntimeError(f"failed to encode image: {path}")
    atomic_write_bytes(path, encoded.tobytes())


def atomic_save_npy(path: Path, array: np.ndarray) -> None:
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("wb") as f:
        np.save(f, array)
    tmp.replace(path)


def write_binary_pcd(path: Path, points: np.ndarray, colors_rgb: np.ndarray) -> None:
    colors = colors_rgb.astype(np.uint32)
    rgb_packed = (colors[:, 0] << 16) | (colors[:, 1] << 8) | colors[:, 2]

    cloud = np.empty(points.shape[0], dtype=[("x", "<f4"), ("y", "<f4"), ("z", "<f4"), ("rgb", "<u4")])
    cloud["x"] = points[:, 0]
    cloud["y"] = points[:, 1]
    cloud["z"] = points[:, 2]
    cloud["rgb"] = rgb_packed

    header = (
        "# .PCD v0.7 - Point Cloud Data file format\n"
        "VERSION 0.7\n"
        "FIELDS x y z rgb\n"
        "SIZE 4 4 4 4\n"
        "TYPE F F F U\n"
        "COUNT 1 1 1 1\n"
        f"WIDTH {len(cloud)}\n"
        "HEIGHT 1\n"
        "VIEWPOINT 0 0 0 1 0 0 0\n"
        f"POINTS {len(cloud)}\n"
        "DATA binary\n"
    ).encode("ascii")

    atomic_write_bytes(path, header + cloud.tobytes())


def clean_output_dirs(args) -> None:
    if args.keep_old_output:
        return
    # 每次接收服务启动时清空旧结果，保证编号从 00000001 开始且 latest 不会指向旧帧。
    patterns = [
        (args.rgb_dir, "*.jpg"),
        (args.depth_dir, "*.npy"),
        (args.point_dir, "*.pcd"),
    ]
    removed = 0
    for directory, pattern in patterns:
        for path in directory.glob(pattern):
            if path.is_file():
                path.unlink()
                removed += 1
        for path in directory.glob("*.tmp"):
            if path.is_file():
                path.unlink()
                removed += 1
    print(f"cleaned old output files: {removed}", flush=True)


def save_outputs(frame_bgr: np.ndarray, pred_depth: np.ndarray, local_frame_id: int, source_frame_id: int, timestamp_us: int, args) -> None:
    rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)

    output_name = f"{local_frame_id:08d}"
    rgb_path = args.rgb_dir / f"{output_name}.jpg"
    depth_path = args.depth_dir / f"{output_name}.npy"
    pcd_path = args.point_dir / f"{output_name}.pcd"

    atomic_imwrite(rgb_path, frame_bgr)
    atomic_save_npy(depth_path, pred_depth.astype(np.float32))

    points, colors = erp_points_from_depth(
        pred_depth,
        rgb,
        stride=args.point_stride,
        depth_scale=args.depth_scale,
        max_depth_m=args.max_depth_m,
    )
    write_binary_pcd(pcd_path, points, colors)

    atomic_imwrite(args.rgb_dir / "latest.jpg", frame_bgr)
    atomic_save_npy(args.depth_dir / "latest.npy", pred_depth.astype(np.float32))
    write_binary_pcd(args.point_dir / "latest.pcd", points, colors)

    print(
        f"local_frame={local_frame_id} source_frame={source_frame_id} ts={timestamp_us} "
        f"rgb={rgb_path} depth={depth_path} pcd={pcd_path} points={len(points)}",
        flush=True,
    )


def handle_client(conn: socket.socket, addr, model, device, args, state) -> None:
    print(f"client connected: {addr}", flush=True)
    while True:
        header = recv_exact(conn, HEADER_SIZE)
        magic, version, codec, source_frame_id, timestamp_us, width, height, payload_len = struct.unpack(HEADER_FMT, header)

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
            print(f"decode failed: source_frame={source_frame_id}", flush=True)
            continue

        if frame_bgr.shape[1] != args.width or frame_bgr.shape[0] != args.height:
            frame_bgr = cv2.resize(frame_bgr, (args.width, args.height), interpolation=cv2.INTER_AREA)

        frame_rgb = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2RGB)
        start = time.perf_counter()
        pred_depth = infer_raw(model, device, frame_rgb)
        elapsed_ms = (time.perf_counter() - start) * 1000.0

        local_frame_id = state["next_frame_id"]
        state["next_frame_id"] += 1
        save_outputs(frame_bgr, pred_depth, local_frame_id, source_frame_id, timestamp_us, args)
        print(f"infer_ms={elapsed_ms:.1f} source={width}x{height}", flush=True)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5001)
    parser.add_argument("--config", default=str(PROJECT_ROOT / "config" / "infer.yaml"))
    parser.add_argument("--gpu", default="0")
    parser.add_argument("--rgb-dir", type=Path, default=PROJECT_ROOT / "data" / "rgb")
    parser.add_argument("--depth-dir", type=Path, default=PROJECT_ROOT / "data" / "depth")
    parser.add_argument("--point-dir", type=Path, default=PROJECT_ROOT / "data" / "point")
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--point-stride", type=int, default=4)
    parser.add_argument("--depth-scale", type=float, default=30.0)
    parser.add_argument("--max-depth-m", type=float, default=30.0)
    parser.add_argument("--keep-old-output", action="store_true")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    os.environ["CUDA_DEVICE_ORDER"] = "PCI_BUS_ID"
    os.environ["CUDA_VISIBLE_DEVICES"] = args.gpu

    args.rgb_dir.mkdir(parents=True, exist_ok=True)
    args.depth_dir.mkdir(parents=True, exist_ok=True)
    args.point_dir.mkdir(parents=True, exist_ok=True)
    clean_output_dirs(args)

    with open(args.config, "r", encoding="utf-8") as f:
        config = yaml.safe_load(f)

    print(f"torch_cuda={torch.cuda.is_available()} device_count={torch.cuda.device_count()}", flush=True)
    model, device = load_model(config)

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(1)
    state = {"next_frame_id": 1}

    print(f"listening on {args.host}:{args.port}", flush=True)
    print(f"rgb_dir={args.rgb_dir} point_dir={args.point_dir}", flush=True)

    while True:
        conn, addr = server.accept()
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            handle_client(conn, addr, model, device, args, state)
        except Exception as exc:
            print(f"client closed/error: {exc}", flush=True)
        finally:
            conn.close()
            time.sleep(0.5)


if __name__ == "__main__":
    main()
