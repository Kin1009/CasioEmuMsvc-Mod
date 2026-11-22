#!/bin/bash
# sideload-package.sh — reproducible iPadOS sideload packaging for CasioEmuMsvc.
#
# This script is APPLE-ONLY (macOS + Xcode command-line tools).  It takes an
# already-built .app, verifies it against the iPadOS sideload rules, prepares a
# *copy* of the bundle, signs it, optionally produces an IPA, and optionally
# installs/launches it on an iOS simulator.  It never modifies the build tree.
#
# PREREQUISITES
#   * macOS with Xcode command-line tools (codesign, PlistBuddy, plutil, xcrun,
#     simctl, actool).  Xcode 26.3 / iOS 26.3 simulator used for verification.
#   * A built app bundle, e.g. build-ipados/CasioEmuMsvc/CasioEmuMsvc.app
#     (produced by ./build-ipados.sh in this tree).
#   * Simulator install: a booted iPad simulator (xcrun simctl list devices).
#   * Device install: a full Xcode install, an iPhoneOS SDK, an Apple
#     Development identity, AND a provisioning profile whose application-
#     identifier matches the app's bundle ID.  THIS SCRIPT DOES NOT INSTALL ON
#     A PHYSICAL DEVICE (no hardware writes); it only prepares a signed IPA
#     for AltStore / Sideloadly / Xcode.  Trusting the certificate is a manual
#     step on the iPad.
#
# HONEST LIMITATIONS
#   * iPadOS forbids JIT, unsigned executable memory and dlopen() of unsigned
#     dylibs.  The app ships the portable interpreter and data-only plugin
#     JSON; this script rejects bundles containing native dylibs/frameworks or
#     native_entry plugin manifests rather than trying to smuggle them in.
#   * `UIRequiredDeviceCapabilities=arm64` is stripped from SIMULATOR bundles
#     because the Intel-host simulator binary is x86_64; it is kept for device
#     bundles.  simctl happens to ignore the mismatch, but leaving it in is
#     misleading and can confuse third-party installers.
#   * Ad-hoc signing is valid for the simulator only.  `--mode device` refuses
#     to sign ad-hoc and requires --identity/--team-id/--profile.
#   * On the Intel host used for the port the iOS 26.3 simulator compositor
#     (SpringBoard RenderBox/Metal) can deadlock.  Launch/install may work
#     while `simctl screenshot` returns a stale home-screen frame.  This
#     script does NOT claim visual verification.

set -euo pipefail

BUNDLE_ID="com.telecomadm1145.casioemumsvc.ipados"
TARGET="CasioEmuMsvc"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLISTBUDDY="/usr/libexec/PlistBuddy"
DEV_ENT="$HERE/CasioEmuMsvc.entitlements"
REL_ENT="$HERE/CasioEmuMsvc.release.entitlements"
TIMEOUT_HELPER="$HERE/run_timeout.py"
SIM_TIMEOUT="${SIM_TIMEOUT:-45}"

APP=""
MODE="simulator"
OUT=""
IDENTITY="-"
TEAM_ID=""
PROFILE=""
ENTITLEMENTS=""
INSTALL_TARGET=""
LAUNCH=0
MAKE_IPA=0
CHECK_ONLY=0
DRY_RUN=0
ACTOOL=0

log()  { printf '\033[1;34m[sideload]\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m[sideload]\033[0m OK: %s\n' "$*"; }
warn() { printf '\033[1;33m[sideload]\033[0m WARN: %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[sideload]\033[0m ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: sideload-package.sh --app PATH [options]

Options:
  --app PATH            Built .app bundle to package (required, unless --help).
  --mode simulator|device   Target mode (default: simulator).
  --out DIR             Output directory (default: <repo>/sideload-out).
  --identity NAME       Device codesign identity ("Apple Development: ...").
  --team-id ID          Apple developer team ID (device only).
  --profile PATH        embedded.mobileprovision to embed in a device bundle.
  --entitlements PATH   Override entitlement plist (default:
                        ipados/CasioEmuMsvc.entitlements, or the .release.
                        variant when --release-entitlements is used).
  --release-entitlements  Use the no-get-task-allow distribution entitlements.
  --install UDID|booted After signing, simctl install on this simulator.
  --launch              Launch after --install (simulator only).
  --ipa                 Package the signed bundle as an IPA.
  --check               Read-only verification of --app (no writes).
  --actool              Validate the app icon with xcrun actool (temp catalog).
  --dry-run             Print commands without executing them.
  -h, --help            Show this help.

Examples:
  ./ipados/sideload-package.sh --check --app build-ipados/CasioEmuMsvc/CasioEmuMsvc.app
  ./ipados/sideload-package.sh --app build-ipados/CasioEmuMsvc/CasioEmuMsvc.app \
      --mode simulator --ipa --install booted --launch --actool
  ./ipados/sideload-package.sh --app build-ipados-device/CasioEmuMsvc/CasioEmuMsvc.app \
      --mode device --identity "Apple Development: ..." --team-id TEAMID \
      --profile /path/to/CasioEmuMsvc.mobileprovision --release-entitlements --ipa
EOF
}

