#include "qr.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstdint>

namespace phonecam {
namespace {

// level M
const int kEccPerBlock[11] = {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
const int kBlocks[11] = {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

int RawDataModules(int ver) {
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}
int DataCodewords(int ver) { return RawDataModules(ver) / 8 - kEccPerBlock[ver] * kBlocks[ver]; }

uint8_t GfMul(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}
std::vector<uint8_t> RsDivisor(int degree) {
    std::vector<uint8_t> r(degree, 0);
    r[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            r[j] = GfMul(r[j], root);
            if (j + 1 < degree) r[j] ^= r[j + 1];
        }
        root = GfMul(root, 0x02);
    }
    return r;
}
std::vector<uint8_t> RsRemainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& div) {
    std::vector<uint8_t> r(div.size(), 0);
    for (uint8_t b : data) {
        uint8_t f = b ^ r[0];
        r.erase(r.begin());
        r.push_back(0);
        for (size_t i = 0; i < r.size(); i++) r[i] ^= GfMul(div[i], f);
    }
    return r;
}

struct Grid {
    int n;
    std::vector<bool> m, fn;  // modules, is-function
    explicit Grid(int size) : n(size), m(size * size, false), fn(size * size, false) {}
    void Set(int x, int y, bool dark) { m[y * n + x] = dark; fn[y * n + x] = true; }
    bool Get(int x, int y) const { return m[y * n + x]; }
};

void DrawFinder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= g.n || y >= g.n) continue;
            int d = std::max(std::abs(dx), std::abs(dy));
            g.Set(x, y, d != 2 && d != 4);
        }
}
void DrawAlign(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) g.Set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}
std::vector<int> AlignPositions(int ver, int size) {
    if (ver == 1) return {};
    int na = ver / 7 + 2;
    int step = (ver * 4 + na * 2 + 1) / (na * 2 - 2) * 2;
    std::vector<int> r(na);
    r[0] = 6;
    for (int i = na - 1, pos = size - 7; i >= 1; i--, pos -= step) r[i] = pos;
    return r;
}
void DrawFormat(Grid& g, int mask) {
    int data = 0 << 3 | mask;  // level M = 00
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
    for (int i = 0; i <= 5; i++) g.Set(8, i, bit(i));
    g.Set(8, 7, bit(6));
    g.Set(8, 8, bit(7));
    g.Set(7, 8, bit(8));
    for (int i = 9; i < 15; i++) g.Set(14 - i, 8, bit(i));
    for (int i = 0; i < 8; i++) g.Set(g.n - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; i++) g.Set(8, g.n - 15 + i, bit(i));
    g.Set(8, g.n - 8, true);  // dark module
}
void DrawVersion(Grid& g, int ver) {
    if (ver < 7) return;
    int rem = ver;
    for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    long bits = (long)ver << 12 | rem;
    for (int i = 0; i < 18; i++) {
        bool b = (bits >> i) & 1;
        int a = g.n - 11 + i % 3, c = i / 3;
        g.Set(a, c, b);
        g.Set(c, a, b);
    }
}
bool MaskBit(int mask, int x, int y) {
    switch (mask) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}
long Penalty(const Grid& g) {
    long p = 0;
    int n = g.n;
    // runs
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++) {
            int run = 1;
            for (int b = 1; b <= n; b++) {
                bool same = b < n && (pass ? g.Get(a, b) == g.Get(a, b - 1) : g.Get(b, a) == g.Get(b - 1, a));
                if (same) { run++; continue; }
                if (run >= 5) p += 3 + run - 5;
                run = 1;
            }
        }
    // 2x2 blocks
    for (int y = 0; y + 1 < n; y++)
        for (int x = 0; x + 1 < n; x++) {
            bool c = g.Get(x, y);
            if (c == g.Get(x + 1, y) && c == g.Get(x, y + 1) && c == g.Get(x + 1, y + 1)) p += 3;
        }
    // finder-like patterns 1011101 with 4 light on a side
    const bool pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < n; a++)
            for (int b = 0; b + 11 <= n; b++) {
                bool m1 = true, m2 = true;
                for (int k = 0; k < 11; k++) {
                    bool v = pass ? g.Get(a, b + k) : g.Get(b + k, a);
                    if (v != pat1[k]) m1 = false;
                    if (v != pat2[k]) m2 = false;
                }
                if (m1) p += 40;
                if (m2) p += 40;
            }
    // balance
    long dark = 0;
    for (bool v : g.m) dark += v;
    long total = (long)n * n;
    long k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    p += std::max(0L, k) * 10;
    return p;
}

}  // namespace

