#!/usr/bin/env bash
# Collects the built files into dist/: PhoneCam-Windows.zip (PC program + OBS plugin + instructions) and PhoneCam.apk.
# Run after pc/build.sh and android/build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist
python3 - <<'PY'
import os, zipfile
files = [
    ("pc/build/PhoneCam.exe", "PhoneCam/Windows/PhoneCam.exe"),
    ("pc/build/PhoneCamVirtual64.dll", "PhoneCam/Windows/PhoneCamVirtual64.dll"),
    ("pc/build/PhoneCamVirtual32.dll", "PhoneCam/Windows/PhoneCamVirtual32.dll"),
    ("pc/build/phonecam-obs.dll", "PhoneCam/OBS plugin/phonecam-obs.dll"),
    ("packaging/obs/Install OBS plugin.bat", "PhoneCam/OBS plugin/Install OBS plugin.bat"),
    ("packaging/obs/Uninstall OBS plugin.bat", "PhoneCam/OBS plugin/Uninstall OBS plugin.bat"),
    ("packaging/Инструкция.txt", "PhoneCam/Инструкция.txt"),
    ("packaging/Instructions.txt", "PhoneCam/Instructions.txt"),
    ("packaging/Інструкція.txt", "PhoneCam/Інструкція.txt"),
]
with zipfile.ZipFile("dist/PhoneCam-Windows.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for src, dst in files:
        z.write(src, dst)
print("dist/PhoneCam-Windows.zip")
PY
cp android/build/PhoneCam.apk dist/PhoneCam.apk
ls -la dist
