/* SMC-PAD bootloader, stage A: chain to the application slot.
 *
 * The factory uboot (sector 0) loads the app area at flash 0x1000 -- this bootloader -- and
 * jumps to its entry point with r0 = BOOT_DEVICE_INFO*.  The application lives in its own
 * JieLi app area (directory + app.bin + cfg_tool.bin) at flash APP_DIR, linked to run from
 * there (TEXT_BASE = 0x1E00000 + APP_DIR - 0x1000 + 0x120).
 *
 * The SDK finds its directory through BOOT_DEVICE_INFO.sfc: it walks the directory at the CPU
 * address app_addr and converts with flash = cpu - app_addr + sfc_base_addr (sdfile_init,
 * sdfile_cpu_addr2flash_addr in the SDK).  So handing over is: point both fields at the
 * application's directory -- what the uboot itself does when sector 0 points elsewhere -- and
 * jump to the entry the directory declares, before touching any hardware, so the application
 * starts in exactly the state the uboot leaves.
 *
 * No RAM is used before the jump except the stack the uboot left us.
 */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

#define APP_DIR         0x9000u             /* flash offset of the application's directory */
#define SFC_FLASH_BASE  0x1000u             /* flash offset mapped at CPU 0x1E00000 */
#define SFC_CPU_BASE    0x1E00000u

struct sfc_info {
    u32 sfc_base_addr;                      /* flash offset of the directory */
    u32 app_addr;                           /* CPU address of the directory */
};

struct boot_device_info {
    void *fs_info;                          /* struct flash_head *, from sector 0 */
    struct sfc_info sfc;
    u16 chip_id;
    u16 trim_value;
    u8 bt_mac_addr[8];
};

/* JieLi CRC16: CCITT polynomial 0x1021, init 0, not reflected (host/jlimage.py crc16). */
static u16 crc16(const u8 *p, u32 n)
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

static u32 rd32(const u8 *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

/* r0 = info, r1 = entry: enter the application with r0 as the uboot would pass it. */
__attribute__((naked, noreturn, noinline))
static void enter_app(struct boot_device_info *info, u32 entry)
{
    __asm__ volatile("goto r1");
}

/* Mask ROM chip reset (sdk.ld: chip_reset = ABSOLUTE(0x117fcc)). */
#define ROM_CHIP_RESET  ((void (*)(u32, u32))0x117fccu)

__attribute__((section(".start"), used, noreturn))
void boot_main(struct boot_device_info *info)
{
    const u8 *dir = (const u8 *)(SFC_CPU_BASE + APP_DIR - SFC_FLASH_BASE);
    u16 hdr_crc = dir[0] | dir[1] << 8;

    /* app_area_head: hdr_crc, data_crc, offset (= entry point), size, ... */
    if (crc16(dir + 2, 30) == hdr_crc) {
        u32 entry = rd32(dir + 4);

        info->sfc.app_addr = (u32)dir;
        info->sfc.sfc_base_addr = APP_DIR;
        enter_app(info, entry);
    }

    /* No application: reset.  The ROM checks for the USB_KEY first thing, so the Pico
     * (host/flash_fw.sh) can still take the pad into download mode. */
    ROM_CHIP_RESET(0, 0);
    for (;;) {
    }
}
