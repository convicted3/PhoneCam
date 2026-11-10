// PhoneCam OBS source: connects straight to the phone app and outputs its camera as an async video source.
#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../core/phonecam_client.h"
#include "../core/web_link.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("PhoneCam")

MODULE_EXPORT const char* obs_module_description(void) {
    return "PhoneCam: use an Android phone as a camera source";
}

namespace {

struct ResOpt { int w, h; const char* name; };
const ResOpt kRes[] = {{640, 480, "640 x 480"}, {1280, 720, "1280 x 720 (HD)"}, {1920, 1080, "1920 x 1080 (Full HD)"},
                       {3840, 2160, "3840 x 2160 (4K, если телефон умеет)"}};

// Camera capabilities reported by the phone; shared so the background query can outlive the source.
struct CapsState {
    std::mutex mu;
    std::vector<phonecam::CamCaps> caps;
    std::atomic<bool> busy{false};
};

void QueryCapsAsync(std::shared_ptr<CapsState> st, const phonecam::Config& cfg) {
    if (!cfg.usb && cfg.host.empty()) return;
    if (st->busy.exchange(true)) return;
    std::string host = cfg.host;
    bool usb = cfg.usb;
    int port = cfg.port;
    std::thread([st, host, usb, port] {
        std::vector<phonecam::CamCaps> c;
        std::string err;
        if (phonecam::QueryCaps(host, port, usb, c, err)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->caps = c;
        }
        st->busy = false;
    }).detach();
}

struct PhoneSource {
    std::shared_ptr<CapsState> caps = std::make_shared<CapsState>();
    obs_source_t* source = nullptr;
    phonecam::Client client;
    std::mutex mu;
    phonecam::Config cfg;
    bool have_cfg = false;
    std::string status = "Не подключено";
    std::atomic<bool> streaming{false};
    bool torch = false;
    double zoom = 1.0;
    bool enabled = true;

    void Restart() {
        client.Stop();
        obs_source_output_video(source, nullptr);
        if (!enabled) { status = "Выключено"; return; }
        if (!cfg.web && !cfg.usb && cfg.host.empty()) { status = "Введи IP телефона"; return; }
        if (!cfg.web) QueryCapsAsync(caps, cfg);
        client.Start(cfg,
            [this](const phonecam::Frame& f) { Output(f); },
            [this](phonecam::State s, const std::string& msg) {
                {
                    std::lock_guard<std::mutex> lk(mu);
                    status = msg;
                }
                bool was = streaming.exchange(s == phonecam::State::Streaming);
                if (s == phonecam::State::Streaming && !was) {
                    // re-apply live controls after (re)connect
                    if (torch) client.SendCommand("TORCH 1");
                    if (zoom > 1.01) {
                        char b[32];
                        snprintf(b, sizeof b, "ZOOM %.2f", zoom);
                        client.SendCommand(b);
                    }
                }
                if (s != phonecam::State::Streaming) blog(LOG_INFO, "[PhoneCam] %s", msg.c_str());
            });
    }

    std::atomic<bool> portrait{false};

