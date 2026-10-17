#!/bin/sh
# qemu_test.sh -- build/hull run on the qemu engine, as the user, booting
# the host's own kernel. Not part of `make check`, which needs no KVM:
# `make check-qemu` runs it.
#
# Each machine is booted once and every line it prints is checked, since
# a boot is two seconds; the emu case, under TCG, is about twenty. Every
# command runs under `timeout`, and the one loop, waiting for a machine
# to run, gives up after 40 tries. The files it writes are named in
# FILES, in $BUILD_DIR/test/qemu. The last cases require that no qemu of
# this suite's and none of its machine directories are left.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
dir=${BUILD_DIR:?}/test/qemu
# A core of the test's own, as in cli_test.sh.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR" "$dir/rootfs/proc" "$dir/rootfs/dev" "$dir/rootfs/sys" \
	"$dir/rootfs/srv" "$dir/data"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$probe" "$dir/rootfs/probe"
root=$(cd "$dir" && pwd)
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "dir:./rootfs") %s)\n' "$*" > "$root/$name.conf"
}

# boot NAME [SECONDS]: run it once, from /, as the files' paths are their
# own; its output is in $out and its status in $got.
boot() {
	out=$(cd / && timeout "${2:-60}" "$hull" run -f "$root/$1.conf" 2>&1)
	got=$?
}

# has STATUS PATTERN: the last boot exited STATUS and printed a line
# matching PATTERN.
has() {
	cases=$((cases + 1))
	if [ "$got" != "$1" ]; then
		fail "$name exited $got, wanted $1" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$2"; then
		fail "$name: no line matches /$2/" "$out"
	fi
}

