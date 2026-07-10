#include <camera/camera.h>
#include <camera/device_discovery.h>
#include <ins_realtime_stitcher.h>

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

namespace {

constexpr auto kPreviewResolution =
    ins_camera::VideoResolution::RES_1920_960P30;
constexpr int kOutputWidth = 960;
constexpr int kOutputHeight = 480;
constexpr const char* kWindowName = "Insta360 X5 Preview";
constexpr const char* kTcpHost = "172.16.23.253";
constexpr uint16_t kTcpPort = 5001;
constexpr int kJpegQuality = 85;
constexpr int kTcpSendFps = 5;

uint64_t NowMicros() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

#ifdef _WIN32
uint64_t HostToNetwork64(uint64_t value) {
    static const uint16_t endian_test = 1;
    if (*reinterpret_cast<const uint8_t*>(&endian_test) == 0) {
        return value;
    }

    return (static_cast<uint64_t>(htonl(static_cast<uint32_t>(value))) << 32) |
           htonl(static_cast<uint32_t>(value >> 32));
}

void AppendU16(std::vector<uint8_t>& out, uint16_t value) {
    value = htons(value);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    value = htonl(value);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
    value = HostToNetwork64(value);
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

std::vector<uint8_t> BuildFrameHeader(
    uint64_t frame_id,
    uint64_t timestamp_us,
    uint32_t width,
    uint32_t height,
    uint32_t payload_len) {
    std::vector<uint8_t> header;
    header.reserve(36);

    header.insert(header.end(), {'P', 'A', 'N', 'O'});
    AppendU16(header, 1);
    AppendU16(header, 1);
    AppendU64(header, frame_id);
    AppendU64(header, timestamp_us);
    AppendU32(header, width);
    AppendU32(header, height);
    AppendU32(header, payload_len);

    return header;
}

bool SendAll(SOCKET socket_handle, const uint8_t* data, size_t size) {
    size_t sent = 0;

    while (sent < size) {
        const int chunk = send(
            socket_handle,
            reinterpret_cast<const char*>(data + sent),
            static_cast<int>(size - sent),
            0);
        if (chunk <= 0) {
            return false;
        }

        sent += static_cast<size_t>(chunk);
    }

    return true;
}

SOCKET ConnectTcpServer() {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kTcpPort);

    if (inet_pton(AF_INET, kTcpHost, &addr.sin_addr) != 1) {
        closesocket(socket_handle);
        return INVALID_SOCKET;
    }

    if (connect(
            socket_handle,
            reinterpret_cast<sockaddr*>(&addr),
            sizeof(addr)) != 0) {
        closesocket(socket_handle);
        return INVALID_SOCKET;
    }

    BOOL no_delay = TRUE;
    setsockopt(
        socket_handle,
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&no_delay),
        sizeof(no_delay));

    return socket_handle;
}
#endif

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
    std::mutex send_mutex;
    std::condition_variable send_cv;
    std::vector<uint8_t> pending_jpeg;
    uint64_t pending_frame_id = 0;
    uint64_t pending_timestamp_us = 0;
    int pending_width = 0;
    int pending_height = 0;
    std::atomic<bool> sender_running{true};

#ifdef _WIN32
    auto sender_worker = [&]() {
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            std::cerr << "WSAStartup failed.\n";
            return;
        }

        uint64_t last_sent_frame_id = 0;

        while (sender_running) {
            SOCKET socket_handle = ConnectTcpServer();
            if (socket_handle == INVALID_SOCKET) {
                std::cerr << "TCP connect failed, retrying "
                          << kTcpHost << ':' << kTcpPort << "...\n";
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            std::cout << "TCP connected to "
                      << kTcpHost << ':' << kTcpPort << ".\n";

            while (sender_running) {
                std::vector<uint8_t> jpeg;
                uint64_t frame_id = 0;
                uint64_t timestamp_us = 0;
                int width = 0;
                int height = 0;

                {
                    std::unique_lock<std::mutex> lock(send_mutex);
                    send_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(500),
                        [&]() {
                            return !sender_running ||
                                   pending_frame_id != last_sent_frame_id;
                        });

                    if (!sender_running) {
                        break;
                    }

                    if (pending_frame_id == last_sent_frame_id ||
                        pending_jpeg.empty()) {
                        continue;
                    }

                    jpeg = pending_jpeg;
                    frame_id = pending_frame_id;
                    timestamp_us = pending_timestamp_us;
                    width = pending_width;
                    height = pending_height;
                    last_sent_frame_id = frame_id;
                }

                const auto header = BuildFrameHeader(
                    frame_id,
                    timestamp_us,
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height),
                    static_cast<uint32_t>(jpeg.size()));

                if (!SendAll(socket_handle, header.data(), header.size()) ||
                    !SendAll(socket_handle, jpeg.data(), jpeg.size())) {
                    std::cerr << "TCP send failed, reconnecting...\n";
                    break;
                }
            }

            closesocket(socket_handle);
        }

        WSACleanup();
    };
    std::thread sender_thread;
#endif

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

#ifdef _WIN32
    sender_thread = std::thread(sender_worker);
#endif

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

            static uint64_t frame_id = 0;
            static auto last_send_time = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const auto interval =
                std::chrono::milliseconds(1000 / kTcpSendFps);

            if (now - last_send_time >= interval) {
                last_send_time = now;

                std::vector<int> encode_params = {
                    cv::IMWRITE_JPEG_QUALITY,
                    kJpegQuality,
                };
                std::vector<uint8_t> jpeg;
                if (cv::imencode(".jpg", bgr, jpeg, encode_params)) {
                    std::lock_guard<std::mutex> lock(send_mutex);
                    pending_jpeg = std::move(jpeg);
                    pending_frame_id = ++frame_id;
                    pending_timestamp_us = NowMicros();
                    pending_width = bgr.cols;
                    pending_height = bgr.rows;
                    send_cv.notify_one();
                }
            }
        }

        if (cv::waitKey(1) == 27 ||
            cv::getWindowProperty(kWindowName, cv::WND_PROP_VISIBLE) < 1) {
            running = false;
        }
    }

    // 先停止数据生产，再停止拼接器。
    sender_running = false;
    send_cv.notify_all();
#ifdef _WIN32
    if (sender_thread.joinable()) {
        sender_thread.join();
    }
#endif
    camera->StopLiveStreaming();
    stitcher->CancelStitch();
    cv::destroyAllWindows();
    camera->Close();
    discovery.FreeDeviceDescriptors(devices);

    return stitch_failed.load() ? 1 : 0;
}
