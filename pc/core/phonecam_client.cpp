#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>

#include "h264_decoder.h"
#include "night_filter.h"
#include "qr.h"
#include "web_link.h"
#include "phonecam_client.h"

namespace phonecam {

namespace {

struct WsaInit {
    WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WsaInit() { WSACleanup(); }
};
WsaInit& Wsa() { static WsaInit w; return w; }

constexpr SOCKET kBad = INVALID_SOCKET;

bool SendAll(SOCKET s, const void* data, int len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, int len) {
    char* p = (char*)data;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool RecvLine(SOCKET s, std::string& line) {
    line.clear();
    char c;
    while (line.size() < 512) {
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return false;
        if (c == '\n') return true;
        line.push_back(c);
    }
    return false;
}

SOCKET ConnectTo(const std::string& host, int port, int timeout_ms, std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host.c_str(), ps, &hints, &res) != 0 || !res) {
        err = "Неверный IP-адрес: " + host;
        return kBad;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = select(0, nullptr, &w, &e, &tv);
    if (r <= 0 || FD_ISSET(s, &e)) {
        closesocket(s);
        err = r == 0 ? "Телефон не отвечает (" + host + "). Приложение открыто? Одна Wi-Fi сеть?"
                     : "Не удалось подключиться к " + host;
        return kBad;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
    int rb = 256 * 1024;  // big enough for a key frame, small enough not to hide latency
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof rb);
    DWORD to = 6000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    return s;
}

uint32_t BE32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
int64_t BE64(const uint8_t* p) { return (int64_t)((uint64_t)BE32(p) << 32 | BE32(p + 4)); }

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ExeDir, &self);
    GetModuleFileNameW(self, buf, MAX_PATH);
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring FindAdb() {
    std::vector<std::wstring> cand;
    std::wstring d = ExeDir();
    cand.push_back(d + L"adb.exe");
    cand.push_back(d + L"platform-tools\\adb.exe");
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", env, MAX_PATH))
        cand.push_back(std::wstring(env) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    if (GetEnvironmentVariableW(L"ANDROID_HOME", env, MAX_PATH))
        cand.push_back(std::wstring(env) + L"\\platform-tools\\adb.exe");
    cand.push_back(L"C:\\platform-tools\\adb.exe");
    for (auto& c : cand)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

}  // namespace

void Log(const std::string& line) {
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    wchar_t ad[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH)) return;
    std::wstring dir = std::wstring(ad) + L"\\PhoneCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\log.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2 * 1024 * 1024)
        DeleteFileW(path.c_str());
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds, line.c_str());
    fclose(f);
}

bool AdbForward(int port, std::string& err) {
    std::wstring adb = FindAdb();
    if (adb.empty()) {
        err = "Не найден adb.exe. Скачай Android Platform Tools и положи adb.exe (+AdbWinApi.dll, AdbWinUsbApi.dll) рядом с PhoneCam.";
        return false;
    }
    wchar_t cmd[1024];
    swprintf(cmd, 1024, L"\"%ls\" forward tcp:%d tcp:%d", adb.c_str(), port, port);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        err = "Не удалось запустить adb.exe";
        return false;
    }
    DWORD w = WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (w != WAIT_OBJECT_0 || code != 0) {
        err = "adb: телефон не найден по USB. Включи «Отладку по USB» и подтверди запрос на телефоне.";
        return false;
    }
    return true;
}

std::vector<FoundPhone> Discover(int timeout_ms) {
    Wsa();
    std::vector<FoundPhone> out;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBad) return out;
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    bind(s, (sockaddr*)&local, sizeof local);

    std::vector<uint32_t> targets = {INADDR_BROADCAST};
    ULONG len = 15000;
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len]);
    auto* aa = (IP_ADAPTER_ADDRESSES*)buf.get();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, aa, &len) == NO_ERROR) {
        for (auto* a = aa; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
                if (sin->sin_family != AF_INET) continue;
                uint32_t ip = ntohl(sin->sin_addr.s_addr);
                int pl = u->OnLinkPrefixLength;
                if (pl <= 0 || pl >= 32) continue;
                uint32_t mask = pl == 0 ? 0 : 0xffffffffu << (32 - pl);
                targets.push_back((ip & mask) | ~mask);
            }
        }
    }
    const char msg[] = "PHC_DISCOVER";
    for (int round = 0; round < 2; round++) {
        for (uint32_t t : targets) {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kUdpPort);
            to.sin_addr.s_addr = htonl(t);
            sendto(s, msg, (int)sizeof msg - 1, 0, (sockaddr*)&to, sizeof to);
        }
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms / 2);
        for (;;) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(s, &r);
            timeval tv{(long)(left / 1000), (long)((left % 1000) * 1000)};
            if (select(0, &r, nullptr, nullptr, &tv) <= 0) break;
            char rb[300];
            sockaddr_in from{};
            int fl = sizeof from;
            int n = recvfrom(s, rb, sizeof rb - 1, 0, (sockaddr*)&from, &fl);
            if (n <= 0) continue;
            rb[n] = 0;
            if (strncmp(rb, "PHC_HERE ", 9) != 0) continue;
            FoundPhone p;
            char ipb[32];
            inet_ntop(AF_INET, &from.sin_addr, ipb, sizeof ipb);
            p.ip = ipb;
            p.port = atoi(rb + 9);
            const char* sp = strchr(rb + 9, ' ');
            p.name = sp ? sp + 1 : "Android";
            bool dup = false;
            for (auto& e : out) dup |= e.ip == p.ip;
            if (!dup) out.push_back(p);
        }
        if (!out.empty()) break;
    }
    closesocket(s);
    return out;
}

