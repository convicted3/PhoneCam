// See web_link.h. Portable (Windows + POSIX) so it can be tested on Linux against a real browser.
#include "web_link.h"

#include <rtc/rtc.hpp>

#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
static const sock_t kNoSock = INVALID_SOCKET;
static void CloseSock(sock_t s) { closesocket(s); }
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
static const sock_t kNoSock = -1;
static void CloseSock(sock_t s) { close(s); }
#endif

namespace phonecam {

void Log(const std::string& line);

namespace {

const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::mt19937_64& Rng() {
    static std::mt19937_64 r([] {
        std::random_device rd;
        uint64_t s = ((uint64_t)rd() << 32) ^ rd();
        return s ^ (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }());
    return r;
}
std::mutex g_rng_mu;
void RandomBytes(uint8_t* p, size_t n) {
    std::random_device rd;  // OS entropy where available
    std::lock_guard<std::mutex> lk(g_rng_mu);
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(rd() ^ Rng()());
}

void Sha256(const std::string& s, uint8_t out[32]) { mbedtls_sha256((const unsigned char*)s.data(), s.size(), out, 0); }

std::string Hex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

// ------------------------------------------------------------------ minimal MQTT 3.1.1 client (QoS 0)
class Mqtt {
public:
    using MsgFn = std::function<void(int idx, const std::string& topic, const std::string& payload)>;
    Mqtt(int idx, std::string host, int port, std::string sub, MsgFn on_msg, std::function<void(int, bool)> on_state)
        : idx_(idx), host_(std::move(host)), port_(port), sub_(std::move(sub)), on_msg_(std::move(on_msg)),
          on_state_(std::move(on_state)) {
        th_ = std::thread([this] { Run(); });
    }
    ~Mqtt() {
        stop_ = true;
        {
            std::lock_guard<std::mutex> lk(wmu_);
            if (s_ != kNoSock) shutdown(s_, 2);
        }
        if (th_.joinable()) th_.join();
    }
    bool Up() const { return up_; }
    bool Publish(const std::string& topic, const std::string& payload) {
        std::string vh;
        PutStr(vh, topic);
        std::string pkt;
        pkt += (char)0x30;
        PutLen(pkt, vh.size() + payload.size());
        pkt += vh;
        pkt += payload;
        return Send(pkt);
    }
    std::string Name() const { return host_ + ":" + std::to_string(port_); }

private:
    static void PutStr(std::string& o, const std::string& s) {
        o += (char)(s.size() >> 8);
        o += (char)(s.size() & 255);
        o += s;
    }
    static void PutLen(std::string& o, size_t n) {
        do {
            uint8_t b = n % 128;
            n /= 128;
            if (n) b |= 128;
            o += (char)b;
        } while (n);
    }
    bool Send(const std::string& d) {
        std::lock_guard<std::mutex> lk(wmu_);
        if (s_ == kNoSock) return false;
        size_t off = 0;
        while (off < d.size()) {
            int n = send(s_, d.data() + off, (int)(d.size() - off), 0);
            if (n <= 0) return false;
            off += n;
        }
        return true;
    }
    // read exactly n bytes; false on error/stop. Timeouts are used to send pings.
    bool ReadN(uint8_t* p, size_t n) {
        while (n > 0) {
            if (stop_) return false;
            int r = recv(s_, (char*)p, (int)n, 0);
            if (r > 0) { p += r; n -= r; continue; }
            if (r == 0) return false;
#ifdef _WIN32
            if (WSAGetLastError() != WSAETIMEDOUT) return false;
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
#endif
            if (NowMs() - last_ping_ > 20000) {
                last_ping_ = NowMs();
                Send(std::string("\xC0\x00", 2));
            }
            if (NowMs() - last_rx_ > 60000) return false;  // broker gone quiet
        }
        return true;
    }
    bool ReadPacket(uint8_t& type, std::string& body) {
        uint8_t h;
        if (!ReadN(&h, 1)) return false;
        size_t len = 0, mul = 1;
        for (int i = 0; i < 4; i++) {
            uint8_t b;
            if (!ReadN(&b, 1)) return false;
            len += (b & 127) * mul;
            mul *= 128;
            if (!(b & 128)) break;
        }
        if (len > 4 * 1024 * 1024) return false;
        body.resize(len);
        if (len && !ReadN((uint8_t*)&body[0], len)) return false;
        type = h;
        last_rx_ = NowMs();
        return true;
    }
    sock_t Connect() {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res) != 0 || !res) return kNoSock;
        sock_t s = kNoSock;
        for (addrinfo* a = res; a && s == kNoSock; a = a->ai_next) {
            s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSock) continue;
            SetTimeout(s, 6000);
            if (connect(s, a->ai_addr, (int)a->ai_addrlen) != 0) { CloseSock(s); s = kNoSock; }
        }
        freeaddrinfo(res);
        return s;
    }
    static void SetTimeout(sock_t s, int ms) {
#ifdef _WIN32
        DWORD t = ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
#else
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    }
    void Run() {
        int backoff = 2000;
        while (!stop_) {
            sock_t s = Connect();
            if (s != kNoSock) {
                {
                    std::lock_guard<std::mutex> lk(wmu_);
                    s_ = s;
                }
                if (Session()) backoff = 2000;
                {
                    std::lock_guard<std::mutex> lk(wmu_);
                    CloseSock(s_);
                    s_ = kNoSock;
                }
                if (up_) {
                    up_ = false;
                    on_state_(idx_, false);
                }
            }
            for (int i = 0; i < backoff / 100 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff = std::min(backoff * 2, 30000);
        }
    }
    bool Session() {
        uint8_t rnd[6];
        RandomBytes(rnd, 6);
        std::string vh;
        PutStr(vh, "MQTT");
        vh += (char)4;     // protocol level 3.1.1
        vh += (char)0x02;  // clean session
        vh += (char)0;
        vh += (char)60;    // keep alive 60 s
        PutStr(vh, "phcpc-" + Hex(rnd, 6));
        std::string pkt;
        pkt += (char)0x10;
        PutLen(pkt, vh.size());
        pkt += vh;
        last_rx_ = last_ping_ = NowMs();
        if (!Send(pkt)) return false;
        uint8_t t;
        std::string body;
        if (!ReadPacket(t, body) || (t & 0xF0) != 0x20 || body.size() < 2 || body[1] != 0) return false;
        // subscribe
        std::string sv;
        sv += (char)0;
        sv += (char)1;  // packet id
        PutStr(sv, sub_);
        sv += (char)0;  // QoS 0
        std::string sp;
        sp += (char)0x82;
        PutLen(sp, sv.size());
        sp += sv;
        if (!Send(sp)) return false;
        SetTimeout(s_, 1000);
        bool ok = false;
        while (!stop_) {
            if (!ReadPacket(t, body)) return ok;
            switch (t & 0xF0) {
            case 0x90:  // SUBACK
                ok = true;
                up_ = true;
                on_state_(idx_, true);
                break;
            case 0x30: {  // PUBLISH
                if (body.size() < 2) break;
                size_t tl = (uint8_t)body[0] << 8 | (uint8_t)body[1];
                size_t off = 2 + tl;
                if ((t >> 1) & 3) off += 2;  // QoS > 0 has a packet id (we only subscribe with QoS 0 though)
                if (off > body.size()) break;
                on_msg_(idx_, body.substr(2, tl), body.substr(off));
                break;
            }
            default: break;  // PINGRESP etc.
            }
        }
        return ok;
    }

    int idx_;
    std::string host_;
    int port_;
    std::string sub_;
    MsgFn on_msg_;
    std::function<void(int, bool)> on_state_;
    std::thread th_;
    std::mutex wmu_;
    sock_t s_ = kNoSock;
    std::atomic<bool> stop_{false}, up_{false};
    int64_t last_rx_ = 0, last_ping_ = 0;
};

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Adjust libdatachannel's answer so browsers send at a good bitrate:
//  - no transport-cc (we don't send transport feedback; REMB is used instead)
//  - no video orientation extension (the phone then rotates the picture itself)
//  - start/min/max bitrate hints and b=AS
std::string MungeAnswer(const std::string& sdp, int kbps) {
    std::vector<std::string> in = SplitLines(sdp), out;
    std::vector<std::string> h264;
    for (auto& l : in) {
        auto p = l.find(" H264/90000");
        if (l.rfind("a=rtpmap:", 0) == 0 && p != std::string::npos) h264.push_back(l.substr(9, p - 9));
    }
    bool in_video = false;
    for (size_t i = 0; i < in.size(); i++) {
        const std::string& l = in[i];
        if (l.rfind("m=", 0) == 0) in_video = l.rfind("m=video", 0) == 0;
        if (in_video) {
            if (l.find("transport-cc") != std::string::npos || l.find("transport-wide-cc") != std::string::npos ||
                l.find("urn:3gpp:video-orientation") != std::string::npos)
                continue;
            if (l.rfind("b=", 0) == 0) continue;
        }
        if (in_video && l.rfind("a=fmtp:", 0) == 0) {
            std::string pt = l.substr(7, l.find(' ') - 7);
            if (std::find(h264.begin(), h264.end(), pt) != h264.end()) {
                char b[160];
                snprintf(b, sizeof b, ";x-google-start-bitrate=%d;x-google-min-bitrate=%d;x-google-max-bitrate=%d",
                         kbps * 6 / 10, std::max(300, kbps / 4), kbps);
                out.push_back(l + b);
                continue;
            }
        }
        out.push_back(l);
        if (in_video && l.rfind("c=", 0) == 0) out.push_back("b=AS:" + std::to_string(kbps));
        if (in_video && l.rfind("a=rtpmap:", 0) == 0) {
            std::string pt = l.substr(9, l.find(' ') - 9);
            if (std::find(h264.begin(), h264.end(), pt) != h264.end()) {
                for (const char* fb : {"goog-remb", "nack", "nack pli", "ccm fir"}) {
                    std::string want = "a=rtcp-fb:" + pt + " " + fb;
                    bool have = std::find(in.begin(), in.end(), want) != in.end();
                    if (!have) out.push_back(want);
                }
            }
        }
    }
    std::string r;
    for (auto& l : out) r += l + "\r\n";
    return r;
}

}  // namespace

