# iPadOS port — CasioEmuMsvc

> **SH4 integration is now data-driven.** See `ARCH_DESCRIPTOR_FORMAT.md` in
> this directory. On iPadOS a static architecture provider is registered once
> through the generic `RegisterStaticArchitectureProvider()` hook (there is no
> `dlopen`); the config `arch_provider` key selects it. Adding e.g. an fx-CG50
> board is a model-pack-only change, with no host source edit.

This branch (`port/ipados`) adds a real iPadOS application target to the
CasioEmuMsvc macOS tree. It is deliberately conservative: iPadOS forbids JIT,
unsigned executable memory and `dlopen()` of arbitrary code, so the port ships
the **portable CPU interpreter**, an **interpreter-only core**, and
**data-only bundled plugins**. The CG100/SH4 Go plugin and native `.dylib`
plugins cannot run on iPadOS (see below).

## 1. Prerequisites

| Requirement | Notes |
| --- | --- |
| macOS with Xcode 15+ | Deployment target iPadOS 16.0+. Built and verified with Xcode 26.3 / iOS 26.3 simulator runtime. |
| iOS Simulator SDK installed | `xcodebuild -showsdks` must list `iOS Simulator`. On the build machine used here the SDK directory existed but was **stripped of `SDKSettings.plist` and framework binaries**, so the Xcode generator could not locate it. See §6. |
| CMake 3.20+ | CMake 4.4.2 used. |
| SDL2 + SDL_image sources in `SDL/` and `SDL_image/` | Required by the existing top-level CMake; both are gitignored third-party source trees in this repository. |
| Apple developer team | Only needed for **device** builds/signing. Simulator builds are ad-hoc signed. |
| Go toolchain | Only needed to build the external CG100/SH4 plugin on a desktop host; not used by the iPadOS app. |

## 2. Build steps

### 2.1 Simulator build (verified path on this Intel Mac)

```bash
cd <worktree>
cmake -S . -B build-ipados -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DCMAKE_OSX_SYSROOT=/path/to/iPhoneSimulator.sdk \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCASIOEMU_ENABLE_SENTRY=OFF \
  -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DBUILD_EXECUTABLE=ON \
  -DCURL_USE_OPENSSL=OFF -DCURL_ENABLE_SSL=OFF \
  -DCURL_USE_LIBIDN2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_NGHTTP2=OFF
cmake --build build-ipados --target CasioEmuMsvc
```

The same configure/build/install sequence is wrapped in the canonical driver
`./build-ipados.sh` at the repository root (a thin compatibility wrapper remains
at `ipados/build-ipados.sh [simulator-UDID]`):

```bash
./build-ipados.sh --check                       # verify an existing .app (read-only)
./build-ipados.sh --dry-run                     # print the exact CMake/codesign commands
IPHONE_SIMULATOR_SDK=<sdk> ./build-ipados.sh     # simulator build
./build-ipados.sh --install booted --launch --screenshot shot.png
./build-ipados.sh --device --team-id TEAM --identity "Apple Development: ..." --ipa
```

The driver verifies that all resources / data-only plugins landed in the bundle
(themes, locales, `wiz/`, `docs/`, `roms.db`, `Plugins/*/plugin.json`), rejects a
`Resources/` directory, copies `ipados/AppIcon.png` into the bundle, ad-hoc signs
simulator builds, and prints exact install / sideload next steps. Device builds
require a full Xcode install, a real `iphoneos` SDK, `--team-id` and
`--identity`; they are signed with `ipados/CasioEmuMsvc.entitlements`, and
`--ipa` packages the signed app for AltStore / Sideloadly / Xcode.

`CURL_*` flags are needed because the bundled curl FetchContent build otherwise
picks up the **macOS Homebrew** OpenSSL/libidn2 and fails to link for iOS.
TLS-backed online features are therefore unavailable in this verification
build; a real release should supply an iOS OpenSSL build instead.

Output: `build-ipados/CasioEmuMsvc/CasioEmuMsvc.app` (Mach-O x86_64 simulator
app, ad-hoc signed with `codesign --force --deep --sign - <app>`), then:

