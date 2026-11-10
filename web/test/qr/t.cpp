#include "../../../pc/core/qr.h"
#include <cstdio>
using namespace phonecam;
int main(int argc, char** argv) {
    QrCode q = MakeQr(argv[1]);
    if (!q.size) { printf("fail\n"); return 1; }
    int s = 8, n = q.size + 8;
    FILE* f = fopen(argv[2], "wb");
    fprintf(f, "P5\n%d %d\n255\n", n * s, n * s);
    for (int y = 0; y < n * s; y++) for (int x = 0; x < n * s; x++) {
        int mx = x / s - 4, my = y / s - 4;
        bool d = mx >= 0 && my >= 0 && mx < q.size && my < q.size && q.At(mx, my);
        fputc(d ? 0 : 255, f);
    }
    fclose(f); printf("%d\n", q.size);
}
