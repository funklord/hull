; pc_boot.asm -- the system test/pc_test.sh boots: one sector, assembled
; with nasm, that says on COM1 which drive it came from and powers off.
;
; From a hard disk or floppy it prints the next sector of its drive, then
; overwrites that sector with "written", so a second boot shows whether
; the first one's write was kept. From a CD, in El Torito's no-emulation
; mode, it installs: this sector and a fresh next one go to the first
; hard disk, which then boots.

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
	mov [drive], dl
	cmp dl, 0xe0
	jae install

	mov si, booted
	call print
	mov ax, 0x0201		; read the next sector
	mov bx, 0x8000
	mov cx, 2
	xor dh, dh
	mov dl, [drive]
	int 0x13
	jc failed
	mov si, 0x8000
	call print
	mov si, written		; and leave a word in it
	mov di, 0x8000
	mov cx, written_end - written
	rep movsb
	mov ax, 0x0301
	mov bx, 0x8000
	mov cx, 2
	xor dh, dh
	mov dl, [drive]
	int 0x13
	jc failed
	jmp off

install:
	mov si, installing
	call print
	mov ax, 0x0301		; this sector to the disk's first
	mov bx, 0x7c00
	mov cx, 1
	xor dh, dh
	mov dl, 0x80
	int 0x13
	jc failed
	mov si, fresh		; a fresh word in its second
	mov di, 0x8000
	mov cx, fresh_end - fresh
	rep movsb
	mov ax, 0x0301
	mov bx, 0x8000
	mov cx, 2
	xor dh, dh
	mov dl, 0x80
	int 0x13
	jc failed
	jmp off

failed:
	mov si, failure
	call print

off:
	mov dx, 0x604		; the power off of pc's and q35's ACPI, as
	mov ax, 0x2000		; SeaBIOS places it
	out dx, ax
	hlt
	jmp off

print:
	lodsb
	test al, al
	jz .done
	mov dx, 0x3f8
	out dx, al
	jmp print
.done:
	ret

drive:		db 0
booted:		db 'booted from a drive', 13, 10, 0
installing:	db 'installing from a disc', 13, 10, 0
failure:	db 'a read or write failed', 13, 10, 0
written:	db 'word: written', 13, 10, 0
written_end:
fresh:		db 'word: fresh', 13, 10, 0
fresh_end:

times 510 - ($ - $$) db 0
dw 0xaa55

; The second sector, as a disk or floppy first has it.
db 'word: fresh', 13, 10, 0
times 1024 - ($ - $$) db 0