bool QueryCaps(const std::string& host_in, int port, bool usb, std::vector<CamCaps>& out, std::string& err) {
    Wsa();
    out.clear();
    std::string host = host_in;
    if (usb) {
        if (!AdbForward(port, err)) return false;
        host = "127.0.0.1";
    }
    SOCKET s = ConnectTo(host, port, 2000, err);
    if (s == kBad) return false;
    const char q[] = "PHC1 CAPS\n";
    std::string line;
    bool ok = SendAll(s, q, (int)sizeof q - 1) && RecvLine(s, line);
    closesocket(s);
    if (!ok || line.rfind("PHC1 CAPS", 0) != 0) {
        err = "Телефон не сообщил возможности камер (старая версия приложения?)";
        return false;
    }
    const char* p = line.c_str() + 9;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char side[16] = {0};
        CamCaps c;
        int n = 0;
        if (sscanf(p, "%15[^/]/%dx%d/%dx%d/%d/%d%n", side, &c.req_w, &c.req_h, &c.w, &c.h, &c.max_fps, &c.hs_fps, &n) == 7) {
            c.side = side;
            out.push_back(c);
        }
        while (*p && *p != ' ') p++;
    }
    return !out.empty();
}

const CamCaps* FindCaps(const std::vector<CamCaps>& caps, const std::string& side, int w, int h) {
    for (auto& c : caps)
        if (c.side == side && c.req_w == w && c.req_h == h) return &c;
    return nullptr;
}

Client::Client() { Wsa(); }

Client::~Client() { Stop(); }

void Client::Start(const Config& cfg, FrameCallback on_frame, StatusCallback on_status) {
    Stop();
    cfg_ = cfg;
    mirror_ = cfg.mirror;
    user_rot180_ = cfg.rotate180;
    on_frame_ = std::move(on_frame);
    on_status_ = std::move(on_status);
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void Client::Stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lk(sock_mutex_);
        if (sock_ != (uintptr_t)kBad) {
            shutdown((SOCKET)sock_, SD_BOTH);
            closesocket((SOCKET)sock_);
            sock_ = (uintptr_t)kBad;
        }
    }
    if (thread_.joinable()) thread_.join();
}

