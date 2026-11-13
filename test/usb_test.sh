#!/bin/sh
# usb_test.sh -- a USB device given to a chroot machine, (devices (usb
# ...)), followed when it re-enumerates while the machine runs (project.md
# section 4.13): a board reset, a probe's firmware flashed, and the device
# comes back at a new address, its old node gone. This host has no device
# to reset at will, and making one is root's, so it is made where it can
# be: inside a vm of hull's own, from the host's tree, whose guest root
# makes a USB device of its kernel's dummy_hcd and a configfs gadget, gives
# its node to the user, and then unbinds and binds the gadget, which the
# guest's USB core sees as an unplug and a plug, the device at a new
# address. As the user, hull's core runs a chroot machine given the
# device, and an exec in it lists the device's bus before and after.
# Nothing on the host is given root. Not part of `make check`, which needs
# no KVM: `make check-usb` runs it.
#
# Its arguments are build/hull and the static probe. The vm runs under
# `timeout`, as each command inside it does, and every wait in the guest's
# script is a bounded count. It leaves the vm's conf and script, named in
# FILES, in $BUILD_DIR/test/usb, its tree removed.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/usb
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

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
	printf 'usb: needs /dev/kvm open to this user\n'
	exit 1
fi
uid=$(id -u)
gid=$(id -g)

# The chroot machine's tree: the probe, and the dirs it is given.
tree=$root/tree
case $tree in
*/test/usb/tree) rm -rf -- "$tree" ;;
*) printf 'usb: %s is not this suite'"'"'s tree\n' "$tree"; exit 1 ;;
esac
mkdir -p "$tree/dev" "$tree/proc"
cp "$probe" "$tree/probe"

# The guest's script, as its root: a gadget of vendor 1d6b product 0104 on
# dummy_hcd, its node the user's; a chroot machine given it, started as the
# user and left running; its bus listed by an exec; the gadget unbound and
# bound again, the device back at another address, that node the user's
# too; the bus listed again, and the new node's kind, a char device whose
# minor is the new address's.
cat > "$root/run.sh" <<EOF
modprobe -a dummy_hcd libcomposite usb_f_ss_lb 2> /dev/null
grep -q ' /sys/kernel/config ' /proc/mounts || mount -t configfs none /sys/kernel/config
g=/sys/kernel/config/usb_gadget/hull
mkdir -p \$g/strings/0x409 \$g/configs/c.1 \$g/functions/SourceSink.0
echo 0x1d6b > \$g/idVendor
echo 0x0104 > \$g/idProduct
echo hulltest > \$g/strings/0x409/serialnumber
ln -s \$g/functions/SourceSink.0 \$g/configs/c.1/
udc=\$(ls /sys/class/udc | head -1)
echo "\$udc" > \$g/UDC
found() {
	for d in /sys/bus/usb/devices/*; do
		[ "\$(cat \$d/idVendor 2> /dev/null)" = 1d6b ] &&
			[ "\$(cat \$d/idProduct 2> /dev/null)" = 0104 ] &&
			printf '%s %03d %03d\n' "\${d##*/}" "\$(cat \$d/busnum)" "\$(cat \$d/devnum)" && return
	done
}
tries=0
while [ -z "\$(found)" ] && [ \$tries -lt 50 ]; do tries=\$((tries + 1)); sleep 0.2; done
set -- \$(found)
port=\${1:-none} bus=\${2:-000} dev=\${3:-000}
echo "first=\$port \$bus \$dev"
chmod 666 /dev/bus/usb/\$bus/\$dev
mkdir -p /tmp/run && chown $uid:$gid /tmp/run && chmod 700 /tmp/run
as() { setpriv --reuid $uid --regid $gid --init-groups env HOME=/tmp XDG_RUNTIME_DIR=/tmp/run HULL_CORE_IDLE=1 "\$@"; }
printf '(machine (source "dir:$tree") (backend chroot) (network none) (hostname "usbm") (devices (usb (device (vendor "1d6b") (product "0104")))) (command "/probe" "sleep" "90"))\n' > /tmp/c.conf
chmod 644 /tmp/c.conf
(as timeout -k 2 120 $hull run -f /tmp/c.conf > /tmp/run.out 2>&1; echo "status=\$?" >> /tmp/run.out) &
tries=0
until as $hull ps 2> /dev/null | grep -q '^usbm .*running' || [ \$tries -ge 60 ]; do tries=\$((tries + 1)); sleep 0.5; done
echo "== before"
as timeout 20 $hull exec usbm /probe list /dev/bus/usb/\$bus 2>&1
echo "" > \$g/UDC
tries=0
while [ -n "\$(found)" ] && [ \$tries -lt 50 ]; do tries=\$((tries + 1)); sleep 0.2; done
echo "\$udc" > \$g/UDC
tries=0
while [ -z "\$(found)" ] && [ \$tries -lt 50 ]; do tries=\$((tries + 1)); sleep 0.2; done
set -- \$(found)
again=\${3:-000}
echo "second=\${1:-none} \${2:-000} \$again"
chmod 666 /dev/bus/usb/\$bus/\$again
minor=\$(( (\$(echo \$bus | sed 's/^0*//') - 1) * 128 + \$(echo \$again | sed 's/^0*//') - 1 ))
echo "minor=\$minor"
tries=0
until as timeout 20 $hull exec usbm /probe list /dev/bus/usb/\$bus 2>&1 | grep -q " \$again" || [ \$tries -ge 20 ]; do tries=\$((tries + 1)); sleep 0.5; done
echo "== after"
as timeout 20 $hull exec usbm /probe list /dev/bus/usb/\$bus kind /dev/bus/usb/\$bus/\$again 2>&1
as timeout 30 $hull stop usbm > /dev/null 2>&1
wait
echo "== run"
cat /tmp/run.out
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (memory "2G") (cpus 2) (network none) (tuning (vm (profile full))) (command "/bin/sh" "%s/run.sh"))\n' \
	"$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 400 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)
got=$?

# The guest made a device, and it came back at another address.
cases=$((cases + 1))
first=$(printf '%s\n' "$out" | sed -n 's/^first=[^ ]* [0-9]* \([0-9]*\)$/\1/p')
second=$(printf '%s\n' "$out" | sed -n 's/^second=[^ ]* [0-9]* \([0-9]*\)$/\1/p')
[ "$got" = 0 ] && [ -n "$first" ] && [ -n "$second" ] && [ "$first" != "$second" ] ||
	fail "the guest did not make a USB device that re-enumerated" "exit $got
$out"
# Before: the machine has the device's node on its bus, and no other.
cases=$((cases + 1))
before=$(printf '%s\n' "$out" | sed -n '/^== before/,/^list=/p')
printf '%s\n' "$before" | grep -qx "list= $first" ||
	fail "a chroot machine did not have the USB device's node" "$out"
# After: the old node gone and the new one there, the device's own.
cases=$((cases + 1))
after=$(printf '%s\n' "$out" | sed -n '/^== after/,/^== run/p')
minor=$(printf '%s\n' "$out" | sed -n 's/^minor=//p')
printf '%s\n' "$after" | grep -qx "list= $second" &&
	printf '%s\n' "$after" | grep -qx "kind=chr 189:$minor" ||
	fail "a chroot machine's USB device was not followed when it re-enumerated" "$out"
# And the machine ran on to its stop, not ended by the device going.
cases=$((cases + 1))
printf '%s\n' "$out" | sed -n '/^== run/,$p' | grep -q '^status=' ||
	fail "the machine did not end" "$out"

case $tree in
*/test/usb/tree) rm -rf -- "$tree" ;;
esac

printf 'usb: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
