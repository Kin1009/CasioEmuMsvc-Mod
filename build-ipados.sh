#!/bin/bash
# build-ipados.sh — reproducible iPadOS build + sideload preparation for CasioEmuMsvc.
#
# This is the canonical driver for the port/ipados branch.  It configures and
# builds the iPadOS app, verifies that all bundle resources / data-only plugins
# were copied, signs the .app, optionally produces an IPA and optionally
# installs it on a simulator.  It runs only on macOS with Xcode command-line
# tools because the iOS SDK, clang, codesign and simctl are Apple tools.
#
# The verified configuration (Ninja + explicitly selected clang + an iOS
# simulator SDK) is reproduced here exactly; the Xcode generator needs a
# complete Xcode/iOS SDK install and is documented in docs/IPADOS_PORT.md.
#
# Examples:
#   ./build-ipados.sh --check                     # verify an existing .app only
#   ./build-ipados.sh --dry-run                   # print the exact build commands
#   IPHONE_SIMULATOR_SDK=/path/to/iPhoneSimulator.sdk ./build-ipados.sh
#   ./build-ipados.sh --install booted --launch
#   ./build-ipados.sh --device --team-id TEAMID --identity "Apple Development: ..." --ipa
#
set -euo pipefail

BUNDLE_ID="com.telecomadm1145.casioemumsvc.ipados"
TARGET="CasioEmuMsvc"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IPADOS_DIR="$ROOT/ipados"
ENTITLEMENTS="$IPADOS_DIR/CasioEmuMsvc.entitlements"
ICON_SRC="$IPADOS_DIR/AppIcon.png"
PLISTBUDDY="/usr/libexec/PlistBuddy"

MODE="simulator"
ARCH=""
SDK=""
DEPLOYMENT_TARGET="16.0"
BUILD_DIR=""
CLEAN=0
IDENTITY="-"
TEAM_ID=""
INSTALL_TARGET=""
LAUNCH=0
SCREENSHOT=""
IPA=""
CHECK_ONLY=0
CONFIGURE_ONLY=0
DRY_RUN=0
JOBS=""

log()  { printf '\033[1;34m[ipados]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[ipados]\033[0m OK: %s\n' "$*"; }
warn() { printf '\033[1;33m[ipados]\033[0m WARN: %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[ipados]\033[0m ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: build-ipados.sh [options]

Build the iPadOS CasioEmuMsvc app and prepare it for sideloading.

Options:
  --simulator           Build for the iOS simulator (default).
  --device              Build for a physical iPad (requires a real SDK,
                        development identity and team ID).
  --sdk PATH            Explicit SDK root. Env: IPHONE_SIMULATOR_SDK /
                        IPHONEOS_SDK. Otherwise xcrun is queried.
  --arch ARCH           Override architecture (default: x86_64 on Intel Macs
                        for the simulator, arm64 on Apple Silicon / device).
  --deployment-target V Minimum iPadOS version (default: 16.0).
  --build-dir DIR       Build directory (default: build-ipados or
                        build-ipados-device).
  --clean               Delete the build directory before configuring.
  --identity NAME       codesign identity for device builds ("-" = ad-hoc).
  --team-id ID          Apple developer team ID for device builds.
  --install TARGET      After signing, simctl install on TARGET (UDID or
                        "booted"). Simulator builds only.
  --launch              Launch the app after --install.
  --screenshot PATH     Take a simulator screenshot after --launch.
  --ipa [PATH]          Package the signed .app as an IPA for sideloading
                        (default: <build-dir>/CasioEmuMsvc.ipados.ipa).
  --check               Read-only verification of an existing .app bundle.
  --configure-only      Run only the CMake configure step.
  --dry-run             Print the exact commands without executing them.
  --jobs N              Parallel build jobs (default: hw.logicalcpu).
  -h, --help            Show this help.

Prerequisites (macOS only): Xcode command-line tools, CMake >= 3.20, Ninja,
codesign, PlistBuddy. A full Xcode install is required for device builds.
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --simulator)           MODE="simulator"; shift ;;
    --device)              MODE="device"; shift ;;
    --sdk)                 [ "$#" -ge 2 ] || die "--sdk needs a path"; SDK="$2"; shift 2 ;;
    --sdk=*)               SDK="${1#*=}"; shift ;;
    --arch)                [ "$#" -ge 2 ] || die "--arch needs a value"; ARCH="$2"; shift 2 ;;
    --deployment-target)   [ "$#" -ge 2 ] || die "--deployment-target needs a version"; DEPLOYMENT_TARGET="$2"; shift 2 ;;
    --build-dir)           [ "$#" -ge 2 ] || die "--build-dir needs a path"; BUILD_DIR="$2"; shift 2 ;;
    --clean)               CLEAN=1; shift ;;
    --identity)            [ "$#" -ge 2 ] || die "--identity needs a value"; IDENTITY="$2"; shift 2 ;;
    --team-id)             [ "$#" -ge 2 ] || die "--team-id needs a value"; TEAM_ID="$2"; shift 2 ;;
    --install)             [ "$#" -ge 2 ] || die "--install needs a device UDID or 'booted'"; INSTALL_TARGET="$2"; shift 2 ;;
    --launch)              LAUNCH=1; shift ;;
    --screenshot)          [ "$#" -ge 2 ] || die "--screenshot needs a path"; SCREENSHOT="$2"; shift 2 ;;
    --ipa)                 if [ "$#" -ge 2 ] && [ "${2#-}" = "$2" ]; then IPA="$2"; shift 2; else IPA="AUTO"; shift; fi ;;
    --check)               CHECK_ONLY=1; shift ;;
    --configure-only)      CONFIGURE_ONLY=1; shift ;;
    --dry-run)             DRY_RUN=1; shift ;;
    --jobs)                [ "$#" -ge 2 ] || die "--jobs needs a number"; JOBS="$2"; shift 2 ;;
    -h|--help)             usage; exit 0 ;;
    *)                     die "unknown argument: $1 (see --help)" ;;
  esac
