#!/bin/sh
# 86box_test.sh -- a period's PC on 86Box (project.md section 5.0): an IBM
# XT of 1986, its own BIOS from the fetched ROM set, booting FreeDOS 1.4's
# 720K floppy, whose FDAUTO.BAT writes a line to COM1, which is 86Box's
# stdio and so the run's output; and a pc machine of 86box's planned with
# what 86Box does not give refused. Not part of `make check`: it needs
# `hull 86box fetch` done, Xvfb, mtools, and the network once for
# FreeDOS: `make check-86box`.
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
