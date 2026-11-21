#!/usr/bin/env python3
"""Build ONE universal ``CG100SH4.plugin`` from the four platform artifacts.

A single ZIP that works on macOS x64, Linux x64, Windows x64 and Android
arm64-v8a hosts.  The manifest is deliberately portable across all five host
parsers (desktop, Android, iOS, iPadOS):

  * ``platforms`` / ``architectures`` are OMITTED (every parser defaults to
    ``["macos"]``; other values are rejected by the strict parsers).
  * ``permissions`` is exactly ``["ui.shell"]`` (the ``arch`` permission is
    unknown on Android/iOS/iPadOS parsers).
  * ``native_entry`` points at the macOS binary: it keeps legacy macOS hosts
    working and is ignored at load time on hosts that find tagged binaries.
  * ``os_tags`` / ``arch_tags`` (underscores) are advisory only.

The ZIP contains exactly one tagged native binary per platform.  There are
deliberately NO ``*-any-*`` binaries: the host's platform selection is
exclusive (exact os-arch > os-any > any-arch > any-any), so a wildcard fallback
shipped beside a concrete binary would otherwise be loaded as well.

Windows dependencies are FLATTENED to the ZIP root: the Windows dlfcn shim uses
``LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS``, which
searches the directory of the loaded DLL (the extracted root), not ``root/lib``.

The output is self-verified: ZIP magic, exact entry set, per-entry bytes and
modes, JSON validity, per-platform selection, and the final SHA-256.

Usage
-----
    python3 tools/build_universal_plugin.py \
        --out "/Users/kin/Desktop/emu cg100-universal/CG100SH4.plugin"
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# Reuse the authoritative helpers/validators from build_plugin.py.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_plugin import BuildError, Payload, add_bytes, ensure_unique, sha256  # noqa: E402

# --------------------------------------------------------------------------- #
# Authoritative source artifacts (the four platform ZIPs inventoried in
# temp/UNIVERSAL_PLUGIN_RESEARCH.md).
# --------------------------------------------------------------------------- #
DEFAULT_SOURCES: Dict[str, Path] = {
    "mac": Path("/Users/kin/Desktop/emu cg100-test/CG100SH4.plugin"),
    "linux": Path("/Volumes/Code/ports/port/linux/build-linux/plugin/CG100SH4.plugin"),
    "win": Path("/Volumes/Code/ports/port/windows/tools/cg100-win/artifacts/CG100SH4-Windows.plugin"),
    "android": Path("/Volumes/Code/ports/port/android/app/src/main/assets/plugins/CG100SH4-Android.plugin"),
}

UNIVERSAL_MANIFEST = {
    "schema": 1,
    "id": "com.casioemu.arch.cg100sh4",
    "name": "CG100 SH4 Architecture Core",
    "version": "1.0.0",
    "author": "CasioEmuMsvc",
    "description": "Native SH4 emulator core for the CG100 board model (multi-platform build).",
    "min_host": "2.1.0",
    "type": "tool",
    "permissions": ["ui.shell"],
    "entry_point": "assets/info.json",
    "native_entry": "CG100SH4-macos-x64.dylib",
    "assets": ["assets/info.json"],
    "os_tags": ["macos", "linux", "windows", "android", "any"],
    "arch_tags": ["x64", "x32", "arm64", "arm64-android", "any"],
}

UNIVERSAL_INFO = {
    "schema": 1,
    "name": "CG100 SH4 Architecture Core",
    "description": "Native SH4 emulator core for the CG100 board model (universal multi-platform package).",
}

# (entry inside source ZIP, canonical entry in universal ZIP, executable?)
PICKS: Dict[str, Tuple[Tuple[str, str, bool], ...]] = {
    "mac": (
        ("CG100SH4.plugin", "CG100SH4-macos-x64.dylib", True),
        ("lib/libcig100core.dylib", "lib/libcig100core.dylib", True),
    ),
    "linux": (
        ("CG100SH4-linux-x64.so", "CG100SH4-linux-x64.so", True),
        ("lib/libcig100core.so", "lib/libcig100core.so", True),
    ),
    "win": (
        ("CG100SH4-windows-x64.dll", "CG100SH4-windows-x64.dll", True),
        ("lib/cg100core.dll", "cg100core.dll", True),           # flattened to root
        ("lib/libwinpthread-1.dll", "libwinpthread-1.dll", True),  # flattened to root
    ),
    "android": (
        ("libcg100sh4-android-arm64-android.so", "CG100SH4-android-arm64-android.so", True),
    ),
}

KNOWN_OS = ("macos", "linux", "windows", "android", "any")
KNOWN_ARCH = ("x64", "x32", "arm64", "arm64-android", "any")
NATIVE_EXTS = (".dylib", ".so", ".dll", ".plugin")

# (host os, host arch) -> expected single selected binary.
TARGETS = {
    ("macos", "x64"): "CG100SH4-macos-x64.dylib",
    ("linux", "x64"): "CG100SH4-linux-x64.so",
    ("windows", "x64"): "CG100SH4-windows-x64.dll",
    ("android", "arm64-android"): "CG100SH4-android-arm64-android.so",
}


@dataclass(frozen=True)
class Source:
    key: str
    zip_path: Path


def validate_manifest(manifest: dict) -> None:
    """Mirror the host's cross-port manifest rules for the universal package."""
    for forbidden in ("platforms", "architectures"):
        if forbidden in manifest:
            raise BuildError(f"manifest must omit '{forbidden}' (portability)")
    if manifest.get("schema") != 1:
        raise BuildError("manifest schema must be 1")
    if manifest.get("permissions") != ["ui.shell"]:
        raise BuildError("permissions must be exactly ['ui.shell']")
    if manifest.get("type") != "tool":
        raise BuildError("universal package must be type 'tool'")
    if not manifest.get("native_entry"):
        raise BuildError("native_entry is required for legacy macOS hosts")
    if manifest.get("os_tags") is None or manifest.get("arch_tags") is None:
        raise BuildError("os_tags/arch_tags (underscores) are required for the desktop tool check")


