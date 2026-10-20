/* SMC-PAD bootloader: app.bin of the app area at flash 0x1000, entry at its first byte. */
MEMORY
{
    code (rx) : ORIGIN = 0x1E00120, LENGTH = 0x8000 - 0x120
}

ENTRY(boot_main)

SECTIONS
{
    .text :
    {
        *(.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > code

    /DISCARD/ : { *(.data .data.* .bss .bss.* COMMON) }
}
