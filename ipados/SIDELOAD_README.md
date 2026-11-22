# iPadOS sideload build — reproducible instructions

Bundle id: `com.telecomadm1145.casioemumsvc.ipados`
Target: iPad mini (7th generation) and later, iPadOS 16.0+
Delivery: **sideloading** (ad-hoc simulator install, or development-signed IPA for a real device).

All commands below run from the repository root on a **Mac with Xcode**. The
Linux host that produced this tree has no Xcode / iOS SDK / UIKit / signing
tools and cannot perform any of these steps — see “Verification boundary” at
the end.

## 1. One-command build

```bash
# Simulator (default); pass an optional simulator UDID to install + launch
ipados/build-ipados-sideload.sh simulator
ipados/build-ipados-sideload.sh simulator A1B2C3D4-...      # install + launch

# Real device (arm64). Requires an Apple Developer Team ID.
TEAM_ID=ABCDE12345 DEV_IDENTITY="Apple Development: Your Name (TEAMID)" \
  ipados/build-ipados-sideload.sh device <device-UDID>
```

The script is deterministic in structure: configure → build → bundle audit →
sign → package. Outputs default to `build-ipados-<mode>/` and `ipados-build/`
(override with `BUILD_DIR` / `OUTPUT_DIR`). It writes a SHA256 manifest of the
whole bundle to `ipados-build/bundle-CasioEmuMsvc.app.sha256`.

If the local iOS SDK is stripped (as on the Intel Mac used for the original
port), point `SDK`/`IPHONE_SIMULATOR_SDK` at a usable SDK, or follow the
synthetic-SDK workaround in `docs/IPADOS_PORT.md` §6. The script checks the SDK
path and fails loudly rather than producing a misleading artifact.

## 2. What the script actually verifies

| Check | Command behind it | Meaning |
| --- | --- | --- |
| Bundle identifier | `PlistBuddy :CFBundleIdentifier` | equals the fixed bundle id |
| Plist syntax | `plutil -lint` | well-formed |
| iOS bundle layout | `test ! -d Contents`, `test ! -d Resources` | avoids the “Missing bundle ID” `simctl` failure |
| Bundled data | `Plugins/`, `models/`, `script_data/`, `locales/`, `PrivacyInfo.xcprivacy` | app is self-contained |
| No native code | `find … -name '*.dylib' -o -name '*.so'` | iPadOS forbids `dlopen`/JIT |
| Signature | `codesign …`, `codesign --verify --deep --strict` | cryptographic integrity only |

A **running** process after `simctl launch` is not proof that pixels are
presented. Screenshots are opt-in (`WITH_SCREENSHOT=1`) because the Intel-host
SpringBoard `RenderBox`/Metal pipeline can deadlock; the script fails visibly
instead of fabricating an image.

## 3. Bundle layout

```
CasioEmuMsvc.app/
  Info.plist, CasioEmuMsvc, PrivacyInfo.xcprivacy
  Plugins/theme-vscode-dark/{plugin.json,assets/theme.json}   # data-only
  models/cg50/…  models/giii/…  models/cg100/README.md        # data-only
  script_data/model.lua                                       # reference data
  locales/*.lc, themes/, roms.db
  wiz/…                                                       # script toolchain data
```

`ipados/src/IPadOSBridge.mm::SetBundleResourcePaths()` exports
`CASIOEMU_MODELS_DIR=<app>/models` and
`CASIOEMU_IPADOS_PLUGINS_DIR=<app>/Plugins` at startup, so resource discovery
does not depend on the process working directory (Files / Stage Manager
launches). `ipados/src/IPadOSPlugins.cpp` honours the plugin override and
accepts only data-only packages with an `ipados`/`ios` platform tag.

## 4. Code signing / entitlements

- `ipados/CasioEmuMsvc.entitlements` is the checked-in statement of intent:
  sandbox/file-access/network keys plus explicit `allow-jit=false` and
  `allow-unsigned-executable-memory=false`. The `com.apple.security.*` keys are
  macOS hardened-runtime keys and are inert on iOS; they are harmless but are
  **not** a device signing configuration.
- Device mode generates a minimal iOS entitlements plist at build time in
  `ipados-build/CasioEmuMsvc.device.entitlements` with `application-identifier`,
  `com.apple.developer.team-identifier` and `keychain-access-groups` built from
  `TEAM_ID` and the bundle id. It is passed to `codesign --entitlements`.
- Development signing uses `DEV_IDENTITY` (default `Apple Development`) and an
  optional `PROVISIONING_PROFILE_UUID`. Free accounts sign for 7 days only;
  re-install before expiry. App Store / notarisation is out of scope.
- No JIT entitlement is requested anywhere: the build excludes AsmJit and the
  `JitU8` translator and uses the portable interpreter only.

## 5. Installing the IPA on a device

Pick one:

1. Xcode → Window → Devices and Simulators → install the `.ipa`.
2. `xcrun devicectl device install app --device <UDID> <ipa>` (script tries this
   automatically when `devicectl` and a UDID are available).
3. AltStore / Sideloadly / a personal MDM for a sideload without Xcode.

Trust the development certificate on the device (Settings → General → VPN &
Device Management) before first launch. iCloud/Data sync of the sandbox is not
configured; imports land in `<container>/Documents/Imports`.

## 6. Data provenance and limitations of the bundled data

See `ipados/model_packs/README.md` and `ipados/script_data/README.md`.
Short version:

- CG50 pack: official OS 3.81 resources decoded by PKGCG50; 396×224 descriptor.
- GIII pack: official OS 3.71 resources decoded by PKG9860; 128×64 descriptor;
  no bootcode, no boot demo.
- CG100 pack: **not present on this host**; add the real folder on the Mac.
  Native CG100 plugin binaries are deliberately not bundled.
- No Lua runtime plugin exists in this schema; `model.lua` is host-side
  reference data only.

Bundling a pack only makes it appear in Welcome → All models. No SH4 provider
is statically registered, so those models cannot emulate yet.

## 7. Verification boundary (read before reporting results)

**Can be checked on Linux (done in this task):**
- Source audits, CMake/plist/plist-syntax structure, SHA256 of staged data.
- `g++ -std=c++20 -fsyntax-only` on the modified C++ translation units.
- `bash -n` on the sideload script and its clean refusal to run on Linux.
- Host-side `arch_descriptor_smoke` / plugin checks inherited from SH4IOS.

**Cannot be checked on Linux:**
- iOS/iPadOS compilation (no SDK), UIKit/Metal rendering, simulator or device
  install/run, `codesign`, IPA install, and any screenshot.
- The Intel-host SpringBoard `RenderBox`/Metal deadlock is an Apple simulator
  host issue; no source edit here can prove or fix it. The honest status of
  visual verification remains **FAIL/inconclusive** (`IPADOS_SIM_VERIFY.md`
  fact supplied by the Mac session; no screenshot artefact exists on the Pi).
