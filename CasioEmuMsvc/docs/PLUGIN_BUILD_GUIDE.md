# Building `.plugin` packages for CasioEmuMsvc

This guide explains how to build one `.plugin` file that works on **macOS,
Linux, Windows and Android**, what goes in the manifest, how the host picks the
right native binary, and what ABI the native code must implement.

Tool: `tools/build_plugin.py` (Python 3, standard library only). It also exports
the shared helpers used by `tools/build_universal_plugin.py`, which assembles
one universal CG100 package from the four platform builds.
Worked example: `docs/examples/build_hello.sh` + `docs/examples/dummy/`.

---

## 1. What a `.plugin` file is

A `.plugin` is an ordinary **ZIP archive** (the host checks the `PK\x03\x04`
magic in `src/Plugin/PluginPackage.cpp`). Minimum layout:

```
my-plugin.plugin                 (the ZIP file itself)
├── plugin.json                  manifest, schema 1  (required)
├── assets/info.json             declarative info document (entry_point)
├── my-plugin-macos-x64.dylib    tagged native binaries (0..N)
├── my-plugin-macos-arm64.dylib
├── my-plugin-linux-x64.so
├── my-plugin-windows-x64.dll
├── my-plugin-android-arm64-android.so
└── lib/                         optional embedded dependency dylibs
    └── libcig100core.dylib
```

Rules enforced by the host when installing:

