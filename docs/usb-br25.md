# BR25 USB device controller, and our TinyUSB driver

`fw/smcpad/usb/dcd_br25.c` is a TinyUSB device controller driver (`dcd_*`) for the full-speed
USB of the JieLi BR25 (AC696x/AC636x). It replaces the SDK's USB stack when the firmware is
built with `SMCPAD_TINYUSB=1 fw/build.sh`; TinyUSB itself is vendored at
`third_party/tinyusb` (release 0.21.0). The SDK build (`SMCPAD_TINYUSB=0`, the default)
keeps the JieLi stack.

Status (2026-10-20): enumerates on macOS, CDC console, firmware self-update and the hands-free
`J` flash all work over it. `host/usb_selftest.py` passes 300 commands, the TX pattern and 50
consecutive 1 KB uploads in full 64-byte packets with matching CRCs.

## The controller

A Mentor MUSB core with two JieLi additions (register details in
`tools/jielie/periph/usb-fs.md`):

- **Indirect SIE registers.** Every MUSB register (JieLi reorders the addresses: `FAddr` 0x00,
  `Power` 0x01, `IntrTx` 0x02/03, `IntrRx` 0x04/05, `IntrUSB` 0x06, enables 0x07–0x0B,
  `Index` 0x0E, `TxMaxP` 0x10, `CSR0/TxCSRL` 0x11, `TxCSRH` 0x12, `RxMaxP` 0x13, `RxCSRL` 0x14,
  `RxCSRH` 0x15, `Count0/RxCount` 0x16/0x17) is reached one byte at a time through `USB_CON1`
  (0x1E1804): `addr << 8 | data` to write, `addr << 8 | 1 << 14` to read, then wait for bit 15.
  Indexed CSRs go through the shared `Index` register, so task-context accesses must mask the
  USB interrupt (index 9) or they race the interrupt handler.
- **DMA instead of a FIFO data register.** `EP0_ADR` (0x1E181C), `EPn_TADR` (0x1E1820 +
  8(n−1)), `EPn_RADR` (0x1E1824 + 8(n−1)) hold RAM buffer addresses; `EPn_CNT` (0x1E1808 + 4n)
  starts a TX. The DMA pointers advance with every packet: rewrite the address before each TX
  and after each RX packet, as the SDK does.

Init (read off the SDK, `usb_device_mode`): `USB_IO_CON0` (0x1E5100, in the GPIO block) gets
DM/DP digital inputs on, USB IO mode on, slew rate on; `CON0` cleared, `PHY_ON`; pulls off,
D+ pull-up on; `CON0 |= NRST|TM1|CID|VBUS` (0x3C); interrupts cleared; `CON0 |= SIEIE`; EP0
DMA address; `IntrUSBE = Reset|Suspend|Resume`; `Power = SoftConn` (0x40). The clock is the
SDK's (the PHY runs off the system PLL). Reading `CON0` acknowledges the SIE interrupt.

## Measured behaviour that the driver depends on

None of this is in any datasheet we have; each item cost a debugging session.

1. **MaxP must be written as 0xFF** (as the SDK does), whatever the descriptor's packet size.
   With the real size (64) the controller double-buffers the endpoint on its own and moves
   every other packet through *address + 512*: IN packets came out of RAM 512 bytes past
   `TADR` (every other packet garbage), and RX counts went wrong.
2. **The RX FIFO still holds two packets** even with MaxP 0xFF (`RxCSRL` reads `RxPktRdy |
   FIFOFull`). In a run of full packets `RxPktRdy` can come up while the next packet is still
   streaming into the buffer: `RxCount` reads 0. Moving `RADR` or popping at that moment
   splits the packet (seen as exactly one packet per run with only its first byte wrong).
   The driver spins until the count is non-zero (≤ 400 reads; 64 bytes take ~43 µs), then
   copies, rewinds `RADR`, and pops with `FlushFIFO`. A real zero-length packet comes without
   `FIFOFull`.
3. **One buffer per direction is right**; software ping-pong of `RADR` (what the SDK's bulk
   read does) loses the first bytes of the next packet whenever the switch lands during its
   reception.
4. **EP0**: the TinyUSB MUSB driver's state machine works unchanged (ported with the FIFO
   reads replaced by `EP0_ADR`), including its two subtleties, both seen here: the host's
   STATUS OUT can complete in the same interrupt as the last DATA IN packet, and usbd (a task)
   can arm the previous transfer's STATUS OUT after the next SETUP arrived — that SETUP is
   parked until the old transfer is retired.
5. **IN and OUT on different endpoint numbers** (CDC: notify 0x81, OUT 0x02, IN 0x83). Not
   re-tested since items 1–3 were understood; kept because it costs nothing.
6. **Interrupt handler before connecting**: `request_irq` must precede `tusb_init()`, which
   enables the pull-up and the interrupt; a bus reset into an empty vector reset the chip
   mid-enumeration, and after a few of those macOS stopped looking at the port until the
   cable was replugged.

The SDK's own CDC stack suffers from 2 and 3: a run of full packets stalls or corrupts it,
which is why `host/update.py` writes 63 bytes at a time when it talks to an SDK-stack build.

## Debugging aids

- Console `L`: the driver's event log (data endpoints; EP0 events are filtered out).
- Console `T`: TX test pattern; `G`: per-64-byte-block summary of the last `uw` upload.
- **1200-baud touch** (open the port at 1200 baud, drop DTR) jumps to the mask ROM over EP0
  alone — the way to get a build with broken data endpoints into download mode without the
  power switch. `host/flash_fw.sh` tries it when `J` gets no answer.
- If the host has not configured the device within 25 s of boot, the TinyUSB build saves the
  driver log to flash sector 0x70000 ("USLG", count, entries) and jumps to the ROM like `J`;
  `host/flash_fw.sh` (which keeps the USB_KEY armed) catches it and its full read-back
  (`work/flash/verify_last.bin`) contains the log.
- macOS kernel log: `log show --predicate 'process == "kernel" AND sender == "IOUSBHostFamily"'`
  shows enumeration, control-transfer timeouts and "hardware connection lost".

The development id is pid.codes' test VID/PID `1209:0001`, product "SMC-PAD open fw".
