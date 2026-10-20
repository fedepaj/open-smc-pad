/*
 * Internal SPI flash: erase and program, with every wait bounded.
 *
 * The SDK's own path (sfc_erase / norflash_write -> norflash_erase / _norflash_write) is
 * already RAM-resident and runs with interrupts masked, but none of its waits has a timeout,
 * and on this firmware one of them occasionally never ends: the chip then sits with
 * interrupts off until the 8 s watchdog resets it (measured: 7.8 s every time, in erases and
 * in writes alike). A hang cannot be retried -- the call never returns.
 *
 * This does the same register sequence the SDK does (read off its disassembly: sfc_suspend,
 * spi_cs, spi_writebyte/readbyte, norflash_write_enable, norflash_wait_ok, exit_spi_code),
 * minus its timer-based __udelay, and gives up on any wait that runs too long: the SFC is
 * put back, interrupts come back on, and the caller gets told which wait it was and can
 * simply try again.
 *
 * Constraints, all because the flash is unmapped while we talk to it over SPI0:
 *  - everything that runs between "SFC off" and "SFC back on" lives in .volatile_ram_code
 *    (copied to RAM by the crt0), and calls nothing that lives in flash. clr_wdt() is in
 *    flash, so it is not called; the bounds below keep a stuck wait far short of 8 s.
 *  - no switch statements or constant tables in that code: they would end up in .rodata,
 *    i.e. in flash.
 *  - the data to program must be in RAM, and one call programs within one 256-byte page.
 *  - sfc_resume() and sfc_drop_cache() are mask-ROM routines, so they are safe to call from
 *    there -- and sfc_drop_cache() ONLY there: SFC suspended, interrupts off (see
 *    flash_op_ram()).
 */
#include "system/includes.h"
#include "asm/power_interface.h"            /* AT_VOLATILE_RAM_CODE */

#define RAMFN   AT_VOLATILE_RAM_CODE __attribute__((noinline))

#define REG(a)          (*(volatile u32 *)(a))
#define SFC_CON         REG(0x1F0200)       /* JL_SFC->CON: bit0 enable, bit31 busy, 8..11 mode */
#define SPI0_CON        REG(0x1E1C00)       /* JL_SPI0->CON */
#define SPI0_BUF        REG(0x1E1C08)       /* JL_SPI0->BUF */
#define FLASH_PORT_SEL  REG(0x1E511C)       /* bit5: flash on the alternate port (spi_get_port) */
#define SFC_IDLE_STAT   REG(0x101000)       /* bit5 set when the SFC side may be suspended */

#define PORT_OUT(base)  REG((base) + 0x00)
#define PORT_DIR(base)  REG((base) + 0x08)
#define PORT_PU(base)   REG((base) + 0x10)
#define PORTC_BASE      0x1E5080
#define PORTD_BASE      0x1E50C0

#define CMD_WREN        0x06
#define CMD_RDSR        0x05
#define CMD_PP          0x02
#define CMD_SE          0x20                /* 4 KB sector erase */

#define PAGE_SIZE       256
#define SECTOR_SIZE     4096

/* Iteration bounds. Each spin is a handful of cycles, so SPIN_LIMIT is well under a second.
 * A status poll is a few SPI bytes plus a short pause (tens of microseconds), so WIP_POLLS
 * covers a second or two -- a 4 KB erase takes milliseconds -- and ends long before the
 * 8 s watchdog. (The first version allowed 200000 polls, which at this clock is over 8 s:
 * a flash that never reported ready would still have ended in a watchdog reset.) */
#define SPIN_LIMIT      2000000u
#define WIP_POLLS       20000u
#define PROBE_LIMIT     100000u    /* re-reads of the XIP probe after resume */
#define CMD_RDID        0x9F

/*
 * What the flash said during the last operation, to settle whether its status register can
 * be trusted: after WREN, SR must have WEL (bit 1) set; right after an erase or program
 * command it must report WIP (bit 0) for a while. A status that reads "idle" immediately
 * means we are about to hand the flash back to the SFC while it is still busy -- and the
 * next instruction fetch from flash returns garbage.
 */
