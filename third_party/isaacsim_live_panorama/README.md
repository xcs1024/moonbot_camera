# Isaac Sim 实时 X5 全景背景

该脚本在 Isaac Sim 6.0.1 中接收 Windows 端发送的 `PANO/JPEG/TCP` 帧，
并将标准 2:1 ERP 全景更新到内视球体的动态纹理。它只负责显示，不创建碰撞。

## 启动

1. 启动 Isaac Sim。
2. 打开 `Window > Script Editor`。
3. 执行：

```python
exec(open("/home/nvidia/insta360_live_panorama/isaacsim_live_panorama.py", encoding="utf-8").read())
```

4. 在 Windows 启动 `insta360_ai_stitch_preview.exe`。

正常日志：

```text
[Insta360 Live ...] Listening on 0.0.0.0:5002
[Insta360 Live ...] Camera connected: ...
[Insta360 Live ...] Displayed frames=..., latest=...
```

重新执行脚本会先停止旧实例，因此不会重复占用端口。

## 停止

在 Script Editor 执行：

```python
import builtins
builtins._insta360_live_panorama.stop()
```

## 参数

- TCP 端口：`5002`
- 动态纹理：`dynamic://insta360_live_panorama`
- 球体半径：`100 m`
- 内视球水平纠正：`FLIP_HORIZONTAL = True`
- 输入：JPEG 编码的 2:1 ERP 图像
