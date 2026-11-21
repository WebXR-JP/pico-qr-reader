#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"

# Toolchain paths (overridable via environment variables)
WOLVIC_TP="/home/halby/repos/wolvic-webgpu/src/third_party"
ANDROID_SDK="${ANDROID_SDK:-$WOLVIC_TP/android_sdk/public}"
BUILD_TOOLS="${BUILD_TOOLS:-$(ls -d "$ANDROID_SDK"/build-tools/* | sort -V | tail -n 1)}"
PLATFORM_DIR="${PLATFORM_DIR:-$(ls -d "$ANDROID_SDK"/platforms/android-* | sort -V | tail -n 1)}"
ANDROID_JAR="$PLATFORM_DIR/android.jar"
ADB="${ADB:-$ANDROID_SDK/platform-tools/adb}"

JDK_HOME="${JDK_HOME:-$WOLVIC_TP/jdk/current}"
export JAVA_HOME="$JDK_HOME"
export PATH="$JDK_HOME/bin:$PATH"
JAVAC="$JDK_HOME/bin/javac"
KEYTOOL="$JDK_HOME/bin/keytool"

CLANG_BIN="${CLANG_BIN:-$WOLVIC_TP/llvm-build/Release+Asserts/bin}"
NDK_SYSROOT="${NDK_SYSROOT:-$WOLVIC_TP/android_toolchain/ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot}"

TARGET="aarch64-linux-android29"
OUT_APK="$BUILD_DIR/pico-qr-reader.apk"

mkdir -p "$BUILD_DIR/obj" "$BUILD_DIR/classes" "$BUILD_DIR/gen" "$BUILD_DIR/res_compiled" "$BUILD_DIR/apk/lib/arm64-v8a"

echo "==> [1/6] Compiling quirc (C)..."
for src in quirc.c decode.c identify.c version_db.c; do
    "$CLANG_BIN/clang" \
        --target="$TARGET" \
        --sysroot="$NDK_SYSROOT" \
        -O2 -fPIC -c \
        -I"$ROOT_DIR/third_party/quirc" \
        "$ROOT_DIR/third_party/quirc/$src" \
        -o "$BUILD_DIR/obj/${src%.c}.o"
done

echo "==> [2/6] Compiling native C++ OpenXR + Camera..."
for src in pico_camera.cpp main.cpp; do
    "$CLANG_BIN/clang++" \
        --target="$TARGET" \
        --sysroot="$NDK_SYSROOT" \
        -std=c++17 -O2 -fPIC -fno-exceptions -fno-rtti -c \
        -I"$ROOT_DIR/jni" \
        -I"$ROOT_DIR/third_party" \
        -I"$ROOT_DIR/third_party/quirc" \
        "$ROOT_DIR/jni/$src" \
        -o "$BUILD_DIR/obj/${src%.cpp}.o"
done

echo "==> [3/6] Linking libpico_qr_reader.so..."
"$CLANG_BIN/clang++" \
    --target="$TARGET" \
    --sysroot="$NDK_SYSROOT" \
    -fuse-ld=lld \
    -shared \
    -static-libstdc++ \
    --unwindlib=none \
    -Wl,-z,max-page-size=16384 \
    "$BUILD_DIR/obj/"*.o \
    -L"$ROOT_DIR/third_party/lib/arm64-v8a" \
    -lopenxr_loader \
    -lGLESv3 -lEGL -landroid -llog -lm \
    -o "$BUILD_DIR/apk/lib/arm64-v8a/libpico_qr_reader.so"

cp "$ROOT_DIR/third_party/lib/arm64-v8a/libopenxr_loader.so" "$BUILD_DIR/apk/lib/arm64-v8a/libopenxr_loader.so"

echo "==> [4/6] Compiling resources and AndroidManifest.xml..."
"$BUILD_TOOLS/aapt2" compile --dir "$ROOT_DIR/res" -o "$BUILD_DIR/res_compiled/res.zip"
"$BUILD_TOOLS/aapt2" link \
    -o "$BUILD_DIR/base.apk" \
    -I "$ANDROID_JAR" \
    --manifest "$ROOT_DIR/AndroidManifest.xml" \
    --java "$BUILD_DIR/gen" \
    "$BUILD_DIR/res_compiled/res.zip"

echo "==> [5/6] Compiling Java & DEX..."
"$JAVAC" \
    --release 11 \
    -classpath "$ANDROID_JAR" \
    -d "$BUILD_DIR/classes" \
    "$ROOT_DIR/java/dev/pico/qrreader/MainActivity.java" \
    "$BUILD_DIR/gen/dev/pico/qrreader/R.java"

bash "$BUILD_TOOLS/d8" \
    --min-api 29 \
    --lib "$ANDROID_JAR" \
    --output "$BUILD_DIR/apk" \
    "$BUILD_DIR/classes/dev/pico/qrreader/"*.class

echo "==> [6/6] Packaging, aligning (4096-byte page-aligned .so), and signing APK..."
cp "$BUILD_DIR/base.apk" "$BUILD_DIR/unaligned.apk"
python3 - "$BUILD_DIR/unaligned.apk" "$BUILD_DIR/apk" <<'PY'
import os, sys, zipfile
apk_path, apk_dir = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(apk_path, "a") as zf:
    zf.write(os.path.join(apk_dir, "classes.dex"), "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
    for so in ("lib/arm64-v8a/libopenxr_loader.so", "lib/arm64-v8a/libpico_qr_reader.so"):
        zf.write(os.path.join(apk_dir, so), so, compress_type=zipfile.ZIP_STORED)
PY

"$BUILD_TOOLS/zipalign" -f -p 4 "$BUILD_DIR/unaligned.apk" "$OUT_APK"

KEYSTORE="$BUILD_DIR/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    "$KEYTOOL" -genkeypair -v \
        -keystore "$KEYSTORE" \
        -storepass android -keypass android \
        -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Debug,O=PicoQrReader,C=JP" >/dev/null 2>&1
fi

bash "$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-pass pass:android \
    --key-pass pass:android \
    --ks-key-alias androiddebugkey \
    "$OUT_APK"

echo "==> Verifying APK..."
"$BUILD_TOOLS/zipalign" -c -p 4 "$OUT_APK"
"$BUILD_TOOLS/aapt" dump xmltree "$OUT_APK" AndroidManifest.xml | grep -A 2 "handtracking" || true
echo "SUCCESS: $OUT_APK ($(du -h "$OUT_APK" | cut -f1))"

if [ "${1:-}" = "install" ]; then
    echo "==> Installing to connected PICO device..."
    "$ADB" install -r "$OUT_APK"
    "$ADB" shell pm grant dev.pico.qrreader android.permission.CAMERA || true
    echo "==> Installed and granted CAMERA permission."
fi