RELEASE_ENT=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --app)                [ "$#" -ge 2 ] || die "--app needs a path"; APP="$2"; shift 2 ;;
    --app=*)              APP="${1#*=}"; shift ;;
    --mode)               [ "$#" -ge 2 ] || die "--mode needs a value"; MODE="$2"; shift 2 ;;
    --out)                [ "$#" -ge 2 ] || die "--out needs a path"; OUT="$2"; shift 2 ;;
    --identity)           [ "$#" -ge 2 ] || die "--identity needs a value"; IDENTITY="$2"; shift 2 ;;
    --team-id)            [ "$#" -ge 2 ] || die "--team-id needs a value"; TEAM_ID="$2"; shift 2 ;;
    --profile)            [ "$#" -ge 2 ] || die "--profile needs a path"; PROFILE="$2"; shift 2 ;;
    --entitlements)       [ "$#" -ge 2 ] || die "--entitlements needs a path"; ENTITLEMENTS="$2"; shift 2 ;;
    --release-entitlements) RELEASE_ENT=1; shift ;;
    --install)            [ "$#" -ge 2 ] || die "--install needs a device"; INSTALL_TARGET="$2"; shift 2 ;;
    --launch)             LAUNCH=1; shift ;;
    --ipa)                MAKE_IPA=1; shift ;;
    --check)              CHECK_ONLY=1; shift ;;
    --actool)             ACTOOL=1; shift ;;
    --dry-run)            DRY_RUN=1; shift ;;
    -h|--help)            usage; exit 0 ;;
    *)                    die "unknown argument: $1 (see --help)" ;;
  esac
done

[ -n "$APP" ] || { usage; die "--app is required"; }
[ "$MODE" = "simulator" ] || [ "$MODE" = "device" ] || die "--mode must be simulator or device"
if [ "$MODE" = "device" ] && [ -n "$INSTALL_TARGET" ]; then
  die "--install targets simulators only; sideload the IPA on the device manually"
fi
if [ "$LAUNCH" -eq 1 ] && [ -z "$INSTALL_TARGET" ]; then
  die "--launch requires --install"
fi
[ -z "$OUT" ] && OUT="$ROOT/sideload-out"
[ -z "$ENTITLEMENTS" ] && { [ "$RELEASE_ENT" -eq 1 ] && ENTITLEMENTS="$REL_ENT" || ENTITLEMENTS="$DEV_ENT"; }

run() {
  printf '  $'; printf ' %q' "$@"; printf '\n'
  if [ "$DRY_RUN" -eq 0 ]; then "$@"; fi
}

require_tools() {
  local tool
  for tool in codesign plutil "$PLISTBUDDY"; do
    if [ "$tool" = "$PLISTBUDDY" ]; then
      [ -x "$tool" ] || die "missing Apple tool: $tool (install Xcode command-line tools)"
    else
      command -v "$tool" >/dev/null 2>&1 || die "missing tool on PATH: $tool"
    fi
  done
  if [ "$DRY_RUN" -eq 0 ] && [ "$(uname -s)" != "Darwin" ]; then
    die "macOS + Xcode are required (iOS SDK, codesign, simctl are Apple tools)"
  fi
  if [ "$MAKE_IPA" -eq 1 ]; then
    command -v zip >/dev/null 2>&1 || die "missing tool on PATH: zip (needed for --ipa)"
  fi
  if [ -n "$INSTALL_TARGET" ] || [ "$ACTOOL" -eq 1 ]; then
    command -v xcrun >/dev/null 2>&1 || die "missing tool on PATH: xcrun"
  fi
}

