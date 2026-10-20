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
 * Otherwise (BOOT_MAGIC_DFU left by the application, or no valid application) the DFU mode
 * starts: everything else of the bootloader is linked to run from RAM (boot.ld), copied
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
    }
    if (!reason) {
        info->sfc.app_addr = (u32)APP_DIR_CPU;
        info->sfc.sfc_base_addr = APP_DIR;
        enter_app(info, APP_ENTRY);
    }
    switch_stack(info->chip_id, reason, valid, (u32)stack_top);
}
