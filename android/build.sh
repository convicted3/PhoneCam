#!/usr/bin/env bash
# Builds PhoneCam.apk without Android Studio / Gradle.
# Needs: JDK 11+ (javac, jar, jarsigner, keytool), python3 + Pillow, zip.
# tools/android.jar (API 29) and tools/dx-30.0.2.jar are downloaded by tools/fetch_sdk.sh on first build.
# Signing key: $PHONECAM_KEYSTORE if set, else tools/phonecam.keystore (created on first build).
set -euo pipefail
cd "$(dirname "$0")"
[ -f tools/android.jar ] && [ -f tools/dx-30.0.2.jar ] || bash tools/fetch_sdk.sh
OUT=build
rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/apk/res/drawable"

echo "[1/5] javac"
javac -nowarn --release 8 -encoding UTF-8 -cp tools/android.jar -d "$OUT/classes" $(find src -name '*.java') 2>&1 | grep -v "warning: \[options\]" || true
test -f "$OUT/classes/com/phonecam/app/MainActivity.class"

echo "[2/5] dex"
java -cp tools/dx-30.0.2.jar com.android.dx.command.Main --dex --min-sdk-version=21 \
    --output="$OUT/apk/classes.dex" "$OUT/classes"

echo "[3/5] manifest + resources"
python3 tools/make_icon.py "$OUT/apk/res/drawable/icon.png"
python3 tools/arsc.py "$OUT/apk/resources.arsc" icon=res/drawable/icon.png
python3 tools/axml.py "$OUT/apk/AndroidManifest.xml" 7f010000

echo "[4/5] zip"
(cd "$OUT/apk" && zip -q -X -D -r ../unsigned.apk resources.arsc AndroidManifest.xml classes.dex res)

echo "[5/5] sign"
KS=${PHONECAM_KEYSTORE:-tools/phonecam.keystore}
KP=${PHONECAM_KEYSTORE_PASS:-phonecam}
if [ ! -f "$KS" ]; then
    keytool -genkeypair -keystore "$KS" -storepass "$KP" -keypass "$KP" -alias phonecam \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=PhoneCam" >/dev/null 2>&1
fi
cp "$OUT/unsigned.apk" "$OUT/PhoneCam.apk"
jarsigner -keystore "$KS" -storepass "$KP" -keypass "$KP" -sigalg SHA256withRSA -digestalg SHA-256 \
    "$OUT/PhoneCam.apk" phonecam >/dev/null
echo "OK -> $OUT/PhoneCam.apk"
