# open_smc_pad

Open firmware for the M-Vave SMC-PAD (JieLi AC6966C / BR25, pi32v2 CPU): our own firmware,
flashing and update tooling, and the reverse engineering behind them.

| path | what |
|---|---|
| `fw/smcpad/` | the firmware (JieLi SDK app; `usb/` = TinyUSB + our BR25 driver) |
| `fw/build.sh` | build in Docker (`SMCPAD_TINYUSB=1` for the TinyUSB stack) |
| `host/` | flashing (`flash_fw.sh`), self-update (`update.py`), console, tests, root USB helper (`bridge-rs`) |
| `hw/jl-usbkey/` | Raspberry Pi Pico sketch that sends the USB_KEY (ROM download mode) |
| `docs/` | hardware map, flash layout, USB controller notes, open gaps |
| `re/` | reverse-engineering notes and captures |
| `sdk/`, `tools/`, `third_party/`, `ref/` | upstream projects, as submodules |

Not in the repository: `dumps/` and `vendor/` (stock firmware, flash dumps), the JieLi
Linux toolchain and post-build tools (`tools/jl-toolchain/`, `tools/jl-postbuild/`, unpacked
from the archives JieLi publishes), build output in `work/`.

The macOS backend for jl-uboot-tool lives in `host/patches/jl-uboot-tool-macos.patch`
(`git -C tools/jl-uboot-tool apply ../../host/patches/jl-uboot-tool-macos.patch`).

Start with `docs/flash-layout.md` (flashing and updating) and `docs/hardware.md`.
