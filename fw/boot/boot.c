/* SMC-PAD bootloader: entry point and the decision, executed in place from flash.
 *
 * The factory uboot (sector 0) loads the app area at flash 0x1000 -- this bootloader -- and
 * jumps to its entry point with r0 = BOOT_DEVICE_INFO*.  The application lives in its own
 * JieLi app area (directory + app.bin + cfg_tool.bin) at flash APP_DIR, linked to run from
 * there (TEXT_BASE = APP_ENTRY).
 *
 * The SDK finds its directory through BOOT_DEVICE_INFO.sfc: it walks the directory at the CPU
 * address app_addr and converts with flash = cpu - app_addr + sfc_base_addr (sdfile_init,
 * sdfile_cpu_addr2flash_addr in the SDK).  So handing over is: point both fields at the
 * application's directory -- what the uboot itself does when sector 0 points elsewhere -- and
 * jump to the entry the directory declares, before touching any hardware, so the application
 * starts in exactly the state the uboot leaves.
 *
 * Otherwise the DFU mode starts: BOOT_MAGIC_DFU left by the application, no valid application,
 * shift + play held at power-up, a crash loop (BOOT_CRASH_LIMIT boots in a row the application
 * never confirmed), or an application area that fails its CRC. The last three are what keeps a
 * broken application from locking the pad out of USB. The DFU mode then starts: everything else of the bootloader is linked to run from RAM (boot.ld), copied
 * there from here, and from then on the flash is only data.
 *
 * Everything in this file lives in .start and must call nothing outside it: until the copy,
 * .text/.rodata are not in RAM yet.  No constant tables or switches (they would go to
 * .rodata), and the copy loops use volatile so that they do not become memcpy calls.
 */
#include "boot.h"

#define START   __attribute__((section(".start"), noinline))

struct sfc_info {
    u32 sfc_base_addr;                      /* flash offset of the directory */
    u32 app_addr;                           /* CPU address of the directory */
};

struct boot_device_info {
    void *fs_info;                          /* struct flash_head *, from sector 0 */
    struct sfc_info sfc;
    u16 chip_id;                            /* also the SFC cipher key */
    u16 trim_value;
    u8 bt_mac_addr[8];
};

extern u32 ram_load[], ram_begin[], ram_end[], bss_begin[], bss_end[], stack_top[];

/* JieLi CRC16: CCITT polynomial 0x1021, init 0, not reflected (host/jlimage.py crc16). */
START static u16 start_crc16(const volatile u8 *p, u32 n)
{
    u16 crc = 0;

    while (n--) {
        crc ^= (u16)(*p++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
        }
    }
    return crc;
}

