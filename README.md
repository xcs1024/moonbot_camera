# 调用Insta360进行三维重建场景

## 必要条件

libs中导入Windows_CameraSDK-2.1.1_MediaSDK-3.1.3.zip并重新命名
下载https://github.com/opencv/opencv/releases/download/4.13.0/opencv-4.13.0-windows.exe
保存到libs里面

```bash
C:.
libs
├─CameraSDK
│      ├─bin
│      ├─example
│      ├─include
│      │  ├─camera
│      │  └─stream
│      └─lib
└─MediaSDK
    ├─bin
    │  └─models
    │      ├─cameraaccessory
    │      └─coolingshell
    ├─example
    ├─include
    │  └─stitcher
    ├─lib
    └─models
        ├─cameraaccessory
        └─coolingshell
└─opencv
```

## 环境配置

### window使用

1. 使用Zadig安装驱动
![Zadig安装图](./docs/zadig.png)

2. Insta x4 air SDK 使用

实现路径：

```text
相机 --USB--> Camera SDK --双鱼眼 H.265/H.264 流 + 陀螺仪/曝光数据--> MediaSDK AI 拼接 --> 全景预览 / data 输出
```

输出:

```text
data/Insta/preview.mp4
data/Insta/snapshot.jpg
data/Insta/logs/camera_sdk.log
data/Insta/logs/media_sdk.log
```

构建:

```powershell
.\scripts\build_camera_msvc.cmd
```

运行:

在仓库根目录运行，保证 `libs/MediaSDK/models` 这个相对路径存在：

```powershell
.\build\camera\Release\insta360_ai_stitch_preview.exe --seconds 30 --output-size 1920x960
```

无窗口预览，只保存数据到 `data/Insta`：

```powershell
.\build\camera\Release\insta360_ai_stitch_preview.exe --seconds 30 --no-preview
```

参数:

```text
--seconds N        采集时长，默认 30
--output-size WxH  全景输出尺寸，默认 1920x960，必须保持 2:1
--no-preview       不显示实时预览窗口
--no-mp4           不保存 data/Insta/preview.mp4
--no-snapshot      不保存 data/Insta/snapshot.jpg
```

注意:

- 相机必须通过 USB 连接，并在相机上选择 Android 模式。
- Windows 需要 libusbK 驱动。
- 当前拼接算法固定为 `ins::STITCH_TYPE::AIFLOW`。
- 实时保存 mp4/jpg 使用 OpenCV；CameraSDK 和 MediaSDK 只负责取流与拼接。
