#!/usr/bin/env python3
"""reactos_disc.py -- a disc shaped as ReactOS's boot CD is, for
test/cli_test.sh's `hull setup reactos`, and a reader of one.

    reactos_disc.py write OUT      a primary volume and a Joliet one, each
                                   with FREELDR.INI at the root and
                                   I386/UNATTEND.INF under it, the two
                                   volumes naming the same file sectors
    reactos_disc.py read DISC PATH the file at PATH by each volume, which
                                   must agree on where it lies and how
                                   long it is: its bytes, or exit 1

ECMA-119 enough for this: sixteen system sectors, the descriptors and
their terminator, path tables, each volume's root and I386 directory, a
sector each, and the files. Joliet's names are UCS-2, big-endian, its
descriptor marked by the escape %/E. No ISO tool is installed here to
make one, and the reader shares no code with hull's.
"""

import struct
import sys

SECTOR = 2048
INI = (b"[FREELOADER]\r\nDefaultOS=LiveImg\r\nTimeOut=3\r\n\r\n[Operating Systems]\r\n"
       b"Setup=\"ReactOS Setup\"\r\nLiveImg=\"ReactOS Live\"\r\n\r\n[Setup]\r\n"
       b"BootType=ReactOSSetup\r\n\r\n[LiveImg]\r\nBootType=Windows2003\r\n")
UNATTEND = b"[Unattend]\r\nSignature = \"$ReactOS$\"\r\nUnattendSetupEnabled = no\r\n" + b";" * 2100 + b"\r\n"
# Sectors: 16 primary, 17 Joliet, 18 terminator, 19-22 path tables, then
# each volume's root and I386, then the files.
ROOT, I386, JROOT, JI386 = 23, 24, 25, 26
FILE_INI, FILE_UNATTEND = 27, 28
TOTAL = FILE_UNATTEND + (len(UNATTEND) + SECTOR - 1) // SECTOR


def both16(n):
	return struct.pack('<H', n) + struct.pack('>H', n)


def both32(n):
	return struct.pack('<I', n) + struct.pack('>I', n)


def record(extent, size, flags, ident):
	body = (b'\0' + both32(extent) + both32(size) + bytes(7) + bytes([flags, 0, 0]) +
	        both16(1) + bytes([len(ident)]) + ident)
	body += b'\0' if len(ident) % 2 == 0 else b''
	body = bytes([len(body) + 1]) + body
	return body + (b'\0' if len(body) % 2 else b'')


def name(text, joliet):
	return text.encode('utf-16-be') if joliet else text.upper().encode()


def directory(this, parent, entries):
	data = record(this, SECTOR, 2, b'\0') + record(parent, SECTOR, 2, b'\1')
	for entry in entries:
		data += entry
	return data


def descriptor(kind, root, joliet):
	escape = b'%/E' if joliet else b''
	return (bytes([kind]) + b'CD001\1\0' + b' ' * 32 + b'REACTOS'.ljust(32) + bytes(8) +
	        both32(TOTAL) + escape.ljust(32, b'\0') + both16(1) + both16(1) + both16(SECTOR) +
	        both32(10) + struct.pack('<I', 19 if not joliet else 21) + bytes(4) +
	        struct.pack('>I', 20 if not joliet else 22) + bytes(4) +
	        record(root, SECTOR, 2, b'\0') + b' ' * (128 * 4 + 37 * 3) + b'0' * 16 * 4 + b'\0' * 4 + b'\1')


def path_table(order, root):
	return bytes([1, 0]) + struct.pack(order + 'I', root) + struct.pack(order + 'H', 1) + b'\0\0'


def write(out):
	sectors = {}
	for joliet, root, sub in ((False, ROOT, I386), (True, JROOT, JI386)):
		suffix = '' if joliet else ';1'
		sectors[root] = directory(root, root, [
			record(FILE_INI, len(INI), 0, name('freeldr.ini' + suffix, joliet)),
			record(sub, SECTOR, 2, name('i386', joliet))])
		sectors[sub] = directory(sub, root, [
			record(FILE_UNATTEND, len(UNATTEND), 0, name('unattend.inf' + suffix, joliet))])
	sectors[16] = descriptor(1, ROOT, False)
	sectors[17] = descriptor(2, JROOT, True)
	sectors[18] = b'\xffCD001\1'
	sectors[19], sectors[20] = path_table('<', ROOT), path_table('>', ROOT)
	sectors[21], sectors[22] = path_table('<', JROOT), path_table('>', JROOT)
	sectors[FILE_INI] = INI
	with open(out, 'wb') as image:
		for s in range(FILE_UNATTEND):
			image.write(sectors.get(s, b'').ljust(SECTOR, b'\0'))
		image.write(UNATTEND.ljust((TOTAL - FILE_UNATTEND) * SECTOR, b'\0'))


def read(disc, path):
	data = open(disc, 'rb').read()
	found = set()
	for s in (16, 17):
		d = data[s * SECTOR:(s + 1) * SECTOR]
		joliet = d[0] == 2
		extent, size = struct.unpack('<I', d[158:162])[0], struct.unpack('<I', d[166:170])[0]
		for part in path.split('/'):
			block = data[extent * SECTOR:extent * SECTOR + size]
			i, hit = 0, None
			while i < len(block) and block[i]:
				n = block[i]
				r = block[i:i + n]
				ident = r[33:33 + r[32]]
				if ident in (b'\0', b'\1'):
					i += n
					continue
				text = ident.decode('utf-16-be') if joliet else ident.decode('ascii', 'replace')
				if text.split(';')[0].lower() == part.lower():
					hit = struct.unpack('<I', r[2:6])[0], struct.unpack('<I', r[10:14])[0]
					assert r[10:14] == r[14:18][::-1], 'the two byte orders of a length disagree'
				i += n
			if hit is None:
				sys.exit(1)
			extent, size = hit
		found.add((extent, size))
	if len(found) != 1:
		sys.exit(1)
	extent, size = found.pop()
	sys.stdout.buffer.write(data[extent * SECTOR:extent * SECTOR + size])


if __name__ == '__main__':
	if sys.argv[1] == 'write':
		write(sys.argv[2])
	else:
		read(sys.argv[2], sys.argv[3])
