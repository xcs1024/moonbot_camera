# Insta360 USB AI Stitch Demo

这个 demo 路径是：

```text
相机 --USB--> Camera SDK --双鱼眼 H.265/H.264 流 + 陀螺仪/曝光数据--> MediaSDK AI 拼接 --> 全景预览 / data 输出
```

## 输出

全部使用相对路径：

```text
data/Insta/preview.mp4
data/Insta/snapshot.jpg
data/Insta/logs/camera_sdk.log
data/Insta/logs/media_sdk.log
```

## 构建

```powershell
cmake -S src/camera -B build/camera -A x64 -DOpenCV_DIR=你的OpenCVConfig.cmake目录
cmake --build build/camera --config Release
```

如果系统已经能被 CMake 自动找到 OpenCV，可以省略 `-DOpenCV_DIR=...`。

## 运行

在仓库根目录运行，保证 `libs/MediaSDK/models` 这个相对路径存在：

```powershell
.\build\camera\Release\insta360_ai_stitch_preview.exe --seconds 30 --output-size 1920x960
```

无窗口预览，只保存数据到 `data/Insta`：

```powershell
.\build\camera\Release\insta360_ai_stitch_preview.exe --seconds 30 --no-preview
```

## 参数

```text
--seconds N        采集时长，默认 30
--output-size WxH  全景输出尺寸，默认 1920x960，必须保持 2:1
--no-preview       不显示实时预览窗口
--no-mp4           不保存 data/Insta/preview.mp4
--no-snapshot      不保存 data/Insta/snapshot.jpg
```

## 注意

- 相机必须通过 USB 连接，并在相机上选择 Android 模式。
- Windows 需要 libusbK 驱动。
- 当前拼接算法固定为 `ins::STITCH_TYPE::AIFLOW`。
- 实时保存 mp4/jpg 使用 OpenCV；CameraSDK 和 MediaSDK 只负责取流与拼接。
