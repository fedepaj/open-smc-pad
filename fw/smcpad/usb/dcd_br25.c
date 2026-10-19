/*
 * TinyUSB device controller driver for the JieLi BR25 (AC696x / AC636x) full-speed USB.
 *
 * The controller is a Mentor MUSB core behind two JieLi additions (see
 * tools/jielie/periph/usb-fs.md and docs/usb-br25.md):
 *  - the SIE registers are not memory mapped: each 8-bit register is read or written
 *    through USB_CON1 (address << 8 | data, bit 14 = read, bit 15 = done). Indexed CSRs go
 *    through the shared INDEX register, so every access sequence from task context runs
 *    with the USB interrupt masked;
 *  - there is no FIFO data register: each endpoint moves data by DMA between the FIFO and
 *    a RAM buffer whose address is in EP0_ADR / EPn_TADR / EPn_RADR. IN: write the packet
 *    into the buffer, point TADR at it, write its length into EPn_CNT, set TxPktRdy. OUT: the
 *    packet is in the RAM buffer by the time RxPktRdy is set; copy it out, rewind RADR and pop
 *    it with FlushFIFO -- what the SDK does after every packet. The DMA pointers advance with
 *    every packet, hence the rewinds.
 *
 * The data endpoints' quirks (MaxP, the second RX packet, RxCount reading 0) are measured
 * behaviour, written up in docs/usb-br25.md; host/usb_selftest.py exercises them.
 *
 * The EP0 state machine follows TinyUSB's own MUSB driver (src/portable/mentor/musb).
 * Register bit names are MUSB's; JieLi reorders the register addresses (SIE_* below).
 */
#include "tusb_option.h"

#if CFG_TUD_ENABLED && (CFG_TUSB_MCU == OPT_MCU_NONE) && defined(SMCPAD_DCD_BR25)

#include <string.h>
#include "device/dcd.h"

/* ------------------------------------------------------------------ registers */

#define REG32(a)            (*(volatile uint32_t *)(a))
#define USB_CON0            REG32(0x1E1800)
#define USB_CON1            REG32(0x1E1804)
#define USB_EP_CNT(n)       REG32(0x1E1808 + 4u * (n))
#define USB_EP0_ADR         REG32(0x1E181C)
#define USB_EP_TADR(n)      REG32(0x1E1820 + 8u * ((n) - 1u))
#define USB_EP_RADR(n)      REG32(0x1E1824 + 8u * ((n) - 1u))
#define USB_IO_CON0         REG32(0x1E5100)

/* USB_CON0 */
#define CON0_PHY_ON         (1u << 0)
#define CON0_NRST           (1u << 2)
#define CON0_SIEIE          (1u << 11)

/* USB_IO_CON0 */
#define IO_DPPD             (1u << 4)
#define IO_DMPD             (1u << 5)
#define IO_DPPU             (1u << 6)
#define IO_DMPU             (1u << 7)
#define IO_DPDIE            (1u << 9)
#define IO_DMDIE            (1u << 10)
#define IO_USB_IO_MODE      (1u << 11)
#define IO_SR               (1u << 12)

/* SIE register addresses (JieLi order) */
#define SIE_FADDR           0x00
#define SIE_POWER           0x01
#define SIE_INTRTX1         0x02
#define SIE_INTRTX2         0x03
#define SIE_INTRRX1         0x04
#define SIE_INTRRX2         0x05
#define SIE_INTRUSB         0x06
#define SIE_INTRTXE1        0x07
#define SIE_INTRTXE2        0x08
#define SIE_INTRRXE1        0x09
#define SIE_INTRRXE2        0x0A
#define SIE_INTRUSBE        0x0B
#define SIE_INDEX           0x0E
#define SIE_TXMAXP          0x10
#define SIE_CSR0            0x11            /* = TxCSRL for n > 0 */
#define SIE_TXCSRH          0x12
#define SIE_RXMAXP          0x13
#define SIE_RXCSRL          0x14
#define SIE_RXCSRH          0x15
#define SIE_COUNT0          0x16            /* = RxCount1 for n > 0 */
#define SIE_RXCOUNT2        0x17

