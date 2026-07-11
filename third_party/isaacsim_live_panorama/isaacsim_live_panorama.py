"""Run in Isaac Sim Script Editor to display the X5 live panorama."""

from __future__ import annotations

import asyncio
import builtins
import math
import socket
import struct
import threading
import time
from typing import Optional

import cv2
import numpy as np
import omni.kit.app
import omni.ui as ui
import omni.usd
from pxr import Gf, Sdf, UsdGeom, UsdShade, Vt


HOST = "0.0.0.0"
PORT = 5002
MAX_PAYLOAD_BYTES = 32 * 1024 * 1024
HEADER = struct.Struct("!4sHHQQIII")
MAGIC = b"PANO"
VERSION = 1
CODEC_JPEG = 1

SPHERE_PATH = "/World/Insta360LivePanorama"
MATERIAL_PATH = "/World/Insta360LivePanoramaMaterial"
TEXTURE_NAME = "insta360_live_panorama"
SPHERE_RADIUS = 100.0
SPHERE_SEGMENTS = 128
SPHERE_RINGS = 64
FLIP_HORIZONTAL = True


def log(message: str) -> None:
    print(f"[Insta360 Live {time.strftime('%H:%M:%S')}] {message}", flush=True)


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


def create_panorama_sphere(stage) -> None:
    """创建带 ERP UV 的内视球体；球体只负责显示，不参与物理碰撞。"""
    if stage.GetPrimAtPath(SPHERE_PATH).IsValid():
        stage.RemovePrim(SPHERE_PATH)
    if stage.GetPrimAtPath(MATERIAL_PATH).IsValid():
        stage.RemovePrim(MATERIAL_PATH)

    points = []
    texcoords = []
    for ring in range(SPHERE_RINGS + 1):
        v = ring / SPHERE_RINGS
        phi = v * math.pi
        sin_phi = math.sin(phi)
        cos_phi = math.cos(phi)
        for segment in range(SPHERE_SEGMENTS + 1):
            u = segment / SPHERE_SEGMENTS
            theta = u * 2.0 * math.pi
            points.append(
                Gf.Vec3f(
                    SPHERE_RADIUS * sin_phi * math.cos(theta),
                    SPHERE_RADIUS * sin_phi * math.sin(theta),
                    SPHERE_RADIUS * cos_phi,
                )
            )
            # 从球体内部观察会产生水平镜像；直接翻转 UV，避免逐帧复制 4K 图像。
            texture_u = 1.0 - u if FLIP_HORIZONTAL else u
            texcoords.append(Gf.Vec2f(texture_u, 1.0 - v))

    counts = []
    indices = []
    row_size = SPHERE_SEGMENTS + 1
    for ring in range(SPHERE_RINGS):
        for segment in range(SPHERE_SEGMENTS):
            top_left = ring * row_size + segment
            top_right = top_left + 1
            bottom_left = top_left + row_size
            bottom_right = bottom_left + 1
            counts.append(4)
            indices.extend([top_left, top_right, bottom_right, bottom_left])

    mesh = UsdGeom.Mesh.Define(stage, SPHERE_PATH)
    mesh.CreatePointsAttr().Set(points)
    mesh.CreateFaceVertexCountsAttr().Set(counts)
    mesh.CreateFaceVertexIndicesAttr().Set(indices)
    mesh.CreateDoubleSidedAttr().Set(True)
    mesh.CreatePurposeAttr().Set(UsdGeom.Tokens.render)

    st = UsdGeom.PrimvarsAPI(mesh.GetPrim()).CreatePrimvar(
        "st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex
    )
    st.Set(Vt.Vec2fArray(texcoords))

    material = UsdShade.Material.Define(stage, MATERIAL_PATH)
    st_reader = UsdShade.Shader.Define(stage, MATERIAL_PATH + "/PrimvarReader")
    st_reader.CreateIdAttr("UsdPrimvarReader_float2")
    st_reader.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")
    st_reader.CreateOutput("result", Sdf.ValueTypeNames.Float2)

    texture = UsdShade.Shader.Define(stage, MATERIAL_PATH + "/Texture")
    texture.CreateIdAttr("UsdUVTexture")
    texture.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(
        Sdf.AssetPath(f"dynamic://{TEXTURE_NAME}")
    )
    texture.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("sRGB")
    texture.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(
        st_reader.ConnectableAPI(), "result"
    )
    texture.CreateOutput("rgb", Sdf.ValueTypeNames.Color3f)

    surface = UsdShade.Shader.Define(stage, MATERIAL_PATH + "/PreviewSurface")
    surface.CreateIdAttr("UsdPreviewSurface")
    surface.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(
        Gf.Vec3f(0.0, 0.0, 0.0)
    )
    surface.CreateInput("emissiveColor", Sdf.ValueTypeNames.Color3f).ConnectToSource(
        texture.ConnectableAPI(), "rgb"
    )
    surface.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(1.0)
    surface.CreateOutput("surface", Sdf.ValueTypeNames.Token)
    material.CreateSurfaceOutput().ConnectToSource(surface.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)


