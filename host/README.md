# Host tools

Python tools run with [uv](https://docs.astral.sh/uv/) (`uv run --with <deps> host/<tool>.py`);
each one says what it needs at the top.

**Using the pad**

| | |
|---|---|
| `install_from_stock.py` | install the bootloader and a first firmware on a pad with the factory firmware, over USB-MIDI ([docs/bootloader.md](../docs/bootloader.md)) |
| `dfu_flash.sh` | send a firmware to the bootloader (`--bootloader`: update the bootloader itself) |
| `midi_monitor.py` | print what the pad sends; optionally send it MIDI clock or Mackie LED messages |
| `core_test.sh`, `core_test.py` | build, upload and run the Arduino core's self-test, and check its BLE advertising |

**Building and packaging**

| | |
|---|---|
| `package_fw.sh` | firmware / bootloader builds → app areas in `work/flash/` |
| `jlimage.py` | JieLi flash images: JLFS directories, CRCs, SFC encryption (`selftest` rebuilds the stock area byte for byte) |
| `bootpkg.py` | the bootloader update package (installer + staging area) |
| `check_appbin.py` | refuses an `app.bin` whose layout disagrees with what its crt0 copies |

**Recovery and reverse engineering** (ROM download mode through a Raspberry Pi Pico, [docs/flash-layout.md](../docs/flash-layout.md))

| | |
|---|---|
| `flash_fw.sh` | write / dump / restore the flash in download mode (`stock` puts the factory firmware back) |
| `jl_usb_bridge.py`, `bridge-rs/`, `install-bridge.sh` | the macOS SCSI bridge to a chip in download mode, and the small root helper it needs |
| `patches/` | macOS backend for kagaimiq's jl-uboot-tool |
| `sinco_proto.py`, `sinco_dump.py`, `sinco_flash.py` | the stock firmware's SysEx protocol: read and write its flash over USB-MIDI ([docs/sinco-protocol.md](../docs/sinco-protocol.md)) |
| `fwsc.py` | M-Vave's `.fwsc` update container |
| `update.py`, `con.py`, `con_daemon.py`, `dbg_*.sh`, `usb_selftest.py` | the bring-up console firmware's tools |