/* Power */
#define POWER_SOFTCONN      0x40

/* IntrUSB */
#define INTRUSB_SUSPEND     0x01
#define INTRUSB_RESUME      0x02
#define INTRUSB_RESET       0x04
#define INTRUSB_SOF         0x08

/* CSR0 */
#define CSR0_RXPKTRDY       0x01
#define CSR0_TXPKTRDY       0x02
#define CSR0_SENTSTALL      0x04
#define CSR0_DATAEND        0x08
#define CSR0_SETUPEND       0x10
#define CSR0_SENDSTALL      0x20
#define CSR0_SVDRXPKTRDY    0x40
#define CSR0_SVDSETUPEND    0x80

/* TxCSRL */
#define TXCSR_TXPKTRDY      0x01
#define TXCSR_FIFONOTEMPTY  0x02
#define TXCSR_UNDERRUN      0x04
#define TXCSR_FLUSHFIFO     0x08
#define TXCSR_SENDSTALL     0x10
#define TXCSR_SENTSTALL     0x20
#define TXCSR_CLRDATATOG    0x40

/* RxCSRL */
#define RXCSR_RXPKTRDY      0x01
#define RXCSR_FIFOFULL      0x02
#define RXCSR_OVERRUN       0x04
#define RXCSR_FLUSHFIFO     0x10
#define RXCSR_SENDSTALL     0x20
#define RXCSR_SENTSTALL     0x40
#define RXCSR_CLRDATATOG    0x80

#define EP_MAX              TUP_DCD_ENDPOINT_MAX        /* EP0..EP4 */
#define EP0_SIZE            CFG_TUD_ENDPOINT0_SIZE
#define EP_BUF_SIZE         64                          /* full speed bulk/interrupt */
#define SIE_SPIN_LIMIT      20000u

extern void bit_clr_ie(unsigned char index);
extern void bit_set_ie(unsigned char index);
#define IRQ_USB_CTRL_IDX    9

/* ------------------------------------------------------------------ event log */

/* Last events, kept for a post-mortem when the console itself is the thing being brought
 * up: smcpad_usb_log() hands them to whoever can print. */
#define LOG_LEN             128
static volatile uint32_t log_buf[LOG_LEN];
static volatile uint32_t log_pos;

static void ulog(uint32_t tag, uint32_t a, uint32_t b)
{
    if (tag == 0x30 || tag == 0x40 || tag == 0x51 || (tag == 0x02 && b == 0x010)) {
        return;                         /* EP0 works: keep the log for the data endpoints */
    }
    log_buf[log_pos++ % LOG_LEN] = (tag << 24) | ((a & 0xFFF) << 12) | (b & 0xFFF);
}

uint32_t smcpad_usb_log(uint32_t *out, uint32_t max)
{
    uint32_t n = log_pos < LOG_LEN ? log_pos : LOG_LEN;
    uint32_t first = log_pos - n;

    if (n > max) {
        first += n - max;
        n = max;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = log_buf[(first + i) % LOG_LEN];
    }
    return n;
}

/* ------------------------------------------------------------------ SIE access */

static uint8_t sie_read(uint8_t addr)
{
    if (!(USB_CON0 & CON0_NRST)) {
        return 0;
    }
    USB_CON1 = ((uint32_t)(addr & 0x3F) << 8) | (1u << 14);
    for (uint32_t n = 0; !(USB_CON1 & (1u << 15)) && n < SIE_SPIN_LIMIT; n++) {
    }
    return (uint8_t)USB_CON1;
}

static void sie_write(uint8_t addr, uint8_t value)
{
    if (!(USB_CON0 & CON0_NRST)) {
        return;
    }
    USB_CON1 = ((uint32_t)(addr & 0x3F) << 8) | value;
    for (uint32_t n = 0; !(USB_CON1 & (1u << 15)) && n < SIE_SPIN_LIMIT; n++) {
    }
}

