# SMC-PAD flash layout, encryption and boot selection

Everything here was verified on the live board (fw `SMC-PAD_004`) against the full backup
`dumps/SMC-PAD_004_flash_t0.bin` and the vendor update `SMC-PAD_005.fwsc`.
Tools: [`host/jlimage.py`](../host/jlimage.py) (image builder, `selftest` rebuilds the stock
area byte for byte) and [`host/bootsel.py`](../host/bootsel.py) (boot pointer + arm flag).

## Map

| flash | content |
|---|---|
| `0x00000` | top area: flash header, file list (`uboot.boot`, `isd_config.ini`, `app_dir_head`, `key_mac`), uboot.boot, isd_config.ini |
| `0x01000` | **app directory** (9 × 32 B entries) — mapped to CPU `0x1E00000` |
| `0x01120` | stock `app.bin` (`0x2417c` B), then `cfg_tool.bin` at `0x2429c` |
| `0x255fc` | resource area: `config.dat`, `ver_info`, `md5.bin` … up to `0x26000` |
| `0x26000` | VM (stock), free and erased from `0x27000` to `0x75000` |
| `0x75000` | BTIF · `0x76000` EXIF · `0x77000` USRFLASH · `0x7e000` USRTRIM · `0x7f000` key_mac |

CPU address = `0x1E00000 + flash_off - 0x1000` (the SFC maps flash `0x1000` at `0x1E00000`).

## Directory entries (JLFS2, 32 bytes)

`u16 hdr_crc, u16 data_crc, u32 offset, u32 size, u8 attr, u8 resvd, u16 index, char name[16]`

- `hdr_crc` = CRC16-CCITT (poly `0x1021`, init 0, not reflected) over the other 30 bytes.
- `data_crc` = the same CRC over the file's data.
- `app_area_head.data_crc` = CRC over the area from `+0x20` to `size` (everything after its own
  entry). `app_area_head.offset` is **the entry point the uboot jumps to**, not an offset.
- `index != 0` marks the last entry. Offsets inside the area are relative to the directory.

## Encryption

The SFC's ENC block descrambles flash reads with the chipkey `0x980F`, in 32-byte blocks, with
`key = chipkey ^ (cpu_addr >> 2)` (`host/jlimage.py: sfc_cipher`).  The window is **not** the
whole flash: the firmware programs `JL_SFCENC` (`0x1F0300`) with `LENC_ADRH = cpu(vm_saddr)`,
i.e. **everything below the VM start is encrypted, everything above is stored as is**.
`vm_saddr` comes from the VM entry of the directory the uboot loaded, so a firmware in free
flash controls that boundary through its own directory.

Consequences, all verified live:

- SINCO reads (cmd `0x23`) return what the CPU sees: plain below `0x26000` on the stock layout
  (so a dump of the app area is the plaintext app.bin), as-is above it.
- SINCO writes (cmd `0x22`) are **raw**: to put code below the boundary you must write the
  SFC-encrypted form (`jlimage.sfc_encrypt_at`), and the read-back then shows the plaintext.
- **Max 1024 bytes per write.** 2048 corrupts the data, 4096 crashes and reboots the firmware.

Sector 0 is the exception: it does not read back what you write (one side of the path XORs it
with a fixed keystream, the boot area being stored scrambled). `bootsel.py` writes it, looks at
what comes back and uses the difference as the keystream for the next attempt, then verifies.

## Flashing our firmware (full replacement)

The firmware replaces the stock one in the app area; sector 0 / uboot stay factory.

1. Build: `fw/build.sh` (linked at the factory entry `0x1E00120`; `app.bin` is built exactly as
   the SDK's `download.sh` does it, overlays included).
2. Package: `host/package_fw.sh` — runs `host/check_appbin.py` (the crt0's copy addresses must
   match where `.data`/`.data_code` really land in `app.bin`), then `jlimage.py build` (same
   directory as stock: VM `0x26000`, PRCT `0..0x26000`; `--raw` = SFC-encrypted for `0x1000`),
   then writes `work/flash/expected_with_fw.bin`, the image the read-back is checked against.
3. Wire the Pico running `hw/jl-usbkey` to the pad's USB data lines (GP2 -270R- D+ (U9 pin 25),
   GP3 -270R- D- (pin 24), GND) and leave it plugged in.
4. Once per machine: build and install the root helper (see below).
5. `host/flash_fw.sh` — or `host/flash_fw.sh stock` to put the factory firmware back. **Runs as
   you, no password.** If our firmware is running it enters download mode by itself; otherwise it
   asks for a power cycle.

### The root helper (once per machine)

macOS binds its mass-storage driver to the chip in download mode and only root may detach it;
no library avoids that. So the privileged part is one small Rust binary,
[`host/bridge-rs`](../host/bridge-rs) → `/usr/local/libexec/smcpad-usb-bridge`, allowed in
sudoers without a password:

    (cd host/bridge-rs && cargo build --release)
    sudo host/install-bridge.sh

