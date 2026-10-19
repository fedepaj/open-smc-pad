/*
 * Post-mortem for CPU exceptions.
 *
 * The SDK points interrupt vectors 0 (emulator exception) and 1 (exception) at
 * exception_irq_handler, which hands the register frame to exception_analyze() -- a report
 * printed on the log UART, which this board does not have -- and then spins until the 8 s
 * watchdog resets the chip. From the outside that is indistinguishable from a hang.
 * (exception_analyze cannot simply be replaced: the linker pulls the SDK's copy in anyway.)
 *
 * So smcpad_fault_install() re-points both vectors at handlers of ours, one per vector so we
 * also learn which one fired. They copy the special registers into the RAM block below the
 * mask ROM's memory, which a reset does not clear, and spin. Early in the next boot (before
 * app_main's update_result_get(), which zeroes part of that block) smcpad_fault_capture()
 * moves the record to .bss and the console prints it. Map the addresses with
 *   objdump -d fw/smcpad/out/sdk.elf --start-address=<addr-0x20> --stop-address=<addr+0x10>
 * on the same build.
 *
 * If the chip dies and no record appears, it was a hang, not an exception.
 */
#include "system/includes.h"
#include "asm/power_interface.h"            /* AT_VOLATILE_RAM_CODE */
#include "asm/hwi.h"

#define FAULT_REGS      5
#define FAULT_MAGIC     0xFA017E00u         /* | vector index */

extern u32 UPDATA_BEG;
/* UPDATA_BEG + 8: past the console breadcrumb (+0) and the flash driver summary (+4). */
#define FAULT_STORE     ((volatile u32 *)((u32)&UPDATA_BEG + 8))

static u32 captured[FAULT_REGS];
static u32 captured_vector;
static int captured_valid;

/*
 * The handlers record and then CHAIN to the SDK's exception_irq_handler, so its behaviour is
 * unchanged: vector 0 in particular is not only for crashes -- debug_init() routes stack-limit,
 * misalignment and illegal-instruction checks through it, and the SDK's breakpoint machinery
 * (breakpoint_isr, bp_tab, __bp_ret) handles some events and resumes. A first version that
 * recorded and then spun hung the boot on the first such event, before USB came up.
 *
 * Written as naked assembly: nothing may change reti/rets/... before they are read, and every
 * register must be back as it was when we jump on. r0/r1 are saved on the stack around the
 * copy. Runs from RAM: the fault may have been a bad fetch from flash.
 * Record layout at FAULT_STORE: [0] magic | vector, [1] reti, [2] rete, [3] retx, [4] rets,
 * [5] psr. Whatever exception came last before a reset is what the next boot reports.
 */
extern void exception_irq_handler(void);

#define FAULT_ISR(name, vector)                                         \
    AT_VOLATILE_RAM_CODE __attribute__((naked)) void name(void)         \
    {                                                                   \
        __asm__ volatile(                                               \
            "[--sp] = {r1-r0}\n\t"                                      \
            "r1 = UPDATA_BEG\n\t"                                       \
            "r0 = reti\n\t"  "[r1+12] = r0\n\t"                         \
            "r0 = rete\n\t"  "[r1+16] = r0\n\t"                         \
            "r0 = retx\n\t"  "[r1+20] = r0\n\t"                         \
            "r0 = rets\n\t"  "[r1+24] = r0\n\t"                         \
            "r0 = psr\n\t"   "[r1+28] = r0\n\t"                         \
            "r0 = %0\n\t"    "[r1+8] = r0\n\t"                          \
            "{r1-r0} = [sp++]\n\t"                                      \
            "goto exception_irq_handler\n\t"                            \
            :: "i"(FAULT_MAGIC | (vector)));                            \
    }

FAULT_ISR(fault_isr_emu, 0)
FAULT_ISR(fault_isr_exc, 1)

void smcpad_fault_install(void)
{
    request_irq(IRQ_EMUEXCPT_IDX, 2, fault_isr_emu, 0);
    request_irq(IRQ_EXCEPTION_IDX, 2, fault_isr_exc, 0);
}

/* Call as early as possible in the boot: the block gets zeroed during app_main. */
void smcpad_fault_capture(void)
{
    volatile u32 *store = FAULT_STORE;

    if ((store[0] & 0xFFFFFF00u) != FAULT_MAGIC) {
        return;
    }
    captured_vector = store[0] & 0xFF;
    for (int i = 0; i < FAULT_REGS; i++) {
        captured[i] = store[1 + i];
    }
    store[0] = 0;
    captured_valid = 1;
}

/* For the console: 0 if no exception was recorded before the last reset. Registers in the
 * order reti, rete, retx, rets, psr. */
int smcpad_fault_get(u32 regs[FAULT_REGS], u32 *vector)
{
    if (!captured_valid) {
        return 0;
    }
    for (int i = 0; i < FAULT_REGS; i++) {
        regs[i] = captured[i];
    }
    *vector = captured_vector;
    return 1;
}

