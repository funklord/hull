#!/usr/bin/env python3
"""msdos_source.py -- MS-DOS 4.00 from Microsoft's MIT-licensed source, for
test/legacy_test.sh's `hull setup msdos`: the published tree put right to
build, and a boot floppy made of what it built.

    msdos_source.py repair SRC    SRC the v4.0 tree, copied: put right to
                                  build on C:, as its BUILD.BAT does
    msdos_source.py floppy SRC OUT a 1.44 MB boot floppy of SRC's build:
                                  MS-DOS's own boot sector, IO.SYS and
                                  MSDOS.SYS first, COMMAND, FORMAT, SYS

The repairs are the ones the published tree is known to need (its import
turned CP437 bytes into UTF-8 and LF line ends, 2024): SETENV.BAT's
include and library paths, which name tools\\lib and tools\\inc where the
tree has tools\\bld\\lib and tools\\bld\\inc; three files whose box-drawing
bytes became sequences MASM refuses, made '#'; and CRLF in every batch,
assembly and skeleton file. BAKROOT is C:, the machine's directory.

The boot sector is the last 512 bytes of BOOT\\MSBOOT.BIN, which the build
links at 7C00h. FORMAT would write its BPB, and its drive number: the
template says 80h, a hard disk, and a floppy's is 0, without which the
sector reads the wrong drive and says Non-System disk. mtools writes the
BPB; the drive number is set here.
"""

import os
import re
import subprocess
import sys

BOX = (b'\xef\xbf\xbd', b'\xc4\xbf', b'\xc4\xb4')
BUILD = (b'@echo off\r\ncd \\src\r\ncall setenv.bat\r\nnmake\r\nif errorlevel 1 goto end\r\n'
         b'md c:\\bin\r\ncall cpy.bat c:\\bin\r\n:end\r\n')


def repair(root):
	src = os.path.join(root, 'src')
	path = os.path.join(src, 'SETENV.BAT')
	text = open(path, 'rb').read()
	fixed = (text.replace(b'set BAKROOT=d:', b'set BAKROOT=c:')
	         .replace(b'tools\\lib', b'tools\\bld\\lib').replace(b'tools\\inc', b'tools\\bld\\inc'))
	if fixed == text:
		sys.exit('msdos_source: SETENV.BAT is not as published')
	open(path, 'wb').write(fixed)
	for name in ('MAPPER/GETMSG.ASM', 'SELECT/SELECT2.ASM', 'SELECT/USA.INF'):
		path = os.path.join(src, name)
		text = open(path, 'rb').read()
		for seq in BOX:
			text = text.replace(seq, b'#')
		open(path, 'wb').write(text)
	for directory, _, files in os.walk(src):
		for name in files:
			upper = name.upper()
			if upper.endswith(('.BAT', '.ASM', '.SKL')) or upper in ('ZERO.DAT', 'LOCSCR'):
				path = os.path.join(directory, name)
				text = open(path, 'rb').read()
				crlf = re.sub(rb'(?<!\r)\n', b'\r\n', text)
				if crlf != text:
					open(path, 'wb').write(crlf)
	open(os.path.join(root, 'BUILD.BAT'), 'wb').write(BUILD)


def floppy(root, out):
	sector = open(os.path.join(root, 'src', 'BOOT', 'msboot.bin'), 'rb').read()[-512:]
	if sector[510:] != b'\x55\xaa':
		sys.exit('msdos_source: BOOT\\MSBOOT.BIN does not end in a boot sector')
	boot = out + '.boot'
	open(boot, 'wb').write(sector)
	if os.path.exists(out):
		os.unlink(out)
	subprocess.run(['mformat', '-C', '-f', '1440', '-B', boot, '-i', out, '::'], check=True)
	os.unlink(boot)
	with open(out, 'r+b') as image:
		image.seek(0x24)
		image.write(b'\0')
	built = os.path.join(root, 'bin')
	for name in ('io.sys', 'msdos.sys', 'command.com', 'format.com', 'sys.com', 'fdisk.exe'):
		subprocess.run(['mcopy', '-i', out, os.path.join(built, name), '::' + name.upper()], check=True)
	subprocess.run(['mattrib', '-i', out, '+s', '+h', '+r', '::IO.SYS', '::MSDOS.SYS'], check=True)


if __name__ == '__main__':
	if sys.argv[1] == 'repair':
		repair(sys.argv[2])
	else:
		floppy(sys.argv[2], sys.argv[3])