START static u32 start_rd32(const volatile u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* CRC16-CCITT (XMODEM) a byte at a time without a table: same result as start_crc16, about
 * ten times faster, and nothing in .rodata. */
START static u16 start_crc16_fast(const volatile u8 *p, u32 n)
{
    u16 crc = 0;

    while (n--) {
        crc = (u16)((crc >> 8) | (crc << 8));
        crc ^= *p++;
        crc ^= (crc & 0xFF) >> 4;
        crc ^= (u16)(crc << 12);
        crc ^= (u16)((crc & 0xFF) << 5);
    }
    return crc;
}

/* The application's directory, through the SFC (decrypted). */
#define APP_DIR_CPU     ((const volatile u8 *)(SFC_CPU_BASE + APP_DIR - SFC_FLASH_BASE))

START static int app_valid(void)
{
    const volatile u8 *dir = APP_DIR_CPU;
    u32 size = start_rd32(dir + DIR_SIZE);

    return start_crc16(dir + 2, 30) == (dir[0] | dir[1] << 8) &&
           start_rd32(dir + DIR_OFFSET) == APP_ENTRY &&
           size > 0x120 && size <= APP_AREA_MAX;
}

/* The whole area against the directory's data_crc (everything after the head entry). */
START static int app_intact(void)
{
    const volatile u8 *dir = APP_DIR_CPU;
    u32 size = start_rd32(dir + DIR_SIZE);

    return start_crc16_fast(dir + 0x20, size - 0x20) == (dir[DIR_DATA_CRC] | dir[DIR_DATA_CRC + 1] << 8);
}

/* ---------------------------------------------------------------- shift + play at power-up
 * The key matrix as the console scans it (docs/hardware.md): two 74HC595 on PB6 (clock),
 * PB7 (data), PB4 (latch), /OE on PB5; a selected column is driven low, a pressed button pulls
 * the sense line PC1 low against its pull-up. Every GPIO register touched is put back, so the
 * application still starts from the uboot's state. */
#define PORTB(o)        REG32(0x1E5040u + (o))
#define PORTC(o)        REG32(0x1E5080u + (o))
#define P_OUT           0x00
#define P_IN            0x04
#define P_DIR           0x08
#define P_DIE           0x0C
#define P_PU            0x10
#define P_PD            0x14
#define B_SR_MASK       ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 7))
#define C_SENSE         (1u << 1)

START static void spin(u32 n)
{
    for (volatile u32 i = 0; i < n; i++) {
    }
}

START static void sr_write(u32 v)
{
    for (int bit = 15; bit >= 0; bit--) {
        PORTB(P_OUT) = (PORTB(P_OUT) & ~(1u << 7)) | ((v >> bit) & 1) << 7;
        PORTB(P_OUT) &= ~(1u << 6);
        PORTB(P_OUT) |= 1u << 6;
    }
    PORTB(P_OUT) &= ~(1u << 6);
    PORTB(P_OUT) |= 1u << 4;                /* latch */
    PORTB(P_OUT) &= ~(1u << 4);
    spin(2000);                             /* the pull-up has to win through the diodes */
}

START static int sense_low(u32 col_mask)
{
    sr_write(~col_mask & 0xFFFF);
    return !(PORTC(P_IN) & C_SENSE);
}

START static int dfu_keys_held(void)
{
    u32 b_out = PORTB(P_OUT), b_dir = PORTB(P_DIR), b_pu = PORTB(P_PU), b_pd = PORTB(P_PD);
    u32 c_dir = PORTC(P_DIR), c_die = PORTC(P_DIE), c_pu = PORTC(P_PU), c_pd = PORTC(P_PD);
    int held = 1;

    PORTB(P_PU) &= ~B_SR_MASK;
    PORTB(P_PD) &= ~B_SR_MASK;
    PORTB(P_OUT) &= ~B_SR_MASK;             /* /OE low: outputs on */
    PORTB(P_DIR) &= ~B_SR_MASK;
    PORTC(P_PD) &= ~C_SENSE;
    PORTC(P_PU) |= C_SENSE;
    PORTC(P_DIE) |= C_SENSE;
    PORTC(P_DIR) |= C_SENSE;

    /* three agreeing looks a few ms apart; a sense line low with no column selected is a
     * fault (or a LED phase driving it), not a key */
    for (int look = 0; look < 3 && held; look++) {
        held = !sense_low(0) && sense_low(1u << DFU_KEY_COL_A) && sense_low(1u << DFU_KEY_COL_B) &&
               !sense_low(1u << 15);
        spin(20000);
    }
    sr_write(0xFFFF);

    PORTB(P_OUT) = (PORTB(P_OUT) & ~B_SR_MASK) | (b_out & B_SR_MASK);
    PORTB(P_DIR) = (PORTB(P_DIR) & ~B_SR_MASK) | (b_dir & B_SR_MASK);
    PORTB(P_PU) = (PORTB(P_PU) & ~B_SR_MASK) | (b_pu & B_SR_MASK);
    PORTB(P_PD) = (PORTB(P_PD) & ~B_SR_MASK) | (b_pd & B_SR_MASK);
    PORTC(P_DIR) = (PORTC(P_DIR) & ~C_SENSE) | (c_dir & C_SENSE);
    PORTC(P_DIE) = (PORTC(P_DIE) & ~C_SENSE) | (c_die & C_SENSE);
    PORTC(P_PU) = (PORTC(P_PU) & ~C_SENSE) | (c_pu & C_SENSE);
    PORTC(P_PD) = (PORTC(P_PD) & ~C_SENSE) | (c_pd & C_SENSE);
    return held;
}

/* One more boot that the application has not confirmed yet; true when there were too many. */
START static int crash_loop(void)
{
    u32 n = BOOT_COUNT[0];

    if ((n ^ BOOT_COUNT_CHECK) != BOOT_COUNT[1]) {
        n = 0;                              /* power-up garbage, or cleared by the application */
    }
    n++;
    BOOT_COUNT[0] = n;
    BOOT_COUNT[1] = n ^ BOOT_COUNT_CHECK;
    return n > BOOT_CRASH_LIMIT;
}

START static void crash_count_clear(void)
{
    BOOT_COUNT[0] = 0;
    BOOT_COUNT[1] = 0;
}

/* r0 = info, r1 = entry: enter the application with r0 as the uboot would pass it. */
__attribute__((section(".start"), naked, noreturn, noinline))
static void enter_app(struct boot_device_info *info, u32 entry)
{
    __asm__ volatile("goto r1");
}

/* r0..r2 = dfu_start's arguments, r3 = the new stack: off the uboot's stack before the copy
 * to RAM overwrites who knows what. */
__attribute__((section(".start"), naked, noreturn, noinline))
static void switch_stack(u32 chip_key, u32 reason, u32 valid, u32 sp_top)
{
    __asm__ volatile("sp = r3\n\t"
                     "ssp = r3\n\t"
                     "goto dfu_start");
}

__attribute__((section(".start"), used, noreturn, noinline))
void dfu_start(u32 chip_key, u32 reason, u32 valid)
{
    volatile u32 *src = ram_load, *dst = ram_begin;

    while (dst < ram_end) {
        *dst++ = *src++;
    }
    for (dst = bss_begin; dst < bss_end; ) {
        *dst++ = 0;
    }
    /* through a register: RAM is out of reach of a direct call from flash */
    void (*volatile ram_main)(u32, u32, u32) = dfu_main;
    ram_main(chip_key, reason, valid);
    for (;;) {
    }
}

/* The uboot jumps to the first byte of app.bin: boot.ld puts .start.entry there. */
__attribute__((section(".start.entry"), used, noreturn))
void boot_main(struct boot_device_info *info)
{
    volatile u32 *magic = (volatile u32 *)BOOT_MAGIC_ADDR;
    u32 reason = 0, valid = app_valid();

    if (*magic == BOOT_MAGIC_DFU) {
        *magic = 0;                         /* once: a reset in DFU mode goes back to the app */
        reason = DFU_REASON_MAGIC;
    } else if (!valid) {
        reason = DFU_REASON_NO_APP;
    } else if (dfu_keys_held()) {
        reason = DFU_REASON_KEYS;
    } else if (crash_loop()) {
        reason = DFU_REASON_CRASH;
    } else if (!app_intact()) {
        reason = DFU_REASON_BAD_CRC;
        valid = 0;                          /* not worth going back to */
    }
    if (!reason) {
        info->sfc.app_addr = (u32)APP_DIR_CPU;
        info->sfc.sfc_base_addr = APP_DIR;
        enter_app(info, APP_ENTRY);
    }
    crash_count_clear();                    /* in DFU mode: the count starts over */
    switch_stack(info->chip_id, reason, valid, (u32)stack_top);
}
