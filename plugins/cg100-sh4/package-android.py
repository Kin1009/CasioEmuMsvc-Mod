#!/usr/bin/env python3
"""Package the Android SH4 plugin as a host-loadable .plugin ZIP container.

Layout follows CasioEmuMsvc's plugin format:

    plugin.json                       manifest (platforms: android)
    assets/info.json                  declarative entry point
    libcg100sh4-android-arm64-android.so   native_entry, stored with mode 0755

Usage:
    package-android.py --so <path-to-libcg100sh4-...so> \
        --abi arm64-v8a --out <output.plugin>
"""

from __future__ import annotations

import argparse
import json
import os
import stat
import zipfile
from pathlib import Path


def platform_tag(abi: str) -> str:
    return {
        "arm64-v8a": "arm64-android",
        "armeabi-v7a": "any",
        "x86_64": "x64",
        "x86": "x32",
    }.get(abi, "any")


def manifest(abi: str, native: str) -> dict:
    tag = platform_tag(abi)
    return {
        "schema": 1,
        "id": "com.casioemu.arch.cg100sh4",
        "name": "CG100 SH4 Architecture Core (Android)",
        "version": "1.0.0",
        "author": "CasioEmuMsvc",
        "description": (
            "Self-contained native SH4 core for the fx-CG100 board model, "
            f"built for Android {abi} ({tag})."
        ),
        "min_host": "2.1.0",
        "platforms": ["android", "any" if tag == "any" else f"android-{tag}"],
        "type": "tool",
        "permissions": ["ui.shell"],
        "entry_point": "assets/info.json",
        "native_entry": native,
        "assets": ["assets/info.json"],
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--so", required=True, type=Path, help="built libcg100sh4-*.so")
    parser.add_argument("--abi", default="arm64-v8a")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    so = args.so.resolve()
    if not so.is_file():
        raise SystemExit(f"missing native library: {so}")
    native_name = so.name
    info = {
        "name": "CG100 SH4 Architecture Core (Android)",
        "description": f"Native SH4/emulator bridge for Android {args.abi}.",
        "native_entry": native_name,
    }

    def add_bytes(archive: zipfile.ZipFile, name: str, data: bytes, mode: int) -> None:
        # PluginPackage.cpp rejects entries without S_IFREG/S_ISDIR mode bits.
        # Python's writestr(name, str) sets only 0o600 and no file-type bit,
        # which would make the host refuse to extract this package.
        entry = zipfile.ZipInfo(name, date_time=(2020, 1, 1, 0, 0, 0))
        entry.compress_type = zipfile.ZIP_DEFLATED
        entry.external_attr = (stat.S_IFREG | mode) << 16
        entry.create_system = 3  # Unix: mode bits are honoured by the extractor
        archive.writestr(entry, data)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as archive:
        add_bytes(archive, "plugin.json",
                  (json.dumps(manifest(args.abi, native_name), indent=2) + "\n").encode("utf-8"),
                  0o644)
        add_bytes(archive, "assets/info.json",
                  (json.dumps(info, indent=2) + "\n").encode("utf-8"), 0o644)
        add_bytes(archive, native_name, so.read_bytes(), 0o755)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