```bash
xcrun simctl boot <iPad-device-UDID>
xcrun simctl install booted <app>
xcrun simctl launch booted com.telecomadm1145.casioemumsvc.ipados
xcrun simctl io booted screenshot screenshot.png
```

The iPad bundle puts every resource (**themes, wiz data, docs, locales,
roms.db, bundled plugins**) at the **bundle root**, not in a `Resources/`
subdirectory. `simctl install` rejects a `.app` containing a `Resources/`
directory on an iOS bundle with “Missing bundle ID”.

### 2.2 Xcode generator / device build (steps for a Mac with a full Xcode install)

```bash
cmake -S . -B build-ipados-xcode -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
  -DCASIOEMU_ENABLE_SENTRY=OFF -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DBUILD_EXECUTABLE=ON
cmake --build build-ipados-xcode --target CasioEmuMsvc \
  -- -configuration Release \
     CODE_SIGN_STYLE=Manual \
     DEVELOPMENT_TEAM=<TEAMID> \
     CODE_SIGN_IDENTITY="Apple Development"
```

`ipados/CasioEmuMsvc.entitlements` is attached via
`XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS`. The canonical app bundle is a
MACOSX_BUNDLE executable with identifier
`com.telecomadm1145.casioemumsvc.ipados`, `TARGETED_DEVICE_FAMILY=2`
(iPad only) and `UIRequiredDeviceCapabilities=arm64`.

## 3. What was ported

| Area | Implementation |
| --- | --- |
| CMake target | `CASIOEMU_IPADOS` (derived from `IOS`); Sentry, crash reporter, AsmJit and the x86 `JitU8` translator are excluded; desktop-only test executables, `casioctl` and discord-rpc are disabled/stubbed. |
| UIKit bridge | `ipados/src/IPadOSBridge.mm` — finds the key `UIWindowScene`, applies a 640×480 minimum size for Slide Over / Split View / Stage Manager, reads safe-area insets each frame, attaches `UIPointerInteraction` (trackpad pointer) and registers `UIKeyCommand` shortcuts for hardware keyboards. |
| File import | `SystemDialogs::*` are implemented with `UIDocumentPickerViewController`; picked files are copied into `<sandbox>/Documents/Imports` with security-scoped access and timestamped names. Info.plist declares `.package`, `.plugin`, `public.data` and ZIP document types. |
| On-screen keypad | `ipados/src/IPadOSKeypad.cpp` — ImGui overlay with 30 large touch buttons, safe-area-aware placement, and SDL key-event injection so existing data-driven key bindings are reused. Buttons for in-app **Settings** and hiding the keypad. |
| Settings | The existing ImGui Settings window is always available and is opened from the keypad/toolbar, replacing the macOS native menu. |
| Renderer | SDL2's iOS backend with `SDL_CreateRenderer(...ACCELERATED)` → Metal. ImGui uses the existing `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` backends. |
| Plugins | `ipados/src/IPadOSPlugins.cpp` scans the bundle for `plugin.json` packages, accepts only the `ipados`/`ios` platform tags, rejects any `native_entry`, and registers data-only themes/layouts/injections. `PluginRegistry::LoadNative()` returns `Unavailable` on iPadOS; the early architecture pass is a no-op. |
| OS tag | `ipados/src/PlatformTagsIPadOS.h` is a **local mirror** of the proposed `ipados` tag. The canonical `PlatformTags.h` in the main tree is owned by another agent and was **not edited**. When that header lands, replace the include and delete the mirror. |
| Info.plist / entitlements | `ipados/Info.plist`, `ipados/CasioEmuMsvc.entitlements` — app sandbox, user-selected file access, network client; JIT and unsigned-executable-memory entitlements are explicitly **false**. |

## 4. Plugin model on iPadOS

Native plugins are impossible on iPadOS: `dlopen()` of a dylib that is not part
of the signed app bundle is rejected, and JIT/W^X prevents runtime code
generation. iPadOS therefore supports:

1. **Bundled declarative plugins** — a directory in the app bundle containing
   `plugin.json` plus JSON assets. Example:
   `ipados/bundled_plugins/theme-vscode-dark/`. These are compiled into the app
   at build time and discovered at startup.
2. **Data-only packages imported via UIDocumentPicker** — copied into the app
   sandbox. Only packages with **no `native_entry`** can be used.
