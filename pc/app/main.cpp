// PhoneCam for Windows: connects to the phone, shows a preview and feeds the "PhoneCam" virtual webcam.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../core/phonecam_client.h"
#include "../core/qr.h"
#include "../core/web_link.h"

using namespace phonecam;

namespace {

enum : int {
    IDC_WIFI = 100, IDC_USB, IDC_IP, IDC_FIND, IDC_CAMERA, IDC_RES, IDC_FPS, IDC_QUALITY,
    IDC_MIRROR, IDC_ROT, IDC_TORCH, IDC_ZOOM, IDC_CONNECT, IDC_VCAM, IDC_INSTALL, IDC_UNINSTALL,
    IDC_STATUS, IDC_VCAMSTATE, IDC_AF, IDC_HS, IDC_THEME, IDC_WARN, IDC_ORIENT, IDC_GEAR, IDC_WARN2, IDC_VCAMHINT, IDC_TG,
    // settings window
    IDS_EV = 130, IDS_EV_VAL, IDS_WB, IDS_EDGE, IDS_NR, IDS_AELOCK, IDS_AWBLOCK,
    IDS_BRI, IDS_BRI_VAL, IDS_CON, IDS_CON_VAL, IDS_SAT, IDS_SAT_VAL, IDS_GAM, IDS_GAM_VAL, IDS_SHP, IDS_SHP_VAL,
    IDS_RESET, IDS_DONE, IDS_TMP, IDS_TMP_VAL, IDS_AUTOBRI, IDS_VCAM_STATE, IDS_VCAM_BTN, IDS_VCAM_PROG,
};
constexpr UINT WM_APP_FRAME = WM_APP + 1;
constexpr UINT WM_APP_STATUS = WM_APP + 2;
constexpr UINT WM_APP_FOUND = WM_APP + 3;
constexpr UINT WM_APP_CAPS = WM_APP + 4;
constexpr UINT WM_APP_ORIENT = WM_APP + 5;
constexpr UINT WM_APP_CTRL = WM_APP + 6;
constexpr UINT WM_APP_VCAMDONE = WM_APP + 7;
constexpr UINT WM_APP_TRAY = WM_APP + 8;
constexpr UINT ID_TRAY_OPEN = 40001, ID_TRAY_SETTINGS = 40002, ID_TRAY_EXIT = 40003, ID_TRAY_BATTERY = 40004;
void SetBattery(int pct, bool charging);

const wchar_t* kVcamClsid = L"{BCCAFE7B-72EE-4201-B71A-72A05420E24D}";

struct Res { int w, h; const wchar_t* name; };
const Res kRes[] = {{640, 480, L"640 × 480"}, {1280, 720, L"1280 × 720 (HD)"}, {1920, 1080, L"1920 × 1080 (Full HD)"}};
const int kFps[] = {15, 24, 30, 60};
struct Quality { int kbps; const wchar_t* name; };
const Quality kQuality[] = {{2000, L"Низкое (2 Мбит/с)"}, {4000, L"Среднее (4 Мбит/с)"},
                            {8000, L"Высокое (8 Мбит/с)"}, {16000, L"Максимум (16 Мбит/с)"}};

// softcam sender API (loaded at runtime from PhoneCamVirtual64.dll)
using scCreateCamera_t = void*(__cdecl*)(int, int, float);
using scDeleteCamera_t = void(__cdecl*)(void*);
using scSendFrame_t = void(__cdecl*)(void*, const void*);
using scIsConnected_t = bool(__cdecl*)(void*);

struct App {
    HINSTANCE inst{};
    HWND wnd{};
    HFONT font{}, bold{}, big{};
    float dpi = 1.f;
    HWND h[64]{};
    bool chk[64]{};          // check state of our owner-drawn check boxes / radio buttons
    int kind[64]{};          // 0 = none, 1 = button, 2 = primary button, 3 = check, 4 = radio, 5 = icon button
    bool dark = true;
    HFONT title{}, symbol{};
    HICON logo{};
    HBRUSH br_bg{}, br_panel{}, br_input{};
    RECT ip_field{};
    HWND tooltip{};
    HWND make_parent{};     // parent for Make(); defaults to the main window
    HWND settings{};        // image settings window (null when closed)
    // phone camera controls
    int ev10 = 0;           // exposure compensation x10 (EV)
    int wb = 1, edge = -1, nr = -1;
    bool ae_lock = false, awb_lock = false;
    ImageAdjust adj;
    bool auto_bri = false;
    std::string wanted_side = "back";
    bool phone_found = false;   // the phone app answered a capability query
    // Telegram / browser mode
    std::string tg_code;        // pairing code (8 chars, no dash)
    std::wstring saved_ip;      // IP text while the field shows the code
    HWND ip_label{};
    bool tray_added = false;
    RECT battery_rc{};
    int battery = -1;             // phone battery %, -1 = unknown
    bool charging = false;
    bool bat_warned15 = false, bat_warned5 = false;
    bool tray_hint_shown = false;
    UINT taskbar_created = 0;   // Explorer restarted -> re-add the tray icon
    // virtual camera install / uninstall animation
    bool vcam_busy = false;
    bool vcam_installing = false;
    float vcam_fade = 1.0f;     // action button opacity 0..1
    int vcam_fade_dir = 0;      // -1 fading out, +1 fading in
    int vcam_blink = 0;         // remaining highlight blinks of the install button
    bool vcam_hl = false;
    std::wstring vcam_result;
    std::atomic<bool> portrait{false};   // video orientation reported by the phone
    int base_w = 1280, base_h = 720, base_fps = 30;
    RECT preview_rc{};

    Client client;
    std::atomic<bool> connected{false};
    std::wstring status = L"Не подключено";
    State state = State::Idle;

    // preview buffer
    std::mutex pmu;
    std::vector<uint8_t> pbuf;
    int pw = 0, ph = 0;
    int src_w = 0, src_h = 0;
    std::atomic<bool> frame_pending{false};
    DWORD last_preview = 0;

    // virtual cam
    HMODULE vdll{};
    scCreateCamera_t scCreate{};
    scDeleteCamera_t scDelete{};
    scSendFrame_t scSend{};
    scIsConnected_t scIsConn{};
    std::mutex vmu;
    void* vcam = nullptr;
    int vw = 0, vh = 0;
    std::atomic<bool> vcam_enabled{true};
    std::vector<uint8_t> vbuf;

    std::vector<FoundPhone> found;
    std::vector<CamCaps> caps;          // what the phone said its cameras can do (empty = unknown)
    std::string caps_host;
    int wanted_fps = 30;                // fps the user picked, kept even if the list is temporarily shorter
    std::atomic<bool> caps_busy{false};
    std::wstring ini;
} g;

int S(int v) { return (int)std::lround(v * g.dpi); }

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring ExeDir() {
    wchar_t b[MAX_PATH];
    GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s = b;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring GetText(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring s(n, 0);
    GetWindowTextW(w, &s[0], n + 1);
    return s;
}

// ---------- settings ----------
int IniInt(const wchar_t* k, int def) { return GetPrivateProfileIntW(L"phonecam", k, def, g.ini.c_str()); }
void IniSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"phonecam", k, v.c_str(), g.ini.c_str()); }

bool Checked(int id) { return g.chk[id - 100]; }
void SetCheck(int id, bool v) {
    g.chk[id - 100] = v;
    if (g.h[id - 100]) InvalidateRect(g.h[id - 100], nullptr, FALSE);
}

void RefreshCameraList();
std::string SelectedSide();

void LoadImageSettings() {
    g.ev10 = IniInt(L"ev10", 0);
    g.wb = IniInt(L"wb", 1);
    g.edge = IniInt(L"edge", -1);
    g.nr = IniInt(L"nr", -1);
    g.ae_lock = IniInt(L"aelock", 0) != 0;
    g.awb_lock = IniInt(L"awblock", 0) != 0;
    g.adj.brightness = IniInt(L"bri", 0);
    g.adj.contrast = IniInt(L"con", 0);
    g.adj.saturation = IniInt(L"sat", 0);
    g.adj.gamma = IniInt(L"gam", 100);
    g.adj.sharpness = IniInt(L"shp", 0);
    g.adj.temperature = IniInt(L"tmp", 0);
    g.auto_bri = IniInt(L"autobri", 0) != 0;
}

void SaveImageSettings() {
    IniSet(L"ev10", std::to_wstring(g.ev10));
    IniSet(L"wb", std::to_wstring(g.wb));
    IniSet(L"edge", std::to_wstring(g.edge));
    IniSet(L"nr", std::to_wstring(g.nr));
    IniSet(L"aelock", g.ae_lock ? L"1" : L"0");
    IniSet(L"awblock", g.awb_lock ? L"1" : L"0");
    IniSet(L"bri", std::to_wstring(g.adj.brightness));
    IniSet(L"con", std::to_wstring(g.adj.contrast));
    IniSet(L"sat", std::to_wstring(g.adj.saturation));
    IniSet(L"gam", std::to_wstring(g.adj.gamma));
    IniSet(L"shp", std::to_wstring(g.adj.sharpness));
    IniSet(L"tmp", std::to_wstring(g.adj.temperature));
    IniSet(L"autobri", g.auto_bri ? L"1" : L"0");
}

// Camera control lines for the phone (sent on connect and whenever something changes).
std::vector<std::string> CameraCommands() {
    char b[64];
    std::vector<std::string> v;
    snprintf(b, sizeof b, "EV %.1f", g.ev10 / 10.0);
    v.push_back(b);
    v.push_back("WB " + std::to_string(g.wb));
    v.push_back("EDGE " + std::to_string(g.edge));
    v.push_back("NR " + std::to_string(g.nr));
    v.push_back(std::string("AELOCK ") + (g.ae_lock ? "1" : "0"));
    v.push_back(std::string("AWBLOCK ") + (g.awb_lock ? "1" : "0"));
    return v;
}

void LoadSettings() {
    LoadImageSettings();
    SetCheck(IDS_AUTOBRI, g.auto_bri);
    wchar_t ip[64];
    GetPrivateProfileStringW(L"phonecam", L"ip", L"", ip, 64, g.ini.c_str());
    SetWindowTextW(g.h[IDC_IP - 100], ip);
    bool usb = IniInt(L"usb", 0) != 0;
    bool tg = IniInt(L"tg", 0) != 0;
    SetCheck(IDC_WIFI, !usb && !tg);
    SetCheck(IDC_USB, usb && !tg);
    SetCheck(IDC_TG, tg);
    {
        wchar_t code[16];
        GetPrivateProfileStringW(L"phonecam", L"tgcode", L"", code, 16, g.ini.c_str());
        g.tg_code = NormalizePairCode(U8(code));
        if (g.tg_code.empty()) {
            g.tg_code = NewPairCode();
            IniSet(L"tgcode", W(g.tg_code));
        }
    }
    {
        wchar_t side[16];
        GetPrivateProfileStringW(L"phonecam", L"camside", L"", side, 16, g.ini.c_str());
        // older settings stored an index: 0 = back, 1 = front
        g.wanted_side = side[0] ? U8(side) : (IniInt(L"camera", 0) == 1 ? "front" : "back");
        RefreshCameraList();
    }
    SendMessageW(g.h[IDC_RES - 100], CB_SETCURSEL, IniInt(L"res", 1), 0);
    g.wanted_fps = IniInt(L"fpsv", 30);
    SendMessageW(g.h[IDC_QUALITY - 100], CB_SETCURSEL, IniInt(L"quality", 2), 0);
    SetCheck(IDC_MIRROR, IniInt(L"mirror", 0));
    SetCheck(IDC_HS, IniInt(L"hs", 0));
    SetCheck(IDC_ROT, IniInt(L"rot180", 0));
}

int Sel(int id) { int s = (int)SendMessageW(g.h[id - 100], CB_GETCURSEL, 0, 0); return s < 0 ? 0 : s; }

void SaveSettings() {
    IniSet(L"ip", Checked(IDC_TG) ? g.saved_ip : GetText(g.h[IDC_IP - 100]));
    IniSet(L"usb", Checked(IDC_USB) ? L"1" : L"0");
    IniSet(L"tg", Checked(IDC_TG) ? L"1" : L"0");
    IniSet(L"tgcode", W(g.tg_code));
    IniSet(L"camside", W(SelectedSide()));
    IniSet(L"res", std::to_wstring(Sel(IDC_RES)));
    IniSet(L"fpsv", std::to_wstring(g.wanted_fps));
    IniSet(L"quality", std::to_wstring(Sel(IDC_QUALITY)));
    IniSet(L"mirror", Checked(IDC_MIRROR) ? L"1" : L"0");
    IniSet(L"hs", Checked(IDC_HS) ? L"1" : L"0");
    IniSet(L"rot180", Checked(IDC_ROT) ? L"1" : L"0");
}

// ---------- virtual camera ----------
bool VcamRegistered() {
    std::wstring key = std::wstring(L"CLSID\\") + kVcamClsid + L"\\InprocServer32";
    HKEY k;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegCloseKey(k);
        return true;
    }
    return false;
}

