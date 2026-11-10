#!/usr/bin/env bash
# Cross-compiles everything for Windows with zig (pip install ziglang). Output in build/.
set -euo pipefail
cd "$(dirname "$0")"
Z="${ZIG:-python3 -m ziglang}"
OUT=build
mkdir -p "$OUT" "$OUT/obj"
SC=../third_party/softcam/src
LIBS="-lws2_32 -liphlpapi -lmfplat -lole32"
# WebRTC receiver for the Telegram/browser mode: libdatachannel + mbedTLS, prebuilt static libs in third_party/webrtc
WR=../third_party/webrtc
WEB_SRC="core/web_link.cpp core/qr.cpp"
WEB_INC="-I$WR/include -DRTC_STATIC"
WEB_LIBS="$WR/lib/libdatachannel-static.a $WR/lib/libjuice-static.a $WR/lib/libsrtp2.a $WR/lib/libusrsctp.a $WR/lib/libmbedtls.a $WR/lib/libmbedx509.a $WR/lib/libmbedcrypto.a $WR/lib/libeverest.a $WR/lib/libp256m.a -lbcrypt -lcrypt32 -lmswsock"

echo "[1/4] virtual camera (DirectShow filter, x64 + x86)"
for T in x86_64 x86; do
    O="$OUT/obj/sc-$T"; mkdir -p "$O"
    for f in $SC/baseclasses/*.cpp $SC/softcamcore/*.cpp $SC/softcam/softcam.cpp $SC/mingw_stubs.cpp; do
        $Z c++ -target $T-windows-gnu -fms-extensions -O2 -w -I$SC -I$SC/baseclasses -include $SC/sal_shim.h \
            -DUNICODE -D_UNICODE -c "$f" -o "$O/$(basename "$f").o"
    done
    BITS=$([ $T = x86 ] && echo 32 || echo 64)
    $Z c++ -target $T-windows-gnu -shared -s -o "$OUT/PhoneCamVirtual$BITS.dll" "$O"/*.o $SC/softcam/softcam.def \
        -lole32 -loleaut32 -lstrmiids -luuid -lwinmm -luser32 -lgdi32 -ladvapi32
done

echo "[2/4] PhoneCam.exe"
$Z c++ -target x86_64-windows-gnu -std=c++17 -O2 -municode -Wl,--subsystem,windows -s -o "$OUT/PhoneCam.exe" \
    $WEB_INC app/main.cpp core/phonecam_client.cpp core/h264_decoder.cpp core/night_filter.cpp $WEB_SRC app/app.rc \
    $WEB_LIBS $LIBS -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32

echo "[3/4] OBS plugin"
if [ ! -f third_party/libobs.a ]; then
    $Z dlltool -m i386:x86-64 -d third_party/obs.def -l third_party/libobs.a
fi
$Z c++ -target x86_64-windows-gnu -std=c++17 -O2 -shared -s -Ithird_party/libobs-headers -o "$OUT/phonecam-obs.dll" \
    $WEB_INC obs-plugin/phonecam-obs.cpp core/phonecam_client.cpp core/h264_decoder.cpp core/night_filter.cpp $WEB_SRC obs-plugin/phonecam-obs.def \
    third_party/libobs.a $WEB_LIBS $LIBS

echo "[4/4] done"
ls -la "$OUT"/*.exe "$OUT"/*.dll
