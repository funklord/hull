#!/bin/sh
# pc_test.sh -- the pc class (project.md section 5.0): a whole PC whose
# system is its own, booted by its firmware from its media. Not part of
# `make check`, which needs no qemu: `make check-pc` runs it. Its argument
# is build/hull. The system booted is test/pc_boot.asm, a sector that says
# on COM1 where it came from, and for the last cases a Linux of
# `hull export --bootable`. It writes only under $BUILD_DIR/test/pc,
# removed at its start and left on failure, what it starts is under
# `timeout`, and the core is the test's own, whose idle clock is a second.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/pc
case $dir in
*/test/pc) ;;
*) printf 'pc: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir/run" "$dir/cache"
chmod 700 "$dir/run"
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_CACHE_HOME=$root/cache
XDG_DATA_HOME=$root/data
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR XDG_CACHE_HOME XDG_DATA_HOME HULL_CORE_IDLE
unset DISPLAY
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# says NAME OUT WORD: OUT holds WORD.
says() {
	cases=$((cases + 1))
	printf '%s\n' "$2" | grep -qF "$3" || fail "$1" "$2"
}

# status NAME GOT WANTED OUT
status() {
	cases=$((cases + 1))
	[ "$2" = "$3" ] || fail "$1: exited $2, wanted $3" "$4"
}

# conf ENTRIES...: the machine of $root/hull.conf.
conf() {
	printf '(machine (backend pc) %s)\n' "$*" > "$root/hull.conf"
}

run() {
	out=$(timeout 120 "$hull" run -f "$root/hull.conf" "$@" 2>&1)
	got=$?
}

for tool in nasm qemu-system-x86_64 qemu-img; do
	if ! command -v "$tool" > /dev/null; then
		printf 'pc: %s is not installed\n' "$tool"
		exit 1
	fi
done
nasm -f bin -o "$root/boot.bin" "$here/pc_boot.asm" || exit 1

# image SIZE PATH: the sector and its next on a new image.
image() {
	truncate -s "$1" "$2"
	dd if="$root/boot.bin" of="$2" conv=notrunc status=none
}

# --- what the plan refuses ---------------------------------------------------

image 1M "$root/disk.img"
conf '(source "media:disk") (media (medium (type disk) (source "image:./disk.img")))'
out=$("$hull" plan -f "$root/hull.conf" --backend vm 2>&1)
got=$?
[ "$got" != 0 ] || fail 'a media: source is refused on vm' "$out"
cases=$((cases + 1))
says 'a media: source named as what vm lacks' "$out" 'source.media'

conf '(source "media:cdrom") (media (medium (type disk) (source "image:./disk.img")))'
run
status 'a drive with no medium is refused' "$got" 125 "$out"
says 'and said' "$out" 'no medium'

conf '(source "media:disk") (media (medium (type disk) (source "image:./gone.img")))'
run
status 'an image not there is refused' "$got" 125 "$out"

conf '(source "media:disk") (ports (port (host 7000) (guest 7000)))' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'ports with no network are refused' "$got" 125 "$out"
says 'and said' "$out" '(network nat)'

conf '(source "media:disk") (display window)' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'a window with no DISPLAY is refused' "$got" 125 "$out"

conf '(source "media:disk") (command "/bin/sh")' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
status 'a command is refused, no agent running it' "$got" 125 "$out"

# --- a disk: its writes kept or not ------------------------------------------

before=$(sha256sum < "$root/disk.img")
conf '(source "media:disk") (media (medium (type disk) (source "image:./disk.img")))'
run
status 'a disk boots' "$got" 0 "$out"
says 'and says so on its serial port' "$out" 'booted from a drive'
says 'its word fresh' "$out" 'word: fresh'
run
says 'an overlay forgets the write' "$out" 'word: fresh'
cases=$((cases + 1))
[ "$(sha256sum < "$root/disk.img")" = "$before" ] || fail 'an overlay left the image alone' ''

conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "image:./disk.img")))'
run
says 'a direct disk boots fresh' "$out" 'word: fresh'
run
says 'and keeps the write' "$out" 'word: written'

# --- an attached image, as it came -------------------------------------------

