#!/bin/sh
# disk_test.sh -- `hull disk write` (project.md section 3.13): a (disk ...)
# written to an image file, read by readers that share no code with hull --
# disk_table.py for the table, e2fsprogs' debugfs and mtools for the trees,
# SeaBIOS for the boot -- and the refusals. Its argument is build/hull. It
# writes only under $BUILD_DIR/test/disk, removed at its start and its
# end, its store there too, and each command is under `timeout`. The
# drive case, which needs a block device and so root, is device_test's,
# in a vm (`make check-disk`).

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/disk
case $dir in
*/test/disk) ;;
*) printf 'disk: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir"
dir=$(cd "$dir" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
XDG_DATA_HOME=$dir/data
export XDG_DATA_HOME
PATH=$PATH:/usr/sbin:/sbin
cases=0
failures=0

check() { # NAME STATUS WANTED OUTPUT
	cases=$((cases + 1))
	[ "$2" = "$3" ] && return
	failures=$((failures + 1))
	printf 'FAIL: %s: got %s, wanted %s\n' "$1" "$2" "$3"
	printf '%s\n' "$4" | sed 's/^/    /'
}

write() { # CONF TARGET
	out=$(timeout 60 "$hull" disk write "$1" "$2" 2>&1)
	got=$?
}

table() {
	python3 -I "$here/disk_table.py" "$1"
}

# The trees: an ext one with a link and a nested file, a FAT one.
mkdir -p "$dir/tree/etc" "$dir/tree/boot/sub" "$dir/fat/EFI" "$dir/syslinux"
printf 'hello from the tree\n' > "$dir/tree/etc/motd"
printf 'deep\n' > "$dir/tree/boot/sub/file"
ln -s ../etc/motd "$dir/tree/boot/link"
printf 'efi\n' > "$dir/fat/EFI/x.txt"
mkdir -p "$dir/big"
head -c 3145728 /dev/urandom > "$dir/big/blob"
head -c 440 /dev/urandom > "$dir/code.bin"

cat > "$dir/dos.conf" <<'EOF'
(disk
  (label dos)
  (boot "code.bin")
  (partitions
    (partition (size "64M") (type fat) (volume "BOOT") (source "dir:./fat"))
    (partition (size "rest") (active true) (filesystem ext4)
               (features "^64bit" "^metadata_csum") (volume "RESCUE") (source "dir:./tree"))))
EOF

# A dos disk sized by what it holds: its table as written, the boot code
# and the flag where they were asked for.
write "$dir/dos.conf" "$dir/dos.img"
check 'a dos disk written and read back' "$got" 0 "$out"
t=$(table "$dir/dos.img")
want=$(printf 'dos 1 0e 0 2048 131072\ndos 2 83 1 133120 133120')
check 'its table, by a reader of its own' "$(printf '%s\n' "$t" | sed 1d)" "$want" "$t"
cmp -s -n 440 "$dir/dos.img" "$dir/code.bin"
check 'the boot code in its first 440 bytes' $? 0 ''
# Each tree, read by tools that are not hull's: debugfs at the offset,
# mtools at the other's.
got=$(debugfs -R 'cat /etc/motd' "$dir/dos.img?offset=$((133120 * 512))" 2> /dev/null)
check 'the ext tree read by debugfs' "$got" 'hello from the tree' ''
got=$(debugfs -R 'stat /boot/link' "$dir/dos.img?offset=$((133120 * 512))" 2> /dev/null |
	sed -n 's/.*Fast link dest: "\(.*\)"/\1/p')
check 'its link a link' "$got" '../etc/motd' ''
got=$(debugfs -R features "$dir/dos.img?offset=$((133120 * 512))" 2> /dev/null)
case $got in *64bit* | *metadata_csum*) r=on ;; *) r=off ;; esac
check 'the features turned off' "$r" off "$got"
got=$(debugfs -R stats "$dir/dos.img?offset=$((133120 * 512))" 2> /dev/null |
	sed -n 's/^Filesystem volume name: *//p')