struct flash_diag {
    u8 id[3];
    u8 sr_before;
    u8 sr_after_wren;
    u8 sr_first;            /* first status read after the erase/program command */
    u8 sr_last;
    u32 polls;
    /* XIP check after sfc_resume: a known word of our own code, read as data (so garbage is
     * recorded, not executed) before the op and again right after the SFC comes back. */
    u32 probe_before;
    u32 probe_after;        /* first read after resume */
    u32 probe_spins;        /* re-reads needed until it matched again */
    /* Marginal-read detector: a checksum of SUM_LEN bytes of our code fetched through the SFC
     * (cache dropped each time) -- twice before the op as a baseline, SUM_ROUNDS times after
     * the resume. A flash clocked too fast gives sums that differ now and then. */
    u32 sum_ref;
    u8 sum_base_bad;        /* the two baseline sums disagreed: reads marginal even before */
    u8 sum_bad;             /* after-resume rounds that disagreed with sum_ref */
};
static struct flash_diag diag;

/* Survives a reset, next to the console's breadcrumb (BOOT_STATUS_ADDR has 8 reserved bytes;
 * the breadcrumb uses the first 4):
 *   bit 31     the first XIP read after resume was wrong
 *   bit 30     it never came right within the bound
 *   bits 29..24 variant used
 *   bit 23     baseline checksums disagreed (reads marginal before the op too)
 *   bits 19..16 after-resume checksum rounds that were wrong
 *   bits 15..0  probe_spins, clamped (0xFFFF: died before the XIP check finished) */
extern u32 UPDATA_BEG;
#define DIAG_PERSIST    (*(volatile u32 *)((u32)&UPDATA_BEG + 4))

/* Where the probe reads: the first word of .text, the crt0's `sp = ...` (ee ff 00 12). */
#define XIP_PROBE       0x1E00120u
#define XIP_CODE_BEGIN  0x1E00000u
#define XIP_CODE_LEN    0x80000u
#define SUM_LEN         0x2000u
#define SUM_ROUNDS      4

/* Bit 0: invalidate the whole code cache before returning to flash, in case lines filled
 * while the SFC was suspended were cached with garbage. Set from the console for A/B runs. */
static u32 variant;

enum {
    FLASH_OK = 0,
    FLASH_ERR_SFC_STAT = -1,        /* 0x101000 bit5 never set */
    FLASH_ERR_SFC_BUSY = -2,        /* SFC never went idle */
    FLASH_ERR_SPI = -3,             /* an SPI0 byte never completed */
    FLASH_ERR_WIP = -4,             /* the flash stayed busy */
    FLASH_ERR_MODE = -5,            /* SFC in continuous-read mode: not handled here */
    FLASH_ERR_ARG = -6,
};

extern void sfc_resume(u32 arg);                        /* mask ROM */
extern void sfc_drop_cache(u32 cpu_addr, u32 len);      /* mask ROM */
extern void local_irq_disable(void);
extern void local_irq_enable(void);
extern void clr_wdt(void);                              /* in flash: only outside the op */

/* Which wait gave up last, and how often each one has, for the console. The counts are the
 * diagnosis: they say which of the SDK's unbounded waits was the one hanging. */
static int last_error;
static u32 error_counts[7];

static RAMFN void spin_pause(u32 loops)
{
    for (volatile u32 i = 0; i < loops; i++) {
        /* busy, in RAM */
    }
}

static RAMFN u32 cs_port_base(u32 port)
{
    return port ? PORTC_BASE : PORTD_BASE;
}

static RAMFN u32 cs_bit(u32 port)
{
    return port ? (1u << 1) : (1u << 3);    /* PC1 or PD3, as spi_cs / sfc_suspend */
}

static RAMFN void cs_level(u32 port, int level)
{
    u32 base = cs_port_base(port);

    if (level) {
        PORT_OUT(base) |= cs_bit(port);
    } else {
        PORT_OUT(base) &= ~cs_bit(port);
    }
    spin_pause(4);
}

/* spi_writebyte / spi_readbyte, with spi_wait_ok bounded. */
static RAMFN int spi_xfer(u32 out, int receive, u8 *in)
{
    u32 n;

    SPI0_CON |= 0x8;
    if (receive) {
        SPI0_CON |= 0x1000;
    } else {
        SPI0_CON &= ~0x1000;
    }
    SPI0_BUF = out;
    for (n = 0; !(SPI0_CON & 0x8000); n++) {
        if (n > SPIN_LIMIT) {
            SPI0_CON &= ~0x8;
            return FLASH_ERR_SPI;
        }
    }
    if (receive) {
        *in = (u8)SPI0_BUF;
    }
    SPI0_CON |= 0x4000;                     /* clear pending */
    SPI0_CON &= ~0x8;
    return FLASH_OK;
}

