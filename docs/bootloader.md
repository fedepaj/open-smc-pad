# Bootloader (fw/boot): USB DFU in front of the application

Status (2026-10-20): installed on the pad. It chains to the application, takes it over DFU
(`host/dfu_flash.sh`, about 14 s round trip, no Pico, no sudo), survives interrupted
transfers, and installs on a stock pad without the Pico (`host/install_from_stock.py`). It is 9 KB and bare-metal: no JieLi SDK, TinyUSB 0.21 (DFU class) on our
`fw/smcpad/usb/dcd_br25.c` driver, polled.

## Layout

| flash | content |
|---|---|
| `0x00000` | factory sector 0: flash header, uboot.boot, isd_config.ini (never touched) |
| `0x01000` | **bootloader** app area: directory + `boot.bin` (entry `0x1E00120`) + cfg_tool.bin |
| `0x09000` | **application** app area: directory + app.bin (entry `0x1E08120`) + cfg_tool.bin |
| `0x71000` | VM, 0x4000 (both directories declare it here) |
| `0x75000` | BTIF, EXIF, USRFLASH, USRTRIM, key_mac (factory, untouched) |

The application has 0x9000..0x71000 (416 KB). Both directories declare the VM at 0x71000: the
SFC decrypts everything below `vm_saddr` (docs/flash-layout.md), so bootloader and application
both stay inside the encryption window in every boot phase.

Build and install (once, with the Pico; afterwards DFU):

    fw/boot/build.sh                                        # -> fw/boot/out/boot.bin
    SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh        # the application, linked for 0x9000
    host/package_fw.sh boot                                 # both areas -> work/flash/
    host/flash_fw.sh                                        # download mode, as before

Or, on a pad that only has the stock firmware (no Pico, no sudo; one power cycle by hand):

    host/package_fw.sh boot
    uv run --with mido --with python-rtmidi host/install_from_stock.py

Updating the application afterwards:

    SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh && host/package_fw.sh boot
    host/dfu_flash.sh                                       # work/flash/fw_area.bin over DFU

## Updating the bootloader itself (DFU alt 2) -- no Pico

    fw/boot/build.sh && host/package_fw.sh boot     # also builds work/flash/boot_update.bin
    host/dfu_flash.sh --bootloader                   # ~40 s, verifies the version at the end

The package (`host/bootpkg.py`) is the plaintext image of 0x41000..: the installer's app area
(fw/boot/install.c) and the staging area (header + the new bootloader's app area, erase_end =
0x9000). The bootloader writes it (installer part encrypted), and only once **all** of it checks
out -- installer directory + area CRC, staging header CRC, payload CRC32, payload directory +
area CRC -- points sector 0 at the installer and resets. The installer rewrites 0x1000..0x9000
(the application stays), points sector 0 back, erases the slot, resets: new bootloader, same
application. Verified: 0102 -> 0103 and 0106 -> 0107 by DFU; packages with one flipped bit in
the installer or in the payload are refused at the manifestation (dfuERROR, "programmed memory
failed verification") and nothing else changes. The risky windows are the two sector-0 writes
(tens of ms each), as for the install from the stock firmware.

The version is not in the USB descriptors: `dfu-util -a 2 -U f` returns
`smcpad-boot <version> <build date>`. **The descriptors never change between versions**: on
Apple silicon macOS ("allow accessories to connect") takes a device whose descriptors changed
for a new accessory and leaves it `!registered` until the user approves it (or unlocks the
Mac) -- bcdDevice 0x0104 and 0x0105 made the updated bootloader invisible to dfu-util, the same
code as 0x0103 was fine. Same rule for the application and the Arduino core.

The one deliberate exception is 0x0108, for Windows: dfu-util talks to the device through libusb,
which on Windows needs the WinUSB driver bound to it, and Windows binds nothing to a DFU-mode
device by itself (hence Zadig, for most DFU boards). 0x0108 announces USB 2.1 (bcdUSB 0x0210)
and a BOS descriptor with the Microsoft OS 2.0 platform capability (vendor code 1); Windows then
asks for the descriptor set (vendor request 1, wIndex 7) and gets: compatible ID `WINUSB`, and
`DeviceInterfaceGUIDs` = `{7d279a71-4e27-4bcb-9ca9-6a8c93b56630}`. WinUSB binds at the first
plug-in, no driver install, no Zadig (fw/boot/usb_desc.c). macOS asks to allow the "new"
accessory once after the update to 0x0108; nothing else changes. The application (CDC + MIDI)
needs nothing: Windows has class drivers for both.

## Hardening: a broken application must not lock the pad out

Besides "no valid directory" and the application's own request (`B`, 1200-baud touch), the
bootloader stays in DFU mode when:

- **SHIFT + PLAY are held at power-up** -- it scans those two key-matrix columns itself
  (bit-banged 595s, sense on PC1; a sense line that reads low with no column selected is
  ignored) and puts every GPIO register it touched back;
- **the application crash-loops**: every boot increments a counter at 0x31AD8 (with a check
  word, so power-up RAM reads as 0); the application clears it after ~3 s of running
  (`smcpad_boot_ok()` in the console; the Arduino core must do the same). The 4th boot in a
  row that was never confirmed stays in DFU mode. Verified with quick `Z` resets: reason 4;
- **the application area fails its CRC** (the whole area against the directory's data_crc,
  table-free CRC16 in `.start`, a few tens of ms at boot).

In DFU mode the SHIFT button's LED blinks. The reason is in the diagnostics (`boot.magic`
low digit: 1 asked, 2 no app, 3 keys, 4 crash loop, 5 bad CRC).

