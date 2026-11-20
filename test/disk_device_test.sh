#!/bin/sh
# disk_device_test.sh -- `hull disk write` to a drive (project.md section
# 3.13), which needs a block device the user may write, and making one is
# root's. So it is held where it can be: inside a vm of hull's own, from
# the host's tree on the host's kernel, whose root makes a loop device
# with partitions scanned, as an administrator gives a drive to a user.
# Then, as the user: refused while the device is not the user's; written
# at the device's own size and read back; the table and the tree read by
# tools that are not hull's; and refused again while a partition of it is
# mounted. Nothing on the host is given root. Not part of `make check`,
# which needs no KVM: `make check-disk` runs it.
#
# Its argument is build/hull. The vm runs under `timeout`, as each command
# inside it does; the guest's script has no loop. Its files are under
# $BUILD_DIR/test/disk-device, removed at its start and its end.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/disk-device
case $dir in
*/test/disk-device) ;;
*) printf 'disk-device: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
	printf 'disk-device: needs /dev/kvm open to this user\n'
	exit 1
fi
uid=$(id -u)
gid=$(id -g)

mkdir -p "$root/tree/etc"
printf 'on a drive\n' > "$root/tree/etc/motd"
cat > "$root/disk.conf" <<'EOF'
(disk
  (label gpt)
  (partitions
    (partition (name "boot") (size "32M") (type efi) (volume "BOOT"))
    (partition (name "root") (size "rest") (filesystem ext4) (volume "ROOT")
               (source "dir:./tree"))))
EOF

# The guest's script, as its root. The drive's owner is root:disk 0660,
# as a drive's is, until it is given to the user.
cat > "$root/run.sh" <<EOF
PATH=\$PATH:/usr/sbin:/sbin
modprobe loop 2> /dev/null
truncate -s 300M /tmp/drive.img
dev=\$(losetup -P -f --show /tmp/drive.img)
chown 0:6 "\$dev"
chmod 660 "\$dev"
mkdir -p /tmp/home /tmp/data
chown $uid:$gid /tmp/home /tmp/data
as() { setpriv --reuid $uid --regid $gid --clear-groups env HOME=/tmp/home XDG_DATA_HOME=/tmp/data PATH=\$PATH "\$@"; }
echo "== refused"
as timeout 60 $hull disk write $root/disk.conf "\$dev" 2>&1
echo "status=\$?"
chown $uid:$gid "\$dev"
echo "== written"
as timeout 120 $hull disk write $root/disk.conf "\$dev" 2>&1
echo "status=\$?"
echo "== table"
python3 -I $here/disk_table.py "\$dev"
# The user cannot have the kernel re-read the table, which is root's:
# what attaching the drive again does.
blockdev --rereadpt "\$dev"
echo "== tree"
debugfs -R 'cat /etc/motd' "\${dev}p2" 2> /dev/null
echo "== mounted"
mkdir -p /tmp/m
mount -o ro "\${dev}p2" /tmp/m && echo mounted=yes
as timeout 60 $hull disk write $root/disk.conf "\$dev" 2>&1
echo "status=\$?"
umount /tmp/m
losetup -d "\$dev"
echo "== done"
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "1G") (tuning (vm (profile full))) (command "/bin/sh" "%s/run.sh"))\n' \
	"$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 400 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)
got=$?
section() {
	printf '%s\n' "$out" | sed -n "/^== $1\$/,/^== /p"
}

cases=$((cases + 1))
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -qx '== done' ||
	fail "the guest did not run its script" "exit $got
$out"
# A drive the user may not write: refused, naming whose it is to give.
cases=$((cases + 1))
section refused | grep -q 'is not the user.s to write' && ! section refused | grep -qx 'status=0' ||
	fail "a drive not the user's was not refused" "$(section refused)"
# Written at its own size, 300 MiB, the rest running to its end.
cases=$((cases + 1))
section written | grep -q ': 300 MiB, gpt, read back as written' &&
	section written | grep -qx 'status=0' ||
	fail "the drive was not written and read back" "$(section written)"
cases=$((cases + 1))
want=$(printf '== table\ngpt ok\ngpt 1 C12A7328-F81F-11D2-BA4B-00A0C93EC93B 0 2048 67583 boot\ngpt 2 0FC63DAF-8483-4772-8E79-3D69D8477DE4 0 67584 612351 root')
[ "$(section table | sed '$d')" = "$want" ] ||
	fail "the drive's table, read by a reader of its own, is not as described" "$(section table)"
# The kernel was told the table: its partition is a device of its own,
# holding the tree.
cases=$((cases + 1))
section tree | grep -qx 'on a drive' ||
	fail "the drive's partition, by the kernel's own device, did not hold the tree" "$(section tree)"
# A partition mounted: refused, and nothing written over it.
cases=$((cases + 1))
section mounted | grep -qx 'mounted=yes' && section mounted | grep -q 'is mounted' &&
	! section mounted | grep -qx 'status=0' ||
	fail "a drive with a partition mounted was not refused" "$(section mounted)"

case $root in
*/test/disk-device) rm -rf -- "$root" ;;
esac

printf 'disk-device: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
