/*
 * Bring-up debug channel for the open SMC-PAD firmware: a USB CDC serial port.
 *
 * The board gives us nowhere to log: the SDK's debug UART is UART0, whose TX pins on this
 * chip are PA05/PB00/PB05/PC03 (all taken by the LED rows, the 595s and the key scan, and
 * none of them reachable without soldering), and the first images came up frozen with no way
 * to ask them why.  USB is already cabled to the host, so the firmware exposes a CDC serial
 * and talks over that; on macOS it shows up as /dev/cu.usbmodem*.
 *
 * The catch is that USB only comes up once the app is running, which is later than the
 * interesting part of the boot.  So the early milestones go into a small RAM trace with
 * smcpad_trace(), and the whole trace is pushed out the moment the host opens the port --
 * the log of a boot that happened before the log channel existed.  If nothing ever arrives,
 * the firmware never reached the app, which is itself the answer.
 *
 * Nothing here can block: cdc_write_data() returns 0 while the port is unconfigured.
 * smcpad_trace() is safe before the kernel starts (it only touches a static buffer);
 * the write side needs the OS (CDC takes a mutex), so only call it from the app.
 */
#include "system/includes.h"
#include "app_config.h"
#include "smcpad_usb_cfg.h"
#include "asm/hwi.h"
#if SMCPAD_TINYUSB
#include "tusb.h"
#else
#include "usb/device/usb_stack.h"
#include "usb_config.h"
#include "cdc.h"

/* The debug channel is the whole point of this file: if the USB device or the CDC class got
 * configured out, the firmware would ship silent and we would burn a flash cycle to find out. */
#if !TCFG_PC_ENABLE
#error "SMCPAD: TCFG_PC_ENABLE is off -- board config not reaching usb_common_def.h"
#endif
#if !TCFG_USB_SLAVE_ENABLE
#error "SMCPAD: USB slave disabled"
#endif
#if !TCFG_USB_SLAVE_CDC_ENABLE
#error "SMCPAD: USB CDC class disabled"
#endif
#endif

#define TRACE_MAX   768

static char trace_buf[TRACE_MAX];
static u16 trace_len;
static u8 trace_sent;

static u32 cstr_len(const char *s)
{
    const char *p = s;
    while (*p) {
        p++;
    }
    return p - s;
}

static void trace_putc(char c)
{
    if (trace_len < TRACE_MAX) {
        trace_buf[trace_len++] = c;
    }
}

/* Record a boot milestone. Safe anywhere, including before the kernel is up. */
void smcpad_trace(const char *tag)
{
    while (*tag) {
        trace_putc(*tag++);
    }
    trace_putc('\r');
    trace_putc('\n');
}

void smcpad_trace_u32(const char *tag, u32 value)
{
    char digits[10];
    int n = 0;

    while (*tag) {
        trace_putc(*tag++);
    }
    trace_putc('=');
    do {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    } while (value && n < (int)sizeof(digits));
    while (n--) {
        trace_putc(digits[n]);
    }
    trace_putc('\r');
    trace_putc('\n');
}

int smcpad_dbg_ready(void);

#if SMCPAD_TINYUSB
/* ------------------------------------------------------------------ TinyUSB backend */

extern void smcpad_tusb_start(void);
extern volatile u32 smcpad_rx_byte_count;          /* smcpad_fault.c */

void smcpad_dbg_usb_start(void)
{
    smcpad_tusb_start();
}

/* Writes what it can within ~50 ms of the host not reading, and drops the rest: the console
 * must never hang on a closed port. */
static u32 dbg_write(const char *buf, u32 len)
{
    u32 done = 0, idle = 0;

    tud_task();
    if (!tud_cdc_connected()) {
        return 0;
    }
    while (done < len && idle < 50) {
        u32 n = tud_cdc_write(buf + done, len - done);

        done += n;
        tud_cdc_write_flush();
        tud_task();
        if (n) {
            idle = 0;
        } else {
            idle++;
            for (volatile u32 i = 0; i < 4000; i++) {
            }
        }
    }
    return done;
}

/* The 1200-baud touch is honoured wherever the console is reading, long commands included. */
extern volatile int smcpad_usb_touch_1200;
extern void smcpad_on_touch_1200(void);

u32 smcpad_dbg_read(char *buf, u32 len)
{
    u32 n;

    tud_task();
    if (smcpad_usb_touch_1200) {
        smcpad_on_touch_1200();
    }
    if (!smcpad_dbg_ready() || !tud_cdc_available()) {
        return 0;
    }
    n = tud_cdc_read(buf, len);
    smcpad_rx_byte_count += n;
    return n;
}

#else
static void cdc_rx_isr(struct usb_device_t *usb_device);

void smcpad_dbg_usb_start(void)
{
#if TCFG_USB_SLAVE_ENABLE
    usb_device_mode(USB0, CDC_CLASS);
    cdc_set_wakeup_handler(cdc_rx_isr);
#endif
}

/*
 * The USB controller is a MUSB: an endpoint's CSR is reached through one shared INDEX
 * register. cdc_write_data() from the main loop selects the IN endpoint and then works its
 * TXCSR; a USB interrupt in between (an EP0 request, say) leaves INDEX on endpoint 0, and the
 * rest of the write lands in EP0's CSR. EP0 then stops answering, the host times out a
 * control request, drops the device and the chip restarts -- seconds after a reply went out,
 * at random, more often the more we print. (Seen in the macOS kernel log: "endpoint 0x00:
 * status timeout", then "hardware connection lost".) So every USB access from the main loop
 * runs with the USB interrupts masked. Only those: the tick keeps running, so the SDK's
 * jiffies-based timeouts in the write still work.
 */
