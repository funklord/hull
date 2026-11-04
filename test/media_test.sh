#!/bin/sh
# media_test.sh -- `hull media import`, `verify` and `export` (project.md
# section 3.10): a disc as plain files and back, byte for byte. Its
# argument is build/hull. The disc is iso.py's, which shares no code with
# hull. It writes only under $BUILD_DIR/test/media, removed at its start
# and its end, and each command is under `timeout`.

set -u

hull=$1
dir=${BUILD_DIR:?}/test/media
case $dir in
*/test/media) ;;
*) printf 'media: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir"
here=$(dirname "$0")
cases=0
failures=0

check() { # NAME STATUS WANTED OUTPUT
	cases=$((cases + 1))
	[ "$2" = "$3" ] && return
	failures=$((failures + 1))
	printf 'FAIL: %s: exited %s, wanted %s\n' "$1" "$2" "$3"
	printf '%s\n' "$4" | sed 's/^/    /'
}

media() {
	out=$(timeout 30 "$hull" media "$@" 2>&1)
	got=$?
}

python3 -I "$here/iso.py" "$dir/disc.iso" README.TXT 'on a disc'

# Its file is a file, and the descriptors and directories are the patch.
media import "$dir/disc.iso" "$dir/disc"
check 'an ISO imported' "$got" 0 "$out"
cases=$((cases + 1))
grep -q 'on a disc' "$dir/disc/files/README.TXT" 2>/dev/null ||
	{ failures=$((failures + 1)); printf 'FAIL: the disc'"'"'s file is not a file\n'; }
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '1 file' ||
	{ failures=$((failures + 1)); printf 'FAIL: it said: %s\n' "$out"; }
media verify "$dir/disc"
check 'the collection verifies' "$got" 0 "$out"
media export "$dir/disc" "$dir/again.iso"
check 'the collection exports' "$got" 0 "$out"
cmp -s "$dir/disc.iso" "$dir/again.iso"
check 'the export is the disc, byte for byte' "$?" 0 "$(cmp "$dir/disc.iso" "$dir/again.iso" 2>&1)"

# An edit is seen, a file changed in place or grown past its size alike.
cp "$dir/disc/files/README.TXT" "$dir/kept"
printf 'X' | dd of="$dir/disc/files/README.TXT" bs=1 count=1 conv=notrunc 2>/dev/null
media verify "$dir/disc"
check 'a file changed fails' "$got" 1 "$out"
cp "$dir/kept" "$dir/disc/files/README.TXT"
printf 'more' >> "$dir/disc/files/README.TXT"
media verify "$dir/disc"
check 'a file grown fails' "$got" 1 "$out"
cp "$dir/kept" "$dir/disc/files/README.TXT"
media verify "$dir/disc"
check 'put back, it verifies again' "$got" 0 "$out"

# A layout may not leave the collection, whoever wrote it.
cp "$dir/disc/layout.conf" "$dir/layout.kept"
printf '20 1 ../media.conf\n' >> "$dir/disc/layout.conf"
media verify "$dir/disc"
check 'a path out of files/ is refused' "$got" 1 "$out"
cp "$dir/layout.kept" "$dir/disc/layout.conf"

# What is no ISO is the honest blob: exact, and all of it patch.
head -c 5000 /dev/urandom > "$dir/blob.bin"
media import "$dir/blob.bin" "$dir/blob"
check 'a blob imported' "$got" 0 "$out"
media export "$dir/blob" "$dir/blob.again"
check 'a blob exports' "$got" 0 "$out"
cmp -s "$dir/blob.bin" "$dir/blob.again"
check 'a blob is itself' "$?" 0 ''

# A disc that lies, in the structures an importer trusts, comes back as it
# was, its lies in the patch, with nothing written outside the collection.
python3 -I - "$dir/disc.iso" "$dir/lying.iso" <<'PY'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
root = 20 * 2048
# Past the root's . and ..: a name that climbs out, and an extent past the
# image's end, in the one file record.
at = root + b[root] + b[root + b[root]]
b[at + 2:at + 6] = (0x7fffffff).to_bytes(4, 'little')
name = b'../../ESCAPE'
b[at + 32] = len(name)
b[at + 33:at + 33 + len(name)] = name
open(sys.argv[2], 'wb').write(b)
PY
media import "$dir/lying.iso" "$dir/lying"
check 'a lying disc imported' "$got" 0 "$out"
media export "$dir/lying" "$dir/lying.again"
check 'a lying disc exports' "$got" 0 "$out"
cmp -s "$dir/lying.iso" "$dir/lying.again"
check 'a lying disc is itself' "$?" 0 ''
[ ! -e "$dir/ESCAPE" ] && [ ! -e "$dir/../ESCAPE" ]
check 'nothing escaped' "$?" 0 ''

media import "$dir/disc.iso" "$dir/disc"
check 'an existing directory is refused' "$got" 1 "$out"

