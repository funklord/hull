; key_boot.asm -- the system test/virtualbox_test.sh boots to see `hull type`
; and `hull key` reach a PC's keyboard: one sector, assembled with nasm,
; that says it is ready on COM1, reads keys by the BIOS until Enter, and
; says what they spelled. Then it halts; the test stops the machine.

bits 16
org 0x7c00

start:
	cli
	xor ax, ax
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov sp, 0x7c00
	sti
	mov si, ready
	call print
	mov di, line
.key:
	xor ah, ah		; a key, its character in al
	int 0x16
	cmp al, 13
	je .done
	test al, al
	jz .key			; one with no character, a shift alone
	cmp di, line + 60
	jae .key
	stosb
	jmp .key
.done:
	xor al, al
	stosb
	mov si, typed
	call print
	mov si, line
	call print
	mov si, crlf
	call print
.halt:
	hlt
	jmp .halt

; Each byte once the UART can take it, as pc_boot.asm does.
print:
	lodsb
	test al, al
	jz .done
	mov ah, al
	mov dx, 0x3fd
.wait:
	in al, dx
	test al, 0x20
	jz .wait
	mov al, ah
	mov dx, 0x3f8
	out dx, al
	jmp print
.done:
	ret

ready:	db 'ready-to-type', 13, 10, 0
typed:	db 'typed=', 0
crlf:	db 13, 10, 0
line:	times 62 db 0

times 510 - ($ - $$) db 0
dw 0xaa55