static void usb_irqs_off(void)
{
    bit_clr_ie(IRQ_USB_CTRL_IDX);
    bit_clr_ie(IRQ_USB_SOF_IDX);
}

static void usb_irqs_on(void)
{
    bit_set_ie(IRQ_USB_SOF_IDX);
    bit_set_ie(IRQ_USB_CTRL_IDX);
}

static u32 dbg_write(const char *buf, u32 len)
{
#if TCFG_USB_SLAVE_ENABLE
    u32 n;

    usb_irqs_off();
    n = cdc_write_data(USB0, (u8 *)buf, len);
    usb_irqs_on();
    return n;
#else
    return 0;
#endif
}

/*
 * Console input, the way the SDK's own CDC example (task_pc.c) does it: the OUT endpoint is
 * read in the USB interrupt, from the CDC wakeup hook, into a ring the main loop empties.
 * Nothing else ever touches the endpoint -- reading it from the main loop as well, or
 * instead, left the double-buffered endpoint stalled now and then in long transfers.
 * The ring is far bigger than one acknowledged upload chunk (host/update.py), so it never
 * has to refuse a packet.
 */
extern volatile u32 smcpad_rx_byte_count;          /* smcpad_fault.c */
extern u32 usb_g_bulk_read(const usb_dev usb_id, u32 ep, u8 *ptr, u32 len, u32 block);

#define RX_RING     4096                            /* power of two */

static u8 rx_ring[RX_RING];
static volatile u32 rx_head, rx_tail;               /* head: interrupt; tail: main loop */
static u8 rx_packet[MAXP_SIZE_CDC_BULKOUT] __attribute__((aligned(4)));
volatile u32 smcpad_rx_overflow;

/*
 * The host never sends full 64-byte packets: a run of them stalls this endpoint after six
 * (the host's transfer then sits there NAKed; not understood -- the endpoint is
 * double-buffered and the SDK's bulk read is not written for that). host/update.py sends 63
 * bytes per write, commands are shorter anyway.
 * The SDK's bulk read keeps reading packets until it has `len` bytes, copying only what fits
 * of the last one and dropping the rest; asking for exactly RX_PACKET makes it stop at the
 * end of each short packet. Then read packet by packet until the endpoint is empty, since
 * two can be waiting behind one interrupt.
 */
#define RX_PACKET   (MAXP_SIZE_CDC_BULKOUT - 1)

static void cdc_rx_isr(struct usb_device_t *usb_device)
{
    for (int k = 0; k < 4; k++) {
        u32 got = usb_g_bulk_read(USB0, CDC_DATA_EP_OUT, rx_packet, RX_PACKET, 0);

        if (got == 0 || got > sizeof(rx_packet)) {
            break;
        }
        for (u32 i = 0; i < got; i++) {
            if (rx_head - rx_tail >= RX_RING) {
                smcpad_rx_overflow++;
                break;
            }
            rx_ring[rx_head & (RX_RING - 1)] = rx_packet[i];
            rx_head++;
        }
        smcpad_rx_byte_count += got;
    }
}

/* Non-blocking: returns 0 when the host has sent nothing. */
u32 smcpad_dbg_read(char *buf, u32 len)
{
    u32 n = 0;

#if TCFG_USB_SLAVE_ENABLE
    cdc_set_wakeup_handler(cdc_rx_isr);     /* the CDC state is re-created on re-enumeration */
    if (!smcpad_dbg_ready()) {
        return 0;
    }
    while (n < len && rx_tail != rx_head) {
        buf[n++] = (char)rx_ring[rx_tail & (RX_RING - 1)];
        rx_tail++;
    }
#endif
    return n;
}

#endif /* SMCPAD_TINYUSB */

/* True once the host has opened the port; on the first success the boot trace goes out. */
int smcpad_dbg_ready(void)
{
    if (trace_sent) {
        return 1;
    }
    if (trace_len == 0 || dbg_write(trace_buf, trace_len) == 0) {
        return 0;
    }
    trace_sent = 1;
    return 1;
}

void smcpad_dbg_puts(const char *s)
{
    if (smcpad_dbg_ready()) {
        dbg_write(s, cstr_len(s));
    }
}

void smcpad_dbg_hex16(const char *tag, u32 value)
{
    static const char hexdigits[] = "0123456789abcdef";
    char line[40];
    u32 i = 0;

    while (*tag && i < sizeof(line) - 10) {
        line[i++] = *tag++;
    }
    line[i++] = '=';
    line[i++] = '0';
    line[i++] = 'x';
    for (int shift = 12; shift >= 0; shift -= 4) {
        line[i++] = hexdigits[(value >> shift) & 0xF];
    }
    line[i++] = '\r';
    line[i++] = '\n';

    if (smcpad_dbg_ready()) {
        dbg_write(line, i);
    }
}

void smcpad_dbg_kv(const char *tag, u32 value)
{
    char line[80];                      /* long enough for a sentence-ish tag */
    char digits[10];
    u32 i = 0;
    int n = 0;

    while (*tag && i < sizeof(line) - 14) {
        line[i++] = *tag++;
    }
    line[i++] = '=';
    do {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    } while (value && n < (int)sizeof(digits));
    while (n--) {
        line[i++] = digits[n];
    }
    line[i++] = '\r';
    line[i++] = '\n';

    if (smcpad_dbg_ready()) {
        dbg_write(line, i);
    }
}
