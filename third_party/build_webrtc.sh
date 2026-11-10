#!/usr/bin/env bash
# Builds the WebRTC dependencies (libdatachannel + Mbed TLS) from source for Windows x64 with zig,
# into third_party/webrtc/{include,lib}. Pinned versions, so every build uses the same code.
set -euo pipefail
cd "$(dirname "$0")"
LDC_REPO=https://github.com/paullouisageneau/libdatachannel.git
LDC_COMMIT=2afeedc70878043bcd59d2d726e1476824f687c3
MBED_REPO=https://github.com/Mbed-TLS/mbedtls.git
MBED_TAG=v3.6.2
OUT=$PWD/webrtc
WORK=$PWD/.webrtc-build
ZIG="${ZIG:-python3 -m ziglang}"
mkdir -p "$WORK" "$OUT/lib" "$OUT/include"

# zig as a cross compiler for CMake
TC=$WORK/toolchain; mkdir -p "$TC"
for t in cc c++; do printf '#!/bin/sh\nexec %s %s -target x86_64-windows-gnu "$@"\n' "$ZIG" "$t" > "$TC/z$t"; done
for t in ar ranlib rc; do printf '#!/bin/sh\nexec %s %s "$@"\n' "$ZIG" "$t" > "$TC/z$t"; done
chmod +x "$TC"/z*
cat > "$TC/win.cmake" <<CM
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $TC/zcc)
set(CMAKE_CXX_COMPILER $TC/zc++)
set(CMAKE_AR $TC/zar)
set(CMAKE_RANLIB $TC/zranlib)
set(CMAKE_RC_COMPILER $TC/zrc)
set(CMAKE_FIND_ROOT_PATH $WORK/prefix)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
CM

# Mbed TLS (with DTLS-SRTP, needed by WebRTC)
if [ ! -d "$WORK/mbedtls" ]; then
    git clone -q --depth 1 -b "$MBED_TAG" "$MBED_REPO" "$WORK/mbedtls"
    git -C "$WORK/mbedtls" submodule update -q --init --depth 1
fi
python3 "$WORK/mbedtls/scripts/config.py" -f "$WORK/mbedtls/include/mbedtls/mbedtls_config.h" set MBEDTLS_SSL_DTLS_SRTP
cmake -S "$WORK/mbedtls" -B "$WORK/mbedtls/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC/win.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF \
    -DCMAKE_INSTALL_PREFIX="$WORK/prefix" >/dev/null
ninja -C "$WORK/mbedtls/build" install >/dev/null

# libdatachannel (+ libjuice, libsrtp, usrsctp submodules)
if [ ! -d "$WORK/libdatachannel" ]; then
    git init -q "$WORK/libdatachannel"
    git -C "$WORK/libdatachannel" fetch -q --depth 1 "$LDC_REPO" "$LDC_COMMIT"
    git -C "$WORK/libdatachannel" checkout -q FETCH_HEAD
    git -C "$WORK/libdatachannel" submodule update -q --init --recursive --depth 1
fi
cmake -S "$WORK/libdatachannel" -B "$WORK/libdatachannel/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC/win.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DUSE_MBEDTLS=ON -DMbedTLS_DIR="$WORK/prefix/lib/cmake/MbedTLS" \
    -DNO_WEBSOCKET=ON -DNO_EXAMPLES=ON -DNO_TESTS=ON -DBUILD_SHARED_LIBS=OFF >/dev/null
ninja -C "$WORK/libdatachannel/build" datachannel-static >/dev/null

B=$WORK/libdatachannel/build
cp "$B/libdatachannel-static.a" "$B/deps/libjuice/libjuice-static.a" "$B/deps/libsrtp/libsrtp2.a" \
   "$B/deps/usrsctp/usrsctplib/libusrsctp.a" "$OUT/lib/"
cp "$WORK/prefix/lib/"libmbed*.a "$WORK/prefix/lib/libeverest.a" "$WORK/prefix/lib/libp256m.a" "$OUT/lib/"
rm -rf "$OUT/include/rtc" "$OUT/include/mbedtls" "$OUT/include/psa"
cp -r "$WORK/libdatachannel/include/rtc" "$OUT/include/"
cp -r "$WORK/prefix/include/mbedtls" "$WORK/prefix/include/psa" "$OUT/include/"
echo "OK: third_party/webrtc"
