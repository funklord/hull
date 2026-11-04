#!/bin/sh
# virtualbox_test.sh -- the virtualbox engine (project.md section 12,
# phase 2): a machine under VBoxHeadless inside a qemu vm, as the user.
# Not part of `make check`, which needs no KVM and fetches nothing:
# `make check-virtualbox` runs it, and it needs VirtualBox's pinned
# package fetched (`hull virtualbox fetch`, 110 MB) and the host's
# vboxdrv.ko for this kernel. It says so and exits 1 where they are not.
#
# Its arguments are build/hull and the static probe. Each machine takes
# over a minute, booting nested, so there are few of them. What it starts
# is under `timeout`, a machine goes with its `hull run`, and the core is
# the test's own, whose idle clock is a second.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
dir=${BUILD_DIR:?}/test/virtualbox
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR" "$dir/rootfs/proc" "$dir/rootfs/dev" "$dir/rootfs/sys" \
	"$dir/rootfs/srv" "$dir/rootfs/etc" "$dir/rootfs/run"
chmod 700 "$XDG_RUNTIME_DIR"
root=$(cd "$dir" && pwd)
cp "$probe" "$root/rootfs/probe"
printf 'from the directory\n' > "$root/rootfs/etc/flip"
cases=0
failures=0
work=${XDG_CACHE_HOME:-$HOME/.cache}/hull/virtualbox-$(sed -n 's/^#define HULL_VBOX_VERSION "\(.*\)"/\1/p' engine/vbox.h)

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

if [ ! -x "$work/usr/lib/virtualbox/VBoxHeadless" ]; then
	printf 'virtualbox: nothing fetched: `hull virtualbox fetch` downloads it\n'
	exit 1
fi
if [ ! -r "/lib/modules/$(uname -r)/misc/vboxdrv.ko" ]; then
	printf 'virtualbox: no vboxdrv.ko for this kernel\n'
	exit 1
fi

# conf NAME ENTRIES...: a machine on the rootfs, its id NAME.
conf() {
	name=$1
	shift
	mkdir -p "$root/$name"
	printf '(machine (source "dir:%s/rootfs") (mutable overlay) %s)\n' "$root" "$*" \
		> "$root/$name/hull.conf"
}

# Nothing of a machine left: no helper, no outer vm, whose every process
# names the work directory, and no work directory.
left() {
	sleep 3
	ps -eo pid=,args= | grep -F "$work-run/" | grep -v grep
	ls -A "$work-run" 2>/dev/null
}

# --- at plan ---------------------------------------------------------------

conf keep '(backend virtualbox) (on_disconnect keep) (command "/probe" "say" "x")'
cases=$((cases + 1))
out=$("$hull" plan -f "$root/keep/hull.conf" 2>&1)
[ $? -ne 0 ] && printf '%s\n' "$out" | grep -q 'on_disconnect.keep: rejected: a UART' ||
	fail 'a kept machine was not refused at plan' "$out"

conf vol '(backend virtualbox) (volumes (volume (host "/tmp") (guest "/t")))' \
	'(command "/probe" "say" "x")'
cases=$((cases + 1))
out=$("$hull" plan -f "$root/vol/hull.conf" 2>&1)
[ $? -ne 0 ] && printf '%s\n' "$out" | grep -q 'volumes: rejected' ||
	fail 'a volume was not refused at plan' "$out"

# Nothing fetched: refused at once, naming the command.
conf bare '(backend virtualbox) (network none) (command "/probe" "say" "x")'
cases=$((cases + 1))
mkdir -p "$root/nocache" "$root/noruntime"
chmod 700 "$root/noruntime"
out=$(XDG_RUNTIME_DIR=$root/noruntime XDG_CACHE_HOME=$root/nocache \
	timeout 30 "$hull" run -f "$root/bare/hull.conf" 2>&1)
got=$?
[ "$got" -ne 0 ] && [ "$got" -ne 124 ] && printf '%s\n' "$out" | grep -q 'hull virtualbox fetch' ||
	fail "with nothing fetched, a run exited $got" "$out"

# --- one file, two engines --------------------------------------------------

# The file names no backend: the machine's own lines are the same on
# qemu and on VirtualBox, and its exit status, and it reaches the host's
# network by VirtualBox's NAT through the outer vm's passt.
listen=$(hostname -I | cut -d' ' -f1)
port=$(python3 -c '
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])')
timeout 200 python3 -c '
import socket, sys
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((sys.argv[1], int(sys.argv[2])))
s.listen(4)
while True:
	c, _ = s.accept()
	c.sendall(b"from the host\n")
	c.close()' "$listen" "$port" &
