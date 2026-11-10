#!/bin/sh
# gui_test.sh -- hull-gui against a core of its own: the window drawn by
# qtty as text (--tui --snapshot) with no core and with machines, and
# core_link, the GUI's side of the protocol, driven through each request
# against a running machine.
#
# Arguments: hull, hull-gui, core_link_test, the static probe,
# machine_list_test. Everything
# is under $BUILD_DIR/test/gui: the machines' rootfs, the core's runtime
# directory, and the configuration directory the GUI saves its list in,
# so that the account's own list is never read or written. Every command
# runs under `timeout`; the one run in the background is two 150-second
# probes, stopped by the test and killed at the exit by its pid.

set -u

hull=$1
gui=$2
link_test=$3
probe=$4
list_test=$5
dir=$(mkdir -p "${BUILD_DIR:?}/test/gui" && cd "${BUILD_DIR:?}/test/gui" && pwd)
case $dir in
*/test/gui) ;;
*) echo "gui_test: $dir is not a test/gui directory" >&2; exit 2 ;;
esac

XDG_RUNTIME_DIR=$dir/run
XDG_CONFIG_HOME=$dir/config
XDG_DATA_HOME=$dir/data
export XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME
export HULL_CORE_IDLE=1
DOCKER_HOST=unix://$XDG_RUNTIME_DIR/no-docker.sock
export DOCKER_HOST
rm -rf "${dir:?}/config" "${dir:?}/rootfs" "${dir:?}/data" "${dir:?}/newm"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$dir/rootfs"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$probe" "$dir/rootfs/probe"
printf '%s\n' '(machine (hostname "gui-a") (source "dir:./rootfs") (command "/probe" "ticks" "600"))' \
	> "$dir/a.conf"
printf '%s\n' '(machine (hostname "gui-b") (source "dir:./rootfs") (command "/probe" "say" "gui-hello" "ticks" "600"))' \
	> "$dir/b.conf"

# Refused on a vm unless --accept: one machine, though plan names it on
# the refused field's line as well as on its own.
printf '%s\n' '(machines' \
	'  (gui-da (source "dir:./rootfs") (segments "gui-wire") (command "/probe" "ticks" "1500"))' \
	'  (gui-db (source "dir:./rootfs") (segments "gui-wire") (command "/probe" "ticks" "1500")))' \
	> "$dir/d.conf"
printf '%s\n' '(machine (hostname "gui-c") (backend vm) (source "dir:./rootfs") (fidelity inotify))' \
	> "$dir/c.conf"

cases=0
failures=0
run_pid=

finish() {
	if [ -n "$run_pid" ] && [ -d "/proc/$run_pid" ]; then
		kill "$run_pid" 2>/dev/null
		wait "$run_pid" 2>/dev/null
	fi
}
trap finish EXIT

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# has TEXT PATTERN WHAT: some line of TEXT matches PATTERN (grep -E).
has() {
	cases=$((cases + 1))
	if ! printf '%s\n' "$1" | grep -Eq -- "$2"; then
		fail "$3: no line matches /$2/" "$1"
	fi
}

here=$(cd "$(dirname "$0")" && pwd)

# keys WORKDIR KEY... [-- FILE...]: the window on a terminal of its own,
# sent the keys, its screen's text in $out.
keys() {
	work=$1
	shift
	timeout 90 python3 -I "$here/gui_keys.py" "$gui" "$hull" "$work" "$dir/screen.txt" "$@" \
		> /dev/null 2>&1
	out=$(tr -s ' ' < "$dir/screen.txt")
}

snapshot() {
	(cd "$dir" && timeout 60 "$gui" --hull "$hull" --tui --snapshot 110x26 "$@" 2>/dev/null)
}

# A core of an earlier run gone first, as it goes a second after its
# last machine and client, unlinking its socket as it does.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 100 ]; do
	tries=$((tries + 1))
	sleep 0.1
done

out=$(timeout 10 "$gui" --version 2>&1)
has "$out" '^hull-gui [0-9]' "--version"

# No core: the added machine, stopped, its backend from its plan.
out=$(snapshot a.conf)
has "$out" 'gui-a +stopped +chroot \(ns\)' "no core: the added machine"
has "$out" 'core: not running' "no core: the status"
has "$out" 'on_disconnect.stop: native' "no core: the plan in the details"

