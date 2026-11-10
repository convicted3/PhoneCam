// Minimal QR code generator (byte mode, error correction level M, versions 1-10).
#pragma once
#include <string>
#include <vector>

namespace phonecam {

struct QrCode {
    int size = 0;                 // modules per side (0 = failed)
    std::vector<bool> modules;    // size*size, true = dark
    bool At(int x, int y) const { return modules[y * size + x]; }
};

QrCode MakeQr(const std::string& text);

}  // namespace phonecam