3. **Static architecture providers** — any future CG100/SH4 core must be
   compiled into the app as C/C++ (or pure-Go compiled to a static archive
   *without cgo*); the dynamic `.plugin` path is unavailable.

## 5. CG100 / SH4 core assessment

The CG100 model in this repository is driven by an **external Go-built
`CG100SH4.plugin`** (see `Cg100CoreInfo.hpp` and the `HW_CG100` paths in
`Emulator.cpp`). No Go sources ship in this tree. On iPadOS:

* `GOOS=ios GOARCH=arm64` requires cgo to be disabled (`CGO_ENABLED=0`); the
  Go runtime can then produce a pure-Go static library, but Apple's toolchain
  requires it to be linked into the signed app bundle. A `go build -buildmode=c-shared`
  dylib is not loadable on iPadOS.
* The existing plugin uses C ABI exports (`cg100_cycles`, `casioemu_arch_core_handle`,
  etc.) resolved with `dlsym(RTLD_DEFAULT, ...)`, which cannot work when the
  code is statically linked with hidden visibility.
* **Decision:** keep the working nX-U8/U16 **interpreter** path for iPadOS;
  expose CG100 only in a future statically linked, symbol-exported build. The
  CG100 UI helpers degrade honestly (they report “unavailable” rather than
  inventing register values) when no live core is attached.

## 6. Build environment findings (this machine)

* Xcode 26.3 is installed, iPadOS simulator runtimes are present, but
  `xcodebuild -showsdks` reports **“No SDKs”** for iOS: the on-disk
  `iPhoneOS.sdk`/`iPhoneSimulator.sdk` directories were stripped of
  `SDKSettings.plist` and framework binaries.
* Consequence: `cmake -G Xcode -DCMAKE_SYSTEM_NAME=iOS ...` fails at compiler
  detection with `SDK lookup failed for canonical name: iphonesimulator`.
  Adding a synthesized `SDKSettings.plist` in a temporary SDK mirror lets
  **Ninja + explicit clang** compile and link the app, but Xcode's own SDK
  registry still refuses the SDK. The verified build therefore uses Ninja.
* On a Mac with a complete Xcode installation the Xcode generator and scheme
  generation are expected to work with the documented flags in §2.2.
* `simctl install` initially failed with “Missing bundle ID” whenever the app
  bundle contained a `Resources/` directory; moving all resources to the
  bundle root fixed it.

## 7. Verification evidence

| Check | Result |
| --- | --- |
| CMake configure (Ninja, synthetic iOS SDK, x86_64 simulator) | PASS |
| Full compile + link of `CasioEmuMsvc` iPadOS target | PASS — 22 MB Mach-O x86_64 simulator executable |
| App bundle contents (Info.plist, locales, themes, wiz docs, bundled plugin) | PASS |
| `codesign --force --deep --sign -` | PASS |
| App icon bundled (`AppIcon.png` + `CFBundleIconFile`) | Fixed in this worktree — CMake copies the icon and `ipados/Info.plist` declares it; the earlier verification bundle had the icon added manually only for testing. |
| `xcrun simctl install <iPad Pro 13-in> <app>` | PASS |
| `xcrun simctl launch` / screenshot | See final report `PORT_IPADOS_FINAL.md` (launch verification in progress at time of writing). |

## 8. Known limitations

* Interpreter-only: no JIT, no CG100/SH4 plugin on iPadOS.
* curl is built without TLS in the verification build; online model downloads
  are unavailable until an iOS OpenSSL build is supplied.
* No Xcode project/scheme was generated on this machine because Xcode cannot
  locate the stripped iOS SDK; CMake generator files work on a complete Xcode.
* The on-screen keypad is a fixed 30-key layout; it does not implement every
  model-specific key binding, but synthesizes SDL keys so the existing
  data-driven bindings and hardware keyboard remain authoritative.
* File import copies documents into the sandbox; in-place editing is disabled
  (`LSSupportsOpeningDocumentsInPlace=NO`).
* A 640×480 minimum scene size may prevent Slide Over on the smallest iPad
  widths; adjust `scene.sizeRestrictions.minimumSize` in `IPadOSBridge.mm` if
  smaller split panes are required.