plist_get() {
  "$PLISTBUDDY" -c "Print :$2" "$1" 2>/dev/null || true
}

# Read-only verification of a built bundle.  Every check corresponds to a
# real sideload failure mode documented in docs/IPADOS_PORT.md.
verify_bundle() {
  local app="$1"
  log "Verifying bundle: $app"
  [ -d "$app" ] || die "app bundle not found: $app"
  plutil -lint "$app/Info.plist" >/dev/null 2>&1 \
    || die "invalid Info.plist: $app/Info.plist"

  local bid exe arch
  bid="$(plist_get "$app/Info.plist" CFBundleIdentifier)"
  [ "$bid" = "$BUNDLE_ID" ] || die "wrong bundle ID: '$bid' (expected '$BUNDLE_ID')"
  exe="$(plist_get "$app/Info.plist" CFBundleExecutable)"
  [ -n "$exe" ] || die "Info.plist has no CFBundleExecutable"
  [ -f "$app/$exe" ] || die "executable missing: $app/$exe"
  [ -x "$app/$exe" ] || die "executable not executable: $app/$exe"
  arch="$(lipo -info "$app/$exe" 2>&1)"
  case "$arch" in
    *x86_64*) log "  executable arch: x86_64 (simulator)" ;;
    *arm64*)  log "  executable arch: arm64 (device / Apple-silicon simulator)" ;;
    *)        die "unsupported architecture: $arch" ;;
  esac

  local rel
  for rel in Info.plist "$exe" Plugins themes locales docs wiz roms.db AppIcon.png; do
    [ -e "$app/$rel" ] || die "bundle is missing '$rel'"
  done
  if ! find "$app/Plugins" -type f -name 'plugin.json' -print -quit | grep -q .; then
    die "no data-only plugin manifest under Plugins/"
  fi
  for rel in themes locales docs wiz Plugins; do
    [ -n "$(find "$app/$rel" -mindepth 1 -print -quit)" ] || die "bundled '$rel' is empty"
  done
  [ ! -d "$app/Resources" ] || die "iOS bundle contains Resources/ (simctl install fails: 'Missing bundle ID')"

  local native
  native="$(find "$app" \( -name '*.dylib' -o -name '*.framework' -o -name '*.bundle' -o -name '*.plugin' \) -print -quit)"
  [ -z "$native" ] || die "native code in bundle: $native (iPadOS forbids dlopen/JIT)"

  # Native plugin manifests are not loadable on iPadOS.  Reject them instead
  # of shipping dead weight (or a launch-time surprise).
  local manifest
  while IFS= read -r manifest; do
    if grep -q 'native_entry' "$manifest"; then
      die "native_entry in bundled plugin manifest: $manifest"
    fi
    if ! grep -Eq '"ipados"|"ios"' "$manifest"; then
      die "plugin manifest lacks ipados/ios platform tag: $manifest"
    fi
  done < <(find "$app/Plugins" -type f -name 'plugin.json' -print)

  local icon_w icon_h
  icon_w="$(sips -g pixelWidth "$app/AppIcon.png" 2>/dev/null | awk '/pixelWidth/{print $2}' || true)"
  icon_h="$(sips -g pixelHeight "$app/AppIcon.png" 2>/dev/null | awk '/pixelHeight/{print $2}' || true)"
  [ "${icon_w:-0}" -ge 1024 ] && [ "${icon_h:-0}" -ge 1024 ] \
    || die "AppIcon.png must be >=1024x1024 (got ${icon_w:-?}x${icon_h:-?})"

  "$PLISTBUDDY" -c 'Print :CFBundleIconFile' "$app/Info.plist" >/dev/null 2>&1 \
    || warn "Info.plist has no CFBundleIconFile (icon may not appear)"

  codesign --verify --deep --strict "$app" >/dev/null 2>&1 \
    || warn "current bundle signature is invalid (it will be replaced by this script)"
  ok "bundle verification passed (bundle id=$bid)"
}