static RAMFN int spi_cmd(u32 port, u32 cmd)
{
    int err;

    cs_level(port, 0);
    err = spi_xfer(cmd, 0, 0);
    cs_level(port, 1);
    return err;
}

/* Everything suspend_sfc changes, saved so resume_sfc_ours() can put back exactly that and
 * nothing else. */
static struct {
    u32 sfc_con, spi0_con, out, dir, pu;
} saved;

/* sfc_suspend: wait for the SFC to be idle, park CS high as a GPIO, SFC off, SPI0 on. */
static RAMFN int suspend_sfc(u32 port)
{
    u32 base = cs_port_base(port), bit = cs_bit(port), n;

    saved.sfc_con = SFC_CON;
    saved.spi0_con = SPI0_CON;
    saved.out = PORT_OUT(base);
    saved.dir = PORT_DIR(base);
    saved.pu = PORT_PU(base);

    for (n = 0; !(SFC_IDLE_STAT & 0x20); n++) {
        if (n > SPIN_LIMIT) {
            return FLASH_ERR_SFC_STAT;
        }
    }
    for (n = 0; SFC_CON & 0x80000000u; n++) {
        if (n > SPIN_LIMIT) {
            return FLASH_ERR_SFC_BUSY;
        }
    }
    PORT_PU(base) |= bit;
    PORT_DIR(base) |= bit;
    SFC_CON &= ~1u;
    PORT_OUT(base) |= bit;
    PORT_DIR(base) &= ~bit;
    PORT_PU(base) &= ~bit;
    SPI0_CON |= 1u;
    return FLASH_OK;
}

static RAMFN int read_status(u32 port, u8 *sr)
{
    int err;

    *sr = 0xEE;                             /* recognisable if the read never happens */
    cs_level(port, 0);
    err = spi_xfer(CMD_RDSR, 0, 0);
    if (!err) {
        err = spi_xfer(0xFF, 1, sr);
    }
    cs_level(port, 1);
    return err;
}

static RAMFN int read_id(u32 port)
{
    int err;

    cs_level(port, 0);
    err = spi_xfer(CMD_RDID, 0, 0);
    for (u32 i = 0; !err && i < 3; i++) {
        err = spi_xfer(0xFF, 1, &diag.id[i]);
    }
    cs_level(port, 1);
    return err;
}

/*
 * The mirror image of suspend_sfc, instead of the mask ROM's sfc_resume(). suspend_sfc never
 * touches JL_SFC->BAUD (the flash clock divider, which the SDK reprograms through
 * __hw_spi_clk_div whenever the system clock changes); if sfc_resume() restores a divider or
 * mode of its own -- one meant for the boot-time clock -- the flash ends up clocked too fast
 * and reads go marginal: right almost always, wrong now and then. This puts back only what
 * suspend_sfc changed, in reverse order, CS held high by its pull-up throughout.
 */
static RAMFN void resume_sfc_ours(u32 port)
{
    u32 base = cs_port_base(port), bit = cs_bit(port);

    SPI0_CON = saved.spi0_con;
    PORT_PU(base) |= bit;
    PORT_DIR(base) |= bit;
    SFC_CON = saved.sfc_con;
    PORT_OUT(base) = (PORT_OUT(base) & ~bit) | (saved.out & bit);
    PORT_DIR(base) = (PORT_DIR(base) & ~bit) | (saved.dir & bit);
    PORT_PU(base) = (PORT_PU(base) & ~bit) | (saved.pu & bit);
}

static RAMFN int wait_not_busy(u32 port)
{
    for (u32 polls = 0; polls < WIP_POLLS; polls++) {
        u8 sr;
        int err = read_status(port, &sr);

        if (err) {
            return err;
        }
        if (polls == 0) {
            diag.sr_first = sr;
        }
        diag.sr_last = sr;
        diag.polls = polls;
        if (!(sr & 1)) {
            return FLASH_OK;
        }
        spin_pause(200);
    }
    return FLASH_ERR_WIP;
}