static void sie_index(uint8_t ep)
{
    sie_write(SIE_INDEX, ep);
}

static uint16_t sie_read16(uint8_t lo)
{
    return (uint16_t)(sie_read(lo) | ((uint16_t)sie_read((uint8_t)(lo + 1)) << 8));
}

/* ------------------------------------------------------------------ state */

/*
 * DMA buffers: plain RAM, word aligned. EP0 has one buffer for both directions.
 *
 * Endpoints 1..4 get MaxP = 0xFF, as the SDK writes it, whatever the descriptor says. With
 * the real packet size (64) the controller double-buffers the endpoint by itself, moving
 * every other packet through EPn_xADR + 512, and the RX count then reads 0 at random
 * (measured; docs/usb-br25.md). With 0xFF each direction uses the one buffer its address
 * register names -- though the RX FIFO can still hold a second packet: see ep_out_take().
 */
#define MAXP_REG        0xFF
static uint8_t ep0_buf[EP0_SIZE] __attribute__((aligned(4)));
static uint8_t ep_tx_buf[EP_MAX][EP_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t ep_rx_buf[EP_MAX][EP_BUF_SIZE] __attribute__((aligned(4)));

typedef struct {
    uint8_t *buf;
    uint16_t length;
    uint16_t remaining;
    uint16_t last_len;          /* IN: size of the packet in flight */
    bool armed;
} pipe_t;

static pipe_t pipe_in[EP_MAX], pipe_out[EP_MAX];

/* EP0: the state machine of TinyUSB's MUSB driver (dcd_musb.c, pipe0_*), with the FIFO
 * accesses replaced by the DMA buffer. Its two subtleties matter here too, both seen on this
 * chip: the host's STATUS OUT can complete together with the last DATA IN packet (one
 * interrupt for both), and usbd -- running in a task -- arms the previous transfer's STATUS
 * OUT after the next SETUP has already arrived; that SETUP is parked until the old transfer
 * is retired. */
enum {
    EP0_IDLE = 0,
    EP0_DATA_IN,
    EP0_DATA_OUT,
    EP0_STATUS_IN,
    EP0_STATUS_OUT,             /* after DATAEND: neither the arm nor the confirmation yet */
    EP0_STATUS_OUT_PENDING_XFER,/* armed first; the confirmation IRQ completes it */
    EP0_STATUS_OUT_PENDING_IRQ, /* confirmation first; the arm completes it */
};

static struct {
    uint8_t *buf;               /* DATA OUT target */
    uint16_t xact_len;
    uint16_t remain_wlength;
    uint8_t state;
    uint8_t pending_addr;
    bool rxrdy_consumed;        /* RxPktRdy left set for a packet already taken */
    bool deferred_setup_valid;
    uint8_t deferred_setup[8];
} ep0;

static void usb_irq_off(void) { bit_clr_ie(IRQ_USB_CTRL_IDX); }
static void usb_irq_on(void)  { bit_set_ie(IRQ_USB_CTRL_IDX); }

/* ------------------------------------------------------------------ EP0 */

static bool ep0_data_done(uint16_t len)
{
    return ep0.remain_wlength == 0 || len < EP0_SIZE;
}

static void ep0_read_setup(uint8_t setup[8])
{
    memcpy(setup, ep0_buf, 8);
}

static void ep0_start_setup(uint8_t rhport, const uint8_t setup[8], bool is_isr)
{
    tusb_control_request_t req;

    memcpy(&req, setup, sizeof(req));
    ulog(0x51, req.bmRequestType, req.bRequest);
    ep0.remain_wlength = req.wLength;
    if (req.wLength == 0) {
        ep0.state = EP0_STATUS_IN;      /* edpt0_xfer(STATUS IN) acks it with DATAEND */
        ep0.rxrdy_consumed = true;
    } else if (req.bmRequestType & TUSB_DIR_IN_MASK) {
        ep0.state = EP0_DATA_IN;
        if (!ep0.rxrdy_consumed) {
            sie_index(0);
            sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY);
        }
    } else {
        ep0.state = EP0_DATA_OUT;       /* NAK the data until usbd arms a buffer */
        ep0.rxrdy_consumed = true;
    }
    dcd_event_setup_received(rhport, setup, is_isr);
}