// ------------------------------------------------------------------ public helpers
const char* kPhoneCamWebPage = "https://phonecam.phonecam.workers.dev/";
const char* kPhoneCamBot = "phonecamweb_bot";

std::string PairLink(const std::string& code, const std::string& bot_in) {
    std::string bot = bot_in;
    if (!bot.empty() && bot[0] == '@') bot.erase(0, 1);
    if (!bot.empty()) return "https://t.me/" + bot + "?start=" + code;  // bot answers /start CODE with the button
    return std::string(kPhoneCamWebPage) + "#" + code;
}

std::string NewPairCode() {
    uint8_t b[8];
    RandomBytes(b, 8);
    std::string s;
    for (int i = 0; i < 8; i++) s += kAlphabet[b[i] & 31];
    return s;
}

std::string NormalizePairCode(const std::string& in) {
    std::string s;
    for (char c : in) {
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!strchr(kAlphabet, c)) return "";
        s += c;
    }
    return s.size() == 8 ? s : "";
}

std::string PairTopic(const std::string& code) {
    uint8_t h[32];
    Sha256("phonecam-topic:" + code, h);
    return "phonecam/v1/" + Hex(h, 10);
}

bool SealMessage(const std::string& code, const std::string& plain, std::string& out) {
    uint8_t key[32], iv[12], tag[16];
    Sha256("phonecam-key:" + code, key);
    RandomBytes(iv, 12);
    std::string ct(plain.size(), '\0');
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!rc)
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plain.size(), iv, 12, nullptr, 0,
                                       (const unsigned char*)plain.data(), (unsigned char*)&ct[0], 16, tag);
    mbedtls_gcm_free(&g);
    if (rc) return false;
    out.assign((const char*)iv, 12);
    out += ct;
    out.append((const char*)tag, 16);
    return true;
}