/*
 * One complete operation with the SFC suspended. `cmd` is CMD_SE or CMD_PP; for CMD_PP,
 * `data`/`len` are the bytes to program (RAM, one page at most). Interrupts must already be
 * masked by the caller.
 *
 * The cache lines of the range are dropped HERE, with the SFC still suspended and interrupts
 * off, exactly as the SDK's flushinv_dcache_api / norflash_erase do. Calling the ROM's
 * sfc_drop_cache from flash code with interrupts on -- which the first versions of this
 * driver did right after every operation, and the console's `R` did on every read -- lets an
 * interrupt (whose code is in flash) fetch through the cache while it is being invalidated:
 * the chip then runs garbage, and that was the "random death a few seconds after a flash
 * operation" (and the USB console dying after `R`), not the flash or the SFC.
 */
static RAMFN int flash_op_ram(u32 port, u32 cmd, u32 addr, const u8 *data, u32 len,
                              u32 drop_cpu, u32 drop_len)
{
    int err = suspend_sfc(port);

    if (err) {
        return err;                         /* SFC never stopped: nothing to undo */
    }

    diag.sr_before = diag.sr_after_wren = diag.sr_first = diag.sr_last = 0xEE;
    diag.polls = 0;
    err = read_id(port);
    if (!err) err = read_status(port, &diag.sr_before);
    if (!err) err = spi_cmd(port, CMD_WREN);
    if (!err) err = read_status(port, &diag.sr_after_wren);
    if (!err) {
        cs_level(port, 0);
        err = spi_xfer(cmd, 0, 0);
        if (!err) err = spi_xfer((addr >> 16) & 0xFF, 0, 0);
        if (!err) err = spi_xfer((addr >> 8) & 0xFF, 0, 0);
        if (!err) err = spi_xfer(addr & 0xFF, 0, 0);
        for (u32 i = 0; !err && i < len; i++) {
            err = spi_xfer(data[i], 0, 0);
        }
        cs_level(port, 1);
    }
    if (!err) {
        err = wait_not_busy(port);
    }

    sfc_drop_cache(drop_cpu, drop_len);
    if (variant & 1) {
        sfc_drop_cache(XIP_CODE_BEGIN, XIP_CODE_LEN);
    }

    /* exit_spi_code: hand the pins back to the SFC, always -- even after an error. */
    if (variant & 2) {
        resume_sfc_ours(port);
    } else {
        sfc_resume(1);
    }
    return err;
}

/* Status registers 1 and 2 (RDSR 0x05 / 0x35), for the console's `S`: whether the SDK has
 * block-protected the flash (BP bits SR1 0x7C, CMP SR2 0x40). */
RAMFN int smcpad_flash_read_sr(u8 *sr1, u8 *sr2)
{
    u32 port = (FLASH_PORT_SEL >> 5) & 1;
    int err;

    local_irq_disable();
    err = suspend_sfc(port);
    if (!err) {
        err = read_status(port, sr1);
        if (!err) {
            cs_level(port, 0);
            err = spi_xfer(0x35, 0, 0);
            if (!err) err = spi_xfer(0xFF, 1, sr2);
            cs_level(port, 1);
        }
        sfc_resume(1);
    }
    local_irq_enable();
    return err;
}

/* Drop the cache lines of a flash range, the way flushinv_dcache_api does: SFC suspended,
 * interrupts off, from RAM. For readers of flash we have just written. */
RAMFN int smcpad_flash_drop_cache(u32 addr, u32 len)
{
    u32 port = (FLASH_PORT_SEL >> 5) & 1;
    int err;

    local_irq_disable();
    err = suspend_sfc(port);
    if (!err) {
        sfc_drop_cache(0x1E00000 + addr - 0x1000, len);
        sfc_resume(1);
    }
    local_irq_enable();
    return err;
}

/* ---------------------------------------------------------------- callable from flash */

static u32 flash_to_cpu(u32 addr);
/*
 * Bisection of the post-operation death: each mode adds one step of a real erase, with the
 * same masked-interrupt window around it. Whatever mode first kills the chip names the step.
 *   0  interrupts masked, spin (no SFC, no flash)
 *   1  + SFC suspended and resumed, nothing sent to the flash
 *   2  + status register polled over SPI0 while suspended
 *   3  + WREN, then WRDI (write enable latched and dropped, no erase)
 *   4  + a real 4 KB sector erase at `addr`
 */