check 'its volume name' "$got" RESCUE ''
got=$(MTOOLSRC=/dev/null mtype -i "$dir/dos.img@@1048576" ::/EFI/x.txt 2>&1)
check 'the FAT tree read by mtools' "$got" efi ''

# One description, one disk: written again, the same bytes.
write "$dir/dos.conf" "$dir/again.img"
cmp -s "$dir/dos.img" "$dir/again.img"
check 'written twice, the same bytes' $? 0 "$out"

# GPT at a size of its own: both headers and arrays, their CRCs, the
# rest to the end less the copy's room, the flag as legacy BIOS bootable.
sed 's/(label dos)/(label gpt)/; s/(boot "code.bin")/(size "300M")/' "$dir/dos.conf" > "$dir/gpt.conf"
write "$dir/gpt.conf" "$dir/gpt.img"
check 'a gpt disk written and read back' "$got" 0 "$out"
t=$(table "$dir/gpt.img")
want=$(printf 'gpt ok\ngpt 1 EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 0 2048 133119 -\ngpt 2 0FC63DAF-8483-4772-8E79-3D69D8477DE4 4 133120 612351 -')
check 'its table, by a reader of its own' "$t" "$want" "$t"
got=$(stat -c %s "$dir/gpt.img")
check 'the size it was given' "$got" $((300 * 1024 * 1024)) ''

# Refused, each naming why, and nothing left behind.
refuse() { # NAME CONF-BODY WORD
	printf '%s\n' "$2" > "$dir/bad.conf"
	write "$dir/bad.conf" "$dir/bad.img"
	r=fine
	[ "$got" != 0 ] && printf '%s\n' "$out" | grep -q "$3" && [ ! -e "$dir/bad.img" ] && r=refused
	check "$1" "$r" refused "$out"
}
refuse 'rest before the last' '(disk (label dos) (partitions (partition (size "rest")) (partition (size "1M"))))' 'only the last'
refuse 'a tree with no filesystem' '(disk (label dos) (partitions (partition (size "8M") (filesystem none) (source "dir:./fat"))))' 'no filesystem'
refuse 'features on FAT' '(disk (label dos) (partitions (partition (size "8M") (type fat) (features "^64bit"))))' "mke2fs's"
refuse 'SYSLINUX on ext' '(disk (label dos) (partitions (partition (size "8M") (loader syslinux))))' 'EXTLINUX'
refuse 'a BIOS boot partition on dos' '(disk (label dos) (partitions (partition (size "1M") (type bios))))' "GPT's"
refuse 'too small a disk' '(disk (label dos) (size "4M") (partitions (partition (size "8M"))))' 'need'
refuse 'a tree larger than its partition' '(disk (label dos) (partitions (partition (size "1M") (source "dir:./big"))))' 'could not be made'
refuse 'boot code of the wrong size' '(disk (label dos) (boot "dos.conf") (partitions (partition (size "8M"))))' '440'
refuse 'an image as a tree' '(disk (label dos) (partitions (partition (size "8M") (source "image:./x"))))' 'an image is none'
mkdir -p "$dir/case"
printf 'lower\n' > "$dir/case/x"
printf 'upper\n' > "$dir/case/X"
refuse 'names FAT folds into one' '(disk (label dos) (partitions (partition (size "8M") (type fat) (source "dir:./case"))))' 'folds'
refuse 'a feature with a comma' '(disk (label dos) (partitions (partition (size "8M") (features "a,b"))))' 'feature'

# A file that is there is written over; one that the write refuses is
# left as it was, a refusal before the write touching nothing.
printf 'keep\n' > "$dir/keep.img"
printf '(disk (label dos) (partitions (partition (size "8M") (filesystem none) (source "dir:./fat"))))\n' > "$dir/bad.conf"
write "$dir/bad.conf" "$dir/keep.img"
check 'a refused write leaves a file alone' "$(cat "$dir/keep.img")" keep "$out"