bool OpenMessage(const std::string& code, const std::string& sealed, std::string& out) {
    if (sealed.size() < 28) return false;
    uint8_t key[32];
    Sha256("phonecam-key:" + code, key);
    size_t n = sealed.size() - 28;
    out.assign(n, '\0');
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (!rc)
        rc = mbedtls_gcm_auth_decrypt(&g, n, (const unsigned char*)sealed.data(), 12, nullptr, 0,
                                      (const unsigned char*)sealed.data() + 12 + n, 16,
                                      (const unsigned char*)sealed.data() + 12, n ? (unsigned char*)&out[0] : nullptr);
    mbedtls_gcm_free(&g);
    return rc == 0;
}

// ------------------------------------------------------------------ implementation
struct WebLink::Impl {
    WebLinkConfig cfg;
    Callbacks cb;
    std::string topic;
    std::vector<std::unique_ptr<Mqtt>> brokers;
    std::atomic<bool> stopping{false};

    std::mutex pmu;  // peer objects
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dc;
    std::string sid;
    uint64_t gen = 0;

    // ---- RTP reassembly (libdatachannel thread)
    struct Pkt {
        uint32_t ts;
        bool marker;
        std::vector<uint8_t> payload;
    };
    std::mutex rmu;
    std::map<int64_t, Pkt> pkts;
    bool have_seq = false, synced = false, need_key = true;
    int64_t max_seq = 0, next_seq = 0;
    int64_t gap_since = 0;
    std::map<int64_t, std::pair<int, int64_t>> nacked;  // seq -> (tries, last time)
    uint32_t media_ssrc = 0;
    bool have_ts = false;
    uint32_t last_ts = 0;
    int64_t ts_ext = 0;
    int64_t last_pli = 0, last_remb = 0;
    Stats stats;
    std::mutex smu;

