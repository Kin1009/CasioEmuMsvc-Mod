#!/bin/bash
# Convenience driver for the iPadOS simulator build.
# Requires a complete Xcode/iOS SDK installation.  Pass a device UDID as $1 to
# also install + launch + screenshot on that iPad simulator.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-ipados"
SDK="${IPHONE_SIMULATOR_SDK:-$(xcrun --sdk iphonesimulator --show-sdk-path)}"
DEVICE="${1:-}"

echo "[ipados] SDK: $SDK"
cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_SYSROOT="$SDK" \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCASIOEMU_ENABLE_SENTRY=OFF \
  -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DBUILD_EXECUTABLE=ON \
  -DCURL_USE_OPENSSL=OFF -DCURL_ENABLE_SSL=OFF \
  -DCURL_USE_LIBIDN2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_NGHTTP2=OFF
cmake --build "$BUILD" --target CasioEmuMsvc

APP="$BUILD/CasioEmuMsvc/CasioEmuMsvc.app"
codesign --force --deep --sign - "$APP"
echo "[ipados] app: $APP"

if [ -n "$DEVICE" ]; then
  xcrun simctl install "$DEVICE" "$APP"
  xcrun simctl launch "$DEVICE" com.telecomadm1145.casioemumsvc.ipados
  xcrun simctl io "$DEVICE" screenshot "$ROOT/ipados-simulator.png"
  echo "[ipados] screenshot: $ROOT/ipados-simulator.png"
fi