# It boots: SeaBIOS reads the MBR's code, which chains to the active
# partition, where SYSLINUX finds its configuration in the tree.
if command -v qemu-system-x86_64 > /dev/null && command -v syslinux > /dev/null &&
   [ -r /usr/lib/syslinux/mbr/mbr.bin ]; then
	printf 'SERIAL 0 115200\nPROMPT 0\nDEFAULT none\nLABEL none\n  KERNEL /hull-disk-booted\n' \
		> "$dir/syslinux/syslinux.cfg"
	cat > "$dir/boot.conf" <<'EOF'
(disk
  (label dos)
  (boot "/usr/lib/syslinux/mbr/mbr.bin")
  (partitions
    (partition (size "rest") (type fat) (active true) (loader syslinux) (volume "HULLBOOT")
               (source "dir:./syslinux"))))
EOF
	write "$dir/boot.conf" "$dir/boot.img"
	check 'a SYSLINUX disk written and read back' "$got" 0 "$out"
	accel=tcg
	[ -r /dev/kvm ] && [ -w /dev/kvm ] && accel=kvm
	timeout 20 qemu-system-x86_64 -nodefaults -no-user-config -machine "pc,accel=$accel" -m 128 \
		-drive "file=$dir/boot.img,format=raw,snapshot=on" -serial "file:$dir/serial.log" \
		-display none > /dev/null 2>&1
	grep -aq 'Loading /hull-disk-booted' "$dir/serial.log"
	check 'it boots under SeaBIOS to its own configuration' $? 0 "$(cat -v "$dir/serial.log")"
	# The same disk imported as files, described by the import, and
	# written again at another size: it boots as it did.
	timeout 60 "$hull" media import "$dir/boot.img" "$dir/boot-files" > /dev/null 2>&1
	sed 's/(label dos)/(label dos) (size "200M")/' "$dir/boot-files/disk.conf" > "$dir/boot-files/big.conf"
	write "$dir/boot-files/big.conf" "$dir/boot-big.img"
	check 'an imported SYSLINUX disk written at 200M' "$got" 0 "$out
$(cat "$dir/boot-files/disk.conf")"
	rm -f "$dir/serial.log"
	timeout 20 qemu-system-x86_64 -nodefaults -no-user-config -machine "pc,accel=$accel" -m 128 \
		-drive "file=$dir/boot-big.img,format=raw,snapshot=on" -serial "file:$dir/serial.log" \
		-display none > /dev/null 2>&1
	grep -aq 'Loading /hull-disk-booted' "$dir/serial.log"
	check 'it boots at its new size' $? 0 "$(cat -v "$dir/serial.log")"
else
	printf 'disk: no qemu-system-x86_64, syslinux or its mbr.bin: the boot is not tried\n'
fi

# EXTLINUX on ext4, the rescue stick's layout: installed by a hull vm's
# root, the partition copied whole and grown to the rest of a 256M disk,
# and booted by SeaBIOS through the MBR's code to EXTLINUX reading its
# configuration from the tree.
if [ -r /dev/kvm ] && [ -w /dev/kvm ] && command -v extlinux > /dev/null &&
   command -v qemu-system-x86_64 > /dev/null && [ -r /usr/lib/EXTLINUX/mbr.bin ]; then
	XDG_RUNTIME_DIR=$dir/run HULL_CORE_IDLE=1
	export XDG_RUNTIME_DIR HULL_CORE_IDLE
	mkdir -p "$XDG_RUNTIME_DIR" "$dir/ext/boot/extlinux"
	chmod 700 "$XDG_RUNTIME_DIR"
	printf 'SERIAL 0 115200\nPROMPT 0\nDEFAULT none\nLABEL none\n  KERNEL /hull-ext-booted\n' \
		> "$dir/ext/boot/extlinux/extlinux.conf"
	cat > "$dir/ext.conf" <<'EOF'
