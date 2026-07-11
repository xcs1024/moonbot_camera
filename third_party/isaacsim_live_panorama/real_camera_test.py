"""Validate sustained X5 frame uploads through the real Isaac Sim renderer."""

from __future__ import annotations

import time

from isaacsim import SimulationApp


app = SimulationApp({"headless": True})

import builtins  # noqa: E402


SCRIPT = "/home/nvidia/insta360_live_panorama/isaacsim_live_panorama.py"
TARGET_UPLOAD_COUNT = 30
TIMEOUT_SECONDS = 75.0


try:
    exec(compile(open(SCRIPT, "rb").read(), SCRIPT, "exec"))
    deadline = time.monotonic() + TIMEOUT_SECONDS
    first_upload_time = None

    while time.monotonic() < deadline:
        app.update()
        instance = builtins._insta360_live_panorama
        if instance.uploaded_count and first_upload_time is None:
            first_upload_time = time.monotonic()
        if instance.uploaded_count >= TARGET_UPLOAD_COUNT:
            elapsed = time.monotonic() - first_upload_time
            fps = (instance.uploaded_count - 1) / elapsed
            print(
                "REAL_CAMERA_TEST_OK "
                f"received={instance.received_count} uploaded={instance.uploaded_count} "
                f"latest={instance.last_uploaded_frame_id} size={instance.last_frame_size} "
                f"elapsed={elapsed:.2f}s upload_fps={fps:.2f}",
                flush=True,
            )
            break
    else:
        instance = builtins._insta360_live_panorama
        raise RuntimeError(
            f"timed out: received={instance.received_count}, uploaded={instance.uploaded_count}"
        )
finally:
    instance = getattr(builtins, "_insta360_live_panorama", None)
    if instance is not None:
        instance.stop()
    app.close()
