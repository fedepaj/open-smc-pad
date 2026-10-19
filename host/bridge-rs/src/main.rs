//! smcpad-usb-bridge: relay raw SCSI commands to a JieLi chip in ROM download mode.
//!
//! In download mode the BR25's mask ROM enumerates as a USB mass-storage device ("BR25UBOOT1.00",
//! 4C4A:2542) and is driven with vendor SCSI commands. macOS binds its mass-storage driver to it
//! and only root may detach that driver, so this small helper does exactly that and nothing
//! else: it claims the JieLi interface and relays Bulk-Only Transport commands from a Unix
//! socket. jl-uboot-tool and everything else run unprivileged and talk to the socket through
//! tools/jl-uboot-tool/scsiio/macport.py -- the wire protocol is the one host/jl_usb_bridge.py
//! used, unchanged.
//!
//! It is meant to be installed root-owned and allowed in sudoers without a password (see
//! host/install-bridge.sh), so it is built to be harmless in that position:
//!  - it takes no arguments and touches one fixed path, /var/run/smcpad/jl.sock, in a directory
//!    it creates root-owned and refuses to use if someone else made it;
//!  - it only opens JieLi devices, and refuses our own firmware (4C4A:4155, a CDC device);
//!  - libusb is linked in statically, so nothing is loaded from a user-writable prefix;
//!  - it exits by itself after IDLE_EXIT without a client, since an unprivileged caller
//!    cannot signal a root process.
//!
//! Wire protocol (little endian):
//!   request: u8 dir (0 none, 1 out, 2 in) | u8 cdb_len | u32 data_len | u32 timeout_ms
//!            | cdb | data_out (dir 1)
//!   reply:   u8 status (0 good, 1 check condition, 2 transport error) | u32 residue
//!            | u32 n | data_in (n bytes) | u32 m | sense (m bytes)

use rusb::{Context, Device, DeviceHandle, Direction, TransferType, UsbContext};
use std::fs;
use std::io::{self, Read, Write};
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::net::{UnixListener, UnixStream};
use std::process::exit;
use std::thread::sleep;
use std::time::{Duration, Instant};

const JL_VID: u16 = 0x4C4A;
const OUR_FW_PID: u16 = 0x4155;
const SOCK_DIR: &str = "/var/run/smcpad";
const SOCK_PATH: &str = "/var/run/smcpad/jl.sock";
const FIND_TIMEOUT: Duration = Duration::from_secs(10);
const IDLE_EXIT: Duration = Duration::from_secs(10);
const MAX_CDB: usize = 16;
const MAX_DATA: u32 = 1 << 20;

#[derive(Debug)]
#[allow(dead_code)] // the payloads are read through Debug when printed
enum Error {
    Usb(rusb::Error),
    Protocol(String),
}

impl From<rusb::Error> for Error {
    fn from(e: rusb::Error) -> Self {
        Error::Usb(e)
    }
}

struct BulkOnly {
    handle: DeviceHandle<Context>,
    ep_in: u8,
    ep_out: u8,
    tag: u32,
    detached: bool,
}

impl BulkOnly {
    fn open(device: Device<Context>) -> Result<Self, Error> {
        let handle = device.open()?;
        let detached = match handle.kernel_driver_active(0) {
            Ok(true) => {
                handle.detach_kernel_driver(0)?;
                true
            }
            _ => false,
        };
        handle.claim_interface(0)?;

        let config = device.active_config_descriptor()?;
        let (mut ep_in, mut ep_out) = (None, None);
        if let Some(iface) = config.interfaces().find(|i| i.number() == 0) {
            if let Some(alt) = iface.descriptors().next() {
                for ep in alt.endpoint_descriptors() {
                    if ep.transfer_type() != TransferType::Bulk {
                        continue;
                    }
                    match ep.direction() {
                        Direction::In => ep_in = Some(ep.address()),
                        Direction::Out => ep_out = Some(ep.address()),
                    }
                }
            }
        }
        match (ep_in, ep_out) {
            (Some(ep_in), Some(ep_out)) => Ok(BulkOnly { handle, ep_in, ep_out, tag: 0, detached }),
            _ => Err(Error::Protocol("interface 0 has no bulk in/out pair: not the ROM loader".into())),
        }
    }

    fn close(self) {
        let _ = self.handle.release_interface(0);
        if self.detached {
            let _ = self.handle.attach_kernel_driver(0);
        }
    }

