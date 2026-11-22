#!/usr/bin/env python3
"""iso_tree.py -- a disc holding the files named, for test/cli_test.sh

    iso_tree.py OUT PATH...    a primary volume of ECMA-119 with each PATH
                               (slash-separated, 8.3 names) a file of a
                               few bytes naming itself, its directories
                               made on the way

Sixteen system sectors, the descriptor and its terminator, the two path
tables, then every directory a sector of its own and every file a
sector. No ISO tool is installed here to make one, and this shares no
code with hull's own reader or writer.
"""

import struct
import sys

SECTOR = 2048


def both16(n):
	return struct.pack('<H', n) + struct.pack('>H', n)


def both32(n):
	return struct.pack('<I', n) + struct.pack('>I', n)


def record(extent, size, directory, ident):
	length = 33 + len(ident) + (len(ident) + 1) % 2
	return (bytes([length, 0]) + both32(extent) + both32(size) + bytes(7) +
	        bytes([2 if directory else 0, 0, 0]) + both16(1) + bytes([len(ident)]) + ident +
	        bytes((len(ident) + 1) % 2))


def main():
	out, paths = sys.argv[1], sys.argv[2:]
	# The tree: a directory is a dict of its entries, a file its bytes.
	root = {}
	for path in paths:
		parts = path.upper().split('/')
		here = root
		for part in parts[:-1]:
			here = here.setdefault(part, {})
		here[parts[-1]] = ('%s\r\n' % path).encode()
	# Sectors: 16 descriptor, 17 terminator, 18 and 19 path tables, then
	# directories breadth first, then files.
	directories, files = [(root, None)], []
	at = 0
	while at < len(directories):
		for name, entry in sorted(directories[at][0].items()):
			if isinstance(entry, dict):
				directories.append((entry, at))
			else:
				files.append(entry)
		at += 1
	sector_of = {id(d): 20 + i for i, (d, _) in enumerate(directories)}
	next_file = 20 + len(directories)
	file_sector = {}
	for entry in files:
		file_sector[id(entry)] = next_file
		next_file += 1
	total = next_file
	image = bytearray(total * SECTOR)
	for i, (d, parent) in enumerate(directories):
		this = sector_of[id(d)]
		up = sector_of[id(directories[parent][0])] if parent is not None else this
		body = record(this, SECTOR, True, b'\0') + record(up, SECTOR, True, b'\1')
		for name, entry in sorted(d.items()):
			if isinstance(entry, dict):
				body += record(sector_of[id(entry)], SECTOR, True, name.encode())
			else:
				body += record(file_sector[id(entry)], len(entry), False, name.encode() + b';1')
		assert len(body) <= SECTOR
		image[this * SECTOR:this * SECTOR + len(body)] = body
		for name, entry in d.items():
			if not isinstance(entry, dict):
				start = file_sector[id(entry)] * SECTOR
				image[start:start + len(entry)] = entry
	# Path tables: the root alone is enough for a reader walking records.
	table = bytes([1, 0]) + struct.pack('<I', 20) + struct.pack('<H', 1) + b'\0\0'
	image[18 * SECTOR:18 * SECTOR + len(table)] = table
	table = bytes([1, 0]) + struct.pack('>I', 20) + struct.pack('>H', 1) + b'\0\0'
	image[19 * SECTOR:19 * SECTOR + len(table)] = table
	pvd = bytearray(SECTOR)
	pvd[0:7] = b'\x01CD001\x01'
	pvd[40:72] = b'HULLTEST'.ljust(32)
	pvd[80:88] = both32(total)
	pvd[120:124] = both16(1)
	pvd[124:128] = both16(1)
	pvd[128:132] = both16(SECTOR)
	pvd[132:140] = both32(10)
	pvd[140:144] = struct.pack('<I', 18)
	pvd[148:152] = struct.pack('>I', 19)
	pvd[156:190] = record(20, SECTOR, True, b'\0')
	pvd[881] = 1
	image[16 * SECTOR:17 * SECTOR] = pvd
	image[17 * SECTOR:17 * SECTOR + 7] = b'\xffCD001\x01'
	with open(out, 'wb') as disc:
		disc.write(image)


if __name__ == '__main__':
	main()