static void ep0_try_deferred_setup(uint8_t rhport, bool is_isr)
{
    if (ep0.deferred_setup_valid) {
        ep0.deferred_setup_valid = false;
        ep0_start_setup(rhport, ep0.deferred_setup, is_isr);
    }
}

/* A tail event of the current transfer: TX done, or the status stage confirmed. */
static void ep0_tail_isr(uint8_t rhport)
{
    switch (ep0.state) {
    case EP0_DATA_IN:
        if (ep0_data_done(ep0.xact_len)) {
            ep0.state = ep0.deferred_setup_valid ? EP0_STATUS_OUT_PENDING_IRQ : EP0_STATUS_OUT;
        }
        dcd_event_xfer_complete(rhport, TU_EP0_IN, ep0.xact_len, XFER_RESULT_SUCCESS, true);
        break;
    case EP0_STATUS_OUT:
        ep0.state = EP0_STATUS_OUT_PENDING_IRQ;
        break;
    case EP0_STATUS_OUT_PENDING_XFER:
        ep0.state = EP0_IDLE;
        dcd_event_xfer_complete(rhport, TU_EP0_OUT, 0, XFER_RESULT_SUCCESS, true);
        ep0_try_deferred_setup(rhport, true);
        break;
    case EP0_STATUS_IN:
        if (ep0.pending_addr) {
            sie_write(SIE_FADDR, ep0.pending_addr);
            ep0.pending_addr = 0;
        }
        ep0.state = EP0_IDLE;
        dcd_event_xfer_complete(rhport, TU_EP0_IN, 0, XFER_RESULT_SUCCESS, true);
        ep0_try_deferred_setup(rhport, true);
        break;
    default:
        break;
    }
}

static void ep0_isr(uint8_t rhport)
{
    uint8_t csr;

    sie_index(0);
    csr = sie_read(SIE_CSR0);
    ulog(0x30, csr, ep0.state);

    if (csr & CSR0_SENTSTALL) {
        sie_write(SIE_CSR0, 0);
        ep0.state = EP0_IDLE;
        ep0.deferred_setup_valid = false;
        ep0.rxrdy_consumed = false;
        return;
    }
    if (csr & CSR0_SETUPEND) {
        sie_write(SIE_CSR0, CSR0_SVDSETUPEND);
        ep0.state = EP0_IDLE;
        ep0.deferred_setup_valid = false;
        ep0.rxrdy_consumed = false;
        if (!(csr & CSR0_RXPKTRDY)) {
            return;
        }
    }

    if (csr & CSR0_RXPKTRDY) {
        if (ep0.rxrdy_consumed) {
            return;                     /* stale: this packet was already taken */
        }
        switch (ep0.state) {
        case EP0_IDLE: {
            uint8_t setup[8];

            ep0_read_setup(setup);
            ep0_start_setup(rhport, setup, true);
            break;
        }
        case EP0_DATA_OUT: {
            uint16_t count = sie_read(SIE_COUNT0);

            if (count && ep0.buf) {
                memcpy(ep0.buf, ep0_buf, count);
            }
            ep0.remain_wlength -= (count < ep0.remain_wlength) ? count : ep0.remain_wlength;
            ep0.rxrdy_consumed = true;  /* acked by the next edpt0_xfer */
            if (ep0_data_done(count)) {
                ep0.state = EP0_STATUS_IN;
            }
            dcd_event_xfer_complete(rhport, TU_EP0_OUT, count, XFER_RESULT_SUCCESS, true);
            break;
        }
        case EP0_DATA_IN:
        case EP0_STATUS_OUT:
        case EP0_STATUS_OUT_PENDING_XFER:
        case EP0_STATUS_OUT_PENDING_IRQ:
        case EP0_STATUS_IN:
            /* A new SETUP while the old transfer's tail is still in flight: park it, then
             * retire the old one (its status confirmation came with this interrupt). */
            ep0_read_setup(ep0.deferred_setup);
            ep0.deferred_setup_valid = true;
            ep0.rxrdy_consumed = true;
            ep0_tail_isr(rhport);
            break;
        default:
            break;
        }
        return;
    }

    if (csr & CSR0_DATAEND) {
        return;                         /* status stage still running */
    }
    ep0_tail_isr(rhport);
}

