/* esp32_ram.ld -- a bare ESP32 image the ROM loads as its bootloader
 * (test/board/lora_esp32.c): code in IRAM, constants and data in DRAM,
 * which bytes can be read from, as IRAM's cannot. */
ENTRY(start)
SECTIONS
{
	.text 0x40080400 : { *(.literal .text .literal.* .text.*) }
	.data 0x3ffb0000 : { *(.rodata .rodata.* .data .data.* .bss .bss.* COMMON) }
	/DISCARD/ : { *(.comment .xtensa.info .note.*) }
}
