#!/usr/bin/env python3
"""pc_disc.py -- a bootable ISO9660 disc, for test/pc_test.sh.

    pc_disc.py OUT BOOT   write OUT: BOOT's first 2048 bytes, booted

An El Torito no-emulation disc with no tool of hull's in it, none being
installed here to make one: sixteen system sectors, the primary volume
descriptor, the boot record naming the catalog, the terminator, path
tables, the root directory, the catalog and the boot image.
"""

import struct
import sys

SECTOR = 2048
ROOT = 21
CATALOG = 22
IMAGE = 23


def both16(n):
	return struct.pack('<H', n) + struct.pack('>H', n)


def both32(n):
	return struct.pack('<I', n) + struct.pack('>I', n)


def record(extent, size, ident):
	body = (b'\0' + both32(extent) + both32(size) + bytes(7) + bytes([2, 0, 0]) + both16(1) +
	        bytes([len(ident)]) + ident)
	if len(body) % 2 == 0:
		body += b'\0'
	return bytes([len(body) + 1]) + body


def padded(data):
	return data + bytes(-len(data) % SECTOR)


def main():
	out, boot = sys.argv[1:3]
	with open(boot, 'rb') as f:
		image = padded(f.read()[:SECTOR])
	total = IMAGE + 1
	root = record(ROOT, SECTOR, b'\0') + record(ROOT, SECTOR, b'\1')
	pvd = bytearray(SECTOR)
	pvd[0:6] = b'\1CD001'
	pvd[6] = 1
	pvd[40:72] = b'HULLBOOT'.ljust(32)
	pvd[80:88] = both32(total)
	pvd[120:124] = both16(1)
	pvd[124:128] = both16(1)
	pvd[128:132] = both16(SECTOR)
	pvd[132:140] = both32(10)
	pvd[140:144] = struct.pack("<I", 19)
	pvd[148:152] = struct.pack('>I', 20)
	pvd[156:190] = record(ROOT, SECTOR, b'\0')
	pvd[881] = 1
	el_torito = bytearray(SECTOR)
	el_torito[0:6] = b'\0CD001'
	el_torito[6] = 1
	el_torito[7:30] = b'EL TORITO SPECIFICATION'
	el_torito[71:75] = struct.pack('<I', CATALOG)
	end = bytearray(SECTOR)
	end[0:6] = b'\xffCD001'
	end[6] = 1
	table = bytes([1, 0]) + struct.pack('<I', ROOT) + struct.pack('<H', 1) + b'\0\0'
	# The validation entry's words sum to zero; the default entry boots.
	entry = bytearray(32)
	entry[0] = 1
	entry[30:32] = b'\x55\xaa'
	entry[28:30] = struct.pack('<H', -sum(struct.unpack('<16H', bytes(entry))) & 0xffff)
	default = bytearray(32)
	default[0] = 0x88
	default[6:8] = struct.pack('<H', 4)
	default[8:12] = struct.pack('<I', IMAGE)
	with open(out, 'wb') as f:
		f.write(bytes(16 * SECTOR))
		f.write(pvd)
		f.write(el_torito)
		f.write(end)
		f.write(padded(table))
		f.write(padded(table[:2] + struct.pack('>I', ROOT) + b'\0\1\0\0'))
		f.write(padded(root))
		f.write(padded(bytes(entry) + bytes(default)))
		f.write(image)


main()
