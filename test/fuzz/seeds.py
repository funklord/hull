"""seeds.py -- the corpora check-fuzz starts from, written into the
directory it is given: JSON a registry sends, and tar layers of each kind
the unpacker meets -- files, directories, links, whiteouts, long names in
pax and GNU form, gzip, and paths that try to leave the tree."""

import gzip
import io
import os
import subprocess
import sys
import tarfile

out = sys.argv[1]
os.makedirs(os.path.join(out, "json"), exist_ok=True)
os.makedirs(os.path.join(out, "tar"), exist_ok=True)

MANIFEST = '{"schemaVersion":2,"mediaType":"a","layers":[{"digest":"sha256:ab","size":12}]}'
ARRAY = '[1,-2.5e3,true,false,null,"\\u00e9\\n","\\ud83d\\ude00"]'
for name, text in (("manifest", MANIFEST), ("array", ARRAY)):
	with open(os.path.join(out, "json", name), "w") as f:
		f.write(text)


def make(name, members, gz=False, fmt=tarfile.PAX_FORMAT):
	data = io.BytesIO()
	archive = tarfile.open(fileobj=data, mode="w", format=fmt)
	for kind, path, extra in members:
		info = tarfile.TarInfo(path)
		if kind == "file":
			info.size = len(extra)
			info.uid, info.gid = 101, 102
			archive.addfile(info, io.BytesIO(extra))
		elif kind == "dir":
			info.type = tarfile.DIRTYPE
			info.mode = 0o755
			archive.addfile(info)
		elif kind == "symlink":
			info.type = tarfile.SYMTYPE
			info.linkname = extra
			archive.addfile(info)
		elif kind == "hardlink":
			info.type = tarfile.LNKTYPE
			info.linkname = extra
			archive.addfile(info)
	archive.close()
	raw = data.getvalue()
	with open(os.path.join(out, "tar", name), "wb") as f:
		f.write(gzip.compress(raw) if gz else raw)


PLAIN = [("dir", "etc", None), ("file", "etc/a", b"hello\n"), ("symlink", "etc/b", "a")]
make("plain", PLAIN + [("hardlink", "etc/c", "etc/a")])
WHITEOUT = [("dir", "x", None), ("file", "x/.wh.y", b""), ("file", "x/.wh..wh..opq", b"")]
make("whiteout", WHITEOUT + [("file", "x/z", b"z")])
make("long", [("file", "d/" + "n" * 150, b"long")], gz=True)
make("gnu", [("file", "g/" + "q" * 120, b"gnu")], fmt=tarfile.GNU_FORMAT)
ESCAPE = [("symlink", "up", "../.."), ("file", "up/x", b"out"), ("file", "../y", b"out")]
make("escape", ESCAPE)

# A disc from sector 16 on, as media_fuzz.c takes one: iso.py's.
disc = os.path.join(out, 'disc')
os.makedirs(disc, exist_ok=True)
image = os.path.join(disc, 'whole.iso')
subprocess.check_call([sys.executable, '-I', os.path.join(os.path.dirname(__file__), '..', 'iso.py'),
                       image, 'README.TXT', 'on a disc'])
with open(image, 'rb') as whole, open(os.path.join(disc, 'seed'), 'wb') as seed:
	seed.write(whole.read()[16 * 2048:])
os.unlink(image)

# A FAT12 disk of 32 sectors, whole: a file in two runs, and a directory.
fat = bytearray(32 * 512)
fat[0:3] = b'\xeb\x3c\x90'
fat[11:13] = (512).to_bytes(2, 'little')
fat[13] = 1
fat[14:16] = (1).to_bytes(2, 'little')
fat[16] = 1
fat[17:19] = (16).to_bytes(2, 'little')
fat[19:21] = (32).to_bytes(2, 'little')
fat[21] = 0xF0
fat[22:24] = (1).to_bytes(2, 'little')
fat[512:512 + 9] = bytes([0xF0, 0xFF, 0xFF, 0x04, 0xF0, 0xFF, 0xFF, 0x0F, 0x00])
root = 1024
fat[root:root + 11] = b'FILE    BIN'
fat[root + 26:root + 28] = (2).to_bytes(2, 'little')
fat[root + 28:root + 32] = (700).to_bytes(4, 'little')
fat[root + 32:root + 43] = b'DIR        '
fat[root + 43] = 0x10
fat[root + 58:root + 60] = (3).to_bytes(2, 'little')
with open(os.path.join(disc, 'fat'), 'wb') as seed:
	seed.write(fat)

# An ext2 filesystem of 64 blocks, whole, where mke2fs is: its boot block
# is free, and the 0xE2 there is media_fuzz.c's sign that it is whole.
for tool in ('/usr/sbin/mke2fs', '/sbin/mke2fs'):
	if os.path.exists(tool):
		tree = os.path.join(disc, 'tree')
		image = os.path.join(disc, 'ext')
		os.makedirs(os.path.join(tree, 'd'), exist_ok=True)
		with open(os.path.join(tree, 'd', 'f'), 'wb') as f:
			f.write(b'x' * 3000)
		subprocess.check_call([tool, '-q', '-F', '-t', 'ext2', '-b', '1024', '-N', '16', '-O',
		                       '^resize_inode', '-d', tree, image, '64'])
		# mke2fs leaves a file short of the filesystem's size, and a reader
		# that holds the size to the image would refuse it.
		with open(image, 'r+b') as f:
			f.write(b'\xe2')
			f.truncate(64 * 1024)
		os.unlink(os.path.join(tree, 'd', 'f'))
		os.rmdir(os.path.join(tree, 'd'))
		os.rmdir(tree)
		break

# A compose file and a Dockerfile, each reaching most of what
# import_fuzz.c's reader knows: services with every key it maps, flow and
# block lists, anchors' absence, interpolation with defaults, and the
# Dockerfile's ARG, multi-stage FROM, ENV forms and exec and shell forms.
imp = os.path.join(out, 'import')
os.makedirs(imp, exist_ok=True)
COMPOSE = """services:
  web:
    image: "nginx:${TAG:-alpine}"
    command: ["nginx", "-g", "daemon off;"]
    environment:
      - A=1
      - B
    ports:
      - "8080:80/tcp"
    volumes: ["./data:/data:ro"]
    depends_on: [db]
    healthcheck:
      test: ["CMD", "true"]
      interval: 5s
    profiles: [front]
  db:
    build:
      context: .
      dockerfile: Dockerfile
      args:
        V: 2
      target: run
    working_dir: /srv
    restart: on-failure
    deploy:
      resources:
        limits: {memory: 256M, cpus: '0.5'}
secrets:
  key:
    file: ./key
"""
DOCKERFILE = """ARG BASE=debian:13
FROM $BASE AS build
ENV A=1 B="two words"
WORKDIR /src
COPY --chown=1:1 . /src
RUN make && \\
    make install
FROM build AS run
USER 101
EXPOSE 80/tcp
ENTRYPOINT ["/bin/app"]
CMD --serve
"""
for name, text in (('compose', COMPOSE), ('dockerfile', DOCKERFILE)):
	with open(os.path.join(imp, name), 'w') as f:
		f.write(text)