#define CMD_WRDI        0x04
#define HOLD_POLLS      100

static RAMFN int experiment_ram(u32 port, u32 mode, u32 addr)
{
    int err = FLASH_OK;
    u8 sr;

    if (mode == 0) {
        for (u32 i = 0; i < HOLD_POLLS; i++) {
            spin_pause(400);
        }
        return FLASH_OK;
    }
    err = suspend_sfc(port);
    if (err) {
        return err;
    }
    if (mode == 1) {
        for (u32 i = 0; i < HOLD_POLLS; i++) {
            spin_pause(400);
        }
    } else if (mode == 2) {
        for (u32 i = 0; !err && i < HOLD_POLLS; i++) {
            err = read_status(port, &sr);
            spin_pause(200);
        }
    } else if (mode == 3) {
        err = spi_cmd(port, CMD_WREN);
        if (!err) err = read_status(port, &diag.sr_after_wren);
        if (!err) err = spi_cmd(port, CMD_WRDI);
        for (u32 i = 0; !err && i < HOLD_POLLS; i++) {
            err = read_status(port, &sr);
            spin_pause(200);
        }
    } else {
        err = spi_cmd(port, CMD_WREN);
        if (!err) {
            cs_level(port, 0);
            err = spi_xfer(CMD_SE, 0, 0);
            if (!err) err = spi_xfer((addr >> 16) & 0xFF, 0, 0);
            if (!err) err = spi_xfer((addr >> 8) & 0xFF, 0, 0);
            if (!err) err = spi_xfer(addr & 0xFF, 0, 0);
            cs_level(port, 1);
        }
        if (!err) err = wait_not_busy(port);
        sfc_drop_cache(0x1E00000 + addr - 0x1000, SECTOR_SIZE);
    }
    sfc_resume(1);
    return err;
}

int smcpad_flash_experiment(u32 mode, u32 addr)
{
    u32 port = (FLASH_PORT_SEL >> 5) & 1;
    int err;

    if (mode > 4 || (addr & (SECTOR_SIZE - 1))) {
        return FLASH_ERR_ARG;
    }
    local_irq_disable();
    err = experiment_ram(port, mode, addr);
    local_irq_enable();
    clr_wdt();
    return err;
}



static u32 flash_to_cpu(u32 addr)
{
    return 0x1E00000 + addr - 0x1000;       /* flash 0x1000 is mapped at 0x1E00000 */
}

static int run(u32 cmd, u32 addr, const u8 *data, u32 len, u32 drop_len, int attempts)
{
    u32 mode = (SFC_CON >> 8) & 0xF;
    u32 port = (FLASH_PORT_SEL >> 5) & 1;

    /* Modes 6/7 keep the flash in continuous-read mode, which would have to be exited and
     * re-entered around the operation (exit_continue_mode / enter_continue_mode). Not done
     * here; refuse rather than leave the SFC reading garbage. */
    if (mode == 6 || mode == 7) {
        last_error = FLASH_ERR_MODE;
        return FLASH_ERR_MODE;
    }

    for (int attempt = 1; attempt <= attempts; attempt++) {
        int err;

        local_irq_disable();
        err = flash_op_ram(port, cmd, addr, data, len, flash_to_cpu(addr), drop_len);
        local_irq_enable();
        clr_wdt();

        last_error = err;
        if (!err) {
            return attempt;                 /* > 0: success, after this many attempts */
        }
        if (-err < (int)ARRAY_SIZE(error_counts)) {
            error_counts[-err]++;
        }
    }
    return last_error;
}

/* How many attempts have given up on wait `err` (a FLASH_ERR_* value) since boot. */
u32 smcpad_flash_error_count(int err)
{
    return (err < 0 && -err < (int)ARRAY_SIZE(error_counts)) ? error_counts[-err] : 0;
}

/* Returns the number of attempts it took (> 0), or a FLASH_ERR_* (< 0). */
int smcpad_flash_erase_sector(u32 addr, int attempts)
{
    if (addr & (SECTOR_SIZE - 1)) {
        return FLASH_ERR_ARG;
    }
    return run(CMD_SE, addr, 0, 0, SECTOR_SIZE, attempts);
}