static bool ep0_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total, bool is_isr)
{
    bool in = tu_edpt_dir(ep_addr) == TUSB_DIR_IN;

    TU_ASSERT(total <= EP0_SIZE);
    sie_index(0);
    ulog(0x40, ep_addr, ((uint32_t)ep0.state << 8) | total);

    switch (ep0.state) {
    case EP0_DATA_IN:
        TU_ASSERT(in);
        ep0.xact_len = total;
        if (ep0.rxrdy_consumed) {       /* replayed SETUP: ack its parked RxPktRdy first */
            sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY);
            ep0.rxrdy_consumed = false;
        }
        if (total) {
            memcpy(ep0_buf, buffer, total);
        }
        ep0.remain_wlength -= (total < ep0.remain_wlength) ? total : ep0.remain_wlength;
        USB_EP0_ADR = (uint32_t)ep0_buf;
        USB_EP_CNT(0) = total;
        sie_write(SIE_CSR0, ep0_data_done(total) ? (CSR0_TXPKTRDY | CSR0_DATAEND) : CSR0_TXPKTRDY);
        break;

    case EP0_DATA_OUT:
        TU_ASSERT(!in);
        ep0.xact_len = total;
        ep0.buf = buffer;
        sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY);
        ep0.rxrdy_consumed = false;
        break;

    case EP0_STATUS_IN:
        TU_ASSERT(in && total == 0);
        sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
        ep0.rxrdy_consumed = false;
        break;

    case EP0_STATUS_OUT:
        TU_ASSERT(!in && total == 0);
        ep0.state = EP0_STATUS_OUT_PENDING_XFER;
        break;

    case EP0_STATUS_OUT_PENDING_IRQ:
        ep0.state = EP0_IDLE;
        dcd_event_xfer_complete(rhport, ep_addr, 0, XFER_RESULT_SUCCESS, is_isr);
        ep0_try_deferred_setup(rhport, is_isr);
        break;

    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ EP1..4 */

static void ep_in_load(uint8_t epn)
{
    pipe_t *p = &pipe_in[epn];
    uint16_t n = p->remaining < EP_BUF_SIZE ? p->remaining : EP_BUF_SIZE;

    if (n) {
        memcpy(ep_tx_buf[epn], p->buf, n);
        p->buf += n;
        p->remaining -= n;
    }
    p->last_len = n;
    ulog(0x70 + epn, n, p->remaining);
    sie_index(epn);
    USB_EP_TADR(epn) = (uint32_t)ep_tx_buf[epn];
    USB_EP_CNT(epn) = n;
    sie_write(SIE_CSR0, TXCSR_TXPKTRDY);
}

static void ep_in_isr(uint8_t rhport, uint8_t epn)
{
    pipe_t *p = &pipe_in[epn];
    uint8_t csr;

    sie_index(epn);
    csr = sie_read(SIE_CSR0);
    if (csr & TXCSR_SENTSTALL) {
        sie_write(SIE_CSR0, csr & ~(TXCSR_SENTSTALL | TXCSR_UNDERRUN));
        return;
    }
    if (!p->armed || (csr & TXCSR_TXPKTRDY)) {
        return;
    }
    if (p->remaining) {
        ep_in_load(epn);
        return;
    }
    p->armed = false;
    ulog(0x78 + epn, p->length, csr);
    dcd_event_xfer_complete(rhport, tu_edpt_addr(epn, TUSB_DIR_IN), p->length,
                            XFER_RESULT_SUCCESS, true);
}