Still open:
- block-protecting 0x0..0x8000 in the flash status register. Measured (console `S`): with our
  SDK application running SR1 = SR2 = 0, i.e. nothing is protected (the SDK's "protection below
  vm_saddr" is a software check, not the BP bits). Setting BP from the bootloader needs the BP
  semantics of this flash (JEDEC id 0xCD7013, not a common vendor) confirmed first, and every
  writer of sector 0 / 0x1000 (installer, alt 2) to unprotect around its write -- the
  installer already does.
- power cuts at random points of a DFU transfer and of a bootloader update (hands on the
  switch).
- the install from the stock firmware after the jlfs.c refactor (same installer logic; needs
  a power cycle by hand to re-run).

## Boot decision (fw/boot/boot.c, runs in place from flash)

The factory uboot jumps to the bootloader's entry with r0 = `BOOT_DEVICE_INFO *`. Before
touching any hardware:

- `BOOT_MAGIC_DFU` (0xB007DF00) at `0x31AE0` (the SDK's UPDATA_BEG, kept across a chip reset)
  -> clear it, DFU mode. The application sets it for the console's `B` and the 1200-baud touch.
- no valid directory at 0x9000 (header CRC, entry = 0x1E08120, size within the area), SHIFT +
  PLAY held, a crash loop, or a failed area CRC -> DFU mode (see "Hardening");
- otherwise hand over.

**Handing over** = what the uboot does for a directory elsewhere: the SDK walks its directory at
CPU `app_addr` and converts addresses with `flash = cpu - app_addr + sfc_base_addr`
(`sdfile_init` / `sdfile_cpu_addr2flash_addr`), so the bootloader sets `sfc_base_addr = 0x9000`,
`app_addr = 0x1E08000` in the uboot's struct and jumps to the entry with r0 unchanged. The SFC
mapping itself stays flash 0x1000 = CPU 0x1E00000. Verified with the application's `I` command.

## DFU mode (fw/boot/dfu.c, flash.c, runs from RAM)

`dfu_start` moves to its own stack (RAM top 0x30000), copies `.ram` (all code but `.start`,
data) to 0x12000, clears `.bss`, and calls `dfu_main` through a register (RAM is out of reach of
a direct call from flash). From then on nothing executes from flash: the SFC is parked once and
SPI0 talks to the flash directly (same register sequence as `fw/smcpad/smcpad_flash.c`, every
wait bounded), interrupts stay off, USB is polled (`dcd_int_handler` + `tud_task` in a loop),
timer 0 on the 24 MHz oscillator is the millisecond clock, the watchdog is fed through the ROM's
`p33_or_1byte`. USB came up on the uboot's clock setup as it is (CLK_CON1 = 0x80000000,
PLL_CON = 0x3440002B).

USB id `1209:0002` "SMC-PAD bootloader" (pid.codes test PID; the application is `1209:0001`),
bcdDevice fixed at 0x0100. DFU 1.1, 4096-byte transfers (one sector per block), manifestation
tolerant, three alternates:

- **alt 0 "app"**: the application's app area, plaintext (`work/flash/fw_area.bin`). The
  bootloader encrypts with the chip key from `BOOT_DEVICE_INFO.chip_id`, so the same file fits
  any pad. Block 0 (the directory) is checked and kept in RAM and its sector is erased at once;
  the other blocks are erased, programmed and read back; at manifestation the area's CRC (the
  directory's `data_crc`) must match, and only then is the directory written. An interrupted
  transfer therefore leaves no valid application and the next boot stays in DFU mode
  (verified: killed at 16 %, detach, the bootloader came back with reason "no app").
  Upload returns the area decrypted: `dfu-util -U` gives back the downloaded file byte for byte.
- **alt 1 "flash"**: read-only, the whole 512 KB raw, identical to a download-mode read
  (a backup without the Pico).
- **alt 2 "bootloader"**: download = bootloader update package, upload = the version line
  (see "Updating the bootloader itself").

It goes back to the application on DFU_DETACH (`dfu-util -e`), 1.5 s after a good
manifestation, or -- when it was only asked for and the application is valid -- if the host
has not configured it within 20 s. Before resetting it drops off the bus (PHY off, pulls off)
so the host sees the detach.

Diagnostics: what the last DFU session saw is left at `0x31AA0` (`struct boot_diag`: reason,
milliseconds, polls, USB/clock registers, events, blocks, last error) and the application's `I`
prints it.

## Installing from the stock firmware (host/install_from_stock.py, fw/boot/install.c)

Verified 2026-10-20 on a pad restored to the factory image: stock -> installer -> bootloader ->
application in one run, the only manual step a power cycle.

1. Over USB-MIDI the stock firmware writes (SINCO 0x21/0x22, 1 KB pieces, every byte read back):
   the installer as an app area at 0x41000 and a staging area at 0x43000 (header with CRC32s +
   the bootloader's app area in plaintext). Then sector 0's `app_dir_head` is pointed at
   0x41000: the stock firmware reads sector 0 XOR a fixed ENC keystream (key 0xFFFF,
   restarting every 256 bytes -- the same on every chip) and writes it raw, so the physical
   sector is known exactly and only the one 32-byte entry changes (offset + CRC16, each entry
   ENC-ciphered with 0xFFFF on its own). On a mismatch the original sector goes back.
2. Power cycle: the uboot runs the installer (from RAM, SFC parked). It checks the staging
   area; if it does not hold up it points sector 0 back at 0x1000 and the untouched stock
   firmware boots again. Otherwise it erases 0x1000..0x26000, writes the bootloader encrypted
   with the chip key, points sector 0 back at 0x1000 (the only window -- tens of ms -- where
   losing power needs the Pico), erases its own area and resets. Interrupted before that, it
   simply runs again at the next boot. Afterwards sector 0 is byte for byte the factory one.
3. The bootloader finds no application, comes up in DFU mode, and the script runs
   host/dfu_flash.sh.

The installer leaves a record at 0x4C000 ("IDG1", step << 4 | result, SR1/SR2 before and after
its unprotect, result of the unprotect; 0x3F = done) and a breadcrumb in RAM at 0x31AD0.
`host/flash_fw.sh dump` / `cmd "memread ..."` read them in download mode.

**How the uboot boots a slot** (read off `sfc_mode_boot` in the user_boot SDK's uboot.a, then
confirmed): `app_addr = entry & 0xFFFF0000`, `sfc_base_addr = app_dir_head`, and the SFC's
`BASE_ADR = sfc_base_addr + 0x1000000 - app_addr`, i.e. the CPU address `entry & ~0xFFFF` is
mapped onto the slot's directory. Code in a slot must therefore be linked at 0x1E00120 like the
stock firmware -- not at "its" address (the first installer, linked at 0x1E5F120 for 0x60000,
was entered at flash 0x6F120: reset loop; this is probably also what bricked the day-1 slot).
The slot is 0x41000 because 0x41000 - 0x1000 is a multiple of 0x40000: the SFC key uses the
low 16 bits of addr >> 2, so the encryption is the same whether the hardware keys on the CPU or
on the flash address. (The bootloader's own hand-over to 0x9000 does not go through this: the
mapping set up for the bootloader stays, and the application is linked at 0x1E08120.)

## Pitfalls met

- **IPRA and weak functions (JieLi clang 4.0.1).** The compiler's interprocedural register
  allocation trusts the body of a *weak* default (TinyUSB's `tud_dfu_upload_cb`, which touches
  nothing) and keeps r2 live across the call; the strong definition linked in its place clobbers
  r2, and `tud_control_xfer` then sent a stale buffer -- uploads came back as zeros while every
  descriptor was fine. Fixed with `-mllvm -enable-ipra=false` in fw/boot/build.sh. The SDK build
  is LTO, where the strong definition is visible, so it is not affected; **any non-LTO build with
  weak callbacks (the Arduino core) needs the flag**.
- The JieLi `objcopy` takes exactly one `-j`: `.start` and `.ram` are dumped separately and
  concatenated (boot.ld asserts they are contiguous).
- `.start` code must call only `.start` code and use no `.rodata` (no switches, no tables, no
  memcpy-able loops): until the copy, nothing else is in RAM.
- Two DFU alternates make `dfu-util -e` complain about "more than one device": give `-a 0`.

## Related change in the application

The bring-up console used to run inside the SDK's `app_core` task and never return, so that
task never took its event queue again; `sys_event_task` waits for every event it posts to be
taken and calls `chip_reset()` when its semaphore times out -- the pad rebooted every ~40 s
unless the console was being kept busy (reset record: caller in `sys_event_task`, r0 = 11 =
the timeout). The console now has its own task (`smcpad_con`, app_idle.c / app_main.c).
