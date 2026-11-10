// Low-light enhancement used by "Умная яркость PhoneCam":
// temporal + spatial noise reduction, local (regional) tone mapping and colour that follows the brightness.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "phonecam_client.h"

namespace phonecam {

// Tiny fixed-size thread pool: ParallelFor splits [0, n) into chunks for the workers and waits.
class WorkPool {
public:
    WorkPool();
    ~WorkPool();
    void ParallelFor(int n, const std::function<void(int begin, int end)>& fn);
private:
    void Worker(int idx);
    std::vector<std::thread> threads_;
    std::mutex mu_;
    std::condition_variable cv_, done_cv_;
    const std::function<void(int, int)>* job_ = nullptr;
    int n_ = 0, chunks_ = 0, next_ = 0, finished_ = 0;
    uint64_t gen_ = 0;
    bool quit_ = false;
};

class NightFilter {
public:
    static constexpr int kLevels = 17;  // local brightness factors from 0.6x to 1.7x
    // tone[L][i]: output luma (16..235) for 10-bit input i at local level L; csat[L][i]: chroma gain x256
    uint8_t tone[kLevels][1024];
    uint16_t csat[kLevels][1024];
    static double LevelFactor(int L);

    // in -> out (NV12, out stride = width). gain = smart brightness gain (for noise-reduction strength).
    void Process(const Frame& in, uint8_t* outY, uint8_t* outUV, double gain, double median);
    void Reset() { valid_ = false; }

private:
    WorkPool pool_;
    std::vector<uint16_t> y16_, uv16_;  // temporally filtered frame, x16 fixed point
    std::vector<float> grid_, tmp_;
    int w_ = 0, h_ = 0, gw_ = 0, gh_ = 0;
    bool valid_ = false;
};

}  // namespace phonecam