static void rx_irq_enable(uint8_t epn, bool on)
{
    uint8_t r = epn < 8 ? SIE_INTRRXE1 : SIE_INTRRXE2;
    uint8_t bit = (uint8_t)(1u << (epn & 7));
    uint8_t v = sie_read(r);

    sie_write(r, on ? (v | bit) : (v & ~bit));
}

/* Returns true when the transfer completed. */
static bool ep_out_take(uint8_t rhport, uint8_t epn, bool is_isr)
{
    pipe_t *p = &pipe_out[epn];
    uint8_t csr;
    uint16_t count, n;

    sie_index(epn);
    csr = sie_read(SIE_RXCSRL);
    if (csr & RXCSR_SENTSTALL) {
        sie_write(SIE_RXCSRL, csr & ~(RXCSR_SENTSTALL | RXCSR_OVERRUN));
        return false;
    }
    if (!(csr & RXCSR_RXPKTRDY)) {
        return false;
    }
    if (!p->armed) {
        /* Leave it in the endpoint (the host is NAKed meanwhile) and stop the interrupt
         * until a transfer is armed; dcd_edpt_xfer() takes it then. */
        rx_irq_enable(epn, false);
        return false;
    }
    count = sie_read16(SIE_COUNT0);
    /* The RX FIFO holds two packets, and in a run of them RxPktRdy can come up while the
     * packet is still streaming into the buffer: RxCount reads 0, FIFOFull is set. Taking it
     * then -- copying, rewinding RADR, popping -- splits it (measured: one packet in a run
     * arrived with just its first byte wrong; treating the event as a 64-byte packet read a
     * stale buffer). Wait for the count; a 64-byte packet takes ~43 us at full speed. A real
     * zero-length packet comes without FIFOFull, or stays at 0 past the bound. */
    for (uint32_t spin = 0; count == 0 && (csr & RXCSR_FIFOFULL) && spin < 400; spin++) {
        count = sie_read16(SIE_COUNT0);
    }
    n = count < p->remaining ? count : p->remaining;
    ulog(0x60 + epn, count, p->remaining);
    if (n) {
        memcpy(p->buf, ep_rx_buf[epn], n);
        p->buf += n;
        p->remaining -= n;
    }
    /* Rewind the DMA write pointer, then pop the packet: the one queued behind it, if any,
     * is written to the start of the buffer as it becomes the head. */
    USB_EP_RADR(epn) = (uint32_t)ep_rx_buf[epn];
    sie_write(SIE_RXCSRL, RXCSR_FLUSHFIFO);

    if (count < EP_BUF_SIZE || p->remaining == 0) {
        p->armed = false;
        dcd_event_xfer_complete(rhport, epn, p->length - p->remaining, XFER_RESULT_SUCCESS, is_isr);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ bus events */

static void bus_reset(uint8_t rhport)
{
    ulog(0x10, 0, 0);
    memset(&ep0, 0, sizeof(ep0));
    memset(pipe_in, 0, sizeof(pipe_in));
    memset(pipe_out, 0, sizeof(pipe_out));
    USB_EP0_ADR = (uint32_t)ep0_buf;
    sie_write(SIE_INTRTXE1, 0x01);
    sie_write(SIE_INTRTXE2, 0);
    sie_write(SIE_INTRRXE1, 0);
    sie_write(SIE_INTRRXE2, 0);
    dcd_event_bus_reset(rhport, TUSB_SPEED_FULL, true);
}

/* ------------------------------------------------------------------ dcd API */

bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
    (void)rh_init;

    /* The SDK's sequence (usb_device_mode -> SIE init, read off its disassembly). */
    USB_IO_CON0 |= IO_DMDIE | IO_DPDIE;
    USB_IO_CON0 &= ~IO_USB_IO_MODE;
    USB_IO_CON0 |= IO_SR;
    USB_CON0 &= ~0x4035u;
    USB_CON1 = 0;
    USB_CON0 |= CON0_PHY_ON;
    USB_IO_CON0 &= ~(IO_DPPD | IO_DMPD | IO_DPPU | IO_DMPU);
    USB_CON0 |= 0x3Cu;                  /* NRST, TM1, CID = device, VBUS */

    sie_write(SIE_INTRUSBE, 0);
    sie_write(SIE_INTRTXE1, 0);
    sie_write(SIE_INTRTXE2, 0);
    sie_write(SIE_INTRRXE1, 0);
    sie_write(SIE_INTRRXE2, 0);
    USB_CON0 |= CON0_SIEIE;

    USB_EP0_ADR = (uint32_t)ep0_buf;
    for (uint8_t n = 1; n < EP_MAX; n++) {
        USB_EP_TADR(n) = (uint32_t)ep_tx_buf[n];
        USB_EP_RADR(n) = (uint32_t)ep_rx_buf[n];
    }
    sie_write(SIE_INTRUSBE, INTRUSB_RESET | INTRUSB_SUSPEND | INTRUSB_RESUME);
    sie_write(SIE_INTRTXE1, 0x01);

    ulog(0x01, USB_CON0 & 0xFFF, USB_IO_CON0 & 0xFFF);
    dcd_connect(rhport);
    return true;
}

void dcd_int_enable(uint8_t rhport)
{
    (void)rhport;
    usb_irq_on();
}

void dcd_int_disable(uint8_t rhport)
{
    (void)rhport;
    usb_irq_off();
}

void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
    (void)rhport;
    /* Applied when the STATUS IN completes (ep0_isr); answer the request now. */
    ep0.pending_addr = dev_addr;
    ep0.buf = NULL;
    ep0.xact_len = 0;
    ep0.state = EP0_STATUS_IN;
    ep0.rxrdy_consumed = false;
    sie_index(0);
    sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
}