It is built to be safe in that position: no arguments (and `""` in the sudoers rule), one fixed
socket `/var/run/smcpad/jl.sock` in a directory it creates root-owned and refuses to use
otherwise, only JieLi download-mode devices (not our own 4C4A:4155), libusb linked statically
(nothing loaded from the user-writable `/opt/homebrew`), and it exits on its own after 10 s
without a client — an unprivileged caller cannot signal it. Same wire protocol as the old
`host/jl_usb_bridge.py`, so jl-uboot-tool is unchanged.

### Download mode without touching the power switch

The ROM enters download mode when it sees the USB_KEY as it starts. Our firmware's console
command `J` drops off USB (pins to GPIO, no pull-up — the Pico reads "D+ high" as the chip's
acknowledgement) and jumps to `__JUMP_TO_MASKROM` (SDK, RAM), i.e. the ROM's reset entry
`0x110000`; `flash_fw.sh` sends `J`, then starts the key on the Pico, and the chip comes up in
UBOOT. A firmware stuck in a watchdog reset loop goes through the ROM every few seconds too, so
keying alone usually catches it. Only the stock firmware still needs a hand on the switch.

Pitfalls found getting this to work hands-free:

- `J` must switch the USB PHY off (`JL_USB->CON0 = 0`, clear `DPPU`/`DMPU` in `USB_IO_CON0`).
  `usb_iomode(1)` and the GPIO pulls alone leave the 1.5k on D+: the host drops the device
  anyway, but the Pico reads D+ high and takes it for the ROM's acknowledgement after 0 keys.
- The Pico (`hw/jl-usbkey`) therefore starts keying (and accepts an acknowledgement) only after
  D+ has stayed low for 10 ms in that attempt -- one low sample is not enough, the host's
  packets toggle D+ while a device is still attached; while D+ is high it leaves both lines
  alone, so an armed attempt is harmless. The ROM looks for the key right after reset, sooner
  than those 10 ms, so the Pico must be keying *before* the chip restarts: that is why `J`
  detaches and waits 2 s before jumping, and why `Z` (plain reset) or an old `J` that left
  D+ pulled up cannot be caught -- those need a power cycle.
- Commands to a CDC port must be written with the port held open (`port_send` in
  `flash_fw.sh`: open, drain with `cat`, write, wait 0.5 s). `printf J > /dev/cu...` opens and
  closes at once and our console loses the command. To read the Pico reliably use pyserial
  (`host/.venv-serial`). On its serial port: `r` starts
  (or restarts) an attempt, `x` aborts it, `d` holds SE0 for 300 ms (an unplug as far as the
  host can tell). Never send `r` repeatedly: the old sketch queued one 60 s attempt per `r`,
  each driving SE0, and the Mac gave up on the port (pad no longer seen at all, even after
  `d`) -- only a real cable replug or power cycle brought it back.
- In the script, never `ioreg | grep -q` under `set -o pipefail`: grep quits on the first
  match, ioreg dies of SIGPIPE and the pipeline "fails" at random -- that made "our firmware
  is on USB" false and "JieLi but not 4155, so UBOOT" true while our firmware was running.
- The Pico sketch is built and uploaded without touching it: `arduino-cli compile --fqbn
  rp2040:rp2040:rpipico --output-dir build hw/jl-usbkey` and `arduino-cli upload ... -p
  /dev/cu.usbmodem<pico> --input-dir build` (1200-baud touch into its UF2 loader).

What the script does, and what to do by hand if it breaks:

- ROM download mode: with the pad off it sends the USB_KEY over the Pico's serial port
  (`printf r > /dev/cu.usbmodem*`, same as pressing BOOTSEL); power the pad on and it enumerates
  as `BR25 UBOOT1.00` (VID `0x4C4A`).
  **The vendor id alone is not enough to recognise it.** Our own firmware's CDC uses the SDK's
  default descriptor, which is JieLi's too, so both answer to vendor `0x4C4A`:

  | | vid:pid | ioreg name |
  |---|---|---|
  | ROM loader | `4C4A:2542` | `"USB Vendor Name" = "BR25UBOOT1.00"` |
  | our firmware | `4C4A:4155` | `"USB Product Name" = "USB Composite Device"` |

  Matching only the vendor id made `flash_fw.sh` announce "chip is in download mode" when the
  USB_KEY had failed and the pad had merely booted our firmware, and the bridge then died on a
  missing bulk endpoint. Note the loader's name is in **`USB Vendor Name`**, not the product
  name (and `UBOOT1.00` as reported by jl-uboot-tool comes over SCSI INQUIRY, which is a third
  place again). `flash_fw.sh` now matches that string, and falls back to "a JieLi device that
  is not pid `0x4155`" so it keeps working if the string ever differs.
- Bridge, as root (macOS only lets root detach its mass-storage driver), **one instance**:
  `sudo host/.venv-bridge/bin/python host/jl_usb_bridge.py work/jl.sock`
- Write + verify with jl-uboot-tool (macOS backend `tools/jl-uboot-tool/scsiio/macport.py`,
  client deps in `host/.venv-uboot`):
  `jluboottool.py --device <abs>/work/jl.sock "erase 0x1000 0x25000" "write 0x1000 fw_area_raw.bin" "read 0 0x80000 verify.bin"`
  then compare `verify.bin` with the expected full image (all 128 sectors); the script refuses to
  boot a firmware whose read-back does not match.