/* `data` must be in RAM; the range must not cross a 256-byte page. */
int smcpad_flash_program(u32 addr, const u8 *data, u32 len, int attempts)
{
    if (!len || len > PAGE_SIZE || ((addr & (PAGE_SIZE - 1)) + len) > PAGE_SIZE) {
        return FLASH_ERR_ARG;
    }
    return run(CMD_PP, addr, data, len, len, attempts);
}

u32 smcpad_flash_sfc_con(void)
{
    return SFC_CON;
}

void smcpad_flash_set_variant(u32 v)
{
    variant = v;
}

/* XIP probe from the last operation: the word before, the first read after resume, and how
 * many re-reads it took to match again (0 = matched at once). */
void smcpad_flash_get_probe(u32 *before, u32 *after, u32 *spins)
{
    *before = diag.probe_before;
    *after = diag.probe_after;
    *spins = diag.probe_spins;
}

/* Checksum rounds: whether the two baseline sums disagreed, and how many of the
 * after-resume rounds were wrong (out of SUM_ROUNDS). */
void smcpad_flash_get_sums(u32 *base_bad, u32 *after_bad)
{
    *base_bad = diag.sum_base_bad;
    *after_bad = diag.sum_bad;
}

/* Last operation's view of the flash: id[3], then sr_before, sr_after_wren, sr_first,
 * sr_last; and how many status polls it took to see "not busy". */
void smcpad_flash_get_diag(u8 id[3], u8 sr[4], u32 *polls)
{
    for (int i = 0; i < 3; i++) {
        id[i] = diag.id[i];
    }
    sr[0] = diag.sr_before;
    sr[1] = diag.sr_after_wren;
    sr[2] = diag.sr_first;
    sr[3] = diag.sr_last;
    *polls = diag.polls;
}

/* The summary saved before the last SFC resume, if any, then cleared. Survives a reset. */
u32 smcpad_flash_take_persisted_diag(void)
{
    u32 value = DIAG_PERSIST;

    DIAG_PERSIST = 0;
    return value;
}

int smcpad_flash_last_error(void)
{
    return last_error;
}

const char *smcpad_flash_strerror(int err)
{
    if (err > 0) return "ok";
    if (err == FLASH_ERR_SFC_STAT) return "0x101000 bit5 never set (sfc_suspend's first wait)";
    if (err == FLASH_ERR_SFC_BUSY) return "SFC never went idle (sfc_suspend's second wait)";
    if (err == FLASH_ERR_SPI) return "an SPI0 byte never completed (spi_wait_ok)";
    if (err == FLASH_ERR_WIP) return "flash stayed busy (norflash_wait_ok)";
    if (err == FLASH_ERR_MODE) return "SFC is in continuous-read mode 6/7, not handled";
    if (err == FLASH_ERR_ARG) return "bad address or length";
    return "?";
}

/* ------------------------------------------------------------------ firmware update */

/*
 * Copy `len` bytes of flash from `src` to `dst` (erasing `erase_len` from `dst` first), check
 * the copy, and reset the chip -- all from RAM with interrupts off and the SFC suspended for
 * the whole job, because `dst` is the area the running firmware executes from. Never
 * returns; the result lands in the breadcrumb word (UPDATA_BEG+0, survives the reset) as
 * UPDATE_DONE_MAGIC | failed-ops count (low 8 bits) | mismatching pages (bits 8..15), for
 * the next boot to report.
 *
 * Staging is read back over SPI0 (READ 0x03), not through the SFC: the SFC is off. Both
 * ends are raw physical bytes, as download mode writes them. The watchdog is fed through the
 * ROM's p33_or_1byte, i.e. what wdt_clear() does, since clr_wdt() itself is in flash.
 */
#define CMD_READ            0x03
#define UPDATE_DONE_MAGIC   0x5CD00000u
#define UPDATE_TRIES        3

extern void smcpad_rom_chip_reset(u32 r0, u32 r1);          /* mask ROM, from fw/build.sh */

static u8 copy_buf[PAGE_SIZE] __attribute__((aligned(4)));
static u8 check_buf[PAGE_SIZE] __attribute__((aligned(4)));

static RAMFN void wdt_kick(void)
{
    p33_or_1byte(0x80, 0x40);
}