void dcd_remote_wakeup(uint8_t rhport)
{
    (void)rhport;
}

void dcd_connect(uint8_t rhport)
{
    (void)rhport;
    USB_IO_CON0 |= IO_DPPU;
    sie_write(SIE_POWER, POWER_SOFTCONN);
}

void dcd_disconnect(uint8_t rhport)
{
    (void)rhport;
    sie_write(SIE_POWER, 0);
    USB_IO_CON0 &= ~IO_DPPU;
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
    (void)rhport;
    (void)en;
}

bool dcd_edpt_open(uint8_t rhport, tusb_desc_endpoint_t const *desc)
{
    uint8_t epn = tu_edpt_number(desc->bEndpointAddress);
    bool in = tu_edpt_dir(desc->bEndpointAddress) == TUSB_DIR_IN;
    uint16_t mps = tu_edpt_packet_size(desc);

    (void)rhport;
    TU_ASSERT(epn > 0 && epn < EP_MAX && mps <= EP_BUF_SIZE);
    usb_irq_off();
    sie_index(epn);
    if (in) {
        memset(&pipe_in[epn], 0, sizeof(pipe_t));
        USB_EP_TADR(epn) = (uint32_t)ep_tx_buf[epn];
        sie_write(SIE_TXMAXP, MAXP_REG);
        sie_write(SIE_CSR0, TXCSR_CLRDATATOG | TXCSR_FLUSHFIFO);
        sie_write(SIE_INTRTXE1, sie_read(SIE_INTRTXE1) | (uint8_t)(1u << epn));
    } else {
        memset(&pipe_out[epn], 0, sizeof(pipe_t));
        USB_EP_RADR(epn) = (uint32_t)ep_rx_buf[epn];
        sie_write(SIE_RXMAXP, MAXP_REG);
        sie_write(SIE_RXCSRL, RXCSR_CLRDATATOG | RXCSR_FLUSHFIFO);
        rx_irq_enable(epn, true);
    }
    ulog(0x20, desc->bEndpointAddress, mps);
    usb_irq_on();
    return true;
}

