/*
 * echo_esp_xt.S -- firmware for Espressif's Xtensa chips, the ESP32 and the
 * ESP32-S3, under a pc machine of hull's (test/board_test.sh): "ready" on
 * UART0, then each line typed, ended by a carriage return, said back as
 * "said LINE". Assembly, as clang 19 has no Xtensa front end and llvm-mc
 * does; position-independent, no literals or calls, each constant built
 * by shifts. Loaded by the chip's ROM as its bootloader, one segment,
 * test/board/esp_image.py making the flash image. S3 selects the S3's
 * UART0 and its RAM. a2 the UART, a4 the line's buffer, a7 its end.
 */
	.macro put r
91:	l32i	a6, a2, 0x1c
	extui	a6, a6, 16, 8
	bgeui	a6, 64, 91b
	s32i	\r, a2, 0
	.endm
	.macro putc c
	movi	a8, \c
	put	a8
	.endm

	.text
	.global	start
start:
#ifdef S3
	movi	a2, 6
	slli	a2, a2, 28
	movi	a4, 0x3fc
	slli	a4, a4, 20
	movi	a3, 0x90
	slli	a3, a3, 12
	add	a4, a4, a3
#else
	movi	a2, 0x3ff
	slli	a2, a2, 20
	movi	a3, 4
	slli	a3, a3, 16
	add	a2, a2, a3
	movi	a3, 7
	slli	a3, a3, 16
	add	a4, a2, a3
#endif
	or	a7, a4, a4
	putc	'r'
	putc	'e'
	putc	'a'
	putc	'd'
	putc	'y'
	putc	13
	putc	10
wait:
	l32i	a6, a2, 0x1c
	extui	a6, a6, 0, 8
	beqz	a6, wait
	l32i	a8, a2, 0
	extui	a8, a8, 0, 8
	movi	a9, 13
	beq	a8, a9, line
	sub	a11, a7, a4
	bgeui	a11, 32, wait
	s8i	a8, a7, 0
	addi	a7, a7, 1
	j	wait
line:
	putc	's'
	putc	'a'
	putc	'i'
	putc	'd'
	putc	' '
	or	a10, a4, a4
said:
	bgeu	a10, a7, done
	l8ui	a8, a10, 0
	put	a8
	addi	a10, a10, 1
	j	said
done:
	putc	13
	putc	10
	or	a7, a4, a4
	j	wait