    void Output(const phonecam::Frame& f) {
        portrait = f.height > f.width;
        obs_source_frame frame{};
        frame.width = (uint32_t)f.width;
        frame.height = (uint32_t)f.height;
        frame.format = VIDEO_FORMAT_NV12;
        frame.data[0] = const_cast<uint8_t*>(f.y);
        frame.data[1] = const_cast<uint8_t*>(f.uv);
        frame.linesize[0] = (uint32_t)f.y_stride;
        frame.linesize[1] = (uint32_t)f.uv_stride;
        frame.timestamp = os_gettime_ns();
        video_format_get_parameters(f.height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601, VIDEO_RANGE_PARTIAL,
                                    frame.color_matrix, frame.color_range_min, frame.color_range_max);
        frame.full_range = false;
        obs_source_output_video(source, &frame);
    }
};

const char* GetName(void*) { return "PhoneCam (камера телефона)"; }

void Defaults(obs_data_t* s) {
    obs_data_set_default_bool(s, "enabled", true);
    obs_data_set_default_bool(s, "usb", false);
    obs_data_set_default_bool(s, "tg", false);
    obs_data_set_default_string(s, "ip", "");
    obs_data_set_default_int(s, "res", 1);
    obs_data_set_default_int(s, "fps", 30);
    obs_data_set_default_int(s, "kbps", 8000);
    obs_data_set_default_string(s, "camera", "back");
    obs_data_set_default_bool(s, "mirror", false);
    obs_data_set_default_bool(s, "hs", false);
    obs_data_set_default_bool(s, "rot180", false);
    obs_data_set_default_bool(s, "torch", false);
    obs_data_set_default_double(s, "zoom", 1.0);
}

void Update(void* data, obs_data_t* s) {
    auto* p = (PhoneSource*)data;
    phonecam::Config c;
    c.usb = obs_data_get_bool(s, "usb");
    c.host = obs_data_get_string(s, "ip");
    c.web = obs_data_get_bool(s, "tg");
    c.web_code = phonecam::NormalizePairCode(obs_data_get_string(s, "tgcode"));
    if (c.web_code.empty()) {
        c.web_code = phonecam::NewPairCode();
        obs_data_set_string(s, "tgcode", c.web_code.c_str());
    }
    c.web_link = phonecam::PairLink(c.web_code);
    if (c.web) c.high_speed = false;
    c.web_qr_frames = true;
    while (!c.host.empty() && (c.host.back() == ' ' || c.host.back() == '\t')) c.host.pop_back();
    while (!c.host.empty() && c.host.front() == ' ') c.host.erase(0, 1);
    int ri = (int)obs_data_get_int(s, "res");
    if (ri < 0 || ri >= (int)(sizeof kRes / sizeof kRes[0])) ri = 1;
    c.width = kRes[ri].w;
    c.height = kRes[ri].h;
    c.fps = (int)obs_data_get_int(s, "fps");
    c.kbps = (int)obs_data_get_int(s, "kbps");
    c.camera = obs_data_get_string(s, "camera");
    c.mirror = obs_data_get_bool(s, "mirror");
    c.high_speed = obs_data_get_bool(s, "hs");
    c.rotate180 = obs_data_get_bool(s, "rot180");
    bool enabled = obs_data_get_bool(s, "enabled");

    if (c.web) c.high_speed = false;
    bool need_restart = !p->have_cfg || enabled != p->enabled || c.usb != p->cfg.usb || c.host != p->cfg.host ||
                        c.web != p->cfg.web || c.web_code != p->cfg.web_code ||
                        c.width != p->cfg.width || c.fps != p->cfg.fps || c.kbps != p->cfg.kbps ||
                        c.camera != p->cfg.camera || c.high_speed != p->cfg.high_speed;

    p->client.SetMirror(c.mirror);
    p->client.SetRotate180(c.rotate180);

    bool torch = obs_data_get_bool(s, "torch");
    double zoom = obs_data_get_double(s, "zoom");
    p->cfg = c;
    p->have_cfg = true;
    p->enabled = enabled;

    if (need_restart) {
        p->torch = torch;
        p->zoom = zoom;
        p->Restart();
        return;
    }
    if (torch != p->torch) {
        p->torch = torch;
        p->client.SendCommand(torch ? "TORCH 1" : "TORCH 0");
    }
    if (zoom != p->zoom) {
        p->zoom = zoom;
        char b[32];
        snprintf(b, sizeof b, "ZOOM %.2f", zoom);
        p->client.SendCommand(b);
    }
}

void* Create(obs_data_t* settings, obs_source_t* source) {
    auto* p = new PhoneSource;
    p->source = source;
    Update(p, settings);
    return p;
}

void Destroy(void* data) {
    auto* p = (PhoneSource*)data;
    p->client.Stop();
    delete p;
}

// Fill the fps list (and enable the high-speed box) from what the phone said it can do.
void FillFps(PhoneSource* p, obs_properties_t* props, obs_data_t* s) {
    obs_property_t* fps = obs_properties_get(props, "fps");
    obs_property_t* hs = obs_properties_get(props, "hs");
    if (!fps) return;
    int ri = (int)obs_data_get_int(s, "res");
    if (ri < 0 || ri >= (int)(sizeof kRes / sizeof kRes[0])) ri = 1;
    std::string side = obs_data_get_string(s, "camera");
    int normal_max = 60;
    bool hs_ok = true;
    if (p) {
        std::lock_guard<std::mutex> lk(p->caps->mu);
        if (const phonecam::CamCaps* c = phonecam::FindCaps(p->caps->caps, side, kRes[ri].w, kRes[ri].h)) {
            normal_max = c->max_fps;
            hs_ok = c->hs_fps >= 60 && c->max_fps < 60;
        }
    }
    if (hs) obs_property_set_enabled(hs, hs_ok);
    bool hs_on = hs_ok && obs_data_get_bool(s, "hs");
    obs_property_list_clear(fps);
    int count = 0;
    for (int f : {15, 24, 30, 60}) {
        if (f > normal_max && !(f == 60 && hs_on)) continue;
        std::string name = std::to_string(f);
        if (f == 60 && normal_max < 60) name += " (скоростная съёмка)";
        obs_property_list_add_int(fps, name.c_str(), f);
        count++;
    }
    if (!count) obs_property_list_add_int(fps, std::to_string(normal_max).c_str(), normal_max);
}

bool FpsDepsChanged(void* priv, obs_properties_t* props, obs_property_t*, obs_data_t* s) {
    FillFps((PhoneSource*)priv, props, s);
    return true;
}

bool FindClicked(obs_properties_t* props, obs_property_t*, void* data) {
    auto* p = (PhoneSource*)data;
    auto found = phonecam::Discover(1500);
    obs_property_t* info = obs_properties_get(props, "status");
    if (found.empty()) {
        obs_property_set_description(info, "Телефон не найден. Открой PhoneCam на телефоне, проверь Wi-Fi.");
        return true;
    }
    obs_data_t* s = obs_source_get_settings(p->source);
    obs_data_set_string(s, "ip", found[0].ip.c_str());
    obs_data_set_bool(s, "usb", false);
    obs_source_update(p->source, s);
    {
        std::vector<phonecam::CamCaps> c;
        std::string err;
        if (phonecam::QueryCaps(found[0].ip, phonecam::kTcpPort, false, c, err)) {
            std::lock_guard<std::mutex> lk(p->caps->mu);
            p->caps->caps = c;
        }
    }
    FillFps(p, props, s);
    obs_data_release(s);
    std::string msg = "Найден: " + found[0].name + " (" + found[0].ip + ")";
    obs_property_set_description(info, msg.c_str());
    return true;
}

bool ReconnectClicked(obs_properties_t* props, obs_property_t*, void* data) {
    auto* p = (PhoneSource*)data;
    p->Restart();
    obs_property_set_description(obs_properties_get(props, "status"), "Переподключение…");
    return true;
}

bool RefreshStatus(obs_properties_t* props, obs_property_t*, void* data) {
    auto* p = (PhoneSource*)data;
    std::string st;
    {
        std::lock_guard<std::mutex> lk(p->mu);
        st = "Статус: " + p->status;
    }
    obs_property_set_description(obs_properties_get(props, "status"), st.c_str());
    return true;
}

bool OrientClicked(obs_properties_t*, obs_property_t*, void* data) {
    auto* p = (PhoneSource*)data;
    p->client.SendCommand(p->portrait ? "ORIENT l" : "ORIENT p");
    return false;
}

bool UsbChanged(obs_properties_t* props, obs_property_t*, obs_data_t* s) {
    bool usb = obs_data_get_bool(s, "usb");
    bool tg = obs_data_get_bool(s, "tg");
    obs_property_set_visible(obs_properties_get(props, "tginfo"), tg);
    obs_property_set_enabled(obs_properties_get(props, "usb"), !tg);
    obs_property_set_enabled(obs_properties_get(props, "ip"), !usb && !tg);
    obs_property_set_enabled(obs_properties_get(props, "find"), !usb && !tg);
    obs_property_set_enabled(obs_properties_get(props, "hs"), !tg);
    return true;
}

obs_properties_t* Properties(void* data) {
    auto* p = (PhoneSource*)data;
    obs_properties_t* props = obs_properties_create();

    std::string st = "Статус: ";
    if (p) {
        std::lock_guard<std::mutex> lk(p->mu);
        st += p->status;
    }
    obs_properties_add_text(props, "status", st.c_str(), OBS_TEXT_INFO);
    obs_properties_add_button(props, "refresh", "Обновить статус", RefreshStatus);

    obs_properties_add_bool(props, "enabled", "Включено");
    obs_property_t* tg = obs_properties_add_bool(props, "tg", "Через Телеграм / браузер (iPhone и любой телефон)");
    obs_property_set_modified_callback(tg, UsbChanged);
    {
        std::string code = "";
        if (p) {
            std::lock_guard<std::mutex> lk(p->mu);
            code = p->cfg.web_code;
        }
        std::string info = "Код для Телеграма: " + (code.size() == 8 ? code.substr(0, 4) + "-" + code.substr(4) : code) +
                           "  (пока телефон не подключён, источник показывает QR-код - наведи на него камеру телефона - откроется бот @phonecamweb_bot)";
        obs_properties_add_text(props, "tginfo", info.c_str(), OBS_TEXT_INFO);
    }
    obs_property_t* usb = obs_properties_add_bool(props, "usb", "Подключение по USB (adb)");
    obs_property_set_modified_callback(usb, UsbChanged);
    obs_properties_add_text(props, "ip", "IP телефона", OBS_TEXT_DEFAULT);
    obs_properties_add_button(props, "find", "Найти телефон в сети", FindClicked);

    obs_property_t* cam = obs_properties_add_list(props, "camera", "Камера", OBS_COMBO_TYPE_LIST,
                                                  OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(cam, "Основная (задняя)", "back");
    obs_property_list_add_string(cam, "Широкоугольная (если есть)", "wide");
    obs_property_list_add_string(cam, "Фронтальная", "front");

    obs_property_t* res = obs_properties_add_list(props, "res", "Разрешение", OBS_COMBO_TYPE_LIST,
                                                  OBS_COMBO_FORMAT_INT);
    for (int i = 0; i < (int)(sizeof kRes / sizeof kRes[0]); i++) obs_property_list_add_int(res, kRes[i].name, i);

    obs_property_t* hsp = obs_properties_add_bool(props, "hs", "60 FPS через скоростную съёмку");
    obs_properties_add_list(props, "fps", "Кадров в секунду", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_set_modified_callback2(cam, FpsDepsChanged, p);
    obs_property_set_modified_callback2(res, FpsDepsChanged, p);
    obs_property_set_modified_callback2(hsp, FpsDepsChanged, p);
    if (p) {
        obs_data_t* cur = obs_source_get_settings(p->source);
        FillFps(p, props, cur);
        obs_data_release(cur);
    } else {
        obs_data_t* d = obs_data_create();
        FillFps(nullptr, props, d);
        obs_data_release(d);
    }

    obs_property_t* q = obs_properties_add_list(props, "kbps", "Качество (битрейт)", OBS_COMBO_TYPE_LIST,
                                                OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(q, "Низкое - 2 Мбит/с", 2000);
    obs_property_list_add_int(q, "Среднее - 4 Мбит/с", 4000);
    obs_property_list_add_int(q, "Высокое - 8 Мбит/с", 8000);
    obs_property_list_add_int(q, "Очень высокое - 16 Мбит/с", 16000);
    obs_property_list_add_int(q, "Максимум - 30 Мбит/с", 30000);

    obs_properties_add_button(props, "orient", "Переключить: горизонтально / вертикально", OrientClicked);
    obs_properties_add_bool(props, "mirror", "Отразить зеркально");
    obs_properties_add_bool(props, "rot180", "Повернуть на 180°");
    obs_properties_add_bool(props, "torch", "Фонарик (у фронтальной - подсветка экраном)");
    obs_properties_add_float_slider(props, "zoom", "Зум", 1.0, 8.0, 0.1);
    obs_properties_add_button(props, "reconnect", "Переподключить", ReconnectClicked);
    return props;
}

}  // namespace

bool obs_module_load(void) {
    obs_source_info info{};
    info.id = "phonecam_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = GetName;
    info.create = Create;
    info.destroy = Destroy;
    info.update = Update;
    info.get_defaults = Defaults;
    info.get_properties = Properties;
    info.icon_type = OBS_ICON_TYPE_CAMERA;
    obs_register_source(&info);
    blog(LOG_INFO, "[PhoneCam] plugin loaded");
    return true;
}
