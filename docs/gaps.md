# Open gaps & things to verify

Running list for the open SMC-PAD firmware. Updated as we go.

## Blocking / high-risk

- ~~Power architecture~~ **RESOLVED 2026-10-18:** the power control is a mechanical **slide switch**
  that cuts the battery. No GPIO power-hold is needed; a custom
  image cannot "fail to stay on". Original analysis kept below for reference.
- *(historical)* Power architecture — two possibilities were considered:
  1. *Self-held rail*: the MCU keeps its own regulator on through a GPIO (soft latch with the
     Q transistors). Then our firmware must drive that pin within ms of boot or the board turns off.
     Hint for this: the stock `isd_config.ini` has `UPDATE_JUMP`, which the SDK documents
     (`CONFIG_UPDATE_JUMP_TO_MASK`) as meant for "power KEEP'd by an IO" boards — but it may be a default.
  2. *Standard JieLi*: chip powered straight from the battery/LDO, power control is a hard slide
     switch or a wake-up key, soft-off handled by the P33 PMU (the stock fw does use P33: 6 refs).
     Then no hold pin is needed.
  Checks (2 minutes, multimeter): is the power control a **slide switch** (2 positions) or a
  **momentary button**? With the board "off" and the battery connected, is the 3.3 V rail
  (U12 LDO output, C105/C106) at 0 V or at 3.3 V? 0 V + momentary button ⇒ case 1.

## Bring-up

- **First boot of our own firmware (2026-10-18): every LED lit, frozen, no sweep.** Both the
  button LEDs and the pad RGBs, which our test never touches — that is what the board looks like
  when nothing drives the shift registers, so the firmware stops somewhere in the SDK's init.
  Ruled out by inspection: the image format (`host/check_appbin.py` verifies the crt0's copy
  addresses against the real offsets in `app.bin`, and the stock image checks out the same way),
  and `TCFG_POWER_ON_NEED_KEY` (0 in our board config).
  Still suspect, in order: `cfg_file_parse(0)` in `board_init` (our area has no `config.dat`
  resources, only `app.bin` + `cfg_tool.bin`), `board_power_init`, and — if the boot does
  complete — `eSystemConfirmStopStatus()` returning 1 ("Endless Sleep"), which would freeze the
  LEDs exactly like this once `sys_timer` stops firing.
- ~~Does our firmware even run?~~ **RESOLVED 2026-10-19: it boots completely.** The CDC port
  appeared and printed the whole trace: every step of `board_init` (including
  `cfg_file_parse`, the prime suspect), `app_main`, USB up, the idle app, the sweep running.
  The image format and the SDK init are therefore fine.
- **The LEDs never react to anything we drive**, which makes the inferred LED wiring in
  [hardware.md](hardware.md) the open question. A wrong **latch** pin is the leading suspect
  because it is indistinguishable from a dead output stage: data shifts in forever and the
  595's output register never updates. /OE on PB5, the seven "rows"
  (PA5, PA2, PA0, PA1, PA3, PC2, PC3) and the LED polarity are equally unconfirmed.
  Tool for this: the **bring-up console** (`fw/smcpad/examples/smcpad_test/smcpad_bringup.c`),
  a shell on the CDC port — drive any pin, shift into the 595s, reassign the clock/data/latch
  pins at runtime, scan every pin one at a time, or walk all 24 clock/data/latch permutations
  of PB4..PB7. Host side: `host/dbg_listen.sh` to read and `host/dbg_send.sh` to send.
