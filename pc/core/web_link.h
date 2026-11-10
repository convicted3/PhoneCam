// PhoneCam "Telegram / browser" mode: the phone runs a web page (Telegram Mini App or any browser) and sends
// its camera over WebRTC. Pairing and the SDP exchange go through public MQTT brokers, end-to-end encrypted with
// a key derived from the pairing code. Video then flows directly phone -> PC (LAN or STUN-punched).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace phonecam {

// 8 characters, shown as XXXX-XXXX. Alphabet without 0/O/1/I.
std::string NewPairCode();
std::string NormalizePairCode(const std::string& in);  // strips '-', spaces, uppercases; "" if invalid

struct WebFrame {
    std::vector<uint8_t> annexb;  // one access unit, Annex-B start codes
    int64_t pts_us = 0;
    bool key = false;             // contains an IDR slice
};

struct WebLinkConfig {
    std::string code;              // pairing code (normalized)
    int width = 1280, height = 720, fps = 30, kbps = 6000;
    std::string camera = "back";   // back | front | wide
    std::vector<std::string> brokers;  // "host:port" (TCP MQTT); empty = built-in list
};

class WebLink {
public:
    enum class Event { BrokerUp, BrokerDown, PhoneOffer, Connected, Disconnected, Failed };
    struct Callbacks {
        std::function<void(Event, const std::string&)> on_event;   // any thread
        std::function<void(WebFrame&&)> on_frame;                    // libdatachannel thread
        std::function<void(const std::string&)> on_line;             // control line from the phone (datachannel)
    };

    WebLink();
    ~WebLink();
    bool Start(const WebLinkConfig& cfg, Callbacks cb, std::string& err);
    void Stop();

    void SendLine(const std::string& line);  // to the phone over the data channel
    void RequestKeyframe();                  // RTCP PLI
    void SetConfig(int w, int h, int fps, int kbps, const std::string& camera);  // sends CFG to the phone
    int BrokersUp() const;

    struct Stats {
        uint64_t packets = 0, bytes = 0, lost = 0, nacks = 0, plis = 0, frames = 0, dropped_frames = 0;
    };
    Stats TakeStats();  // since the previous call

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

// Where the phone page lives and the Telegram bot that opens it.
extern const char* kPhoneCamWebPage;  // https://.../
extern const char* kPhoneCamBot;      // bot username without @, "" = not set
// What the pairing QR code holds: t.me/<bot>?start=<code> (the bot replies with the button), or the web page with #<code>.
std::string PairLink(const std::string& code, const std::string& bot = kPhoneCamBot);

// Crypto helpers (exposed for tests)
std::string PairTopic(const std::string& code);  // MQTT topic prefix for a pairing code
bool SealMessage(const std::string& code, const std::string& plain, std::string& out);
bool OpenMessage(const std::string& code, const std::string& sealed, std::string& out);

}  // namespace phonecam