void dcd_edpt_close_all(uint8_t rhport)
{
    (void)rhport;
    usb_irq_off();
    sie_write(SIE_INTRTXE1, 0x01);
    sie_write(SIE_INTRRXE1, 0);
    memset(pipe_in, 0, sizeof(pipe_in));
    memset(pipe_out, 0, sizeof(pipe_out));
    usb_irq_on();
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total, bool is_isr)
{
    uint8_t epn = tu_edpt_number(ep_addr);
    bool ok = true;

    if (!is_isr) {
        usb_irq_off();
    }
    if (epn == 0) {
        ok = ep0_xfer(rhport, ep_addr, buffer, total, is_isr);
    } else if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
        pipe_t *p = &pipe_in[epn];

        p->buf = buffer;
        p->length = total;
        p->remaining = total;
        p->armed = true;
        ep_in_load(epn);
    } else {
        pipe_t *p = &pipe_out[epn];

        p->buf = buffer;
        p->length = total;
        p->remaining = total;
        p->armed = true;
        ulog(0x68 + epn, total, 0);
        rx_irq_enable(epn, true);
        ep_out_take(rhport, epn, is_isr);   /* a packet parked while nothing was armed */
    }
    if (!is_isr) {
        usb_irq_on();
    }
    return ok;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
    uint8_t epn = tu_edpt_number(ep_addr);

    (void)rhport;
    usb_irq_off();
    sie_index(epn);
    if (ep_addr == TU_EP0_IN) {
        /* EP0 is stalled through its OUT half only, as in TinyUSB's MUSB driver */
    } else if (epn == 0) {
        ep0.state = EP0_IDLE;
        ep0.buf = NULL;
        if (ep0.deferred_setup_valid) {
            /* the stalled transfer already ended on the wire: replay instead of stalling
             * the host's next request */
            ep0_try_deferred_setup(rhport, false);
        } else {
            ep0.rxrdy_consumed = false;
            sie_write(SIE_CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
        }
    } else if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
        sie_write(SIE_CSR0, TXCSR_SENDSTALL);
        pipe_in[epn].armed = false;
    } else {
        sie_write(SIE_RXCSRL, RXCSR_SENDSTALL);
        pipe_out[epn].armed = false;
    }
    usb_irq_on();
}

void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
    uint8_t epn = tu_edpt_number(ep_addr);

    (void)rhport;
    usb_irq_off();
    sie_index(epn);
    if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN) {
        sie_write(SIE_CSR0, TXCSR_CLRDATATOG);
    } else {
        sie_write(SIE_RXCSRL, RXCSR_CLRDATATOG);
    }
    usb_irq_on();
}

/* ------------------------------------------------------------------ interrupt */

void dcd_int_handler(uint8_t rhport)
{
    uint8_t usb, rx, saved;
    uint16_t tx;

    (void)USB_CON0;                     /* reading CON0 acknowledges the SIE interrupt */
    saved = sie_read(SIE_INDEX);
    usb = sie_read(SIE_INTRUSB) & sie_read(SIE_INTRUSBE);
    tx = sie_read16(SIE_INTRTX1);
    rx = sie_read(SIE_INTRRX1);
    tx &= (uint16_t)(sie_read(SIE_INTRTXE1) | ((uint16_t)sie_read(SIE_INTRTXE2) << 8));
    rx &= sie_read(SIE_INTRRXE1);
    if (usb || tx || rx) {
        ulog(0x02, usb, ((uint32_t)tx << 4) | rx);
    }

    if (usb & INTRUSB_RESET) {
        bus_reset(rhport);
    }
    if (usb & INTRUSB_SUSPEND) {
        dcd_event_bus_signal(rhport, DCD_EVENT_SUSPEND, true);
    }
    if (usb & INTRUSB_RESUME) {
        dcd_event_bus_signal(rhport, DCD_EVENT_RESUME, true);
    }
    if (tx & 1) {
        ep0_isr(rhport);
    }
    for (uint8_t n = 1; n < EP_MAX; n++) {
        if (tx & (1u << n)) {
            ep_in_isr(rhport, n);
        }
        if (rx & (1u << n)) {
            ep_out_take(rhport, n, true);
        }
    }
    sie_index(saved);
}

#endif