/*
 * PC sampler, for hangs and for exceptions (the SDK's exception handler reports on a UART we
 * do not have, then spins until the watchdog): a usr_timer callback stores the interrupted PC
 * (reti) every SAMPLE_MS into a ring in SMCPAD_NOINIT -- 256 bytes under UPDATA_BEG that
 * fw/build.sh takes out of the heap, so nothing initialises them and they survive a reset
 * (the UPDATA block itself does not: the boot clears all but its first 8 bytes).
 *
 * usr_timer callbacks run in timer1's interrupt (priority 1): a hang inside the USB interrupt
 * (3) freezes the samples too -- the coarse ring's sample counter then stops well before the
 * reset, which is itself the tell. (Raising timer1 to 3 for a while confirmed the console
 * deaths were not such hangs.) Exceptions do not spin here: exception_analyze() resets at
 * once, and smcpad_chip_reset() below records it.
 */
#define SAMPLE_MS       2
#define RING_LEN        16
#define FRAME_LEN       24
#define COARSE_LEN      64
#define COARSE_EVERY    50              /* samples: 100 ms */
#define RING_MAGIC      0x5A3E0000u
#define RESET_MAGIC     0xC4E5E700u

extern u32 SMCPAD_NOINIT[];
extern u32 ISR_BASE[];

struct noinit {
    u32 ring_head;                  /* RING_MAGIC | next slot */
    u32 ring[RING_LEN];
    u32 reset_magic;                /* RESET_MAGIC: chip_reset() was called */
    u32 reset_rets;                 /* its caller (an ASSERT, exception_irq_handler, ...) */
    u32 reset_r0, reset_r1, reset_sp;
    u32 frame[FRAME_LEN];           /* words at r0, when r0 points into RAM */
    u32 coarse_head;                /* next slot */
    u32 coarse_pc[COARSE_LEN];      /* one PC every 100 ms: the last 6.4 s */
    u32 coarse_stat[COARSE_LEN];    /* samples so far << 16 | console bytes received */
};

/* Counters for the coarse ring: USB interrupts (usb_isr_marked) and bytes the console read
 * (smcpad_dbg.c). */
volatile u32 smcpad_usb_irq_count;
volatile u32 smcpad_rx_byte_count;
static u32 coarse_tick, sample_count;
#define NOINIT          ((volatile struct noinit *)SMCPAD_NOINIT)

static struct noinit captured_noinit;
static int ring_valid, reset_valid;

static void pc_sample(void *priv)
{
    volatile struct noinit *n = NOINIT;
    u32 pc, slot = n->ring_head & 0xFF;

    __asm__ volatile("%0 = reti" : "=r"(pc));
    if ((n->ring_head & 0xFFFF0000u) != RING_MAGIC || slot >= RING_LEN) {
        slot = 0;
    }
    n->ring[slot] = pc;
    n->ring_head = RING_MAGIC | ((slot + 1) % RING_LEN);

    sample_count++;
    if (++coarse_tick >= COARSE_EVERY) {
        u32 c = n->coarse_head;

        coarse_tick = 0;
        if (c >= COARSE_LEN) {
            c = 0;
        }
        n->coarse_pc[c] = pc;
        n->coarse_stat[c] = (sample_count << 16) | (smcpad_rx_byte_count & 0xFFFF);
        n->coarse_head = (c + 1) % COARSE_LEN;
    }

}

void smcpad_chip_reset(u32 r0, u32 r1);

/* Only the linker script refers to smcpad_chip_reset; this keeps LTO from dropping it. */
void (*volatile smcpad_chip_reset_keep)(u32, u32);

void smcpad_pcsample_start(void)
{
    smcpad_chip_reset_keep = smcpad_chip_reset;
    NOINIT->ring_head = 0;
    NOINIT->reset_magic = 0;
    NOINIT->coarse_head = 0;
    for (int i = 0; i < COARSE_LEN; i++) {
        NOINIT->coarse_pc[i] = 0;
        NOINIT->coarse_stat[i] = 0;
    }
    usr_timer_add(0, pc_sample, SAMPLE_MS, 1);
}

/* Call early in the boot, before smcpad_pcsample_start() resets the records. */
void smcpad_pcsample_capture(void)
{
    volatile struct noinit *n = NOINIT;
    u32 *dst = (u32 *)&captured_noinit;

    for (u32 i = 0; i < sizeof(captured_noinit) / 4; i++) {
        dst[i] = ((volatile u32 *)n)[i];
    }
    ring_valid = (n->ring_head & 0xFFFF0000u) == RING_MAGIC;
    reset_valid = n->reset_magic == RESET_MAGIC;
    n->ring_head = 0;
    n->reset_magic = 0;
}

/* The PCs sampled before the last reset, oldest first; 0 if none. */
int smcpad_pcsample_get(u32 out[RING_LEN])
{
    u32 slot = captured_noinit.ring_head & 0xFF;

    if (!ring_valid || slot >= RING_LEN) {
        return 0;
    }
    for (int i = 0; i < RING_LEN; i++) {
        out[i] = captured_noinit.ring[(slot + i) % RING_LEN];
    }
    return RING_LEN;
}

