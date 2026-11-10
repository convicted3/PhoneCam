#!/usr/bin/env bash
# Downloads the two Android SDK files the build needs (not stored in the repository):
#   tools/android.jar      - from SDK Platform 29 (Android 10)
#   tools/dx-30.0.2.jar    - from Build-Tools 30.0.2
set -euo pipefail
cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if [ ! -f android.jar ]; then
    curl -fL -o "$TMP/p.zip" https://dl.google.com/android/repository/platform-29_r05.zip
    unzip -q -j "$TMP/p.zip" '*/android.jar' -d "$TMP/p"
    mv "$TMP/p/android.jar" android.jar
fi
if [ ! -f dx-30.0.2.jar ]; then
    curl -fL -o "$TMP/b.zip" https://dl.google.com/android/repository/build-tools_r30.0.2-linux.zip
    unzip -q -j "$TMP/b.zip" '*/lib/dx.jar' -d "$TMP/b"
    mv "$TMP/b/dx.jar" dx-30.0.2.jar
fi
echo "OK: android.jar, dx-30.0.2.jar"