# refute TEXT PATTERN WHAT: no line of TEXT matches PATTERN.
refute() {
	cases=$((cases + 1))
	if printf '%s\n' "$1" | grep -Eq -- "$2"; then
		fail "$3: a line matches /$2/" "$1"
	fi
}

# A refused one-machine file is one row, named for its machine.
out=$(snapshot c.conf)
has "$out" 'gui-c +stopped +qemu \(vm\)' "a refused machine added"
refute "$out" '^.{0,3}machine +stopped' "a refused machine added once"

# Start on it offers the token that would honour the refusal; Esc
# declines, and nothing runs.
keys "$dir" "b'\\x1b[B'" "b'\\x13'" "b'\\x1b'"
has "$out" 'vm:inotify-poll\?' "Start offers --accept"
cases=$((cases + 1))
if timeout 10 "$hull" ps 2>/dev/null | grep -q gui-c; then
	fail "declined, gui-c ran" "$(timeout 10 "$hull" ps 2>&1)"
fi

# New: a name and a command typed, Enter, and the file written and listed.
mkdir -p "$dir/newm/rootfs"
keys "$dir/newm" "b'\\x0e'" "b'gui-n'" "b'\\t\\t\\t\\t/probe say made-by-new'" "b'\\r'"
cases=$((cases + 1))
if ! grep -q '(command "/probe" "say" "made-by-new")' "$dir/newm/hull.conf" 2>/dev/null; then
	fail "New wrote no such command" "$(cat "$dir/newm/hull.conf" 2>&1)"
fi
has "$out" 'gui-n' "New's machine listed"

# Images: one of two refs removed through Store > Images: the menu opened,
# Down onto its item, Enter; Alt+R, which clicks Remove, and Enter for Yes.
timeout 30 "$hull" commit "$dir/rootfs" gui/one > /dev/null 2>&1
timeout 30 "$hull" commit "$dir/rootfs" gui/two > /dev/null 2>&1
keys "$dir" "b'\\x1bs'" "b'\\x1b[B'" "b'\\r'" "b'\\x1br'" "b'\\r'"
has "$out" 'hull tag -d gui/one' "Images ran the removal"
out=$(timeout 10 "$hull" images 2>&1)
refute "$out" 'gui/one' "Images removed the ref"
has "$out" 'gui/two' "Images kept the other"
# Push: the ref left, by Alt+U, to an address no registry answers at.
# The command is the one formed, and its refusal is shown, not lost.
keys "$dir" "b'\\x1bs'" "b'\\x1b[B'" "b'\\r'" "b'\\x1bu'" "b'127.0.0.1:1/gui/two:1'" "b'\\r'"
has "$out" 'hull push gui/two 127\.0\.0\.1:1/gui/two:1' "Push ran hull push REF IMAGE"
has "$out" 'exited [1-9]' "Push showed the refusal"

# Edit: $EDITOR on the window's terminal, the file planned again after.
# The editor is a script that renames the machine.
printf '%s\n' '(machine (hostname "gui-e0") (source "dir:./rootfs") (command "/probe"))' \
	> "$dir/e.conf"
printf '%s\n' 'sed -i s/gui-e0/gui-e1/ "$1"' > "$dir/edit.sh"
EDITOR="sh $dir/edit.sh"
export EDITOR
keys "$dir" "b'gui-e0'" "b'\\x1bOS'" -- e.conf
unset EDITOR
cases=$((cases + 1))
if ! grep -q gui-e1 "$dir/e.conf"; then
	fail "Edit ran no editor on the file" "$(cat "$dir/e.conf")"
fi
has "$out" 'gui-e1' "Edit planned the file again"

# Pools: a lease reserved in a pool of pools.conf, then released, each by
# its button's Alt+letter.
mkdir -p "$XDG_CONFIG_HOME/hull"
printf '%s\n' '(pools (gui-pool (capacity 2)))' > "$XDG_CONFIG_HOME/hull/pools.conf"
keys "$dir" "b'\\x1bp'" "b'\\x1b[B'" "b'\\r'" "b'\\x1br'"
has "$out" 'hull reserve: lease-' "Pools reserved a lease"
out=$(timeout 10 "$hull" lease ls 2>&1)
has "$out" '^lease-.* gui-pool +1 ' "the lease held in the pool"
keys "$dir" "b'\\x1bp'" "b'\\x1b[B'" "b'\\r'" "b'\\x1bl'"
out=$(timeout 10 "$hull" lease ls 2>&1)
refute "$out" 'gui-pool' "Pools released it"

