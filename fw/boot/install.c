/* SMC-PAD installer: puts a bootloader at 0x1000 without the Pico or download mode -- in place
 * of the stock firmware (written by host/install_from_stock.py through the stock firmware), or
 * in place of an older bootloader (written by the bootloader itself, DFU alt 2; erase_end =
 * APP_DIR, so the application stays).
 *
 * host/install_from_stock.py, talking to the *stock* firmware over USB-MIDI (SINCO), writes
 * this installer as an app area at flash INSTALL_DIR, the bootloader's app area (plaintext)
 * into a staging area behind it, and points sector 0's app_dir_head at INSTALL_DIR.  At the
 * next power-up the uboot runs us instead of the stock firmware, which never runs again:
 *
 *   1. check the staging area: header CRC, payload CRC32, and the payload being an app area
 *      for 0x1000 whose own directory CRCs hold;
 *      bad -> point sector 0 back at 0x1000 and reset: the stock firmware, still intact,
 *      boots again and the host can retry.
 *   2. erase 0x1000..erase_end (the stock firmware, or just the old bootloader), write the
 *      bootloader there, encrypted with the chip key, and read it back;
 *      a failure or a power cut here just runs the installer again at the next boot.
 *   3. point sector 0's app_dir_head back at 0x1000 (erase + program + verify; the only window
 *      in which losing power needs the Pico), erase the installer and staging, reset.
 *
 * The bootloader then finds no application and comes up in DFU mode for host/dfu_flash.sh.
 *
 * Like the bootloader, the part in .start runs in place and makes nothing but the move to
 * RAM; everything else runs from RAM with the SFC parked (flash.c).
 */
#include <string.h>
#include "boot.h"


/* What the installer did, for the application's `I` (next to the bootloader's diagnostics). */
#define INSTALL_DIAG        ((volatile u32 *)0x31AC8u)
#define INSTALL_DIAG_MAGIC  0x1A57A000u         /* | step << 4 | result */

#define START   __attribute__((section(".start"), noinline))

/* Breadcrumb in RAM that survives a reset and download mode: how far the last run got. */
#define CRUMB               (*(volatile u32 *)0x31AD0u)

extern u32 ram_load[], ram_begin[], ram_end[], bss_begin[], bss_end[], stack_top[];

void install_main(u32 chip_key);

__attribute__((section(".start"), naked, noreturn, noinline))
static void switch_stack(u32 chip_key, u32 sp_top)
{
    __asm__ volatile("sp = r1\n\t"
                     "ssp = r1\n\t"
                     "goto install_start");
}

__attribute__((section(".start"), used, noreturn, noinline))
void install_start(u32 chip_key)
{
    volatile u32 *src = ram_load, *dst = ram_begin;

    CRUMB = 0xC0DE0002;                     /* on our stack */
    while (dst < ram_end) {
        *dst++ = *src++;
    }
    for (dst = bss_begin; dst < bss_end; ) {
        *dst++ = 0;
    }
    CRUMB = 0xC0DE0003;                     /* copied */
    void (*volatile ram_main)(u32) = install_main;
    ram_main(chip_key);
    for (;;) {
    }
}

/* Entry point: r0 = BOOT_DEVICE_INFO * from the uboot; chip_id is at +12. */
__attribute__((section(".start.entry"), used, noreturn))
void installer_entry(u8 *info)
{
    CRUMB = 0xC0DE0001;                     /* the uboot ran us */
    switch_stack(info[12] | info[13] << 8, (u32)stack_top);
}

/* ------------------------------------------------------------------------- RAM part */

static u32 chip_key;
static u8 sector[SECTOR_SIZE] __attribute__((aligned(4)));

/* What happened, also in flash (DIAG_PAGE, inside the area the installer erases when it is
 * done), since a failed install has nothing to show it with: read it in download mode. */
#define DIAG_PAGE           0x4C000u
#define DIAG_PAGE_MAGIC     0x31474449u         /* "IDG1" */
static u32 diag_words[8];

static void diag(u32 step, u32 result)
{
    INSTALL_DIAG[0] = INSTALL_DIAG_MAGIC | step << 4 | (result & 0xF);
    diag_words[0] = DIAG_PAGE_MAGIC;
    diag_words[1] = step << 4 | (result & 0xF);
}

static void diag_save(void)
{
    flash_erase_sector(DIAG_PAGE);
    flash_program(DIAG_PAGE, (const u8 *)diag_words, sizeof(diag_words));
}

static __attribute__((noreturn)) void reset(void)
{
    diag_save();
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}

void install_main(u32 key)
{
    struct stage_header h;

    CRUMB = 0xC0DE0004;                     /* in RAM */
    ROM_IRQ_DISABLE();
    wdt_kick();
    chip_key = key;
    diag(1, 0);
    if (flash_begin()) {
        diag(1, 1);
        reset();                            /* try again from the top */
    }
    /* The stock firmware leaves the flash block-protected: erases and writes would be ignored
     * without a word. Status registers before and after go in the diagnostics. */
    flash_read_sr(&diag_words[2], &diag_words[3]);
    diag_words[6] = (u32)flash_unprotect();
    flash_read_sr(&diag_words[4], &diag_words[5]);

    /* 1. the staging area */
    if (stage_check(&h)) {
        diag(1, 2);
        set_boot_dir(DEST);                 /* back to whatever was there, untouched */
        reset();
    }

    /* 2. the stock firmware goes, the bootloader comes */
    diag(2, 0);
    for (u32 a = DEST; a < h.erase_end; a += SECTOR_SIZE) {
        if (flash_erase_sector(a)) {
            diag(2, 1);
            reset();
        }
        wdt_kick();
    }
    for (u32 off = 0; off < h.payload_len; off += SECTOR_SIZE) {
        u32 n = h.payload_len - off < SECTOR_SIZE ? h.payload_len - off : SECTOR_SIZE;

        if (flash_read(STAGE + h.payload_off + off, sector, n)) {
            diag(2, 2);
            reset();
        }
        sfc_cipher(sector, n, DEST + off, chip_key);
        if (flash_write_sector(DEST + off, sector, n)) {
            diag(2, 3);
            reset();
        }
    }

    /* 3. boot it from now on */
    diag(3, 0);
    if (set_boot_dir(DEST)) {
        diag(3, 1);
        reset();
    }
    for (u32 a = INSTALL_DIR; a < STAGE_END; a += SECTOR_SIZE) {
        flash_erase_sector(a);
        wdt_kick();
    }
    diag(3, 0xF);                           /* done: RAM record only, the flash page could be
                                             * inside a large application by now */
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}