static RAMFN int spi_read(u32 port, u32 addr, u8 *buf, u32 len)
{
    int err;

    cs_level(port, 0);
    err = spi_xfer(CMD_READ, 0, 0);
    if (!err) err = spi_xfer((addr >> 16) & 0xFF, 0, 0);
    if (!err) err = spi_xfer((addr >> 8) & 0xFF, 0, 0);
    if (!err) err = spi_xfer(addr & 0xFF, 0, 0);
    for (u32 i = 0; !err && i < len; i++) {
        err = spi_xfer(0xFF, 1, &buf[i]);
    }
    cs_level(port, 1);
    return err;
}

/* WREN + one erase or page program + wait, already suspended. */
static RAMFN int raw_op(u32 port, u32 cmd, u32 addr, const u8 *data, u32 len)
{
    int err = spi_cmd(port, CMD_WREN);

    if (!err) {
        cs_level(port, 0);
        err = spi_xfer(cmd, 0, 0);
        if (!err) err = spi_xfer((addr >> 16) & 0xFF, 0, 0);
        if (!err) err = spi_xfer((addr >> 8) & 0xFF, 0, 0);
        if (!err) err = spi_xfer(addr & 0xFF, 0, 0);
        for (u32 i = 0; !err && i < len; i++) {
            err = spi_xfer(data[i], 0, 0);
        }
        cs_level(port, 1);
    }
    if (!err) {
        err = wait_not_busy(port);
    }
    return err;
}

static RAMFN int raw_op_retry(u32 port, u32 cmd, u32 addr, const u8 *data, u32 len)
{
    int err = 0;

    for (int t = 0; t < UPDATE_TRIES; t++) {
        err = raw_op(port, cmd, addr, data, len);
        if (!err) {
            break;
        }
    }
    return err;
}

static RAMFN __attribute__((noreturn)) void copy_and_reset_ram(u32 port, u32 dst, u32 src,
                                                               u32 len, u32 erase_len)
{
    u32 failed = 0, mismatched = 0;

    if (suspend_sfc(port)) {
        failed = 0xFF;                      /* nothing touched */
        goto out;
    }
    for (u32 a = 0; a < erase_len; a += SECTOR_SIZE) {
        if (raw_op_retry(port, CMD_SE, dst + a, 0, 0)) {
            failed++;
        }
        wdt_kick();
    }
    for (u32 a = 0; a < len; a += PAGE_SIZE) {
        u32 n = len - a < PAGE_SIZE ? len - a : PAGE_SIZE;

        if (spi_read(port, src + a, copy_buf, n) ||
            raw_op_retry(port, CMD_PP, dst + a, copy_buf, n)) {
            failed++;
        }
        wdt_kick();
    }
    for (u32 a = 0; a < len; a += PAGE_SIZE) {
        u32 n = len - a < PAGE_SIZE ? len - a : PAGE_SIZE;

        if (spi_read(port, src + a, copy_buf, n) || spi_read(port, dst + a, check_buf, n)) {
            failed++;
            continue;
        }
        for (u32 i = 0; i < n; i++) {
            if (copy_buf[i] != check_buf[i]) {
                mismatched++;
                break;
            }
        }
        wdt_kick();
    }
    sfc_resume(1);
out:
    *(volatile u32 *)&UPDATA_BEG = UPDATE_DONE_MAGIC | ((mismatched > 0xFF ? 0xFF : mismatched) << 8) |
                                   (failed > 0xFF ? 0xFF : failed);
    smcpad_rom_chip_reset(0, 0);
    for (;;) {
    }
}

/* Refuses ranges that are not sector aligned; otherwise does not return. */
int smcpad_flash_copy_and_reset(u32 dst, u32 src, u32 len, u32 erase_len)
{
    u32 port = (FLASH_PORT_SEL >> 5) & 1;
    u32 mode = (SFC_CON >> 8) & 0xF;

    if ((dst | src | erase_len) & (SECTOR_SIZE - 1) || len > erase_len || !len) {
        return FLASH_ERR_ARG;
    }
    if (mode == 6 || mode == 7) {
        return FLASH_ERR_MODE;
    }
    clr_wdt();
    local_irq_disable();
    copy_and_reset_ram(port, dst, src, len, erase_len);
}