void Client::SendCommand(const std::string& line) {
    {
        std::lock_guard<std::mutex> lk(web_mu_);
        if (web_) {
            if (line == "KEYFRAME") web_->RequestKeyframe();
            else if (line.rfind("PING", 0) != 0) web_->SendLine(line);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(sock_mutex_);
    if (sock_ == (uintptr_t)kBad) return;
    std::string l = line + "\n";
    send((SOCKET)sock_, l.data(), (int)l.size(), 0);
}

void Client::Run() {
    while (running_) {
        if (on_status_) on_status_(State::Connecting, "Подключение…");
        std::string err;
        bool clean = Session(err);
        if (!running_) break;
        Log("session ended: " + (err.empty() ? std::string("connection lost") : err));
        if (on_status_) on_status_(State::Error, err.empty() ? "Соединение потеряно" : err);
        if (!cfg_.auto_reconnect && !clean) break;
        for (int i = 0; i < 20 && running_; i++) Sleep(100);
    }
    if (on_status_) on_status_(State::Idle, "Отключено");
}

bool Client::Session(std::string& err) {
    if (cfg_.web) return SessionWeb(err);
    std::string host = cfg_.host;
    if (cfg_.usb) {
        if (!AdbForward(cfg_.port, err)) return false;
        host = "127.0.0.1";
    }
    SOCKET s = ConnectTo(host, cfg_.port, 3000, err);
    if (s == kBad) return false;
    {
        std::lock_guard<std::mutex> lk(sock_mutex_);
        if (!running_) { closesocket(s); return true; }
        sock_ = (uintptr_t)s;
    }
    auto close_sock = [&] {
        std::lock_guard<std::mutex> lk(sock_mutex_);
        if (sock_ != (uintptr_t)kBad) { closesocket((SOCKET)sock_); sock_ = (uintptr_t)kBad; }
    };

    char hello[256];
    snprintf(hello, sizeof hello, "PHC1 %d %d %d %d %s%s\n", cfg_.width, cfg_.height, cfg_.fps, cfg_.kbps,
             cfg_.camera.c_str(), cfg_.high_speed ? " hs" : "");
    std::string line;
    if (!SendAll(s, hello, (int)strlen(hello)) || !RecvLine(s, line)) {
        err = cfg_.usb ? "Телефон не ответил по USB. Приложение PhoneCam открыто?" : "Телефон не ответил";
        close_sock();
        return false;
    }
    if (line.rfind("PHC1 OK", 0) != 0) {
        err = line.rfind("PHC1 ERR ", 0) == 0 ? "Телефон: " + line.substr(9) : "Это не PhoneCam";
        close_sock();
        return false;
    }
    int w = 0, h = 0, rot = 0, real_fps = 0, max_fps = 0;
    sscanf(line.c_str() + 7, "%d %d %d %d %d", &w, &h, &rot, &real_fps, &max_fps);
    if (real_fps <= 0) real_fps = cfg_.fps;
    std::string fps_note, phone_note;
    {
        // anything after the 5 numbers is a human readable note from the phone
        const char* p = line.c_str() + 7;
        for (int i = 0; i < 5 && *p; i++) {
            while (*p == ' ') p++;
            while (*p && *p != ' ') p++;
        }
        while (*p == ' ') p++;
        phone_note = p;
    }
    if (real_fps < cfg_.fps)
        fps_note = "\nКамера телефона даёт максимум " + std::to_string(real_fps) + " к/с в " + std::to_string(w) + "x" +
                   std::to_string(h) + " (выбрано " + std::to_string(cfg_.fps) + ")";
    phone_rotation_ = rot;

    Log("connected to " + host + ": " + line);
    ev_hw_ = true;
    {
        std::lock_guard<std::mutex> lk(adj_mu_);
        RebuildLuts();
    }
    for (auto& c : cfg_.init_commands) SendCommand(c);
    H264Decoder dec;
    if (!dec.Init(err)) { close_sock(); return false; }

    char st[128];
    snprintf(st, sizeof st, "Идёт трансляция %dx%d @ %d fps%s", w, h, cfg_.fps, cfg_.usb ? " (USB)" : "");
    if (on_status_) on_status_(State::Streaming, st);

    // From here on use a 1 s receive timeout so stats keep updating even if the phone goes quiet.
    DWORD to = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);

    std::vector<uint8_t> payload;
    bool have_config = false;
    int consecutive_errors = 0;
    int pk_in = 0, skipped = 0, decoded = 0, errors = 0, total_errors = 0;
    uint64_t bytes = 0;
    std::string phone_stats, last_err;
    DWORD stat_t0 = GetTickCount();
    DWORD last_data = stat_t0;
    // ---- latency measurement: PC and phone clocks are synced with PING/PONG (offset from the fastest round trip)
    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    auto now_us = [&]() -> int64_t {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (int64_t)(c.QuadPart * 1000000 / qpf.QuadPart);
    };
    bool have_offset = false;
    int64_t clock_offset = 0, best_rtt = INT64_MAX, best_rtt_at = 0, last_ping = 0;
    int64_t cur_pts = 0, lat_sum = 0, lat_max = 0, dec_sum = 0, resync_until = 0;
    int lat_n = 0, dec_n = 0, resyncs = 0;
    bool need_resync = false;
    // Catch-up is triggered by GROWTH of latency over the best value seen recently (immune to a constant error in
    // the clock sync), or by a real backlog of unread data in the socket. Not by an absolute number.
    int64_t floor_cur = INT64_MAX, floor_prev = INT64_MAX, floor_t0 = 0, last_growth_lat = 0;
    const int64_t backlog_limit = std::max<int64_t>(400 * 1024, (int64_t)cfg_.kbps * 1000 / 8 * 3 / 5);  // ~0.6 s
    auto on_decoded = [&](const Frame& f) {
        decoded++;
        Deliver(f);
        if (have_offset) {
            int64_t lat = now_us() - (cur_pts - clock_offset);
            if (lat >= 0 && lat < 30000000) {
                lat_sum += lat;
                lat_n++;
                if (lat > lat_max) lat_max = lat;
                int64_t tn = now_us();
                if (tn - floor_t0 > 5000000) { floor_prev = floor_cur; floor_cur = INT64_MAX; floor_t0 = tn; }
                if (lat < floor_cur) floor_cur = lat;
                int64_t floor = std::min(floor_cur, floor_prev);
                last_growth_lat = lat - floor;
                // fell behind by more than 0.4 s compared with how it normally runs
                if (floor != INT64_MAX && lat - floor > 400000 && tn > resync_until) need_resync = true;
            }
        }
    };

    auto tick = [&]() {
        int64_t tnow = now_us();
        if (tnow - last_ping > 1000000) {
            last_ping = tnow;
            SendCommand("PING " + std::to_string(tnow));
        }
        DWORD now = GetTickCount();
        if (now - stat_t0 < 1000) return;
        double sec = (now - stat_t0) / 1000.0;
        char sb[400];
        snprintf(sb, sizeof sb, "%dx%d@%d%s • приём %.0f к/с (%.1f Мбит/с) • вывод %.0f к/с%s%s%s",
                 w, h, real_fps, cfg_.usb ? " USB" : "", pk_in / sec, bytes * 8 / sec / 1e6, decoded / sec,
                 errors ? " • ошибок декодера " : "", errors ? std::to_string(errors).c_str() : "",
                 skipped ? (" • пропущено " + std::to_string(skipped)).c_str() : "");
        std::string msg = sb;
        if (lat_n) {
            char lb[160];
            snprintf(lb, sizeof lb, "\nЗадержка ≈ %lld мс (макс %lld) • декодер %.1f мс/кадр%s",
                     (long long)(lat_sum / lat_n / 1000), (long long)(lat_max / 1000),
                     dec_n ? dec_sum / (double)dec_n / 1000.0 : 0.0,
                     resyncs ? (" • догоняли " + std::to_string(resyncs) + " раз").c_str() : "");
            msg += lb;
        }
        msg += fps_note;
        if (!ev_hw_) msg += "\nЭкспозиция делается на ПК: камера телефона её игнорирует";
        if (auto_bri_) {
            char ab[96];
            double k;
            { std::lock_guard<std::mutex> lk(adj_mu_); k = auto_k_; }
            if (k > 1.03) snprintf(ab, sizeof ab, "\nУмная яркость: мало света, осветляет (усиление ×%.1f)", k);
            else snprintf(ab, sizeof ab, "\nУмная яркость: света хватает, не вмешивается");
            msg += ab;
        }
        lat_sum = lat_max = dec_sum = 0;
        lat_n = dec_n = 0;
        if (!phone_note.empty()) msg += "\n" + phone_note;
        if (!phone_stats.empty()) msg += "\nТелефон: " + phone_stats;
        if (on_status_) on_status_(State::Streaming, msg);
        pk_in = skipped = decoded = errors = 0;
        bytes = 0;
        stat_t0 = now;
    };

    // Receive exactly len bytes, tolerating 1 s timeouts (up to 8 s of silence).
    auto recv_all = [&](void* data, int len) -> bool {
        char* p = (char*)data;
        while (len > 0 && running_) {
            int n = recv(s, p, len, 0);
            if (n > 0) { p += n; len -= n; last_data = GetTickCount(); continue; }
            if (n == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT) {
                tick();
                if (GetTickCount() - last_data > 8000) return false;
                continue;
            }
            return false;
        }
        return len == 0;
    };

    while (running_) {
        uint8_t hdr[13];
        if (!recv_all(hdr, 13)) { err = "Соединение с телефоном потеряно"; break; }
        uint32_t len = BE32(hdr);
        int type = hdr[4];
        int64_t pts = BE64(hdr + 5);
        if (len > 16 * 1024 * 1024) { err = "Повреждённые данные"; break; }
        payload.resize(len);
        if (len && !recv_all(payload.data(), (int)len)) { err = "Соединение с телефоном потеряно"; break; }
        bytes += len + 13;
        if (type == 4) {
            if (len >= 2) phone_rotation_ = payload[0] << 8 | payload[1];
            tick();
            continue;
        }
        if (type == 7) {
            HandleInfo(std::string((const char*)payload.data(), payload.size()));
            continue;
        }
        if (type == 6) {
            long long t = 0, ph = 0;
            std::string sp((const char*)payload.data(), payload.size());
            if (sscanf(sp.c_str(), "%lld %lld", &t, &ph) == 2) {
                int64_t tn = now_us(), rtt = tn - t;
                if (rtt >= 0 && (rtt <= best_rtt || tn - best_rtt_at > 10000000)) {
                    best_rtt = rtt;
                    best_rtt_at = tn;
                    clock_offset = ph - (t + rtt / 2);
                    have_offset = true;
                }
            }
            continue;
        }
        if (type == 5) {
            phone_stats.assign((const char*)payload.data(), payload.size());
            tick();
            continue;
        }
        if (type != 1) pk_in++;
        // SPS/PPS come either as a separate config packet or inside the key frame itself.
        if (type == 1 || type == 2) have_config = true;
        if (!have_config && type != 2) { skipped++; tick(); continue; }
        std::string derr;
        cur_pts = pts;
        int64_t d0 = now_us();
        bool dec_ok = dec.Decode(payload.data(), payload.size(), pts, on_decoded, derr);
        dec_sum += now_us() - d0;
        dec_n++;
        {
            // real backlog: lots of unread video waiting in the socket = the PC can't keep up
            u_long avail = 0;
            if (ioctlsocket(s, FIONREAD, &avail) == 0 && (int64_t)avail > backlog_limit && now_us() > resync_until)
                need_resync = true;
        }
        if (need_resync) {
            need_resync = false;
            resyncs++;
            resync_until = now_us() + 3000000;
            floor_cur = floor_prev = INT64_MAX;  // re-learn the normal latency after catching up
            Log("latency over 500 ms, skipping to a fresh key frame");
            dec.Reset();
            have_config = false;
            SendCommand("KEYFRAME");
            tick();
            continue;
        }
        if (dec_ok) {
            consecutive_errors = 0;
        } else {
            errors++;
            if (total_errors++ < 40 || derr != last_err)
                Log("decode error (type " + std::to_string(type) + ", " + std::to_string(len) + " bytes): " + derr);
            last_err = derr;
            if (++consecutive_errors >= 30) {
                // Decoder is stuck: flush it and restart from a fresh key frame.
                Log("decoder reset");
                dec.Reset();
                have_config = false;
                consecutive_errors = 0;
                SendCommand("KEYFRAME");
            }
        }
        tick();
    }
    close_sock();
    return !running_;
}

// ---------------------------------------------------------------- Telegram / browser mode
bool Client::SessionWeb(std::string& err) {
    WebLink link;
    WebLinkConfig wc;
    wc.code = cfg_.web_code;
    wc.width = cfg_.width;
    wc.height = cfg_.height;
    wc.fps = cfg_.fps;
    wc.kbps = cfg_.kbps;
    wc.camera = cfg_.camera;

    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<WebFrame> queue;
    std::atomic<bool> phone_connected{false}, phone_ready{false};
    std::atomic<int> brokers_up{0};
    std::string phone_stats, phone_caps;
    std::mutex info_mu;
    bool waiting_key = false;

    WebLink::Callbacks cb;
    cb.on_event = [&](WebLink::Event e, const std::string&) {
        if (e == WebLink::Event::BrokerUp) brokers_up++;
        else if (e == WebLink::Event::BrokerDown) brokers_up--;
        else if (e == WebLink::Event::Connected) phone_connected = true;
        else if (e == WebLink::Event::Disconnected || e == WebLink::Event::Failed || e == WebLink::Event::PhoneOffer) {
            phone_connected = false;
            phone_ready = false;
        }
        qcv.notify_one();
    };
    cb.on_frame = [&](WebFrame&& f) {
        std::lock_guard<std::mutex> lk(qmu);
        if (queue.size() >= 6) {  // the PC can't keep up: drop the backlog and restart from a key frame
            queue.clear();
            waiting_key = true;
        }
        if (waiting_key && !f.key) return;
        waiting_key = false;
        queue.push_back(std::move(f));
        qcv.notify_one();
    };
    cb.on_line = [&](const std::string& l) {
        if (l.rfind("INFO ", 0) == 0) HandleInfo(l.substr(5));
        else if (l.rfind("STATS ", 0) == 0) { std::lock_guard<std::mutex> lk(info_mu); phone_stats = l.substr(6); }
        else if (l.rfind("CAPS ", 0) == 0) {
            { std::lock_guard<std::mutex> lk(info_mu); phone_caps = l.substr(5); }
            Log("phone page: " + l);
            phone_ready = true;  // data channel open: send the camera settings
            for (auto& c : cfg_.init_commands) link.SendLine(c);
        }
    };
    phone_rotation_ = 0;  // the browser sends upright pictures
    if (!link.Start(wc, cb, err)) return false;
    {
        std::lock_guard<std::mutex> lk(web_mu_);
        web_ = &link;
    }
    ev_hw_ = true;
    {
        std::lock_guard<std::mutex> lk(adj_mu_);
        RebuildLuts();
    }
    H264Decoder dec;
    if (!dec.Init(err)) {
        std::lock_guard<std::mutex> lk(web_mu_);
        web_ = nullptr;
        return false;
    }
    std::string code_txt = cfg_.web_code.substr(0, 4) + "-" + cfg_.web_code.substr(4);
    auto t0 = std::chrono::steady_clock::now();
    auto last_stat = t0, last_frame = t0, last_qr = t0 - std::chrono::seconds(10);
    int decoded = 0, errors = 0, consecutive_errors = 0, w = 0, h = 0;
    uint64_t bytes = 0;
    bool streaming = false;
    auto on_decoded = [&](const Frame& f) {
        decoded++;
        w = f.width;
        h = f.height;
        Deliver(f);
    };
    while (running_) {
        WebFrame f;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(qmu);
            qcv.wait_for(lk, std::chrono::milliseconds(250), [&] { return !queue.empty() || !running_; });
            if (!queue.empty()) {
                f = std::move(queue.front());
                queue.pop_front();
                have = true;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (have) {
            last_frame = now;
            bytes += f.annexb.size();
            std::string derr;
            if (dec.Decode(f.annexb.data(), f.annexb.size(), f.pts_us, on_decoded, derr)) {
                consecutive_errors = 0;
            } else {
                errors++;
                if (errors < 20) Log("web decode error: " + derr);
                if (++consecutive_errors >= 20) {
                    dec.Reset();
                    consecutive_errors = 0;
                    std::lock_guard<std::mutex> lk(qmu);
                    waiting_key = true;
                    link.RequestKeyframe();
                }
            }
        }
        bool video = phone_connected && now - last_frame < std::chrono::seconds(3);
        if (!video && cfg_.web_qr_frames && now - last_qr > std::chrono::milliseconds(500)) {
            last_qr = now;
            EmitQrFrame();
        }
        if (now - last_stat < std::chrono::seconds(1)) continue;
        double sec = std::chrono::duration<double>(now - last_stat).count();
        last_stat = now;
        auto st = link.TakeStats();
        std::string msg;
        if (video) {
            char b[300];
            snprintf(b, sizeof b, "%dx%d • приём %.0f к/с (%.1f Мбит/с) • вывод %.0f к/с • через Телеграм/браузер",
                     w, h, st.frames / sec, bytes * 8 / sec / 1e6, decoded / sec);
            msg = b;
            if (st.lost || st.nacks) {
                snprintf(b, sizeof b, "\nСеть: потеряно пакетов %llu, повторно запрошено %llu",
                         (unsigned long long)st.lost, (unsigned long long)st.nacks);
                msg += b;
            }
            if (errors) msg += "\nОшибок декодера: " + std::to_string(errors);
            if (!ev_hw_) msg += "\nЭкспозиция делается на ПК: камера телефона её не умеет";
            if (auto_bri_) {
                double k;
                { std::lock_guard<std::mutex> lk(adj_mu_); k = auto_k_; }
                char ab[96];
                if (k > 1.03) snprintf(ab, sizeof ab, "\nУмная яркость: мало света, осветляет (усиление ×%.1f)", k);
                else snprintf(ab, sizeof ab, "\nУмная яркость: света хватает, не вмешивается");
                msg += ab;
            }
            std::lock_guard<std::mutex> lk(info_mu);
            if (!phone_stats.empty()) msg += "\nТелефон: " + phone_stats;
            streaming = true;
            if (on_status_) on_status_(State::Streaming, msg);
        } else {
            if (streaming) Log("web: video stopped");
            streaming = false;
            int up = brokers_up;
            double waited = std::chrono::duration<double>(now - t0).count();
            if (phone_connected) msg = "Телефон подключён, ждём картинку…";
            else if (up > 0) msg = "Ждём телефон. Код: " + code_txt +
                                   "\nНаведи камеру телефона на QR-код (бот @phonecamweb_bot в Телеграме) или введи код в боте.";
            else if (waited < 8) msg = "Подключение к серверу связи…";
            else msg = "Нет связи с сервером. Проверь интернет на ПК (нужен выход в интернет, порт 1883).";
            if (on_status_) on_status_(State::Connecting, msg);
        }
        decoded = errors = 0;
        bytes = 0;
    }
    {
        std::lock_guard<std::mutex> lk(web_mu_);
        web_ = nullptr;
    }
    link.Stop();
    return true;
}

// Picture with the pairing QR code (used by the OBS plugin while waiting for the phone).
void Client::EmitQrFrame() {
    if (!on_frame_) return;
    const int W = 1280, H = 720;
    QrCode q = MakeQr(cfg_.web_link.empty() ? cfg_.web_code : cfg_.web_link);
    if (!q.size) return;
    qr_frame_.assign(W * H * 3 / 2, 0);
    uint8_t* Y = qr_frame_.data();
    uint8_t* UV = Y + W * H;
    memset(Y, 30, W * H);
    memset(UV, 128, W * H / 2);
    int mod = (H - 160) / (q.size + 8);
    int side = mod * (q.size + 8);
    int ox = (W - side) / 2, oy = (H - side) / 2;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            int mx = x / mod - 4, my = y / mod - 4;
            bool dark = mx >= 0 && my >= 0 && mx < q.size && my < q.size && q.At(mx, my);
            Y[(oy + y) * W + ox + x] = dark ? 16 : 235;
        }
    Frame f;
    f.width = W;
    f.height = H;
    f.y = Y;
    f.uv = UV;
    f.y_stride = W;
    f.uv_stride = W;
    on_frame_(f);
}

// "torch 1", "zoom 2.0", "battery 76 1", "evhw 0" - from the phone app (INFO packet) or the web page.
void Client::HandleInfo(const std::string& sp) {
    if ((sp.rfind("torch ", 0) == 0 || sp.rfind("zoom ", 0) == 0) && on_control_) {
        size_t sp_at = sp.find(' ');
        on_control_(sp.substr(0, sp_at), (float)atof(sp.c_str() + sp_at + 1));
    }
    if (sp.rfind("battery ", 0) == 0 && on_control_) {
        int pct = -1, chg = 0;
        if (sscanf(sp.c_str() + 8, "%d %d", &pct, &chg) >= 1 && pct >= 0 && pct <= 100) {
            on_control_("charging", (float)chg);
            on_control_("battery", (float)pct);
        }
    }
    if (sp.rfind("evhw ", 0) == 0 && sp.size() > 5) {
        bool hw = sp[5] == '1';
        if (hw != ev_hw_) {
            ev_hw_ = hw;
            std::lock_guard<std::mutex> lk(adj_mu_);
            RebuildLuts();
            Log(std::string("phone exposure compensation ") + (ev_hw_ ? "works" : "ignored, doing it on the PC"));
        }
    }
}

void Client::Deliver(const Frame& src) {
    if (!on_frame_) return;
    // Total clockwise rotation: what the phone says is needed to be upright, plus the user's extra 180.
    int rot = ((phone_rotation_ % 360) + 360 + (user_rot180_ ? 180 : 0)) % 360;
    bool mirror = mirror_;
    if (rot == 0 && !mirror) {
        Emit(src);
        return;
    }
    const int W = src.width, H = src.height;
    const bool swap = rot == 90 || rot == 270;
    const int ow = swap ? H : W, oh = swap ? W : H;
    size_t ysz = (size_t)ow * oh;
    work_.resize(ysz * 3 / 2);
    uint8_t* Y = work_.data();
    uint8_t* UV = Y + ysz;

    // For output pixel (x, y): src = origin + x * along_x + y * along_y (in source pixel coordinates).
    auto plan = [&](int sw, int sh, int& ox, int& oy, int& ax, int& ay, int& bx, int& by) {
        switch (rot) {
        case 90:  ox = 0;      oy = sh - 1; ax = 0;  ay = -1; bx = 1;  by = 0;  break;  // src = (y, H-1-x)
        case 180: ox = sw - 1; oy = sh - 1; ax = -1; ay = 0;  bx = 0;  by = -1; break;
        case 270: ox = sw - 1; oy = 0;      ax = 0;  ay = 1;  bx = -1; by = 0;  break;  // src = (W-1-y, x)
        default:  ox = 0;      oy = 0;      ax = 1;  ay = 0;  bx = 0;  by = 1;  break;
        }
    };
    int ox, oy, ax, ay, bx, by;
    // luma
    plan(W, H, ox, oy, ax, ay, bx, by);
    if (mirror) { ox += (ow - 1) * ax; oy += (ow - 1) * ay; ax = -ax; ay = -ay; }
    {
        const ptrdiff_t dx = ax + (ptrdiff_t)ay * src.y_stride, dy = bx + (ptrdiff_t)by * src.y_stride;
        const uint8_t* row = src.y + ox + (ptrdiff_t)oy * src.y_stride;
        for (int y = 0; y < oh; y++, row += dy) {
            uint8_t* d = Y + (size_t)y * ow;
            if (dx == 1) { memcpy(d, row, ow); continue; }
            const uint8_t* p = row;
            for (int x = 0; x < ow; x++, p += dx) d[x] = *p;
        }
    }
    // chroma (interleaved U,V pairs at half resolution)
    const int cw = W / 2, chh = H / 2, ocw = ow / 2, och = oh / 2;
    plan(cw, chh, ox, oy, ax, ay, bx, by);
    if (mirror) { ox += (ocw - 1) * ax; oy += (ocw - 1) * ay; ax = -ax; ay = -ay; }
    {
        const ptrdiff_t dx = 2 * ax + (ptrdiff_t)ay * src.uv_stride, dy = 2 * bx + (ptrdiff_t)by * src.uv_stride;
        const uint8_t* row = src.uv + 2 * ox + (ptrdiff_t)oy * src.uv_stride;
        for (int y = 0; y < och; y++, row += dy) {
            uint8_t* d = UV + (size_t)y * ow;
            if (dx == 2) { memcpy(d, row, (size_t)ocw * 2); continue; }
            const uint8_t* p = row;
            for (int x = 0; x < ocw; x++, p += dx) { d[2 * x] = p[0]; d[2 * x + 1] = p[1]; }
        }
    }
    Frame f;
    f.width = ow;
    f.height = oh;
    f.y = Y;
    f.uv = UV;
    f.y_stride = ow;
    f.uv_stride = ow;
    Emit(f);
}

void Client::SetAdjust(const ImageAdjust& a) {
    std::lock_guard<std::mutex> lk(adj_mu_);
    adj_ = a;
    RebuildLuts();
}

void Client::SetAutoBrightness(bool on) {
    std::lock_guard<std::mutex> lk(adj_mu_);
    auto_bri_ = on;
    if (!on) { auto_k_ = 1.0; auto_awake_ = false; }
    RebuildLuts();
}

void Client::SetExposureEv(float ev) {
    std::lock_guard<std::mutex> lk(adj_mu_);
    digital_ev_ = ev;
    RebuildLuts();
}

// Smart brightness curve: real gain (like exposure) with a soft shoulder so highlights don't burn out,
// and a tiny black point so the noise floor stays black instead of turning into a grey veil.
static double SmartCurve(double n, double g) {
    const double black = 0.012, knee = 0.45, contrast = 0.12;
    double x = std::max(0.0, n - black) / (1.0 - black) * g;
    double y = x <= knee ? x : knee + (1.0 - knee) * (1.0 - exp(-(x - knee) / (1.0 - knee)));
    // gentle S-curve: deeper shadows and a bit more punch, so the lifted picture doesn't look flat
    y += contrast * sin(3.14159265358979 * (y - 0.5)) * y * (1.0 - y) * 2.0;
    return std::max(0.0, std::min(1.0, y));
}

// adj_mu_ must be held
void Client::RebuildLuts() {
    const ImageAdjust& a = adj_;
    double ev = ev_hw_ ? 0.0 : digital_ev_;
    adj_active_ = a.Active() || ev != 0.0 || auto_k_ > 1.005;
    auto_k_built_ = auto_k_;
    const double ak = auto_k_;   // smart brightness gain (1 = off)
    double ev_gain = pow(2.0, ev / 2.2);  // EV is linear light; frames are gamma encoded
    double g = std::max(50, std::min(200, a.gamma)) / 100.0;
    double cf = a.contrast >= 0 ? 1.0 + a.contrast / 50.0 : 1.0 + a.contrast / 100.0;
    double bo = a.brightness / 250.0;
    for (int v = 0; v < 256; v++) {
        double n = (v - 16) / 219.0;
        n = n < 0 ? 0 : n > 1 ? 1 : n;
        n = std::min(1.0, n * ev_gain);
        double n_before = n;
        if (ak > 1.005) n = SmartCurve(n, ak);
        n = pow(n, 1.0 / g);
        n = (n - 0.5) * cf + 0.5 + bo;
        int o = (int)lround(16 + n * 219);
        ylut_[v] = (uint8_t)(o < 16 ? 16 : o > 235 ? 235 : o);
        // Colour follows the brightness boost, otherwise a brightened picture looks washed out.
        // Partial (85%) and capped, and faded out in the deepest blacks where it would only colour the noise.
        double cs = 1.0;
        if (ak > 1.005) {
            double after = SmartCurve(n_before, ak);
            double ratio = n_before > 0.002 ? after / n_before : ak;
            cs = 1.0 + (ratio - 1.0) * 0.85;
            double fade = std::min(1.0, n_before / 0.05);
            cs = 1.0 + (std::min(cs, 3.5) - 1.0) * fade;
        }
        csat_[v] = (uint16_t)lround(cs * 256);
        // saturation scales chroma around neutral; temperature shifts it: warm = more red (V), less blue (U)
        double sf = (100 + a.saturation) / 100.0;
        double shift = a.temperature * 0.14;
        int u = (int)lround(128 + (v - 128) * sf - shift);
        int vv = (int)lround(128 + (v - 128) * sf + shift);
        ulut_[v] = (uint8_t)(u < 16 ? 16 : u > 240 ? 240 : u);
        vlut_[v] = (uint8_t)(vv < 16 ? 16 : vv > 240 ? 240 : vv);
    }
    // Low-light pipeline tables (10-bit input x local brightness level), used while smart brightness is active.
    if (ak > 1.005) {
        if (!night_) night_.reset(new NightFilter);
        for (int L = 0; L < NightFilter::kLevels; L++) {
            double fac = NightFilter::LevelFactor(L);
            for (int i = 0; i < 1024; i++) {
                double n = (i / 4.0 - 16) / 219.0;
                n = n < 0 ? 0 : n > 1 ? 1 : n;
                n = std::min(1.0, n * ev_gain);
                // toe: the darkest values are mostly sensor/codec noise - keep them dark instead of lifting blotches
                const double toe = 0.045;
                double nt = n < toe ? n * n / toe : n;
                double after = SmartCurve(std::min(1.0, nt * fac), ak);
                double o = pow(after, 1.0 / g);
                o = (o - 0.5) * cf + 0.5 + bo;
                int oy = (int)lround(16 + o * 219);
                night_->tone[L][i] = (uint8_t)(oy < 16 ? 16 : oy > 235 ? 235 : oy);
                double ratio = n > 0.002 ? after / n : ak * fac;
                double cs = 1.0 + (ratio - 1.0) * 0.85;
                double fade = std::min(1.0, n / 0.08);
                cs = 1.0 + (std::min(cs, 3.5) - 1.0) * fade;
                night_->csat[L][i] = (uint16_t)lround(cs * 256);
            }
        }
    }
}

// Apply PC-side picture adjustments (if any) and hand the frame out.
void Client::Emit(const Frame& in) {
    std::unique_lock<std::mutex> lk(adj_mu_);
    if (auto_bri_) {
        // Median brightness of the frame (sparse sample). The median ignores small bright spots - a lamp or a
        // window in a dark room no longer makes it think there is enough light.
        uint32_t hist[256] = {0};
        uint32_t cnt = 0;
        for (int y = 4; y < in.height; y += 12) {
            const uint8_t* row = in.y + (size_t)y * in.y_stride;
            for (int x = 4; x < in.width; x += 12) { hist[row[x]]++; cnt++; }
        }
        if (cnt) {
            uint32_t acc = 0;
            int med = 0;
            while (med < 255 && (acc += hist[med]) < cnt / 2) med++;
            double m = std::max(0.0, (med - 16.0) / 219.0);
            auto_median_ = m;
            // wakes up below 0.28, aims at 0.40 (natural, not over-lit), sleeps again once help isn't needed;
            // the gap between the thresholds stops it from switching on and off
            const double wake = 0.28, target = 0.33;
            if (!auto_awake_ && m < wake) auto_awake_ = true;
            double g = 1.0;
            if (auto_awake_) {
                double base = std::max(0.004, m - 0.012) / (1.0 - 0.012);
                g = std::max(1.0, std::min(10.0, target / base));  // up to ~10x, enough for a nearly dark room
                if (m >= target + 0.02 && auto_k_ < 1.05) auto_awake_ = false;
            }
            auto_k_ += (g - auto_k_) * 0.06;  // smooth (~0.3 s at 60 FPS)
            if (fabs(auto_k_ - auto_k_built_) > 0.03 * auto_k_built_) RebuildLuts();
        }
    }
    if (!adj_active_) {
        lk.unlock();
        on_frame_(in);
        return;
    }
    const int w = in.width, h = in.height;
    const size_t ysz = (size_t)w * h;
    adj_buf_.resize(ysz * 3 / 2);
    uint8_t* Y = adj_buf_.data();
    uint8_t* UV = Y + ysz;
    const int sharp = adj_.sharpness;
    uint8_t* yl = sharp ? (adj_tmp_.resize(ysz), adj_tmp_.data()) : Y;
    // Low light: noise reduction + local tone mapping + colour (replaces the plain tables below)
    const bool night = auto_k_built_ > 1.03 && night_;
    if (night) {
        night_->Process(in, yl, UV, auto_k_built_, auto_median_);
        if (adj_.saturation || adj_.temperature)
            for (size_t i = 0; i < ysz / 2; i += 2) { UV[i] = ulut_[UV[i]]; UV[i + 1] = vlut_[UV[i + 1]]; }
    } else {
        if (night_) night_->Reset();
    for (int y = 0; y < h; y++) {
        const uint8_t* s = in.y + (size_t)y * in.y_stride;
        uint8_t* d = yl + (size_t)y * w;
        for (int x = 0; x < w; x++) d[x] = ylut_[s[x]];
    }
    }
    if (sharp) {
        // unsharp mask with a 4-neighbour Laplacian, amount up to 1.5
        const int k = sharp * 384 / 100;  // x256
        memcpy(Y, yl, (size_t)w);
        memcpy(Y + (size_t)(h - 1) * w, yl + (size_t)(h - 1) * w, (size_t)w);
        for (int y = 1; y < h - 1; y++) {
            const uint8_t* c = yl + (size_t)y * w;
            uint8_t* d = Y + (size_t)y * w;
            d[0] = c[0];
            d[w - 1] = c[w - 1];
            for (int x = 1; x < w - 1; x++) {
                int lap = 4 * c[x] - c[x - 1] - c[x + 1] - c[x - w] - c[x + w];
                int v = c[x] + ((lap * k) >> 10);
                d[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
    const bool chroma_boost = auto_k_built_ > 1.005;
    for (int y = 0; y < (night ? 0 : h / 2); y++) {
        const uint8_t* s = in.uv + (size_t)y * in.uv_stride;
        const uint8_t* ly = in.y + (size_t)(2 * y) * in.y_stride;  // luma under this chroma sample
        uint8_t* d = UV + (size_t)y * w;
        if (!chroma_boost) {
            for (int x = 0; x < w; x += 2) {
                d[x] = ulut_[s[x]];
                d[x + 1] = vlut_[s[x + 1]];
            }
            continue;
        }
        for (int x = 0; x < w; x += 2) {
            int k = csat_[ly[x]];
            int u = 128 + (((s[x] - 128) * k) >> 8);
            int v = 128 + (((s[x + 1] - 128) * k) >> 8);
            d[x] = ulut_[u < 0 ? 0 : u > 255 ? 255 : u];
            d[x + 1] = vlut_[v < 0 ? 0 : v > 255 ? 255 : v];
        }
    }
    lk.unlock();
    Frame f;
    f.width = w;
    f.height = h;
    f.y = Y;
    f.uv = UV;
    f.y_stride = w;
    f.uv_stride = w;
    on_frame_(f);
}

// ---- colour conversion ----

namespace {
inline uint8_t Clamp(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

template <int BPP>
void Convert(const Frame& f, uint8_t* dst, int dw, int dh, bool bt709) {
    // limited range YUV -> RGB, fixed point (x1024)
    const int cy = 1192;  // 1.164
    const int crv = bt709 ? 1836 : 1634;
    const int cgu = bt709 ? 218 : 401;
    const int cgv = bt709 ? 546 : 832;
    const int cbu = bt709 ? 2163 : 2066;
    // Uniform scale: take the largest centered source rectangle with the destination's aspect ratio
    // (crops edges if the shapes differ, never stretches).
    int sw = f.width, sh = f.height, sx0 = 0, sy0 = 0;
    if ((int64_t)sw * dh > (int64_t)sh * dw) { sw = (int)((int64_t)sh * dw / dh); sx0 = (f.width - sw) / 2; }
    else if ((int64_t)sw * dh < (int64_t)sh * dw) { sh = (int)((int64_t)sw * dh / dw); sy0 = (f.height - sh) / 2; }
    std::vector<int> xm(dw);
    for (int x = 0; x < dw; x++) xm[x] = sx0 + (int)((int64_t)x * sw / dw);
    for (int y = 0; y < dh; y++) {
        int sy = sy0 + (int)((int64_t)y * sh / dh);
        const uint8_t* yr = f.y + (size_t)sy * f.y_stride;
        const uint8_t* uvr = f.uv + (size_t)(sy / 2) * f.uv_stride;
        uint8_t* d = dst + (size_t)y * dw * BPP;
        for (int x = 0; x < dw; x++) {
            int sx = xm[x];
            int Y = (yr[sx] - 16) * cy;
            int U = uvr[sx & ~1] - 128;
            int V = uvr[(sx & ~1) + 1] - 128;
            d[0] = Clamp((Y + cbu * U + 512) >> 10);
            d[1] = Clamp((Y - cgu * U - cgv * V + 512) >> 10);
            d[2] = Clamp((Y + crv * V + 512) >> 10);
            if (BPP == 4) d[3] = 255;
            d += BPP;
        }
    }
}
}  // namespace

void Nv12ToBgr24(const Frame& f, uint8_t* dst, int dw, int dh, bool bt709) { Convert<3>(f, dst, dw, dh, bt709); }
void Nv12ToBgra(const Frame& f, uint8_t* dst, int dw, int dh, bool bt709) { Convert<4>(f, dst, dw, dh, bt709); }

}  // namespace phonecam