def parse_tagged(name: str) -> Optional[Tuple[str, str, str, str]]:
    """Python mirror of PlatformTags.h::ParseTaggedNativeFileName."""
    dot = name.rfind(".")
    if dot <= 0 or dot + 1 >= len(name):
        return None
    stem, ext = name[:dot], name[dot:]
    for os_tag in KNOWN_OS:
        for arch_tag in KNOWN_ARCH:
            suffix = f"-{os_tag}-{arch_tag}"
            if len(stem) > len(suffix) and stem.endswith(suffix):
                return (stem[: -len(suffix)], os_tag, arch_tag, ext)
    return None


def match_rank(os_tag: str, arch_tag: str, host_os: str, host_arch: str) -> int:
    """Python mirror of PlatformTags.h::PlatformMatchRank (-1 = no match)."""
    os_ok = os_tag == "any" or host_os == "any" or os_tag == host_os
    arch_ok = arch_tag == "any" or host_arch == "any" or arch_tag == host_arch
    if not (os_ok and arch_ok):
        return -1
    os_exact = os_tag != "any" and os_tag == host_os
    arch_exact = arch_tag != "any" and arch_tag == host_arch
    if os_exact and arch_exact:
        return 4
    if os_exact:
        return 3
    if arch_exact:
        return 2
    return 1


def validate_selection(payloads: List[Payload]) -> Dict[Tuple[str, str], str]:
    """Self-check: exactly one best-match binary per target; no any-* binaries."""
    names = [p.archive_name for p in payloads]
    tagged: List[Tuple[str, str, str, str]] = []
    tagged_names: List[str] = []
    for name in names:
        parsed = parse_tagged(name)
        if parsed is None or parsed[3] not in NATIVE_EXTS:
            continue
        if parsed[1] == "any" or parsed[2] == "any":
            raise BuildError(f"universal package must not contain an any-* fallback binary: {name}")
        tagged.append(parsed)
        tagged_names.append(name)

    selected: Dict[Tuple[str, str], str] = {}
    for host, expected in TARGETS.items():
        ranks = [(name, match_rank(t[1], t[2], host[0], host[1]))
                 for name, t in zip(tagged_names, tagged)]
        matched = [(name, rank) for name, rank in ranks if rank >= 0]
        if not matched:
            raise BuildError(f"no tagged binary matched host {host[0]}-{host[1]}")
        best = max(rank for _, rank in matched)
        best_names = [name for name, rank in matched if rank == best]
        if best_names != [expected]:
            raise BuildError(
                f"host {host[0]}-{host[1]}: expected exactly [{expected}], got {best_names} "
                f"(tier {best})"
            )
        selected[host] = best_names[0]

    # Dependency placement: mac/Linux deps in lib/, Windows deps at root.
    dep_names = {p.archive_name for p in payloads}
    for required in ("lib/libcig100core.dylib", "lib/libcig100core.so",
                     "cg100core.dll", "libwinpthread-1.dll"):
        if required not in dep_names:
            raise BuildError(f"missing dependency entry: {required}")
    if any(n.startswith("lib/") and n.endswith(".dll") for n in names):
        raise BuildError("Windows DLL dependency found under lib/; it must be flattened to root")
    return selected


