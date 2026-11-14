/* esp32s3_ram.ld -- a bare ESP32-S3 image the ROM loads as its
 * bootloader (test/board/lora_esp32.c built with S3): code in IRAM,
 * constants and data in DRAM, clear of the ROM loader's own at the top. */
ENTRY(start)
SECTIONS
{
	.text 0x40380000 : { *(.literal .text .literal.* .text.*) }
	.data 0x3fcb0000 : { *(.rodata .rodata.* .data .data.* .bss .bss.* COMMON) }
	/DISCARD/ : { *(.comment .xtensa.info .note.*) }
}
