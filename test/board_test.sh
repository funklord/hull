#!/bin/sh
# board_test.sh -- microcontroller boards as pc machines (project.md
# section 5.0): qemu's Arduino Uno and STM32VLDISCOVERY, and an 8051 by
# SDCC's s51 where ucsim_51 is installed, each booted from its firmware,
# the image: source, its UART the machine's serial port. The firmware is
# test/board's: "ready" at boot, then each line typed said back as "said
# LINE", through `hull exec -t ID -- console` on a pty. Not part of `make
# check`, which needs no compiler for a board: `make check-board` runs it.
#
# Its arguments are build/hull, the AVR firmware, the STM32 one and the
# 8051 one, any of the three "" where its compiler is not installed. Each
# machine runs under `timeout`, each console session too, and every wait
# is a bounded count; everything is under $BUILD_DIR/test/board, removed
# at the start.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/board
case $dir in
*/test/board) ;;
*) printf 'board: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir/run"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_DATA_HOME=$root/data
XDG_CACHE_HOME=$root/cache
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR XDG_DATA_HOME XDG_CACHE_HOME HULL_CORE_IDLE
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# board NAME MACHINE FIRMWARE: the board run, a line typed into its
# console, its answer and its boot read back, the machine stopped.
board() {
	name=$1
	machine=$2
	firmware=$3
	cp "$firmware" "$root/$name.fw"
	printf '(machine (source "image:%s.fw") (backend pc) (hostname "%s") (tuning (pc (machine %s))))\n' \
		"$name" "$name" "$machine" > "$root/$name.conf"
	(cd "$root" && exec timeout -k 2 60 "$hull" run -f "$root/$name.conf" > "$root/$name.out" 2>&1) &
	runner=$!
	tries=0
	until grep -q '^ready' "$root/$name.out" 2> /dev/null || [ "$tries" -ge 60 ]; do
		tries=$((tries + 1))
		sleep 0.5
	done
	said=$(timeout 20 python3 -I - "$hull" "$name" <<'PY'
import os, pty, select, subprocess, sys, time
primary, secondary = pty.openpty()
child = subprocess.Popen([sys.argv[1], "exec", "-t", sys.argv[2], "--", "console"],
                         stdin=secondary, stdout=secondary, stderr=secondary)
os.close(secondary)
time.sleep(1)
for byte in b"hello board\r":
	os.write(primary, bytes([byte]))
	time.sleep(0.01)
got = b""
end = time.monotonic() + 5
while b"said hello board" not in got and time.monotonic() < end:
	ready, _, _ = select.select([primary], [], [], 0.2)
	if ready:
		try:
			got += os.read(primary, 4096)
		except OSError:
			break
child.kill()
child.wait()
sys.stdout.write(got.decode(errors="replace"))
PY
)
	timeout 30 "$hull" stop "$name" > /dev/null 2>&1
	wait "$runner"
	cases=$((cases + 1))
	grep -q '^ready' "$root/$name.out" ||
		fail "the $machine board did not boot its firmware" "$(cat "$root/$name.out")"
	cases=$((cases + 1))
	printf '%s\n' "$said" | grep -q 'said hello board' ||
		fail "the $machine board's console did not answer a line typed" "$said
$(cat "$root/$name.out")"
}

if [ -n "${2:-}" ] && command -v qemu-system-avr > /dev/null; then
	board uno arduino-uno "$2"
else
	printf 'board: the Arduino Uno not tried: no avr-gcc or qemu-system-avr\n'
fi
if [ -n "${3:-}" ] && command -v qemu-system-arm > /dev/null; then
	board stm stm32vldiscovery "$3"
else
	printf 'board: the STM32VLDISCOVERY not tried: no clang and lld or qemu-system-arm\n'
fi
if [ -n "${4:-}" ] && command -v ucsim_51 > /dev/null; then
	board at89 at89c51ed2 "$4"
else
	printf 'board: the AT89C51ED2 not tried: no sdcc or ucsim_51\n'
fi

# What a board does not have, refused at its start: a network and drives.
printf '(machine (source "image:none.fw") (backend pc) (network nat) (tuning (pc (machine arduino-uno))))\n' \
	> "$root/net.conf"
: > "$root/none.fw"
cases=$((cases + 1))
out=$(cd "$root" && timeout 30 "$hull" run -f "$root/net.conf" 2>&1)
printf '%s\n' "$out" | grep -q "serial port is its one way in" ||
	fail "a board with a network was not refused" "$out"

printf 'board: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