    void Event(WebLink::Event e, const std::string& s) {
        if (cb.on_event) cb.on_event(e, s);
    }

    void Publish(const std::string& plain) {
        std::string sealed;
        if (!SealMessage(cfg.code, plain, sealed)) return;
        for (auto& b : brokers)
            if (b->Up()) b->Publish(topic + "/ph", sealed);
    }

    void OnMqtt(int, const std::string&, const std::string& payload) {
        std::string msg;
        if (!OpenMessage(cfg.code, payload, msg)) return;  // not ours / wrong code
        size_t nl = msg.find('\n');
        std::string head = msg.substr(0, nl);
        std::string body = nl == std::string::npos ? "" : msg.substr(nl + 1);
        std::istringstream hs(head);
        std::string kind, s;
        hs >> kind >> s;
        if (kind == "OFFER" && !s.empty()) {
            {
                std::lock_guard<std::mutex> lk(pmu);
                if (s == sid && pc) {
                    // the phone repeats its offer until it gets an answer: resend the answer we already made
                    auto ld = pc->localDescription();
                    if (ld && pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
                        std::string ans = AnswerText(std::string(*ld));
                        Publish(ans);
                    }
                    return;
                }
            }
            Event(WebLink::Event::PhoneOffer, s);
            NewPeer(s, body);
        } else if (kind == "BYE") {
            std::lock_guard<std::mutex> lk(pmu);
            if (s == sid) ClosePeerLocked();
        }
    }

    std::string AnswerText(const std::string& sdp) {
        char h[200];
        snprintf(h, sizeof h, "ANSWER %s %d %d %d %d %s\n", sid.c_str(), cfg.width, cfg.height, cfg.fps, cfg.kbps,
                 cfg.camera.c_str());
        return h + MungeAnswer(sdp, cfg.kbps);
    }

    void ClosePeerLocked() {
        auto p = pc;
        pc.reset();
        track.reset();
        dc.reset();
        sid.clear();
        gen++;
        if (p) {
            // close outside of libdatachannel callbacks: detach a thread
            std::thread([p] { p->close(); }).detach();
        }
    }

