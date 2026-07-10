#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <ins_realtime_stitcher.h>

#include <opencv2/opencv.hpp>

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

constexpr auto kPreviewResolution =
    ins_camera::VideoResolution::RES_1920_960P30;
constexpr int kOutputWidth = 960;
constexpr int kOutputHeight = 480;
constexpr const char* kWindowName = "Insta360 X5 Preview";

class StitchStreamDelegate final : public ins_camera::StreamDelegate {
public:
    explicit StitchStreamDelegate(
        const std::shared_ptr<ins::RealTimeStitcher>& stitcher)
        : stitcher_(stitcher) {}

    void OnAudioData(const uint8_t*, size_t, int64_t) override {}

    void OnVideoData(
        const uint8_t* data,
        size_t size,
        int64_t timestamp,
        uint8_t stream_type,
        int stream_index) override {
        if (data == nullptr || size == 0) {
            return;
        }

        if (!received_first_video_.exchange(true)) {
            std::cout << "CameraSDK 已收到首个视频帧，size="
                      << size << "。\n";
        }

        // 官方要求在 CameraSDK 数据回调中直接转交编码视频帧。
        stitcher_->HandleVideoData(
            data, size, timestamp, stream_type, stream_index);
    }

    void OnGyroData(
        const std::vector<ins_camera::GyroData>& data) override {
        std::vector<ins::GyroData> gyro_data(data.size());
        std::memcpy(
            gyro_data.data(),
            data.data(),
            data.size() * sizeof(ins_camera::GyroData));
        stitcher_->HandleGyroData(gyro_data);
    }

    void OnExposureData(
        const ins_camera::ExposureData& data) override {
        ins::ExposureData exposure{};
        exposure.exposure_time = data.exposure_time;
        exposure.timestamp = data.timestamp;
        stitcher_->HandleExposureData(exposure);
    }

private:
    std::shared_ptr<ins::RealTimeStitcher> stitcher_;
    std::atomic<bool> received_first_video_{false};
};

}  // namespace

