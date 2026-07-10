# Insta360 X5 实时全景拼接与远端 DAP 深度估计

本项目用于从 Insta360 X5 通过 USB 获取实时双鱼眼视频流，使用 CameraSDK + MediaSDK 在 Windows 端完成实时全景拼接预览。后续规划是将拼接后的全景图通过 TCP 发送到远端 NVIDIA DGX Spark，由 DAP 模型生成单目全景深度图，再按需要转换为点云。

当前主程序：

```text
src/camera/insta360_ai_stitch_preview.cpp
```

当前本机预览数据链路：

```text
Insta360 X5
  -- USB / Android 模式 -->
CameraSDK
  -- H.265/H.264 视频帧 + gyro + exposure -->
MediaSDK RealTimeStitcher
  -- RGBA 全景帧 -->
OpenCV 预览窗口
```

当前 TCP 数据链路：

```text
MediaSDK RGBA 全景帧
  -- OpenCV 转 BGR + JPEG 编码 -->
TCP 172.16.23.253:5001
  -- Python 接收端 -->
/tmp/dap_tcp/latest.jpg
```

## 目录结构

SDK 和 OpenCV 需要放在 `libs` 目录下：

```text
libs
├─CameraSDK
│  ├─bin
│  ├─example
│  ├─include
│  │  ├─camera
│  │  └─stream
│  └─lib
├─MediaSDK
│  ├─bin
│  │  └─models
│  ├─example
│  ├─include
│  │  └─stitcher
│  ├─lib
│  └─models
└─opencv
   └─build
```

依赖来源：

- Insta360 Windows CameraSDK + MediaSDK：解压后分别重命名为 `libs/CameraSDK` 和 `libs/MediaSDK`
- OpenCV Windows 包：下载 `opencv-4.13.0-windows.exe`，解压到 `libs/opencv`

## Windows 侧准备

1. 相机通过 USB 连接电脑。
2. 相机 USB 模式选择 Android。
3. Windows 使用 Zadig 安装 `libusbK` 驱动。

驱动安装参考：

![Zadig 安装图](./docs/zadig.png)

## 构建

在仓库根目录运行：

```powershell
.\scripts\build_camera_msvc.cmd
```

生成的程序位于：

```text
build\camera_msvc\Release\insta360_ai_stitch_preview.exe
```

## 运行

建议在仓库根目录运行程序，保证 SDK 的相对路径和模型目录可访问：

```powershell
.\build\camera_msvc\Release\insta360_ai_stitch_preview.exe
```

预期输出包含：

```text
Camera: Insta360 X5, firmware: ...
Starting CameraSDK live stream...
CameraSDK live stream started.
Starting MediaSDK stitcher...
MediaSDK stitcher started.
实时全景预览已启动，按 ESC 退出。
CameraSDK 已收到首个视频帧，size=...
MediaSDK 已输出首个全景帧。
```

按 `ESC` 或关闭 OpenCV 窗口退出。

## 当前实现要点

当前程序固定支持 Insta360 X5：

```text
ins_camera::CameraType::Insta360X5
```

当前预览流参数：

```text
CameraSDK live stream: RES_1920_960P30
MediaSDK output:      960 x 480 RGBA
Stitch type:          DYNAMICSTITCH
FlowState:            enabled
Audio:                disabled
TCP target:           172.16.23.253:5001
JPEG quality:         85
TCP send FPS:         5
```

当前代码按官方顺序启动：

```text
1. camera->StartLiveStreaming(...)
2. stitcher->StartStitch()
3. CameraSDK 回调里把视频/gyro/exposure 直接转交给 MediaSDK
4. MediaSDK 回调里复制 RGBA 全景帧
5. 主线程 OpenCV 显示最新一帧
```

注意：MediaSDK 回调里的图像内存只在回调期间有效，必须立即复制，不能保存裸指针。

## 常见问题

### 1. PowerShell 提示找不到 exe

确认运行目录。程序路径是相对仓库根目录的：

```powershell
cd C:\code\moonbot\Insta360
.\build\camera_msvc\Release\insta360_ai_stitch_preview.exe
```

如果当前目录是 `src\camera`，这个相对路径会不正确。

### 2. 控制台中文乱码

程序里已经设置 Windows 控制台为 UTF-8：

```text
SetConsoleOutputCP(CP_UTF8)
SetConsoleCP(CP_UTF8)
```

如果仍乱码，先在 PowerShell 执行：

```powershell
chcp 65001
```

### 3. 程序退出后仍有进程常驻

通常是旧进程没有正常退出，占用了相机或 SDK 资源。先查看：

```powershell
Get-Process insta360_ai_stitch_preview -ErrorAction SilentlyContinue
```

必要时结束旧进程：

```powershell
Stop-Process -Name insta360_ai_stitch_preview -Force
```

### 4. MediaSDK 崩溃或异常退出

`src/camera/CMakeLists.txt` 已在构建后复制 CameraSDK、MediaSDK 和 OpenCV 运行库，并删除 MediaSDK 附带的旧版 VC 运行库，避免旧 `msvcp140.dll` 覆盖系统新版 CRT。

