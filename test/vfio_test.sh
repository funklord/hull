#!/bin/sh
# vfio_test.sh -- a PCI device given by VFIO, (devices (pci ...))
# (project.md section 4.13), which this host cannot hold itself: it has no
# device bound to vfio-pci, and binding one is root's. So it is held where
# it can be: inside a vm of hull's own, from the host's tree, with
# (tuning (qemu (iommu true))) giving its guest an intel-iommu; the guest's
# root binds the guest's own NIC to vfio-pci and gives its group's node to
# the user, as an administrator does once on a host; then, as the user,
# hull's core gives the device to a chroot machine, a driver in user space
# opening it, and to a vm nested in the guest, which sees the NIC on its
# bus -- refused first where the user may lock too little memory for it.
# Nothing on the host is given root. Not part of `make check`, which needs
# no KVM: `make check-vfio` runs it.
#
# Its arguments are build/hull and the static probe. The vm runs under
# `timeout`, as each run inside it does; the guest's script has no loop.
# It leaves the vm's conf and script, named in FILES, in
# $BUILD_DIR/test/vfio, its tree removed.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/vfio
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
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

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'vfio: needs /dev/kvm and /dev/vhost-vsock open to this user\n'
	exit 1
fi
uid=$(id -u)
gid=$(id -g)

# The chroot machine's tree: the probe, and the dirs it is given.
tree=$root/tree
case $tree in
*/test/vfio/tree) rm -rf -- "$tree" ;;
*) printf 'vfio: %s is not this suite'"'"'s tree\n' "$tree"; exit 1 ;;
esac
mkdir -p "$tree/dev" "$tree/proc"
cp "$probe" "$tree/probe"

# The guest's script, as its root: vfio-pci loaded, the NIC passt's
# eth0 is unbound from virtio and bound to vfio-pci, its group's node the
# user's; KVM and vsock open to the user for the nested vm.
cat > "$root/run.sh" <<EOF
modprobe -a vfio-pci kvm_intel kvm_amd vhost_vsock 2> /dev/null
chmod 666 /dev/kvm /dev/vhost-vsock
pci=\$(basename "\$(dirname "\$(readlink -f /sys/class/net/eth0/device)")")
echo "\$pci" > /sys/bus/pci/devices/\$pci/driver/unbind
echo vfio-pci > /sys/bus/pci/devices/\$pci/driver_override
echo "\$pci" > /sys/bus/pci/drivers_probe
group=\$(basename "\$(readlink /sys/bus/pci/devices/\$pci/iommu_group)")
echo "pci=\$pci group=\$group driver=\$(basename "\$(readlink /sys/bus/pci/devices/\$pci/driver)")"
chown $uid:$gid /dev/vfio/\$group
mkdir -p /tmp/r1 /tmp/r2 /tmp/r3 && chown $uid:$gid /tmp/r1 /tmp/r2 /tmp/r3 && chmod 700 /tmp/r1 /tmp/r2 /tmp/r3
as() { setpriv --reuid $uid --regid $gid --init-groups env HOME=/tmp HULL_CORE_IDLE=1 "\$@"; }
printf '(machine (source "dir:$tree") (backend chroot) (network none) (devices (pci "%s")) (command "/probe" "vfio" "/dev/vfio/%s" "%s"))\n' "\$pci" "\$group" "\$pci" > /tmp/c.conf
printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "256M") (devices (pci "%s")) (command "/bin/sh" "-c" "grep -l 0x0200 /sys/bus/pci/devices/*/class > /dev/null && echo nic=seen"))\n' "\$pci" > /tmp/v.conf
chmod 644 /tmp/c.conf /tmp/v.conf
echo "== chroot"
as env XDG_RUNTIME_DIR=/tmp/r1 timeout -k 2 60 $hull run -f /tmp/c.conf 2>&1
echo "status=\$?"
echo "== locked"
(ulimit -l 8192; as env XDG_RUNTIME_DIR=/tmp/r2 timeout -k 2 60 $hull run -f /tmp/v.conf 2>&1; echo "status=\$?")
echo "== nested"
(ulimit -l unlimited; as env XDG_RUNTIME_DIR=/tmp/r3 timeout -k 2 120 $hull run -f /tmp/v.conf 2>&1; echo "status=\$?")
echo "== pc"
mkdir -p /tmp/r4 && chown $uid:$gid /tmp/r4 && chmod 700 /tmp/r4
truncate -s 1M /tmp/blank.img && chmod 644 /tmp/blank.img
printf '(machine (backend pc) (source "image:/tmp/blank.img") (memory "256M") (devices (pci "%s")))\n' "\$pci" > /tmp/p.conf
chmod 644 /tmp/p.conf
(ulimit -l unlimited; as env XDG_RUNTIME_DIR=/tmp/r4 timeout -k 2 60 $hull run -f /tmp/p.conf > /tmp/p.out 2>&1) &
sleep 8
for p in \$(pgrep -x qemu-system-x86); do
	grep -q "vfio-pci,host=\$pci" /proc/\$p/cmdline && ls -l /proc/\$p/fd | grep -q "/dev/vfio/\$group" && echo pc=has-card
