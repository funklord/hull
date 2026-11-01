"""seeds.py -- the corpora check-fuzz starts from, written into the
directory it is given: JSON a registry sends, and tar layers of each kind
the unpacker meets -- files, directories, links, whiteouts, long names in
pax and GNU form, gzip, and paths that try to leave the tree."""

import gzip
import io
import os
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
