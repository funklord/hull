#!/usr/bin/env python3
# disk_table.py IMAGE -- a disk's partition table, as disk_test.sh reads
# it: written from the MBR and GPT specifications, sharing no code with
# hull. One line a partition,
#
#     dos N TYPE ACTIVE FIRST SECTORS     (TYPE in hex, ACTIVE 0 or 1)
#     gpt N TYPE-GUID ATTRIBUTES FIRST LAST NAME
#
# after a first line `dos SIGNATURE` or `gpt GOOD`, GOOD the checks of
# both headers' and both entry arrays' CRCs and that each header points
# at the other: `ok`, or what failed. Byte offsets are 512-byte sectors.

import struct
import sys
import uuid
import zlib


def main(path):
	with open(path, 'rb') as f:
		data = f.read(34 * 512)
		f.seek(0, 2)
		size = f.tell()
		sectors = size // 512
		mbr = data[:512]
		if mbr[510:512] != b'\x55\xaa':
			print('none')
			return
		if mbr[446 + 4] == 0xee:
			gpt(f, sectors)
			return
		print('dos %08x' % struct.unpack_from('<I', mbr, 440)[0])
		for n in range(4):
			e = mbr[446 + 16 * n:446 + 16 * n + 16]
			if e[4] == 0:
				continue
			first, count = struct.unpack_from('<II', e, 8)
			print('dos %d %02x %d %d %d' % (n + 1, e[4], 1 if e[0] == 0x80 else 0, first, count))


def header(f, lba):
	f.seek(lba * 512)
	h = f.read(512)
	if h[:8] != b'EFI PART':
		return None, 'no header at %d' % lba
	size = struct.unpack_from('<I', h, 12)[0]
	crc = struct.unpack_from('<I', h, 16)[0]
	plain = h[:16] + b'\0\0\0\0' + h[20:size]
	if zlib.crc32(plain) & 0xffffffff != crc:
		return None, 'header CRC at %d' % lba
	return h, None


def gpt(f, sectors):
	problems = []
	primary, why = header(f, 1)
	if primary is None:
		print('gpt ' + why)
		return
	backup_lba = struct.unpack_from('<Q', primary, 32)[0]
	if backup_lba != sectors - 1:
		problems.append('the backup is not at the last sector')
	backup, why = header(f, backup_lba)
	if backup is None:
		problems.append(why)
	elif struct.unpack_from('<Q', backup, 32)[0] != 1:
		problems.append('the backup does not point at the primary')
	arrays = []
	for h in (primary, backup):
		if h is None:
			continue
		at, count, width, crc = struct.unpack_from('<QIII', h, 72)
		f.seek(at * 512)
		entries = f.read(count * width)
		if zlib.crc32(entries) & 0xffffffff != crc:
			problems.append('entries CRC at %d' % at)
		arrays.append(entries)
	if len(arrays) == 2 and arrays[0] != arrays[1]:
		problems.append('the two entry arrays differ')
	print('gpt ' + ('ok' if not problems else '; '.join(problems)))
	entries = arrays[0]
	for n in range(len(entries) // 128):
		e = entries[128 * n:128 * n + 128]
		if e[:16] == b'\0' * 16:
			continue
		kind = str(uuid.UUID(bytes_le=e[:16])).upper()
		first, last, attributes = struct.unpack_from('<QQQ', e, 32)
		name = e[56:128].decode('utf-16-le').rstrip('\0')
		print('gpt %d %s %d %d %d %s' % (n + 1, kind, attributes, first, last, name or '-'))


if __name__ == '__main__':
	main(sys.argv[1])