int main() {
#ifdef _WIN32
    // PowerShell/控制台统一使用 UTF-8，避免中文输出乱码。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    // MediaSDK 要求在主程序开始时初始化。
    ins::InitEnv();

    ins_camera::DeviceDiscovery discovery;
    auto devices = discovery.GetAvailableDevices();
    if (devices.empty()) {
        std::cerr << "未发现相机，请检查 USB 安卓模式和 libusbK 驱动。\n";
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    const auto& device = devices.front();
    std::cout << "Camera: " << device.camera_name
              << ", firmware: " << device.fw_version << '\n';

    if (device.camera_type != ins_camera::CameraType::Insta360X5) {
        std::cerr << "当前连接的第一台设备不是 Insta360 X5。\n";
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    auto camera = std::make_shared<ins_camera::Camera>(device.info);
    if (!camera->Open()) {
        std::cerr << "打开相机失败。\n";
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    // X5 必须进入直播预览模式并设置直播分辨率。
    if (!camera->SetVideoSubMode(
            ins_camera::SubVideoMode::VIDEO_LIVEVIEW)) {
        std::cerr << "切换 VIDEO_LIVEVIEW 模式失败。\n";
        camera->Close();
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    ins_camera::RecordParams record_params{};
    record_params.resolution = kPreviewResolution;
    if (!camera->SetVideoCaptureParams(
            record_params,
            ins_camera::CameraFunctionMode::FUNCTION_MODE_LIVE_STREAM)) {
        std::cerr << "设置直播参数失败。\n";
        camera->Close();
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    auto stitcher = std::make_shared<ins::RealTimeStitcher>();

    // 将 CameraSDK 的预览参数完整传给 MediaSDK。
    const auto preview = camera->GetPreviewParam();
    ins::CameraInfo camera_info{};
    camera_info.cameraName = preview.camera_name;
    camera_info.decode_type =
        static_cast<ins::VideoDecodeType>(preview.encode_type);
    camera_info.offset = preview.offset;
    camera_info.window_crop_info_.crop_offset_x =
        preview.crop_info.crop_offset_x;
    camera_info.window_crop_info_.crop_offset_y =
        preview.crop_info.crop_offset_y;
    camera_info.window_crop_info_.dst_width =
        preview.crop_info.dst_width;
    camera_info.window_crop_info_.dst_height =
        preview.crop_info.dst_height;
    camera_info.window_crop_info_.src_width =
        preview.crop_info.src_width;
    camera_info.window_crop_info_.src_height =
        preview.crop_info.src_height;
    camera_info.gyro_timestamp = preview.delay_timestamp;
    camera_info.sweep_timestamp = preview.sweep_time;

    stitcher->SetCameraInfo(camera_info);
    stitcher->SetStitchType(ins::STITCH_TYPE::DYNAMICSTITCH);
    stitcher->SetOutputSize(kOutputWidth, kOutputHeight);
    stitcher->EnableFlowState(true);

    std::mutex frame_mutex;
    cv::Mat latest_rgba;
    std::atomic<bool> running{true};
    std::atomic<bool> stitch_failed{false};
    std::atomic<bool> received_first_panorama{false};

    stitcher->SetStitchStateCallback(
        [&](int error, const char* message) {
            std::cerr << "拼接失败，error=" << error
                      << ", message="
                      << (message != nullptr ? message : "unknown")
                      << '\n';
            stitch_failed = true;
            running = false;
        });

    stitcher->SetStitchRealTimeDataCallback(
        [&](uint8_t* data[4],
            int linesize[4],
            int width,
            int height,
            int,
            int64_t) {
            if (data == nullptr || data[0] == nullptr ||
                width <= 0 || height <= 0) {
                return;
            }

            if (!received_first_panorama.exchange(true)) {
                std::cout << "MediaSDK 已输出首个全景帧。\n";
            }

            const int minimum_stride = width * 4;
            const int stride =
                linesize[0] > 0 ? linesize[0] : minimum_stride;
            if (stride < minimum_stride) {
                return;
            }

            // SDK 回调缓冲区只在回调期间有效，必须立即复制。
            cv::Mat rgba_view(
                height,
                width,
                CV_8UC4,
                data[0],
                static_cast<size_t>(stride));
            cv::Mat rgba_copy = rgba_view.clone();

            std::lock_guard<std::mutex> lock(frame_mutex);
            latest_rgba = rgba_copy;
        });

    auto stitch_delegate =
        std::make_shared<StitchStreamDelegate>(stitcher);
    std::shared_ptr<ins_camera::StreamDelegate> delegate = stitch_delegate;
    camera->SetStreamDelegate(delegate);

    ins_camera::LiveStreamParam stream_param{};
    stream_param.video_resolution = kPreviewResolution;
    stream_param.lrv_video_resulution =
        kPreviewResolution;
    stream_param.video_bitrate = 1024 * 1024 / 2;
    stream_param.enable_audio = false;
    stream_param.using_lrv = false;

    std::cout << "Starting CameraSDK live stream..." << std::endl;
    if (!camera->StartLiveStreaming(stream_param)) {
        std::cerr << "启动相机预览流失败。\n";
        camera->Close();
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }
    std::cout << "CameraSDK live stream started." << std::endl;

    // 官方顺序：先启动相机流，再启动实时拼接器。
    std::cout << "Starting MediaSDK stitcher..." << std::endl;
    stitcher->StartStitch();
    std::cout << "MediaSDK stitcher started." << std::endl;

    cv::namedWindow(kWindowName, cv::WINDOW_NORMAL);
    std::cout << "实时全景预览已启动，按 ESC 退出。\n";

    while (running) {
        cv::Mat rgba;
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            rgba = latest_rgba;
            latest_rgba.release();
        }

        if (!rgba.empty()) {
            cv::Mat bgr;
            cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);
            cv::imshow(kWindowName, bgr);
        }

        if (cv::waitKey(1) == 27 ||
            cv::getWindowProperty(kWindowName, cv::WND_PROP_VISIBLE) < 1) {
            running = false;
        }
    }

    // 先停止数据生产，再停止拼接器。
    camera->StopLiveStreaming();
    stitcher->CancelStitch();
    cv::destroyAllWindows();
    camera->Close();
    discovery.FreeDeviceDescriptors(devices);

    return stitch_failed.load() ? 1 : 0;
}