image 1M "$root/raw.img"
qemu-img convert -f raw -O vmdk "$root/raw.img" "$root/old.vmdk"
before=$(sha256sum < "$root/old.vmdk")
conf '(source "image:./old.vmdk")'
run
status 'a vmdk attached boots' "$got" 0 "$out"
says 'its word read through the container' "$out" 'word: fresh'
cases=$((cases + 1))
[ "$(sha256sum < "$root/old.vmdk")" = "$before" ] || fail 'an attached overlay left it alone' ''

# --- a floppy ----------------------------------------------------------------

image 1440K "$root/floppy.img"
conf '(source "media:floppy") (media (medium (type floppy) (source "image:./floppy.img")))'
run
status 'a floppy boots' "$got" 0 "$out"
says 'and reads its next sector' "$out" 'word: fresh'

# --- a collection: plain files, and the disk they are ------------------------

image 1M "$root/c.img"
"$hull" media import "$root/c.img" "$root/c" > /dev/null 2>&1
conf '(source "media:disk") (media (medium (type disk) (source "dir:./c")))'
run
status 'a collection boots' "$got" 0 "$out"
says 'as the disk it was' "$out" 'word: fresh'
run
says 'its writes gone with the machine' "$out" 'word: fresh'
cases=$((cases + 1))
"$hull" media verify "$root/c" > /dev/null 2>&1 || fail 'the collection is as it was' ''

conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "dir:./c")))'
run
status 'a collection is not written' "$got" 125 "$out"
says 'and said' "$out" 'collection'

first=$(find "$root/c/quirks" -type f | head -1)
cp "$first" "$root/quirk.saved"
printf 'x' | dd of="$first" bs=1 seek=1 conv=notrunc status=none
conf '(source "media:disk") (media (medium (type disk) (source "dir:./c")))'
run
status 'a collection changed by a byte is refused' "$got" 125 "$out"
cp "$root/quirk.saved" "$first"

# --- a disc installs, and the disk boots, by one line ------------------------

python3 -I "$here/pc_disc.py" "$root/setup.iso" "$root/boot.bin"
truncate -s 1M "$root/install.img"
conf '(source "media:cdrom") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
status 'a disc boots' "$got" 0 "$out"
says 'and installs' "$out" 'installing from a disc'
conf '(source "media:disk") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
status 'the disk it installed boots' "$got" 0 "$out"
says 'its word there' "$out" 'word: fresh'

# Both drives boot now, so it is the source that chooses, the firmware
# otherwise falling through to whichever one will.
conf '(source "media:cdrom")' \
	'(media (medium (type disk) (source "image:./install.img"))' \
	'(medium (type cdrom) (source "image:./setup.iso")))'
run
says 'with both bootable the source chooses the disc' "$out" 'installing from a disc'

"$hull" media import "$root/setup.iso" "$root/setup" > /dev/null 2>&1
truncate -s 1M "$root/install2.img"
conf '(source "media:cdrom") (mutable direct)' \
	'(media (medium (type disk) (source "image:./install2.img"))' \
	'(medium (type cdrom) (source "dir:./setup")))'
run
says 'a disc from its collection installs too' "$out" 'installing from a disc'

# --- the other PCs -----------------------------------------------------------

conf '(source "media:disk") (tuning (pc (accel tcg) (machine q35) (vga cirrus)))' \
	'(media (medium (type disk) (source "image:./raw.img")))'
run
status 'under emulation, on a q35' "$got" 0 "$out"
says 'it boots the same' "$out" 'word: fresh'

# --- a stop, a killed run, and nothing left ----------------------------------

printf '\353\376' > "$root/spin.bin"
truncate -s 1M "$root/spin.img"
dd if="$root/spin.bin" of="$root/spin.img" conv=notrunc status=none
printf '\125\252' | dd of="$root/spin.img" bs=1 seek=510 conv=notrunc status=none
conf '(source "media:disk") (media (medium (type disk) (source "image:./spin.img")))'
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1 &
pid=$!
sleep 2
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
out=$(timeout 30 "$hull" stop "$id" 2>&1)
status 'a stop' "$?" 0 "$out"
wait "$pid"
cases=$((cases + 1))
sleep 1
out=$(ps -eo pid=,args= | grep -F "$root/" | grep '[q]emu-system')
[ -z "$out" ] || fail 'a stop left no qemu' "$out"