QrCode MakeQr(const std::string& text) {
    QrCode out;
    int ver = 1;
    for (; ver <= 10; ver++) {
        int bits = 4 + (ver < 10 ? 8 : 16) + 8 * (int)text.size();
        if (bits <= DataCodewords(ver) * 8) break;
    }
    if (ver > 10) return out;
    int cap = DataCodewords(ver);
    // ---- bit stream
    std::vector<bool> bb;
    auto put = [&](uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) bb.push_back((v >> i) & 1); };
    put(4, 4);
    put((uint32_t)text.size(), ver < 10 ? 8 : 16);
    for (unsigned char c : text) put(c, 8);
    int capbits = cap * 8;
    put(0, std::min(4, capbits - (int)bb.size()));
    put(0, (8 - (int)bb.size() % 8) % 8);
    for (uint8_t pad = 0xEC; (int)bb.size() < capbits; pad ^= 0xEC ^ 0x11) put(pad, 8);
    std::vector<uint8_t> data(cap, 0);
    for (size_t i = 0; i < bb.size(); i++) data[i >> 3] |= (uint8_t)bb[i] << (7 - (i & 7));
    // ---- blocks + error correction, interleaved
    int nblocks = kBlocks[ver], ecl = kEccPerBlock[ver];
    int raw = RawDataModules(ver) / 8;
    int nshort = nblocks - raw % nblocks, shortlen = raw / nblocks;
    auto div = RsDivisor(ecl);
    std::vector<std::vector<uint8_t>> blocks;
    for (int i = 0, k = 0; i < nblocks; i++) {
        int dl = shortlen - ecl + (i < nshort ? 0 : 1);
        std::vector<uint8_t> d(data.begin() + k, data.begin() + k + dl);
        k += dl;
        auto ec = RsRemainder(d, div);
        if (i < nshort) d.push_back(0);  // placeholder so all blocks have equal length
        d.insert(d.end(), ec.begin(), ec.end());
        blocks.push_back(d);
    }
    std::vector<uint8_t> all;
    for (size_t i = 0; i < blocks[0].size(); i++)
        for (int j = 0; j < nblocks; j++)
            if (i != (size_t)(shortlen - ecl) || j >= nshort) all.push_back(blocks[j][i]);
    // ---- function patterns
    int n = ver * 4 + 17;
    Grid g(n);
    for (int i = 0; i < n; i++) {
        g.Set(6, i, i % 2 == 0);
        g.Set(i, 6, i % 2 == 0);
    }
    DrawFinder(g, 3, 3);
    DrawFinder(g, n - 4, 3);
    DrawFinder(g, 3, n - 4);
    auto ap = AlignPositions(ver, n);
    for (size_t i = 0; i < ap.size(); i++)
        for (size_t j = 0; j < ap.size(); j++)
            if (!((i == 0 && j == 0) || (i == 0 && j == ap.size() - 1) || (i == ap.size() - 1 && j == 0)))
                DrawAlign(g, ap[i], ap[j]);
    DrawFormat(g, 0);  // reserve
    DrawVersion(g, ver);
    // ---- data placement
    size_t bi = 0;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < n; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool up = ((right + 1) & 2) == 0;
                int y = up ? n - 1 - vert : vert;
                if (!g.fn[y * n + x] && bi < all.size() * 8) {
                    g.m[y * n + x] = (all[bi >> 3] >> (7 - (bi & 7))) & 1;
                    bi++;
                }
            }
    }
    // ---- mask
    int best = 0;
    long bestp = LONG_MAX;
    for (int mask = 0; mask < 8; mask++) {
        Grid t = g;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (!t.fn[y * n + x] && MaskBit(mask, x, y)) t.m[y * n + x] = !t.m[y * n + x];
        DrawFormat(t, mask);
        long p = Penalty(t);
        if (p < bestp) { bestp = p; best = mask; }
    }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (!g.fn[y * n + x] && MaskBit(best, x, y)) g.m[y * n + x] = !g.m[y * n + x];
    DrawFormat(g, best);
    out.size = n;
    out.modules = g.m;
    return out;
}

}  // namespace phonecam
