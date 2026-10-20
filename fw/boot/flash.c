/* Raw SPI NOR access for the DFU mode.
 *
 * The same register sequence as fw/smcpad/smcpad_flash.c (read off the SDK: sfc_suspend,
 * spi_cs, spi_writebyte/readbyte, norflash_write_enable, norflash_wait_ok), every wait
 * bounded.  The difference: the DFU mode runs entirely from RAM and never executes from flash
 * again (it ends in a chip reset), so the SFC is parked once in flash_begin() and SPI0 is ours
 * from then on -- no suspend/resume around each operation, no cache to keep coherent.
 *
 * Reads and writes are raw physical bytes: the SFC's decryption is not in the path.
 */
#include "boot.h"

#define SFC_CON         REG32(0x1F0200)     /* JL_SFC->CON: bit0 enable, bit31 busy, 8..11 mode */
#define SPI0_CON        REG32(0x1E1C00)
#define SPI0_BUF        REG32(0x1E1C08)
#define FLASH_PORT_SEL  REG32(0x1E511C)     /* bit5: flash on the alternate port */
#define SFC_IDLE_STAT   REG32(0x101000)     /* bit5 set when the SFC may be suspended */

#define PORT_OUT(base)  REG32((base) + 0x00)
#define PORT_DIR(base)  REG32((base) + 0x08)
#define PORT_PU(base)   REG32((base) + 0x10)
#define PORTC_BASE      0x1E5080u
#define PORTD_BASE      0x1E50C0u

#define CMD_WREN        0x06
#define CMD_RDSR        0x05
#define CMD_READ        0x03
#define CMD_PP          0x02
#define CMD_SE          0x20

#define SPIN_LIMIT      2000000u
#define WIP_POLLS       40000u

static u32 cs_base, cs_bit;

void wdt_kick(void)
{
    ROM_P33_OR_1BYTE(0x80, 0x40);           /* what the SDK's wdt_clear() does */
}

static void spin_pause(u32 loops)
{
    for (volatile u32 i = 0; i < loops; i++) {
    }
}

static void cs_level(int level)
{
    if (level) {
        PORT_OUT(cs_base) |= cs_bit;
    } else {
        PORT_OUT(cs_base) &= ~cs_bit;
    }
    spin_pause(4);
}

static int spi_xfer(u32 out, u8 *in)
{
    SPI0_CON |= 0x8;
    if (in) {
        SPI0_CON |= 0x1000;
    } else {
        SPI0_CON &= ~0x1000u;
    }
    SPI0_BUF = out;
    for (u32 n = 0; !(SPI0_CON & 0x8000); n++) {
        if (n > SPIN_LIMIT) {
            SPI0_CON &= ~0x8u;
            return -1;
        }
    }
    if (in) {
        *in = (u8)SPI0_BUF;
    }
    SPI0_CON |= 0x4000;                     /* clear pending */
    SPI0_CON &= ~0x8u;
    return 0;
}

static int send_cmd_addr(u32 cmd, u32 addr)
{
    if (spi_xfer(cmd, 0) || spi_xfer((addr >> 16) & 0xFF, 0) ||
        spi_xfer((addr >> 8) & 0xFF, 0) || spi_xfer(addr & 0xFF, 0)) {
        return -1;
    }
    return 0;
}

static int read_status(u8 *sr)
{
    int err;

    cs_level(0);
    err = spi_xfer(CMD_RDSR, 0) || spi_xfer(0xFF, sr);
    cs_level(1);
    return err ? -1 : 0;
}

static int wait_ready(void)
{
    for (u32 polls = 0; polls < WIP_POLLS; polls++) {
        u8 sr;

        if (read_status(&sr)) {
            return -1;
        }
        if (!(sr & 1)) {
            return 0;
        }
        if ((polls & 63) == 0) {
            wdt_kick();
        }
        spin_pause(200);
    }
    return -2;
}

static int write_enable(void)
{
    int err;

    cs_level(0);
    err = spi_xfer(CMD_WREN, 0);
    cs_level(1);
    return err;
}

