// Linux test: receive the phone page's WebRTC stream and write it to out.h264
#include "../../pc/core/web_link.h"
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
namespace phonecam { void Log(const std::string& l) { printf("[log] %s\n", l.c_str()); fflush(stdout); } }
const char* TR(const char* s) { return s; }
using namespace phonecam;
int main(int argc, char** argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    FILE* f = fopen("out.h264", "wb");
    WebLink link;
    WebLinkConfig c; c.code = "ABCD2345"; c.brokers = {"127.0.0.1:18830"}; c.width = 1280; c.height = 720; c.kbps = 6000;
    WebLink::Callbacks cb;
    int frames = 0, keys = 0;
    cb.on_event = [](WebLink::Event e, const std::string& s) { printf("event %d %s\n", (int)e, s.c_str()); fflush(stdout); };
    cb.on_frame = [&](WebFrame&& fr) { frames++; keys += fr.key; fwrite(fr.annexb.data(), 1, fr.annexb.size(), f); };
    cb.on_line = [&](const std::string& l) { printf("line: %s\n", l.c_str()); fflush(stdout); };
    std::string err;
    if (!link.Start(c, cb, err)) { printf("start failed %s\n", err.c_str()); return 1; }
    for (int i = 0; i < secs; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto s = link.TakeStats();
        printf("t=%d frames=%d keys=%d pk=%llu kbit=%llu lost=%llu nack=%llu pli=%llu drop=%llu\n", i, frames, keys,
               (unsigned long long)s.packets, (unsigned long long)s.bytes * 8 / 1000, (unsigned long long)s.lost,
               (unsigned long long)s.nacks, (unsigned long long)s.plis, (unsigned long long)s.dropped_frames);
        fflush(stdout);
        if (i == 3) link.SetConfig(1920, 1080, 30, 6000, "back");
        
    }
    link.Stop();
    fclose(f);
    printf("done frames=%d\n", frames);
}