server=$!
conf flip '(hostname "flip") (workdir "/srv") (env (WHO "flip")) (memory "256M")' \
	"(command \"/probe\" \"env\" \"WHO\" \"cwd\" \"host\" \"read\" \"/etc/flip\"" \
	'"write" "/srv/flipped" "read" "/srv/flipped"' \
	"\"dial\" \"$listen\" \"$port\" \"exit\" \"7\")"
lines() {
	grep -v '^hull: \|: emulated: \|^\S*hull.conf' "$1" | sed "s/$listen:$port/HOST/"
}
for backend in vm virtualbox; do
	cases=$((cases + 1))
	(cd / && timeout -k 2 200 "$hull" run -f "$root/flip/hull.conf" --backend "$backend" \
		> "$root/$backend.out" 2>&1)
	got=$?
	[ "$got" = 7 ] || fail "$backend: the file exited $got" "$(cat "$root/$backend.out")"
done
kill "$server" 2>/dev/null
wait "$server" 2>/dev/null
cases=$((cases + 1))
lines "$root/vm.out" > "$root/vm.lines"
lines "$root/virtualbox.out" > "$root/virtualbox.lines"
[ -s "$root/vm.lines" ] && grep -q 'from the host' "$root/virtualbox.lines" &&
	cmp -s "$root/vm.lines" "$root/virtualbox.lines" ||
	fail 'the two engines said different things' \
		"$(diff "$root/vm.lines" "$root/virtualbox.lines" 2>&1; cat "$root/virtualbox.out")"

# --- a port, an exec and a stop ---------------------------------------------

address=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' live)
conf live '(backend virtualbox) (hostname "live") (memory "256M")' \
	"(ports (port (host $port) (guest 8080)))" \
	'(command "/probe" "serve" "tcp" "8080" "sleep" "120")'
(cd / && exec timeout -k 2 240 "$hull" run -f "$root/live/hull.conf" > "$root/live.out" 2>&1) &
runner=$!
# The probe serves for 20 seconds and a boot takes most of a minute or
# more on a loaded host, so the client is trying from the start.
timeout 200 python3 -c '
import socket, sys, time
end = time.time() + 190
while time.time() < end:
	try:
		c = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
		line = c.makefile().readline()
		if line:
			c.sendall(b"from the host\n")
			print(line.strip())
			sys.exit(0)
		c.close()
	except OSError:
		pass
	time.sleep(0.25)
print("nobody greeted")' "$address" "$port" > "$root/heard" 2>&1 &
client=$!
cases=$((cases + 1))
# An exec is refused while the machine boots, then taken.
exec_out=
tries=0
while [ "$tries" -lt 90 ]; do
	exec_out=$(timeout 20 "$hull" exec live -- /probe say inside 2>&1) && break
	tries=$((tries + 1))
	sleep 2
done
printf '%s\n' "$exec_out" | grep -q '^inside$' ||
	fail 'an exec did not reach the machine' "$exec_out"
cases=$((cases + 1))
wait "$client"
heard=$(cat "$root/heard")
[ -n "$heard" ] && [ "$heard" != 'nobody greeted' ] ||
	fail "a client on the machine's address heard: $heard" "$(cat "$root/live.out")"
cases=$((cases + 1))
out=$(timeout 60 "$hull" stop --timeout 5 live 2>&1)
got=$?
wait "$runner"
[ "$got" = 0 ] || fail "a stop exited $got" "$out"
cases=$((cases + 1))
out=$(left)
[ -z "$out" ] || fail 'a stopped machine left something' "$out"

# --- killed outright ---------------------------------------------------------

# Its `hull run` killed, the machine goes, and the helper, the outer vm
# and its work with it: mid-boot, the case with no agent to tell.
conf dead '(backend virtualbox) (network none) (memory "256M")' '(command "/probe" "sleep" "120")'
cases=$((cases + 1))
(cd / && exec "$hull" run -f "$root/dead/hull.conf" > "$root/dead.out" 2>&1) &
runner=$!
sleep 20
kill -9 "$runner"
wait "$runner" 2>/dev/null
tries=0
while [ "$tries" -lt 20 ] && [ -n "$(left)" ]; do
	tries=$((tries + 1))
done
out=$(left)
[ -z "$out" ] || fail 'a killed run left something' "$out"

printf 'virtualbox: %d cases, %d failures\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