void UpdateVcamUi() {
    bool reg = VcamRegistered();
    if (g.h[IDC_VCAMHINT - 100]) ShowWindow(g.h[IDC_VCAMHINT - 100], reg ? SW_HIDE : SW_SHOW);
    if (HWND st = g.h[IDS_VCAM_STATE - 100]) {
        std::wstring t = !g.vcam_result.empty() ? g.vcam_result
            : reg ? L"Вебка «PhoneCam» установлена. Выбирай её в Discord, Zoom, Telegram, OBS и т.д. - картинка идёт, пока программа подключена к телефону."
                  : L"Вебка не установлена. Установи её, чтобы телефон появился как камера в Discord, Zoom и других программах.";
        SetWindowTextW(st, t.c_str());
    }
    if (HWND b = g.h[IDS_VCAM_BTN - 100]) {
        if (!g.vcam_busy) SetWindowTextW(b, reg ? L"Удалить вебку" : L"Установить вебку");
        g.kind[IDS_VCAM_BTN - 100] = reg ? 1 : 2;   // install is the primary action
        InvalidateRect(b, nullptr, FALSE);
    }
}

bool LoadVcamDll() {
    if (g.vdll) return true;
    g.vdll = LoadLibraryW((ExeDir() + L"PhoneCamVirtual64.dll").c_str());
    if (!g.vdll) return false;
    g.scCreate = (scCreateCamera_t)GetProcAddress(g.vdll, "scCreateCamera");
    g.scDelete = (scDeleteCamera_t)GetProcAddress(g.vdll, "scDeleteCamera");
    g.scSend = (scSendFrame_t)GetProcAddress(g.vdll, "scSendFrame");
    g.scIsConn = (scIsConnected_t)GetProcAddress(g.vdll, "scIsConnected");
    return g.scCreate && g.scDelete && g.scSend;
}

void VcamOpen(int w, int h, int fps) {
    std::lock_guard<std::mutex> lk(g.vmu);
    if (g.vcam && g.vw == w && g.vh == h) return;
    if (g.vcam) { g.scDelete(g.vcam); g.vcam = nullptr; }
    if (!LoadVcamDll()) return;
    g.vcam = g.scCreate(w, h, (float)fps);
    g.vw = w;
    g.vh = h;
    g.vbuf.assign((size_t)w * h * 3, 0);
}

void VcamClose() {
    std::lock_guard<std::mutex> lk(g.vmu);
    if (g.vcam) { g.scDelete(g.vcam); g.vcam = nullptr; }
}