# A raw dump. Every sector here is what its data says, by raw_disc.py's
# reading of ECMA-130, so nothing of the raw layer is left over: that no
# quirks/raw exists is hull and the standard agreeing.
python3 -I "$here/raw_disc.py" "$dir/disc.iso" "$dir/game.bin" "$dir/game.cue"
media import "$dir/game.cue" "$dir/game"
check 'a dump imported' "$got" 0 "$out"
[ -f "$dir/game/files/README.TXT" ] && [ -f "$dir/game/tracks/02.wav" ] && [ -f "$dir/game/disc.cue" ]
check 'its file, track and sheet are files' "$?" 0 "$(ls -R "$dir/game")"
[ ! -e "$dir/game/quirks/raw" ]
check 'every sector regenerated' "$?" 0 "$(ls -la "$dir/game/quirks/raw" 2>&1)"
python3 -I -c '
import sys, wave
w = wave.open(sys.argv[1])
assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (2, 2, 44100)
assert w.getnframes() == 2352 * 75 // 4' "$dir/game/tracks/02.wav"
check 'the track is a WAVE of a second' "$?" 0 ''
media verify "$dir/game"
check 'the dump verifies' "$got" 0 "$out"
media export "$dir/game" "$dir/game.again"
check 'the dump exports' "$got" 0 "$out"
cmp -s "$dir/game.bin" "$dir/game.again"
check 'the dump is itself, byte for byte' "$?" 0 "$(cmp "$dir/game.bin" "$dir/game.again" 2>&1)"
printf 'X' | dd of="$dir/game/tracks/02.wav" bs=1 seek=1000 count=1 conv=notrunc 2>/dev/null
media verify "$dir/game"
check 'an audio track changed fails' "$got" 1 "$out"

# A sector with an EDC its data does not give, as a protection has: that
# sector, and no other, is the quirk.
python3 -I "$here/raw_disc.py" "$dir/disc.iso" "$dir/prot.bin" "$dir/prot.cue" bad-edc 18
media import "$dir/prot.cue" "$dir/prot"
check 'a protected dump imported' "$got" 0 "$out"
[ "$(ls "$dir/prot/quirks/raw" 2>&1)" = 0x12.raw ]
check 'the bad sector is the quirk' "$?" 0 "$(ls "$dir/prot/quirks/raw" 2>&1)"
media export "$dir/prot" "$dir/prot.again"
check 'a protected dump exports' "$got" 0 "$out"
cmp -s "$dir/prot.bin" "$dir/prot.again"
check 'a protected dump is itself' "$?" 0 ''

# A sheet of several files, a track each: back as the files they were.
python3 -I "$here/raw_disc.py" "$dir/disc.iso" "$dir/set.bin" "$dir/set.cue" split
media import "$dir/set.cue" "$dir/set"
check 'a set imported' "$got" 0 "$out"
[ ! -e "$dir/set/quirks/raw" ] && [ -f "$dir/set/tracks/02.wav" ]
check 'its sectors regenerated and its track a file' "$?" 0 "$(ls -R "$dir/set" 2>&1)"
media export "$dir/set" "$dir/set.out"
check 'a set exports' "$got" 0 "$out"
cmp -s "$dir/set.bin" "$dir/set.out/set.bin" && cmp -s "$dir/set.bin.2" "$dir/set.out/set.bin.2" &&
	cmp -s "$dir/set.cue" "$dir/set.out/disc.cue"
check 'each file of a set is itself' "$?" 0 "$(ls -la "$dir/set.out" 2>&1)"

# A floppy of FAT, mtools' rather than hull's, a file fragmented by a
# delete: its bytes back in a file, and the image exact.
if command -v mformat > /dev/null 2>&1; then
	mformat -C -f 1440 -v HULLDISK -i "$dir/fd.img" ::
	head -c 200000 /dev/urandom > "$dir/a.bin"
	head -c 100000 /dev/urandom > "$dir/b.bin"
	head -c 500000 /dev/urandom > "$dir/c.bin"
	mcopy -i "$dir/fd.img" "$dir/a.bin" ::A.BIN
	mcopy -i "$dir/fd.img" "$dir/b.bin" ::B.BIN
	mmd -i "$dir/fd.img" ::GAME
	mdel -i "$dir/fd.img" ::A.BIN
	mcopy -i "$dir/fd.img" "$dir/c.bin" ::GAME/C.BIN
	media import "$dir/fd.img" "$dir/fd"
	check 'a floppy imported' "$got" 0 "$out"
	cmp -s "$dir/c.bin" "$dir/fd/files/GAME/C.BIN"
	check 'a fragmented file is its bytes' "$?" 0 "$(cat "$dir/fd/layout.conf" 2>&1)"
	grep -q '@0/500000 GAME/C.BIN$' "$dir/fd/layout.conf"
	check 'it is fragmented, so that means something' "$?" 0 "$(cat "$dir/fd/layout.conf" 2>&1)"
	grep -q '(volume "HULLDISK")' "$dir/fd/media.conf"
	check 'its label is read' "$?" 0 "$(cat "$dir/fd/media.conf" 2>&1)"
	media export "$dir/fd" "$dir/fd.again"
	check 'a floppy exports' "$got" 0 "$out"
	cmp -s "$dir/fd.img" "$dir/fd.again"
	check 'a floppy is itself' "$?" 0 ''
	# A chain that loops is no file, and exact all the same.
	python3 -I - "$dir/fd.img" "$dir/loop.img" <<'LOOPED'