done

if [ -z "$ARCH" ]; then
  if [ "$MODE" = "device" ] || [ "$(uname -m)" = "arm64" ]; then ARCH="arm64"; else ARCH="x86_64"; fi
fi
if [ -z "$BUILD_DIR" ]; then
  if [ "$MODE" = "device" ]; then BUILD_DIR="$ROOT/build-ipados-device"; else BUILD_DIR="$ROOT/build-ipados"; fi
fi
APP="$BUILD_DIR/$TARGET/$TARGET.app"

run() {
  printf '  $'; printf ' %q' "$@"; printf '\n'
  if [ "$DRY_RUN" -eq 0 ]; then "$@"; fi
}

require_tools() {
  local tool
  for tool in cmake ninja codesign "$PLISTBUDDY"; do
    if [ "$tool" = "$PLISTBUDDY" ]; then
      [ -x "$tool" ] || die "missing Apple tool: $tool (Xcode command-line tools required)"
    else
      command -v "$tool" >/dev/null 2>&1 || die "missing tool on PATH: $tool"
    fi
  done
  if [ "$DRY_RUN" -eq 0 ]; then
    command -v xcrun >/dev/null 2>&1 || die "missing tool on PATH: xcrun (macOS/Xcode required)"
  fi
  if [ -n "$IPA" ] && [ "$DRY_RUN" -eq 0 ]; then
    command -v zip >/dev/null 2>&1 || die "missing tool on PATH: zip (needed for --ipa)"
  fi
}

resolve_sdk() {
  if [ -n "$SDK" ]; then
    printf '%s\n' "$SDK"
    return
  fi
  if [ "$MODE" = "simulator" ]; then
    SDK="${IPHONE_SIMULATOR_SDK:-}"
    [ -n "$SDK" ] || SDK="$(xcrun --sdk iphonesimulator --show-sdk-path 2>/dev/null || true)"
  else
    SDK="${IPHONEOS_SDK:-}"
    [ -n "$SDK" ] || SDK="$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || true)"
  fi
  [ -n "$SDK" ] || die "no SDK found. Install the iOS SDK with Xcode, or set IPHONE_SIMULATOR_SDK / IPHONEOS_SDK (or pass --sdk)."
  [ -d "$SDK" ] || die "SDK directory does not exist: $SDK"
  if [ ! -f "$SDK/SDKSettings.plist" ]; then
    warn "SDK '$SDK' has no SDKSettings.plist; if CMake compiler detection fails, point to a complete SDK or a synthetic SDK mirror (see docs/IPADOS_PORT.md §6)."
  fi
  printf '%s\n' "$SDK"
}

