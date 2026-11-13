/* stm32f100.ld -- the STM32F100RB's flash and RAM, for echo_stm32.c. */
MEMORY
{
	FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 128K
	RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 8K
}
SECTIONS
{
	.text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
	.bss  : { *(.bss*) *(COMMON) } > RAM
}
