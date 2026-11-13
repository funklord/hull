/* lm3s6965.ld -- the LM3S6965's flash and RAM, for lm3s_net.c. */
MEMORY
{
	FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
	RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}
SECTIONS
{
	.text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
	.bss  : { *(.bss*) *(COMMON) } > RAM
}
