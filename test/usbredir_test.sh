#!/bin/sh
# usbredir_test.sh -- a USB device on another host given to a vm
# (project.md section 4.13): (devices (usb (device (vendor ...) (product
# ...) (host "far")))), the qemu engine's ssh running usbredirect there and
# forwarding its port to qemu's usb-redir. The far host is a vm of hull's
# own, the outer machine, whose root makes a USB device of dummy_hcd and a
# configfs gadget, installs Debian's usbredirect and runs an sshd on its
# loopback; as the user, a vm nested in it is given the device by `far`,
# an ssh alias for that sshd, and finds the device's ids on its own bus.
# Not part of `make check`: it needs KVM nested, the network once for
# Debian's usbredirect and its library, and about two minutes:
# `make check-usbredir`.
#
# Its argument is build/hull. The packages are kept in
# $BUILD_DIR/test/usbredir-data; everything else is under
# $BUILD_DIR/test/usbredir, removed at the start. Every run is under
# `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/usbredir
case $dir in
*/test/usbredir) ;;
*) printf 'usbredir: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
	printf 'usbredir: not tried: no /dev/kvm open to this user\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run" "${BUILD_DIR:?}/test/usbredir-data"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
data=$(cd "${BUILD_DIR:?}/test/usbredir-data" && pwd)
XDG_RUNTIME_DIR=$root/run
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR HULL_CORE_IDLE
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# Debian's usbredirect and the library it needs, unpacked once.
if [ ! -x "$data/x/usr/bin/usbredirect" ]; then
	(cd "$data" && timeout 120 apt-get download usbredirect libusbredirhost1t64 > /dev/null 2>&1)
	for deb in "$data"/*.deb; do
		[ -f "$deb" ] && dpkg-deb -x "$deb" "$data/x"
	done
fi
if [ ! -x "$data/x/usr/bin/usbredirect" ]; then
	printf 'usbredir: not tried: Debian'"'"'s usbredirect could not be had\n'
	exit 0
fi
uid=$(id -u)
gid=$(id -g)
me=$(id -un)

# The outer machine's script, as its root: the gadget, usbredirect, an
# sshd on 127.0.0.1:2222 that knows the user's key; then, as the user, a
# vm nested in it given the device by ssh to that sshd.
cat > "$root/run.sh" <<EOF
modprobe -a dummy_hcd libcomposite usb_f_ss_lb kvm_intel kvm_amd vhost_vsock 2> /dev/null
chmod 666 /dev/kvm /dev/vhost-vsock 2> /dev/null
grep -q ' /sys/kernel/config ' /proc/mounts || mount -t configfs none /sys/kernel/config
g=/sys/kernel/config/usb_gadget/hull
mkdir -p \$g/strings/0x409 \$g/configs/c.1 \$g/functions/SourceSink.0
echo 0x1d6b > \$g/idVendor
echo 0x0104 > \$g/idProduct
ln -s \$g/functions/SourceSink.0 \$g/configs/c.1/
echo "\$(ls /sys/class/udc | head -1)" > \$g/UDC
sleep 2
for d in /sys/bus/usb/devices/*; do
	[ "\$(cat \$d/idVendor 2> /dev/null)" = 1d6b ] && [ "\$(cat \$d/idProduct 2> /dev/null)" = 0104 ] &&
		chmod 666 /dev/bus/usb/\$(printf %03d \$(cat \$d/busnum))/\$(printf %03d \$(cat \$d/devnum)) &&
		echo gadget=up
done
cp -a $data/x/usr/bin/usbredirect /usr/local/bin/
cp -a $data/x/usr/lib/x86_64-linux-gnu/libusbredirhost.so.1* /usr/local/lib/
ldconfig
mkdir -p /run/sshd /tmp/ak /tmp/home/.ssh
chown 0:0 /run/sshd && chmod 755 /run/sshd
ssh-keygen -q -t ed25519 -N '' -f /tmp/hk
cat > /tmp/sshd_config <<CONF
Port 2222
ListenAddress 127.0.0.1
HostKey /tmp/hk
AuthorizedKeysFile /tmp/ak/%u
PidFile /tmp/sshd.pid
UsePAM no
StrictModes no
CONF
/usr/sbin/sshd -f /tmp/sshd_config
ssh-keygen -q -t ed25519 -N '' -f /tmp/home/.ssh/id
cp /tmp/home/.ssh/id.pub /tmp/ak/$me
# Where ssh reads it: the system's config, the guest's own under its
# overlay -- ~/.ssh/config is the passwd entry's home, not HOME, and that
# is the host's.
cat > /etc/ssh/ssh_config.d/hull-far.conf <<CONF
Host far
	HostName 127.0.0.1
	Port 2222
	User $me
	IdentityFile /tmp/home/.ssh/id
	StrictHostKeyChecking no
	UserKnownHostsFile /dev/null
	LogLevel ERROR
CONF
chown -R $uid:$gid /tmp/home /tmp/ak/$me
mkdir -p /tmp/run && chown $uid:$gid /tmp/run && chmod 700 /tmp/run
cat > /usr/local/bin/hull-usbredir-look <<'LOOK'
#!/bin/sh
for i in \$(seq 1 60); do
	for d in /sys/bus/usb/devices/*; do
		# Both ids: 1d6b alone is every root hub's, the Linux Foundation's.
		[ "\$(cat \$d/idVendor 2> /dev/null)" = 1d6b ] &&
			[ "\$(cat \$d/idProduct 2> /dev/null)" = 0104 ] && echo found=redir && exit 0
	done
	sleep 0.5
done
echo found=none
LOOK
chmod 755 /usr/local/bin/hull-usbredir-look
printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "512M") (tuning (vm (profile full))) (devices (usb (device (vendor "1d6b") (product "0104") (host "far")))) (command "/usr/local/bin/hull-usbredir-look"))\n' > /tmp/n.conf
chmod 644 /tmp/n.conf
as() { setpriv --reuid $uid --regid $gid --init-groups env HOME=/tmp/home XDG_RUNTIME_DIR=/tmp/run HULL_CORE_IDLE=1 "\$@"; }
echo "== far"
as timeout 20 ssh far 'id -un; command -v usbredirect; ls -l /dev/bus/usb/*/*' 2>&1 | head -12
echo "== nested"
as timeout -k 2 150 $hull run -f /tmp/n.conf > /tmp/n.out 2>&1 &
sleep 25
echo "== log"
cat /tmp/run/hull/qemu-*/usbredir-0.log 2>&1 | head -10
wait
cat /tmp/n.out
echo "status=done"
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (memory "2G") (cpus 2) (network none) (tuning (vm (profile full))) (command "/bin/sh" "%s/run.sh"))\n' \
	"$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 400 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)

cases=$((cases + 1))
printf '%s\n' "$out" | grep -qx 'gadget=up' ||
	fail "the far host made no USB device" "$out"
cases=$((cases + 1))
printf '%s\n' "$out" | sed -n '/^== nested/,/^status=done/p' | grep -qx 'found=redir' ||
	fail "a vm was not given a USB device on another host by usbredir" "$out"

printf 'usbredir: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