# A core killed with a collection's image open: qemu goes with it, and
# its unnamed image and snapshot with qemu.
conf '(source "media:disk") (media (medium (type disk) (source "dir:./spinc")))'
"$hull" media import "$root/spin.img" "$root/spinc" > /dev/null 2>&1
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/spin.out" 2>&1 &
pid=$!
sleep 2
core=$(ps -eo pid=,args= | awk '/[h]ull core/ { print $1 }' | while read -r p; do
	[ "$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | sed -n 's/^XDG_RUNTIME_DIR=//p')" = "$root/run" ] && echo "$p"
done | head -1)
qemu=$(ps -o pid= --ppid "${core:-1}" | while read -r p; do
	[ "$(cat "/proc/$p/comm" 2>/dev/null)" = qemu-system-x86 ] && echo "$p"
done | head -1)
cases=$((cases + 1))
if [ -z "$core" ] || [ -z "$qemu" ]; then
	fail 'the core and its qemu are found' "core ${core:-none}, qemu ${qemu:-none}"
else
	kill -9 "$core"
fi
wait "$pid"
sleep 2
cases=$((cases + 1))
[ -n "$qemu" ] && [ ! -d "/proc/$qemu" ] || fail 'a killed core left no qemu' "qemu ${qemu:-none}"
cases=$((cases + 1))
out=$(find "$root/cache" -type f)
[ -z "$out" ] || fail 'nothing of the machines is left in the cache' "$out"

# --- its window --------------------------------------------------------------

if command -v Xvfb > /dev/null && command -v xwininfo > /dev/null; then
	number=$((90 + $$ % 300))
	Xvfb ":$number" -screen 0 800x600x24 -nolisten tcp > /dev/null 2>&1 &
	x=$!
	sleep 1
	conf '(source "media:disk") (display window)' \
		'(media (medium (type disk) (source "image:./spin.img")))'
	DISPLAY=":$number" timeout 30 "$hull" run -f "$root/hull.conf" > "$root/x.out" 2>&1 &
	pid=$!
	sleep 3
	out=$(DISPLAY=":$number" xwininfo -root -tree 2>&1)
	says 'a window on the display' "$out" 'QEMU'
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	timeout 30 "$hull" stop "$id" > /dev/null 2>&1
	wait "$pid"
	kill "$x"
else
	printf 'pc: no Xvfb, the window not tried\n'
fi

# --- its screen by VNC, at its address, with no X -----------------------------

conf '(source "media:disk") (display vnc)' \
	'(media (medium (type disk) (source "image:./spin.img")))'
timeout 30 "$hull" run -f "$root/hull.conf" > "$root/vnc.out" 2>&1 &
pid=$!
sleep 2
address=$("$hull" ps | awk '$5 == "running" { print $2 }')
out=$(timeout 5 nc -w 2 "$address" 5900 < /dev/null | head -c 12)
says 'the screen answers at the machine'"'"'s address' "$out" 'RFB 003'
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
timeout 30 "$hull" stop "$id" > /dev/null 2>&1
wait "$pid"

# --- a real system, its port at the machine's address ------------------------

busybox=$(command -v busybox)
module=$(ls "/lib/modules/$(uname -r)/kernel/drivers/net/ethernet/intel/e1000/"e1000.ko* 2>/dev/null)
if [ -n "$busybox" ] && [ -n "$module" ]; then
	tree=$root/tree
	mkdir -p "$tree/bin" "$tree/proc" "$tree/sys" "$tree/dev"
	cp "$busybox" "$tree/bin/busybox"
	for lib in $(ldd "$busybox" | grep -oE '/[^ ]+'); do
		mkdir -p "$tree$(dirname "$lib")"
		cp -L "$lib" "$tree$lib"
	done
	case $module in
	*.xz) xz -dc "$module" > "$tree/e1000.ko" ;;
	*) cp "$module" "$tree/e1000.ko" ;;
	esac
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
echo "a linux on a pc"
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
echo listening
nc -l -p 7000
echo served
poweroff -f
INIT
	chmod +x "$tree/init.sh"
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/linux.img" --init /init.sh 2>&1)
	status 'a bootable export' "$?" 0 "$out"
	# UEFI, which a bootable export always boots by: its BIOS stage is
	# there only where Limine was built.
	conf '(source "image:./linux.img") (memory "512M") (network nat)' \
		'(ports (port (host 7000) (guest 7000))) (tuning (pc (nic e1000) (firmware uefi)))'
	timeout 90 "$hull" run -f "$root/hull.conf" > "$root/linux.out" 2>&1 &
	pid=$!
	sleep 2
	address=$("$hull" ps | awk '$5 == "running" { print $2 }')
	n=0
	while [ "$n" -lt 40 ] && ! grep -qa listening "$root/linux.out"; do
		sleep 1
		n=$((n + 1))
	done
	sleep 1
	printf 'from the host\n' | timeout 10 nc -w 3 "$address" 7000
	wait "$pid"
	status 'a linux powers off' "$?" 0 "$(tail -5 "$root/linux.out")"
	says 'it booted' "$(cat "$root/linux.out")" 'a linux on a pc'
	says 'a client at its address reached it' "$(cat "$root/linux.out")" 'from the host'

	# The serial bridge: a shell on COM1, driven from the host through
	# `hull exec ... console`, with nothing of hull's in the guest.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox mount -t devtmpfs none /dev
