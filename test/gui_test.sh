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
# runs under `timeout`; the one machine run in the background is a
# 60-second probe, stopped by the test and killed at the exit by its pid.

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
export XDG_RUNTIME_DIR XDG_CONFIG_HOME
export HULL_CORE_IDLE=1
DOCKER_HOST=unix://$XDG_RUNTIME_DIR/no-docker.sock
export DOCKER_HOST
rm -rf "${dir:?}/config" "${dir:?}/rootfs"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_CONFIG_HOME" "$dir/rootfs"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$probe" "$dir/rootfs/probe"
printf '%s\n' '(machine (hostname "gui-a") (source "dir:./rootfs") (command "/probe" "ticks" "600"))' \
	> "$dir/a.conf"
printf '%s\n' '(machine (hostname "gui-b") (source "dir:./rootfs") (command "/probe" "say" "gui-hello" "ticks" "600"))' \
	> "$dir/b.conf"

# Refused on a vm unless --accept: one machine, though plan names it on
# the refused field's line as well as on its own.
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

snapshot() {
	(cd "$dir" && timeout 60 "$gui" --hull "$hull" --tui --snapshot 110x26 "$@" 2>/dev/null)
}

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

rm -rf "${dir:?}/config" "${dir:?}/new"
printf 'gui_test: %d cases, %d failed\n' "$((cases))" "$failures"
[ "$failures" = 0 ]
