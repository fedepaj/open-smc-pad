#!/usr/bin/env python3
"""system/app_template.bin: the application area's directory (9 JLFS entries, VM and PRCT moved
for the bootloader layout) and the SDK's cfg_tool.bin, which smcpad_pack wraps around a sketch.

    make_template.py <out>
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import jlimage as J  # noqa: E402

t, cfg = J.template_and_cfg()
area = J.build_app_area(b"\0" * 0x200, cfg, t, entry_vma=0x1E08120, vm=(0x71000, 0x4000), prct_size=0x71000)
Path(sys.argv[1]).write_bytes(area[:0x120] + cfg)