import sys
b = bytearray(open(sys.argv[1], 'rb').read())
# FAT12, sector 1: cluster 2's entry made to name cluster 2.
b[512 + 3] = 2
b[512 + 4] = (b[512 + 4] & 0xF0)
open(sys.argv[2], 'wb').write(b)
LOOPED
	media import "$dir/loop.img" "$dir/loop"
	check 'a looping chain imported' "$got" 0 "$out"
	media export "$dir/loop" "$dir/loop.again"
	check 'a looping chain exports' "$got" 0 "$out"
	cmp -s "$dir/loop.img" "$dir/loop.again"
	check 'a looping chain is itself' "$?" 0 ''

	# A partitioned disk, MBR and a FAT partition, by mtools again, and a
	# GPT with an ESP, hull's own export: each tree out, each disk exact.
	head -c 20971520 /dev/zero > "$dir/hd.img"
	cat > "$dir/mtools.conf" <<MTOOLS
drive h: file="$dir/hd.img" partition=1
MTOOLS
	MTOOLSRC=$dir/mtools.conf mpartition -I -c -T 6 -s 32 -h 4 -t 320 h: 2> /dev/null
	MTOOLSRC=$dir/mtools.conf mformat h: 2> /dev/null
	MTOOLSRC=$dir/mtools.conf mcopy "$dir/b.bin" h:B.BIN 2> /dev/null
	media import "$dir/hd.img" "$dir/hd"
	check 'a partitioned disk imported' "$got" 0 "$out"
	cmp -s "$dir/b.bin" "$dir/hd/files/p1/B.BIN"
	check 'its partition is a tree' "$?" 0 "$(ls -R "$dir/hd/files" 2>&1)"
	media export "$dir/hd" "$dir/hd.again"
	check 'a partitioned disk exports' "$got" 0 "$out"
	cmp -s "$dir/hd.img" "$dir/hd.again"
	check 'a partitioned disk is itself' "$?" 0 ''
else
	printf 'media: FAT not measured: mtools is not installed\n'
fi

# ext2 at 1 KiB blocks, whose big file reaches double indirection, ext3,
# and ext4 by extents: mke2fs's, not hull's. A name of a space and a +, a
# link, which is no file, and a hole.
mke2fs=$(command -v mke2fs || ls /usr/sbin/mke2fs /sbin/mke2fs 2>/dev/null | head -1)
if [ -n "$mke2fs" ]; then
	mkdir -p "$dir/tree/sub/deep"
	head -c 3000000 /dev/urandom > "$dir/tree/big.bin"
	head -c 5000 /dev/urandom > "$dir/tree/sub/deep/small"
	printf 'hi\n' > "$dir/tree/sub/lib++ name"
	: > "$dir/tree/empty"
	ln -s big.bin "$dir/tree/link"
	dd if=/dev/urandom of="$dir/tree/holed" bs=4096 count=1 seek=100 2>/dev/null
	for fs in ext2:1024 ext3:4096 ext4:4096; do
		type=${fs%:*}
		"$mke2fs" -q -F -t "$type" -b "${fs#*:}" -L HULLFS -d "$dir/tree" "$dir/$type.img" 16M
		media import "$dir/$type.img" "$dir/$type"
		check "$type imported" "$got" 0 "$out"
		diff -r -x link -x lost+found "$dir/tree" "$dir/$type/files" > "$dir/differs" 2>&1
		check "$type's tree is the tree" "$?" 0 "$(cat "$dir/differs")"
		grep -q '(volume "HULLFS")' "$dir/$type/media.conf"
		check "$type's label is read" "$?" 0 "$(cat "$dir/$type/media.conf")"
		media export "$dir/$type" "$dir/$type.again"
		check "$type exports" "$got" 0 "$out"
		cmp -s "$dir/$type.img" "$dir/$type.again"
		check "$type is itself" "$?" 0 ''
	done
	cases=$((cases + 1))
	patch=$(find "$dir/ext4/quirks" -type f -printf '%s\n' | awk '{ n += $1 } END { print n + 0 }')
	[ "$patch" -lt 1000000 ] ||
		{ failures=$((failures + 1)); printf 'FAIL: ext4 is mostly patch: %s\n' "$patch"; }
else
	printf 'media: ext not measured: mke2fs is not installed\n'
fi

printf 'media: %d cases, %d failed\n' "$cases" "$failures"
rm -rf "$dir"
[ "$failures" -eq 0 ]
