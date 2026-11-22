# Data-only model packs bundled into the iPadOS app

These directories are copied by `CasioEmuMsvc/CMakeLists.txt` POST_BUILD into
`<CasioEmuMsvc.app>/models/` (the iOS bundle root has no `Contents/Resources/`).
They are **data only**: no architecture core, no native code, no JIT. The host
start-up code points `CASIOEMU_MODELS_DIR` at this absolute bundle directory via
`ipados/src/IPadOSBridge.mm::SetBundleResourcePaths()`, so Welcome → All models
finds them regardless of the launch working directory.

## Contents

| Path | Source | What it is | Boot claim |
| --- | --- | --- | --- |
| `cg50/fx-CG50.model.zip` | `/mnt/code/worktrees/pkg-cg50` (PKGCG50) | Host-compatible ZIP: `board.svg` (396×224 screen slot + 50 keys), `config.json` (`arch_provider=cg50sh4`), schematic `face.png`, decoded `rom.bin` | **None.** No `cg50sh4` provider is statically linked on iPadOS; selecting this model cannot emulate it yet. |
| `cg50/manifest.json`, `cg50/PROVENANCE.md` | PKGCG50 | SHA256 manifest + official-installer provenance (Casio OS 3.81) | Provenance only. |
| `giii/fx-9860GIII/` | `/mnt/code/worktrees/pkg-9860` (PKG9860 M4) | 128×64 board model, schematic skin, decoded OS 3.71 `os.bin`/`os_3071.bin`, original encoded `os_payload/*.bin`, `os_manifest.json` | **None.** No registered `giii.sh4a.core` provider, no proven bootcode, no boot demo. |
| `cg100/` | — | Placeholder instructions only | The CG100 pack (board.svg/flash.bin/interface.png/rom.bin) lives on the Mac build host, not in these Pi trees. It is **not** fabricated here. |

## What must NOT be interpreted as success

- The decoded `rom.bin` / `os.bin` files are genuine decoded update resources,
  not a proven bootable flash image with validated bootcode.
- Bundling a pack makes it **appear in Welcome → All models**. It does not make
  the model run. The generic SH4 descriptor hook
  (`RegisterStaticArchitectureProvider()`) has no call site in this tree; see
  `docs/IPADOS_PORT.md` §5 and the IPADOSSTATUS report C9/V8.
- `arch_provider` ids (`cg50sh4`, `sh3.giii.skeleton`) are opaque dispatch ids;
  the GIII one has a documented naming-mismatch/rename plan in its manifest.

## How to add the CG100 pack on the Mac

1. Copy the real CG100 model folder (the one used by the desktop/Linux builds)
   to `ipados/model_packs/cg100/fx-CG100/`. Keep `config.json`, `board.svg`,
   `interface.png`, `rom.bin`/`flash.bin` exactly as shipped — do not synthesise
   missing files.
2. Re-run `ipados/build-ipados-sideload.sh <mode>`; CMake's `copy_directory`
   refreshes `<app>/models/` on every build.
3. Verify in the bundle: `find <app>/models -maxdepth 2 -type f | sort`.

The CG100 **native** `CG100SH4.plugin`/`libcig100core.dylib` is deliberately not
bundled: iPadOS forbids `dlopen()` of unsigned dylibs and JIT. A real CG100
capability needs a statically linked, symbol-exported core.
