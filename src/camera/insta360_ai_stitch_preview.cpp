#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <camera/photography_settings.h>
#include <ins_realtime_stitcher.h>
#include <ins_stitcher.h>

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct AppConfig {
    int seconds = 30;
    int output_width = 1920;
    int output_height = 960;
    bool show_preview = true;
    bool save_mp4 = true;
    bool save_snapshot = true;
};

struct SharedFrame {
    cv::Mat rgba;
    int64_t timestamp = 0;
    bool has_frame = false;
    bool stop = false;
    std::mutex mutex;
    std::condition_variable cv;
};

static AppConfig ParseArgs(int argc, char* argv[]) {
    AppConfig config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            config.seconds = std::stoi(argv[++i]);
        } else if (arg == "--output-size" && i + 1 < argc) {
            const std::string size = argv[++i];
            const auto x_pos = size.find('x');
            if (x_pos != std::string::npos) {
                config.output_width = std::stoi(size.substr(0, x_pos));
                config.output_height = std::stoi(size.substr(x_pos + 1));
            }
        } else if (arg == "--no-preview") {
            config.show_preview = false;
        } else if (arg == "--no-mp4") {
            config.save_mp4 = false;
        } else if (arg == "--no-snapshot") {
            config.save_snapshot = false;
        }
    }

    return config;
}

static bool IsX4OrNewer(ins_camera::CameraType camera_type) {
    return camera_type == ins_camera::CameraType::Insta360X4 ||
        camera_type == ins_camera::CameraType::Insta360X4Air ||
        camera_type == ins_camera::CameraType::Insta360X5;
}

static const char* EncodeName(ins_camera::VideoEncodeType type) {
    return type == ins_camera::VideoEncodeType::H265 ? "H.265" : "H.264";
}

class AiStitchStreamDelegate final : public ins_camera::StreamDelegate {
public:
    explicit AiStitchStreamDelegate(std::shared_ptr<ins::RealTimeStitcher> stitcher)
        : stitcher_(std::move(stitcher)) {}

    void OnAudioData(const uint8_t* data, size_t size, int64_t timestamp) override {
        (void)data;
        (void)size;
        (void)timestamp;
    }

    void OnVideoData(const uint8_t* data, size_t size, int64_t timestamp, uint8_t stream_type, int stream_index) override {
        video_frames_++;
        stitcher_->HandleVideoData(data, size, timestamp, stream_type, stream_index);
    }

    void OnGyroData(const std::vector<ins_camera::GyroData>& data) override {
        // 关键：陀螺仪数据用于 FlowState 防抖和方向稳定，直接转给 MediaSDK。
        gyro_batches_++;
        std::vector<ins::GyroData> gyro_data(data.size());
        std::memcpy(gyro_data.data(), data.data(), data.size() * sizeof(ins_camera::GyroData));
        stitcher_->HandleGyroData(gyro_data);
    }

    void OnExposureData(const ins_camera::ExposureData& data) override {
        // 关键：曝光数据用于实时拼接时的画面一致性处理。
        exposure_frames_++;
        ins::ExposureData exposure{};
        exposure.exposure_time = data.exposure_time;
        exposure.timestamp = data.timestamp;
        stitcher_->HandleExposureData(exposure);
    }

    uint64_t video_frames() const { return video_frames_.load(); }
    uint64_t gyro_batches() const { return gyro_batches_.load(); }
    uint64_t exposure_frames() const { return exposure_frames_.load(); }

private:
    std::shared_ptr<ins::RealTimeStitcher> stitcher_;
    std::atomic<uint64_t> video_frames_{0};
    std::atomic<uint64_t> gyro_batches_{0};
    std::atomic<uint64_t> exposure_frames_{0};
};

static void FillCameraInfo(const ins_camera::PreviewParam& preview_param, ins::CameraInfo& camera_info) {
    camera_info.cameraName = preview_param.camera_name;
    camera_info.decode_type = static_cast<ins::VideoDecodeType>(preview_param.encode_type);
    camera_info.offset = preview_param.offset;
    camera_info.gyro_timestamp = preview_param.delay_timestamp;
    camera_info.sweep_timestamp = preview_param.sweep_time;

    camera_info.window_crop_info_.crop_offset_x = preview_param.crop_info.crop_offset_x;
    camera_info.window_crop_info_.crop_offset_y = preview_param.crop_info.crop_offset_y;
    camera_info.window_crop_info_.dst_width = preview_param.crop_info.dst_width;
    camera_info.window_crop_info_.dst_height = preview_param.crop_info.dst_height;
    camera_info.window_crop_info_.src_width = preview_param.crop_info.src_width;
    camera_info.window_crop_info_.src_height = preview_param.crop_info.src_height;
}