    void NewPeer(const std::string& s, const std::string& offer_sdp) {
        std::unique_lock<std::mutex> lk(pmu);
        ClosePeerLocked();
        sid = s;
        uint64_t g = ++gen;
        {
            std::lock_guard<std::mutex> rl(rmu);
            pkts.clear();
            nacked.clear();
            have_seq = synced = false;
            need_key = true;
            have_ts = false;
            gap_since = 0;
            media_ssrc = 0;
        }
        rtc::Configuration c;
        c.iceServers.emplace_back("stun:stun.l.google.com:19302");
        c.iceServers.emplace_back("stun:stun.cloudflare.com:3478");
        auto p = std::make_shared<rtc::PeerConnection>(c);
        pc = p;
        lk.unlock();  // libdatachannel may call the callbacks below synchronously
        std::weak_ptr<rtc::PeerConnection> wp = p;
        p->onTrack([this, g](std::shared_ptr<rtc::Track> t) {
            // No media handler: we do the RTCP ourselves (PLI/NACK/REMB/RR), which the library then only encrypts.
            t->onMessage([this, g](rtc::binary b) { OnRtp(g, b); }, [](std::string) {});
            std::lock_guard<std::mutex> lk(pmu);
            if (g == gen) track = t;
        });
        p->onDataChannel([this, g](std::shared_ptr<rtc::DataChannel> d) {
            d->onMessage([](rtc::binary) {},
                         [this](std::string s) {
                             for (auto& l : SplitLines(s))
                                 if (!l.empty() && cb.on_line) cb.on_line(l);
                         });
            std::lock_guard<std::mutex> lk(pmu);
            if (g == gen) dc = d;
        });
        p->onStateChange([this, g](rtc::PeerConnection::State st) {
            {
                std::lock_guard<std::mutex> lk(pmu);
                if (g != gen) return;
            }
            Log("webrtc state " + std::to_string((int)st));
            if (st == rtc::PeerConnection::State::Connected) Event(WebLink::Event::Connected, "");
            else if (st == rtc::PeerConnection::State::Failed) Event(WebLink::Event::Failed, "");
            else if (st == rtc::PeerConnection::State::Disconnected || st == rtc::PeerConnection::State::Closed)
                Event(WebLink::Event::Disconnected, "");
        });
        p->onGatheringStateChange([this, g, wp](rtc::PeerConnection::GatheringState gs) {
            if (gs != rtc::PeerConnection::GatheringState::Complete) return;
            auto pp = wp.lock();
            if (!pp) return;
            auto ld = pp->localDescription();
            if (!ld) return;
            std::string ans;
            {
                std::lock_guard<std::mutex> lk(pmu);
                if (g != gen) return;
                ans = AnswerText(std::string(*ld));
            }
            Publish(ans);
        });
        try {
            p->setRemoteDescription(rtc::Description(offer_sdp, "offer"));
        } catch (const std::exception& e) {
            Log(std::string("bad offer: ") + e.what());
            Event(WebLink::Event::Failed, e.what());
        }
    }