def build_payloads(sources: Dict[str, Source]) -> Tuple[List[Payload], Dict[str, bytes]]:
    validate_manifest(UNIVERSAL_MANIFEST)
    payloads: List[Payload] = [
        Payload("plugin.json",
                data=(json.dumps(UNIVERSAL_MANIFEST, indent=2) + "\n").encode("utf-8"),
                mode=0o644),
        Payload("assets/info.json",
                data=(json.dumps(UNIVERSAL_INFO, indent=2) + "\n").encode("utf-8"),
                mode=0o644),
    ]

    raw: Dict[str, bytes] = {
        "plugin.json": payloads[0].data,
        "assets/info.json": payloads[1].data,
    }
    for key in ("mac", "linux", "win", "android"):
        source = sources[key]
        if not source.zip_path.is_file():
            raise BuildError(f"source ZIP not found ({key}): {source.zip_path}")
        with zipfile.ZipFile(source.zip_path, "r") as archive:
            for entry, canonical, executable in PICKS[key]:
                if entry not in archive.namelist():
                    raise BuildError(f"{source.zip_path}: missing entry {entry}")
                data = archive.read(entry)
                mode = 0o755 if executable else 0o644
                payloads.append(Payload(canonical, data=data, mode=mode, native=executable))
                raw[canonical] = data

    ensure_unique(payloads)
    validate_selection(payloads)
    return payloads, raw


def write_and_verify(payloads: List[Payload], raw: Dict[str, bytes], out: Path) -> str:
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".universal.tmp")
    if tmp.exists():
        tmp.unlink()
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as archive:
        for payload in payloads:
            add_bytes(archive, payload.archive_name, payload.data, payload.mode)
    os.replace(tmp, out)

    with out.open("rb") as handle:
        if handle.read(4) != b"PK\x03\x04":
            raise BuildError("output does not have ZIP magic")
    with zipfile.ZipFile(out, "r") as archive:
        infos = archive.infolist()
        actual = {i.filename for i in infos}
        expected = {p.archive_name for p in payloads}
        if actual != expected:
            raise BuildError(
                f"entry list mismatch: missing={sorted(expected - actual)}, "
                f"extra={sorted(actual - expected)}"
            )
        folded = {}
        for info in infos:
            lower = info.filename.lower()
            if lower in folded:
                raise BuildError(f"case-colliding entries: {folded[lower]} / {info.filename}")
            folded[lower] = info.filename
        for payload in payloads:
            info = archive.getinfo(payload.archive_name)
            mode = (info.external_attr >> 16) & 0o7777
            if mode != payload.mode:
                raise BuildError(f"mode mismatch for {payload.archive_name}: {oct(mode)}")
            if info.create_system != 3:
                raise BuildError(f"{payload.archive_name}: create_system != Unix(3)")
            if archive.read(payload.archive_name) != raw[payload.archive_name]:
                raise BuildError(f"content mismatch for {payload.archive_name}")
        for json_name in ("plugin.json", "assets/info.json"):
            if not isinstance(json.loads(archive.read(json_name)), dict):
                raise BuildError(f"{json_name} is not a JSON object")
    return sha256(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="output universal .plugin path")
    for key in DEFAULT_SOURCES:
        parser.add_argument(f"--{key}", type=Path, default=DEFAULT_SOURCES[key],
                            help=f"override {key} source ZIP")
    args = parser.parse_args()
    try:
        sources = {key: Source(key, getattr(args, key)) for key in DEFAULT_SOURCES}
        payloads, raw = build_payloads(sources)
        digest = write_and_verify(payloads, raw, args.out)
    except (BuildError, zipfile.BadZipFile, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"Wrote universal plugin: {args.out}")
    print(f"  SHA-256: {digest}")
    print(f"  Entries ({len(payloads)}):")
    for payload in payloads:
        blob = raw[payload.archive_name]
        entry_hash = hashlib.sha256(blob).hexdigest()[:16]
        print(f"    {payload.archive_name:42s} {oct(payload.mode)} "
              f"{len(blob):>10d} B  sha256:{entry_hash}...")
    print("  Manifest: platforms/architectures omitted, permissions=['ui.shell'], "
          "native_entry=CG100SH4-macos-x64.dylib")
    print("  Selection: macos-x64=mac dylib, linux-x64=linux so, "
          "windows-x64=win dll, android-arm64-android=android so (exactly one each)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