    fn read_csw(&mut self, timeout: Duration) -> Result<(u32, u8), Error> {
        let mut csw = [0u8; 13];
        let n = match self.handle.read_bulk(self.ep_in, &mut csw, timeout) {
            Err(rusb::Error::Pipe) => {
                self.handle.clear_halt(self.ep_in)?;
                self.handle.read_bulk(self.ep_in, &mut csw, timeout)?
            }
            other => other?,
        };
        let tag = u32::from_le_bytes([csw[4], csw[5], csw[6], csw[7]]);
        if n != 13 || &csw[0..4] != b"USBS" || tag != self.tag {
            return Err(Error::Protocol(format!("bad CSW {:02x?}", &csw[..n])));
        }
        Ok((u32::from_le_bytes([csw[8], csw[9], csw[10], csw[11]]), csw[12]))
    }

    fn send_cbw(&mut self, cdb: &[u8], data_len: u32, data_in: bool, timeout: Duration) -> Result<(), Error> {
        self.tag = self.tag.wrapping_add(1);
        let mut cbw = [0u8; 31];
        cbw[0..4].copy_from_slice(b"USBC");
        cbw[4..8].copy_from_slice(&self.tag.to_le_bytes());
        cbw[8..12].copy_from_slice(&data_len.to_le_bytes());
        cbw[12] = if data_in { 0x80 } else { 0x00 };
        cbw[14] = cdb.len() as u8;
        cbw[15..15 + cdb.len()].copy_from_slice(cdb);
        self.handle.write_bulk(self.ep_out, &cbw, timeout)?;
        Ok(())
    }

    /// One SCSI command. Returns (status, residue, data_in, sense).
    fn command(&mut self, cdb: &[u8], dir: u8, data_len: u32, data_out: &[u8], timeout: Duration)
        -> Result<(u8, u32, Vec<u8>, Vec<u8>), Error>
    {
        self.send_cbw(cdb, data_len, dir == 2, timeout)?;

        let mut data_in = Vec::new();
        if dir == 1 && data_len > 0 {
            match self.handle.write_bulk(self.ep_out, data_out, timeout) {
                Err(rusb::Error::Pipe) => self.handle.clear_halt(self.ep_out)?,
                other => {
                    other?;
                }
            }
        } else if dir == 2 && data_len > 0 {
            data_in = vec![0u8; data_len as usize];
            match self.handle.read_bulk(self.ep_in, &mut data_in, timeout) {
                Ok(n) => data_in.truncate(n),
                Err(rusb::Error::Pipe) => {
                    self.handle.clear_halt(self.ep_in)?;
                    data_in.clear();
                }
                Err(e) => return Err(e.into()),
            }
        }

        let (residue, status) = self.read_csw(timeout)?;
        let mut sense = Vec::new();
        if status == 1 {
            // REQUEST SENSE, for the caller's diagnostics
            self.send_cbw(&[0x03, 0, 0, 0, 18, 0], 18, true, timeout)?;
            sense = vec![0u8; 18];
            let n = self.handle.read_bulk(self.ep_in, &mut sense, timeout)?;
            sense.truncate(n);
            self.read_csw(timeout)?;
        }
        Ok((status, residue, data_in, sense))
    }
}

fn find_loader(ctx: &Context) -> Option<Device<Context>> {
    let deadline = Instant::now() + FIND_TIMEOUT;
    loop {
        if let Ok(list) = ctx.devices() {
            for dev in list.iter() {
                let Ok(desc) = dev.device_descriptor() else { continue };
                if desc.vendor_id() != JL_VID {
                    continue;
                }
                if desc.product_id() == OUR_FW_PID {
                    eprintln!("4c4a:4155 is our own firmware running normally, not the ROM loader");
                    continue;
                }
                println!("found {:04x}:{:04x}", desc.vendor_id(), desc.product_id());
                return Some(dev);
            }
        }
        if Instant::now() > deadline {
            return None;
        }
        sleep(Duration::from_millis(200));
    }
}

/// The socket directory must be ours: root-owned, a real directory, not a symlink planted by
/// someone else -- we create and chmod a socket in it as root.
fn prepare_socket_dir() -> io::Result<()> {
    match fs::symlink_metadata(SOCK_DIR) {
        Ok(meta) => {
            if !meta.file_type().is_dir() || meta.uid() != 0 {
                return Err(io::Error::new(io::ErrorKind::PermissionDenied,
                    format!("{SOCK_DIR} exists but is not a root-owned directory; refusing")));
            }
        }
        Err(_) => fs::create_dir(SOCK_DIR)?,
    }
    fs::set_permissions(SOCK_DIR, fs::Permissions::from_mode(0o755))?;
    match fs::symlink_metadata(SOCK_PATH) {
        Ok(meta) if meta.file_type().is_dir() => {
            return Err(io::Error::new(io::ErrorKind::Other, "socket path is a directory"));
        }
        Ok(_) => fs::remove_file(SOCK_PATH)?,
        Err(_) => {}
    }
    Ok(())
}