* every entry path must be a **safe relative path** (no leading `/`, no `\`, no
  `.` / `..` components);
* no duplicate **or case-insensitively colliding** entry names (macOS volumes
  are usually case-insensitive);
* no symlinks, hard links, device/FIFO/socket entries, or encrypted entries;
* entry-count / size / compression-ratio limits apply (zip-bomb protection);
* native binaries must be **regular files with executable bits (0755)** in the
  ZIP so the OS can `dlopen` them after extraction. JSON entries are 0644.
  The builder sets the Unix `create_system` flag so the modes survive.

See `src/Plugin/PluginPackage.cpp` (`ExtractPackage`) for the exact checks.

---

## 2. Quick start — one package, four platforms

```bash
python3 tools/build_plugin.py \
  --name hello --id com.example.hello --version 1.0.0 \
  --author "Example Author" --description "Tiny cross-platform example plugin." \
  --type tool --permissions ui.shell \
  --os-tags macos,linux,windows,android \
  --arch-tags x64,arm64,arm64-android \
  --binary-os macos   --binary-arch x64           --binary core.dylib \
  --binary-os macos   --binary-arch arm64         --binary core-arm64.dylib \
  --binary-os linux   --binary-arch x64           --binary core.so \
  --binary-os windows --binary-arch x64           --binary core.dll \
  --binary-os android --binary-arch arm64-android --binary core-android.so \
  --out hello.plugin
```

Run `bash docs/examples/build_hello.sh <output.plugin>` for a self-contained
demo using the dummy files in `docs/examples/dummy/` (placeholder text, **not**
real native libraries). Pass an explicit output path outside the source tree.

The tool prints the output SHA-256 and a full entry listing, then re-opens the
finished ZIP and verifies: ZIP magic, exact entry list, every entry's bytes,
every entry's Unix mode, and that both JSON documents parse with `schema == 1`.
Invalid input or a failed verification exits non-zero.

---

## 3. CLI reference

| Flag | Meaning |
|---|---|
| `--name NAME` | Display name, 1–80 characters. Also used as the binary base name. |
| `--id ID` | Reverse-DNS id (3–128 chars; lower-case letters, digits, `.`, `_`, `-`; at least one dot; no leading/trailing/double dot). Example: `com.example.hello`. |
| `--version SEMVER` | Plugin version, `MAJOR.MINOR.PATCH` (optional `-pre` / `+build`). Default `1.0.0`. |
| `--author TEXT`, `--description TEXT` | Optional metadata (description ≤ 500 chars). Copied into `plugin.json` and `assets/info.json`. |
| `--min-host SEMVER` | Minimum host version. Default `2.1.0`; current host is `2.2.0` (`kHostVersion` in `src/Plugin/PluginManifest.h`). |
| `--entry-point PATH` | Safe relative entry path. Default `assets/info.json`; `script` plugins must use a path ending in `.lua`. |
| `--os-tags a,b,...` | OS tags written to `os_tags`. See §5. |
| `--arch-tags a,b,...` | Arch tags written to `arch_tags`. See §5. |
| `--permissions a,b,...` | Permission list. See §8. Comma-separated or repeated. |
| `--type TYPE` | `theme`, `tab`, `inject`, `ui_revamp`, `tool`, `script` (default `tool`). |
| `--binary PATH` | Native binary to embed (repeatable). Archive name is auto-derived: `<name>-<os>-<arch>.<ext>`. |
| `--binary-os OS`, `--binary-arch ARCH` | OS/arch for subsequent `--binary` flags. **Sticky** until changed. If omitted, OS is derived from the extension (`.dylib`→macos, `.dll`→windows, `.so`→linux, `.plugin`→macos) and arch defaults to `any` (or the sole declared `--arch-tags` value). |
| `--asset SRC[=DEST]` | Extra data asset (repeatable). `DEST` defaults to the source basename; must be a safe relative path. |
| `--lib PATH` | Dependency dylib (repeatable); embedded as `lib/<basename>` with mode 0755. |
| `--core PATH` | Used only by `--preset cg100-sh4`. |
| `--preset cg100-sh4` | Reproduce the bundled CG100 SH4 package exactly (see §10). |
| `--out PATH` | Output `.plugin` ZIP (required). Existing file is replaced atomically. |

Validation mirrors the host: unknown permission/tag/type, bad id, bad semver,
unsafe paths, case-colliding entries, wrong extension, native code in a
`theme`/`inject`/`script` plugin, or iOS/iPadOS tags with native binaries are
all rejected before any ZIP is written.

### Per-platform binary commands

```bash
# macOS (Intel) — .dylib implies macos
--binary-os macos   --binary-arch x64           --binary build/macos-x64/core.dylib
# macOS (Apple Silicon)
--binary-os macos   --binary-arch arm64         --binary build/macos-arm64/core.dylib
# Linux x86-64 — .so implies linux
--binary-os linux   --binary-arch x64           --binary build/linux-x64/core.so
# Windows x86-64 — .dll implies windows
--binary-os windows --binary-arch x64           --binary build/win-x64/core.dll
# Android 64-bit ARM — note the hyphenated arch tag
--binary-os android --binary-arch arm64-android --binary build/android-arm64/core.so
# Universal fallback (lower priority than any concrete tag for a host)
--binary-os any     --binary-arch any           --binary build/portable/core.so
```

Native extensions recognised by the host are `.dylib`, `.so`, `.dll`, and
`.plugin` (`IsNativeLibraryExtension` in `src/Plugin/PlatformTags.h`).

---

## 4. `plugin.json` fields (schema 1)

Source of truth: `ParseManifest()` in `src/Plugin/PluginManifest.cpp`.

| Field | Required | Type | Notes |
|---|---|---|---|
| `schema` | yes | int | Must be `1`. |
| `id` | yes | string | Reverse-DNS-ish; validated as described above. |
| `name` | yes | string | 1–80 chars. |
| `version` | yes | string | semver. |
| `author`, `description`, `homepage` | no | string | description ≤ 500 chars. |
| `min_host` | no | string | semver; host rejects a value newer than its own `2.2.0`. |
| `platforms` | no | array of OS tags | Legacy field; defaults to `["macos"]`. The builder omits it for portability with strict mobile parsers. |
| `os_tags` | no | array of OS tags | **Additive field the host actually reads** to mark a platform-tagged native package (`HasPlatformTaggedBinaries`). |
| `arch_tags` | no | array of arch tags | Same, for CPU architectures. Filename tags decide selection; these mark the package as tagged. |
| `type` | yes | string | One of the six types below. |
| `permissions` | no | array of strings | No duplicates; all names must be known (§8). |
| `entry_point` | yes | string | Safe relative path; normally `assets/info.json`. `script` plugins require a `.lua` path. |
| `native_entry` | conditional | string | `tab`/`tool` need this **or** platform tags. Forbidden for `theme`/`inject`/`script`. Ignored when tagged binaries are found. |
| `assets` | no | array of strings | Safe relative paths; normally just `["assets/info.json"]`. |
| `api_version` | no | int | Host accepts `1..3` (`src/Plugin/PluginVersion.h`); builder relies on the default. |
| `script_engine`, `script_api`, `script_budget` | script only | string/int | `script_engine` must be `lua54`; `script_api` 1..1; `script_budget` 1..200000000. |

Plugin types:

* `theme` — data/appearance only, **no native code**;
* `inject` — payloads, **no native code**;
* `tab` — adds a native tab;
* `ui_revamp` — theme + layout;
* `tool` — native tool / architecture core;
* `script` — data-only Lua plugin (no native code); the only type that can
  legitimately declare `ios` / `ipados` tags.

Generated example (from the demo, with `script`-irrelevant fields shown):

```json
{
  "schema": 1,
  "id": "com.example.hello",
  "name": "hello",
  "version": "1.0.0",
  "type": "tool",
  "permissions": ["ui.shell"],
  "entry_point": "assets/info.json",
  "os_tags": ["macos", "linux", "windows", "android"],
  "arch_tags": ["x64", "arm64", "arm64-android"],
  "native_entry": "hello-macos-x64.dylib"
}
```

`assets/info.json` is a small object with `name` and `description`.

---

## 5. OS and architecture tags

Authoritative source: `src/Plugin/PlatformTags.h` (`kKnownOsTags`,
`kKnownArchTags`).

### OS tags

| Tag | Meaning |
|---|---|
| `macos` | Apple macOS (desktop / laptop) |
| `linux` | Linux |
| `windows` | Microsoft Windows |
| `android` | Android |
| `ipados` | Apple iPadOS — script plugins only (no native code) |
| `ios` | Apple iOS — script plugins only (no native code) |
| `any` | Any OS — wildcard |

### Architecture tags

| Tag | Meaning |
|---|---|
| `x64` | 64-bit x86 (AMD64 / Intel64) |
| `x32` | 32-bit x86 (IA-32) — the old draft name `x6432` is **not** valid |
| `arm64` | 64-bit ARM on laptops/desktops (Apple Silicon, Windows on ARM, Linux aarch64) |
| `arm64-android` | 64-bit ARM on Android phones / tablets |
| `any` | Any CPU architecture — wildcard |

`any` is a wildcard **on either side**. Do not ship `ios`/`ipados` tags on a
package with native binaries — those platforms run data-only script plugins.

---

## 6. Native binary naming convention

Pattern: `<name>-<os>-<arch>.<ext>`. Examples:

```
my-plugin1-macos-x64.dylib
my-plugin2-macos-arm64.dylib
my-plugin1-windows-x64.dll
my-plugin1-linux-arm64.so
my-plugin1-android-arm64-android.so
my-plugin-any-any.so
```

Notes:

* The `-<os>-<arch>` suffix must be at the **end of the filename stem**; the
  base name before it must be non-empty. See `ParseTaggedNativeFileName()` in
  `PlatformTags.h`.
* `arm64-android` contains a hyphen; the parser matches known suffixes rather
  than splitting on `-`, so do not "fix" that name.
* Several binaries per platform are allowed — use distinct base names
  (`my-plugin1`, `my-plugin2`, …). All binaries tied at the best tier load.
* Because selection is ranked (§7), an `any-*` fallback shipped next to a
  concrete binary is **ignored** on hosts with a concrete match — it is safe,
  but redundant. Do not rely on it to add a second module on such hosts.
* A binary with **no** tag suffix (e.g. the legacy `CG100SH4.plugin`) is *not*
  a tagged binary; it is only considered through the manifest's `native_entry`.

The builder derives these names automatically from `--name`, the per-binary
OS/arch tags and the source extension, and refuses to emit a name the host
cannot parse.

---

## 7. How the host chooses binaries

`SelectPlatformBinariesForRoot()` in `src/Plugin/PlatformTags.h` scans the
**entire extracted package recursively**:

1. For each regular file, parse `-<os>-<arch>` from the filename (and confirm a
   native extension).
2. Record its `<os>-<arch>` key in the "available" list.
3. Compute a rank with `PlatformMatchRank()` (higher is better; `-1` = no match):

   | Rank | Tier | Example |
   |---|---|---|
   | 4 | exact OS + exact arch | `hello-macos-x64.dylib` on `macos-x64` |
   | 3 | exact OS + arch `any` | `hello-macos-any.dylib` on `macos-x64` |
   | 2 | OS `any` + exact arch | `hello-any-x64.so` on `macos-x64` |
   | 1 | OS `any` + arch `any` | `hello-any-any.so` on any host |

4. Keep **only** binaries in the single highest-ranked tier that matched
   (`matchedRanks` / `bestRank`); several binaries tied at that tier all load.
   This prevents a wildcard fallback from being dlopened alongside a concrete
   binary (which could double-register `fPluginLoad` / `fPluginLoadArch`).
5. Result:
   * **`NoTaggedBinaries`** — no tagged files at all → fall back to
     `native_entry` (legacy single-dylib packages).
   * **`Matched`** — one or more files match → load the best tier only.
   * **`NoMatch`** — tagged files exist but none match → reject the plugin with
     `NoCompatibleBinaryError()`, e.g. **`No native binary for macos-x64`**.

The current build's OS/arch come from compiler predefines (the header discards
any injected `CASIOEMU_PLATFORM_OS` / `CASIOEMU_PLATFORM_ARCH` macros):
`__APPLE__` → `macos`, `_WIN32` → `windows`, `__ANDROID__` → `android`,
`__linux__` → `linux`; `__aarch64__` → `arm64`, `__x86_64__` → `x64`,
`__i386__` → `x32`, Android ARM64 → `arm64-android`.

---

## 8. Permissions

Manifest names and descriptions: `kPermissionNames` / `kPermissionDescriptions`
in `src/Plugin/PluginManifest.cpp`; runtime grants: `kGrants` in
`src/Plugin/PluginPermissions.cpp`; capability enum:
`src/Plugin/PluginPermissions.h`.

| Permission | Grants (capabilities) | What it lets the plugin do |
|---|---|---|
| `emulator.read` | `EmulatorRead` | Observe the model, ROM, memory, registers and debugger state. |
| `emulator.write` | `EmulatorWrite` | Write RAM, registers and memory monitors. |
| `emulator.code` | `EmulatorCode`, `EmulatorWrite` | Patch ROM/code bytes and hot-reload images. |
| `emulator.inject` | `Inject` | Register and run injection payloads. |
| `filesystem.read` | `FilesystemRead` | Read files outside the plugin package. |
| `filesystem.write` | `FilesystemWrite` | Write files outside the plugin package. |
| `network` | `Network` | Make outbound network requests. |
| `process` | `Process`, `Native` | Spawn processes, including terminal shells. |
| `ui.shell` | `Shell`, `Window`, `Theme`, `Native` | Add/modify tabs, tools, layout, themes or the shell. |
| `clipboard` | `Clipboard` | Read or write the clipboard. |
| `arch` | `Arch`, `Native` | Register an architecture provider and define custom hardware IDs / model classes. |

`arch` is **additive**: it grants only `Arch + Native` and does not absorb or
narrow `ui.shell`, `emulator.*` or any other permission. An architecture core
that also supplies its own UI buttons/tabs keeps `ui.shell` (the bundled CG100
package declares `["ui.shell", "arch"]`). Capability checks happen in the
permission facade (`src/Plugin/PluginApi_Impl.cpp`); a denial is recorded with
`plugin.permission` and shown as "limited" in the plugin manager.

Declare the **narrowest** set that works — permissions are shown to the user at
install time. Note that the universal CG100 package declares only `ui.shell`
because the `arch` permission is unknown to stricter mobile parsers.

### 8.1 Script plugin host API (`type: "script"`, `script_api: 1`)

A script plugin receives a sandboxed `host` table; every entry is checked
against the manifest permissions before it can run.  Missing host facilities
never raise an error — the call returns `nil`/`false` and logs once.

| `host` function | Required permission | Returns | Notes |
|---|---|---|---|
| `log(msg)`, `get_tick()` | none | — / integer | Rate-limited log; monotonic ms tick. |
| `get_model()` | `emulator.read` | string | Running model name. |
| `get_framebuffer_size()` | `emulator.read` | `w, h, pitch` or `nil` | Metadata only, never a pointer. |
| `fb_get_info()` | `emulator.read` | table or `nil` | `{width,height,pitch}`. |
| `reg_read(name)`, `mem_read8(addr)` | `emulator.read` | value(s) or `nil` | Debugger-backed. |
| `mem_write8(addr, value)` | `emulator.write` | — | MMU-backed. |
| `key_is_down(ki, ko)` | `emulator.read` | boolean | Live key matrix. |
| `ui_add_window(title)` | `ui.shell` | — | Phase 1 logs the request. |
| **`ui_add_button(win, label, cb)`** | `ui.shell` | button id or `false` | Keeps `cb` in the Lua registry; tapping re-enters the script with a fresh instruction budget. Returns `false` until script window UI is bound. |
| **`arch_control(cmd [, arg])`** | `emulator.inject` (or `ui.shell`) | boolean | `cmd`: `reboot`, `reset`, `power_on`, `enter_diag` (`arg` = mode; `enter_diag(1)` also accepted). `reboot`/`reset` use `IDebugger::Reset`; `enter_diag` needs the native SH4 core and returns `false` until exposed via `PluginApi`. |
| **`arch_status()`** | `emulator.read` | table or `nil` | `{running, paused, cycles, model, hw_id}` (`hardware_id` alias included). `cycles` is `0` when the host exposes no cycle counter. |

`arch_register(desc)` (design doc §6, `emulator.inject`) is a separate
architecture-provider registration entry point, not part of this table.

---

## 9. Native ABI contract (very important)

**The host owns the ABI function names; the plugin implements them.**

* Host declarations (source of truth): `src/Plugin/ArchCoreABI.h`
  ("ARCH ABI v1", 2026-11-21). The plugin's `cg100core.h` / `ArchCoreABI.h` must
  be a byte-identical mirror or a thin shim that includes the host header.
* The plugin implements each declared symbol with `extern "C"` and an
  identical signature; the host calls it after `dlopen`.
* Handshake symbols: `fPluginLoad`, `fPluginLoadArch`, `fPluginApiVersion`,
  `fPluginAbiSignature`. The current CG100 plugin exports the first two and
  takes the documented legacy/unverified path for the version/signature pair.
* Generic bridge: `casioemu_arch_core_framebuffer`, `_handle`, `_key_down`,
  `_key_up`, `_key_matrix`, `_power_on`, `_release_all`, `_reboot`,
  `_enter_diag` — implemented by `CG100SH4.plugin`.
* CG100 core: `cg100_create`, `cg100_run`, `cg100_framebuffer`, `cg100_*` USB
  helpers, etc. — implemented by `libcig100core.dylib` and resolved as imports
  of the plugin.
* **Additive changes only.** Never change an existing signature, parameter
  width, return type or symbol spelling. Adding a symbol requires a new version
  comment in `ArchCoreABI.h`, an updated plugin mirror, and a re-check with
  `nm -gU` (defined exports) and `nm -gm` (imports) before repackaging.
* Host API version is `PLUGIN_API_VERSION` (`src/Plugin/PluginVersion.h`,
  currently 3); the UI-window revision is
  `kPluginUiWindowAbiRevision` / `HostUiWindowAbiSignature()`.

### macOS dynamic-library details

* Native `.dylib` files need executable bits (the builder sets 0755).
* The host adds `@loader_path` and `@loader_path/lib` to the dylib search
  rpath, so **embedded dependencies belong in `lib/` inside the ZIP** — e.g.
  `lib/libcig100core.dylib`. Use `--lib /path/to/libcig100core.dylib`.
* Keep the dylib's install name relative (`@rpath/...` or `@loader_path/...`);
  an absolute path would make the host look outside the extracted package.
* Windows DLL dependencies are an exception: they must be at the ZIP **root**
  (see `tools/build_universal_plugin.py`) because the Windows loader searches
  the loaded DLL's directory, not `root/lib`.

---

## 10. Preset: reproduce the bundled CG100 SH4 package

```bash
python3 tools/build_plugin.py --preset cg100-sh4 \
  --core /path/to/CG100SH4.plugin \
  --lib  /path/to/libcig100core.dylib \
  --out CG100SH4.plugin
```

The preset fixes the manifest to the known-good values (id
`com.casioemu.arch.cg100sh4`, permissions `["ui.shell", "arch"]`,
`native_entry: "CG100SH4.plugin"`, `platforms: ["macos"]`), writes the same
`assets/info.json`, and embeds the dependency as `lib/libcig100core.dylib`.
`--core` / `--lib` default to the app bundle's plugins directory relative to
this checkout. Rebuilding from the known-good embedded binaries reproduces the
bundled ZIP byte-for-byte (SHA-256
`7479bf1841f3622d46e591e15a453cdb20944c72dbeda9670cf4d38ca90c9356`).

The original CG100-specific helper, `tools/make_bundled_plugin.py`, is left
untouched; `build_plugin.py` is a superset and shares its helpers with
`tools/build_universal_plugin.py`.

---

## 11. Install and enable

1. In the emulator, open **Settings → Plugins** (also reachable via
   *File → Plugins* / the command palette; `src/Gui/Theme.cpp`,
   `RenderPluginList`).
2. Click **Install .plugin...** and pick the `.plugin` ZIP.
   (`src/Gui/PluginLogWindow.cpp` shows the same button in the legacy log
   window.)
3. The host extracts and validates it into its plugin cache, shows the
   requested permissions, and lists the entry.
4. Enable the plugin (and relaunch if asked). Optional model packages can
   declare a dependency with `extra.requires_plugins` in `config.json`
   (e.g. `"requires_plugins": "com.casioemu.arch.cg100sh4"`).

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `not a readable zip archive` / bad magic | The file is a raw dylib, not a ZIP. Use this builder; native binaries go **inside** the ZIP. |
| `No native binary for macos-x64` | No tagged binary matches this build. Ship a matching `<name>-<os>-<arch>.<ext>` (or an `any-any` fallback), or fix a typo such as `x6432` → `x32`. |
| `only the 'macos' platform is supported` | An older/stricter host parser. Use the updated host, or omit `platforms` (the builder does) and declare `os_tags`/`arch_tags`. |
| Plugin loads but `dlopen` fails for a dependency | Dependency dylib is not in `lib/` or uses an absolute install name. Put it in `lib/` with `--lib` and use `@rpath`/`@loader_path`. On Windows put DLLs at the ZIP root. |
| `permission denied: ... requires the '<capability>' capability` | Add the permission that grants it (§8), e.g. `arch` for architecture registration, `ui.shell` for tabs/UI. |
| `unsafe zip entry` / `case-colliding zip entry` | Rename the entry: no absolute paths, `..`, backslashes, duplicates or case-only differences. |
| `type 'tool' requires native_entry or platform-tagged native binaries` | Pass `--binary`, or declare `--os-tags` / `--arch-tags` for a tagged package. |
| `theme plugins cannot include native code` | Match the type to the payload: native code needs `tab`/`tool`; `theme`/`inject`/`script` must have none. |
| `unsupported schema version` | Manifest must have `"schema": 1`. |
| `plugin requires host X or newer` | Lower `min_host` or update the host. |
| Both a concrete binary and a fallback load | Not possible with the current ranked selector: only the best tier loads. Remove redundant `any-*` binaries if you relied on them. |
| Dylib fails to load after install | Check ZIP mode bits are 0755 (`unzip -l` / `python3 -m zipfile -l`); the builder adds them automatically. |

### Inspect a built package

```bash
python3 -m zipfile -l hello.plugin
python3 - <<'PY'
import zipfile, json
with zipfile.ZipFile('hello.plugin') as z:
    m = json.loads(z.read('plugin.json'))
    print(m['id'], m['type'], m['permissions'], m.get('os_tags'), m.get('arch_tags'))
    for i in z.infolist():
        print(i.filename, oct((i.external_attr >> 16) & 0o777))
PY
```