- Boot it with the tool's **`reset 1`** (`run_app`) instead of a second USB_KEY + power cycle:
  the chip leaves download mode and starts the firmware, and the Pico only sends the key when
  asked, so it does not fall back into UBOOT.
- Then Ctrl-C the bridge and unplug the Pico.

Download-mode reads/writes are **raw physical bytes** (the loader bypasses the ENC block).
Restore the stock firmware the same way from `dumps/original_physical_full.bin` (the factory
physical image, rebuilt from the SINCO dump and checked against the vendor UFW).

### Abandoned: booting our firmware next to the stock one

Repointing `app_dir_head` in sector 0 at a slot in free flash, with a launcher falling back to
the stock firmware, bricked USB: the uboot builds `boot_info` (VM start, app address) from the
directory it loaded, so the stock firmware started with our values, could not find its
resources and hung before USB init.  Recovered with the Pico.  Rule since then: either the
stock firmware or ours, never both.

## Writing flash from our own firmware

`fw/smcpad/smcpad_flash.c` erases and programs over SPI0 with the SFC suspended, every wait
bounded, all of it in `.volatile_ram_code` (RAM). Console: `F <sector> [variant]` (erase,
program, verify, then 32 trials), `X <mode> [n] [sector]` (bisection of the steps).
Measured 2026-10-19: 32/32 first-try three times, 200 erases in a row, no reset.

**The one rule that cost a day: the ROM's `sfc_drop_cache(cpu_addr, len)` (0x117FB4) may only
be called from RAM, with interrupts off and the SFC suspended** -- the way the SDK's
`flushinv_dcache_api` / `norflash_erase` do it. Called from flash code with interrupts on, an
interrupt fetching from flash during the invalidation runs garbage. Symptoms, all of which were
this and nothing else: the chip dying "about 8 s after a flash operation" (the watchdog, or the
host dropping the USB device -- macOS logs `endpoint 0x00 ... timeout` then `hardware connection
lost`), and the console dying after a few `R` commands. Readers of freshly written flash use
`smcpad_flash_drop_cache()`, which does it properly.

Post-mortem helpers left in the firmware (`fw/smcpad/smcpad_fault.c`):

- `chip_reset` (ROM 0x117fcc, where every SDK `ASSERT` and -- after LTO --
  `exception_analyze()` go) is re-pointed by `fw/build.sh` at `smcpad_chip_reset`, which records
  the caller, r0/r1/sp and 24 words at r0, then resets. The next boot prints it.
- A PC sampler (every 2 ms, plus a 100 ms ring over 6.4 s with a sample counter) in
  `SMCPAD_NOINIT`, 1 KB that `fw/build.sh` takes off the top of the heap. Only the first 8 bytes
  of the `UPDATA` block survive a reset; everything after them is cleared during boot.
- The reset-source registers read 0 by the time the app runs: something earlier clears them.

## Updating over USB, no Pico (our firmware only)

    host/.venv-serial/bin/python host/update.py [work/flash/fw_area_raw.bin]

Same image `host/flash_fw.sh` writes. ~30 s for 90 KB, no root, no download mode:

1. `ub <size>` erases a staging area in free flash (0x40000); `uw <off> <len>` + raw bytes, in
   acknowledged 1 KB chunks with a CRC32 each; `uv <size> <crc32>` checks the whole copy.
2. `uc <size> <crc32>` checks again, then `smcpad_flash_copy_and_reset()` runs from RAM with
   interrupts off and the SFC suspended for the whole job: erases 0x1000..0x26000, copies
   staging over it (reading it back over SPI0, the SFC being off), compares, records the result
   in the breadcrumb word and resets through the ROM's `chip_reset`. The watchdog is fed via the
   ROM's `p33_or_1byte(0x80, 0x40)` (what `wdt_clear` does), since `clr_wdt` lives in flash.
3. The next boot prints `update applied: copy verified` (or the failure counts); the console
   banner shows the build date and time.

Only the copy itself is unprotected: power lost during those ~2 s leaves a half-written app
area, and then only the ROM download mode (`host/flash_fw.sh` with the Pico) brings it back.

USB receive quirks found on the way (`fw/smcpad/smcpad_dbg.c`):

- The OUT endpoint is read in the USB interrupt (CDC wakeup hook), as the SDK's own example
  does, and nowhere else.
- **The host never sends full 64-byte packets**: six of them in a row stall the endpoint (the
  host's transfer just sits NAKed). Not understood; the endpoint is double-buffered and the
  SDK's bulk read does not seem written for it. `update.py` writes 63 bytes at a time.
- The SDK's bulk read keeps reading packets until it has `len` bytes and drops what does not
  fit of the last one, so the firmware asks for exactly 63.
- The first command after the host opens the port is sometimes lost: tools first send empty
  lines until the prompt comes back. `flash_fw.sh` sends `J` a second time if needed.
- A failed `uw` leaves the console waiting for the rest of the chunk for a few seconds, eating
  whatever comes meanwhile.
