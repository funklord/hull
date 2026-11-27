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
rm -rf "${dir:?}/config" "${dir:?}/rootfs" "${dir:?}/data" "${dir:?}/newm" "${dir:?}/home"
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
	# A home of the test's own: New writes under ~/hull, the user's
	# machines, which a run here once did.
	HOME=$dir/home timeout 90 python3 -I "$here/gui_keys.py" "$gui" "$hull" "$work" "$dir/screen.txt" "$@" \
		> /dev/null 2>&1
	out=$(tr -s ' ' < "$dir/screen.txt")
}

# HOME the test's, as for keys: the window lists ~/hull's machines, and
# the user's own would be rows of every snapshot.
snapshot() {
	mkdir -p "$dir/home"
	(cd "$dir" && HOME=$dir/home timeout 60 "$gui" --hull "$hull" --tui --snapshot 110x26 "$@" \
		2>/dev/null)
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

# A compose file added is a row per service, the CLI reading it as a
# (machines ...) form, as hull up does; saved in a list of its own, so
# the rows the cases below count are not changed.
mkdir -p "$dir/compose"
printf '%s\n' 'services:' '  gui-web:' '    image: dir:../rootfs' '    command: ["/probe"]' \
	'  gui-db:' '    image: dir:../rootfs' '    command: ["/probe"]' > "$dir/compose/compose.yaml"
out=$(XDG_CONFIG_HOME=$dir/compose/config snapshot compose/compose.yaml)
has "$out" 'gui-web +stopped +chroot' "a compose file's first service added"
has "$out" 'gui-db +stopped +chroot' "and its second"
rm -rf "${dir:?}/compose"

# Settings (F3): Memory moved from Default down to 512M and written into
# the file on a line of its own, the comment kept; moved back to Default,
# the file byte for byte as it was; OK on a form nobody touched writing
# nothing. A list of its own, so the file is the only row and selected.
mkdir -p "$dir/settings/config"
printf '; kept\n(machine (hostname "gui-set") (source "dir:./rootfs")\n  (command "/probe"))\n' \
	> "$dir/settings.conf"
cp "$dir/settings.conf" "$dir/settings/orig.conf"
config_saved=$XDG_CONFIG_HOME
XDG_CONFIG_HOME=$dir/settings/config
keys "$dir" "b'\\x1bOR'" "b'\\x1b[B'" "b'\\x1b[B'" "b'\\r'" -- "$dir/settings.conf"
has "$(cat "$dir/settings.conf")" '^  \(memory "512M"\)\)$' "Settings wrote the memory chosen"
has "$(cat "$dir/settings.conf")" '^; kept$' "and kept the file's comment"
keys "$dir" "b'\\x1bOR'" "b'\\x1b[A'" "b'\\x1b[A'" "b'\\r'"
cases=$((cases + 1))
cmp -s "$dir/settings.conf" "$dir/settings/orig.conf" ||
	fail "Settings back to Default did not give the file back" "$(cat "$dir/settings.conf")"
touch -d '2020-01-01' "$dir/settings.conf"
keys "$dir" "b'\\x1bOR'" "b'\\r'"
cases=$((cases + 1))
[ "$(stat -c %Y "$dir/settings.conf")" = "$(date -d '2020-01-01' +%s)" ] ||
	fail "Settings untouched wrote the file" "$(cat "$dir/settings.conf")"
# And a pc's disks and discs: the first record, selected as the page
# opens, removed by Tab to Remove and Space, then OK past the ports' and
# folders' lists, the disc kept.
printf '(machine (backend pc) (hostname "gui-media") (source "media:disk")\n  (media\n    (medium (type disk) (source "image:./a.img"))\n    (medium (type cdrom) (source "image:./b.iso"))))\n' \
	> "$dir/settings.conf"
rm -rf "${dir:?}/settings/config"
mkdir -p "$dir/settings/config"
truncate -s 1M "$dir/settings/a.img" "$dir/settings/b.iso"
ln -sf settings/a.img "$dir/a.img"
ln -sf settings/b.iso "$dir/b.iso"
keys "$dir" "b'\\x1bOR'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" \
	"b'\\t'" "b'\\t'" "b' '" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" \
	"b' '" -- "$dir/settings.conf"
has "$(cat "$dir/settings.conf")" '^  \(media \(medium \(type cdrom\) \(source "image:\./b\.iso"\)\)\)\)$' \
	"Settings removed a disk and kept the disc"
# A PC's firmware, VirtualBox's Enable EFI: the eighth Tab from Memory,
# Down past Default and bios to uefi, written into its (tuning (pc ...)).
printf '(machine (backend pc) (hostname "gui-efi") (source "media:disk")\n  (media (medium (type disk) (source "image:./a.img"))))\n' \
	> "$dir/settings.conf"
rm -rf "${dir:?}/settings/config"
mkdir -p "$dir/settings/config"
keys "$dir" "b'\\x1bOR'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" \
	"b'\\x1b[B'" "b'\\x1b[B'" "b'\\r'" -- "$dir/settings.conf"
has "$(cat "$dir/settings.conf")" '\(tuning \(pc \(firmware uefi\)\)\)' "Settings set a PC's firmware"
# Boot from, VirtualBox's Boot Order, the eleventh Tab: the installer's
# disc chosen with Down, written as the machine's source.
keys "$dir" "b'\\x1bOR'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" \
	"b'\\t'" "b'\\t'" "b'\\t'" "b'\\x1b[B'" "b'\\r'" -- "$dir/settings.conf"
has "$(cat "$dir/settings.conf")" '\(source "media:cdrom"\)' "Settings set what a PC boots from"
# Resize, VirtualBox's Media Manager's size: the disk row the twelfth
# Tab, Resize three more, Up from 1 GiB to 2, Enter; then past Remove and
# the ports' and folders' lists to OK, which grows the qcow2.
if command -v qemu-img > /dev/null; then
	qemu-img create -q -f qcow2 "$dir/settings/grow.qcow2" 1G
	printf '(machine (backend pc) (hostname "gui-grow") (source "media:disk")\n  (media (medium (type disk) (source "image:./settings/grow.qcow2"))))\n' \
		> "$dir/settings.conf"
	rm -rf "${dir:?}/settings/config"
	mkdir -p "$dir/settings/config"
	keys "$dir" "b'\\x1bOR'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" \
		"b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b' '" "b'\\x1b[A'" "b'\\r'" \
		"b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b' '" -- "$dir/settings.conf"
	has "$(qemu-img info "$dir/settings/grow.qcow2" 2>&1)" '^virtual size: 2 GiB' "Settings grew the disk"
fi
# Every class's ports and shared folders: on a chroot machine the port's
# Remove is the sixth Tab from Memory, the folder kept.
printf '(machine (hostname "gui-ports") (source "dir:./rootfs")\n  (ports (port (host 8080) (guest 80)))\n  (volumes (volume (host "./rootfs") (guest "/data") (mode ro))))\n' \
	> "$dir/settings.conf"
rm -rf "${dir:?}/settings/config"
mkdir -p "$dir/settings/config"
keys "$dir" "b'\\x1bOR'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b' '" \
	"b'\\t'" "b'\\t'" "b'\\t'" "b'\\t'" "b' '" -- "$dir/settings.conf"
cases=$((cases + 1))
if grep -q '(ports' "$dir/settings.conf" || ! grep -q '(volume (host "./rootfs") (guest "/data") (mode ro))' \
	"$dir/settings.conf"; then
	fail "Settings did not remove the port and keep the folder" "$(cat "$dir/settings.conf")"
fi
# Export Appliance (Ctrl+X): a pc machine's disk as ~/NAME.ova, the path
# offered taken with Enter, its descriptor first.
printf '(machine (backend pc) (hostname "gui-export") (source "media:disk")\n  (media (medium (type disk) (source "image:./a.img"))))\n' \
	> "$dir/settings.conf"
rm -rf "${dir:?}/settings/config"
mkdir -p "$dir/settings/config"
keys "$dir" "b'\\x18'" "b'\\r'" -- "$dir/settings.conf"
has "$(tar tf "$dir/home/gui-export.ova" 2>&1)" '^gui-export\.ovf$' "Export Appliance wrote the .ova"
has "$(tar tf "$dir/home/gui-export.ova" 2>&1)" '^gui-export-disk1\.vmdk$' "with the machine's disk"
rm -f "$dir/home/gui-export.ova"
# Snapshots (F2) on a pc machine of a qcow2: Tab to Take, Space, the
# name offered taken with Enter, and `hull snapshot ls` then lists it.
if command -v qemu-img > /dev/null; then
	qemu-img create -q -f qcow2 "$dir/settings/snap.qcow2" 1M
	printf '(machine (backend pc) (hostname "gui-snap") (source "media:disk")\n  (media (medium (type disk) (source "image:./settings/snap.qcow2"))))\n' \
		> "$dir/settings.conf"
	rm -rf "${dir:?}/settings/config"
	mkdir -p "$dir/settings/config"
	keys "$dir" "b'\\x1bOQ'" "b'\\t'" "b' '" "b'\\r'" "b'\\x1b'" -- "$dir/settings.conf"
	has "$("$hull" snapshot ls "$dir/settings.conf" 2>&1)" '^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{4}	' \
		"Snapshots took one, named by the time"
fi
XDG_CONFIG_HOME=$config_saved
rm -rf "${dir:?}/settings" "${dir:?}/settings.conf" "${dir:?}/a.img" "${dir:?}/b.iso"

# Start on it offers the token that would honour the refusal; Esc
# declines, and nothing runs.
keys "$dir" "b'\\x1b[B'" "b'\\x13'" "b'\\x1b'"
has "$out" 'vm:inotify-poll\?' "Start offers --accept"
cases=$((cases + 1))
if timeout 10 "$hull" ps 2>/dev/null | grep -q gui-c; then
	fail "declined, gui-c ran" "$(timeout 10 "$hull" ps 2>&1)"
fi

# New: a name and a command typed, Enter, and the file written, in the
# machine's own folder under ~/hull, named as it was typed, and listed.
mkdir -p "$dir/home/hull/gui-n/rootfs" "$dir/newm"
keys "$dir/newm" "b'\\x0e'" "b'gui-n'" "b'\\t\\t\\t\\t/probe say made-by-new'" "b'\\r'"
cases=$((cases + 1))
if ! grep -q '(command "/probe" "say" "made-by-new")' "$dir/home/hull/gui-n/hull.conf" 2>/dev/null; then
	fail "New wrote no such command in ~/hull/gui-n" "$(ls -R "$dir/home" "$dir/newm" 2>&1)"
fi
has "$out" 'gui-n' "New's machine listed"
# New for a pc, as VirtualBox's New: the backend moved down to pc, which
# offers a new disk of 32 GiB and a screen here; Enter makes the disk
# beside the file it writes.
if command -v qemu-img > /dev/null; then
	keys "$dir/newm" "b'\\x0e'" "b'gui-npc'" "b'\\t\\t\\t'" "b'\\x1b[B'" "b'\\x1b[B'" "b'\\x1b[B'" \
		"b'\\x1b[B'" "b'\\r'"
	has "$(cat "$dir/home/hull/gui-npc/hull.conf" 2>&1)" '^    \(medium \(type disk\) \(source "image:\./disk\.qcow2"\)\)\)\)$' \
		"New wrote a pc of a new disk"
	has "$(qemu-img info "$dir/home/hull/gui-npc/disk.qcow2" 2>&1)" '^virtual size: 32 GiB' \
		"and made the disk"
fi

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
	printf '%s\n' '(machine (hostname "gui-pc") (backend pc) (source "media:floppy") (stop_grace "60s")' \
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
	# F12, its screen kept: the name offered taken, a PNG made.
	keys "$dir" "b'gui-pc'" "b'\\x1b[24~'" "b'\\r'" -- pc.conf
	has "$out" "gui-pc's screen is in $dir/gui-pc-screen.png" "F12 takes a screenshot"
	cases=$((cases + 1))
	[ "$(od -An -tx1 -N4 "$dir/gui-pc-screen.png" 2>/dev/null | tr -d ' ')" = 89504e47 ] ||
		fail "F12's screenshot is not a PNG"
	# Stop (Ctrl+T) on a guest spinning in its boot sector, which never
	# acts on the power button: after a moment the window says it is
	# still stopping and offers Power Off, which Enter takes -- the
	# machine stopped well inside its minute's grace, which only Power Off
	# could do.
	keys "$dir" "b'gui-pc'" "b'\\x14'" "b''" "b'\\r'" -- pc.conf
	has "$out" 'gui-pc is still stopping' "Stop says it waits on the guest"
	has "$out" 'gui-pc +(stopped|exited) +qemu-pc' "and Power Off, offered, ends it"
	timeout 70 "$hull" stop gui-pc > /dev/null 2>&1
	wait "$run_pid" 2>/dev/null
	run_pid=
else
	printf 'gui_test: no qemu-system-i386, Change Medium not tried\n'
fi

# Snapshots (F2) of a running pc, its memory with its disk: a boot sector
# spinning on a qcow2 under (mutable direct), its row chosen by typing,
# Tab to Take, Space and Enter; its file then lists the snapshot.
if command -v qemu-system-x86_64 > /dev/null && command -v qemu-img > /dev/null; then
	truncate -s 1M "$dir/live.img"
	printf '\353\376' | dd of="$dir/live.img" conv=notrunc status=none
	printf '\125\252' | dd of="$dir/live.img" bs=1 seek=510 conv=notrunc status=none
	qemu-img convert -O qcow2 "$dir/live.img" "$dir/live.qcow2"
	printf '%s\n' '(machine (hostname "gui-live") (backend pc) (source "media:disk") (mutable direct)' \
		'  (stop_grace "1s") (media (medium (type disk) (source "image:./live.qcow2"))))' > "$dir/live.conf"
	(cd "$dir" && exec timeout 120 "$hull" run -f live.conf > "$dir/live.out" 2>&1) &
	run_pid=$!
	tries=0
	until timeout 10 "$hull" ps 2>/dev/null | grep -q 'gui-live .*running'; do
		tries=$((tries + 1))
		if [ "$tries" -gt 100 ]; then
			break
		fi
		sleep 0.1
	done
	keys "$dir" "b'gui-live'" "b'\\x1bOQ'" "b'\\t'" "b' '" "b'\\r'" "b'\\x1b'" -- live.conf
	has "$("$hull" snapshot ls "$dir/live.conf" 2>&1)" '^[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{4}	' \
		"Snapshots took one of the running machine"
	timeout 30 "$hull" stop gui-live > /dev/null 2>&1
	wait "$run_pid" 2>/dev/null
	run_pid=
	rm -f "$dir/live.img" "$dir/live.qcow2" "$dir/live.conf" "$dir/live.out"
fi

# A machine put in a folder of ~/hull by hand -- copied, or imported by
# the CLI -- is listed when the window opens, nobody having added it.
mkdir -p "$dir/home/hull/gui-found"
printf '%s\n' '(machine (hostname "gui-found") (source "dir:../../../rootfs") (command "/probe"))' \
	> "$dir/home/hull/gui-found/hull.conf"
out=$(snapshot)
has "$out" 'gui-found +stopped +chroot' "a machine in ~/hull listed unasked"

# The Screen tab, pictured on a real X server, Xvfb's: a pc of (display
# local) booting a blank disk, SeaBIOS's text in it -- the grey of VGA
# text, which Qt's minimal platform painted white -- its row the only one,
# the list and home being the case's own and the row chosen by its id,
# the core still listing earlier cases' machines.
if command -v xvfb-run > /dev/null && command -v qemu-system-x86_64 > /dev/null &&
   python3 -I -c 'import PIL' 2> /dev/null; then
	truncate -s 1M "$dir/blank.img"
	printf '(machine (hostname "gui-scr") (backend pc) (source "media:disk") (display local) %s)\n' \
		'(media (medium (type disk) (source "image:./blank.img")))' > "$dir/scr.conf"
	(cd "$dir" && exec timeout 120 "$hull" run -f scr.conf > "$dir/scr.out" 2>&1) &
	scr_pid=$!
	tries=0
	until timeout 10 "$hull" ps 2>/dev/null | grep -q 'gui-scr .*running' || [ "$tries" -ge 30 ]; do
		tries=$((tries + 1))
		sleep 1
	done
	sleep 6
	mkdir -p "$dir/scr-config" "$dir/scr-home"
	(cd "$dir" && HOME=$dir/scr-home XDG_CONFIG_HOME=$dir/scr-config timeout 60 \
		xvfb-run -a -s '-screen 0 1280x800x24' env QT_QPA_PLATFORM=xcb \
		"$gui" --hull "$hull" --gui --select gui-scr --tab Screen --picture "$dir/scr.png" \
		> /dev/null 2>&1)
	grey=$(python3 -I -c 'import sys
from PIL import Image
print(sum(1 for p in Image.open(sys.argv[1]).convert("RGB").getdata() if p == (168, 168, 168)))' \
		"$dir/scr.png" 2>&1)
	cases=$((cases + 1))
	[ "$grey" -gt 200 ] 2>/dev/null ||
		fail "the Screen tab under Xvfb shows no SeaBIOS text (grey pixels: $grey)" "$(cat "$dir/scr.out")"
	timeout 30 "$hull" stop gui-scr > /dev/null 2>&1
	wait "$scr_pid" 2>/dev/null
	rm -rf "${dir:?}/scr-config" "${dir:?}/scr-home" "${dir:?}/blank.img" "${dir:?}/scr.conf" \
		"${dir:?}/scr.out" "${dir:?}/scr.png"
else
	printf 'gui_test: no xvfb-run, qemu-system-x86_64 or PIL, the Screen tab not pictured\n'
fi

rm -rf "${dir:?}/config" "${dir:?}/new" "${dir:?}/newm" "${dir:?}/data" "${dir:?}/screen.txt" \
	"${dir:?}/wire.pcap" "${dir:?}/spin.img" "${dir:?}/swap.img" "${dir:?}/pc.conf" \
	"${dir:?}/pc.out" "${dir:?}/gui-pc-screen.png"
printf 'gui_test: %d cases, %d failed\n' "$((cases))" "$failures"
[ "$failures" = 0 ]