/bin/busybox --install -s /bin
echo console-ready
exec sh < /dev/ttyS0 > /dev/ttyS0 2>&1
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/shell.img" --init /init.sh 2>&1)
	status 'a system with a shell on its serial port' "$?" 0 "$out"
	conf '(source "image:./shell.img") (memory "512M") (tuning (pc (firmware uefi)))'
	timeout 90 "$hull" run -f "$root/hull.conf" > "$root/shell.out" 2>&1 &
	pid=$!
	n=0
	while [ "$n" -lt 60 ] && ! grep -qa console-ready "$root/shell.out"; do
		sleep 1
		n=$((n + 1))
	done
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	out=$(timeout 10 "$hull" exec "$id" -- ls 2>&1)
	status 'a command is refused' "$?" 125 "$out"
	says 'and the console named' "$out" 'console'
	# Its input ending ends nothing; the machine's end ends it.
	out=$( { sleep 1; printf 'echo marker-$((40 + 2))\n'; sleep 2; printf 'poweroff -f\n'; sleep 20; } |
		timeout 40 "$hull" exec "$id" -- console 2>&1)
	status 'a console ends with its machine' "$?" 0 "$out"
	says 'what was typed ran in the guest' "$out" 'marker-42'
	wait "$pid"
	status 'and that was the guest powering off' "$?" 0 "$(tail -3 "$root/shell.out")"

	# Two of them on a wire of hull's: a legacy machine is a host of the
	# networks hull simulates. Each sets its own address, as it must.
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.77.0.1/24 dev eth0
nc -l -p 7001
echo served on the wire
poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/one.img" --init /init.sh 2>&1)
	status 'a first end of a wire' "$?" 0 "$out"
	cat > "$tree/init.sh" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc
/bin/busybox --install -s /bin
insmod /e1000.ko
ip link set eth0 up
ip addr add 10.77.0.2/24 dev eth0
n=0
until ping -c 1 -W 1 10.77.0.1 > /dev/null 2>&1 || [ "$n" = 30 ]; do n=$((n + 1)); done
sleep 2
echo over the wire | nc -w 2 10.77.0.1 7001
poweroff -f
INIT
	out=$(timeout 120 "$hull" export --bootable "$tree" "$root/two.img" --init /init.sh 2>&1)
	status 'a second' "$?" 0 "$out"
	for end in one two; do
		cat <<CONF
  ($end (backend pc) (source "image:./$end.img") (memory "512M") (segments "pcwire")
    (tuning (pc (nic e1000) (firmware uefi))))
CONF
	done > "$root/ends"
	{ printf '(machines\n'; cat "$root/ends"; printf ')\n'; } > "$root/wire.conf"
	out=$(timeout 150 "$hull" run -f "$root/wire.conf" 2>&1)
	got=$?
	status 'two machines on a wire end' "$got" 0 "$(printf '%s\n' "$out" | tail -5)"
	says 'one heard the other' "$out" 'over the wire'
else
	printf 'pc: no busybox or e1000 module, the linux not tried\n'
fi

printf 'pc: %s cases, %s failed\n' "$cases" "$failures"
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
