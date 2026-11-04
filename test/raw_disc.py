#!/usr/bin/env python3
"""raw_disc.py -- a CD dump, for test/media_test.sh.

    raw_disc.py ISO BIN CUE [bad-edc SECTOR]

BIN is ISO's sectors as raw Mode 1, 2352 bytes each, then one audio track
of a second of noise, and CUE its sheet. The sector's sync, header, EDC and
parity are made here from ECMA-130 as it states them: the EDC by dividing
by its polynomial, a bit at a time, and each parity pair by solving annex
A's two check equations over GF(2^8) for it, where hull computes them by
the usual table-driven recurrence, so agreement between the two is
evidence about the sectors and not one program's reading of itself.
"""

import os
import random
import sys

RAW = 2352
EXP = [0] * 512
LOG = [0] * 256
x = 1
for i in range(255):
	EXP[i] = x
	LOG[x] = i
	x <<= 1
	if x & 0x100:
		x ^= 0x11D
for i in range(255, 512):
	EXP[i] = EXP[i - 255]


def mul(a, b):
	return 0 if a == 0 or b == 0 else EXP[LOG[a] + LOG[b]]


def div(a, b):
	return 0 if a == 0 else EXP[(LOG[a] - LOG[b]) % 255]


def edc(data):
	# x^32 + x^31 + x^16 + x^15 + x^4 + x^3 + x + 1, reflected, a bit at a time.
	crc = 0
	for byte in data:
		for bit in range(8):
			mix = (crc ^ (byte >> bit)) & 1
			crc >>= 1
			if mix:
				crc ^= 0xD8018001
	return crc


def solve(values, length):
	"""The last two of `length` symbols, the rest `values`, so that the sum
	of the symbols is 0 and the sum of alpha^(length-1-i) times each is 0."""
	s0 = 0
	s1 = 0
	for i, v in enumerate(values):
		s0 ^= v
		s1 ^= mul(EXP[length - 1 - i], v)
	# a + b = s0, alpha a + b = s1.
	a = div(s0 ^ s1, 1 ^ EXP[1])
	return a, a ^ s0


def parity(sector):
	body = 12
	# P: 43 columns of 26 words, a byte plane at a time.
	for plane in range(2):
		for column in range(43):
			at = [body + 2 * (43 * row + column) + plane for row in range(26)]
			a, b = solve([sector[i] for i in at[:24]], 26)
			sector[at[24]] = a
			sector[at[25]] = b
	# Q: 26 diagonals of 45 words, the last two after the P parity.
	for plane in range(2):
		for diagonal in range(26):
			at = [body + 2 * ((44 * row + 43 * diagonal) % 1118) + plane for row in range(43)]
			at.append(body + 2 * (43 * 26 + diagonal) + plane)
			at.append(body + 2 * (44 * 26 + diagonal) + plane)
			a, b = solve([sector[i] for i in at[:43]], 45)
			sector[at[43]] = a
			sector[at[44]] = b


def bcd(n):
	return (n // 10) << 4 | n % 10


def make(data, number):
	sector = bytearray(RAW)
	sector[1:11] = b'\xff' * 10
	address = number + 150
	sector[12] = bcd(address // 4500)
	sector[13] = bcd(address // 75 % 60)
	sector[14] = bcd(address % 75)
	sector[15] = 1
	sector[16:2064] = data
	sector[2064:2068] = edc(sector[:2064]).to_bytes(4, 'little')
	parity(sector)
	return sector


def main():
	iso, out, cue = sys.argv[1:4]
	bad = int(sys.argv[5]) if len(sys.argv) > 5 else -1
	cooked = open(iso, 'rb').read()
	count = len(cooked) // 2048
	with open(out, 'wb') as bin_file:
		for number in range(count):
			sector = make(cooked[number * 2048:(number + 1) * 2048], number)
			if number == bad:
				sector[2064] ^= 0xFF
			bin_file.write(sector)
		rng = random.Random(7)
		bin_file.write(bytes(rng.randrange(256) for _ in range(RAW * 75)))
	seconds, frames = divmod(count, 75)
	with open(cue, 'w') as sheet:
		sheet.write('FILE "%s" BINARY\n' % os.path.basename(out))
		sheet.write('  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n')
		sheet.write('  TRACK 02 AUDIO\n    INDEX 01 %02d:%02d:%02d\n'
		            % (seconds // 60, seconds % 60, frames))


main()
