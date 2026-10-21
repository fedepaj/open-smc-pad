#!/usr/bin/env python3
"""package_smcpad_index.json for the Boards Manager, from the built archives.

    make_index.py <version> <artifacts dir> <base url> > package_smcpad_index.json

The artifacts dir holds smcpad-br25-<version>.tar.bz2 and smcpad-tools-<version>-<host>.tar.gz;
<base url> is where they will be downloaded from (a GitHub release). The JieLi toolchain is
not ours to redistribute: its entry points at JieLi's own download (the file pkgman.jieliapp.com
links to), pinned by size and SHA-256. dfu-util is Arduino's.
"""
import hashlib
import json
import sys
from pathlib import Path

JIELI_TOOLCHAIN = {
    "version": "20260730.1",
    "url": "https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20260730.1.tar.xz",
    "archiveFileName": "jieli-linux-toolchains-20260730.1.tar.xz",
    "checksum": "SHA-256:a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd",
    "size": "26027508",
}
# the toolchain is Linux x86-64; on macOS it runs through blink (smcpad-tools)
TOOLCHAIN_HOSTS = ["x86_64-linux-gnu", "x86_64-apple-darwin", "arm64-apple-darwin"]
DFU_UTIL = {"packager": "arduino", "name": "dfu-util", "version": "0.11.0-arduino5"}
REPO = "https://github.com/fedepaj/open-smc-pad"


def describe(path, base):
    data = path.read_bytes()
    return {"url": f"{base.rstrip('/')}/{path.name}", "archiveFileName": path.name,
            "checksum": "SHA-256:" + hashlib.sha256(data).hexdigest(), "size": str(len(data))}


def main():
    version, artifacts, base = sys.argv[1], Path(sys.argv[2]), sys.argv[3]
    platform = artifacts / f"smcpad-br25-{version}.tar.bz2"
    tools = sorted(artifacts.glob(f"smcpad-tools-{version}-*.tar.gz"))
    if not platform.exists() or not tools:
        sys.exit(f"missing artifacts in {artifacts}")
    index = {"packages": [{
        "name": "smcpad",
        "maintainer": "open-smc-pad",
        "websiteURL": REPO,
        "help": {"online": REPO + "/tree/main/arduino"},
        "platforms": [{
            "name": "SMC-PAD (open firmware)",
            "architecture": "br25",
            "version": version,
            "category": "Contributed",
            "help": {"online": REPO + "/tree/main/arduino"},
            **describe(platform, base),
            "boards": [{"name": "M-Vave SMC-PAD (open firmware)"}],
            "toolsDependencies": [
                {"packager": "smcpad", "name": "jieli-toolchain", "version": JIELI_TOOLCHAIN["version"]},
                {"packager": "smcpad", "name": "smcpad-tools", "version": version},
                DFU_UTIL,
            ],
        }],
        "tools": [
            {"name": "jieli-toolchain", "version": JIELI_TOOLCHAIN["version"],
             "systems": [dict(host=h, **{k: v for k, v in JIELI_TOOLCHAIN.items() if k != "version"})
                         for h in TOOLCHAIN_HOSTS]},
            {"name": "smcpad-tools", "version": version,
             "systems": [dict(host=t.name[len(f"smcpad-tools-{version}-"):-len(".tar.gz")], **describe(t, base))
                         for t in tools]},
        ],
    }]}
    json.dump(index, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