    // ---------------- RTP
    void SendRtcp(const std::vector<uint8_t>& b) {
        std::shared_ptr<rtc::Track> t;
        {
            std::lock_guard<std::mutex> lk(pmu);
            t = track;
        }
        if (t && t->isOpen()) {
            try {
                t->send(reinterpret_cast<const std::byte*>(b.data()), b.size());
            } catch (...) {}
        }
    }
    static void Put32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v);
    }
    static void SetLen(std::vector<uint8_t>& b) {
        size_t words = b.size() / 4 - 1;
        b[2] = (uint8_t)(words >> 8);
        b[3] = (uint8_t)words;
    }
    void Pli(int64_t now) {
        if (now - last_pli < 300 || !media_ssrc) return;
        last_pli = now;
        std::vector<uint8_t> b = {0x81, 206, 0, 0};
        Put32(b, 1);
        Put32(b, media_ssrc);
        SetLen(b);
        SendRtcp(b);
        std::lock_guard<std::mutex> lk(smu);
        stats.plis++;
    }
    // receiver report + REMB (bandwidth the sender may use), once a second
    int64_t rr_expected_prior = 0, rr_received_prior = 0, rr_received = 0, rr_base = -1;
    void ReportAndRemb() {
        if (!media_ssrc) return;
        std::vector<uint8_t> b = {0x81, 201, 0, 0};
        Put32(b, 1);
        Put32(b, media_ssrc);
        int64_t expected = rr_base < 0 ? 0 : max_seq - rr_base + 1;
        int64_t exp_int = expected - rr_expected_prior, rec_int = rr_received - rr_received_prior;
        rr_expected_prior = expected;
        rr_received_prior = rr_received;
        int64_t lost_int = exp_int - rec_int;
        uint8_t fraction = exp_int <= 0 || lost_int <= 0 ? 0 : (uint8_t)std::min<int64_t>(255, (lost_int << 8) / exp_int);
        int64_t cum = std::max<int64_t>(0, std::min<int64_t>(0x7fffff, expected - rr_received));
        Put32(b, (uint32_t)fraction << 24 | (uint32_t)cum);
        Put32(b, (uint32_t)(max_seq - 131072));  // extended highest sequence number (our offset removed)
        Put32(b, 0);  // jitter
        Put32(b, 0);  // LSR
        Put32(b, 0);  // DLSR
        SetLen(b);
        // REMB
        uint64_t br = (uint64_t)cfg.kbps * 1000;
        int exp = 0;
        while (br > 0x3FFFF) { br >>= 1; exp++; }
        std::vector<uint8_t> r = {0x8F, 206, 0, 0};
        Put32(r, 1);
        Put32(r, 0);
        r.push_back('R'); r.push_back('E'); r.push_back('M'); r.push_back('B');
        r.push_back(1);
        r.push_back((uint8_t)(exp << 2 | (br >> 16)));
        r.push_back((uint8_t)(br >> 8));
        r.push_back((uint8_t)br);
        Put32(r, media_ssrc);
        SetLen(r);
        b.insert(b.end(), r.begin(), r.end());  // compound RTCP packet
        SendRtcp(b);
    }
    void SendNacks(const std::vector<int64_t>& seqs) {
        if (seqs.empty() || !media_ssrc) return;
        std::vector<uint8_t> b = {0x81, 205, 0, 0};
        Put32(b, 1);  // our SSRC
        Put32(b, media_ssrc);
        size_t i = 0;
        while (i < seqs.size()) {
            uint16_t pid = (uint16_t)seqs[i];
            uint16_t blp = 0;
            size_t j = i + 1;
            while (j < seqs.size() && seqs[j] - seqs[i] <= 16) {
                blp |= 1 << (seqs[j] - seqs[i] - 1);
                j++;
            }
            b.push_back(pid >> 8); b.push_back(pid & 255); b.push_back(blp >> 8); b.push_back(blp & 255);
            i = j;
        }
        SetLen(b);
        SendRtcp(b);
        std::lock_guard<std::mutex> lk(smu);
        stats.nacks += seqs.size();
    }

    static int FirstNalType(const std::vector<uint8_t>& p) {
        if (p.empty()) return -1;
        int t = p[0] & 31;
        if (t == 24 && p.size() > 3) return p[3] & 31;  // STAP-A: first aggregated NAL
        if (t == 28 && p.size() > 1) return (p[1] & 0x80) ? (p[1] & 31) : -2;  // FU-A start
        return t;
    }

    void OnRtp(uint64_t g, const rtc::binary& b) {
        if (stopping) return;
        const uint8_t* d = reinterpret_cast<const uint8_t*>(b.data());
        size_t n = b.size();
        if (n < 12 || (d[0] >> 6) != 2) return;
        if (d[1] >= 200 && d[1] <= 206) return;  // RTCP
        bool marker = d[1] & 0x80;
        uint16_t seq = d[2] << 8 | d[3];
        uint32_t ts = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
        uint32_t ssrc = (uint32_t)d[8] << 24 | d[9] << 16 | d[10] << 8 | d[11];
        size_t off = 12 + 4 * (d[0] & 15);
        if (d[0] & 0x10) {
            if (off + 4 > n) return;
            off += 4 + 4 * (size_t)(d[off + 2] << 8 | d[off + 3]);
        }
        size_t end = n;
        if (d[0] & 0x20) {
            if (n == 0 || d[n - 1] > n - off) return;
            end -= d[n - 1];
        }
        bool padding_only = off >= end;  // bandwidth probe: keep its sequence number so it is not a "hole"

        std::lock_guard<std::mutex> lk(rmu);
        {
            std::lock_guard<std::mutex> pl(pmu);
            if (g != gen) return;
        }

        if (ssrc != media_ssrc) {
            media_ssrc = ssrc;
            pkts.clear();
            have_seq = synced = false;
            need_key = true;
            rr_base = -1;
            rr_expected_prior = rr_received_prior = rr_received = 0;
        }
        int64_t es;
        if (!have_seq) {
            es = 131072 + seq;  // multiple of 65536 so unwrapping below works
            max_seq = es;
            have_seq = true;
        } else {
            int64_t base = max_seq & ~int64_t(0xffff);
            es = base + seq;
            if (es < max_seq - 32768) es += 65536;
            else if (es > max_seq + 32768) es -= 65536;
            if (es > max_seq) max_seq = es;
        }
        if (rr_base < 0) rr_base = es;
        rr_received++;
        {
            std::lock_guard<std::mutex> sl(smu);
            stats.packets++;
            stats.bytes += n;
        }
        if (synced && es < next_seq) return;  // late duplicate / retransmission of something already done
        pkts[es] = padding_only ? Pkt{ts, false, {}} : Pkt{ts, marker, std::vector<uint8_t>(d + off, d + end)};
        nacked.erase(es);
        Process();
        int64_t now = NowMs();
        if (now - last_remb > 1000) {
            last_remb = now;
            ReportAndRemb();
        }
    }

    void Process() {
        int64_t now = NowMs();
        if (!synced) {
            // wait for the start of a key frame (SPS first)
            for (auto it = pkts.begin(); it != pkts.end(); ++it) {
                if (FirstNalType(it->second.payload) == 7) {
                    next_seq = it->first;
                    synced = true;
                    pkts.erase(pkts.begin(), it);
                    gap_since = 0;
                    break;
                }
            }
            if (!synced) {
                Pli(now);
                while (pkts.size() > 400) pkts.erase(pkts.begin());
                return;
            }
        }
        for (;;) {
            // complete frame starting at next_seq?
            int64_t s = next_seq;
            bool complete = false;
            while (true) {
                auto it = pkts.find(s);
                if (it == pkts.end()) break;
                if (it->second.marker) { complete = true; break; }
                s++;
            }
            if (complete) {
                EmitFrame(next_seq, s);
                next_seq = s + 1;
                gap_since = 0;
                continue;
            }
            // missing packet at s; is anything newer already here?
            if (pkts.empty() || pkts.rbegin()->first <= s) {  // no hole, just waiting for more data
                gap_since = 0;
                return;
            }
            if (!gap_since) gap_since = now;
            std::vector<int64_t> want;
            for (int64_t q = s; q < pkts.rbegin()->first && want.size() < 64; q++) {
                if (pkts.count(q)) continue;
                auto& nk = nacked[q];
                if (nk.first < 3 && now - nk.second > 40) {
                    nk.first++;
                    nk.second = now;
                    want.push_back(q);
                }
            }
            SendNacks(want);
            if (now - gap_since > 180 || pkts.size() > 800) {
                // give up on the hole: skip to the next frame boundary and wait for a key frame
                int64_t lost = 0;
                for (int64_t q = s; q < pkts.rbegin()->first; q++)
                    if (!pkts.count(q)) lost++;
                {
                    std::lock_guard<std::mutex> sl(smu);
                    stats.lost += lost;
                }
                int64_t resume = -1;
                for (auto it = pkts.upper_bound(s); it != pkts.end(); ++it)
                    if (it->second.marker) { resume = it->first + 1; break; }
                need_key = true;
                Pli(now);
                gap_since = 0;
                if (resume < 0) {
                    synced = false;
                    pkts.clear();
                    return;
                }
                pkts.erase(pkts.begin(), pkts.lower_bound(resume));
                nacked.erase(nacked.begin(), nacked.lower_bound(resume));
                next_seq = resume;
                continue;
            }
            return;
        }
    }

    void EmitFrame(int64_t from, int64_t to) {
        WebFrame f;
        bool has_idr = false;
        uint32_t ts = pkts[from].ts;
        for (int64_t q = from; q <= to; q++)
            if (!pkts[q].payload.empty()) { ts = pkts[q].ts; break; }
        static const uint8_t sc[4] = {0, 0, 0, 1};
        bool fu_ok = false;
        for (int64_t q = from; q <= to; q++) {
            auto& p = pkts[q].payload;
            if (p.empty()) continue;
            int t = p[0] & 31;
            if (t >= 1 && t <= 23) {
                f.annexb.insert(f.annexb.end(), sc, sc + 4);
                f.annexb.insert(f.annexb.end(), p.begin(), p.end());
                if (t == 5) has_idr = true;
            } else if (t == 24) {
                size_t o = 1;
                while (o + 2 <= p.size()) {
                    size_t len = p[o] << 8 | p[o + 1];
                    o += 2;
                    if (o + len > p.size() || !len) break;
                    f.annexb.insert(f.annexb.end(), sc, sc + 4);
                    f.annexb.insert(f.annexb.end(), p.begin() + o, p.begin() + o + len);
                    if ((p[o] & 31) == 5) has_idr = true;
                    o += len;
                }
            } else if (t == 28 && p.size() > 2) {
                bool start = p[1] & 0x80;
                int nt = p[1] & 31;
                if (start) {
                    f.annexb.insert(f.annexb.end(), sc, sc + 4);
                    f.annexb.push_back((p[0] & 0xE0) | nt);
                    fu_ok = true;
                    if (nt == 5) has_idr = true;
                }
                if (fu_ok) f.annexb.insert(f.annexb.end(), p.begin() + 2, p.end());
                if (p[1] & 0x40) fu_ok = false;
            }
        }
        pkts.erase(pkts.begin(), pkts.upper_bound(to));
        nacked.erase(nacked.begin(), nacked.upper_bound(to));
        if (!have_ts) { have_ts = true; ts_ext = ts; }
        else ts_ext += (int32_t)(ts - last_ts);
        last_ts = ts;
        f.pts_us = ts_ext * 100 / 9;
        f.key = has_idr;
        if (need_key && !has_idr) {
            Pli(NowMs());
            std::lock_guard<std::mutex> sl(smu);
            stats.dropped_frames++;
            return;
        }
        if (has_idr) need_key = false;
        {
            std::lock_guard<std::mutex> sl(smu);
            stats.frames++;
        }
        if (cb.on_frame && !f.annexb.empty()) cb.on_frame(std::move(f));
    }
};