# A board marked broken listed so, and put back by Fix, as hull board fix
# does it.
printf '%s\n' '(pools (gui-boards (members (board "gui-gb" (address "127.0.0.1")))))' \
	> "$XDG_CONFIG_HOME/hull/pools.conf"
mkdir -p "$XDG_DATA_HOME/hull/boards"
echo 'no answer' > "$XDG_DATA_HOME/hull/boards/gui-gb.broken"
keys "$dir" "b'\\x1bp'" "b'\\x1b[B'" "b'\\r'" "b'\\x1bf'"
has "$out" 'gui-gb +127\.0\.0\.1 +broken' "Pools lists the broken board"
out=$(timeout 10 "$hull" board ls gui-boards 2>&1)
has "$out" '^gui-gb .* up$' "Fix put the board back"
rm -f "$XDG_CONFIG_HOME/hull/pools.conf"

# Saved: the list again, with no file given.
out=$(snapshot)
has "$out" 'gui-a +stopped' "the added machine kept"

# A machine the core holds, started by the CLI and never added: listed
# beside the added one, running.
(cd "$dir" && exec timeout 120 "$hull" run -f b.conf > "$dir/run.out" 2>&1) &
run_pid=$!
tries=0
until timeout 10 "$hull" ps 2>/dev/null | grep -q 'gui-b .*running'; do
	tries=$((tries + 1))
	if [ "$tries" -gt 100 ]; then
		break
	fi
	sleep 0.1
done
out=$(snapshot)
has "$out" 'gui-b +running +chroot \(ns\)' "a core's machine listed"
has "$out" 'gui-a +stopped' "the added machine beside it"
has "$out" 'core: 1 machine' "the status counts the core's"

# Screen on a machine with none: said, from its address.
keys "$dir" "b'gui-b'" "b'\\x1b[19~'"
has "$out" 'gui-b shows no screen at 127' "Screen on a machine with none"

# Each request the window makes, against that machine.
out=$(timeout 60 "$link_test" gui-b gui-hello 2>&1)
status=$?
cases=$((cases + 1))
if [ "$status" != 0 ]; then
	fail "core_link_test exited $status" "$out"
fi
wait "$run_pid" 2>/dev/null
run_pid=

# The id rule, the spec New writes read back by hull plan, the writer's
# refusals, and the saved list.
rm -rf "${dir:?}/new"
out=$(timeout 60 "$list_test" "$hull" "$dir/new" 2>&1)
status=$?
cases=$((cases + 1))
if [ "$status" != 0 ]; then
	fail "machine_list_test exited $status" "$out"
fi

# Two machines on a wire of hull's: the Network tab lists the wire's
# ports, and Link Down sets the selected machine's link, as hull segment
# does.
(cd "$dir" && exec timeout 200 "$hull" run -f d.conf > "$dir/wire.out" 2>&1) &
run_pid=$!
tries=0
until [ "$(timeout 10 "$hull" ps 2>/dev/null | grep -c 'gui-d. .*running')" = 2 ]; do
	tries=$((tries + 1))
	if [ "$tries" -gt 100 ]; then
		break
	fi
	sleep 0.1
done
# The row is chosen by typing its name, the list's own search.
keys "$dir" "b'gui-db'" "b'\\x1bn'" "b'\\x1bw'" -- d.conf
has "$out" 'gui-wire gui-da out' "the Network tab lists the wire's ports"
has "$out" "gui-db on gui-wire: impair done" "Link Down sets the link"
# Capture gui-db's way in, inject three frames of the switch's own toward
# it, and stop the capture: the file holds them as pcap, and the port
# counts them injected. Each verb is chosen by typing its name's start
# into the Action box, "inj" as "i" finds impair first.
rm -f "${dir:?}/wire.pcap"
keys "$dir" "b'gui-db'" "b'\x1bn'" "b'\x1bt'" "b'c'" "b'\x1by'" "b'i'" \
	"b'\x1br'" "b'wire.pcap'" "b'\x1ba'" \
	"b'\x1bt'" "b'inj'" "b'\x1br'" "b'size=60 count=3'" "b'\x1ba'" \
	"b'\x1bt'" "b'c'" "b'\x1bl'" -- d.conf
