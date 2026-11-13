#!/usr/bin/env python3
# esp_image.py BIN LOAD CHIP OFFSET OUT [ENTRY [BIN2 LOAD2]] -- a flash
# image for an Espressif chip's ROM (test/board_test.sh): BIN, raw code,
# as a segment loaded at LOAD and entered there, or at ENTRY, BIN2 a
# second segment loaded at LOAD2 -- constants in DRAM, which bytes can be
# read from as IRAM's cannot -- for chip id CHIP (0 the ESP32, 9 the S3, 5
# the C3, 13 the C6, 16 the H2, 18 the P4, 20 the C61, 23 the C5), its
# SHA-256 appended, written OFFSET bytes into OUT, where the ROM reads its
# bootloader; erased flash, 0xff, before it. hull fills out the rest.

import hashlib
import struct
import sys


def main():
	if len(sys.argv) not in (6, 7, 9):
		sys.exit("usage: esp_image.py BIN LOAD CHIP OFFSET OUT [ENTRY [BIN2 LOAD2]]")
	segments = [(sys.argv[1], int(sys.argv[2], 0))]
	if len(sys.argv) == 9:
		segments.append((sys.argv[7], int(sys.argv[8], 0)))
	chip = int(sys.argv[3], 0)
	offset = int(sys.argv[4], 0)
	entry = int(sys.argv[6], 0) if len(sys.argv) > 6 else segments[0][1]
	# magic, the segments, DIO, 40 MHz and 4 MB, the entry point
	header = struct.pack("<BBBBI", 0xE9, len(segments), 2, 0x20, entry)
	# the extended header: no WP pin, the chip, any revision, a hash
	extended = struct.pack("<B3sHBHH4sB", 0xEE, b"\0\0\0", chip, 0, 0, 0xFFFF, b"\0" * 4, 1)
	image = header + extended
	checksum = 0xEF
	for path, load in segments:
		with open(path, "rb") as source:
			code = source.read()
		code += b"\0" * (-len(code) % 4)
		image += struct.pack("<II", load, len(code)) + code
		for byte in code:
			checksum ^= byte
	image += b"\0" * (-(len(image) + 1) % 16) + bytes([checksum])
	image += hashlib.sha256(image).digest()
	with open(sys.argv[5], "wb") as out:
		out.write(b"\xff" * offset + image)


main()