bool RunElevated(const std::wstring& args) {
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g.settings ? g.settings : g.wnd;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    WaitForSingleObject(sei.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

struct VcamJob { bool install; HWND notify; };

DWORD WINAPI VcamThread(LPVOID arg) {
    std::unique_ptr<VcamJob> job((VcamJob*)arg);
    std::wstring d = ExeDir();
    std::wstring a64 = d + L"PhoneCamVirtual64.dll", a32 = d + L"PhoneCamVirtual32.dll";
    int result = 0;  // 1 ok, 0 failed, 2 dll missing
    if (GetFileAttributesW(a64.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result = 2;
    } else {
        std::wstring u = job->install ? L"" : L"/u ";
        std::wstring args = L"/c regsvr32 /s " + u + L"\"" + a64 + L"\"";
        if (GetFileAttributesW(a32.c_str()) != INVALID_FILE_ATTRIBUTES)
            args += L" && %SystemRoot%\\SysWOW64\\regsvr32.exe /s " + u + L"\"" + a32 + L"\"";
        result = RunElevated(args) ? 1 : 0;
    }
    Sleep(400);  // let the progress bar be seen even when it's instant
    PostMessageW(job->notify, WM_APP_VCAMDONE, job->install ? 1 : 0, result);
    return 0;
}

void StartVcamJob() {
    if (g.vcam_busy) return;
    g.vcam_busy = true;
    g.vcam_installing = !VcamRegistered();
    g.vcam_result.clear();
    if (!g.vcam_installing) VcamClose();
    g.vcam_fade_dir = -1;  // fade the button out; the progress bar takes over when it's gone
    SetTimer(g.settings, 1, 16, nullptr);
}

void OnVcamDone(bool install, int result) {
    g.vcam_busy = false;
    g.vcam_result = result == 2 ? L"Рядом с программой нет PhoneCamVirtual64.dll - переложи все файлы PhoneCam в одну папку."
                  : result == 0 ? (install ? L"Не получилось установить: нужны права администратора (нажми «Да» в окне Windows)."
                                           : L"Не получилось удалить: нужны права администратора.")
                  : install ? L"Готово! Вебка «PhoneCam» установлена. Если Discord/Zoom были открыты - перезапусти их."
                            : L"Вебка «PhoneCam» удалена из системы.";
    if (g.settings) {
        ShowWindow(g.h[IDS_VCAM_PROG - 100], SW_HIDE);
        ShowWindow(g.h[IDS_VCAM_BTN - 100], SW_SHOW);
        g.vcam_fade = 0;
        g.vcam_fade_dir = 1;
        SetTimer(g.settings, 1, 16, nullptr);
    }
    UpdateVcamUi();
}

// ---------- streaming ----------
// Conversion (vcam + preview) runs on its own thread so the network/decoder thread never waits for it.
// Only the newest frame matters: if conversion falls behind, older frames are simply skipped.
struct FrameSlot {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> nv12;
    int w = 0, h = 0;
    bool fresh = false;
    bool quit = false;
    std::thread worker;
} g_slot;

void ConvertFrame(const Frame& f) {
    // Vertical video: the virtual camera switches to the rotated size, so nothing is stretched or padded.
    bool portrait = f.height > f.width;
    if (portrait != g.portrait) {
        g.portrait = portrait;
        PostMessageW(g.wnd, WM_APP_ORIENT, 0, 0);
    }
    if (g.vcam_enabled && g.connected) {
        int want_w = portrait ? g.base_h : g.base_w, want_h = portrait ? g.base_w : g.base_h;
        if (g.vw != want_w || g.vh != want_h) VcamOpen(want_w, want_h, g.base_fps);
        std::lock_guard<std::mutex> lk(g.vmu);
        if (g.vcam) {
            // uniform scale; if the aspect ratio differs (e.g. 4:3 camera) the edges are cropped, never stretched
            Nv12ToBgr24(f, g.vbuf.data(), g.vw, g.vh, std::min(f.width, f.height) >= 720);
            g.scSend(g.vcam, g.vbuf.data());
        }
    }
    DWORD now = GetTickCount();
    // up to ~60 fps preview; frame_pending skips frames while the UI is still drawing the previous one
    if (now - g.last_preview < 14 || g.frame_pending || !IsWindowVisible(g.wnd) || IsIconic(g.wnd)) return;
    g.last_preview = now;
    RECT r = g.preview_rc;
    int aw = r.right - r.left, ah = r.bottom - r.top;
    if (aw <= 0 || ah <= 0) return;
    int w = aw, h = (int)((int64_t)aw * f.height / f.width);
    if (h > ah) { h = ah; w = (int)((int64_t)ah * f.width / f.height); }
    w &= ~3;
    if (w <= 0 || h <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g.pmu);
        g.pbuf.resize((size_t)w * h * 4);
        Nv12ToBgra(f, g.pbuf.data(), w, h, std::min(f.width, f.height) >= 720);
        g.pw = w;
        g.ph = h;
        g.src_w = f.width;
        g.src_h = f.height;
    }
    g.frame_pending = true;
    PostMessageW(g.wnd, WM_APP_FRAME, 0, 0);
}

void ConvertLoop() {
    std::vector<uint8_t> local;
    for (;;) {
        int w, h;
        {
            std::unique_lock<std::mutex> lk(g_slot.mu);
            g_slot.cv.wait(lk, [] { return g_slot.fresh || g_slot.quit; });
            if (g_slot.quit) return;
            local.swap(g_slot.nv12);
            w = g_slot.w;
            h = g_slot.h;
            g_slot.fresh = false;
        }
        Frame f;
        f.width = w;
        f.height = h;
        f.y = local.data();
        f.uv = local.data() + (size_t)w * h;
        f.y_stride = w;
        f.uv_stride = w;
        ConvertFrame(f);
    }
}

void OnFrame(const Frame& f) {
    std::lock_guard<std::mutex> lk(g_slot.mu);
    size_t ysz = (size_t)f.width * f.height;
    g_slot.nv12.resize(ysz * 3 / 2);
    uint8_t* d = g_slot.nv12.data();
    for (int y = 0; y < f.height; y++) memcpy(d + (size_t)y * f.width, f.y + (size_t)y * f.y_stride, f.width);
    uint8_t* duv = d + ysz;
    for (int y = 0; y < f.height / 2; y++) memcpy(duv + (size_t)y * f.width, f.uv + (size_t)y * f.uv_stride, f.width);
    g_slot.w = f.width;
    g_slot.h = f.height;
    g_slot.fresh = true;
    if (!g_slot.worker.joinable()) g_slot.worker = std::thread(ConvertLoop);
    g_slot.cv.notify_one();
}

void StopConverter() {
    {
        std::lock_guard<std::mutex> lk(g_slot.mu);
        g_slot.quit = true;
    }
    g_slot.cv.notify_one();
    if (g_slot.worker.joinable()) g_slot.worker.join();
}

void OnStatus(State s, const std::string& msg) {
    auto* p = new std::pair<State, std::wstring>(s, W(msg));
    PostMessageW(g.wnd, WM_APP_STATUS, 0, (LPARAM)p);
}

void RefreshFpsList();

// Camera settings are only usable once the phone answered (or while streaming).
void UpdateCameraSettingsEnabled() {
    bool idle = !g.connected;
    bool found = g.phone_found || g.connected;
    for (int id : {IDC_CAMERA, IDC_RES, IDC_FPS, IDC_QUALITY}) EnableWindow(g.h[id - 100], idle && g.phone_found);
    for (int id : {IDC_MIRROR, IDC_ROT, IDS_AUTOBRI}) EnableWindow(g.h[id - 100], found);
    if (idle) RefreshFpsList();  // also decides whether the high-speed box is available
    else EnableWindow(g.h[IDC_HS - 100], FALSE);
}

void SetControlsEnabled(bool idle) {
    for (int id : {IDC_WIFI, IDC_USB, IDC_TG, IDC_IP, IDC_FIND})
        EnableWindow(g.h[id - 100], idle && !(Checked(IDC_USB) && (id == IDC_IP || id == IDC_FIND)));
    UpdateCameraSettingsEnabled();
    EnableWindow(g.h[IDC_TORCH - 100], !idle);
    EnableWindow(g.h[IDC_ZOOM - 100], !idle);
    EnableWindow(g.h[IDC_AF - 100], !idle);
    EnableWindow(g.h[IDC_ORIENT - 100], !idle && !Checked(IDC_TG));  // the browser follows the phone's rotation itself
    InvalidateRect(g.wnd, &g.ip_field, FALSE);
    SetWindowTextW(g.h[IDC_CONNECT - 100], idle ? L"Подключить" : L"Отключить");
}

int SelectedFps() {
    int i = (int)SendMessageW(g.h[IDC_FPS - 100], CB_GETCURSEL, 0, 0);
    if (i < 0) return g.wanted_fps;
    return (int)SendMessageW(g.h[IDC_FPS - 100], CB_GETITEMDATA, i, 0);
}

const char* kSides[] = {"back", "wide", "front"};

std::string SelectedSide() {
    int i = (int)SendMessageW(g.h[IDC_CAMERA - 100], CB_GETCURSEL, 0, 0);
    if (i < 0) return g.wanted_side;
    int k = (int)SendMessageW(g.h[IDC_CAMERA - 100], CB_GETITEMDATA, i, 0);
    return k >= 0 && k < 3 ? kSides[k] : "back";
}

// Front camera has no flash: the same control switches the phone screen to white (screen light).
void UpdateTorchLabel() {
    if (HWND t = g.h[IDC_TORCH - 100]) {
        SetWindowTextW(t, SelectedSide() == "front" ? L"Подсветка" : L"Фонарик");
        InvalidateRect(t, nullptr, FALSE);
    }
}

// Camera list: "Широкоугольная" only appears when the phone said it has one.
void RefreshCameraList() {
    HWND cb = g.h[IDC_CAMERA - 100];
    bool wide = false;
    for (auto& c : g.caps) wide |= c.side == "wide";
    if (Checked(IDC_TG)) wide = true;  // the phone page uses it if the phone has one
    std::string keep = g.wanted_side;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    const wchar_t* names[] = {L"Основная (задняя)", L"Широкоугольная", L"Фронтальная"};
    int sel = 0;
    for (int k = 0; k < 3; k++) {
        if (k == 1 && !wide) continue;
        int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)names[k]);
        SendMessageW(cb, CB_SETITEMDATA, idx, k);
        if (keep == kSides[k]) sel = idx;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
    UpdateTorchLabel();
}

// Rebuild the FPS list from what the phone reported for the chosen camera + resolution.
void RefreshFpsList() {
    HWND cb = g.h[IDC_FPS - 100];
    const Res& r = kRes[Sel(IDC_RES)];
    const CamCaps* c = FindCaps(g.caps, SelectedSide(), r.w, r.h);
    int normal_max = c ? c->max_fps : 60;
    bool hs_ok = c ? (c->hs_fps >= 60 && c->max_fps < 60) : true;
    if (!g.connected) EnableWindow(g.h[IDC_HS - 100], hs_ok && g.phone_found);
    bool hs_on = hs_ok && Checked(IDC_HS);
    std::vector<int> list;
    for (int f : kFps)
        if (f <= normal_max || (f == 60 && hs_on)) list.push_back(f);
    if (list.empty()) list.push_back(normal_max);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    int sel = -1, best = -1;
    for (size_t i = 0; i < list.size(); i++) {
        std::wstring t = std::to_wstring(list[i]) + L" fps";
        if (list[i] == 60 && normal_max < 60) t += L" (скоростная)";
        int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        SendMessageW(cb, CB_SETITEMDATA, idx, list[i]);
        if (list[i] == g.wanted_fps) sel = idx;
        if (list[i] <= g.wanted_fps) best = idx;
    }
    if (sel < 0) sel = best >= 0 ? best : 0;
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

std::wstring CapsSummary() {
    const Res& r = kRes[Sel(IDC_RES)];
    const CamCaps* c = FindCaps(g.caps, SelectedSide(), r.w, r.h);
    if (!c) return L"";
    std::wstring s = L"Камера телефона в " + std::to_wstring(c->w) + L"×" + std::to_wstring(c->h) + L": до " +
                     std::to_wstring(c->max_fps) + L" к/с";
    if (c->max_fps < 60) s += c->hs_fps >= 60 ? L", 60 - через скоростную съёмку" : L", скоростной съёмки нет";
    return s;
}

struct CapsJob { std::string host; bool usb; };

DWORD WINAPI CapsThread(LPVOID arg) {
    std::unique_ptr<CapsJob> job((CapsJob*)arg);
    auto* res = new std::pair<std::string, std::vector<CamCaps>>();
    res->first = job->usb ? "usb" : job->host;
    std::string err;
    QueryCaps(job->host, kTcpPort, job->usb, res->second, err);
    PostMessageW(g.wnd, WM_APP_CAPS, 0, (LPARAM)res);
    return 0;
}

// Ask the phone (in the background) what its cameras can do.
void StartCapsQuery() {
    if (Checked(IDC_TG)) return;
    bool usb = Checked(IDC_USB);
    std::string host = U8(GetText(g.h[IDC_IP - 100]));
    while (!host.empty() && host.back() == ' ') host.pop_back();
    if (!usb && host.empty()) return;
    if (g.caps_busy.exchange(true)) return;
    auto* job = new CapsJob{host, usb};
    CloseHandle(CreateThread(nullptr, 0, CapsThread, job, 0, nullptr));
}

// What the QR code on the PC holds: a Telegram link that opens the bot's mini app with the code
// (or the plain web page, which also works in any phone browser).
std::string TgLink() {
    wchar_t bot[64];
    GetPrivateProfileStringW(L"phonecam", L"tgbot", W(kPhoneCamBot).c_str(), bot, 64, g.ini.c_str());
    return PairLink(g.tg_code, U8(bot));
}

std::wstring PrettyCode() { return W(g.tg_code.substr(0, 4) + "-" + g.tg_code.substr(4)); }

// Wi-Fi / USB / Telegram: what the address field and the buttons mean.
void ApplyMode() {
    bool tg = Checked(IDC_TG);
    HWND ip = g.h[IDC_IP - 100];
    bool showing_code = GetPropW(ip, L"pc_code") != nullptr;
    if (tg && !showing_code) {
        g.saved_ip = GetText(ip);
        SetPropW(ip, L"pc_code", (HANDLE)1);
        SetWindowTextW(ip, PrettyCode().c_str());
    } else if (!tg && showing_code) {
        RemovePropW(ip, L"pc_code");
        SetWindowTextW(ip, g.saved_ip.c_str());
    }
    SendMessageW(ip, EM_SETREADONLY, tg, 0);
    SetWindowTextW(g.ip_label, tg ? L"Код для телефона" : L"IP телефона (показан в приложении)");
    SetWindowTextW(g.h[IDC_FIND - 100], tg ? L"Новый код" : L"Найти");
    if (!g.connected) {
        EnableWindow(ip, !Checked(IDC_USB));
        EnableWindow(g.h[IDC_FIND - 100], !Checked(IDC_USB));
    }
    InvalidateRect(g.h[IDC_FIND - 100], nullptr, FALSE);
    InvalidateRect(g.ip_label, nullptr, TRUE);
    if (tg) {
        g.phone_found = true;  // no capability query: the phone page adapts to what is asked
        g.caps.clear();
        RefreshCameraList();
        UpdateCameraSettingsEnabled();
        SetCheck(IDC_HS, false);
        EnableWindow(g.h[IDC_HS - 100], FALSE);
        if (!g.connected) SetWindowTextW(g.h[IDC_STATUS - 100],
            L"Телеграм: подходит для iPhone и любого телефона. Нажми «Подключить», потом наведи камеру телефона на QR-код.");
    }
    InvalidateRect(g.wnd, &g.ip_field, TRUE);
    InvalidateRect(g.wnd, &g.preview_rc, FALSE);
}

void Connect() {
    if (g.connected) {
        g.client.Stop();
        g.connected = false;
        SetBattery(-1, false);
        VcamClose();
        SetControlsEnabled(true);
        g.status = L"Не подключено";
        SetWindowTextW(g.h[IDC_STATUS - 100], g.status.c_str());
        { std::lock_guard<std::mutex> lk(g.pmu); g.pw = g.ph = 0; }
        InvalidateRect(g.wnd, &g.preview_rc, FALSE);
        return;
    }
    Config c;
    c.web = Checked(IDC_TG);
    c.web_code = g.tg_code;
    c.web_link = TgLink();
    c.usb = Checked(IDC_USB);
    c.host = U8(GetText(g.h[IDC_IP - 100]));
    while (!c.host.empty() && c.host.back() == ' ') c.host.pop_back();
    if (!c.web && !c.usb && c.host.empty()) {
        MessageBoxW(g.wnd, L"Введи IP-адрес, который показывает приложение на телефоне, или нажми «Найти».",
                    L"PhoneCam", MB_ICONINFORMATION);
        return;
    }
    const Res& r = kRes[Sel(IDC_RES)];
    c.width = r.w;
    c.height = r.h;
    c.fps = SelectedFps();
    c.kbps = kQuality[Sel(IDC_QUALITY)].kbps;
    c.camera = SelectedSide();
    c.mirror = Checked(IDC_MIRROR);
    c.high_speed = !c.web && Checked(IDC_HS) && IsWindowEnabled(g.h[IDC_HS - 100]);
    c.rotate180 = Checked(IDC_ROT);
    SaveSettings();
    SetCheck(IDC_TORCH, false);
    SendMessageW(g.h[IDC_ZOOM - 100], TBM_SETPOS, TRUE, 10);
    g.base_w = r.w;
    g.base_h = r.h;
    g.base_fps = c.fps;
    c.init_commands = CameraCommands();
    g.client.SetAdjust(g.adj);
    g.client.SetExposureEv(g.ev10 / 10.0f);
    g.client.SetAutoBrightness(g.auto_bri);
    if (g.vcam_enabled) VcamOpen(r.w, r.h, c.fps);
    g.client.SetControlCallback([](const std::string& name, float v) {
        WPARAM k = name == "torch" ? 1 : name == "zoom" ? 2 : name == "battery" ? 3 : name == "charging" ? 4 : 0;
        if (k) PostMessageW(g.wnd, WM_APP_CTRL, k, (LPARAM)(int)lround(v * 10));
    });
    g.client.Start(c, OnFrame, OnStatus);
    g.connected = true;
    SetControlsEnabled(false);
}

DWORD WINAPI FindThread(LPVOID) {
    auto* v = new std::vector<FoundPhone>(Discover(1500));
    PostMessageW(g.wnd, WM_APP_FOUND, 0, (LPARAM)v);
    return 0;
}

// ---------- UI ----------
struct Theme {
    COLORREF bg, panel, text, sub, border, input, accent, accent_text, preview, preview_text, ok, err, idle, pressed;
};
const Theme kDark{RGB(17, 19, 24),   RGB(24, 27, 34),   RGB(232, 234, 240), RGB(140, 146, 162), RGB(52, 57, 70),
                  RGB(34, 38, 48),   RGB(76, 201, 140), RGB(10, 22, 16),    RGB(10, 12, 16),    RGB(120, 126, 142),
                  RGB(76, 201, 140), RGB(240, 110, 100), RGB(170, 176, 190), RGB(46, 51, 64)};
const Theme kLight{RGB(236, 238, 243), RGB(255, 255, 255), RGB(26, 29, 37),    RGB(98, 104, 118),  RGB(212, 216, 225),
                   RGB(245, 246, 249), RGB(28, 150, 94),   RGB(255, 255, 255), RGB(224, 227, 234), RGB(104, 110, 124),
                   RGB(22, 135, 80),   RGB(200, 60, 50),   RGB(80, 86, 100),   RGB(228, 231, 237)};
const Theme& T() { return g.dark ? kDark : kLight; }

constexpr int kPanelW = 320;   // left panel width (unscaled)
constexpr int kL = 20;         // left margin
constexpr int kW = 280;        // content width

const wchar_t* kHsTip =
    L"ЭКСПЕРИМЕНТАЛЬНЫЙ режим: 60 FPS через скоростную съёмку\n"
    L"(возможны баги и нестабильная работа)\n\n"
    L"Как работает: камера телефона включает режим замедленной съёмки (120 кадров/с), а телефон отдаёт "
    L"в видео только каждый второй кадр. Получаются честные 60 кадров/с, даже если обычный режим камеры "
    L"умеет только 30.\n\n"
    L"Когда включать: если нужна плавная картинка - движение, игры, стрим или запись в OBS на 60 FPS.\n\n"
    L"Минусы: при слабом освещении картинка темнее и шумнее (выдержка не длиннее 1/120 с), телефон сильнее "
    L"греется и быстрее разряжается.\n\n"
    L"Не нужно для Discord, Zoom и Telegram: они всё равно показывают вебку максимум в 30 кадров/с.";

void ApplyTitleBarTheme(HWND wnd = nullptr) {
    if (!wnd) wnd = g.wnd;
    BOOL d = g.dark ? TRUE : FALSE;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = (Fn)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute");
    if (!fn) return;
    if (FAILED(fn(wnd, 20, &d, sizeof d))) fn(wnd, 19, &d, sizeof d);  // DWMWA_USE_IMMERSIVE_DARK_MODE
}

void ApplyControlThemes() {
    using Fn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static Fn set = (Fn)GetProcAddress(LoadLibraryW(L"uxtheme.dll"), "SetWindowTheme");
    if (!set) return;
    for (int id : {IDC_CAMERA, IDC_RES, IDC_FPS, IDC_QUALITY, IDS_WB, IDS_EDGE, IDS_NR}) {
        HWND c = g.h[id - 100];
        if (!c || !IsWindow(c)) continue;
        set(c, g.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
        COMBOBOXINFO ci{sizeof ci};
        if (GetComboBoxInfo(c, &ci) && ci.hwndList) set(ci.hwndList, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    set(g.h[IDC_IP - 100], g.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
}

void MakeBrushes() {
    for (HBRUSH* b : {&g.br_bg, &g.br_panel, &g.br_input}) if (*b) DeleteObject(*b);
    g.br_bg = CreateSolidBrush(T().bg);
    g.br_panel = CreateSolidBrush(T().panel);
    g.br_input = CreateSolidBrush(T().input);
}

void SetTheme(bool dark) {
    g.dark = dark;
    MakeBrushes();
    ApplyTitleBarTheme();
    ApplyControlThemes();
    IniSet(L"theme", dark ? L"dark" : L"light");
    RedrawWindow(g.wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    if (g.settings) {
        ApplyTitleBarTheme(g.settings);
        RedrawWindow(g.settings, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
}

bool SystemPrefersDark() {
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
          HFONT f = nullptr, int kind = 0) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             g.make_parent ? g.make_parent : g.wnd,
                             (HMENU)(INT_PTR)id, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g.font), TRUE);
    if (id >= 100 && id < 164) { g.h[id - 100] = c; g.kind[id - 100] = kind; }
    return c;
}

// kind for statics: 0 normal text, 1 secondary text, 2 section header
HWND Label(const wchar_t* t, int x, int y, int w = kW, HFONT f = nullptr, int kind = 0, int h = 20) {
    HWND c = Make(L"STATIC", t, SS_LEFT, x, y, w, h, 0, f);
    SetPropW(c, L"pc_kind", (HANDLE)(INT_PTR)(kind + 1));
    return c;
}

HWND Button(int id, const wchar_t* text, int x, int y, int w, int h, bool primary = false, HFONT f = nullptr) {
    return Make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id, f, primary ? 2 : 1);
}
HWND CheckBox(int id, const wchar_t* text, int x, int y, int w) {
    return Make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 22, id, nullptr, 3);
}
HWND Radio(int id, const wchar_t* text, int x, int y, int w) {
    return Make(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, 22, id, nullptr, 4);
}
HWND Combo(int id, int x, int y, int w) {
    return Make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL,
                x, y, w, 240, id);
}

int TextWidth(HFONT f, const wchar_t* t) {
    HDC dc = GetDC(g.wnd);
    HFONT o = (HFONT)SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    SelectObject(dc, o);
    ReleaseDC(g.wnd, dc);
    return sz.cx;
}

void AddSmartBrightnessTip(HWND owner, HWND target) {
    HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, owner, nullptr, g.inst,
                              nullptr);
    static const wchar_t* tip =
        L"Умная яркость PhoneCam\n\n"
        L"Включается сама, только когда света реально не хватает: в тёмной комнате, в режиме 60 FPS, "
        L"на слабой камере или при зафиксированной экспозиции. Если камера телефона справляется сама, "
        L"ничего не делает.\n\n"
        L"Плавно вытягивает тени и средние тона, яркие места (лампы, окна) не трогает. Никогда не затемняет.\n\n"
        L"В темноте картинка становится светлее, но немного шумнее: это усиление на ПК, а не настоящий свет.";
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = (UINT_PTR)target;
    ti.lpszText = (LPWSTR)tip;
    SendMessageW(tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(tt, TTM_SETMAXTIPWIDTH, 0, S(360));
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_AUTOPOP, 60000);
    SendMessageW(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 300);
    SendMessageW(tt, WM_SETFONT, (WPARAM)g.font, TRUE);
}

void BuildUi() {
    const int L = kL;
    int y = 74;  // header (logo + name) is painted in WM_PAINT
    Button(IDC_THEME, L"", L + kW - 34, 22, 34, 34, false, g.symbol);
    g.kind[IDC_THEME - 100] = 5;
    Button(IDC_GEAR, L"", L + kW - 74, 22, 34, 34, false, g.symbol);
    g.kind[IDC_GEAR - 100] = 5;

    Label(L"Подключение", L, y, kW - 100, g.bold, 2);
    g.battery_rc = {S(L + kW - 100), S(y - 2), S(L + kW), S(y + 20)};
    y += 26;
    Radio(IDC_WIFI, L"Wi-Fi", L, y, 72);
    Radio(IDC_USB, L"USB", L + 80, y, 66);
    Radio(IDC_TG, L"Телеграм", L + 154, y, 126);
    y += 30;
    g.ip_label = Label(L"IP телефона (показан в приложении)", L, y, kW, nullptr, 1);
    y += 21;
    g.ip_field = {S(L), S(y), S(L + 190), S(y + 30)};
    Make(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, L + 8, y + 6, 176, 20, IDC_IP);
    Button(IDC_FIND, L"Найти", L + 198, y, 82, 30);
    y += 44;

    Label(L"Камера", L, y, kW, g.bold, 2);
    y += 26;
    Label(L"Камера телефона", L, y, 135, nullptr, 1);
    Label(L"Разрешение", L + 145, y, 135, nullptr, 1);
    y += 20;
    Combo(IDC_CAMERA, L, y, 135);  // filled by RefreshCameraList()
    HWND res = Combo(IDC_RES, L + 145, y, 135);
    for (auto& r : kRes) SendMessageW(res, CB_ADDSTRING, 0, (LPARAM)r.name);
    y += 36;
    Label(L"Кадров/с", L, y, 135, nullptr, 1);
    Label(L"Качество", L + 145, y, 135, nullptr, 1);
    y += 20;
    Combo(IDC_FPS, L, y, 135);  // filled by RefreshFpsList() from the phone's capabilities
    HWND q = Combo(IDC_QUALITY, L + 145, y, 135);
    for (auto& k : kQuality) SendMessageW(q, CB_ADDSTRING, 0, (LPARAM)k.name);
    y += 38;

    CheckBox(IDC_MIRROR, L"Отразить зеркально", L, y, 150);
    CheckBox(IDC_ROT, L"Повернуть 180°", L + 150, y, 130);
    y += 28;
    const wchar_t* sb = L"Умная яркость PhoneCam";
    int sbw = (int)(TextWidth(g.font, sb) / g.dpi) + 28;
    CheckBox(IDS_AUTOBRI, sb, L, y, sbw);
    HWND sbinfo = Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, L + sbw + 4, y + 2, 18, 18, IDC_WARN2);
    AddSmartBrightnessTip(g.wnd, sbinfo);
    y += 28;
    const wchar_t* hs = L"60 FPS через скоростную съёмку";
    int hsw = (int)(TextWidth(g.font, hs) / g.dpi) + 28;
    CheckBox(IDC_HS, hs, L, y, hsw);
    HWND warn = Make(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, L + hsw + 4, y + 2, 18, 18, IDC_WARN);
    y += 36;

    Label(L"Управление", L, y, kW, g.bold, 2);
    y += 26;
    Button(IDC_ORIENT, L"Горизонтально", L, y, 135, 30);
    Button(IDC_AF, L"Фокус", L + 145, y, 135, 30);
    y += 38;
    CheckBox(IDC_TORCH, L"Фонарик", L, y, 100);
    Label(L"Зум", L + 112, y + 2, 30, nullptr, 1);
    HWND z = Make(L"PhoneCamSlider", L"", WS_TABSTOP, L + 140, y - 4, 140, 30, IDC_ZOOM);
    SendMessageW(z, TBM_SETRANGE, TRUE, MAKELPARAM(10, 50));
    SendMessageW(z, TBM_SETPOS, TRUE, 10);
    y += 30;
    Label(L"Если фонарик или зум не отвечают - переподключись.", L, y, kW, nullptr, 1, 36);
    y += 40;

    Button(IDC_CONNECT, L"Подключить", L, y, kW, 42, true, g.bold);
    y += 52;
    Make(L"STATIC", L"Не подключено", SS_LEFT, L, y, kW, 84, IDC_STATUS);
    y += 88;

    // shown only while the virtual camera isn't installed; opens the settings with the install button highlighted
    Button(IDC_VCAMHINT, L"Вебка «PhoneCam» не установлена\nНажми, чтобы установить", L, y, kW, 50);
    g.kind[IDC_VCAMHINT - 100] = 7;

    // tooltip for the high-speed warning icon
    g.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.wnd, nullptr, g.inst,
                                nullptr);
    TOOLINFOW ti{};
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g.wnd;
    ti.uId = (UINT_PTR)warn;
    ti.lpszText = (LPWSTR)kHsTip;
    SendMessageW(g.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g.tooltip, TTM_SETMAXTIPWIDTH, 0, S(380));
    SendMessageW(g.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 60000);
    SendMessageW(g.tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
    SendMessageW(g.tooltip, WM_SETFONT, (WPARAM)g.font, TRUE);
}

void Layout() {
    RECT c;
    GetClientRect(g.wnd, &c);
    g.preview_rc = {S(kPanelW + 16), S(16), c.right - S(16), c.bottom - S(16)};
}

// ---- owner drawing ----
HPEN Pen(COLORREF c, int w = 1) { return CreatePen(PS_SOLID, w, c); }

COLORREF Mix(COLORREF a, COLORREF b, float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}


void RoundBox(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = Pen(border);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void DrawShield(HDC dc, int x, int y) {
    HICON ic = (HICON)LoadImageW(nullptr, MAKEINTRESOURCEW(32518), IMAGE_ICON, S(16), S(16), LR_SHARED);
    if (ic) DrawIconEx(dc, x, y, ic, S(16), S(16), 0, nullptr, DI_NORMAL);
}

void DrawButtonItem(DRAWITEMSTRUCT* di) {
    int id = (int)di->CtlID;
    int kind = (id >= 100 && id < 164) ? g.kind[id - 100] : 1;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    bool disabled = di->itemState & ODS_DISABLED, pressed = di->itemState & ODS_SELECTED;
    bool focus = di->itemState & ODS_FOCUS;
    const Theme& t = T();
    FillRect(dc, &r, g.br_panel);
    SetBkMode(dc, TRANSPARENT);
    wchar_t text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));

    if (kind == 3 || kind == 4) {
        bool on = Checked(id);
        int bs = S(16);
        RECT box{r.left + S(1), r.top + (r.bottom - r.top - bs) / 2, r.left + S(1) + bs, 0};
        box.bottom = box.top + bs;
        COLORREF fill = on ? (disabled ? t.border : t.accent) : t.input;
        COLORREF border = on ? fill : (focus ? t.accent : t.border);
        if (kind == 3) {
            RoundBox(dc, box, fill, border, S(5));
            if (on) {
                HPEN p = Pen(t.accent_text == RGB(255, 255, 255) || g.dark ? (g.dark ? RGB(10, 22, 16) : RGB(255, 255, 255)) : t.accent_text, S(2));
                HGDIOBJ op = SelectObject(dc, p);
                POINT pts[3] = {{box.left + S(4), box.top + S(8)}, {box.left + S(7), box.top + S(11)},
                                {box.left + S(12), box.top + S(5)}};
                Polyline(dc, pts, 3);
                SelectObject(dc, op);
                DeleteObject(p);
            }
        } else {
            HBRUSH b = CreateSolidBrush(t.input);
            HPEN p = Pen(on ? (disabled ? t.border : t.accent) : (focus ? t.accent : t.border), on ? S(2) : 1);
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
            Ellipse(dc, box.left, box.top, box.right, box.bottom);
            SelectObject(dc, op);
            DeleteObject(p);
            if (on) {
                HBRUSH dot = CreateSolidBrush(disabled ? t.border : t.accent);
                SelectObject(dc, dot);
                HPEN np = (HPEN)GetStockObject(NULL_PEN);
                op = SelectObject(dc, np);
                Ellipse(dc, box.left + S(4), box.top + S(4), box.right - S(4) + 1, box.bottom - S(4) + 1);
                SelectObject(dc, op);
                SelectObject(dc, b);
                DeleteObject(dot);
            }
            SelectObject(dc, ob);
            DeleteObject(b);
        }
        SetTextColor(dc, disabled ? t.sub : t.text);
        RECT tr = r;
        tr.left = box.right + S(8);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (kind == 7) {
        // warning card: yellow stripe on the left, two lines of text
        RoundBox(dc, r, pressed ? t.pressed : t.input, RGB(225, 170, 40), S(10));
        RECT stripe{r.left + S(1), r.top + S(8), r.left + S(5), r.bottom - S(8)};
        HBRUSH yb = CreateSolidBrush(RGB(245, 190, 40));
        FillRect(dc, &stripe, yb);
        DeleteObject(yb);
        SetTextColor(dc, t.text);
        RECT tr = r;
        tr.left += S(16);
        tr.right -= S(8);
        RECT calc = tr;
        int th = DrawTextW(dc, text, -1, &calc, DT_LEFT | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        tr.top = r.top + (r.bottom - r.top - th) / 2;
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    } else {
        bool primary = kind == 2;
        COLORREF fill = primary ? (disabled ? t.border : t.accent) : (pressed ? t.pressed : t.input);
        if (primary && pressed) fill = g.dark ? RGB(60, 170, 118) : RGB(22, 125, 78);
        COLORREF border = primary ? fill : (focus ? t.accent : t.border);
        COLORREF tcol = primary ? t.accent_text : (disabled ? t.sub : t.text);
        if (id == IDS_VCAM_BTN) {
            if (g.vcam_hl) { fill = RGB(245, 190, 40); border = RGB(255, 220, 110); tcol = RGB(40, 30, 0); }
            float a = g.vcam_fade;  // fade towards the panel colour
            fill = Mix(t.panel, fill, a);
            border = Mix(t.panel, border, a);
            tcol = Mix(t.panel, tcol, a);
        }
        RoundBox(dc, r, fill, border, S(10));
        SetTextColor(dc, tcol);
        if (kind == 5) {
            // theme toggle: sun in dark mode (switch to light), moon in light mode
            const wchar_t* glyph = id == IDC_GEAR ? L"⚙" : g.dark ? L"☀" : L"☾";
            DrawTextW(dc, glyph, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (id == IDS_VCAM_BTN) {
            int tw = TextWidth((HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0), text);
            int total = S(16) + S(6) + tw;
            int x = r.left + (r.right - r.left - total) / 2;
            DrawShield(dc, x, r.top + (r.bottom - r.top - S(16)) / 2);
            RECT tr = r;
            tr.left = x + S(22);
            DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    SelectObject(dc, of);
}

void DrawComboItem(DRAWITEMSTRUCT* di) {
    const Theme& t = T();
    HDC dc = di->hDC;
    bool field = di->itemState & ODS_COMBOBOXEDIT;
    bool sel = (di->itemState & ODS_SELECTED) && !field;
    bool disabled = di->itemState & ODS_DISABLED;
    HBRUSH b = CreateSolidBrush(sel ? t.accent : t.input);
    FillRect(dc, &di->rcItem, b);
    DeleteObject(b);
    if (di->itemID == (UINT)-1) return;
    wchar_t text[128] = {0};
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)text);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, sel ? t.accent_text : (disabled ? t.sub : t.text));
    RECT tr = di->rcItem;
    tr.left += S(6);
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DrawWarnIcon(DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, g.br_panel);
    int w = r.right - r.left, h = r.bottom - r.top;
    POINT tri[3] = {{r.left + w / 2, r.top + S(1)}, {r.right - S(1), r.bottom - S(2)}, {r.left + S(1), r.bottom - S(2)}};
    HBRUSH b = CreateSolidBrush(RGB(245, 190, 40));
    HPEN p = Pen(RGB(220, 160, 20));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    Polygon(dc, tri, 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 30, 0));
    HFONT of = (HFONT)SelectObject(dc, g.bold);
    RECT tr{r.left, r.top + S(3), r.right, r.bottom};
    DrawTextW(dc, L"!", 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    (void)h;
}

LRESULT TrackbarCustomDraw(NMCUSTOMDRAW* cd) {
    const Theme& t = T();
    if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    HWND tb = cd->hdr.hwndFrom;
    bool enabled = IsWindowEnabled(tb);
    RECT thumb{};
    SendMessageW(tb, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
    int cy = (thumb.top + thumb.bottom) / 2, tx = (thumb.left + thumb.right) / 2;
    if (cd->dwItemSpec == TBCD_CHANNEL) {
        RECT ch{cd->rc.left, cy - S(2), cd->rc.right, cy + S(2)};
        RoundBox(cd->hdc, ch, t.border, t.border, S(4));
        RECT act{cd->rc.left, cy - S(2), tx, cy + S(2)};
        if (enabled && act.right > act.left) RoundBox(cd->hdc, act, t.accent, t.accent, S(4));
        return CDRF_SKIPDEFAULT;
    }
    if (cd->dwItemSpec == TBCD_THUMB) {
        int rad = S(7);
        HBRUSH b = CreateSolidBrush(enabled ? t.accent : t.border);
        HPEN p = Pen(t.panel, S(2));
        HGDIOBJ ob = SelectObject(cd->hdc, b), op = SelectObject(cd->hdc, p);
        Ellipse(cd->hdc, tx - rad, cy - rad, tx + rad + 1, cy + rad + 1);
        SelectObject(cd->hdc, ob);
        SelectObject(cd->hdc, op);
        DeleteObject(b);
        DeleteObject(p);
        return CDRF_SKIPDEFAULT;
    }
    return CDRF_SKIPDEFAULT;
}


// ---------- animated progress bar ("Идёт установка…") ----------
LRESULT CALLBACK ProgressProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: SetTimer(w, 1, 16, nullptr); return 0;
    case WM_TIMER: {
        LONG_PTR ph = GetWindowLongPtrW(w, GWLP_USERDATA);
        SetWindowLongPtrW(w, GWLP_USERDATA, (ph + 1) % 1000);
        if (IsWindowVisible(w)) InvalidateRect(w, nullptr, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        const Theme& t = T();
        FillRect(dc, &r, g.br_panel);
        RoundBox(dc, r, t.input, t.border, S(10));
        // a soft accent block sliding left to right
        int phase = (int)GetWindowLongPtrW(w, GWLP_USERDATA);
        int W = r.right, seg = W * 2 / 5;
        int x = (int)((phase % 90) / 90.0 * (W + seg)) - seg;
        HRGN clip = CreateRoundRectRgn(r.left + 1, r.top + 1, r.right, r.bottom, S(10), S(10));
        SelectClipRgn(dc, clip);
        for (int i = 0; i < seg; i += 2) {
            float f = 1.0f - fabsf((i - seg / 2.0f) / (seg / 2.0f));  // brightest in the middle
            HBRUSH b = CreateSolidBrush(Mix(t.input, t.accent, f * 0.85f));
            RECT sr{x + i, r.top, x + i + 2, r.bottom};
            FillRect(dc, &sr, b);
            DeleteObject(b);
        }
        SelectClipRgn(dc, nullptr);
        DeleteObject(clip);
        wchar_t text[64];
        GetWindowTextW(w, text, 64);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, t.text);
        HFONT of = (HFONT)SelectObject(dc, g.bold);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(w, &ps);
        return 0;
    }
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ---------- our own slider control ----------
// Replaces the Windows trackbar (which repaints only pieces of itself and leaves artifacts when custom drawn).
// Speaks the same messages the code already uses: TBM_SETRANGE / TBM_SETPOS / TBM_GETPOS, and notifies the
// parent with WM_HSCROLL (TB_THUMBTRACK while dragging, TB_ENDTRACK when released).
struct SliderData { int lo = 0, hi = 100, pos = 0; bool drag = false; };

int SliderX(HWND w, const SliderData& d, int pos) {
    RECT r;
    GetClientRect(w, &r);
    int pad = S(9);
    int span = std::max(1, (int)(r.right - 2 * pad));
    return pad + (int)((int64_t)(pos - d.lo) * span / std::max(1, d.hi - d.lo));
}

int SliderPosFromX(HWND w, const SliderData& d, int x) {
    RECT r;
    GetClientRect(w, &r);
    int pad = S(9);
    int span = std::max(1, (int)(r.right - 2 * pad));
    int v = d.lo + (int)lround((double)(x - pad) * (d.hi - d.lo) / span);
    return std::max(d.lo, std::min(d.hi, v));
}

void SliderNotify(HWND w, int code) {
    SendMessageW(GetParent(w), WM_HSCROLL, MAKEWPARAM(code, 0), (LPARAM)w);
}

LRESULT CALLBACK SliderProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    auto* d = (SliderData*)GetWindowLongPtrW(w, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)new SliderData);
        return TRUE;
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(w, GWLP_USERDATA, 0);
        return 0;
    case TBM_SETRANGE:
        d->lo = (short)LOWORD(lp);
        d->hi = (short)HIWORD(lp);
        d->pos = std::max(d->lo, std::min(d->hi, d->pos));
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case TBM_SETPOS:
        d->pos = std::max(d->lo, std::min(d->hi, (int)lp));
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case TBM_GETPOS:
        return d->pos;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_ENABLE: case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(w);
        SetCapture(w);
        d->drag = true;
        int v = SliderPosFromX(w, *d, (short)LOWORD(lp));
        if (v != d->pos) { d->pos = v; InvalidateRect(w, nullptr, FALSE); SliderNotify(w, TB_THUMBTRACK); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (d->drag) {
            int v = SliderPosFromX(w, *d, (short)LOWORD(lp));
            if (v != d->pos) { d->pos = v; InvalidateRect(w, nullptr, FALSE); SliderNotify(w, TB_THUMBTRACK); }
        }
        return 0;
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (d->drag) {
            d->drag = false;
            if (m == WM_LBUTTONUP) ReleaseCapture();
            SliderNotify(w, TB_ENDTRACK);
        }
        return 0;
    case WM_MOUSEWHEEL: case WM_KEYDOWN: {
        int step = std::max(1, (d->hi - d->lo) / 20), delta = 0;
        if (m == WM_MOUSEWHEEL) delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? step : -step;
        else if (wp == VK_LEFT || wp == VK_DOWN) delta = -1;
        else if (wp == VK_RIGHT || wp == VK_UP) delta = 1;
        else if (wp == VK_HOME) delta = d->lo - d->pos;
        else if (wp == VK_END) delta = d->hi - d->pos;
        else return DefWindowProcW(w, m, wp, lp);
        int v = std::max(d->lo, std::min(d->hi, d->pos + delta));
        if (v != d->pos) {
            d->pos = v;
            InvalidateRect(w, nullptr, FALSE);
            SliderNotify(w, TB_THUMBTRACK);
            SliderNotify(w, TB_ENDTRACK);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(w, &ps);
        RECT r;
        GetClientRect(w, &r);
        // draw everything into a memory bitmap, then copy at once: no flicker, no leftovers
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, r.right, r.bottom);
        HGDIOBJ ob = SelectObject(dc, bmp);
        const Theme& t = T();
        FillRect(dc, &r, g.br_panel);
        bool en = IsWindowEnabled(w);
        int cy = r.bottom / 2, x0 = SliderX(w, *d, d->lo), x1 = SliderX(w, *d, d->hi), tx = SliderX(w, *d, d->pos);
        RECT ch{x0, cy - S(2), x1, cy + S(2)};
        RoundBox(dc, ch, t.border, t.border, S(4));
        // filled part: from the left end, or from the centre for sliders with a neutral middle (-100..100)
        int from = (d->lo < 0 && d->hi > 0) ? SliderX(w, *d, 0) : x0;
        RECT act{std::min(from, tx), cy - S(2), std::max(from, tx), cy + S(2)};
        if (en && act.right > act.left) RoundBox(dc, act, t.accent, t.accent, S(4));
        int rad = S(7);
        HBRUSH b = CreateSolidBrush(en ? t.accent : t.border);
        HPEN p = CreatePen(PS_SOLID, S(2), GetFocus() == w ? t.text : t.panel);
        HGDIOBJ obr = SelectObject(dc, b), op = SelectObject(dc, p);
        Ellipse(dc, tx - rad, cy - rad, tx + rad + 1, cy + rad + 1);
        SelectObject(dc, obr);
        SelectObject(dc, op);
        DeleteObject(b);
        DeleteObject(p);
        BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(w, &ps);
        return 0;
    }
    }
    return DefWindowProcW(w, m, wp, lp);
}

void PaintHeader(HDC dc) {
    const Theme& t = T();
    int x = S(kL), y = S(20);
    if (g.logo) DrawIconEx(dc, x, y, g.logo, S(38), S(38), 0, nullptr, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    HFONT of = (HFONT)SelectObject(dc, g.title);
    int tx = x + S(48);
    SetTextColor(dc, t.text);
    TextOutW(dc, tx, y - S(3), L"Phone", 5);
    SIZE sz{};
    GetTextExtentPoint32W(dc, L"Phone", 5, &sz);
    SetTextColor(dc, t.accent);
    TextOutW(dc, tx + sz.cx, y - S(3), L"Cam", 3);
    SelectObject(dc, g.font);
    SetTextColor(dc, t.sub);
    TextOutW(dc, tx + S(1), y + S(24), L"телефон как вебкамера", 21);
    SelectObject(dc, of);
}

void PaintIpField(HDC dc) {
    bool en = IsWindowEnabled(g.h[IDC_IP - 100]);
    RoundBox(dc, g.ip_field, T().input, GetFocus() == g.h[IDC_IP - 100] && en ? T().accent : T().border, S(10));
}

// Telegram mode, no picture yet: QR code + code + short instructions.
void PaintTgPairing(HDC dc, RECT r) {
    const Theme& t = T();
    QrCode q = MakeQr(TgLink());
    int aw = r.right - r.left, ah = r.bottom - r.top;
    const wchar_t* steps = g.connected
        ? L"Ждём телефон…\n\n1. Наведи камеру телефона на QR-код - откроется бот @phonecamweb_bot\n"
          L"2. Нажми «Запустить», потом «Открыть камеру»\n"
          L"3. Разреши доступ к камере\n"
          L"Или в боте: «Сканировать QR-код» / ввести код вручную"
        : L"Нажми «Подключить», потом на телефоне:\n\n1. Наведи камеру телефона на QR-код - откроется бот @phonecamweb_bot\n"
          L"2. Нажми «Запустить», потом «Открыть камеру»\n"
          L"3. Разреши доступ к камере\n"
          L"Или в боте: «Сканировать QR-код» / ввести код вручную";
    HFONT old = (HFONT)SelectObject(dc, g.font);
    RECT calc{0, 0, aw - S(60), 0};
    int th = DrawTextW(dc, steps, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    int code_h = S(40);
    int side = std::min(aw - S(80), ah - th - code_h - S(90));
    side = std::max(side, S(120));
    side = std::min(side, S(360));
    int total = side + S(16) + code_h + S(18) + th;
    int top = r.top + std::max(S(10), (ah - total) / 2);
    int left = r.left + (aw - side) / 2;
    RECT card{left, top, left + side, top + side};
    RoundBox(dc, card, RGB(255, 255, 255), RGB(255, 255, 255), S(14));
    if (q.size) {
        int quiet = 3;
        int mod = (side - S(8)) / (q.size + quiet * 2);
        int qs = mod * q.size;
        int qx = left + (side - qs) / 2, qy = top + (side - qs) / 2;
        HBRUSH dark = CreateSolidBrush(RGB(0, 0, 0));
        for (int y = 0; y < q.size; y++)
            for (int x = 0; x < q.size; x++)
                if (q.At(x, y)) {
                    RECT m{qx + x * mod, qy + y * mod, qx + (x + 1) * mod, qy + (y + 1) * mod};
                    FillRect(dc, &m, dark);
                }
        DeleteObject(dark);
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.title);
    SetTextColor(dc, t.text);
    std::wstring code = PrettyCode();
    RECT cr{r.left, top + side + S(16), r.right, top + side + S(16) + code_h};
    DrawTextW(dc, code.c_str(), -1, &cr, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, g.font);
    SetTextColor(dc, t.preview_text);
    RECT tr{r.left + S(30), cr.bottom + S(18), r.right - S(30), r.bottom};
    DrawTextW(dc, steps, -1, &tr, DT_CENTER | DT_WORDBREAK);
    SelectObject(dc, old);
}

void PaintPreview(HDC dc) {
    const Theme& t = T();
    RECT r = g.preview_rc;
    HBRUSH bg = CreateSolidBrush(t.preview);
    std::lock_guard<std::mutex> lk(g.pmu);
    int aw = r.right - r.left, ah = r.bottom - r.top;
    if (g.pw > 0 && g.ph > 0) {
        int x = r.left + (aw - g.pw) / 2, y = r.top + (ah - g.ph) / 2;
        RECT b1{r.left, r.top, r.right, y}, b2{r.left, y + g.ph, r.right, r.bottom};
        RECT b3{r.left, y, x, y + g.ph}, b4{x + g.pw, y, r.right, y + g.ph};
        for (RECT* rr : {&b1, &b2, &b3, &b4}) FillRect(dc, rr, bg);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = g.pw;
        bi.bmiHeader.biHeight = -g.ph;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetDIBitsToDevice(dc, x, y, g.pw, g.ph, 0, 0, 0, g.ph, g.pbuf.data(), &bi, DIB_RGB_COLORS);
    } else if (Checked(IDC_TG)) {
        FillRect(dc, &r, bg);
        PaintTgPairing(dc, r);
    } else {
        FillRect(dc, &r, bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, t.preview_text);
        HFONT old = (HFONT)SelectObject(dc, g.font);
        const wchar_t* tx = g.connected
            ? L"Ждём картинку с телефона…"
            : L"1. Открой PhoneCam на телефоне (телефон и ПК в одной Wi-Fi сети)\n"
              L"2. Впиши IP с экрана телефона или нажми «Найти»\n"
              L"3. Нажми «Подключить»\n\n"
              L"Для USB: включи на телефоне «Отладку по USB» и положи adb.exe рядом с программой.";
        RECT tr = r;
        InflateRect(&tr, -S(30), -S(30));
        RECT calc = tr;
        int th = DrawTextW(dc, tx, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        tr.top = r.top + (ah - th) / 2;
        DrawTextW(dc, tx, -1, &tr, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, old);
    }
    DeleteObject(bg);
}

void UpdateOrientButton() {
    SetWindowTextW(g.h[IDC_ORIENT - 100], g.portrait ? L"Вертикально" : L"Горизонтально");
    InvalidateRect(g.h[IDC_ORIENT - 100], nullptr, FALSE);
}


// ---------- image settings window (gear button) ----------
struct Choice { int value; const wchar_t* name; };
const Choice kWb[] = {{1, L"Авто"}, {5, L"Дневной свет"}, {6, L"Облачно"}, {2, L"Лампа накаливания"},
                      {3, L"Люминесцентная лампа"}};
const Choice kEdge[] = {{-1, L"Как в камере"}, {0, L"Выключена"}, {1, L"Обычная"}, {2, L"Высокая"}};
const Choice kNr[] = {{-1, L"Как в камере"}, {0, L"Выключено"}, {1, L"Обычное"}, {2, L"Сильное"}};

constexpr int kSetW = 420;

void FillChoice(int id, const Choice* c, int n, int cur) {
    HWND cb = g.h[id - 100];
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    int sel = 0;
    for (int i = 0; i < n; i++) {
        int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)c[i].name);
        SendMessageW(cb, CB_SETITEMDATA, idx, c[i].value);
        if (c[i].value == cur) sel = idx;
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

int ChoiceValue(int id) {
    int i = (int)SendMessageW(g.h[id - 100], CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (int)SendMessageW(g.h[id - 100], CB_GETITEMDATA, i, 0);
}

HWND Slider(int id, int x, int y, int w, int lo, int hi, int v) {
    HWND t = Make(L"PhoneCamSlider", L"", WS_TABSTOP, x, y, w, 30, id);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(t, TBM_SETPOS, TRUE, v);
    return t;
}

int SliderPos(int id) { return (int)SendMessageW(g.h[id - 100], TBM_GETPOS, 0, 0); }

void UpdateValueLabels() {
    wchar_t b[32];
    swprintf(b, 32, L"%+.1f EV", g.ev10 / 10.0);
    SetWindowTextW(g.h[IDS_EV_VAL - 100], b);
    swprintf(b, 32, L"%+d", g.adj.brightness);
    SetWindowTextW(g.h[IDS_BRI_VAL - 100], b);
    swprintf(b, 32, L"%+d", g.adj.contrast);
    SetWindowTextW(g.h[IDS_CON_VAL - 100], b);
    swprintf(b, 32, L"%+d", g.adj.saturation);
    SetWindowTextW(g.h[IDS_SAT_VAL - 100], b);
    swprintf(b, 32, L"%.2f", g.adj.gamma / 100.0);
    SetWindowTextW(g.h[IDS_GAM_VAL - 100], b);
    swprintf(b, 32, g.adj.temperature < 0 ? L"холодн. %d" : g.adj.temperature > 0 ? L"тёпл. %d" : L"0", abs(g.adj.temperature));
    SetWindowTextW(g.h[IDS_TMP_VAL - 100], b);
    swprintf(b, 32, L"%d", g.adj.sharpness);
    SetWindowTextW(g.h[IDS_SHP_VAL - 100], b);
}

void SendCameraControls() {
    if (!g.connected) return;
    for (auto& c : CameraCommands()) g.client.SendCommand(c);
}

void SyncSettingsControls() {
    SendMessageW(g.h[IDS_EV - 100], TBM_SETPOS, TRUE, g.ev10);
    FillChoice(IDS_WB, kWb, 5, g.wb);
    FillChoice(IDS_EDGE, kEdge, 4, g.edge);
    FillChoice(IDS_NR, kNr, 4, g.nr);
    SetCheck(IDS_AELOCK, g.ae_lock);
    SetCheck(IDS_AWBLOCK, g.awb_lock);
    SendMessageW(g.h[IDS_BRI - 100], TBM_SETPOS, TRUE, g.adj.brightness);
    SendMessageW(g.h[IDS_CON - 100], TBM_SETPOS, TRUE, g.adj.contrast);
    SendMessageW(g.h[IDS_SAT - 100], TBM_SETPOS, TRUE, g.adj.saturation);
    SendMessageW(g.h[IDS_GAM - 100], TBM_SETPOS, TRUE, g.adj.gamma);
    SendMessageW(g.h[IDS_SHP - 100], TBM_SETPOS, TRUE, g.adj.sharpness);
    SendMessageW(g.h[IDS_TMP - 100], TBM_SETPOS, TRUE, g.adj.temperature);
    UpdateValueLabels();
}

void BuildSettingsUi(HWND wnd) {
    g.make_parent = wnd;
    const int L = 20, LW = 128, SX = 140, SW = 196, VX = 344;
    int y = 16;
    Label(L"Камера телефона", L, y, 360, g.bold, 2);
    y += 28;
    Label(L"Экспозиция", L, y + 4, LW);
    Slider(IDS_EV, SX, y, SW, -30, 30, 0);
    Make(L"STATIC", L"", SS_LEFT, VX, y + 4, 60, 20, IDS_EV_VAL);
    SetPropW(g.h[IDS_EV_VAL - 100], L"pc_kind", (HANDLE)(INT_PTR)2);
    y += 36;
    Label(L"Баланс белого", L, y + 3, LW);
    Combo(IDS_WB, SX, y, 250);
    y += 34;
    Label(L"Резкость камеры", L, y + 3, LW);
    Combo(IDS_EDGE, SX, y, 250);
    y += 34;
    Label(L"Шумоподавление", L, y + 3, LW);
    Combo(IDS_NR, SX, y, 250);
    y += 36;
    CheckBox(IDS_AELOCK, L"Зафиксировать экспозицию", L, y, 360);
    y += 26;
    CheckBox(IDS_AWBLOCK, L"Зафиксировать баланс белого", L, y, 360);
    y += 28;
    Label(L"Это делает сама камера телефона, поэтому качество не теряется. Если телефон что-то не "
          L"поддерживает, настройка просто не сработает. Резкость и шумоподавление не работают в режиме 60 FPS.",
          L, y, 380, nullptr, 1, 52);
    y += 64;

    Label(L"Обработка на ПК", L, y, 360, g.bold, 2);
    y += 28;
    struct Row { const wchar_t* name; int id, val, lo, hi; };
    Row rows[] = {{L"Яркость", IDS_BRI, IDS_BRI_VAL, -100, 100}, {L"Контраст", IDS_CON, IDS_CON_VAL, -100, 100},
                  {L"Насыщенность", IDS_SAT, IDS_SAT_VAL, -100, 100}, {L"Гамма", IDS_GAM, IDS_GAM_VAL, 50, 200},
                  {L"Температура", IDS_TMP, IDS_TMP_VAL, -100, 100}, {L"Резкость", IDS_SHP, IDS_SHP_VAL, 0, 100}};
    for (auto& r : rows) {
        Label(r.name, L, y + 4, LW);
        Slider(r.id, SX, y, SW, r.lo, r.hi, 0);
        Make(L"STATIC", L"", SS_LEFT, VX - 4, y + 4, 74, 20, r.val);
        SetPropW(g.h[r.val - 100], L"pc_kind", (HANDLE)(INT_PTR)2);
        y += 34;
    }
    Label(L"Применяется к вебке «PhoneCam» и превью. Работает с любым телефоном.", L, y, 380, nullptr, 1, 36);
    y += 40;
    Label(L"Виртуальная вебка", L, y, 360, g.bold, 2);
    y += 26;
    Make(L"STATIC", L"", SS_LEFT, L, y, 380, 40, IDS_VCAM_STATE);
    SetPropW(g.h[IDS_VCAM_STATE - 100], L"pc_kind", (HANDLE)(INT_PTR)2);
    y += 44;
    Button(IDS_VCAM_BTN, L"", L, y, 380, 36);
    Make(L"PhoneCamProgress", L"", 0, L, y, 380, 36, IDS_VCAM_PROG);
    ShowWindow(g.h[IDS_VCAM_PROG - 100], g.vcam_busy ? SW_SHOW : SW_HIDE);
    ShowWindow(g.h[IDS_VCAM_BTN - 100], g.vcam_busy ? SW_HIDE : SW_SHOW);
    if (g.vcam_busy) SetWindowTextW(g.h[IDS_VCAM_PROG - 100], g.vcam_installing ? L"Идёт установка…" : L"Идёт удаление…");
    g.vcam_fade = 1.0f;
    y += 50;
    Button(IDS_RESET, L"Сбросить всё", L, y, 150, 34);
    Button(IDS_DONE, L"Готово", kSetW - L - 150, y, 150, 34, true, g.bold);
    g.make_parent = nullptr;
    ApplyControlThemes();
    SyncSettingsControls();
    UpdateVcamUi();
}

void OnSettingsChanged(bool camera) {
    SaveImageSettings();
    UpdateValueLabels();
    g.client.SetExposureEv(g.ev10 / 10.0f);
    if (camera) SendCameraControls();
    else g.client.SetAdjust(g.adj);
}

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp);

LRESULT CALLBACK SettingsProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE:
        g.settings = w;
        ApplyTitleBarTheme(w);
        BuildSettingsUi(w);
        return 0;
    case WM_ERASEBKGND: {
        RECT c;
        GetClientRect(w, &c);
        FillRect((HDC)wp, &c, g.br_panel);
        return 1;
    }
    case WM_DRAWITEM: case WM_MEASUREITEM: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORSTATIC:
        return WndProc(g.wnd, m, wp, lp);
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->code == NM_CUSTOMDRAW) {
            wchar_t cls[32];
            GetClassNameW(nh->hwndFrom, cls, 32);
            if (wcscmp(cls, TRACKBAR_CLASSW) == 0) return TrackbarCustomDraw((NMCUSTOMDRAW*)lp);
        }
        return 0;
    }
    case WM_HSCROLL: {
        HWND t = (HWND)lp;
        int id = GetDlgCtrlID(t);
        int v = (int)SendMessageW(t, TBM_GETPOS, 0, 0);
        bool cam = false;
        switch (id) {
        case IDS_EV: g.ev10 = v; cam = true; break;
        case IDS_BRI: g.adj.brightness = v; break;
        case IDS_CON: g.adj.contrast = v; break;
        case IDS_SAT: g.adj.saturation = v; break;
        case IDS_GAM: g.adj.gamma = v; break;
        case IDS_SHP: g.adj.sharpness = v; break;
        case IDS_TMP: g.adj.temperature = v; break;
        default: return 0;
        }
        // camera exposure is sent when the slider is released, so the phone isn't flooded
        if (cam && LOWORD(wp) != TB_ENDTRACK && LOWORD(wp) != TB_THUMBPOSITION) { UpdateValueLabels(); return 0; }
        OnSettingsChanged(cam);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= 100 && id < 164 && HIWORD(wp) == BN_CLICKED && g.kind[id - 100] == 3) SetCheck(id, !Checked(id));
        switch (id) {
        case IDS_AELOCK: g.ae_lock = Checked(IDS_AELOCK); OnSettingsChanged(true); break;

        case IDS_AWBLOCK: g.awb_lock = Checked(IDS_AWBLOCK); OnSettingsChanged(true); break;
        case IDS_WB: if (HIWORD(wp) == CBN_SELCHANGE) { g.wb = ChoiceValue(IDS_WB); OnSettingsChanged(true); } break;
        case IDS_EDGE: if (HIWORD(wp) == CBN_SELCHANGE) { g.edge = ChoiceValue(IDS_EDGE); OnSettingsChanged(true); } break;
        case IDS_NR: if (HIWORD(wp) == CBN_SELCHANGE) { g.nr = ChoiceValue(IDS_NR); OnSettingsChanged(true); } break;
        case IDS_RESET:
            g.ev10 = 0; g.wb = 1; g.edge = -1; g.nr = -1; g.ae_lock = g.awb_lock = false;
            g.adj = ImageAdjust();
            SyncSettingsControls();
            OnSettingsChanged(true);
            OnSettingsChanged(false);
            break;
        case IDS_DONE: DestroyWindow(w); break;
        case IDS_VCAM_BTN: StartVcamJob(); break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {  // fade of the install/uninstall button
            g.vcam_fade += g.vcam_fade_dir * 0.08f;
            if (g.vcam_fade <= 0.0f && g.vcam_fade_dir < 0) {
                g.vcam_fade = 0.0f;
                g.vcam_fade_dir = 0;
                KillTimer(w, 1);
                ShowWindow(g.h[IDS_VCAM_BTN - 100], SW_HIDE);
                SetWindowTextW(g.h[IDS_VCAM_PROG - 100], g.vcam_installing ? L"Идёт установка…" : L"Идёт удаление…");
                ShowWindow(g.h[IDS_VCAM_PROG - 100], SW_SHOW);
                CloseHandle(CreateThread(nullptr, 0, VcamThread, new VcamJob{g.vcam_installing, w}, 0, nullptr));
            } else if (g.vcam_fade >= 1.0f && g.vcam_fade_dir > 0) {
                g.vcam_fade = 1.0f;
                g.vcam_fade_dir = 0;
                KillTimer(w, 1);
            }
            InvalidateRect(g.h[IDS_VCAM_BTN - 100], nullptr, FALSE);
        } else if (wp == 2) {  // highlight blinks of the install button
            g.vcam_hl = !g.vcam_hl;
            if (!g.vcam_hl && --g.vcam_blink <= 0) KillTimer(w, 2);
            InvalidateRect(g.h[IDS_VCAM_BTN - 100], nullptr, FALSE);
        }
        return 0;
    case WM_APP_VCAMDONE:
        OnVcamDone(wp == 1, (int)lp);
        return 0;
    case WM_CLOSE:
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        for (int id = IDS_EV; id <= IDS_VCAM_PROG; id++) g.h[id - 100] = nullptr;
        g.vcam_hl = false;
        g.vcam_blink = 0;
        g.settings = nullptr;
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

void OpenSettings(bool highlight_install = false) {
    if (!g.settings) {
    RECT r{0, 0, S(kSetW), S(748)};
    AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    RECT mw;
    GetWindowRect(g.wnd, &mw);
    CreateWindowExW(WS_EX_DLGMODALFRAME, L"PhoneCamSettings", L"Настройки изображения",
                    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_CLIPCHILDREN,
                    mw.left + S(60), mw.top + S(30), r.right - r.left, r.bottom - r.top, g.wnd, nullptr, g.inst,
                    nullptr);
    }
    SetForegroundWindow(g.settings);
    if (highlight_install && !VcamRegistered() && !g.vcam_busy) {
        g.vcam_blink = 4;
        g.vcam_hl = false;
        SetTimer(g.settings, 2, 260, nullptr);
        SetFocus(g.h[IDS_VCAM_BTN - 100]);
    }
}


// ---------- phone battery ----------
std::wstring BatteryText() {
    std::wstring t = std::to_wstring(g.battery) + L"%";
    if (g.charging) t += L" (заряжается)";
    return t;
}

void TrayUpdate(bool add, const wchar_t* balloon, const wchar_t* title);

void SetBattery(int pct, bool charging) {
    bool changed = pct != g.battery || charging != g.charging;
    g.battery = pct;
    g.charging = charging;
    if (pct < 0 || charging) { g.bat_warned15 = g.bat_warned5 = false; }
    else if (pct > 20) g.bat_warned15 = g.bat_warned5 = false;
    if (pct >= 0 && !charging && pct <= 5 && !g.bat_warned5) {
        g.bat_warned5 = g.bat_warned15 = true;
        TrayUpdate(false, L"Телефон почти разряжен - поставь его на зарядку, иначе трансляция скоро прервётся.",
                   L"PhoneCam: батарея телефона почти села");
    } else if (pct >= 0 && !charging && pct <= 15 && !g.bat_warned15) {
        g.bat_warned15 = true;
        TrayUpdate(false, (L"Осталось " + std::to_wstring(pct) + L"%. Поставь телефон на зарядку.").c_str(),
                   L"PhoneCam: батарея телефона садится");
    } else if (changed) {
        TrayUpdate(false, nullptr, nullptr);
    }
    if (changed && g.wnd) InvalidateRect(g.wnd, &g.battery_rc, FALSE);
}

// Battery icon + percent, right-aligned in the "Подключение" row of the main window.
void PaintBattery(HDC dc) {
    if (g.battery < 0) return;
    const Theme& t = T();
    RECT r = g.battery_rc;
    int bw = S(24), bh = S(12), nub = S(3);
    int bx = r.right - bw - nub, by = (r.top + r.bottom - bh) / 2;
    COLORREF lvl = g.charging || g.battery > 20 ? t.ok : g.battery > 10 ? RGB(232, 168, 40) : t.err;
    // body outline
    HPEN pen = Pen(t.sub, S(1) < 1 ? 1 : S(1));
    HPEN op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, bx, by, bx + bw, by + bh, S(4), S(4));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    RECT nr{bx + bw, by + bh / 2 - S(3), bx + bw + nub, by + bh / 2 + S(3)};
    HBRUSH sb = CreateSolidBrush(t.sub);
    FillRect(dc, &nr, sb);
    DeleteObject(sb);
    // fill
    int in = S(2);
    int fw = (bw - 2 * in) * std::max(0, std::min(100, g.battery)) / 100;
    if (fw < S(2) && g.battery > 0) fw = S(2);
    RECT fr{bx + in, by + in, bx + in + fw, by + bh - in};
    HBRUSH fb = CreateSolidBrush(lvl);
    FillRect(dc, &fr, fb);
    DeleteObject(fb);
    if (g.charging) {
        // lightning bolt over the body
        int cx = bx + bw / 2, cy = by + bh / 2;
        POINT b[6] = {{cx + S(2), by - S(2)}, {cx - S(4), cy + S(1)}, {cx, cy + S(1)},
                      {cx - S(2), by + bh + S(2)}, {cx + S(4), cy - S(1)}, {cx, cy - S(1)}};
        HPEN bp = Pen(t.bg, 1);
        HBRUSH bb = CreateSolidBrush(t.text);
        op = (HPEN)SelectObject(dc, bp);
        ob = (HBRUSH)SelectObject(dc, bb);
        Polygon(dc, b, 6);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(bb);
        DeleteObject(bp);
    }
    std::wstring txt = std::to_wstring(g.battery) + L"%";
    SetBkMode(dc, TRANSPARENT);
    HFONT of = (HFONT)SelectObject(dc, g.font);
    SetTextColor(dc, g.charging || g.battery > 20 ? t.sub : lvl);
    RECT tr{r.left, r.top, bx - S(6), r.bottom};
    DrawTextW(dc, txt.c_str(), (int)txt.size(), &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// ---------- tray icon ----------
void TrayUpdate(bool add, const wchar_t* balloon = nullptr, const wchar_t* title = nullptr) {
    NOTIFYICONDATAW nd{};
    nd.cbSize = sizeof nd;
    nd.hWnd = g.wnd;
    nd.uID = 1;
    nd.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nd.uCallbackMessage = WM_APP_TRAY;
    nd.hIcon = (HICON)LoadImageW(g.inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
    std::wstring tip = g.connected ? (g.state == State::Streaming ? L"PhoneCam - идёт трансляция" : L"PhoneCam - подключение…")
                                   : L"PhoneCam - не подключено";
    if (g.battery >= 0) tip += L"\nБатарея телефона: " + BatteryText();
    wcsncpy(nd.szTip, tip.c_str(), 127);
    if (balloon) {
        nd.uFlags |= NIF_INFO;
        wcsncpy(nd.szInfoTitle, title ? title : L"PhoneCam работает в фоне", 63);
        wcsncpy(nd.szInfo, balloon, 255);
        nd.dwInfoFlags = NIIF_INFO;
    }
    if (add || !g.tray_added) {
        g.tray_added = Shell_NotifyIconW(NIM_ADD, &nd) != FALSE;
        if (!g.tray_added) g.tray_added = Shell_NotifyIconW(NIM_MODIFY, &nd) != FALSE;
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nd);
    }
    if (nd.hIcon) DestroyIcon(nd.hIcon);
}

void TrayRemove() {
    NOTIFYICONDATAW nd{};
    nd.cbSize = sizeof nd;
    nd.hWnd = g.wnd;
    nd.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nd);
    g.tray_added = false;
}

void ShowMainWindow() {
    ShowWindow(g.wnd, IsIconic(g.wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.wnd);
}

void HideToTray() {
    ShowWindow(g.wnd, SW_HIDE);
    if (!g.tray_hint_shown) {
        g.tray_hint_shown = true;
        IniSet(L"trayhint", L"1");
        TrayUpdate(false, L"Программа свёрнута в трей и продолжает работать. Открыть - двойной клик по значку, "
                          L"выйти - правой кнопкой -> «Выйти».");
    }
}

void TrayMenu() {
    HMENU m = CreatePopupMenu();
    std::wstring bat = g.battery >= 0 ? L"Батарея телефона: " + BatteryText()
                                      : std::wstring(L"Батарея телефона: нет подключения");
    AppendMenuW(m, MF_STRING | (g.battery >= 0 ? 0 : MF_GRAYED), ID_TRAY_BATTERY, bat.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Открыть");
    AppendMenuW(m, MF_STRING, ID_TRAY_SETTINGS, L"Настройки");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Выйти");
    SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g.wnd);  // so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g.wnd, nullptr);
    PostMessageW(g.wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void ExitApp() {
    SaveSettings();
    g.client.Stop();
    StopConverter();
    VcamClose();
    TrayRemove();
    DestroyWindow(g.wnd);
}

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (g.taskbar_created && m == g.taskbar_created) {  // Explorer restarted
        g.tray_added = false;
        TrayUpdate(true);
        return 0;
    }
    switch (m) {
    case WM_CREATE:
        g.wnd = w;
        MakeBrushes();
        BuildUi();
        ApplyTitleBarTheme();
        ApplyControlThemes();
        LoadSettings();
        g.client.SetAdjust(g.adj);
        RefreshFpsList();
        StartCapsQuery();
        UpdateVcamUi();
        SetControlsEnabled(true);
        UpdateOrientButton();
        Layout();
        SetTimer(w, 7, 3000, nullptr);  // keep looking for the phone while it isn't found
        g.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        g.tray_hint_shown = IniInt(L"trayhint", 0) != 0;
        TrayUpdate(true);
        if (Checked(IDC_TG)) ApplyMode();
        else if (!Checked(IDC_USB) && GetWindowTextLengthW(g.h[IDC_IP - 100]) == 0)
            SetWindowTextW(g.h[IDC_STATUS - 100], L"Введи IP телефона или нажми «Найти».");
        return 0;
    case WM_SIZE:
        Layout();
        InvalidateRect(w, nullptr, TRUE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize = {S(880), S(804)};
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT c;
        GetClientRect(w, &c);
        RECT panel{0, 0, S(kPanelW), c.bottom};
        FillRect(dc, &panel, g.br_panel);
        RECT rest{S(kPanelW), 0, c.right, c.bottom};
        HRGN rg = CreateRectRgnIndirect(&rest), pr = CreateRectRgnIndirect(&g.preview_rc);
        CombineRgn(rg, rg, pr, RGN_DIFF);
        FillRgn(dc, rg, g.br_bg);
        DeleteObject(rg);
        DeleteObject(pr);
        HPEN p = Pen(T().border);
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, S(kPanelW) - 1, 0, nullptr);
        LineTo(dc, S(kPanelW) - 1, c.bottom);
        SelectObject(dc, op);
        DeleteObject(p);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        PaintHeader(dc);
        PaintBattery(dc);
        PaintIpField(dc);
        PaintPreview(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MEASUREITEM: {
        auto* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_COMBOBOX) mi->itemHeight = S(22);
        return TRUE;
    }
    case WM_DRAWITEM: {
        auto* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_COMBOBOX) DrawComboItem(di);
        else if (di->CtlType == ODT_STATIC) DrawWarnIcon(di);
        else DrawButtonItem(di);
        return TRUE;
    }
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == g.h[IDC_ZOOM - 100])
            return TrackbarCustomDraw((NMCUSTOMDRAW*)lp);
        return 0;
    }
    case WM_APP_FRAME:
        g.frame_pending = false;
        InvalidateRect(w, &g.preview_rc, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == 7 && !g.phone_found && !g.connected && GetFocus() != g.h[IDC_IP - 100]) StartCapsQuery();
        return 0;
    case WM_APP_VCAMDONE:  // finished while the settings window was closed
        OnVcamDone(wp == 1, (int)lp);
        return 0;
    case WM_APP_CTRL:
        // torch / zoom changed on the phone's screen
        if (wp == 1) SetCheck(IDC_TORCH, lp >= 5);
        else if (wp == 3) SetBattery((int)(lp / 10), g.charging);
        else if (wp == 4) g.charging = lp >= 5;
        else if (wp == 2) SendMessageW(g.h[IDC_ZOOM - 100], TBM_SETPOS, TRUE, std::max(10, std::min(50, (int)lp)));
        return 0;
    case WM_APP_ORIENT:
        UpdateOrientButton();
        return 0;
    case WM_APP_STATUS: {
        auto* p = (std::pair<State, std::wstring>*)lp;
        if (g.connected) {
            bool changed = g.state != p->first;
            g.state = p->first;
            g.status = p->second;
            if (changed && p->first != State::Streaming) SetBattery(-1, false);
            if (changed) TrayUpdate(false);
            SetWindowTextW(g.h[IDC_STATUS - 100], g.status.c_str());
            if (p->first != State::Streaming) {
                std::lock_guard<std::mutex> lk(g.pmu);
                g.pw = g.ph = 0;
            }
            InvalidateRect(w, &g.preview_rc, FALSE);
        }
        delete p;
        return 0;
    }
    case WM_APP_FOUND: {
        auto* v = (std::vector<FoundPhone>*)lp;
        EnableWindow(g.h[IDC_FIND - 100], !g.connected);
        SetWindowTextW(g.h[IDC_FIND - 100], L"Найти");
        if (v->empty()) {
            MessageBoxW(w, L"Телефон не найден в сети.\n\nПроверь, что приложение PhoneCam открыто на телефоне "
                           L"и что телефон с ПК в одной Wi-Fi сети. Можно вписать IP вручную.",
                        L"PhoneCam", MB_ICONINFORMATION);
        } else {
            SetWindowTextW(g.h[IDC_IP - 100], W((*v)[0].ip).c_str());
            std::wstring s = L"Найден: " + W((*v)[0].name) + L" (" + W((*v)[0].ip) + L")";
            if (v->size() > 1) s += L"\nи ещё " + std::to_wstring(v->size() - 1);
            SetWindowTextW(g.h[IDC_STATUS - 100], s.c_str());
            StartCapsQuery();
        }
        delete v;
        return 0;
    }
    case WM_APP_CAPS: {
        auto* res = (std::pair<std::string, std::vector<CamCaps>>*)lp;
        g.caps_busy = false;
        if (Checked(IDC_TG)) {
            // answer to an old query from Wi-Fi/USB mode: not relevant now
        } else if (!res->second.empty()) {
            g.caps = res->second;
            g.caps_host = res->first;
            bool was = g.phone_found;
            g.phone_found = true;
            RefreshCameraList();
            UpdateCameraSettingsEnabled();
            if (!g.connected) SetWindowTextW(g.h[IDC_STATUS - 100], CapsSummary().c_str());
            (void)was;
        } else if (!g.connected) {
            g.phone_found = false;
            UpdateCameraSettingsEnabled();
            std::wstring where = res->first == "usb" ? L"по USB" : L"по адресу " + W(res->first);
            std::wstring msg = res->first.empty() || res->first == "?"
                ? L"Введи IP телефона или нажми «Найти»."
                : L"Телефон не найден " + where + L". Открой PhoneCam на телефоне или нажми «Найти». Настройки камеры станут доступны, когда телефон ответит.";
            SetWindowTextW(g.h[IDC_STATUS - 100], msg.c_str());
        }
        delete res;
        return 0;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g.h[IDC_ZOOM - 100]) {
            int pos = (int)SendMessageW(g.h[IDC_ZOOM - 100], TBM_GETPOS, 0, 0);
            char b[32];
            snprintf(b, sizeof b, "ZOOM %.1f", pos / 10.0);
            g.client.SendCommand(b);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_TRAY_OPEN || id == ID_TRAY_BATTERY) { ShowMainWindow(); return 0; }
        if (id == ID_TRAY_SETTINGS) { OpenSettings(); return 0; }
        if (id == ID_TRAY_EXIT) { ExitApp(); return 0; }
        // our owner-drawn check boxes and radio buttons keep their own state
        if (id >= 100 && id < 164 && HIWORD(wp) == BN_CLICKED) {
            if (g.kind[id - 100] == 3) SetCheck(id, !Checked(id));
            if (g.kind[id - 100] == 4) {
                SetCheck(IDC_WIFI, id == IDC_WIFI);
                SetCheck(IDC_USB, id == IDC_USB);
                SetCheck(IDC_TG, id == IDC_TG);
            }
        }
        switch (id) {
        case IDC_CONNECT: Connect(); break;
        case IDC_THEME: SetTheme(!g.dark); break;
        case IDS_AUTOBRI:
            g.auto_bri = Checked(IDS_AUTOBRI);
            g.client.SetAutoBrightness(g.auto_bri);
            SaveImageSettings();
            break;
        case IDC_GEAR: OpenSettings(); break;
        case IDC_ORIENT:
            g.client.SendCommand(g.portrait ? "ORIENT l" : "ORIENT p");
            break;
        case IDC_FIND:
            if (Checked(IDC_TG)) {
                if (MessageBoxW(w, L"Сделать новый код? Телефонам, которые подключались со старым кодом, "
                                   L"нужно будет отсканировать новый QR-код.", L"PhoneCam",
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    g.tg_code = NewPairCode();
                    SaveSettings();
                    SetWindowTextW(g.h[IDC_IP - 100], PrettyCode().c_str());
                    InvalidateRect(w, &g.preview_rc, FALSE);
                }
                break;
            }
            EnableWindow(g.h[IDC_FIND - 100], FALSE);
            SetWindowTextW(g.h[IDC_FIND - 100], L"Ищу…");
            CloseHandle(CreateThread(nullptr, 0, FindThread, nullptr, 0, nullptr));
            break;
        case IDC_MIRROR: g.client.SetMirror(Checked(IDC_MIRROR)); SaveSettings(); break;
        case IDC_ROT: g.client.SetRotate180(Checked(IDC_ROT)); SaveSettings(); break;
        case IDC_TORCH: g.client.SendCommand(Checked(IDC_TORCH) ? "TORCH 1" : "TORCH 0"); break;
        case IDC_AF: g.client.SendCommand("AF"); break;
        case IDC_VCAMHINT: OpenSettings(true); break;
        case IDC_WIFI: case IDC_USB: case IDC_TG:
            if (!g.connected) { g.phone_found = false; UpdateCameraSettingsEnabled(); StartCapsQuery(); }
            ApplyMode();
            if (!g.connected && HIWORD(wp) == BN_CLICKED) SaveSettings();
            InvalidateRect(w, &g.ip_field, TRUE);
            if (HIWORD(wp) == BN_CLICKED && Checked(IDC_USB)) StartCapsQuery();
            break;
        case IDC_FPS:
            if (HIWORD(wp) == CBN_SELCHANGE) { g.wanted_fps = SelectedFps(); SaveSettings(); }
            break;
        case IDC_CAMERA: case IDC_RES:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                if (id == IDC_CAMERA) { g.wanted_side = SelectedSide(); UpdateTorchLabel(); }
                RefreshFpsList();
                if (!g.connected && !g.caps.empty()) SetWindowTextW(g.h[IDC_STATUS - 100], CapsSummary().c_str());
            }
            break;
        case IDC_HS:
            if (Checked(IDC_HS)) g.wanted_fps = 60;
            RefreshFpsList();
            SaveSettings();
            break;
        case IDC_IP:
            if (Checked(IDC_TG)) break;
            if (HIWORD(wp) == EN_CHANGE && g.phone_found && !g.connected) {
                // another address: lock the settings until this phone answers
                g.phone_found = false;
                UpdateCameraSettingsEnabled();
            }
            if (HIWORD(wp) == EN_KILLFOCUS) StartCapsQuery();
            if (HIWORD(wp) == EN_KILLFOCUS || HIWORD(wp) == EN_SETFOCUS) InvalidateRect(w, &g.ip_field, FALSE);
            break;
        }
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, T().text);
        SetBkColor(dc, T().input);
        return (LRESULT)g.br_input;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, T().text);
        SetBkColor(dc, T().input);
        return (LRESULT)g.br_input;
    }
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        HDC dc = (HDC)wp;
        const Theme& t = T();
        if (c == g.h[IDC_IP - 100]) {  // disabled edit
            SetTextColor(dc, t.sub);
            SetBkColor(dc, t.input);
            return (LRESULT)g.br_input;
        }
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, t.panel);
        int kind = (int)(INT_PTR)GetPropW(c, L"pc_kind") - 1;
        COLORREF col = t.text;
        if (kind == 1) col = t.sub;
        if (c == g.h[IDC_STATUS - 100])
            col = g.state == State::Streaming ? t.ok : g.state == State::Error ? t.err : t.idle;
        SetTextColor(dc, col);
        return (LRESULT)g.br_panel;
    }
    case WM_CLOSE:
        // the cross only hides the window; streaming and the virtual camera keep working
        SaveSettings();
        HideToTray();
        return 0;
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONDBLCLK) ShowMainWindow();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) TrayMenu();
        return 0;
    case WM_DESTROY:
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