/* Park the SFC for good: wait for it to be idle, CS as a GPIO held high, SFC off, SPI0 on. */
int flash_begin(void)
{
    u32 port = (FLASH_PORT_SEL >> 5) & 1, mode = (SFC_CON >> 8) & 0xF;

    if (mode == 6 || mode == 7) {
        return -5;                          /* continuous-read mode: not handled */
    }
    cs_base = port ? PORTC_BASE : PORTD_BASE;
    cs_bit = port ? (1u << 1) : (1u << 3);  /* PC1 or PD3, as the SDK's spi_cs */
    for (u32 n = 0; !(SFC_IDLE_STAT & 0x20); n++) {
        if (n > SPIN_LIMIT) {
            return -1;
        }
    }
    for (u32 n = 0; SFC_CON & 0x80000000u; n++) {
        if (n > SPIN_LIMIT) {
            return -2;
        }
    }
    PORT_PU(cs_base) |= cs_bit;
    PORT_DIR(cs_base) |= cs_bit;
    SFC_CON &= ~1u;
    PORT_OUT(cs_base) |= cs_bit;
    PORT_DIR(cs_base) &= ~cs_bit;
    PORT_PU(cs_base) &= ~cs_bit;
    SPI0_CON |= 1u;
    return 0;
}

int flash_erase_sector(u32 addr)
{
    int err = write_enable();

    if (!err) {
        cs_level(0);
        err = send_cmd_addr(CMD_SE, addr);
        cs_level(1);
    }
    return err ? err : wait_ready();
}

int flash_program(u32 addr, const u8 *data, u32 len)
{
    int err = write_enable();

    if (!err) {
        cs_level(0);
        err = send_cmd_addr(CMD_PP, addr);
        for (u32 i = 0; !err && i < len; i++) {
            err = spi_xfer(data[i], 0);
        }
        cs_level(1);
    }
    return err ? err : wait_ready();
}

/* Status registers 1 (cmd 0x05) and 2 (0x35). */
int flash_read_sr(u32 *sr1, u32 *sr2)
{
    u8 a = 0xEE, b = 0xEE;
    int err;

    cs_level(0);
    err = spi_xfer(CMD_RDSR, 0) || spi_xfer(0xFF, &a);
    cs_level(1);
    cs_level(0);
    err = err || spi_xfer(0x35, 0) || spi_xfer(0xFF, &b);
    cs_level(1);
    *sr1 = a;
    *sr2 = b;
    return err ? -1 : 0;
}

/* Clear the block protection the SDK's update_norflash_write_protect() manages: BP bits
 * (SR1 0x7C) and CMP (SR2 0x40), every other bit kept (QE above all). Non-volatile, as the
 * SDK's WREN path writes it: the next firmware sets its own protection at boot. */
int flash_unprotect(void)
{
    u32 sr1, sr2;
    int err;

    if (flash_read_sr(&sr1, &sr2)) {
        return -1;
    }
    if (!(sr1 & 0x7C) && !(sr2 & 0x40)) {
        return 0;
    }
    err = write_enable();
    if (!err) {
        cs_level(0);
        err = spi_xfer(0x01, 0) || spi_xfer(sr1 & ~0x7Cu, 0) || spi_xfer(sr2 & ~0x40u, 0);
        cs_level(1);
    }
    if (!err) {
        err = wait_ready();
    }
    if (!err && !flash_read_sr(&sr1, &sr2) && (sr2 & 0x40)) {
        /* chips that take SR2 only through its own command */
        err = write_enable();
        if (!err) {
            cs_level(0);
            err = spi_xfer(0x31, 0) || spi_xfer(sr2 & ~0x40u, 0);
            cs_level(1);
        }
        if (!err) {
            err = wait_ready();
        }
    }
    return err;
}

int flash_read(u32 addr, u8 *buf, u32 len)
{
    int err;

    cs_level(0);
    err = send_cmd_addr(CMD_READ, addr);
    for (u32 i = 0; !err && i < len; i++) {
        err = spi_xfer(0xFF, &buf[i]);
    }
    cs_level(1);
    return err;
}
