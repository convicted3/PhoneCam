#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace phonecam {

struct Frame;

// Thin wrapper around the Microsoft H.264 decoder MFT (ships with Windows 10/11).
class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();
    bool Init(std::string& err);
    // Feed one Annex-B access unit; on_frame is called for every decoded picture.
    bool Decode(const uint8_t* data, size_t size, int64_t pts_us,
                const std::function<void(const Frame&)>& on_frame, std::string& err);
    void Reset();

private:
    bool NegotiateOutput();
    bool Drain(const std::function<void(const Frame&)>& on_frame, std::string& err);
    struct Impl;
    Impl* d_;
};

}  // namespace phonecam