如果仍出现访问冲突，优先检查输出目录里是否残留旧 DLL：

```powershell
Get-ChildItem .\build\camera_msvc\Release\*140*.dll
```

### 5. `heartbeat timeout`

这通常不是算力不足的第一嫌疑。更常见原因是：

- 相机流被旧进程占用
- CameraSDK 回调中做了耗时操作
- MediaSDK 启动顺序不对
- USB/驱动状态异常

当前代码已经避免在 CameraSDK 回调里做显示、编码、网络发送等重操作。

## 远端 DAP 环境

### 注意

```text
将tcp_jpeg_receiver.py 已发在third_party下，即在远端配置好DAP环境后，将其放在根目录下就可以进行使用
```

远端机器：

```text
User: nvidia
Conda env: dap
DAP path: /home/nvidia/DAP
GPU: NVIDIA GB10
```

进入环境：

```bash
ssh nvidia@ip
conda activate dap
cd ~/DAP
```

已验证的核心环境：

```text
Python:      3.12
torch:       2.11.0+cu130
torchvision: 0.26.0+cu130
CUDA:        available, 13.0
Device:      NVIDIA GB10
```

DAP 权重路径：

```text
/home/nvidia/DAP/weights/DAP-weights/model.pth
```

`config/infer.yaml` 中的权重目录应为：

```yaml
load_weights_dir: /home/nvidia/DAP/weights/DAP-weights
```

测试 DAP：

```bash
python test/infer.py \
  --config config/infer.yaml \
  --txt /tmp/dap_test.txt \
  --output /tmp/dap_test_output \
  --gpu 0
```

说明：

- `open3d` 在 ARM64 Linux 上没有合适的 PyPI wheel，不建议强行安装。
- DAP 推理不依赖 `open3d`。
- 后续点云建议直接用 `numpy` 写 PCD/PLY，不依赖 Open3D。
- 当前 PyPI 的 `utils3d` 包不包含 `utils3d.numpy.image_uv`，不要依赖它做 ERP 点云转换。

## TCP JPEG 接收端

远端接收脚本：

```text
/home/nvidia/DAP/tcp_jpeg_receiver.py
```

后台服务启动方式：

```bash
conda activate dap
cd ~/DAP
nohup python ~/DAP/tcp_jpeg_receiver.py \
  --host 0.0.0.0 \
  --port 5001 \
  --save-dir /tmp/dap_tcp \
  > /tmp/dap_tcp_receiver.log 2>&1 &
```

检查接收端：

```bash
pgrep -af tcp_jpeg_receiver.py
ss -ltnp | grep ':5001'
tail -f /tmp/dap_tcp_receiver.log
```

接收到的最新 JPEG：

```text
/tmp/dap_tcp/latest.jpg
```

## TCP 方案

Windows 拼接程序和远端 DAP 推理服务拆成两个独立进程：

```text
Windows
  X5 -> CameraSDK -> MediaSDK -> RGBA ERP frame
  -> BGR/RGB
  -> JPEG encode
  -> TCP send latest frame only

DGX Spark
  TCP receive
  -> JPEG decode
  -> RGB 1024x512
  -> DAP resident model inference
  -> depth map
  -> optional PCD/PLY
```

关键原则：

- 不要在 CameraSDK 回调里做 TCP 发送。
- 不要在 MediaSDK 回调里做阻塞网络发送。
- 回调里只复制最新帧，网络线程只发送最新帧。
- 队列容量建议固定为 1，DAP 或网络慢时直接丢旧帧，避免延迟越积越大。
- DAP 模型必须服务启动时加载一次，不能每帧启动 `test/infer.py`。

建议起步参数：

```text
Panorama size: 1024 x 512
Codec:         JPEG
Quality:       85
FPS:           2-5 for DAP
Queue:         latest frame only
Depth output:  float32 numpy or downsampled PCD
```

TCP 帧协议建议：

```text
magic        4 bytes   "PANO"
version      uint16
codec        uint16   1 = JPEG
frame_id     uint64
timestamp_us uint64
width        uint32
height       uint32
payload_len  uint32
payload      bytes
```

TCP 没有消息边界，收发两端都必须实现：

```text
send_all()
recv_exact()
```

不要直接发送 C++ 原生结构体，建议显式按网络字节序序列化字段。

## 点云转换思路

DAP 输出的是单目深度图，不是严格几何真值。用于粗略空间感知可以接受；如果要积累多帧点云，还需要相机位姿或外部里程计/SLAM。

ERP 全景图转点云的基本关系：

```text
u = x / width
v = y / height
theta = (1 - u) * 2π
phi = v * π

dir = [
  sin(phi) * cos(theta),
  sin(phi) * sin(theta),
  cos(phi)
]

point = depth * dir
```

实际生成 PCD 时建议：

- 每 2-4 个像素采样一次，降低点数。
- 深度裁剪到合理范围，例如 `0.3m - 30m`。
- 输出前做 voxel downsample。
- 只在远端需要点云时生成，不要默认把完整点云传回 Windows。
