// PhoneCam PC core: connects to the phone, decodes H.264 (Media Foundation), hands out NV12 frames.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace phonecam {

constexpr int kTcpPort = 5757;
constexpr int kUdpPort = 5758;

struct Frame {
    int width = 0, height = 0;
    const uint8_t* y = nullptr;   // NV12 luma
    const uint8_t* uv = nullptr;  // NV12 interleaved chroma
    int y_stride = 0, uv_stride = 0;
};

enum class State { Idle, Connecting, Streaming, Error };

struct Config {
    std::string host = "192.168.0.100";
    int port = kTcpPort;
    bool usb = false;          // use "adb forward" and connect to 127.0.0.1
    int width = 1280, height = 720, fps = 30, kbps = 6000;
    std::string camera = "back";  // back | front | <id>
    bool mirror = false;
    bool rotate180 = false;    // extra rotation on top of the phone-reported one
    bool auto_reconnect = true;
    bool high_speed = false;
    // Telegram / browser mode (WebRTC from the phone page). web_code = pairing code; web_link = what the QR holds.
    bool web = false;
    std::string web_code;
    bool web_qr_frames = false;  // while waiting, output a picture with the QR code (OBS)
    std::string web_link;
    std::vector<std::string> init_commands;  // control lines sent right after connecting (camera settings)   // experimental: ask the phone for its 120 fps high-speed mode to reach 60 fps
};

struct FoundPhone {
    std::string ip;
    int port = kTcpPort;
    std::string name;
};

// Broadcast a discovery request on every IPv4 interface and collect answers.
std::vector<FoundPhone> Discover(int timeout_ms = 1200);

// What one phone camera can do at one of the PC's resolutions.
struct CamCaps {
    std::string side;        // back | front
    int req_w = 0, req_h = 0; // resolution the PC asked about
    int w = 0, h = 0;         // what the phone would actually use
    int max_fps = 30;         // normal mode limit
    int hs_fps = 0;           // 60 if 60 fps works through the high-speed mode
};

// Ask the phone app what its cameras support (short separate connection, works while streaming).
bool QueryCaps(const std::string& host, int port, bool usb, std::vector<CamCaps>& out, std::string& err);
const CamCaps* FindCaps(const std::vector<CamCaps>& caps, const std::string& side, int w, int h);

// Append a line to %APPDATA%\\PhoneCam\\log.txt (UTF-8).
void Log(const std::string& line);

// Run "adb forward tcp:5757 tcp:5757". Returns true on success, err has a human readable message.
bool AdbForward(int port, std::string& err);

// Picture adjustments done on the PC (work for any phone).
struct ImageAdjust {
    int brightness = 0;   // -100..100
    int contrast = 0;     // -100..100
    int saturation = 0;   // -100..100
    int gamma = 100;      // 50..200 (100 = neutral)
    int sharpness = 0;    // 0..100
    int temperature = 0;  // -100 cold .. 100 warm
    bool Active() const { return brightness || contrast || saturation || gamma != 100 || sharpness || temperature; }
};

class Client {
public:
    using FrameCallback = std::function<void(const Frame&)>;
    using StatusCallback = std::function<void(State, const std::string&)>;

    Client();
    ~Client();

    void Start(const Config& cfg, FrameCallback on_frame, StatusCallback on_status);
    void Stop();
    bool Running() const { return running_; }

    // Change mirror/rotation live without reconnecting.
    void SetMirror(bool m) { mirror_ = m; }
    void SetRotate180(bool r) { user_rot180_ = r; }
    void SetAdjust(const ImageAdjust& a);
    // Called (on the network thread) when the phone's own screen changes torch ("torch", 0/1) or zoom ("zoom", x).
    void SetControlCallback(std::function<void(const std::string&, float)> cb) { on_control_ = std::move(cb); }
    // Exposure in EV. Done by the phone's camera; if the phone reports that its camera ignores it,
    // the PC applies it digitally instead.
    void SetExposureEv(float ev);
    bool ExposureOnPc() const { return !ev_hw_; }
    // Auto brightness: lifts dark frames towards normal brightness (for 60 FPS mode in dim light).
    void SetAutoBrightness(bool on);

    // Control lines understood by the phone: "TORCH 1", "ZOOM 2.0", "AF", "KEYFRAME".
    void SendCommand(const std::string& line);

private:
    void Run();
    bool Session(std::string& err);
    bool SessionWeb(std::string& err);
    void HandleInfo(const std::string& sp);
    void EmitQrFrame();
    void Deliver(const Frame& src);

    Config cfg_;
    FrameCallback on_frame_;
    StatusCallback on_status_;
    std::function<void(const std::string&, float)> on_control_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> mirror_{false};
    std::atomic<bool> user_rot180_{false};
    std::atomic<int> phone_rotation_{0};
    std::mutex sock_mutex_;
    uintptr_t sock_ = ~uintptr_t(0);
    std::vector<uint8_t> work_;
    void Emit(const Frame& f);
    std::mutex adj_mu_;
    ImageAdjust adj_;
    bool adj_active_ = false;
    float digital_ev_ = 0;
    std::atomic<bool> ev_hw_{true};
    void RebuildLuts();
    bool auto_bri_ = false;
    double auto_k_ = 1.0;        // smart brightness gain, 1 = off
    uint16_t csat_[256] = {0};   // chroma boost per luma value (x256) that goes with the brightness gain
    double auto_median_ = 0.3;
    std::unique_ptr<class NightFilter> night_;   // low-light noise reduction + local tone mapping
    double auto_k_built_ = 1.0;
    bool auto_awake_ = false;
    uint8_t ylut_[256], ulut_[256], vlut_[256];
    std::vector<uint8_t> adj_buf_, adj_tmp_;
    std::mutex web_mu_;
    class WebLink* web_ = nullptr;
    std::vector<uint8_t> qr_frame_;
};

// NV12 -> BGR24 (top-down) with nearest scaling. dst must hold dw*dh*3 bytes.
void Nv12ToBgr24(const Frame& f, uint8_t* dst, int dw, int dh, bool bt709);
// NV12 -> BGRA (top-down) with nearest scaling, dst stride = dw*4.
void Nv12ToBgra(const Frame& f, uint8_t* dst, int dw, int dh, bool bt709);

}  // namespace phonecam