static int CALLBACK FontEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *(bool*)lp = true;
    return 0;
}

static bool FontExists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FontEnumProc, (LPARAM)&found, 0);
    ReleaseDC(nullptr, dc);
    return found;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // one copy only: a second launch just brings the running one (maybe sitting in the tray) to the front
    HANDLE single = CreateMutexW(nullptr, TRUE, L"PhoneCam.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"PhoneCamWnd", nullptr)) {
            ShowWindow(other, IsIconic(other) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(other);
        }
        return 0;
    }
    g.inst = inst;
    SetProcessDPIAware();
    HDC sdc = GetDC(nullptr);
    g.dpi = GetDeviceCaps(sdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, sdc);

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\PhoneCam";
        CreateDirectoryW(dir.c_str(), nullptr);
        g.ini = dir + L"\\phonecam.ini";
    } else {
        g.ini = ExeDir() + L"phonecam.ini";
    }

    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(13);
    wcscpy(lf.lfFaceName, L"Segoe UI");
    g.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(24);
    lf.lfWeight = FW_BOLD;
    g.big = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(26);
    lf.lfWeight = FW_BOLD;
    wcscpy(lf.lfFaceName, FontExists(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI");
    g.title = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(16);
    lf.lfWeight = FW_NORMAL;
    wcscpy(lf.lfFaceName, L"Segoe UI Symbol");
    g.symbol = CreateFontIndirectW(&lf);
    g.logo = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, S(38), S(38), 0);
    {
        wchar_t th[16];
        GetPrivateProfileStringW(L"phonecam", L"theme", L"", th, 16, g.ini.c_str());
        g.dark = wcscmp(th, L"dark") == 0 ? true : wcscmp(th, L"light") == 0 ? false : SystemPrefersDark();
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"PhoneCamWnd";
    RegisterClassExW(&wc);
    WNDCLASSEXW sl{sizeof sl};
    sl.lpfnWndProc = SliderProc;
    sl.hInstance = inst;
    sl.hCursor = LoadCursor(nullptr, IDC_HAND);
    sl.lpszClassName = L"PhoneCamSlider";
    RegisterClassExW(&sl);
    WNDCLASSEXW pg = sl;
    pg.lpfnWndProc = ProgressProc;
    pg.hCursor = LoadCursor(nullptr, IDC_WAIT);
    pg.lpszClassName = L"PhoneCamProgress";
    RegisterClassExW(&pg);
    WNDCLASSEXW sc = wc;
    sc.lpfnWndProc = SettingsProc;
    sc.lpszClassName = L"PhoneCamSettings";
    RegisterClassExW(&sc);

    RECT r{0, 0, S(1060), S(794)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"PhoneCam - телефон как вебкамера", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             inst, nullptr);
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(w, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