if [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'qemu: /dev/kvm or /dev/vhost-vsock is not open to this user\n'
	exit 1
fi
printf 'from the host\n' > "$root/data/in"
rm -f "$root/data/out" "$root/rootfs/written"

# Output, status, and what the machine looks like from inside, all
# applied by the agent from the spec.
conf run '(backend vm) (hostname "qtest") (workdir "/srv") (env (GREETING "hi")) (memory "256M") (cpus 2)' \
	'(command "/probe" "say" "out" "err" "to stderr" "env" "GREETING" "cwd" "host" "uid" "nnp"' \
	'"session" "read" "/sys/devices/system/cpu/online" "read" "/proc/meminfo"' \
	'"read" "/sys/devices/system/clocksource/clocksource0/available_clocksource" "exit" "3")'
name=run
boot run
has 3 '^out$'
has 3 '^to stderr$'
has 3 '^GREETING=hi$'
has 3 '^cwd=/srv$'
has 3 '^host=qtest$'
has 3 '^uid=0$'
has 3 '^nnp=1$'
has 3 '^leader=1$'
has 3 '^read=0-1$'
# 256 MiB, less what the kernel keeps for itself.
has 3 '^read=MemTotal: +(1[5-9][0-9]|2[0-6][0-9])[0-9]{3} kB$'
has 3 'vm\.kernel\.bundled: emulated: the host.s own kernel'
# Accelerated: KVM gives the guest its clock.
has 3 '^read=kvm-clock'
cases=$((cases + 1))
if (cd / && timeout 60 "$hull" run -f "$root/run.conf" 2>/dev/null) | grep -q '^to stderr$'; then
	fail "stderr reached stdout" ""
fi

# How a workload ends, exactly: a signal, and a command not there.
conf die '(backend vm) (command "/probe" "die")'
name=die
boot die
has 143 ''
conf nope '(backend vm) (command "/nope")'
name=nope
boot nope
has 127 '/nope: No such file or directory'

# The directory in place, as the user's, and read-only when asked.
conf write '(backend vm) (command "/probe" "write" "/written")'
name=write
boot write
has 0 '^write=ok$'
cases=$((cases + 1))
[ -f "$root/rootfs/written" ] && [ "$(stat -c %u "$root/rootfs/written")" = "$(id -u)" ] ||
	fail "a write in the machine did not land in its directory as the user's" \
		"$(ls -ln "$root/rootfs" 2>&1)"
conf readonly '(backend vm) (security (readonly_root true)) (command "/probe" "write" "/again")'
name=readonly
boot readonly
has 0 '^write=Read-only file system$'

# Volumes, each a share of its own, read-write and read-only.
conf vol '(backend vm) (volumes (volume (host "./data") (guest "/data")))' \
	'(command "/probe" "read" "/data/in" "write" "/data/out")'
name=vol
boot vol
has 0 '^read=from the host$'
has 0 '^write=ok$'
cases=$((cases + 1))
[ -f "$root/data/out" ] || fail "a write through a volume did not reach the host" ""
conf volro '(backend vm) (volumes (volume (host "./data") (guest "/data") (mode ro)))' \
	'(command "/probe" "write" "/data/again")'
name=volro
boot volro
has 0 '^write=Read-only file system$'

# A running machine: listed, signalled through the agent, and stopped by
# SIGTERM to its frontend.
await() { # ID
	tries=0
	until "$hull" ps 2>/dev/null | grep -q "^$1 .*running\$" || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.5
	done
}
conf trap '(backend vm) (hostname "qtrap") (command "/probe" "trap" "say" "after")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/trap.conf" > "$root/trap.out" 2>&1) &
runner=$!
await qtrap
listed=$("$hull" ps 2>&1)
printf '%s\n' "$listed" | grep -Eq '^qtrap +qemu +vm +running$' ||
	fail "a running vm is not listed" "$listed"
cases=$((cases + 1))
tries=0
until grep -q '^ready$' "$root/trap.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
timeout 10 "$hull" kill -s TERM qtrap > /dev/null 2>&1
wait "$runner"
got=$?
[ "$got" = 0 ] && grep -q '^signal=15$' "$root/trap.out" && grep -q '^after$' "$root/trap.out" ||
	fail "a TERM through the agent was not the workload's to catch: $got" "$(cat "$root/trap.out")"

conf wait '(backend vm) (hostname "qwait") (command "/probe" "say" "up" "wait")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/wait.conf" > /dev/null 2>&1) &
runner=$!
await qwait
started=$(date +%s)
kill -TERM "$runner"
wait "$runner"
got=$?
took=$(($(date +%s) - started))
[ "$got" = 143 ] && [ "$took" -lt 10 ] ||
	fail "SIGTERM to hull run ended a vm with $got after ${took}s, wanted 143" ""

# A machine dies with its core: nothing could manage it after.
conf kept '(backend vm) (hostname "qkept") (on_disconnect keep) (command "/probe" "wait")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/kept.conf" > /dev/null 2>&1) &
runner=$!
await qkept
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
if [ -z "$core" ]; then
	fail "no core to kill" ""
else
	kill -KILL "$core"
	sleep 1
	left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
	[ -z "$left" ] || fail "a vm outlived its core" "$(ps -o pid,args -p "$left")"
fi
wait "$runner" 2>/dev/null
# Its directory is left, the core gone: the next core's first machine
# removes it, which the last case requires.

# The emu class: the same machine under TCG.
conf emu '(backend emu) (command "/probe" "say" "under tcg" "read"' \
	'"/sys/devices/system/clocksource/clocksource0/available_clocksource")'
name=emu
boot emu 120
has 0 '^under tcg$'
# Emulated: no KVM clock, which the vm case above has.
cases=$((cases + 1))
if printf '%s\n' "$out" | grep -q 'kvm-clock'; then
	fail "the emu class ran with KVM" "$out"
fi

# Nothing left: no qemu of this suite's, no machine directory.
cases=$((cases + 1))
sleep 2
left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
dirs=$(ls -d "$XDG_RUNTIME_DIR"/hull/qemu-* 2>/dev/null)
[ -z "$left" ] && [ -z "$dirs" ] || fail "a vm or its directory was left" "$left $dirs"

printf 'qemu: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
