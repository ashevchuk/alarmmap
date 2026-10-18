#!/bin/sh
# Builds an arm64 APK of alarmmap.
# Usage: android/build_apk.sh
#
# Needs ANDROID_HOME (default ~/Android/Sdk) with an NDK, build-tools and a
# platform, gpg for the curl signature, and Noto Sans Medium. Output:
# build-android/alarmmap.apk.
#
# With ALARMMAP_KEYSTORE (a PKCS12 file) and ALARMMAP_KEYSTORE_PASSWORD set,
# the APK is signed with that keystore's "alarmmap" key, so it installs as an
# update of earlier releases; otherwise with the debug key in
# ~/.android/debug.keystore.
set -eu

root="$(realpath "$(dirname "$0")/..")"
out="$root/build-android"
sdk="${ANDROID_HOME:-$HOME/Android/Sdk}"
ndk="$(find "$sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
build_tools="$(find "$sdk/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
# A stable API level: the newest platform in an SDK can be a preview.
platform="$sdk/platforms/android-36"
font="$(find /usr/share/fonts -name NotoSans-Medium.ttf | head -n 1)"
min_sdk=24
abi=arm64-v8a
raylib_version=6.2
mbedtls_version=3.6.7
mbedtls_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
curl_version=8.22.0
# Daniel Stenberg signs curl releases.
curl_signing_key=27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2

for path in "$ndk" "$build_tools" "$platform" "${font:-NotoSans-Medium.ttf}"; do
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

mkdir -p "$out/src"
if [ ! -f "$out/sysroot/lib/libmbedtls.a" ]; then
    archive="$out/src/mbedtls-$mbedtls_version.tar.bz2"
    curl -fsSL -o "$archive" \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2"
    echo "$mbedtls_sha256  $archive" | sha256sum -c -
    tar -xjf "$archive" -C "$out/src"
    # libcurl shares one random generator between connections, and the app
    # opens several at once; mbedTLS guards it only with threading enabled.
    # This must be in the installed config header: it changes struct layouts
    # that libcurl compiles against.
    for option in MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD; do
        python3 "$out/src/mbedtls-$mbedtls_version/scripts/config.py" \
            -f "$out/src/mbedtls-$mbedtls_version/include/mbedtls/mbedtls_config.h" set "$option"
    done
    # shellcheck disable=SC2086
    cmake -S "$out/src/mbedtls-$mbedtls_version" -B "$out/mbedtls-build" -G Ninja $android_cmake \
        -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF -DCMAKE_INSTALL_PREFIX="$out/sysroot" \
        -DCMAKE_C_FLAGS="-DMBEDTLS_PLATFORM_DEV_RANDOM=\\\"/dev/urandom\\\""
    cmake --build "$out/mbedtls-build"
    cmake --install "$out/mbedtls-build"
fi

# mbedTLS calls getrandom() only on glibc and reads /dev/random elsewhere,
# which blocks on older Android kernels until their entropy pool fills up;
# the build points it at /dev/urandom, the same generator without the wait.
#
# mbedTLS reads every certificate in a directory, so libcurl can use the
# system store as is; OpenSSL would look certificates up by a hash that
# differs from Android's file names.
if [ ! -f "$out/sysroot/lib/libcurl.a" ]; then
    archive="$out/src/curl-$curl_version.tar.xz"
    curl -fsSL -o "$archive" "https://curl.se/download/curl-$curl_version.tar.xz"
    curl -fsSL -o "$archive.asc" "https://curl.se/download/curl-$curl_version.tar.xz.asc"
    keyring="$(mktemp -d)"
    curl -fsSL https://daniel.haxx.se/mykey.asc | gpg --homedir "$keyring" --quiet --import
    gpg --homedir "$keyring" --status-fd 1 --verify "$archive.asc" "$archive" 2>/dev/null |
        grep -q "VALIDSIG $curl_signing_key"
    rm -rf "$keyring"
    tar -xJf "$archive" -C "$out/src"
    # shellcheck disable=SC2086
    cmake -S "$out/src/curl-$curl_version" -B "$out/curl-build" -G Ninja $android_cmake \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_FIND_ROOT_PATH="$out/sysroot" -DCMAKE_PREFIX_PATH="$out/sysroot" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
        -DCURL_USE_OPENSSL=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
        -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF \
        -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=/system/etc/security/cacerts -DCURL_CA_FALLBACK=OFF \
        -DCMAKE_INSTALL_PREFIX="$out/sysroot"
    cmake --build "$out/curl-build"
    cmake --install "$out/curl-build"
fi

# shellcheck disable=SC2086
cmake -S "$root" -B "$out/app-build" -G Ninja $android_cmake -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_FIND_ROOT_PATH="$out/sysroot" -DCMAKE_PREFIX_PATH="$out/sysroot"
cmake --build "$out/app-build" --target alarmmap

stage="$out/apk"
rm -rf "$stage"
mkdir -p "$stage/assets/fonts" "$stage/res/drawable" "$stage/lib/$abi"
cp "$root/data/cities.geojson" "$root/data/crimea.geojson" "$stage/assets/"
cp "$font" "$stage/assets/fonts/"
cp "$root/packaging/alarmmap.png" "$stage/res/drawable/icon.png"
cp "$out/app-build/libalarmmap.so" "$stage/lib/$abi/"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" "$stage/lib/$abi/libalarmmap.so"

version="$(sed -n 's/^project(alarmmap VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
# Android installs an APK over another only with an equal or higher code.
version_code="$(echo "$version" | awk -F. '{ print $1 * 10000 + $2 * 100 + $3 }')"
case "$version_code" in
    ''|*[!0-9]*) echo "cannot derive a version code from '$version'" >&2; exit 1 ;;
esac
"$build_tools/aapt2" compile --dir "$stage/res" -o "$stage/res.zip"
"$build_tools/aapt2" link -o "$stage/unaligned.apk" -I "$platform/android.jar" \
    --manifest "$root/android/AndroidManifest.xml" -A "$stage/assets" \
    --min-sdk-version "$min_sdk" --target-sdk-version "${platform##*-}" \
    --version-code "$version_code" --version-name "$version" "$stage/res.zip"
# Native libraries go in uncompressed so the system can map them from the APK.
(cd "$stage" && zip -q -0 unaligned.apk "lib/$abi/libalarmmap.so")
"$build_tools/zipalign" -f -P 16 4 "$stage/unaligned.apk" "$stage/aligned.apk"

if [ -n "${ALARMMAP_KEYSTORE:-}" ]; then
    # The password stays in the environment, out of every command line.
    "$build_tools/apksigner" sign --ks "$ALARMMAP_KEYSTORE" --ks-key-alias alarmmap \
        --ks-pass env:ALARMMAP_KEYSTORE_PASSWORD --out "$out/alarmmap.apk" "$stage/aligned.apk"
else
    keystore="$HOME/.android/debug.keystore"
    if [ ! -f "$keystore" ]; then
        mkdir -p "$(dirname "$keystore")"
        keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
            -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=Android Debug,O=Android,C=US"
    fi
    "$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android \
        --out "$out/alarmmap.apk" "$stage/aligned.apk"
fi
"$build_tools/apksigner" verify "$out/alarmmap.apk"
echo "$out/alarmmap.apk"
