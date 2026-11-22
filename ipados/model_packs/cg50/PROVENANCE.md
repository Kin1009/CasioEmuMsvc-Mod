# fx-CG50 model package (PKGCG50)

A **data-only** model pack for the data-driven SH4 architecture host implemented
by SH4IOS in `/mnt/code/ports/ios` and `/mnt/code/ports/ipados`. No host source,
CMake, GUI or registry changes are required to use it: the board is selected
and dispatched entirely from `models/fx-CG50/config.json`.

> **Status after DECRYPT's decode:** the genuine CG50 installer blobs have been
> decoded to real OS 3.81 and boot images (verified markers and hashes). The
> decoded OS is now `models/fx-CG50/rom.bin`. **Decoding is not booting**: no
> CG50 OS boot has been attempted or demonstrated, and no `cg50sh4` core is
> shipped.

## 1. Hardware facts (with provenance)

| Fact | Value | Source |
| --- | --- | --- |
| CPU | SH7305 / SuperH SH-4A, big-endian, no FPU | `SH3RESEARCH.md` §1.1 (WikiPrizm Technical Documentation; gint `hardware.h`) |
| Panel / core framebuffer | **396 × 224**, 16-bit RGB565 (OS usable area 384 × 216) | `SH3RESEARCH.md` §3.1; [WikiPrizm Display](https://prizm.cemetech.net/Technical_Documentation/Display/) |
| CPU clock (host cycle budget) | **118,000,000** (FRQCR `0x0F011112` ⇒ Iφ = PLL/2 ≈ 117.96 MHz) | `SH3RESEARCH.md` §1.1; [CG50 Internals — Timers and clock](https://hexbinoct.github.io/casio-cg50/peripherals/timers/) |
| Keyboard | 8 rows × 12 columns KEYSC; 50 physical keys; **AC/ON = (row 0, col 0)** | `SH3RESEARCH.md` §4; [hexbinoct `re/KEYMAP.md`](https://github.com/hexbinoct/casio-cg50/blob/main/re/KEYMAP.md) |
| Official OS | fx-CG50 / Graph 90+E **v3.81**, Windows and Mac installers | `SH3RESEARCH.md` §7 (official Casio DAM URLs + SHA-256) |

The "58 MHz" figure in older Prizm references is the stock CG10/20 clock; CG50
runs higher (≈118 MHz in the FRQCR decode). We use 118 MHz as the descriptor's
host cycle budget because that is the CG50's actual Iφ clock, not the legacy
CG100 200 MHz budget.

## 2. Descriptor (`config.json` → `extra`)

All `extra` values are **strings**, as required by `ModelInfo::extra`
(`std::map<std::string, std::string>`).

| Key | Value | Why |
| --- | --- | --- |
| `arch_provider` | `cg50sh4` | Unique id for a CG50 core; no CG100 fallback. |
| `arch_architecture` | `sh4` | SH-4A family (descriptor tag is SH4). |
| `arch_fb_width` / `arch_fb_height` | `396` / `224` | Full RGB565 panel, verified above. |
| `arch_cycles_per_second` | `118000000` | CG50 CPU clock, verified above. |
| `arch_cabi_prefix` | `cg50_` | Distinguishes C ABI counters from CG100's `cg100_`. |
| `arch_power_key_row` / `arch_power_key_col` | `0` / `0` | AC/ON matrix position, verified above (NOT the CG100 6/9 default). |

`hardware_id` is `14` (`HW_ARCH`); `ModelInfo::Read()` remaps any model with a
non-empty `board_path` to this reserved id, and the provider is chosen from
`arch_provider`, never from the id.

`rom_path = "rom.bin"` now points to the **decoded OS image**. `flash_path`
stays `""` deliberately: `ArchInitInfo::flash_path` is documented as a
**32 MiB flash image**, and we have a decoded OS prefix plus a boot image, not
a verified full flash dump — assembling one would fabricate layout. The boot
image is available separately at `decoded/boot.bin`.

`board_path`, `rom_path`, `interface_path` and `flash_path` use exactly the
file names whitelisted by the host's `StartupUi/OnlineModelPackage.cpp`, so
`fx-CG50.model.zip` contains only `board.svg`, `config.json`, `face.png`,
`rom.bin` at the archive root.

## 3. Layout

```
pkg-cg50/
├── PKGCG50_REPORT.md      deliverable / final report
├── README.md              this file
├── manifest.json          provenance + SHA-256/size of all 17 managed files
├── keymap.json            50-key verified matrix map (source of truth for the SVG)
├── build_assets.py        reproducible generator for the data assets
├── decode_images.py       independent raw-DEFLATE decoder + marker/hash checks
├── make_manifest.py       computes all hashes from on-disk bytes
├── make_online_zip.py     builds the host-compatible model ZIP
├── verify_package.py      verification script (see §6)
├── fx-CG50.model.zip      host-compatible archive (data + decoded OS image)
├── decoded/
│   ├── os.bin             decoded OS 3.81 image (11,927,544 B)
│   ├── os_alt_3071.bin    decoded alternate model variant
│   └── boot.bin           decoded boot image (67,452 B)
├── os_payload/            original compressed installer resources (provenance)
└── models/fx-CG50/
    ├── config.json        board model + architecture descriptor
    ├── board.svg          generated reference board (screenSlot + 50 keys)
    ├── face.png           generated reference interface art (not Casio artwork)
    └── rom.bin            decoded OS image (byte-identical to decoded/os.bin)
```

`build_assets.py` regenerates only the data files; `decode_images.py` derives
the decoded images from the genuine OSEXTRACT blobs; `make_manifest.py` is the
only writer of `manifest.json`.

### Keymap encoding

`board.svg` button elements carry `data-ki="1<<col"` and `data-ko="1<<row"`.
`SvgBoardModelLoader` yields `kiko=(ko<<4)|ki`; the host splits it back to
`row=kiko>>4`, `col=kiko&0xF`. AC/ON is the power sentinel
`data-ki=0, data-ko=0` → `kiko=0xFF`, mapped by `CtlCommands::ArchKeyFromCode()`
to the descriptor's (0,0). The OS diagnostic position (3,11) is recorded but
excluded from the keypad.

## 4. OS/boot image provenance

All bytes derive from OSEXTRACT's genuine `OS_IMAGES/` artifacts (hashes
verified against `MANIFEST.sha256`). Decode method: RFC1951 **raw DEFLATE**
(`zlib.decompressobj(-15)`) from byte 0, with an opaque 8-byte trailer — the
same format documented for CG100 by `/mnt/code/prior_art/cg100-rom-extract`.
Cross-checked against `/mnt/code/decrypt/tools/decode_os.py`.

| Package file | Size | SHA-256 | Notes |
| --- | ---: | --- | --- |
| `decoded/os.bin` (= `rom.bin`) | 11,927,544 | `33fbc7d2d56aaa978ff035d85600d44101110581d04529ee77b2c34e00d2caf5` | `CASIOWIN` @0xE98, `03.81.0000` @0x20018 |
| `decoded/os_alt_3071.bin` | 11,927,544 | `9c089b10225425d753b929597dd8cf08642657f6174703127f88903f8a43b41a` | alternate model variant (RCDATA 3071) |
| `decoded/boot.bin` | 67,452 | `fde8b95af5d707168505cb683b3e1ef6c530eaffea59fcefc48882610bf7472e` | byte-identical to the known CG100 boot image |

Source installer (verified): CG50 3.81 Windows
`https://www.casio.com/content/dam/casio/global/edu-casio-com/download/files/02_2_fx-cg50_G90_series_update_381_2b.zip`
= `efda30c622262b69ba84d6fdd68ae083fed352e680e3431b18e484516bbe464f`.
The compressed originals stay in `os_payload/` as provenance inputs.

**Honest interpretation:** the boot image shipped by the CG50 updater is
byte-identical to the CG100 bootcode image. A CG50-specific boot role is
inferred from the updater, not proven on hardware. None of this is a boot test.

### Still unresolved

- A **full 32 MiB flash dump** — we have the decoded OS prefix + boot image,
  not a complete flash layout; `flash_path` is intentionally empty.
- A **`cg50sh4` architecture core** — not shipped; the host needs one
  registered under that id before these images can be executed.
- **CG50 OS boot** — not attempted, not claimed.

## 5. Usage

```
CASIOEMU_DEFAULT_MODEL=/mnt/code/worktrees/pkg-cg50/models/fx-CG50 <host app>
```

or load `fx-CG50.model.zip` through the host's online model-package path.
A `cg50sh4` provider must be registered by the host/core build (dynamic plugin
on iOS, `RegisterStaticArchitectureProvider` on iPadOS).

## 6. Verification

```
python3 /mnt/code/worktrees/pkg-cg50/verify_package.py
```

It checks structure, config semantics, keymap↔SVG consistency, **independently
re-decompresses the genuine blobs** and verifies markers/hashes, checks manifest
hashes/byte-identity, builds and runs SH4IOS's `arch_descriptor_smoke.cpp`, and
runs a config-driven `FromModel()` check. `RESULT: PASS` requires every check.
It does not boot the OS. Latest run: **113/113 PASS**.

## 7. Constraints honoured

- No exam-mode material; no warez; only official Casio installer sources.
- No git push; no fabricated paths, hashes, or boot success.