WebLink::WebLink() : d_(new Impl) {}
WebLink::~WebLink() { Stop(); }

bool WebLink::Start(const WebLinkConfig& cfg, Callbacks cb, std::string& err) {
    Stop();
    d_.reset(new Impl);
    d_->cfg = cfg;
    d_->cb = std::move(cb);
    if (d_->cfg.code.empty()) { err = "Нет кода"; return false; }
    d_->topic = PairTopic(d_->cfg.code);
    std::vector<std::string> list = cfg.brokers;
    if (list.empty()) list = {"broker.emqx.io:1883", "broker.hivemq.com:1883", "test.mosquitto.org:1883"};
    Impl* d = d_.get();
    for (size_t i = 0; i < list.size(); i++) {
        auto c = list[i].rfind(':');
        std::string host = list[i].substr(0, c);
        int port = c == std::string::npos ? 1883 : atoi(list[i].c_str() + c + 1);
        d->brokers.emplace_back(new Mqtt(
            (int)i, host, port, d->topic + "/pc",
            [d](int idx, const std::string& t, const std::string& p) { d->OnMqtt(idx, t, p); },
            [d, host](int, bool up) {
                Log(std::string("signal server ") + host + (up ? " up" : " down"));
                d->Event(up ? Event::BrokerUp : Event::BrokerDown, host);
            }));
    }
    return true;
}

