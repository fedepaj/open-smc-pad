/* SMC-PAD bootloader.
 *
 * .start runs in place from flash (it is app.bin of the app area at 0x1000, entry at its first
 * byte) and makes the boot decision.  Everything else is the DFU mode: linked to run from RAM,
 * stored right after .start, copied by dfu_start().  RAM 0x12000..0x30000 is free for it: the
 * DFU mode always ends in a chip reset, and stays clear of the ROM's RAM (0x31B60 up) and of
 * what must survive the reset (0x316E0..0x31AE8: the application's post-mortem records, the
 * bootloader's diagnostics, the DFU magic).
 */
MEMORY
{
    code (rx)  : ORIGIN = 0x1E00120, LENGTH = 0x8000 - 0x120
    ram  (rwx) : ORIGIN = 0x12000,   LENGTH = 0x1E000
}

ENTRY(boot_main)

SECTIONS
{
    .start :
    {
        KEEP(*(.start.entry))
        ASSERT(boot_main == ORIGIN(code), "boot_main must be the entry point");
        KEEP(*(.start))
        . = ALIGN(4);
        start_end = .;
    } > code

    .ram :
    {
        ram_begin = .;
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        . = ALIGN(4);
        ram_end = .;
    } > ram AT > code
    ram_load = LOADADDR(.ram);

    .bss (NOLOAD) :
    {
        bss_begin = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        bss_end = .;
    } > ram

    stack_top = ORIGIN(ram) + LENGTH(ram);

    /* boot.bin is .start then .ram, back to back, and must fit in front of the application */
    ASSERT(ram_load == start_end, ".ram is not stored right after .start")
    ASSERT(start_end + (ram_end - ram_begin) <= 0x1E08000 - 0x400, "bootloader too big (cfg_tool.bin follows it)")
}
