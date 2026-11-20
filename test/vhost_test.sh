#!/bin/sh
# vhost_test.sh -- a GPIO chip and an I2C bus of the host given to a vm
# (project.md section 4.13): (devices (gpio "/dev/gpiochipN") (i2c (bus
# (path "/dev/i2c-N") (clients "0x50")))), the qemu engine running
# rust-vmm's vhost-device-gpio and vhost-device-i2c on them. The host is a
# vm of hull's own, the outer machine, whose root makes an 8-line chip of
# gpio-sim and an I2C bus of i2c-stub with a client at 0x50 and gives their
# nodes to the user; as the user, a vm nested in it is given both and finds
# its virtio GPIO and I2C drivers bound, a gpiochip of its own, and reads a
# byte from 0x50 through the bus. Not part of `make check`: it needs KVM
# nested and `make vhost-device`: `make check-vhost`.
#
# Its argument is build/hull. Everything is under $BUILD_DIR/test/vhost,
# removed at the start. Every run is under `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/vhost
case $dir in
*/test/vhost) ;;
*) printf 'vhost: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ]; then
	printf 'vhost: not tried: no /dev/kvm open to this user\n'
	exit 0
fi
cache=${XDG_CACHE_HOME:-$HOME/.cache}
if [ ! -x "$cache/hull/vhost-device-21f1333/bin/vhost-device-gpio" ] ||
   [ ! -x "$cache/hull/vhost-device-21f1333/bin/vhost-device-i2c" ]; then
	printf 'vhost: not tried: `make vhost-device` has not been run\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
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

# hull's kernel where one is built beside hull: it has gpio-sim and
# virtio's GPIO and I2C drivers, which Debian's does not.
kernel=
[ -f "$(dirname "$hull")/vmlinuz-hull" ] && kernel='(kernel hull)'
uid=$(id -u)
gid=$(id -g)

# The outer machine's script, as its root: the chip and the bus, their
# nodes the user's; then, as the user, the nested vm given both.
cat > "$root/run.sh" <<EOF
for m in kvm_intel kvm_amd vhost_vsock gpio-sim i2c-dev; do modprobe \$m 2> /dev/null; done
modprobe i2c-stub chip_addr=0x50 2> /dev/null
chmod 666 /dev/kvm /dev/vhost-vsock 2> /dev/null
grep -q ' /sys/kernel/config ' /proc/mounts || mount -t configfs none /sys/kernel/config
s=/sys/kernel/config/gpio-sim/hull
mkdir -p \$s/bank0
echo 8 > \$s/bank0/num_lines
echo 1 > \$s/live
chip=\$(cat \$s/bank0/chip_name 2> /dev/null)
bus=
for a in /sys/bus/i2c/devices/i2c-*; do
	grep -q 'SMBus stub' \$a/name 2> /dev/null && bus=\${a##*/}
done
echo "chip=\$chip bus=\$bus"
for n in \$chip \$bus; do chmod 666 /dev/\$n; done
cat > /usr/local/bin/hull-vhost-look <<'LOOK'
#!/bin/sh
sleep 2
# The vhost-user functions on the guest's PCI bus, virtio's GPIO (0x1069)
# and I2C (0x1062): qemu got them from the daemons' sockets.
for f in /sys/bus/pci/devices/*; do
	[ "\$(cat \$f/vendor)" = 0x1af4 ] || continue
	case \$(cat \$f/device) in 0x1069) echo gpio=pci ;; 0x1062) echo i2c=pci ;; esac
done
ls /sys/bus/virtio/drivers/gpio_virtio /sys/bus/virtio/drivers/gpio-virtio 2> /dev/null | grep -q virtio && echo gpio=bound
ls /sys/bus/virtio/drivers/i2c_virtio 2> /dev/null | grep -q virtio && echo i2c=bound
ls /dev/gpiochip* > /dev/null 2>&1 && echo gpiochip=here
for a in /sys/bus/i2c/devices/i2c-*; do
	[ -e "\$a/name" ] || continue
	n=\${a##*i2c-}
	python3 -c '
import fcntl, os, sys
fd = os.open("/dev/i2c-" + sys.argv[1], os.O_RDWR)
fcntl.ioctl(fd, 0x0703, 0x50)
print("read=%d" % len(os.read(fd, 1)))' "\$n" 2>&1
done
LOOK
chmod 755 /usr/local/bin/hull-vhost-look
mkdir -p /tmp/run && chown $uid:$gid /tmp/run && chmod 700 /tmp/run
gpio=
[ -n "\$chip" ] && gpio="(gpio \\"/dev/\$chip\\") "
printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "512M") (tuning (vm (profile full) $kernel)) (devices %s(i2c (bus (path "/dev/%s") (clients "0x50")))) (command "/usr/local/bin/hull-vhost-look"))\n' "\$gpio" "\$bus" > /tmp/n.conf
chmod 644 /tmp/n.conf
# The daemons where the user can reach them: the host's ~/.cache is
# root's in here, and 700.
mkdir -p /tmp/cache/hull
cp -a $cache/hull/vhost-device-21f1333 /tmp/cache/hull/
chown -R $uid:$gid /tmp/cache
echo "== nested"
setpriv --reuid $uid --regid $gid --init-groups env HOME=/tmp XDG_RUNTIME_DIR=/tmp/run XDG_CACHE_HOME=/tmp/cache LD_LIBRARY_PATH=/tmp/cache/hull/vhost-device-21f1333/gpiod/usr/lib/x86_64-linux-gnu HULL_CORE_IDLE=1 timeout -k 2 150 $hull run -f /tmp/n.conf 2>&1
echo "status=\$?"
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (memory "2G") (cpus 2) (network none) (tuning (vm (profile full) %s)) (command "/bin/sh" "%s/run.sh"))\n' \
	"$kernel" "$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 400 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)
nested=$(printf '%s\n' "$out" | sed -n '/^== nested/,/^status=/p')

# The I2C bus: its daemon served, qemu gave the guest the function, and
# where the guest's kernel drives virtio I2C, a byte read through it.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq '^chip=[^ ]* bus=i2c-[0-9]+$' ||
	fail "the host made no I2C bus" "$out"
cases=$((cases + 1))
printf '%s\n' "$nested" | grep -qx 'i2c=pci' ||
	fail "a vm was not given the host's I2C bus by vhost-device-i2c" "$out"
if printf '%s\n' "$nested" | grep -qx 'i2c=bound'; then
	cases=$((cases + 1))
	printf '%s\n' "$nested" | grep -qx 'read=1' ||
		fail "a vm could not read the host's I2C client through its bus" "$out"
else
	printf 'vhost: the guest'"'"'s kernel has no virtio I2C driver: the read through the bus not tried\n'
fi
# The GPIO chip, where the host's kernel has gpio-sim to make one.
if printf '%s\n' "$out" | grep -Eq '^chip=gpiochip[0-9]+ '; then
	cases=$((cases + 1))
	printf '%s\n' "$nested" | grep -qx 'gpio=pci' ||
		fail "a vm was not given the host's GPIO chip by vhost-device-gpio" "$out"
	if printf '%s\n' "$nested" | grep -qx 'gpio=bound'; then
		cases=$((cases + 1))
		printf '%s\n' "$nested" | grep -qx 'gpiochip=here' ||
			fail "a vm's virtio GPIO made no gpiochip" "$out"
	fi
else
	printf 'vhost: the host'"'"'s kernel has no gpio-sim: the GPIO chip not tried\n'
fi

printf 'vhost: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