class LivePanorama:
    def __init__(self) -> None:
        self.running = True
        self.server: Optional[socket.socket] = None
        self.worker: Optional[threading.Thread] = None
        self.task: Optional[asyncio.Task] = None
        self.frame_lock = threading.Lock()
        self.pending_jpeg: Optional[bytes] = None
        self.pending_frame_id = 0
        self.last_uploaded_frame_id = 0
        self.received_count = 0
        self.uploaded_count = 0
        self.last_frame_size = (0, 0)
        self.provider = ui.DynamicTextureProvider(TEXTURE_NAME)

    def start(self) -> None:
        context = omni.usd.get_context()
        stage = context.get_stage()
        if stage is None:
            context.new_stage()
            stage = context.get_stage()
        if stage is None:
            raise RuntimeError("Isaac Sim stage is not available")
        create_panorama_sphere(stage)

        self.worker = threading.Thread(target=self._network_loop, daemon=True)
        self.worker.start()
        self.task = asyncio.ensure_future(self._upload_loop())
        omni.usd.get_context().get_selection().set_selected_prim_paths([SPHERE_PATH], True)
        log(f"Listening on {HOST}:{PORT}; texture=dynamic://{TEXTURE_NAME}")

    def stop(self) -> None:
        self.running = False
        if self.server is not None:
            try:
                self.server.close()
            except OSError:
                pass
        if self.task is not None:
            self.task.cancel()
        log("Stopped")

    def _network_loop(self) -> None:
        try:
            server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server = server
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((HOST, PORT))
            server.listen(1)
            server.settimeout(1.0)

            while self.running:
                try:
                    conn, addr = server.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break

                log(f"Camera connected: {addr[0]}:{addr[1]}")
                conn.settimeout(3.0)
                try:
                    self._receive_client(conn)
                except Exception as exc:
                    if self.running:
                        log(f"Camera disconnected: {exc}")
                finally:
                    conn.close()
        except Exception as exc:
            if self.running:
                log(f"Network worker failed: {exc}")

    def _receive_client(self, conn: socket.socket) -> None:
        while self.running:
            values = HEADER.unpack(recv_exact(conn, HEADER.size))
            magic, version, codec, frame_id, _, width, height, payload_size = values
            if magic != MAGIC or version != VERSION or codec != CODEC_JPEG:
                raise ValueError("unsupported frame header")
            if width <= 0 or height <= 0:
                raise ValueError("invalid frame dimensions")
            if payload_size <= 0 or payload_size > MAX_PAYLOAD_BYTES:
                raise ValueError(f"invalid payload size: {payload_size}")

            jpeg = recv_exact(conn, payload_size)
            # 网络线程只覆盖最新帧，Isaac Sim 慢时不会积累延迟。
            with self.frame_lock:
                self.pending_jpeg = jpeg
                self.pending_frame_id = frame_id
                self.received_count += 1

    async def _upload_loop(self) -> None:
        uploaded_frame_id = 0
        uploaded_count = 0
        last_log = time.monotonic()

        while self.running:
            jpeg = None
            frame_id = 0
            with self.frame_lock:
                if self.pending_frame_id != uploaded_frame_id:
                    jpeg = self.pending_jpeg
                    frame_id = self.pending_frame_id

            if jpeg is not None:
                frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
                if frame is not None:
                    rgba = cv2.cvtColor(frame, cv2.COLOR_BGR2RGBA)
                    height, width = rgba.shape[:2]
                    pixels = np.ascontiguousarray(rgba.reshape(-1), dtype=np.uint8)
                    self.provider.set_data_array(pixels, [width, height])
                    uploaded_frame_id = frame_id
                    self.last_uploaded_frame_id = frame_id
                    self.uploaded_count += 1
                    self.last_frame_size = (width, height)
                    uploaded_count = self.uploaded_count

            now = time.monotonic()
            if now - last_log >= 5.0 and uploaded_count:
                log(f"Displayed frames={uploaded_count}, latest={uploaded_frame_id}")
                last_log = now
            await omni.kit.app.get_app().next_update_async()


old_instance = getattr(builtins, "_insta360_live_panorama", None)
if old_instance is not None:
    old_instance.stop()

instance = LivePanorama()
builtins._insta360_live_panorama = instance
instance.start()
