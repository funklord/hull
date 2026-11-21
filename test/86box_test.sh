#!/bin/sh
# 86box_test.sh -- a period's PC on 86Box (project.md section 5.0): an IBM
# XT of 1986, its own BIOS from the fetched ROM set, booting FreeDOS 1.4's
# 720K floppy, whose FDAUTO.BAT writes a line to COM1, which is 86Box's
# stdio and so the run's output; a disk FreeDOS was installed onto under
# qemu booted on a Pentium board of 1996, twice, its CMOS kept; and a pc
# machine of 86box's planned with what 86Box does not give refused. Not
# part of `make check`: it needs `hull 86box fetch` done, Xvfb, mtools,
# and the network once for FreeDOS; the disk, qemu and xdotool too:
# `make check-86box`.
#
# Its argument is build/hull. FreeDOS is kept in $BUILD_DIR/test/86box-data;
# everything else is under $BUILD_DIR/test/86box, removed at the start.
# Every run is under `timeout`; Xvfb is this suite's, on a free display.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/86box
case $dir in
*/test/86box) ;;
*) printf '86box: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
for tool in Xvfb mcopy curl; do
	if ! command -v "$tool" > /dev/null; then
		printf '86box: not tried: no %s\n' "$tool"
		exit 0
	fi
done
cache=${XDG_CACHE_HOME:-$HOME/.cache}
if [ ! -x "$cache/hull/86box-6.0/squashfs-root/AppRun" ]; then
	printf '86box: not tried: `hull 86box fetch` has not been run\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run" "${BUILD_DIR:?}/test/86box-data"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
data=$(cd "${BUILD_DIR:?}/test/86box-data" && pwd)
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

archive=FD14-FloppyEdition.zip
sum=45b1fa7c52dd996c3bfa5e352ffcd410781b952a6ad629f15a4c9ec4bbaefc5a
if [ ! -f "$data/$archive" ]; then
	if ! curl -fsS -m 300 -o "$data/$archive.part" \
	     "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/$archive"; then
		rm -f "$data/$archive.part"
		printf '86box: %s not fetched, nothing tried\n' "$archive"
		exit 0
	fi
	mv "$data/$archive.part" "$data/$archive"
fi
cases=$((cases + 1))
if ! printf '%s  %s\n' "$sum" "$data/$archive" | sha256sum -c --quiet > /dev/null 2>&1; then
	fail 'FreeDOS is what its publisher published' "$data/$archive"
	exit 1
fi
python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive, open(sys.argv[2], "wb") as out:
	out.write(archive.read("720k/x86BOOT.img"))' "$data/$archive" "$root/xt.img"
printf '@echo off\r\necho hello from 86box > COM1\r\n' > "$root/FDAUTO.BAT"
mcopy -o -i "$root/xt.img" "$root/FDAUTO.BAT" ::FDAUTO.BAT

before=$(ls -d "$cache"/hull/pc-* 2> /dev/null | wc -l)

# A display of the suite's own.
n=150
while [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ]; do
	n=$((n + 1))
done
timeout 200 Xvfb ":$n" -screen 0 1024x768x24 -nolisten tcp > /dev/null 2>&1 &
xvfb=$!
sleep 2

printf '(machine (backend 86box) (source "media:floppy") (mutable direct) (display window) (memory "640K") (tuning (86box (machine "ibmxt86") (video "cga"))) (media (medium (type floppy) (source "image:./xt.img"))))\n' \
	> "$root/xt.conf"
