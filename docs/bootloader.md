# Bootloader (fw/boot): USB DFU in front of the application

Status (2026-10-20): installed on the pad. It chains to the application, takes it over DFU
(`host/dfu_flash.sh`, about 14 s round trip, no Pico, no sudo) and survives interrupted
transfers. It is 9 KB and bare-metal: no JieLi SDK, TinyUSB 0.21 (DFU class) on our
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

Updating the application afterwards:

    SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh && host/package_fw.sh boot
    host/dfu_flash.sh                                       # work/flash/fw_area.bin over DFU

## Boot decision (fw/boot/boot.c, runs in place from flash)

The factory uboot jumps to the bootloader's entry with r0 = `BOOT_DEVICE_INFO *`. Before
touching any hardware:

- `BOOT_MAGIC_DFU` (0xB007DF00) at `0x31AE0` (the SDK's UPDATA_BEG, kept across a chip reset)
  -> clear it, DFU mode. The application sets it for the console's `B` and the 1200-baud touch.
- otherwise, the application's directory at 0x9000 is valid (header CRC, entry = 0x1E08120,
  size within the area) -> hand over; else DFU mode.

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

USB id `1209:0002` "SMC-PAD bootloader" (pid.codes test PID; the application is `1209:0001`).
DFU 1.1, 4096-byte transfers (one sector per block), manifestation tolerant:

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

It goes back to the application on DFU_DETACH (`dfu-util -e`), 1.5 s after a good
manifestation, or -- when it was only asked for and the application is valid -- if the host
has not configured it within 20 s. Before resetting it drops off the bus (PHY off, pulls off)
so the host sees the detach.

Diagnostics: what the last DFU session saw is left at `0x31AA0` (`struct boot_diag`: reason,
milliseconds, polls, USB/clock registers, events, blocks, last error) and the application's `I`
prints it.

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
