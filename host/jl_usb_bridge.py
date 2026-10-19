#!/usr/bin/env python3
"""SCSI pass-through bridge for a JieLi chip in USB download mode (UBOOT1.00), for macOS.

macOS binds its mass-storage driver to the chip and only root may detach it, so this small
helper runs as root, takes the USB interface of the JieLi device (VID 0x4C4A) and nothing
else, and relays raw SCSI commands (USB Bulk-Only Transport) from a Unix socket.  Everything
else -- jl-uboot-tool, the loader, flash reads/writes, verification -- runs unprivileged and
talks to the socket through tools/jl-uboot-tool/scsiio/macport.py.

    sudo host/.venv-bridge/bin/python host/jl_usb_bridge.py /path/to/jl.sock

Ctrl-C to stop; the interface is released and macOS gets the device back.

Wire protocol (all little endian):
    request : u8 dir (0 none, 1 out, 2 in) | u8 cdb_len | u32 data_len | u32 timeout_ms
              | cdb | data_out (dir 1)
    reply   : u8 status (0 good, 1 check condition, 2 transport error) | u32 residue
              | u32 n | data_in (n bytes) | u32 m | sense (m bytes)
"""
import os
import socket
import struct
import sys

import libusb_package
import usb.core
import usb.util

JL_VID = 0x4C4A
OUR_FW_PID = 0x4155     # our own firmware's CDC, same vendor id as the loader


class BulkOnly:
    def __init__(self):
        be = libusb_package.get_libusb1_backend()
        self.dev = usb.core.find(idVendor=JL_VID, backend=be)
        if self.dev is None:
            sys.exit("no JieLi device (VID 0x4C4A) found -- is the chip in UBOOT mode?")
        product = self.dev.product or "?"
        print(f"found {self.dev.idVendor:04x}:{self.dev.idProduct:04x} {product}")

        # JieLi's vendor id is shared: the ROM loader uses it, and so does our own firmware's
        # CDC. Our firmware is 4C4A:4155 and speaks CDC, not mass storage, so catch that case
        # by its product id rather than failing on a missing endpoint further down. (The
        # loader's "UBOOT1.00" name comes over SCSI INQUIRY, not from the USB descriptor, so
        # there is nothing in the descriptor to match on.)
        if self.dev.idProduct == OUR_FW_PID:
            sys.exit(f"{self.dev.idVendor:04x}:{self.dev.idProduct:04x} is our own firmware "
                     f"running normally, not the ROM loader.\n"
                     f"  Power the pad off and send the USB_KEY as it powers on.")

        self.detached = False
        if self.dev.is_kernel_driver_active(0):
            self.dev.detach_kernel_driver(0)
            self.detached = True
        usb.util.claim_interface(self.dev, 0)
        intf = self.dev.get_active_configuration()[(0, 0)]
        eps = {usb.util.endpoint_direction(e.bEndpointAddress): e for e in intf}
        if usb.util.ENDPOINT_OUT not in eps or usb.util.ENDPOINT_IN not in eps:
            sys.exit("this device has no bulk in/out pair on interface 0, so it is not "
                     "the mass-storage loader")
        self.ep_out = eps[usb.util.ENDPOINT_OUT]
        self.ep_in = eps[usb.util.ENDPOINT_IN]
        self.tag = 0

    def close(self):
        usb.util.release_interface(self.dev, 0)
        if self.detached:
            try:
                self.dev.attach_kernel_driver(0)
            except usb.core.USBError:
                pass

    def _csw(self, timeout):
        try:
            raw = bytes(self.ep_in.read(13, timeout=timeout))
        except usb.core.USBError as e:
            if e.errno != 32:                       # not a stall
                raise
            self.dev.clear_halt(self.ep_in)
            raw = bytes(self.ep_in.read(13, timeout=timeout))
        sig, tag, residue, status = struct.unpack("<4sIIB", raw)
        if sig != b"USBS" or tag != self.tag:
            raise IOError(f"bad CSW {raw.hex(' ')}")
        return residue, status

    def command(self, cdb, direction, data_len, data_out, timeout):
        self.tag = (self.tag + 1) & 0xFFFFFFFF
        flags = 0x80 if direction == 2 else 0x00
        cbw = struct.pack("<4sIIBBB", b"USBC", self.tag, data_len, flags, 0, len(cdb))
        self.ep_out.write(cbw + cdb.ljust(16, b"\0"), timeout=timeout)
        data_in = b""
        try:
            if direction == 1 and data_len:
                self.ep_out.write(data_out, timeout=timeout)
            elif direction == 2 and data_len:
                data_in = bytes(self.ep_in.read(data_len, timeout=timeout))
        except usb.core.USBError as e:
            if e.errno != 32:
                raise
            self.dev.clear_halt(self.ep_out if direction == 1 else self.ep_in)
        residue, status = self._csw(timeout)
        sense = b""
        if status == 1:                             # fetch REQUEST SENSE for diagnostics
            sense_cdb = bytes([0x03, 0, 0, 0, 18, 0])
            self.tag = (self.tag + 1) & 0xFFFFFFFF
            cbw = struct.pack("<4sIIBBB", b"USBC", self.tag, 18, 0x80, 0, 6)
            self.ep_out.write(cbw + sense_cdb.ljust(16, b"\0"), timeout=timeout)
            sense = bytes(self.ep_in.read(18, timeout=timeout))
            self._csw(timeout)
        return status, residue, data_in, sense


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("client gone")
        buf += chunk
    return buf


def serve(path):
    bot = BulkOnly()
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    os.chmod(path, 0o666)                           # the unprivileged client connects to it
    srv.listen(1)
    print(f"listening on {path} (Ctrl-C to stop)")
    try:
        while True:
            conn, _ = srv.accept()
            print("client connected")
            try:
                while True:
                    direction, cdb_len, data_len, timeout = struct.unpack("<BBII", recv_exact(conn, 10))
                    cdb = recv_exact(conn, cdb_len)
                    data_out = recv_exact(conn, data_len) if direction == 1 else b""
                    try:
                        status, residue, data_in, sense = bot.command(cdb, direction, data_len,
                                                                      data_out, timeout)
                    except (usb.core.USBError, IOError) as e:
                        print(f"  transport error on cdb {cdb.hex(' ')}: {e}")
                        status, residue, data_in, sense = 2, data_len, b"", b""
                    conn.sendall(struct.pack("<BII", status, residue, len(data_in)) + data_in +
                                 struct.pack("<I", len(sense)) + sense)
            except ConnectionError:
                print("client disconnected")
            finally:
                conn.close()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        os.unlink(path)
        bot.close()
        print("released the device")


if __name__ == "__main__":
    if os.geteuid() != 0:
        sys.exit("run me with sudo: macOS only lets root detach its mass-storage driver")
    serve(sys.argv[1] if len(sys.argv) > 1 else "/tmp/jl-uboot.sock")