(cd "$root" && DISPLAY=":$n" exec timeout -k 2 150 "$hull" run -f "$root/xt.conf" > "$root/xt.out" 2>&1) &
runner=$!
tries=0
until grep -aq '^hello from 86box' "$root/xt.out" 2> /dev/null || [ "$tries" -ge 560 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
grep -aq '^hello from 86box' "$root/xt.out" ||
	fail "an IBM XT on 86Box did not boot FreeDOS and write to COM1" "$(tail -20 "$root/xt.out")"
id=$("$hull" ps | awk '$5 == "running" { print $1 }')
[ -n "$id" ] && timeout 30 "$hull" stop "$id" > /dev/null 2>&1
wait "$runner" 2> /dev/null
kill "$xvfb" 2> /dev/null
wait "$xvfb" 2> /dev/null

# A disk, and the one-line change of engine: FreeDOS installed onto a
# 32 MB disk by `hull setup freedos` under qemu, then that disk booted on
# 86Box's ASUS P/I-P55TVP4 of 1996, whose Award BIOS finds an IDE disk by
# itself and gives it the geometry hull's 86box.cfg names, 63 sectors and
# 16 heads, as FDISK partitioned it under qemu. A CMOS never written
# stops that BIOS at "Press F1 to continue", as a new board did, and hull
# sends 86Box no key yet, so F1 is pressed here by xdotool in the
# suite's own display. The CMOS is then 86Box's nvr/ in (state DIR), and
# the second boot asks nothing. 86Box does not end the run at FDAPM's
# power-off, so each boot is stopped once it has spoken.
boot_disk() {
	(cd "$root" && DISPLAY=":$n" exec timeout -k 2 120 "$hull" run -f "$root/at.conf" > "$root/$1" 2>&1) &
	runner=$!
	tries=0
	until grep -aq '^hello from the disk' "$root/$1" 2> /dev/null || [ "$tries" -ge 360 ] ||
	      [ ! -d "/proc/$runner" ]; do
		tries=$((tries + 1))
		if [ "$2" = press ] && [ "$tries" -ge 32 ] && [ $((tries % 8)) = 0 ]; then
			[ "$tries" = 32 ] && DISPLAY=":$n" xdotool mousemove 370 250 click 1
			DISPLAY=":$n" xdotool key F1
		fi
		sleep 0.25
	done
	id=$("$hull" ps | awk '$5 == "running" { print $1 }')
	[ -n "$id" ] && timeout 30 "$hull" stop "$id" > /dev/null 2>&1
	wait "$runner" 2> /dev/null
}

if command -v qemu-system-x86_64 > /dev/null && command -v xdotool > /dev/null; then
	python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive, open(sys.argv[2], "wb") as out:
	out.write(archive.read("144m/x86BOOT.img"))' "$data/$archive" "$root/boot.img"
	out=$(cd "$root" && timeout 60 "$hull" setup freedos --size 32 boot.img fd 2>&1)
	if [ "$?" = 0 ]; then
		out=$(cd / && timeout -k 10 600 "$hull" run -f "$root/fd/install.conf" < /dev/null 2>&1)
		got=$?
	else
		got=setup
	fi
	cases=$((cases + 1))
	[ "$got" = 0 ] || fail "FreeDOS did not install onto a disk under qemu: $got" "$(printf '%s\n' "$out" | tail -20)"
	printf '@echo off\r\necho hello from the disk > COM1\r\n' > "$root/AUTOEXEC.BAT"
	mcopy -o -i "$root/fd/disk.img@@32256" "$root/AUTOEXEC.BAT" ::AUTOEXEC.BAT
	mkdir "$root/at"
	printf '(machine (backend 86box) (source "media:disk") (mutable direct) (display window) (memory "8M") (tuning (pc (state "./at")) (86box (machine "p55tvp4") (video "vga"))) (media (medium (type disk) (source "image:./fd/disk.img"))))\n' \
		> "$root/at.conf"
	n=150
	while [ -e "/tmp/.X$n-lock" ] || [ -e "/tmp/.X11-unix/X$n" ]; do
		n=$((n + 1))
	done
	timeout 300 Xvfb ":$n" -screen 0 1024x768x24 -nolisten tcp > /dev/null 2>&1 &
	xvfb=$!
	sleep 2
	boot_disk at1.out press
	cases=$((cases + 1))
	grep -aq '^hello from the disk' "$root/at1.out" ||
		fail "the disk qemu installed did not boot on 86Box's P55TVP4" "$(tail -20 "$root/at1.out")"
	boot_disk at2.out none
	cases=$((cases + 1))
	grep -aq '^hello from the disk' "$root/at2.out" ||
		fail "the CMOS kept in (state DIR) did not boot the disk again unasked" "$(tail -20 "$root/at2.out")"
	kill "$xvfb" 2> /dev/null
	wait "$xvfb" 2> /dev/null
else
	printf '86box: the disk not tried: no qemu-system-x86_64 or xdotool\n'
fi

# What 86Box does not give, refused at plan rather than ignored.
printf '(machine (backend 86box) (source "media:floppy") (display window) (network nat) (tuning (86box (machine "ibmxt86"))) (media (medium (type floppy) (source "image:./xt.img"))))\n' \
	> "$root/nat.conf"
out=$(cd "$root" && timeout 30 "$hull" plan -f "$root/nat.conf" 2>&1)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'network.nat: rejected' ||
	fail "a network on 86box was not refused at plan" "$out"

# Nothing of it left: its run directory in the cache gone with it.
sleep 1
cases=$((cases + 1))
after=$(ls -d "$cache"/hull/pc-* 2> /dev/null | wc -l)
[ "$after" = "$before" ] ||
	fail "the machine's run directory was left in the cache" "$(ls -ld "$cache"/hull/pc-* 2>&1)"

printf '86box: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