fn read_exact_or_eof(stream: &mut UnixStream, buf: &mut [u8]) -> io::Result<bool> {
    match stream.read_exact(buf) {
        Ok(()) => Ok(true),
        Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => Ok(false),
        Err(e) => Err(e),
    }
}

/// Serve one client until it disconnects. Returns false if the USB device is gone.
fn serve(bot: &mut BulkOnly, mut stream: UnixStream) -> bool {
    loop {
        let mut hdr = [0u8; 10];
        match read_exact_or_eof(&mut stream, &mut hdr) {
            Ok(true) => {}
            _ => return true,
        }
        let dir = hdr[0];
        let cdb_len = hdr[1] as usize;
        let data_len = u32::from_le_bytes([hdr[2], hdr[3], hdr[4], hdr[5]]);
        let timeout = Duration::from_millis(u32::from_le_bytes([hdr[6], hdr[7], hdr[8], hdr[9]]) as u64);
        if dir > 2 || cdb_len == 0 || cdb_len > MAX_CDB || data_len > MAX_DATA {
            eprintln!("malformed request, dropping the client");
            return true;
        }
        let mut cdb = vec![0u8; cdb_len];
        if !matches!(read_exact_or_eof(&mut stream, &mut cdb), Ok(true)) {
            return true;
        }
        let mut data_out = Vec::new();
        if dir == 1 {
            data_out = vec![0u8; data_len as usize];
            if !matches!(read_exact_or_eof(&mut stream, &mut data_out), Ok(true)) {
                return true;
            }
        }

        let (status, residue, data_in, sense, device_gone) =
            match bot.command(&cdb, dir, data_len, &data_out, timeout) {
                Ok((s, r, d, sn)) => (s, r, d, sn, false),
                Err(e) => {
                    eprintln!("transport error on cdb {:02x?}: {:?}", cdb, e);
                    let gone = matches!(e, Error::Usb(rusb::Error::NoDevice));
                    (2, data_len, Vec::new(), Vec::new(), gone)
                }
            };

        let mut reply = Vec::with_capacity(13 + data_in.len() + sense.len());
        reply.push(status);
        reply.extend_from_slice(&residue.to_le_bytes());
        reply.extend_from_slice(&(data_in.len() as u32).to_le_bytes());
        reply.extend_from_slice(&data_in);
        reply.extend_from_slice(&(sense.len() as u32).to_le_bytes());
        reply.extend_from_slice(&sense);
        if stream.write_all(&reply).is_err() {
            return !device_gone;
        }
        if device_gone {
            return false;
        }
    }
}

fn main() {
    if std::env::args().len() > 1 {
        eprintln!("usage: sudo smcpad-usb-bridge   (no arguments; socket at {SOCK_PATH})");
        exit(2);
    }

    let ctx = match Context::new() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("libusb: {e}");
            exit(1);
        }
    };
    let Some(device) = find_loader(&ctx) else {
        eprintln!("no JieLi ROM loader (4c4a, not 4155) on the bus");
        exit(1);
    };
    let mut bot = match BulkOnly::open(device) {
        Ok(b) => b,
        Err(Error::Usb(rusb::Error::Access)) => {
            eprintln!("access denied: detaching macOS's mass-storage driver needs root");
            exit(1);
        }
        Err(e) => {
            eprintln!("cannot open the loader: {e:?}");
            exit(1);
        }
    };

    if let Err(e) = prepare_socket_dir() {
        eprintln!("{e}");
        bot.close();
        exit(1);
    }
    let listener = match UnixListener::bind(SOCK_PATH) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("bind {SOCK_PATH}: {e}");
            bot.close();
            exit(1);
        }
    };
    let _ = fs::set_permissions(SOCK_PATH, fs::Permissions::from_mode(0o666));
    let _ = listener.set_nonblocking(true);
    println!("listening on {SOCK_PATH}; exits after {}s without a client", IDLE_EXIT.as_secs());

    let mut idle_since = Instant::now();
    loop {
        match listener.accept() {
            Ok((stream, _)) => {
                let _ = stream.set_nonblocking(false);
                println!("client connected");
                let alive = serve(&mut bot, stream);
                println!("client disconnected");
                if !alive {
                    eprintln!("the device went away");
                    break;
                }
                idle_since = Instant::now();
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                if idle_since.elapsed() > IDLE_EXIT {
                    break;
                }
                sleep(Duration::from_millis(50));
            }
            Err(e) => {
                eprintln!("accept: {e}");
                break;
            }
        }
    }

    let _ = fs::remove_file(SOCK_PATH);
    bot.close();
    println!("released the device");
}
