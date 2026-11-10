#include "night_filter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phonecam {

// ---------- WorkPool ----------
WorkPool::WorkPool() {
    unsigned n = std::max(1u, std::min(6u, std::thread::hardware_concurrency()));
    for (unsigned i = 1; i < n; i++) threads_.emplace_back([this, i] { Worker((int)i); });
}

WorkPool::~WorkPool() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
}

void WorkPool::Worker(int) {
    uint64_t seen = 0;
    for (;;) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return quit_ || gen_ != seen; });
        if (quit_) return;
        seen = gen_;
        for (;;) {
            if (next_ >= chunks_) break;
            int c = next_++;
            const auto* job = job_;
            int n = n_, chunks = chunks_;
            lk.unlock();
            int b = (int)((int64_t)n * c / chunks), e = (int)((int64_t)n * (c + 1) / chunks);
            (*job)(b, e);
            lk.lock();
            if (++finished_ == chunks_) done_cv_.notify_all();
        }
    }
}

void WorkPool::ParallelFor(int n, const std::function<void(int, int)>& fn) {
    if (n <= 0) return;
    if (threads_.empty() || n < 32) { fn(0, n); return; }
    std::unique_lock<std::mutex> lk(mu_);
    job_ = &fn;
    n_ = n;
    chunks_ = (int)(threads_.size() + 1) * 2;
    next_ = 0;
    finished_ = 0;
    gen_++;
    cv_.notify_all();
    // the calling thread helps too
    for (;;) {
        if (next_ >= chunks_) break;
        int c = next_++;
        lk.unlock();
        int b = (int)((int64_t)n * c / chunks_), e = (int)((int64_t)n * (c + 1) / chunks_);
        fn(b, e);
        lk.lock();
        ++finished_;
    }
    done_cv_.wait(lk, [&] { return finished_ == chunks_; });
    job_ = nullptr;
}

// ---------- NightFilter ----------
double NightFilter::LevelFactor(int L) {
    const double lo = std::log2(0.6), hi = std::log2(1.7);
    return std::pow(2.0, lo + (hi - lo) * L / (kLevels - 1));
}

