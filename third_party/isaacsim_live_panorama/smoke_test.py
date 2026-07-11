"""Headless smoke test for the Isaac Sim live panorama script."""

from __future__ import annotations

import socket
import struct
import threading
import time

from isaacsim import SimulationApp


app = SimulationApp({"headless": True})

import builtins  # noqa: E402

import cv2  # noqa: E402
import numpy as np  # noqa: E402
import omni.usd  # noqa: E402


SCRIPT = "/home/nvidia/insta360_live_panorama/isaacsim_live_panorama.py"
HEADER = struct.Struct("!4sHHQQIII")


def send_test_frame() -> None:
    time.sleep(2.0)
    image = np.zeros((512, 1024, 3), dtype=np.uint8)
    image[:, :341] = (0, 0, 255)
    image[:, 341:682] = (0, 255, 0)
    image[:, 682:] = (255, 0, 0)
    ok, encoded = cv2.imencode(".jpg", image)
    if not ok:
        raise RuntimeError("failed to encode test frame")

    header = HEADER.pack(
        b"PANO", 1, 1, 1, int(time.time() * 1_000_000), 1024, 512, len(encoded)
    )
    with socket.create_connection(("127.0.0.1", 5002), timeout=5.0) as conn:
        conn.sendall(header)
        conn.sendall(encoded.tobytes())


try:
    exec(compile(open(SCRIPT, "rb").read(), SCRIPT, "exec"))
    sender = threading.Thread(target=send_test_frame, daemon=True)
    sender.start()

    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        app.update()
        instance = builtins._insta360_live_panorama
        if instance.last_uploaded_frame_id == 1:
            for _ in range(10):
                app.update()
            break
    else:
        raise RuntimeError("test frame was not received")

    stage = omni.usd.get_context().get_stage()
    assert stage.GetPrimAtPath("/World/Insta360LivePanorama").IsValid()
    assert builtins._insta360_live_panorama.last_uploaded_frame_id == 1
    print("SMOKE_TEST_OK", flush=True)
finally:
    instance = getattr(builtins, "_insta360_live_panorama", None)
    if instance is not None:
        instance.stop()
    app.close()