static bool ConfigureCameraLiveView(const std::shared_ptr<ins_camera::Camera>& camera, ins_camera::CameraType camera_type) {
    if (!IsX4OrNewer(camera_type)) {
        return true;
    }

    // 关键：X4/X4 Air/X5 开实时预览前需要切到 LIVEVIEW 视频子模式。
    if (!camera->SetVideoSubMode(ins_camera::SubVideoMode::VIDEO_LIVEVIEW)) {
        std::cerr << "Failed to switch camera to VIDEO_LIVEVIEW.\n";
        return false;
    }

    ins_camera::RecordParams params{};
    params.resolution = ins_camera::VideoResolution::RES_3840_1920P30;
    params.bitrate = 20 * 1000 * 1000;
    if (!camera->SetVideoCaptureParams(params, ins_camera::CameraFunctionMode::FUNCTION_MODE_LIVE_STREAM)) {
        std::cerr << "Failed to set live stream capture params.\n";
        return false;
    }

    return true;
}

static void ConsumeFrames(const AppConfig& config, SharedFrame& shared) {
    cv::VideoWriter writer;
    bool snapshot_written = false;
    const fs::path data_dir = fs::path("data") / "Insta";

    if (config.save_mp4) {
        const fs::path mp4_path = data_dir / "preview.mp4";
        const int fourcc = cv::VideoWriter::fourcc('m', 'p', '4', 'v');
        writer.open(mp4_path.string(), fourcc, 30.0, cv::Size(config.output_width, config.output_height));
        if (!writer.isOpened()) {
            std::cerr << "Failed to open mp4 writer: " << mp4_path.string() << "\n";
        }
    }

    if (config.show_preview) {
        cv::namedWindow("Insta360 AI Stitch Preview", cv::WINDOW_NORMAL);
    }

    while (true) {
        cv::Mat rgba;
        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            shared.cv.wait(lock, [&] {
                return shared.stop || shared.has_frame;
            });

            if (shared.stop && !shared.has_frame) {
                break;
            }

            rgba = shared.rgba.clone();
            shared.has_frame = false;
        }

        if (rgba.empty()) {
            continue;
        }

        cv::Mat bgr;
        cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);

        if (writer.isOpened()) {
            writer.write(bgr);
        }

        if (config.save_snapshot && !snapshot_written) {
            cv::imwrite((data_dir / "snapshot.jpg").string(), bgr);
            snapshot_written = true;
        }

        if (config.show_preview) {
            cv::imshow("Insta360 AI Stitch Preview", bgr);
            if (cv::waitKey(1) == 27) {
                std::lock_guard<std::mutex> lock(shared.mutex);
                shared.stop = true;
                shared.cv.notify_all();
            }
        }
    }

    if (writer.isOpened()) {
        writer.release();
    }
    if (config.show_preview) {
        cv::destroyWindow("Insta360 AI Stitch Preview");
    }
}

