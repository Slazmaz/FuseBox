#!/bin/bash
# Builds FuseBox for others' Linux computers, and zips it (make_release.py): on
# Ubuntu 22.04, as root there (a container, or a WSL distribution of its own), for
# its C library is old enough for most. What not every Linux has is linked in: SDL
# and curl from their own releases, the rest from Ubuntu's packages.
#
#    tools/make_linux.sh [<where to build>]
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-/opt/fusebox}
SDL=3.4.20
SDL_SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
CURL=8.23.0
CURL_SHA256=e3de77cfa6367083c776a26647fc9f633b538c06c3baa73d931a252ac8f72b49

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
# SDL's X11, Wayland and the rest are only looked at: it loads them when it runs.
apt-get install -y -q --no-install-recommends gcc g++ cmake ninja-build pkg-config curl \
   ca-certificates python3 binutils libssl-dev zlib1g-dev libzstd-dev libwebp-dev libvpx-dev \
   libgl-dev libegl-dev libgles-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
   libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev wayland-protocols \
   libdbus-1-dev libibus-1.0-dev libudev-dev libdrm-dev libgbm-dev libasound2-dev libpulse-dev
mkdir -p "$WORK/src"
cd "$WORK/src"

get()
{
   [ -f "$1" ] || curl -fsSL --retry 3 -o "$1" "$2"
   echo "$3 *$1" | sha256sum -c -
   tar -xzf "$1"
}
get SDL3-$SDL.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL3-$SDL.tar.gz $SDL_SHA256
get curl-$CURL.tar.gz https://github.com/curl/curl/releases/download/curl-${CURL//./_}/curl-$CURL.tar.gz $CURL_SHA256

cmake -S SDL3-$SDL -B sdl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/lib" \
   -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build sdl
cmake --install sdl >/dev/null
cmake -S curl-$CURL -B curl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$WORK/lib" \
   -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
   -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DHTTP_ONLY=ON \
   -DCURL_USE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=ON -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
   -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_CA_BUNDLE=none \
   -DCURL_CA_PATH=none
cmake --build curl
cmake --install curl >/dev/null

cmake -S "$ROOT" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFUSEBOX_PORTABLE=ON \
   -DOPENSSL_USE_STATIC_LIBS=ON -DCMAKE_PREFIX_PATH="$WORK/lib"
cmake --build "$WORK/build"
strip "$WORK/build/fusebox" "$WORK/build/fusebox-cli" "$WORK/build/libfuseboxlan.so"
echo "Needs:"
readelf -d "$WORK/build/fusebox" "$WORK/build/fusebox-cli" | grep NEEDED | sort -u
readelf -VW "$WORK/build/fusebox" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
python3 "$ROOT/tools/make_release.py" "$WORK/build"