(disk
  (label dos)
  (size "256M")
  (boot "/usr/lib/EXTLINUX/mbr.bin")
  (partitions
    (partition (size "rest") (active true) (filesystem ext4) (loader extlinux)
               (features "^64bit" "^metadata_csum") (volume "RESCUE") (source "dir:./ext"))))
EOF
	out=$(timeout 300 "$hull" disk write "$dir/ext.conf" "$dir/ext.img" 2>&1)
	got=$?
	check 'an EXTLINUX disk written by a vm and read back' "$got" 0 "$out"
	got=$(debugfs -R 'ls /boot/extlinux' "$dir/ext.img?offset=1048576" 2> /dev/null | tr -s ' \n' ' ')
	case $got in *ldlinux.sys*) r=installed ;; *) r=absent ;; esac
	check 'EXTLINUX in the tree' "$r" installed "$got"
	got=$(debugfs -R stats "$dir/ext.img?offset=1048576" 2> /dev/null |
		sed -n 's/^Block count: *//p')
	check 'its filesystem grown to the partition' "$got" 261120 ''
	rm -f "$dir/serial.log"
	timeout 20 qemu-system-x86_64 -nodefaults -no-user-config -machine pc,accel=kvm -m 128 \
		-drive "file=$dir/ext.img,format=raw,snapshot=on" -serial "file:$dir/serial.log" \
		-display none > /dev/null 2>&1
	grep -aq 'Loading /hull-ext-booted' "$dir/serial.log"
	check 'it boots under SeaBIOS to EXTLINUX and its configuration' $? 0 \
		"$(cat -v "$dir/serial.log")"
else
	printf 'disk: no KVM, extlinux, qemu or its mbr.bin: EXTLINUX is not tried\n'
fi

# A disk imported by `hull media import` is described beside its files:
# written from that description at another size, its table is the same
# kinds, flags and labels, its ext features the same, and its trees the
# same; the exact disk stays `media export`'s.
timeout 60 "$hull" media import "$dir/dos.img" "$dir/dos-files" > /dev/null 2>&1
check 'the disk imported' $? 0 ''
[ -f "$dir/dos-files/disk.conf" ] && cmp -s -n 440 "$dir/dos-files/boot.bin" "$dir/code.bin"
check 'described, its boot code beside it' $? 0 "$(cat "$dir/dos-files/disk.conf" 2>&1)"
sed 's/(label dos)/(label dos) (size "512M")/' "$dir/dos-files/disk.conf" > "$dir/dos-files/big.conf"
write "$dir/dos-files/big.conf" "$dir/big.img"
check 'written from its description at 512M' "$got" 0 "$out
$(cat "$dir/dos-files/disk.conf")"
t=$(table "$dir/big.img")
want=$(printf 'dos 1 0e 0 2048 131072\ndos 2 83 1 133120 915456')
check 'its table, the rest grown' "$(printf '%s\n' "$t" | sed 1d)" "$want" "$t"
a=$(debugfs -R features "$dir/dos.img?offset=$((133120 * 512))" 2> /dev/null)
b=$(debugfs -R features "$dir/big.img?offset=$((133120 * 512))" 2> /dev/null)
check 'its ext features the same' "$b" "$a" "$(cat "$dir/dos-files/disk.conf")"
a=$(debugfs -R stats "$dir/big.img?offset=$((133120 * 512))" 2> /dev/null |
	sed -n 's/^Filesystem volume name: *//p')
check 'its label the same' "$a" RESCUE ''
got=$(debugfs -R 'cat /boot/sub/file' "$dir/big.img?offset=$((133120 * 512))" 2> /dev/null)
check 'its tree the same' "$got" deep ''
got=$(MTOOLSRC=/dev/null mtype -i "$dir/big.img@@1048576" ::/EFI/x.txt 2>&1)
check 'its FAT tree the same' "$got" efi ''

case $dir in
*/test/disk) rm -rf -- "$dir" ;;
esac

printf 'disk: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
