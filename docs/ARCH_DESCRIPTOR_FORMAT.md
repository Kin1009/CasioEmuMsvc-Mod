# Architecture board descriptor format (iOS / iPadOS ports)

SH4 integration in these ports is **data-driven**. There is no CG100-only code
path: a board model is selected, dispatched, clocked and rendered from a
descriptor in its own `config.bin`. Adding a future board (for example an
**fx-CG50** SH4 board) is a *model-pack-only* change — no host source edit, no
recompile of the decorator/registry, and no new hardware id.

## 1. What makes a model a "board" model

In `ModelInfo::Read()` (v53 config format), a model is a board model when its
`board_path` field is non-empty, **or** when its `extra` map contains an
`arch_provider` key. Any board model is remapped to the reserved hardware id
`HW_ARCH` (14) so no built-in nX/EPS peripheral path ever touches it. The
hardware id is *not* used to choose the architecture provider.

Legacy CG100 packs (authored with `hardware_id == 10`) are covered by the same
rule: non-empty `board_path` -> `HW_ARCH`. Their previous special-case remap is
gone; the generic rule reproduces it exactly.

## 2. Descriptor keys (`ModelInfo::extra`, all optional)

Parsed by `Plugin/ArchDescriptor.h` (`casioemu::arch::FromModel`). Numeric
values accept decimal or `0x`-prefixed hex; malformed values fall back to the
default and never throw.

| Key | Meaning | Default (legacy SH4) |
| --- | --- | --- |
| `arch_provider` | `IArchitectureProvider::GetId()` to dispatch to | `cg100sh4` for legacy packs |
| `arch_architecture` | free-form architecture tag | `sh4` |
| `arch_fb_width` | core framebuffer width (px) | `396` |
| `arch_fb_height` | core framebuffer height (px) | `224` |
| `arch_cycles_per_second` | host cycle budget | `200000000` (200 MHz) |
| `arch_cabi_prefix` | prefix of optional C ABI counters | `cg100_` |
| `arch_power_key_row` | ON/contact key-matrix row | `6` |
| `arch_power_key_col` | ON/contact key-matrix column | `9` |

Defaults are chosen so the shipped CG100 pack (which only carries `board_path`)
behaves identically to before this refactor.

## 3. Provider dispatch

1. `casioemu.cpp` reads `config.bin` **before** Emulator construction and builds
   the descriptor.
2. If it is a board model, the host calls
   `FindArchitectureProviderById(desc.provider_id)` (a scan of the existing v1
   registry via `GetProvider()`/`GetId()`; no vtable change).
3. If no provider with that id is registered, the host falls back to the legacy
   `FindProvider(hardware_id)` lookup, so unmodified CG100 plugins — which only
   advertise supported hardware ids — still load.
4. The descriptor is stored on the `Emulator` and passed to the core through
   `ArchInitInfo` (the new fields are appended after the v1 struct tail, keeping
   ABI compatibility with older plugins).

For **statically linked** cores (the only option on iPadOS, where `dlopen` is
forbidden), a provider registers itself once at startup with the single generic
hook:

```cpp
casioemu::RegisterStaticArchitectureProvider(new MySh4Provider());
```

There is no board name or hardware id in that hook.

## 4. Adding an fx-CG50 board WITHOUT code changes

1. Author a model pack with `board_path` set and `extra` entries, e.g.:

   ```
   arch_provider           = cg50sh4
   arch_architecture       = sh4
   arch_fb_width           = 384
   arch_fb_height          = 192
   arch_cycles_per_second  = 200000000
   arch_cabi_prefix        = cg50_
   arch_power_key_row      = 7
   arch_power_key_col      = 0
   ```

2. Ship a provider whose `GetId()` is `cg50sh4` (either as a `.plugin` on iOS or
   via `RegisterStaticArchitectureProvider` on iPadOS). The existing SH4
   provider can be reused by setting `arch_provider = cg100sh4` if the core is
   compatible.

3. Optionally set `CASIOEMU_DEFAULT_MODEL=/path/to/models/fx-CG50` (or pass
   `model=...`) to launch that board directly. The bundled CG100 path remains
   the fallback default.

No host source, CMake, GUI or registry code changes are needed. All consumers
(`Emulator`, `Gui/*`, `CtlCommands`) gate on "has architecture core" (or on the
descriptor), not on a CG100 constant.

## 5. Host-side test

`tests/arch_descriptor_smoke.cpp` is a standalone Linux test covering the legacy
CG100 fallback, a hypothetical CG50 board and malformed values. Build:

```
g++ -std=c++20 -I<src> tests/arch_descriptor_smoke.cpp -o /tmp/arch_smoke && /tmp/arch_smoke
```

It requires no SDL, no iOS SDK and no emulator link.
