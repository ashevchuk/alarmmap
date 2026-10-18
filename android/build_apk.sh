#!/bin/sh
# Builds an arm64 APK that shows a bundled snapshot of targets and alerts.
# Usage: android/build_apk.sh TARGETS_JSON ALERTS_JSON
#
# Needs ANDROID_HOME (default ~/Android/Sdk) with an NDK, build-tools and a
# platform, the region geometry the desktop build caches in
# ~/.cache/alarmmap, and Noto Sans Medium. Output: build-android/alarmmap.apk,
# signed with the debug key in ~/.android/debug.keystore.
set -eu

if [ $# -ne 2 ]; then
    echo "usage: $0 TARGETS_JSON ALERTS_JSON" >&2
    exit 2
fi
targets="$(realpath "$1")"
alerts="$(realpath "$2")"

root="$(realpath "$(dirname "$0")/..")"
out="$root/build-android"
sdk="${ANDROID_HOME:-$HOME/Android/Sdk}"
ndk="$(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
build_tools="$(find "$sdk/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
platform="$(find "$sdk/platforms" -mindepth 1 -maxdepth 1 -name 'android-[0-9]*' | sort -V | tail -n 1)"
geometry="$HOME/.cache/alarmmap/regions-standard-v1.geojson"
font=/usr/share/fonts/noto/NotoSans-Medium.ttf
min_sdk=26
abi=arm64-v8a
raylib_version=6.2

for path in "$ndk" "$build_tools" "$platform" "$targets" "$alerts" "$geometry" "$font"; do
    if [ ! -e "$path" ]; then
        echo "missing: $path" >&2
        exit 1
    fi
done
toolchain="$ndk/build/cmake/android.toolchain.cmake"
android_cmake="-DCMAKE_TOOLCHAIN_FILE=$toolchain -DANDROID_ABI=$abi -DANDROID_PLATFORM=android-$min_sdk"

if [ ! -f "$out/sysroot/lib/libraylib.a" ]; then
    [ -d "$out/raylib" ] || git clone --depth 1 --branch "$raylib_version" \
        https://github.com/raysan5/raylib.git "$out/raylib"
    # shellcheck disable=SC2086
    cmake -S "$out/raylib" -B "$out/raylib-build" -G Ninja $android_cmake \
        -DPLATFORM=Android -DOPENGL_VERSION="ES 3.0" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$out/sysroot"
    cmake --build "$out/raylib-build"
    cmake --install "$out/raylib-build"
fi

# shellcheck disable=SC2086
cmake -S "$root" -B "$out/app-build" -G Ninja $android_cmake -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -Draylib_DIR="$out/sysroot/lib/cmake/raylib"
cmake --build "$out/app-build" --target alarmmap

stage="$out/apk"
rm -rf "$stage"
mkdir -p "$stage/assets/fonts" "$stage/res/drawable" "$stage/lib/$abi"
cp "$root/data/cities.geojson" "$root/data/crimea.geojson" "$geometry" "$stage/assets/"
cp "$font" "$stage/assets/fonts/"
cp "$targets" "$stage/assets/targets.json"
cp "$alerts" "$stage/assets/alerts.json"
cp "$root/packaging/alarmmap.png" "$stage/res/drawable/icon.png"
cp "$out/app-build/libalarmmap.so" "$stage/lib/$abi/"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" "$stage/lib/$abi/libalarmmap.so"

version="$(sed -n 's/^project(alarmmap VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
"$build_tools/aapt2" compile --dir "$stage/res" -o "$stage/res.zip"
"$build_tools/aapt2" link -o "$stage/unaligned.apk" -I "$platform/android.jar" \
    --manifest "$root/android/AndroidManifest.xml" -A "$stage/assets" \
    --min-sdk-version "$min_sdk" --target-sdk-version "${platform##*-}" \
    --version-code 1 --version-name "$version" "$stage/res.zip"
# Native libraries go in uncompressed so the system can map them from the APK.
(cd "$stage" && zip -q -0 unaligned.apk "lib/$abi/libalarmmap.so")
"$build_tools/zipalign" -f -P 16 4 "$stage/unaligned.apk" "$stage/aligned.apk"

keystore="$HOME/.android/debug.keystore"
if [ ! -f "$keystore" ]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android \
    --out "$out/alarmmap.apk" "$stage/aligned.apk"
"$build_tools/apksigner" verify "$out/alarmmap.apk"
echo "$out/alarmmap.apk"