# Compile a temporary asset catalog with actool to validate the icon asset.
# The shipped bundle uses a loose CFBundleIconFile PNG (which SpringBoard
# accepts); this is validation evidence, not a change to the bundle layout.
validate_icon_actool() {
  local app="$1" tmp spec px fn
  command -v xcrun >/dev/null 2>&1 || { warn "xcrun unavailable; skipping actool icon validation"; return 0; }
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/ipados-actool.XXXXXX")"
  mkdir -p "$tmp/Assets.xcassets/AppIcon.appiconset" "$tmp/out"
  # actool checks that each PNG has the exact pixel size for its idiom.
  for spec in "40:20x20:2x:icon_20_2x.png" \
              "58:29x29:2x:icon_29_2x.png" \
              "80:40x40:2x:icon_40_2x.png" \
              "152:76x76:2x:icon_76_2x.png" \
              "167:83.5x83.5:2x:icon_83_2x.png" \
              "1024:1024x1024:1x:icon_1024.png"; do
    px="${spec%%:*}"; rest="${spec#*:}"
    fn="${rest##*:}"; rest="${rest%:*}"
    sips -s format png -Z "$px" "$app/AppIcon.png" --out "$tmp/Assets.xcassets/AppIcon.appiconset/$fn" >/dev/null
  done
  cat > "$tmp/Assets.xcassets/AppIcon.appiconset/Contents.json" <<'JSON'
{
  "images": [
    { "idiom": "ipad", "size": "20x20", "scale": "2x", "filename": "icon_20_2x.png" },
    { "idiom": "ipad", "size": "29x29", "scale": "2x", "filename": "icon_29_2x.png" },
    { "idiom": "ipad", "size": "40x40", "scale": "2x", "filename": "icon_40_2x.png" },
    { "idiom": "ipad", "size": "76x76", "scale": "2x", "filename": "icon_76_2x.png" },
    { "idiom": "ipad", "size": "83.5x83.5", "scale": "2x", "filename": "icon_83_2x.png" },
    { "idiom": "ios-marketing", "size": "1024x1024", "scale": "1x", "filename": "icon_1024.png" }
  ],
  "info": { "version": 1, "author": "xcode" }
}
JSON
  log "actool: compiling temporary iPad app-icon catalog (validation only)"
  local out
  out="$(xcrun actool "$tmp/Assets.xcassets" \
    --compile "$tmp/out" \
    --app-icon AppIcon \
    --output-partial-info-plist "$tmp/partial.plist" \
    --platform iphoneos --target-device ipad \
    --minimum-deployment-target 16.0 \
    --warnings --errors --notices --output-format xml1 2>&1)" || true
  printf '%s\n' "$out"
  if grep -q 'Unable to resolve SDK' <<<"$out"; then
    warn "actool could not resolve the iOS SDK on this Mac (SDK is stripped); icon pixels themselves were accepted"
  fi
  if grep -q 'com.apple.actool.document.warnings' <<<"$out"; then
    warn "actool reported icon warnings; see output above"
  fi
  if [ -f "$tmp/partial.plist" ]; then
    log "actool partial Info.plist:"
    plutil -p "$tmp/partial.plist" | head -20
  fi
  rm -rf "$tmp"
}

require_tools

if [ "$CHECK_ONLY" -eq 1 ]; then
  verify_bundle "$APP"
  [ "$ACTOOL" -eq 0 ] || validate_icon_actool "$APP"
  log "Check-only mode: no files were written."
  exit 0
fi

# Always work on a COPY so the build artifact / source tree is untouched.
# The signed result is kept under $OUT for later inspection/sideloading.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ipados-stage.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
STAGE="$OUT/$TARGET.app"
mkdir -p "$WORK/Payload" "$OUT"
rm -rf "$STAGE"
log "Staging a copy of the bundle: $STAGE"
run cp -R "$APP" "$STAGE"
verify_bundle "$STAGE"

# Simulator bundles: drop the arm64 device capability when the binary is
# x86_64.  Device bundles must keep it (and must be arm64).
if [ "$MODE" = "simulator" ]; then
  if lipo -info "$STAGE/$TARGET" 2>&1 | grep -q x86_64; then
    if "$PLISTBUDDY" -c 'Print :UIRequiredDeviceCapabilities' "$STAGE/Info.plist" >/dev/null 2>&1; then
      log "Stripping UIRequiredDeviceCapabilities (arm64) for the x86_64 simulator bundle"
      run "$PLISTBUDDY" -c 'Delete :UIRequiredDeviceCapabilities' "$STAGE/Info.plist"
    fi
  fi
fi