has "$out" "gui-db on gui-wire: capture done" "Apply starts a capture"
cases=$((cases + 1))
magic=$(od -An -tx1 -N4 "$dir/wire.pcap" 2>/dev/null | tr -d ' ')
frames=$(($(wc -c < "$dir/wire.pcap" 2>/dev/null || echo 0)))
if [ "$magic" != "4d3cb2a1" ] || [ "$frames" -lt $((24 + 3 * (16 + 60))) ]; then
	fail "the capture holds the three injected frames" "magic $magic, $frames bytes"
fi
out=$(timeout 10 "$hull" segment gui-wire 2>&1)
has "$out" 'gui-db +in .*injected=3' "inject puts three frames toward gui-db"

# Commit to Image: F6 on gui-da and Enter on the ref offered, its name
# and the time; the store then holds gui-da's tree under it.
keys "$dir" "b'gui-da'" "b'\\x1b[17~'" "b'\\r'" -- d.conf
has "$out" 'gui-da committed as gui-da/[0-9]{8}-[0-9]{6}' "F6 commits the machine"
out=$(timeout 10 "$hull" images 2>&1)
has "$out" 'gui-da/[0-9]{8}-[0-9]{6}' "the store holds the commit"
timeout 30 "$hull" stop gui-da > /dev/null 2>&1
timeout 30 "$hull" stop gui-db > /dev/null 2>&1
wait "$run_pid" 2>/dev/null
run_pid=

# Change Medium on a pc machine, where qemu is here: a floppy of one
# sector that spins, F9, the drive offered (floppy), and a second image;
# then F9 again with no image, taking it out.
if command -v qemu-system-i386 > /dev/null 2>&1; then
	for img in spin.img swap.img; do
		truncate -s 0 "$dir/$img"
		truncate -s 1440K "$dir/$img"
		printf '\353\376' | dd of="$dir/$img" conv=notrunc status=none
		printf '\125\252' | dd of="$dir/$img" bs=1 seek=510 conv=notrunc status=none
	done
	printf '%s\n' '(machine (hostname "gui-pc") (backend pc) (source "media:floppy")' \
		'  (media (medium (type floppy) (source "image:./spin.img"))))' > "$dir/pc.conf"
	(cd "$dir" && exec timeout 120 "$hull" run -f pc.conf > "$dir/pc.out" 2>&1) &
	run_pid=$!
	tries=0
	until timeout 10 "$hull" ps 2>/dev/null | grep -q 'gui-pc .*running'; do
		tries=$((tries + 1))
		if [ "$tries" -gt 100 ]; then
			break
		fi
		sleep 0.1
	done
	keys "$dir" "b'gui-pc'" "b'\\x1b[20~'" "b'\\r'" "b'swap.img'" "b'\\r'" -- pc.conf
	has "$out" "gui-pc's floppy holds $dir/swap.img" "F9 swaps the floppy"
	keys "$dir" "b'gui-pc'" "b'\\x1b[20~'" "b'\\r'" "b'\\r'" -- pc.conf
	has "$out" "gui-pc's floppy taken out" "F9 with no image takes it out"
	timeout 30 "$hull" stop gui-pc > /dev/null 2>&1
	wait "$run_pid" 2>/dev/null
	run_pid=
else
	printf 'gui_test: no qemu-system-i386, Change Medium not tried\n'
fi

rm -rf "${dir:?}/config" "${dir:?}/new" "${dir:?}/newm" "${dir:?}/data" "${dir:?}/screen.txt" \
	"${dir:?}/wire.pcap" "${dir:?}/spin.img" "${dir:?}/swap.img" "${dir:?}/pc.conf" \
	"${dir:?}/pc.out"
printf 'gui_test: %d cases, %d failed\n' "$((cases))" "$failures"
[ "$failures" = 0 ]