void WebLink::Stop() {
    if (!d_) return;
    d_->stopping = true;
    if (!d_->brokers.empty()) d_->Publish("STOP\n");  // lets the phone start waiting for us again right away
    d_->brokers.clear();
    std::shared_ptr<rtc::PeerConnection> p;
    {
        std::lock_guard<std::mutex> lk(d_->pmu);
        p = d_->pc;
        d_->pc.reset();
        d_->track.reset();
        d_->dc.reset();
        d_->gen++;
    }
    if (p) {
        try { p->close(); } catch (...) {}
    }
}

void WebLink::SendLine(const std::string& line) {
    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> lk(d_->pmu);
        dc = d_->dc;
    }
    if (dc && dc->isOpen()) {
        try { dc->send(line + "\n"); } catch (...) {}
    }
}

void WebLink::RequestKeyframe() {
    std::lock_guard<std::mutex> lk(d_->rmu);
    d_->last_pli = 0;
    d_->Pli(NowMs());
}

void WebLink::SetConfig(int w, int h, int fps, int kbps, const std::string& camera) {
    {
        std::lock_guard<std::mutex> lk(d_->pmu);
        d_->cfg.width = w;
        d_->cfg.height = h;
        d_->cfg.fps = fps;
        d_->cfg.kbps = kbps;
        d_->cfg.camera = camera;
    }
    char b[128];
    snprintf(b, sizeof b, "CFG %d %d %d %d %s", w, h, fps, kbps, camera.c_str());
    SendLine(b);
}

int WebLink::BrokersUp() const {
    int n = 0;
    for (auto& b : d_->brokers) n += b->Up();
    return n;
}

WebLink::Stats WebLink::TakeStats() {
    std::lock_guard<std::mutex> lk(d_->smu);
    Stats s = d_->stats;
    d_->stats = Stats{};
    return s;
}

}  // namespace phonecam