# Read-only bundle checks.  Every condition here is a real sideload failure
# mode encountered while porting this app.
verify_bundle() {
  [ -d "$APP" ] || die "app bundle not found: $APP"
  local rel
  for rel in "Info.plist" "$TARGET" "Plugins" "themes" "locales" "docs" "wiz" "roms.db" "AppIcon.png"; do
    [ -e "$APP/$rel" ] || die "bundle is missing '$rel': $APP/$rel"
  done
  if ! find "$APP/Plugins" -type f -name 'plugin.json' -print -quit | grep -q .; then
    die "no data-only plugin manifest under $APP/Plugins (script plugin assets were not bundled)"
  fi
  for rel in themes locales docs wiz Plugins; do
    [ -n "$(find "$APP/$rel" -mindepth 1 -print -quit)" ] || die "bundled '$rel' directory is empty"
  done
  if [ -d "$APP/Resources" ]; then
    die "iOS bundle contains a 'Resources/' directory — simctl install reports 'Missing bundle ID' (see docs/IPADOS_PORT.md §2.1)"
  fi
  local native
  native="$(find "$APP" \( -name '*.dylib' -o -name '*.plugin' -o -name '*.framework' \) -print -quit)"
  [ -z "$native" ] || warn "native plugin/binary found in bundle ($native); iPadOS forbids dlopen/JIT — this must be a data-only package"
  [ -x "$PLISTBUDDY" ] || die "PlistBuddy unavailable; cannot check Info.plist"
  local got
  got="$("$PLISTBUDDY" -c 'Print :CFBundleIdentifier' "$APP/Info.plist" 2>/dev/null || true)"
  [ "$got" = "$BUNDLE_ID" ] || die "wrong bundle ID in $APP/Info.plist: '$got' (expected '$BUNDLE_ID')"
  "$PLISTBUDDY" -c 'Print :CFBundleIconFile' "$APP/Info.plist" >/dev/null 2>&1 \
    || die "Info.plist has no CFBundleIconFile; the sideloaded app will have no icon"
  codesign --verify --deep --strict "$APP" >/dev/null 2>&1 \
    || die "codesign verification failed for $APP"
  ok "bundle verified: $APP"
}

# Build-path only: make sure the icon and its plist key exist even when an
# older build directory is reused.  CMake also copies the icon at build time;
# this is a safety net, not a substitute for the build rule.
prepare_bundle() {
  if [ -f "$ICON_SRC" ]; then
    if [ ! -f "$APP/AppIcon.png" ] || ! cmp -s "$ICON_SRC" "$APP/AppIcon.png"; then
      run cp "$ICON_SRC" "$APP/AppIcon.png"
    fi
  else
    warn "no icon asset at $ICON_SRC; keeping any icon already in the bundle"
  fi
  if ! "$PLISTBUDDY" -c 'Print :CFBundleIconFile' "$APP/Info.plist" >/dev/null 2>&1; then
    run "$PLISTBUDDY" -c 'Add :CFBundleIconFile string AppIcon.png' "$APP/Info.plist"
  fi
}

sign_app() {
  if [ "$MODE" = "simulator" ]; then
    log "Ad-hoc signing simulator bundle (matches the verified install path)"
    run codesign --force --deep --sign - "$APP"
  else
    if [ "$IDENTITY" = "-" ]; then
      warn "device bundle is ad-hoc signed; it cannot be installed on an unjailbroken iPad. Use --identity/--team-id."
    else
      [ -n "$TEAM_ID" ] || die "device signing requires --team-id as well as --identity"
    fi
    log "Signing device bundle with entitlements: $ENTITLEMENTS"
    run codesign --force --deep --sign "$IDENTITY" --entitlements "$ENTITLEMENTS" "$APP"
  fi
}

make_ipa() {
  local out="$1"
  local tmp
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ipados-ipa.XXXXXX")"
  mkdir -p "$tmp/Payload"
  cp -R "$APP" "$tmp/Payload/"
  (cd "$tmp" && zip -qry "$out" Payload)
  rm -rf "$tmp"
  ok "IPA ready for sideloading: $out"
}

