#!/usr/bin/env python3
# esp_image.py BIN LOAD CHIP OFFSET OUT -- a flash image for an Espressif
# chip's ROM (test/board_test.sh): BIN, raw code, as the one segment of an
# image loaded at LOAD and entered there, for chip id CHIP (0 the ESP32, 9
# the S3, 5 the C3, 13 the C6, 16 the H2, 18 the P4, 20 the C61, 23 the C5),
# its SHA-256 appended, written OFFSET bytes into OUT, where the ROM reads
# its bootloader; erased flash, 0xff, before it. hull fills out the rest.

import hashlib
import struct
import sys


def main():
	if len(sys.argv) != 6:
		sys.exit("usage: esp_image.py BIN LOAD CHIP OFFSET OUT")
	with open(sys.argv[1], "rb") as source:
		code = source.read()
	load = int(sys.argv[2], 0)
	chip = int(sys.argv[3], 0)
	offset = int(sys.argv[4], 0)
	code += b"\0" * (-len(code) % 4)
	# magic, one segment, DIO, 40 MHz and 4 MB, the entry point
	header = struct.pack("<BBBBI", 0xE9, 1, 2, 0x20, load)
	# the extended header: no WP pin, the chip, any revision, a hash
	extended = struct.pack("<B3sHBHH4sB", 0xEE, b"\0\0\0", chip, 0, 0, 0xFFFF, b"\0" * 4, 1)
	image = header + extended + struct.pack("<II", load, len(code)) + code
	checksum = 0xEF
	for byte in code:
		checksum ^= byte
	image += b"\0" * (-(len(image) + 1) % 16) + bytes([checksum])
	image += hashlib.sha256(image).digest()
	with open(sys.argv[5], "wb") as out:
		out.write(b"\xff" * offset + image)


main()