- **Debug channel: a USB CDC serial** (`fw/smcpad/smcpad_dbg.c`, `TCFG_PC_ENABLE` +
  `USB_DEVICE_CLASS_CONFIG (CDC_CLASS)` in the board config). The firmware appears as
  `/dev/cu.usbmodem*` under vendor id **0x4C4A** ("JL", from the SDK's CDC descriptor), which
  distinguishes it from the stock firmware (SINCO, 0x4353) and from ROM download mode (also
  0x4C4A but with no serial port). Listen with `host/dbg_listen.sh`, which ignores the ports
  that already exist (the Pico is one of them) and attaches to the first new one.
  Because USB only comes up in the app, milestones are recorded with `smcpad_trace()` into a
  768-byte RAM buffer first, and the whole trace is flushed the moment the host opens the port:
  the log of a boot that happened before the log channel existed. `cdc_write_data()` returns 0
  while the port is unconfigured, so none of it can block; the write side needs the OS (CDC
  takes a mutex), so only the app may call it, while `smcpad_trace()` is safe anywhere.
  No serial port appearing at all is itself the answer: the boot does not reach the app.
  UART0 is not an option here — its TX pins on this chip are PA05/PB00/PB05/PC03, all taken
  and none reachable without soldering. D+ is `UT1_TX` in the driver's pin table
  (`cpu/br25/uart_dev.c`) and the Pico is already wired to it, which is the fallback if USB
  itself turns out to be the problem.
- Button-LED orientation (anode on GPIO rows or on 595 columns) unknown — the LED test
  (`fw/smcpad/examples/smcpad_test/smcpad_ledtest.c`) sweeps both polarities, one LED at a time.
- Pad RGB LEDs (PB1) untouched by the first test.
- Restore path: full 004 flash backup in `dumps/SMC-PAD_004_flash_t0.bin`, vendor 005 in `vendor/`.

## Flashing (resolved 2026-10-18)

- Image format, CRCs, encryption windows and boot selection are fully decoded in
  [docs/flash-layout.md](flash-layout.md); `host/jlimage.py selftest` rebuilds the stock app
  area byte for byte.
- The open firmware is reached by repointing `app_dir_head` in sector 0 at a slot in free
  flash; the stock firmware stays intact and is the recovery path.
- **Open:** our firmware runs with the VM declared at 0x71000 (needed to keep our code inside
  the encrypted window). The SDK's VM therefore lives at 0x71000..0x74000 and the stale test
  images at 0x30000/0x60000 are junk inside the stock VM region; clean up when we own the
  whole flash layout.
- **Open:** writing the slot requires pointing at the stock directory first, because the
  firmware write-protects everything below `vm_saddr` (so below 0x71000 while our directory
  is active).
- **Open:** a firmware that hangs before `app_main` (SDK clock/power init) still locks us out
  of USB; the only fallback is the Pico USB_KEY + jl-uboot-tool on Linux/Windows.

## Build / packaging

- Build: `fw/build.sh` → `fw/smcpad/out/sdk.elf` + `app.bin`, then `host/package_fw.sh` →
  `work/flash/{fw_area.bin,fw_area_raw.bin,expected_with_fw.bin}`, then `sudo host/flash_fw.sh`.
  App = SDK `hid` app copied to `fw/smcpad`, `CONFIG_APP_IDLE`, board `CONFIG_BOARD_SMCPAD`
  (from AC6366C demo), BT off.
- `app.bin` is a bare concatenation and nothing in it records where the parts end: the crt0
  copies `.data`/`.data_code` from absolute flash addresses fixed at link time. `fw/build.sh`
  now mirrors `cpu/br25/tools/download.sh` exactly, overlays included (`remove_tailing_zeros`
  from `tools/jl-postbuild`), and `host/check_appbin.py` fails the build-to-flash path if the
  file and the crt0 ever disagree.
- **The JieLi toolchain builds with LTO**: the `.o` files are LLVM bitcode (`nm` says "File
  format not recognized") and functions get inlined across translation units, so a symbol
  missing from `sdk.elf` does **not** mean the code was configured out. To check that a feature
  really made it in, look for the data it references (e.g. `cdc_desc_config` for USB CDC) or
  assert the configuration at compile time with `#error` — note the SDK's Makefile suppresses
  `#warning`, so warnings are invisible.
- USB device config follows the SDK's board pattern: `TCFG_PC_ENABLE`, then
  `#include "usb_std_class_def.h"` (it supplies the endpoint numbers), then `#undef` and
  redefine `USB_DEVICE_CLASS_CONFIG`. Without that include the build fails on
  `CDC_DATA_EP_IN` and friends.
- With `TCFG_USER_EDR_ENABLE 0` the SDK also requires `USER_SUPPORT_PROFILE_HID/PNP 0`
  (`#error` in `modules/bt/app_comm_edr.c`).

- `make ac636n_hid -jN` races: the post-build script runs before `sdk.elf` finishes linking.
  Must link `cpu/br25/tools/sdk.elf` first, *then* run post-build (or build with `-j1`).
- Linux post-build tools (`ufw_maker`, `fw_add`, `remove_tailing_zeros`, `isd_download`) are the
  x86_64 ELF binaries in `tools/jl-postbuild/jieli-linux-post-build-tools-20260923.1/`. The stock
  `download.sh` instead calls `host-client` (GUI tool, not present) — needs replacing with a plain
  shell post-build that chains the standalone tools.
- Flash size: must set `CONFIG_FLASH_SIZE` to 512K (`0x80000`) to match the AC6966C (dump is 512 KB).

## Hardware map still to confirm (see docs/hardware.md)

- 595 /OE vs 4051 INH on PB5.
- MIDI OUT on PC5: through which UART/timer and at 31250 baud.
- The 2 extra ADC conversions after the 16 pads (battery? internal temp?).

## Decisions deferred

- Our own USB-MIDI updater command in the open firmware ("ci pensiamo poi").
- Whether to keep JieLi BT/BLE-MIDI (closed blobs) in the open firmware or drop it.
