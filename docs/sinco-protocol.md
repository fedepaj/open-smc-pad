# SINCO / M-VAVE USB-MIDI service protocol

Reverse engineered from `M-UPGRADE` (macOS, arm64, unstripped Qt app; Ghidra project
`re/ghidra/open_smc_pad.gpr`). Verified live on SMC-PAD fw `SMC-PAD_004`.
Python implementation: [`host/sinco_proto.py`](../host/sinco_proto.py).

## Transport

- USB-MIDI port **`SMC-PAD-Private`** (the other two ports do not answer).
- 8-bit packets are bit-packed **LSB-first into 7-bit bytes** and wrapped in `F0 … F7`,
  no manufacturer ID (`u8ToMidi` / `CMIDITranfer::prepare_sysex_out_buffer`, transfer mode 1).
- Exception: "free" SysEx (`CUSBConnect::send_free_sysex`) is sent raw, unpacked.

## Packet (before 7-bit packing)

| offset | size | field |
|---|---|---|
| 0 | 2 | `00 59` (`s_arrSysexHead`, constant) |
| 2 | 1 | command |
| 3 | 3 | payload length, 24-bit LE |
| 6 | n | payload |
| 6+n | 1 | `~sum(payload) & 0xFF` |

## Commands (host → device unless noted)

| cmd | name (M-UPGRADE symbol) | payload |
|---|---|---|
| `0x11` | query name/version (`get_device_name_and_version`) | — ; reply `0x11`, 27 B: ASCII `NAME_VER` (e.g. `SMC-PAD_004`) |
| `0x21` | flash erase (`make_flash_erase_packet`) | `type u8, addr u32` |
| `0x22` | flash write (`make_flash_write_packet`) | `type u8, addr u32, len u24, data` |
| `0x23` | flash read (`make_flash_read_packet`) | `type u8, addr u32, len u24` |
| `0x24` | flash checksum (`make_flash_checksum_packet`) | `type u8, addr u32, len u32` |
| `0x30` | data request (device → host) / data reply (host → device) | `type u8, addr u32, len u24 [, data]` |

`0x21`–`0x24` are present in the app but **not called** by the upgrade flow (shared code with the
editor). Verified live on `SMC-PAD_004` (2026-10-18, [`host/sinco_flash.py probe`](../host/sinco_flash.py)):

- **`0x21` erase and `0x22` write are implemented** for `type 0` (same type as the working read).
  A reversible round-trip on the erased sector `0x76000` (read-FF → erase → write 16 B → read-back →
  erase → read-FF) confirmed both: the written pattern appeared and erase returned the sector to `0xFF`.
- `0x21`/`0x22` **do not echo the command**; they reply with a generic status packet
  `cmd 0x00`, payload `[status]`, where `status == 0x00` means success.
- `0x21` erases the **4 KB sector** containing `addr` (JieLi SFC granularity); `0x22` writes into an
  already-erased sector. Address is the raw flash offset, same space as `0x23` reads.
- `0x24` checksum still returns a refusal; other `EFlashType` values beyond 0 are not yet mapped.

## Firmware upgrade (legacy OTA-MIDI flow, `OtaUpgradeWorker::executeUpgradeProcess`)

1. Host sends raw SysEx **`F0 22 24 35 7F F7`** ("upgrade mode command"). **Reboots the device into
   update mode — not tested yet.**
2. Device drives the transfer with `0x30` requests `[type][addr][len]`; host answers with `0x30`
   carrying `len` bytes of the *de-interleaved* UFW image at `addr` (bounds-checked).
3. Special request addresses:
   - `0xCxxxxxxx` — diagnostics / state callbacks (`0xC1`: state, stu, err; `0xC2`: stop + error); host acks with `"success"`.
   - `0xD0xxxxxx` — validation failure; low byte `0x83` = loader validation failed, `0x97` = identical firmware.
   - `0xE0000000` — verification complete → device re-enumerates for the transfer step
     (AC636: **HID loader `4D4A:4155`, JieLi RCSP-over-HID**, `JieliHidOta::upgradeFirmware`;
     others: MIDI device named `ota-<NAME>`).
   - `0xF0000000` — upgrade complete.
4. Host re-queries `0x11` to verify the new version.

Hidden UI shortcut **Y+U+I+O** enables force mode (skips model/version checks).

## `.fwsc` container (`OtaFormat::parse` / `pack`)

A standard JieLi **UFW** image (what the SDK emits as `update.ufw`) with metadata interleaved:
the file is split in 0x30-byte blocks; for the first N blocks (N = 36, else 20) byte 0x2F is a
metadata char `c + i` (i = 1-based block index) spelling `NAME_VERSION` and terminated by `0x7D`.
Removing those bytes yields the UFW. `ref/smk37-firmware-custom-mod/tools/unpack_smk.py` decodes
the UFW further (chipkey `0x980F`, app at `0x1E00120`).