if [ "$MODE" = "device" ]; then
  [ "$IDENTITY" != "-" ] || die "device mode requires --identity (ad-hoc cannot be installed on an unjailbroken iPad)"
  [ -n "$TEAM_ID" ] || die "device mode requires --team-id"
  [ -f "$ENTITLEMENTS" ] || die "entitlements file not found: $ENTITLEMENTS"
  [ -n "$PROFILE" ] || die "device mode requires --profile (provisioning profile with this bundle ID)"
  [ -f "$PROFILE" ] || die "provisioning profile not found: $PROFILE"
  lipo -info "$STAGE/$TARGET" 2>&1 | grep -q arm64 || die "device binary must contain arm64"
  cp "$PROFILE" "$STAGE/embedded.mobileprovision"
  security cms -D -i "$PROFILE" > "$WORK/profile.plist" 2>/dev/null \
    || die "cannot decode provisioning profile: $PROFILE"
  prof_bid="$(plist_get "$WORK/profile.plist" Entitlements:application-identifier)"
  prof_bid="${prof_bid#*.}"
  [ "$prof_bid" = "$BUNDLE_ID" ] \
    || die "profile application-identifier '$prof_bid' does not match bundle ID '$BUNDLE_ID'"
  prof_team="$(plist_get "$WORK/profile.plist" TeamIdentifier:0)"
  [ "$prof_team" = "$TEAM_ID" ] \
    || die "profile team '$prof_team' does not match --team-id '$TEAM_ID'"
  prof_gta="$(plist_get "$WORK/profile.plist" Entitlements:get-task-allow)"
  if [ "$RELEASE_ENT" -eq 0 ] && [ "$prof_gta" != "true" ]; then
    die "development entitlements request get-task-allow=true but the profile has '$prof_gta' (use --release-entitlements or a development profile)"
  fi
  if [ "$RELEASE_ENT" -eq 1 ] && [ "$prof_gta" = "true" ]; then
    warn "release entitlements set get-task-allow=false but the profile allows debugging; iOS uses the profile value"
  fi
  log "Signing device bundle: identity='$IDENTITY' team=$TEAM_ID entitlements=$ENTITLEMENTS"
  run codesign --force --sign "$IDENTITY" --entitlements "$ENTITLEMENTS" \
      --timestamp=none "$STAGE"
else
  log "Ad-hoc signing simulator bundle (matches the verified install path)"
  run codesign --force --sign - --timestamp=none "$STAGE"
fi

run codesign --verify --strict --verbose=2 "$STAGE"
ok "signature valid: $(codesign -dvv "$STAGE" 2>&1 | grep -E '^Signature=|^Identifier=' | tr '\n' ' ')"

if [ "$ACTOOL" -eq 1 ]; then
  validate_icon_actool "$STAGE"
fi

if [ "$MAKE_IPA" -eq 1 ]; then
  IPA="$OUT/$TARGET.ipados.ipa"
  rm -rf "$WORK/Payload/$TARGET.app"
  cp -R "$STAGE" "$WORK/Payload/"
  (cd "$WORK" && zip -qry "$IPA" Payload)
  ok "IPA ready for sideloading: $IPA"
fi

if [ -n "$INSTALL_TARGET" ]; then
  [ -x "$TIMEOUT_HELPER" ] || die "timeout helper missing: $TIMEOUT_HELPER"
  log "Installing on simulator '$INSTALL_TARGET' (timeout ${SIM_TIMEOUT}s)"
  run python3 "$TIMEOUT_HELPER" "$SIM_TIMEOUT" xcrun simctl install "$INSTALL_TARGET" "$STAGE"
  if [ "$LAUNCH" -eq 1 ]; then
    run python3 "$TIMEOUT_HELPER" "$SIM_TIMEOUT" xcrun simctl launch "$INSTALL_TARGET" "$BUNDLE_ID"
  fi
fi

echo ""
log "SUMMARY"
echo "  mode:        $MODE"
echo "  bundle id:   $BUNDLE_ID"
echo "  signed app:  $STAGE  (persisted under --out; source bundle was not modified)"
echo "  ipa:         $([ "$MAKE_IPA" -eq 1 ] && echo "$OUT/$TARGET.ipados.ipa" || echo 'not requested')"
echo "  visual verification: NOT claimed (Intel-host simulator compositor may deadlock)"