done
for id in \$(as env XDG_RUNTIME_DIR=/tmp/r4 $hull ps | awk '\$5 == "running" { print \$1 }'); do
	as env XDG_RUNTIME_DIR=/tmp/r4 timeout 30 $hull stop "\$id" > /dev/null 2>&1
done
wait
cat /tmp/p.out
echo "status=done"
echo "== kept"
mkdir -p /tmp/r5 && chown $uid:$gid /tmp/r5 && chmod 700 /tmp/r5
printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "256M") (hostname "kept") (on_disconnect keep) (devices (pci "%s")) (command "/bin/sleep" "40"))\n' "\$pci" > /tmp/k.conf
chmod 644 /tmp/k.conf
(ulimit -l unlimited; as env XDG_RUNTIME_DIR=/tmp/r5 timeout -k 2 60 $hull run -f /tmp/k.conf > /dev/null 2>&1) &
for i in \$(seq 1 60); do as env XDG_RUNTIME_DIR=/tmp/r5 $hull ps 2> /dev/null | grep -q '^kept .*running' && break; sleep 0.5; done
kill -KILL \$(head -1 /tmp/r5/hull/core.lock)
wait
as env XDG_RUNTIME_DIR=/tmp/r5 timeout 30 $hull ps
(ulimit -l unlimited; as env XDG_RUNTIME_DIR=/tmp/r5 timeout -k 2 30 $hull run -f /tmp/v.conf 2>&1)
as env XDG_RUNTIME_DIR=/tmp/r5 timeout 30 $hull stop kept > /dev/null 2>&1
echo "status=done"
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (memory "2G") (cpus 2) (tuning (qemu (iommu true))) (command "/bin/sh" "%s/run.sh"))\n' \
	"$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 500 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)
got=$?

# The guest has an IOMMU, and its NIC is vfio-pci's in a group of its own.
cases=$((cases + 1))
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -Eq '^pci=[0-9a-f:.]+ group=[0-9]+ driver=vfio-pci$' ||
	fail "the guest gave no device to vfio-pci" "exit $got
$out"
# A chroot machine's driver in user space has the device: its group
# viable, a type 1 container, the device's regions.
cases=$((cases + 1))
chroot=$(printf '%s\n' "$out" | sed -n '/^== chroot/,/^status=/p')
printf '%s\n' "$chroot" | grep -Eq '^vfio=ok pci=1 regions=[1-9][0-9]*$' &&
	printf '%s\n' "$chroot" | grep -qx 'status=0' ||
	fail "a chroot machine did not have the device by VFIO" "$out"
# A vm nested in the guest, refused where the user may lock too little
# of its memory, naming the limit; and given it, seeing the NIC.
cases=$((cases + 1))
locked=$(printf '%s\n' "$out" | sed -n '/^== locked/,/^status=/p')
printf '%s\n' "$locked" | grep -q 'pins the guest.s 256 MiB, and the user may lock 8 MiB' &&
	! printf '%s\n' "$locked" | grep -qx 'status=0' ||
	fail "a vm with too little locked memory for its device was not refused" "$out"
cases=$((cases + 1))
nested=$(printf '%s\n' "$out" | sed -n '/^== nested/,/^status=/p')
printf '%s\n' "$nested" | grep -qx 'nic=seen' &&
	printf '%s\n' "$nested" | grep -qx 'status=0' ||
	fail "a nested vm was not given the device" "$out"
# A pc machine, Windows' kind, given it: its qemu holding the group.
cases=$((cases + 1))
pc=$(printf '%s\n' "$out" | sed -n '/^== pc/,/^status=/p')
printf '%s\n' "$pc" | grep -qx 'pc=has-card' ||
	fail "a pc machine was not given the device" "$out"
# A kept vm holding it, its core killed and the machine taken back by the
# next: a second machine asking for it is refused by hull, the claim
# carried over, and not left to qemu's own error.
cases=$((cases + 1))
kept=$(printf '%s\n' "$out" | sed -n '/^== kept/,/^status=/p')
printf '%s\n' "$kept" | grep -q '^kept .*running' &&
	printf '%s\n' "$kept" | grep -q "another running machine's" ||
	fail "a taken-back vm's device was not held for it" "$out"

case $tree in
*/test/vfio/tree) rm -rf -- "$tree" ;;
esac

printf 'vfio: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