void NightFilter::Process(const Frame& in, uint8_t* outY, uint8_t* outUV, double gain, double median) {
    const int w = in.width, h = in.height;
    if (w != w_ || h != h_) {
        w_ = w;
        h_ = h;
        y16_.assign((size_t)w * h, 0);
        uv16_.assign((size_t)w * h / 2, 0);
        gw_ = (w + 15) / 16;
        gh_ = (h + 15) / 16;
        grid_.assign((size_t)gw_ * gh_, 0);
        tmp_.assign((size_t)gw_ * gh_, 0);
        valid_ = false;
    }
    // --- 1. temporal noise reduction (motion adaptive): still areas average over frames, moving ones don't ---
    const double s = std::max(0.0, std::min(1.0, (gain - 1.0) / 3.0));
    int alut[64];
    for (int d = 0; d < 64; d++) {
        double a = 1.0 - 0.72 * s;                      // ~0.28 for static noise at full strength
        double t = std::max(0.0, std::min(1.0, (d - 2.0) / 9.0));
        alut[d] = (int)std::lround(256 * (a + (1.0 - a) * t));  // differences of 11+ levels = motion
    }
    const bool first = !valid_;
    pool_.ParallelFor(h, [&](int b, int e) {
        for (int y = b; y < e; y++) {
            const uint8_t* src = in.y + (size_t)y * in.y_stride;
            uint16_t* st = &y16_[(size_t)y * w];
            if (first) { for (int x = 0; x < w; x++) st[x] = (uint16_t)(src[x] << 4); continue; }
            for (int x = 0; x < w; x++) {
                int cur = src[x] << 4, prev = st[x];
                int d = std::abs(cur - prev) >> 4;
                st[x] = (uint16_t)(prev + (((cur - prev) * alut[d < 63 ? d : 63]) >> 8));
            }
        }
    });
    pool_.ParallelFor(h / 2, [&](int b, int e) {
        for (int y = b; y < e; y++) {
            const uint8_t* src = in.uv + (size_t)y * in.uv_stride;
            uint16_t* st = &uv16_[(size_t)y * w];
            if (first) { for (int x = 0; x < w; x++) st[x] = (uint16_t)(src[x] << 4); continue; }
            for (int x = 0; x < w; x++) {
                int cur = src[x] << 4, prev = st[x];
                int d = std::abs(cur - prev) >> 4;
                st[x] = (uint16_t)(prev + (((cur - prev) * alut[d < 63 ? d : 63]) >> 8));
            }
        }
    });
    valid_ = true;

    // --- 2. local brightness map: 16x16 cells, blurred; darker regions get a larger factor ---
    for (int gy = 0; gy < gh_; gy++)
        for (int gx = 0; gx < gw_; gx++) {
            uint32_t sum = 0, cnt = 0;
            for (int y = gy * 16 + 2; y < std::min(h, gy * 16 + 16); y += 4)
                for (int x = gx * 16 + 2; x < std::min(w, gx * 16 + 16); x += 4) { sum += y16_[(size_t)y * w + x]; cnt++; }
            grid_[(size_t)gy * gw_ + gx] = cnt ? std::max(0.0f, (sum / (float)cnt / 16.0f - 16.0f) / 219.0f) : 0.0f;
        }
    for (int pass = 0; pass < 2; pass++) {
        for (int gy = 0; gy < gh_; gy++)
            for (int gx = 0; gx < gw_; gx++) {
                float acc = 0;
                int c = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int yy = std::max(0, std::min(gh_ - 1, gy + dy)), xx = std::max(0, std::min(gw_ - 1, gx + dx));
                        acc += grid_[(size_t)yy * gw_ + xx];
                        c++;
                    }
                tmp_[(size_t)gy * gw_ + gx] = acc / c;
            }
        grid_.swap(tmp_);
    }
    // convert to level index (float) for the tone tables
    const double ref = std::max(median, 0.01), lo = std::log2(0.6), hi = std::log2(1.7);
    for (auto& g : grid_) {
        double f = std::pow(ref / std::max<double>(g, 0.003), 0.45);
        f = std::max(0.6, std::min(1.7, f));
        g = (float)((std::log2(f) - lo) / (hi - lo) * (kLevels - 1));
    }

    // weight of the dark-area smoothing for each 10-bit input value
    const double amt = std::max(0.0, std::min(1.0, (gain - 1.5) / 3.5)) * 0.75;
    int wlut[1024];
    for (int i = 0; i < 1024; i++) {
        double n = std::max(0.0, (i / 4.0 - 16.0) / 219.0);
        wlut[i] = (int)std::lround(256 * amt * std::max(0.0, 1.0 - n / 0.18));
    }

    auto level_row = [&](int y, std::vector<float>& row) {
        float fy = (y + 0.5f) / 16.0f - 0.5f;
        int y0 = std::max(0, std::min(gh_ - 1, (int)std::floor(fy))), y1 = std::min(gh_ - 1, y0 + 1);
        float wy = std::max(0.0f, std::min(1.0f, fy - y0));
        for (int gx = 0; gx < gw_; gx++)
            row[gx] = grid_[(size_t)y0 * gw_ + gx] * (1 - wy) + grid_[(size_t)y1 * gw_ + gx] * wy;
    };
    auto level_at = [&](const std::vector<float>& row, int x) -> int {  // 8.8 fixed point level
        float fx = (x + 0.5f) / 16.0f - 0.5f;
        int x0 = std::max(0, std::min(gw_ - 1, (int)std::floor(fx))), x1 = std::min(gw_ - 1, x0 + 1);
        float wx = std::max(0.0f, std::min(1.0f, fx - x0));
        float l = row[x0] * (1 - wx) + row[x1] * wx;
        int li = (int)(l * 256);
        return std::max(0, std::min((kLevels - 1) * 256 - 1, li));
    };

    // --- 3. luma: dark-area smoothing + local tone mapping with 10-bit precision ---
    pool_.ParallelFor(h, [&](int b, int e) {
        std::vector<float> row(gw_);
        for (int y = b; y < e; y++) {
            level_row(y, row);
            const uint16_t* r0 = &y16_[(size_t)std::max(0, y - 1) * w];
            const uint16_t* r1 = &y16_[(size_t)y * w];
            const uint16_t* r2 = &y16_[(size_t)std::min(h - 1, y + 1) * w];
            uint8_t* o = outY + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                int xl = x > 0 ? x - 1 : 0, xr = x < w - 1 ? x + 1 : x;
                int v = r1[x];
                int idx = v >> 2;  // x16 -> x4 = 10-bit
                int wgt = wlut[idx > 1023 ? 1023 : idx];
                if (wgt) {
                    int avg = (r0[xl] + r0[x] + r0[xr] + r1[xl] + r1[x] + r1[xr] + r2[xl] + r2[x] + r2[xr]) / 9;
                    v = (v * (256 - wgt) + avg * wgt) >> 8;
                    idx = v >> 2;
                }
                if (idx > 1023) idx = 1023;
                int lv = level_at(row, x), L = lv >> 8, fr = lv & 255;
                o[x] = (uint8_t)((tone[L][idx] * (256 - fr) + tone[L + 1][idx] * fr + 128) >> 8);
            }
        }
    });

    // --- 4. chroma: spatial 3x3 smoothing of the filtered colour + gain that follows the brightness ---
    pool_.ParallelFor(h / 2, [&](int b, int e) {
        std::vector<float> row(gw_);
        const int ch = h / 2;
        for (int y = b; y < e; y++) {
            level_row(2 * y, row);
            const uint16_t* r0 = &uv16_[(size_t)std::max(0, y - 1) * w];
            const uint16_t* r1 = &uv16_[(size_t)y * w];
            const uint16_t* r2 = &uv16_[(size_t)std::min(ch - 1, y + 1) * w];
            const uint16_t* ly = &y16_[(size_t)(2 * y) * w];
            uint8_t* o = outUV + (size_t)y * w;
            for (int x = 0; x < w; x += 2) {
                int xl = x >= 2 ? x - 2 : x, xr = x + 2 < w ? x + 2 : x;
                int su = r0[xl] + r0[x] + r0[xr] + r1[xl] + r1[x] + r1[xr] + r2[xl] + r2[x] + r2[xr];
                int sv = r0[xl + 1] + r0[x + 1] + r0[xr + 1] + r1[xl + 1] + r1[x + 1] + r1[xr + 1] + r2[xl + 1] +
                         r2[x + 1] + r2[xr + 1];
                int idx = ly[x] >> 2;
                if (idx > 1023) idx = 1023;
                int lv = level_at(row, x), L = lv >> 8, fr = lv & 255;
                int k = (csat[L][idx] * (256 - fr) + csat[L + 1][idx] * fr) >> 8;
                // average of 9 samples in x16 units -> deviation from neutral (x16)
                int du = su / 9 - (128 << 4), dv = sv / 9 - (128 << 4);
                int u = 128 + ((du * k) >> 12), v = 128 + ((dv * k) >> 12);
                o[x] = (uint8_t)(u < 0 ? 0 : u > 255 ? 255 : u);
                o[x + 1] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    });
}

}  // namespace phonecam
