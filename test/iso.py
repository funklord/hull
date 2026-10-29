#!/usr/bin/env python3
"""iso.py -- an ISO9660 image of one file, for test/cli_test.sh's dos media.

    iso.py OUT NAME TEXT   write OUT: the volume HULLTEST, NAME in its root

Enough of ECMA-119 for an emulator to mount: sixteen system sectors, the
primary volume descriptor and its terminator, the path tables both ways
round, the root directory and the file, a sector each. No ISO tool is
installed here to make one.
"""

import struct
import sys

SECTOR = 2048
ROOT = 20       # the root directory's sector
FILE = 21       # the file's


def both16(n):
	return struct.pack('<H', n) + struct.pack('>H', n)


def both32(n):
	return struct.pack('<I', n) + struct.pack('>I', n)


def record(extent, size, flags, ident):
	"""A directory record, padded to an even length."""
	body = (b'\0' + both32(extent) + both32(size) + bytes(7) + bytes([flags, 0, 0]) +
	        both16(1) + bytes([len(ident)]) + ident)
	body = bytes([len(body) + 1]) + body
	return body + (b'\0' if len(body) % 2 else b'')


def path_table(order):
	"""The root's entry alone, little- or big-endian."""
	return bytes([1, 0]) + struct.pack(order + 'I', ROOT) + struct.pack(order + 'H', 1) + b'\0\0'


def main():
	out, name, text = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
	root = (record(ROOT, SECTOR, 2, b'\0') + record(ROOT, SECTOR, 2, b'\1') +
	        record(FILE, len(text), 0, name + b';1'))
	date = b'0' * 16 + b'\0'
	primary = (b'\1CD001\1\0' + b' ' * 32 + b'HULLTEST'.ljust(32) + bytes(8) +
	           both32(FILE + 1) + bytes(32) + both16(1) + both16(1) + both16(SECTOR) +
	           both32(10) + struct.pack('<I', 18) + bytes(4) + struct.pack('>I', 19) +
	           bytes(4) + record(ROOT, SECTOR, 2, b'\0') + b' ' * (128 * 4 + 37 * 3) +
	           date * 4 + b'\1')
	sectors = ([bytes(SECTOR)] * 16 + [primary, b'\xffCD001\1', path_table('<'),
	           path_table('>'), root, text])
	with open(out, 'wb') as image:
		for sector in sectors:
			image.write(sector.ljust(SECTOR, b'\0'))


if __name__ == '__main__':
	main()