int main(int argc, char* argv[]) {
    const AppConfig config = ParseArgs(argc, argv);
    const fs::path data_dir = fs::path("data") / "Insta";
    const fs::path log_dir = data_dir / "logs";

    fs::create_directories(data_dir);
    fs::create_directories(log_dir);

    ins_camera::SetLogLevel(ins_camera::LogLevel::INFO);
    ins_camera::SetLogPath((log_dir / "camera_sdk.log").string());
    ins::SetLogLevel(ins::InsLogLevel::INFO);
    ins::SetLogPath((log_dir / "media_sdk.log").string());

    // 关键：实时拼接使用 MediaSDK 初始化和模型目录；路径保持相对路径。
    ins::InitEnv();
    ins::SetModelFileRootDir((fs::path("libs") / "MediaSDK" / "models").string());

    ins_camera::DeviceDiscovery discovery;
    auto devices = discovery.GetAvailableDevices();
    if (devices.empty()) {
        std::cerr << "No camera found. Check USB Android mode and libusbK driver.\n";
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    const auto camera_desc = devices.front();
    std::cout << "Camera: " << camera_desc.camera_name
              << ", serial: " << camera_desc.serial_number
              << ", firmware: " << camera_desc.fw_version << "\n";

    auto camera = std::make_shared<ins_camera::Camera>(camera_desc.info);
    camera->SetServicePort(9099);

    if (!camera->Open()) {
        std::cerr << "Failed to open camera.\n";
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    if (!ConfigureCameraLiveView(camera, camera_desc.camera_type)) {
        camera->Close();
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    const auto preview_param = camera->GetPreviewParam();
    std::cout << "Preview encode: " << EncodeName(preview_param.encode_type) << "\n";

    auto stitcher = std::make_shared<ins::RealTimeStitcher>();

    ins::CameraInfo camera_info{};
    FillCameraInfo(preview_param, camera_info);
    stitcher->SetCameraInfo(camera_info);

    // 关键：AI 拼接算法。性能压力大时可改为 OPTFLOW/DYNAMICSTITCH 做对照。
    stitcher->SetStitchType(ins::STITCH_TYPE::AIFLOW);
    stitcher->SetOutputSize(config.output_width, config.output_height);
    stitcher->EnableFlowState(true);
    stitcher->EnableDirectionLock(false);
    stitcher->EnableDeflicker(false);
    stitcher->EnableDefringe(false);

    SharedFrame shared;
    stitcher->SetStitchStateCallback([](int error, const char* err_info) {
        std::cerr << "MediaSDK stitch error " << error << ": " << err_info << "\n";
    });
    stitcher->SetStitchRealTimeDataCallback([&](uint8_t* data[4], int linesize[4], int width, int height, int format, int64_t timestamp) {
        (void)linesize;
        (void)format;

        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.rgba = cv::Mat(height, width, CV_8UC4, data[0]).clone();
        shared.timestamp = timestamp;
        shared.has_frame = true;
        shared.cv.notify_one();
    });

    std::shared_ptr<ins_camera::StreamDelegate> delegate = std::make_shared<AiStitchStreamDelegate>(stitcher);
    camera->SetStreamDelegate(delegate);

    std::thread consumer([&] {
        ConsumeFrames(config, shared);
    });

    ins_camera::LiveStreamParam stream_param{};
    stream_param.video_resolution = ins_camera::VideoResolution::RES_1920_960P30;
    stream_param.lrv_video_resulution = ins_camera::VideoResolution::RES_1920_960P30;
    stream_param.video_bitrate = 8 * 1000 * 1000;
    stream_param.lrv_video_bitrate = 1 * 1000 * 1000;
    stream_param.enable_audio = false;
    stream_param.enable_video = true;
    stream_param.enable_gyro = true;
    stream_param.using_lrv = false;

    if (!camera->StartLiveStreaming(stream_param)) {
        std::cerr << "Failed to start live streaming.\n";
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            shared.stop = true;
            shared.cv.notify_all();
        }
        consumer.join();
        camera->Close();
        discovery.FreeDeviceDescriptors(devices);
        return 1;
    }

    stitcher->StartStitch();
    std::cout << "Streaming AI stitched panorama for " << config.seconds << " seconds.\n";
    std::cout << "Outputs: data/Insta/preview.mp4, data/Insta/snapshot.jpg, data/Insta/logs/\n";

    const auto deadline = Clock::now() + std::chrono::seconds(config.seconds);
    while (Clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(shared.mutex);
            if (shared.stop) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stop = true;
        shared.cv.notify_all();
    }

    if (consumer.joinable()) {
        consumer.join();
    }

    camera->StopLiveStreaming();
    stitcher->CancelStitch();
    camera->Close();
    discovery.FreeDeviceDescriptors(devices);

    auto* stats = dynamic_cast<AiStitchStreamDelegate*>(delegate.get());
    if (stats != nullptr) {
        std::cout << "Video frames: " << stats->video_frames()
                  << ", gyro batches: " << stats->gyro_batches()
                  << ", exposure frames: " << stats->exposure_frames() << "\n";
    }

    return 0;
}