/* What called chip_reset() before the last boot: rets, r0, r1, sp, then FRAME_LEN words. */
int smcpad_reset_record(u32 out[4 + FRAME_LEN])
{
    if (!reset_valid) {
        return 0;
    }
    out[0] = captured_noinit.reset_rets;
    out[1] = captured_noinit.reset_r0;
    out[2] = captured_noinit.reset_r1;
    out[3] = captured_noinit.reset_sp;
    for (int i = 0; i < FRAME_LEN; i++) {
        out[4 + i] = captured_noinit.frame[i];
    }
    return 4 + FRAME_LEN;
}

/*
 * Every SDK ASSERT, and exception_analyze() itself (which LTO reduced to a jump here), ends in
 * the mask ROM's chip_reset: an immediate reset that leaves RAM alone -- no watchdog, and the
 * reset-source registers are cleared before we can read them. fw/build.sh points the
 * `chip_reset` symbol here instead; this records who called and goes on to the ROM's.
 * In RAM, and touching nothing but RAM: it may be called from anywhere.
 */
extern void smcpad_rom_chip_reset(u32 r0, u32 r1);     /* = 0x117fcc, from the linker */

AT_VOLATILE_RAM_CODE __attribute__((noinline, used)) void smcpad_chip_reset(u32 r0, u32 r1)
{
    volatile struct noinit *n = NOINIT;
    u32 ra, sp;

    __asm__ volatile("%0 = rets" : "=r"(ra));
    __asm__ volatile("%0 = sp" : "=r"(sp));
    n->reset_rets = ra;
    n->reset_r0 = r0;
    n->reset_r1 = r1;
    n->reset_sp = sp;
    for (int i = 0; i < FRAME_LEN; i++) {
        n->frame[i] = (r0 >= 0x10000 && r0 < 0x31F00 - 4 * FRAME_LEN && !(r0 & 3))
                      ? ((volatile u32 *)r0)[i] : 0;
    }
    n->reset_magic = RESET_MAGIC;
    smcpad_rom_chip_reset(r0, r1);
}

/* The coarse ring from before the last boot, oldest first: pc[i], stat[i]. 0 if none. */
int smcpad_coarse_get(u32 pc[COARSE_LEN], u32 stat[COARSE_LEN])
{
    u32 head = captured_noinit.coarse_head;

    if (!ring_valid || head >= COARSE_LEN) {
        return 0;
    }
    for (int i = 0; i < COARSE_LEN; i++) {
        pc[i] = captured_noinit.coarse_pc[(head + i) % COARSE_LEN];
        stat[i] = captured_noinit.coarse_stat[(head + i) % COARSE_LEN];
    }
    return COARSE_LEN;
}

/* Live view for the console: the ring as it is now, oldest first. */
void smcpad_pcsample_live(u32 out[RING_LEN])
{
    volatile struct noinit *n = NOINIT;
    u32 slot = n->ring_head & 0xFF;

    for (int i = 0; i < RING_LEN; i++) {
        out[i] = n->ring[(slot + i) % RING_LEN];
    }
}

/*
 * The USB interrupt, re-registered through a wrapper: the SDK's usb0_g_isr is nothing but
 * `call usb_isr` between register saves, so this does the same and also marks the breadcrumb
 * word (+0) while inside -- a reset with the mark still set means the chip hung inside the
 * USB interrupt. Registered at a lower priority than the SDK's 3, so the sampler's timer
 * (priority 1) can still interrupt it and record where in usb_isr it is stuck.
 */
#define USB_MARK        (*(volatile u32 *)((u32)&UPDATA_BEG))
#define USB_MARK_VALUE  0x5CC10000u

extern void usb_isr(void);

___interrupt
static void usb_isr_marked(void)
{
    u32 previous = USB_MARK;

    USB_MARK = USB_MARK_VALUE;
    smcpad_usb_irq_count++;
    usb_isr();
    USB_MARK = previous;
}

void smcpad_usb_isr_wrap(u8 priority)
{
    request_irq(IRQ_USB_CTRL_IDX, priority, usb_isr_marked, 0);
}

/* What the PMU / system say reset the chip, read once early in the boot. The libraries'
 * dump functions return a code (and print on the log UART we do not have). */
extern u8 power_reset_source_dump(void);
extern u32 reset_source_dump(void);

static u32 reset_pmu, reset_sys;

void smcpad_reset_source_capture(void)
{
    reset_pmu = power_reset_source_dump();
    reset_sys = reset_source_dump();
}

void smcpad_reset_source_get(u32 *pmu, u32 *sys)
{
    *pmu = reset_pmu;
    *sys = reset_sys;
}

/* Diagnostic: move the USB interrupts (whatever handlers the SDK installed) to priority 2, under
 * the sampler's 3, so a hang inside them is sampled instead of freezing the sampler too. */
void smcpad_usb_irq_demote(void)
{
    request_irq(IRQ_USB_CTRL_IDX, 2, (void (*)(void))ISR_BASE[IRQ_USB_CTRL_IDX], 0);
    request_irq(IRQ_USB_SOF_IDX, 2, (void (*)(void))ISR_BASE[IRQ_USB_SOF_IDX], 0);
}
