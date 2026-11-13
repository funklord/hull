#!/bin/sh
# board_test.sh -- microcontroller boards as pc machines (project.md
# section 5.0): qemu's Arduino Uno and STM32VLDISCOVERY, an 8051 by
# SDCC's s51 where ucsim_51 is installed, and Espressif's chips on
# Espressif's qemu, each booted from its firmware,
# the image: source, its UART the machine's serial port. The firmware is
# test/board's: "ready" at boot, then each line typed said back as "said
# LINE", through `hull exec -t ID -- console` on a pty. Not part of `make
# check`, which needs no compiler for a board: `make check-board` runs it.
#
# Its arguments are build/hull, the AVR firmware, the STM32 one and the
# 8051 one, any of the three "" where its compiler is not installed, then
# any number of ESP32 flash images, each named for its chip. Each
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

# board NAME MACHINE FIRMWARE [SPEC]: the board run, SPEC's forms added, a line typed into its
# console, its answer and its boot read back, the machine stopped.
board() {
	name=$1
	machine=$2
	firmware=$3
	cp "$firmware" "$root/$name.fw"
	printf '(machine (source "image:%s.fw") (backend pc) (hostname "%s") %s(tuning (pc (machine %s))))\n' \
		"$name" "$name" "${4:-}" "$machine" > "$root/$name.conf"
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

# on_nat NAME MACHINE FIRMWARE: a board with Ethernet on slirp's NAT,
# the gateway answering the ARP its firmware asks.
on_nat() {
	board "$1" "$2" "$3" '(network nat) '
	cases=$((cases + 1))
	grep -q '^peer answered' "$root/$1.out" ||
		fail "the $2 board on (network nat) heard no gateway" "$(cat "$root/$1.out")"
}

# on_wire MACHINE-A MACHINE-B FIRMWARE-A FIRMWARE-B: two boards on one
# segment, each asking the other's address and answering for its own.
on_wire() {
	cp "$3" "$root/wa.fw"
	cp "$4" "$root/wb.fw"
	printf '(machines\n  (wa (backend pc) (source "image:./wa.fw") (segments "%s") (tuning (pc (machine %s))))\n  (wb (backend pc) (source "image:./wb.fw") (segments "%s") (tuning (pc (machine %s)))))\n' \
		"wire-$1" "$1" "wire-$1" "$2" > "$root/wire.conf"
	(cd "$root" && exec timeout -k 2 60 "$hull" run -f "$root/wire.conf" > "$root/wire.out" 2>&1) &
	runner=$!
	tries=0
	until [ "$(grep -c 'peer answered' "$root/wire.out" 2> /dev/null)" = 2 ] ||
	      [ "$tries" -ge 60 ]; do
		tries=$((tries + 1))
		sleep 0.5
	done
	timeout 30 "$hull" stop wa > /dev/null 2>&1
	timeout 30 "$hull" stop wb > /dev/null 2>&1
	wait "$runner"
	cases=$((cases + 1))
	[ "$(grep -c 'peer answered' "$root/wire.out")" = 2 ] ||
		fail "a $1 and a $2 on a segment did not each hear the other" "$(cat "$root/wire.out")"
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
# The LM3S6965's own MAC, qemu's stellaris_enet: firmware of the same
# protocol as the ESP32-C3's, test/board/lm3s_net.c, in LM3S_NET.
if [ -n "${LM3S_NET:-}" ] && command -v qemu-system-arm > /dev/null; then
	on_nat natlm lm3s6965evb "$LM3S_NET/lm3s-net-nat.elf"
	on_wire lm3s6965evb lm3s6965evb "$LM3S_NET/lm3s-net-wire-a.elf" "$LM3S_NET/lm3s-net-wire-b.elf"
else
	printf 'board: the LM3S6965 not tried on a network: no clang and lld or qemu-system-arm\n'
fi
if [ -n "${4:-}" ] && command -v ucsim_51 > /dev/null; then
	board at89 at89c51ed2 "$4"
else
	printf 'board: the AT89C51ED2 not tried: no sdcc or ucsim_51\n'
fi

# Espressif's chips, on Espressif's qemu, each image NAME.flash for the chip
# NAME: refused naming the fetch while it is not in this run's cache, then
# the user's fetched copy linked in -- fetched by `hull esp fetch` under
# the user's own cache where it is not there yet -- and each booted; the
# ESP32's image on a Heltec WiFi LoRa 32 too, a board by its name; an
# application's .bin alone refused. Then its Ethernet: an ESP32-C3 on
# (network nat), two on one segment, and one on both refused. The
# LM3S6965's Ethernet the same way, by its own MAC.
shift 4 2> /dev/null || set --
if [ $# -gt 0 ]; then
	first=$1
	name=${first##*/}
	name=${name%.flash}
	cp "$first" "$root/fetch.fw"
	printf '(machine (source "image:fetch.fw") (backend pc) (tuning (pc (machine %s))))\n' "$name" \
		> "$root/fetch.conf"
	cases=$((cases + 1))
	out=$(cd "$root" && timeout 30 "$hull" run -f "$root/fetch.conf" 2>&1)
	printf '%s\n' "$out" | grep -q "hull esp fetch" ||
		fail "an ESP32 with nothing fetched was not refused naming the fetch" "$out"
	if esp=$(XDG_CACHE_HOME=${HULL_TEST_CACHE:-$HOME/.cache} timeout 600 "$hull" esp fetch); then
		mkdir -p "$root/cache/hull"
		ln -s "$esp" "$root/cache/hull/${esp##*/}"
		nat=
		wire_a=
		wire_b=
		for image in "$@"; do
			name=${image##*/}
			name=${name%.flash}
			case $name in
			*-net-nat) nat=$image ;;
			*-net-wire-a) wire_a=$image ;;
			*-net-wire-b) wire_b=$image ;;
			*) board "$name" "$name" "$image" ;;
			esac
		done
		for image in "$@"; do
			case $image in
			*/esp32.flash) board heltec heltec-wifi-lora-32 "$image" ;;
			esac
		done
		[ -n "$nat" ] && on_nat natc3 esp32c3 "$nat"
		[ -n "$wire_a" ] && [ -n "$wire_b" ] &&
			on_wire esp32c3 xiao-esp32c3 "$wire_a" "$wire_b"
		printf '(machine (source "image:fetch.fw") (backend pc) (network nat) (segments "espwire") (tuning (pc (machine esp32c3))))\n' \
			> "$root/two.conf"
		cases=$((cases + 1))
		out=$(cd "$root" && timeout 30 "$hull" run -f "$root/two.conf" 2>&1)
		printf '%s\n' "$out" | grep -q "one Ethernet" ||
			fail "an ESP32 on both a NAT and a segment was not refused" "$out"
		head -c 5000 /dev/zero > "$root/app.fw"
		printf '(machine (source "image:app.fw") (backend pc) (tuning (pc (machine esp32c3))))\n' \
			> "$root/app.conf"
		cases=$((cases + 1))
		out=$(cd "$root" && timeout 30 "$hull" run -f "$root/app.conf" 2>&1)
		printf '%s\n' "$out" | grep -q "not a whole flash image" ||
			fail "an ESP32-C3 given no flash image was not refused" "$out"
	else
		cases=$((cases + 1))
		fail "hull esp fetch failed" "$esp"
	fi
else
	printf 'board: no ESP32 tried: no clang, lld, llvm-mc and llvm-objcopy\n'
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