print_next_steps() {
  echo ""
  log "NEXT STEPS"
  if [ "$MODE" = "simulator" ]; then
    echo "  1. Pick an iPad simulator:  xcrun simctl list devices | grep iPad"
    echo "  2. Boot it:                 xcrun simctl boot <UDID>"
    echo "  3. Install:                 xcrun simctl install <UDID> \"$APP\""
    echo "  4. Launch:                  xcrun simctl launch <UDID> $BUNDLE_ID"
    echo "  5. Screenshot:              xcrun simctl io <UDID> screenshot shot.png"
    echo "  Note: on the Intel host used for this port the iOS 26.3 simulator's"
    echo "  SpringBoard/RenderBox/Metal pipeline can deadlock, so a screenshot may"
    echo "  show a stale home screen even while the app process is running. That is"
    echo "  a simulator compositor issue, not evidence of a working/failed app UI."
    echo "  No visual verification is claimed by this script."
  else
    echo "  1. Sideload the signed app or IPA with Xcode/AltStore/Sideloadly using"
    echo "     team $TEAM_ID and identity '$IDENTITY'."
    echo "  2. Trust the developer certificate in Settings on the iPad."
    echo "  3. A real iPadOS device, a full Xcode install and a valid Apple"
    echo "     development certificate are mandatory; this script cannot supply them."
  fi
  echo "  Bundle ID: $BUNDLE_ID"
  echo "  Data-only plugins: $APP/Plugins (no dlopen, no JIT on iPadOS)"
}

# ---------------------------------------------------------------- main ------
if [ "$(uname -s)" != "Darwin" ]; then
  die "macOS + Xcode are required (iOS SDK, clang, codesign, simctl are Apple tools). On Linux use the host-side smoke tests instead."
fi
require_tools

if [ "$CHECK_ONLY" -eq 1 ]; then
  log "Check-only mode: verifying $APP (nothing is built or modified)"
  verify_bundle
  exit 0
fi

SDK="$(resolve_sdk)"
log "mode=$MODE arch=$ARCH sdk=$SDK deployment-target=$DEPLOYMENT_TARGET build-dir=$BUILD_DIR"

if [ "$MODE" = "device" ] && [ "$DRY_RUN" -eq 0 ] && [ "$CONFIGURE_ONLY" -eq 0 ]; then
  [ -n "$TEAM_ID" ] || die "device builds require --team-id (and a paid/personal Apple developer team)"
  [ "$IDENTITY" != "-" ] || die "device builds require --identity 'Apple Development: ...' (ad-hoc cannot be installed)"
fi
if [ -n "$INSTALL_TARGET" ] && [ "$MODE" != "simulator" ]; then
  die "--install only targets simulators; sideload the IPA on a device instead"
fi

CMAKE_ARGS=(
  cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja
  -DCMAKE_SYSTEM_NAME=iOS
  -DCMAKE_C_COMPILER=/usr/bin/clang
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++
  -DCMAKE_OSX_SYSROOT="$SDK"
  -DCMAKE_OSX_ARCHITECTURES="$ARCH"
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
  -DCMAKE_BUILD_TYPE=Release
  -DBUILD_EXECUTABLE=ON
  -DCASIOEMU_ENABLE_SENTRY=OFF
  -DCASIOEMU_PLUGIN_TESTS=OFF
  -DCURL_USE_OPENSSL=OFF -DCURL_ENABLE_SSL=OFF
  -DCURL_USE_LIBIDN2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_NGHTTP2=OFF
)

if [ "$CLEAN" -eq 1 ]; then
  run rm -rf "$BUILD_DIR"
fi

log "Configuring CMake"
run "${CMAKE_ARGS[@]}"
if [ "$DRY_RUN" -eq 1 ] || [ "$CONFIGURE_ONLY" -eq 1 ]; then
  [ "$DRY_RUN" -eq 1 ] && log "Dry run: commands above were printed only; no build/sign performed."
  exit 0
fi

if [ -z "$JOBS" ]; then
  JOBS="$(sysctl -n hw.logicalcpu 2>/dev/null || printf '4')"
fi
log "Building target $TARGET with $JOBS jobs"
run cmake --build "$BUILD_DIR" --target "$TARGET" -- -j"$JOBS"

log "Finalizing bundle (icon, resources, data-only plugins)"
prepare_bundle
sign_app
verify_bundle

if [ -n "$IPA" ]; then
  [ "$IPA" != "AUTO" ] || IPA="$BUILD_DIR/CasioEmuMsvc.ipados.ipa"
  make_ipa "$IPA"
fi

if [ -n "$INSTALL_TARGET" ]; then
  log "Installing on simulator '$INSTALL_TARGET'"
  run xcrun simctl install "$INSTALL_TARGET" "$APP"
  if [ "$LAUNCH" -eq 1 ]; then
    run xcrun simctl launch "$INSTALL_TARGET" "$BUNDLE_ID"
  fi
  if [ -n "$SCREENSHOT" ]; then
    warn "screenshot may show a stale frame on the Intel-host simulator; see notes below"
    run xcrun simctl io "$INSTALL_TARGET" screenshot "$SCREENSHOT"
  fi
fi

print_next_steps
