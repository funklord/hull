#!/bin/sh
# docker_test.sh -- build/hull run on the docker adapter, against the
# Docker daemon this account can reach. Not part of `make check`, which
# needs no daemon: `make check-docker` runs it.
#
# Its arguments are build/hull, the static probe and build/test/tree_tar.
# Uses alpine:latest, which must be present already: nothing is pulled.
# A dir source is imported as hull-dir:<digest>, and the test removes the
# images it made by those names, which it works out itself.
# Every machine is hostname dtest, so its container is hull-dtest-...,
# and the last case requires none of those to be left. Each command runs
# under `timeout`; the one loop, waiting for a port, gives up after 20
# tries. The files it writes are named in FILES, in $BUILD_DIR/test/docker.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
tree_tar=$3
dir=${BUILD_DIR:?}/test/docker
# A core of the test's own, as in cli_test.sh.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
mkdir -p "$dir/data"
root=$(cd "$dir" && pwd)
# And a store of its own, which `hull gc` collects: without it the suite
# collected the user's real store, every other session's and project's,
# and spent its 19 seconds there while the machine gc was to find in use
# ended its 20-second wait.
XDG_DATA_HOME=$root/data
export XDG_DATA_HOME
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

expect() { # STATUS PATTERN CONF [ARGS...]: run from /, as the files' paths are their own
	want=$1
	pattern=$2
	conf=$3
	shift 3
	cases=$((cases + 1))
	out=$(cd / && timeout 60 "$hull" run -f "$root/$conf.conf" "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$conf exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$conf: no line matches /$pattern/" "$out"
	fi
}

conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "alpine:latest") (backend docker) (hostname "dtest") %s)\n' \
		"$*" > "$root/$name.conf"
}

if ! timeout 10 docker image inspect alpine:latest > /dev/null 2>&1; then
	printf 'docker: alpine:latest is not here, or the daemon is not reachable\n'
	exit 1
fi
printf 'from the host\n' > "$root/data/in"
rm -f "$root/data/out"

# Output, status, and what the machine looks like from inside.
conf run '(command "sh" "-c" "echo out; echo to stderr >&2; echo GREETING=$GREETING; pwd; hostname; echo uid=$(id -u):$(id -g); exit 3")' \
	'(workdir "/tmp") (env (GREETING "hi"))'
expect 3 '^out$' run
expect 3 '^to stderr$' run
expect 3 '^GREETING=hi$' run
expect 3 '^/tmp$' run
expect 3 '^dtest$' run
expect 3 "^uid=$(id -u):$(id -g)\$" run
cases=$((cases + 1))
if (cd / && timeout 60 "$hull" run -f "$root/run.conf" 2>/dev/null) | grep -q '^to stderr$'; then
	fail "stderr reached stdout" ""
fi

# Volumes, read-write and read-only, and a read-only root. The workload is
# this account's uid, so what it writes through a volume is this
# account's file.
conf vol '(command "sh" "-c" "cat /data/in; echo written > /data/out && echo write=ok")' \
	'(volumes (volume (host "./data") (guest "/data")))'
expect 0 '^from the host$' vol
expect 0 '^write=ok$' vol
cases=$((cases + 1))
[ -O "$root/data/out" ] || fail "a write through a volume is not this account's file" "$(ls -l "$root/data")"
rm -f "$root/data/out"
# Refused for being read-only, by name, and somewhere this uid could
# otherwise write: /tmp, on the root, is everybody's.
conf volro '(command "sh" "-c" "echo x > /data/out || true")' \
	'(volumes (volume (host "./data") (guest "/data") (mode ro)))'
expect 0 'Read-only file system' volro
conf rootro '(command "sh" "-c" "touch /tmp/x || true")' '(security (readonly_root true))'
expect 0 'Read-only file system' rootro

# Section 7: no capability, and a grant refused at plan time, since docker
# gives capabilities only to a root workload.
conf nocap '(command "sh" "-c" "touch /tmp/f && chown 1 /tmp/f && echo chown=ok || echo chown=refused")'
expect 0 '^chown=refused$' nocap
conf cap '(command "true")' '(security (allow chown))'
expect 125 'security\.allow: rejected: docker grants capabilities only to root' cap

# A signal is 128 and its number, from the workload or sent to hull. With
# an init at PID 1, a SIGTERM to hull stops the machine well inside the
# ten seconds after which docker would use SIGKILL, and that is 137.
conf die '(command "sh" "-c" "kill -TERM $$")'
expect 143 '' die
conf wait '(command "sh" "-c" "echo waiting; sleep 30")'
cases=$((cases + 1))
started=$(date +%s)
out=$(timeout --preserve-status -s TERM 3 "$hull" run -f "$root/wait.conf" 2>&1)
got=$?
took=$(($(date +%s) - started))
[ "$got" = 143 ] && [ "$took" -lt 10 ] && printf '%s\n' "$out" | grep -q '^waiting$' ||
	fail "SIGTERM to hull run exited $got after ${took}s, wanted 143 within 10" "$out"

# A port bound on the machine's address (section 8.1), reached from the
# host there and not on 127.0.0.1. The address is worked here rather than
# taken from the core: FNV-1a of the id, modulo 256 * 254, as x * 254 + y - 1.
at=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' dtest)
conf port '(command "sh" "-c" "echo hello from the machine | nc -l -p 80")' \
	'(ports (port (host 18080) (guest 80)))'
cases=$((cases + 1))
(cd / && timeout 60 "$hull" run -f "$root/port.conf" > /dev/null 2>&1) &
server=$!
got=
tries=0
while [ -z "$got" ] && [ "$tries" -lt 20 ]; do
	tries=$((tries + 1))
	got=$(timeout 2 bash -c 'cat < /dev/tcp/$0/18080' "$at" 2>/dev/null)
	[ -n "$got" ] || sleep 0.5
done
cases=$((cases + 1))
if timeout 2 bash -c ': < /dev/tcp/127.0.0.1/18080' 2>/dev/null; then
	fail "port 18080 was reached on 127.0.0.1 as well as $at" ""
fi
wait "$server"
[ "$got" = "hello from the machine" ] || fail "port 18080 on $at answered '$got'" ""

# exec in the running container: output and status, stdin, the user's
# uid, and a command ended with the frontend that ran it.
conf execd '(command "sleep" "60")' '(on_disconnect keep)'
(cd / && exec "$hull" run -f "$root/execd.conf" > /dev/null 2>&1) &
execer=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^dtest .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
out=$(timeout 30 "$hull" exec dtest -- sh -c 'echo out; echo err >&2; id -u; exit 4' 2>&1)
got=$?
[ "$got" = 4 ] && printf '%s\n' "$out" | grep -q '^out$' && printf '%s\n' "$out" | grep -q '^err$' &&
	printf '%s\n' "$out" | grep -q "^$(id -u)\$" || fail "exec exited $got" "$out"
cases=$((cases + 1))
out=$(echo through stdin | timeout 30 "$hull" exec dtest cat 2>&1)
[ "$out" = "through stdin" ] || fail "exec's stdin came back as '$out'" ""
cases=$((cases + 1))
out=$(timeout 30 "$hull" exec dtest /nope 2>&1)
got=$?
[ "$got" != 0 ] || fail "a missing command exited 0" "$out"
cases=$((cases + 1))
timeout -s KILL 2 "$hull" exec dtest sleep 300 > /dev/null 2>&1
sleep 1
left=$(timeout 10 docker top "$(timeout 10 docker ps -q --filter name=hull-dtest-)" 2>/dev/null | grep -c 'sleep 300')
[ "$left" = 0 ] || fail "an exec outlived the frontend that ran it" "$left"
# stdin many times the 64 KiB the core holds, whole; and to a command
# that does not read for six seconds, where the core once refused it
# after five. That is 3 MB, since the daemon's own buffers take 300 kB
# without the core's write ever waiting.
cases=$((cases + 1))
out=$(seq 1 200000 | timeout 30 "$hull" exec dtest sh -c 'md5sum' 2>&1)
[ "$out" = "$(seq 1 200000 | md5sum)" ] || fail "exec's stdin did not arrive whole" "$out"
cases=$((cases + 1))
out=$(head -c 3000000 /dev/zero | timeout 30 "$hull" exec dtest sh -c 'sleep 6; wc -c' 2>&1)
[ "$out" = 3000000 ] || fail "a command reading late got '$out' of 3000000 bytes" ""
# Output many times the 64 KiB a frontend leaves unasked, to a reader
# seven seconds behind, where the core once dropped its frontend after
# five; the command is held meanwhile rather than read into the core.
cases=$((cases + 1))
(timeout 40 "$hull" exec dtest seq 1 3000000 | (sleep 7; md5sum) > "$root/held.out" 2>&1) &
held=$!
sleep 3
left=$(timeout 10 docker top "$(timeout 10 docker ps -q --filter name=hull-dtest-)" 2>/dev/null |
	grep -c 'seq 1 3000000')
wait "$held"
out=$(cat "$root/held.out")
[ "$left" = 1 ] && [ "$out" = "$(seq 1 3000000 | md5sum)" ] ||
	fail "output to a reader behind was not held, or did not arrive whole" "$left running; $out"
# -t, through script for a terminal as in cli_test.sh. Docker's API 1.41
# sizes an exec only once it has started, as its own client does, so the
# command waits a second before it looks.
HULL=$hull SHELL=/bin/sh
export HULL SHELL
cases=$((cases + 1))
out=$(timeout 30 script -qec 'stty cols 101 rows 31; "$HULL" exec -t dtest sh -c "test -t 0 &&
	echo terminal; sleep 1; stty size"' /dev/null 2>&1 | tr -d '\r')
printf '%s\n' "$out" | grep -q '^terminal$' && printf '%s\n' "$out" | grep -q '^31 101$' ||
	fail "exec -t gave no terminal of the frontend's size" "$out"
cases=$((cases + 1))
# The resize docker makes once the exec starts is a SIGWINCH of its own,
# so the command waits for the size rather than for a signal, ten seconds
# at most.
out=$(timeout 30 script -qec '"$HULL" exec -t dtest sh -c "i=0; while [ \"\$(stty size)\" != \"20 90\" ] &&
	[ \$i -lt 100 ]; do sleep 0.1; i=\$((i + 1)); done; stty size" < /dev/tty & sleep 2;
	stty cols 90 rows 20; kill -WINCH $!; wait $!' /dev/null 2>&1 | tr -d '\r')
printf '%s\n' "$out" | grep -q '^20 90$' || fail "a resize did not reach the exec" "$out"
# A signal to the frontend is the command's to handle.
cases=$((cases + 1))
out=$(timeout 30 sh -c '"$0" exec dtest sh -c "trap \"echo caught; exit 0\" TERM; echo ready;
	while :; do sleep 0.1; done" & sleep 2; kill -TERM $!; wait $!' "$hull" 2>&1)
got=$?
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -q '^caught$' ||
	fail "a TERM to exec was not the command's: $got" "$out"
# ssh: alpine's own login shell, on a terminal, with the frontend's TERM.
# The line typed echoes as itself, which the pattern does not match.
cases=$((cases + 1))
out=$(timeout 30 sh -c '(sleep 2; printf "echo \"term=\$TERM tty=\$(tty)\"; exit\r"; sleep 2) |
	script -qec "TERM=xterm-hull \"\$HULL\" ssh dtest" /dev/null' 2>&1 | tr -d '\r')
got=$?
printf '%s\n' "$out" | grep -q '^term=xterm-hull tty=/dev/pts/' ||
	fail "ssh gave no login shell with the frontend's TERM" "$out"
timeout 30 "$hull" stop dtest > /dev/null 2>&1
wait "$execer" 2>/dev/null

# kill: docker's kill signals the container's init, which forwards a TERM
# to the workload, here a shell that catches it; a KILL ends the container,
# 137. Each waits for "ready", ten seconds at most.
conf killd '(command "sh" "-c" "trap \"echo caught; exit 0\" TERM; echo ready; while :; do sleep 0.1; done")'
await_ready() { # FILE
	tries=0
	until grep -q '^ready$' "$1" 2>/dev/null || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
}
cases=$((cases + 1))
rm -f "$root/killd.out"
(cd / && exec timeout -k 5 60 "$hull" run -f "$root/killd.conf" > "$root/killd.out" 2>&1) &
killed=$!
await_ready "$root/killd.out"
out=$(timeout 30 "$hull" kill -s TERM dtest 2>&1) || fail "kill -s TERM refused" "$out"
wait "$killed"
got=$?
[ "$got" = 0 ] && grep -q '^caught$' "$root/killd.out" ||
	fail "a TERM to the workload: run exited $got, wanted 0 once caught" "$(cat "$root/killd.out")"
cases=$((cases + 1))
rm -f "$root/killd.out"
(cd / && exec timeout -k 5 60 "$hull" run -f "$root/killd.conf" > "$root/killd.out" 2>&1) &
killed=$!
await_ready "$root/killd.out"
out=$(timeout 30 "$hull" kill dtest 2>&1) || fail "kill refused" "$out"
wait "$killed"
got=$?
[ "$got" = 137 ] && ! grep -q '^caught$' "$root/killd.out" ||
	fail "a KILL to the workload: run exited $got, wanted 137" "$(cat "$root/killd.out")"

# A dir source as an overlay: imported as hull-dir:<the SHA-256 of its
# tar>, worked out here with sha256sum over tree_tar's stream, and run
# from that. A write lands in the container and not in the directory;
# / is root's, as docker makes it. A second run imports nothing, and a
# changed tree is another image. In place, a dir is refused.
mkdir -p "$root/rootfs/srv"
cp "$probe" "$root/rootfs/probe"
rm -f "$root/rootfs/srv/added" "$root/rootfs/srv/written"
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "say" "from a dir" "uid" "write" "/srv/written" "write" "/written")' \
	> "$root/dirov.conf"
digest=$(timeout 60 "$tree_tar" "$root/rootfs" | sha256sum | cut -c1-64)
expect 0 'source\.dir\.overlay: emulated: a docker import of the tree' dirov
printf '%s\n' "$out" | grep -q '^from a dir$' && printf '%s\n' "$out" | grep -q "^uid=$(id -u)\$" &&
	printf '%s\n' "$out" | grep -q '^write=ok$' &&
	printf '%s\n' "$out" | grep -q '^write=Permission denied$' ||
	fail "a dir's machine ran otherwise" "$out"
# (network none) is docker's none: lo alone, so a dial to a port nothing
# holds there is refused and one out is unreachable; ports with it are
# refused, having no network to be on. The same tree, so the same import.
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(network none) (command "/probe" "dial" "127.0.0.1" "9" "dial" "192.0.2.1" "9")' \
	> "$root/netnone.conf"
expect 0 '^dial 127\.0\.0\.1:9=Connection refused$' netnone
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^dial 192\.0\.2\.1:9=Network is unreachable$' ||
	fail "a machine of (network none) reached out" "$out"
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(network none) (ports (port (host 18099) (guest 80))) (command "/probe")' > "$root/netports.conf"
expect 125 'ports need the machine.s own network' netports
cases=$((cases + 1))
created=$(timeout 10 docker image inspect -f '{{.Created}}' "hull-dir:$digest" 2>&1) ||
	fail "no image hull-dir:$digest" "$created"
cases=$((cases + 1))
[ ! -e "$root/rootfs/srv/written" ] || fail "an overlay's write landed in the directory" ""
expect 0 '^from a dir$' dirov
cases=$((cases + 1))
again=$(timeout 10 docker image inspect -f '{{.Created}}' "hull-dir:$digest" 2>&1)
[ "$again" = "$created" ] || fail "the second run imported the tree again" "$created then $again"
# Labelled with the SHA-256 of the tree's real path, worked out here.
cases=$((cases + 1))
label=$(timeout 10 docker image inspect -f '{{index .Config.Labels "hull.tree"}}' "hull-dir:$digest" 2>&1)
named=$(realpath "$root/rootfs" | tr -d '\n' | sha256sum | cut -c1-64)
[ "$label" = "$named" ] || fail "the import's hull.tree is not its tree's" "$label, wanted $named"
# A udp port, as on qemu: each client answered on its own, and each
# datagram whole. Not, as on qemu, an empty datagram, which docker drops,
# nor a client's address kept across a datagram refused in the machine,
# which docker changes (project.md section 4.9). The client sends again,
# 200 times a quarter of a second apart at most.
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(ports (port (host 18053) (guest 5353) (proto udp))) (command "/probe" "echo" "5353")' \
	> "$root/udp.conf"
cases=$((cases + 1))
rm -f "$root/udp.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/udp.conf" > "$root/udp.out" 2>&1) &
runner=$!
heard=$(timeout 120 python3 -u -c '
import socket, sys
at = (sys.argv[1], 18053)
def ask(s, data):
    for _ in range(200):
        s.sendto(data, at)
        s.settimeout(0.25)
        try:
            while True:
                if s.recv(65536) == data:
                    return True
        except socket.timeout:
            pass
    return False
a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print("a=%s b=%s" % (ask(a, b"from a"), ask(b, b"from b")))
for n in (1, 1400, 65000):
    print("%d=%s" % (n, ask(a, bytes((i * 7) & 0xff for i in range(n)))))
print("end=%s" % ask(b, b"end"))' "$at" 2>&1)
wait "$runner"
got=$?
[ "$got" = 0 ] && [ "$heard" = "a=True b=True
1=True
1400=True
65000=True
end=True" ] && grep -q '^echoed from 2$' "$root/udp.out" ||
	fail "a udp port's datagrams were not each client's, whole: $got" \
		"$heard; $(cat "$root/udp.out")"
# hull proxy-ssh, as on qemu: the container's own port 22, declared
# nowhere, reached at its address as ssh's ProxyCommand would; and a
# refusal, dialled in the three seconds before the probe listens.
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "say" "up" "sleep" "3" "serve" "tcp" "22")' > "$root/dial.conf"
cases=$((cases + 1))
rm -f "$root/dial.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/dial.conf" > "$root/dial.out" 2>&1) &
runner=$!
tries=0
until grep -q '^up$' "$root/dial.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
refused=$(cd / && timeout 20 "$hull" proxy-ssh dtest.hull < /dev/null 2>&1)
refused_got=$?
tries=0
until grep -q '^serving$' "$root/dial.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
heard=$(printf 'hi from ssh\n' | (cd / && timeout 20 "$hull" proxy-ssh dtest 2>&1))
got=$?
wait "$runner"
[ "$got" = 0 ] && [ "$heard" = "hello from the machine" ] &&
	grep -q '^served=hi from ssh$' "$root/dial.out" ||
	fail "proxy-ssh did not carry the container's port 22 both ways: $got" \
		"$heard; $(cat "$root/dial.out")"
cases=$((cases + 1))
[ "$refused_got" = 125 ] && [ "$refused" = "hull: port 22: Connection refused" ] ||
	fail "proxy-ssh to a port nothing listens on was not refused: $refused_got" "$refused"
# What the host spends on a container, as on chroot: 300 ms of the probe's
# CPU and 300 of a child it reaped, 32 MiB touched and 4 MiB written and
# synced, then 400 ms and 4 MiB more in an exec, which containerd's shim
# reaps and only docker's cgroup figures still count.
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "spin" "300" "reap" "300" "hold" "32" "fill" "/srv/filled" "4" "trap")' \
	> "$root/statsd.conf"
figure() { # NAME STATS: one figure of hull stats, the cpu in microseconds
	printf '%s\n' "$2" | awk -v name="$1" '$1 == name { gsub(/\./, "", $2); print $2 + 0 }'
}
rm -f "$root/statsd.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/statsd.conf" > "$root/statsd.out" 2>&1) &
runner=$!
tries=0
until grep -q '^ready$' "$root/statsd.out" 2>/dev/null || [ "$tries" -ge 80 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
first=$(timeout 20 "$hull" stats dtest 2>&1)
cpu=$(figure cpu "$first")
memory=$(figure memory "$first")
io=$(figure io "$first")
[ "${cpu:-0}" -ge 590000 ] && [ "$cpu" -lt 5000000 ] &&
	[ "${memory:-0}" -ge 33554432 ] && [ "$memory" -lt 268435456 ] &&
	[ "${io:-0}" -ge 4194304 ] && [ "$io" -lt 67108864 ] ||
	fail "stats: wanted the probe's 0.6 s, its 32 MiB and its 4 MiB" "$first"
out=$(timeout 30 "$hull" exec dtest /probe spin 400 fill /srv/filled 4 2>&1)
cases=$((cases + 1))
second=$(timeout 20 "$hull" stats dtest 2>&1)
[ "$(figure cpu "$second")" -ge $((${cpu:-0} + 390000)) ] &&
	[ "$(figure io "$second")" -ge $((${io:-0} + 4194304)) ] ||
	fail "stats: an ended exec's cpu and io, wanted 0.4 s and 4 MiB more" "$out
$first
$second"
timeout 10 "$hull" kill -s TERM dtest > /dev/null 2>&1
wait "$runner"
cases=$((cases + 1))
out=$(timeout 20 "$hull" stats dtest 2>&1)
[ $? = 1 ] && printf '%s\n' "$out" | grep -q 'not running\|no such machine' ||
	fail "stats of an ended container did not say so" "$out"
# A fresh import retires the tree's others, but not one a container
# uses: a machine kept running on the first state holds it.
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest-keep") %s)\n' \
	'(command "/probe" "trap")' > "$root/dirkeep.conf"
rm -f "$root/dirkeep.out"
(cd / && exec timeout -k 2 30 "$hull" run -f "$root/dirkeep.conf" > "$root/dirkeep.out" 2>&1) &
keeper=$!
tries=0
until grep -q '^ready$' "$root/dirkeep.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
printf 'added\n' > "$root/rootfs/srv/added"
changed=$(timeout 60 "$tree_tar" "$root/rootfs" | sha256sum | cut -c1-64)
expect 0 '^from a dir$' dirov
cases=$((cases + 1))
[ "$changed" != "$digest" ] && timeout 10 docker image inspect "hull-dir:$changed" > /dev/null 2>&1 ||
	fail "a changed tree was not imported as hull-dir:$changed" ""
cases=$((cases + 1))
timeout 10 docker image inspect "hull-dir:$digest" > /dev/null 2>&1 ||
	fail "an import a running machine uses was retired" ""
timeout 10 "$hull" kill -s TERM dtest-keep > /dev/null 2>&1
wait "$keeper"
# Once nothing uses it, the next import retires the first. The second
# has another name as well, which only a forced removal would take, and
# the daemon refuses an unforced one: so it stays. A running container
# cannot tell forced from not, since the daemon refuses both.
timeout 10 docker tag "hull-dir:$changed" "hull-dtest-named:$changed" > /dev/null 2>&1
printf 'more\n' > "$root/rootfs/srv/more"
third=$(timeout 60 "$tree_tar" "$root/rootfs" | sha256sum | cut -c1-64)
expect 0 '^from a dir$' dirov
cases=$((cases + 1))
! timeout 10 docker image inspect "hull-dir:$digest" > /dev/null 2>&1 &&
	timeout 10 docker image inspect "hull-dir:$third" > /dev/null 2>&1 ||
	fail "a fresh import did not retire its tree's other" \
		"$(timeout 10 docker images --filter "label=hull.tree=$named" 2>&1)"
cases=$((cases + 1))
timeout 10 docker image inspect "hull-dtest-named:$changed" > /dev/null 2>&1 ||
	fail "an import with another name was retired with it" ""

# gc removes an import whose tree is gone, keeps one a running machine
# uses until it has stopped, and leaves a live tree's alone. An import
# from before the labels, made here by docker with none, goes too.
mkdir -p "$root/gone" "$root/inuse/srv" "$root/empty"
cp "$probe" "$root/gone/probe"
cp "$probe" "$root/inuse/probe"
printf 'in use\n' > "$root/inuse/srv/which"
printf '(machine (source "dir:./gone") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "say" "gone")' > "$root/gone.conf"
printf '(machine (source "dir:./inuse") (mutable overlay) (backend docker) (hostname "dtest-inuse") %s)\n' \
	'(command "/probe" "wait")' > "$root/inuse.conf"
gone=$(timeout 60 "$tree_tar" "$root/gone" | sha256sum | cut -c1-64)
inuse=$(timeout 60 "$tree_tar" "$root/inuse" | sha256sum | cut -c1-64)
unlabelled=0000000000000000000000000000000000000000000000000000000000000000
expect 0 '^gone$' gone
rm -f "$root/inuse.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/inuse.conf" > "$root/inuse.out" 2>&1) &
holder=$!
tries=0
until grep -q '^waiting$' "$root/inuse.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
timeout 30 tar -C "$root/empty" -cf - . | timeout 60 docker import - "hull-dir:$unlabelled" > /dev/null 2>&1
rm -rf -- "${root:?}/gone" "${root:?}/inuse"
cases=$((cases + 1))
out=$(cd / && timeout 60 "$hull" gc 2>&1)
got=$?
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -qx "docker: removed hull-dir:$gone, its tree is gone" &&
	printf '%s\n' "$out" | grep -qx "docker: removed hull-dir:$unlabelled, imported before hull.tree labelled its tree" &&
	printf '%s\n' "$out" | grep -q "^docker: kept: .* hull-dir:$inuse, its tree is gone\$" &&
	! printf '%s\n' "$out" | grep -q "hull-dir:$third" ||
	fail "gc did not remove the imports nothing wants, and only those: $got" "$out"
cases=$((cases + 1))
! timeout 10 docker image inspect "hull-dir:$gone" > /dev/null 2>&1 &&
	! timeout 10 docker image inspect "hull-dir:$unlabelled" > /dev/null 2>&1 &&
	timeout 10 docker image inspect "hull-dir:$inuse" > /dev/null 2>&1 &&
	timeout 10 docker image inspect "hull-dir:$third" > /dev/null 2>&1 ||
	fail "gc's report is not what the daemon holds" \
		"$(timeout 10 docker images --filter reference=hull-dir 2>&1)"
timeout 10 "$hull" kill -s TERM dtest-inuse > /dev/null 2>&1
wait "$holder"
cases=$((cases + 1))
out=$(cd / && timeout 60 "$hull" gc 2>&1)
printf '%s\n' "$out" | grep -qx "docker: removed hull-dir:$inuse, its tree is gone" &&
	! timeout 10 docker image inspect "hull-dir:$inuse" > /dev/null 2>&1 ||
	fail "gc kept an import once nothing used it" "$out"
timeout 30 docker rmi "hull-dir:$digest" "hull-dir:$changed" "hull-dtest-named:$changed" \
	"hull-dir:$third" "hull-dir:$gone" "hull-dir:$inuse" "hull-dir:$unlabelled" > /dev/null 2>&1
rm -f "$root/rootfs/srv/added" "$root/rootfs/srv/more"
printf '(machine (source "dir:./rootfs") (backend docker) (hostname "dtest") (command "/probe"))\n' \
	> "$root/dirin.conf"
expect 125 'source\.dir\.direct: rejected' dirin

# A machine kept, under a core killed outright, is the next core's: one
# that `hull ps` starts, though it starts none otherwise, as the kept
# machine is a core's to hold. It has the log from the machine's start,
# runs a command, and has its exit; then the file and the container go.
# The workload waits for the file that command makes, 30 s at most.
kept=$XDG_RUNTIME_DIR/hull/machines
printf '(machine (source "alpine:latest") (backend docker) (hostname "dtest-adopt") %s %s)\n' \
	'(command "sh" "-c" "echo before; i=0; while [ ! -e /tmp/go ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done; echo after; exit 5")' \
	'(on_disconnect keep)' > "$root/adopt.conf"
# keep_until_kept CONF OUT: run CONF in the background, its frontend's
# pid in $runner, until OUT says `before`, 10 s at most.
keep_until_kept() {
	rm -f "$2"
	(cd / && exec timeout -k 2 60 "$hull" run -f "$1" > "$2" 2>&1) &
	runner=$!
	tries=0
	until grep -q '^before$' "$2" 2>/dev/null || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
}
# kill_core: SIGKILL to the core that holds the lock, and its frontend
# gone with it.
kill_core() {
	kill -KILL "$(head -n 1 "$XDG_RUNTIME_DIR/hull/core.lock")" 2> /dev/null
	wait "$runner"
}
keep_until_kept "$root/adopt.conf" "$root/adopt.out"
cases=$((cases + 1))
[ -f "$kept/dtest-adopt" ] || fail "a kept machine has no file for the next core" "$(ls -la "$kept")"
kill_core
cases=$((cases + 1))
out=$(timeout 30 "$hull" ps 2>&1)
printf '%s\n' "$out" | grep -q '^dtest-adopt .*running$' &&
	grep -q '^hull core: machine dtest-adopt is taken back$' "$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "the next core did not take the kept machine back" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
cases=$((cases + 1))
out=$(timeout 30 "$hull" exec dtest-adopt touch /tmp/go 2>&1)
got=$?
logs=$(timeout 30 "$hull" logs -f dtest-adopt 2>&1)
[ "$got" = 0 ] && [ "$logs" = "before
after" ] || fail "a taken-back machine's exec exited $got, its log '$logs'" "$out"
cases=$((cases + 1))
out=$(timeout 30 "$hull" attach dtest-adopt 2>&1)
got=$?
[ "$got" = 5 ] || fail "a taken-back machine's exit was $got, wanted 5" "$out"
cases=$((cases + 1))
left=$(timeout 10 docker ps -a --filter name=hull-dtest-adopt- --format '{{.Names}}')
[ ! -e "$kept/dtest-adopt" ] && [ -z "$left" ] ||
	fail "a taken-back machine's file or container is left" "$(ls "$kept") $left"
# One whose container went while no core held it is forgotten, and said
# to be; one stopped is no longer kept.
keep_until_kept "$root/adopt.conf" "$root/adopt.out"
kill_core
timeout 10 docker rm -f "$(timeout 10 docker ps -aq --filter name=hull-dtest-adopt-)" > /dev/null 2>&1
cases=$((cases + 1))
out=$(timeout 30 "$hull" ps 2>&1)
! printf '%s\n' "$out" | grep -q '^dtest-adopt ' && [ ! -e "$kept/dtest-adopt" ] &&
	grep -q '^hull core: machine dtest-adopt is forgotten: the container is gone$' \
		"$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "a kept machine whose container went was not forgotten" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
# Nor is a container that is not hull's taken, whatever a file says: read
# by a core that starts once the last has left, 10 s at most.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
notours=$(timeout 10 docker create --name dtestx-notours alpine:latest true 2>&1)
printf '(4:kept(7:machine7:notours)(6:engine6:docker)(5:class2:ns)(5:state%d:%s))' \
	"${#notours}" "$notours" > "$kept/notours"
cases=$((cases + 1))
out=$(timeout 30 "$hull" ps 2>&1)
! printf '%s\n' "$out" | grep -q '^notours ' && [ ! -e "$kept/notours" ] &&
	grep -q '^hull core: machine notours is forgotten: the container is not hull.s$' \
		"$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "a container not hull's was taken back" "$notours; $out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
timeout 10 docker rm -f dtestx-notours > /dev/null 2>&1
keep_until_kept "$root/adopt.conf" "$root/adopt.out"
timeout 30 "$hull" stop --timeout 1 dtest-adopt > /dev/null 2>&1
wait "$runner"
cases=$((cases + 1))
[ ! -e "$kept/dtest-adopt" ] || fail "a kept machine stopped is still kept" "$(cat "$kept/dtest-adopt")"

# Secrets (J.3): read-only at /run/hull/secrets, as the user the container
# runs as, and one in the environment. The core that writes them needs a
# tmpfs, so it is one of their own in a runtime directory under /dev/shm,
# removed at the end once that core has left.
shm=$(mktemp -d /dev/shm/hull-docker-test.XXXXXX)
printf 'from a file\n' > "$root/pw.txt"
printf '(machine (source "alpine:latest") (backend docker) (hostname "dtest-secret") %s %s)\n' \
	'(command "sh" "-c" "cat /run/hull/secrets/pw; echo env=$made; stat -c %a /run/hull/secrets/pw; touch /run/hull/secrets/new 2>&1 || echo refused")' \
	'(secrets (pw (source "file:pw.txt")) (made (source "exec:printf made")  (expose env)))' \
	> "$root/secret.conf"
cases=$((cases + 1))
out=$(cd / && XDG_RUNTIME_DIR=$shm timeout 60 "$hull" run -f "$root/secret.conf" 2>&1)
got=$?
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -q '^from a file$' &&
	printf '%s\n' "$out" | grep -q '^env=made$' && printf '%s\n' "$out" | grep -q '^400$' &&
	printf '%s\n' "$out" | grep -q '^refused$' ||
	fail "a docker machine's secrets exited $got" "$out"
tries=0
while [ -S "$shm/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
left=$(ls -A "$shm/hull/secrets" 2>&1)
[ -z "$left" ] || fail "a docker machine's secrets were left" "$left"
rm -rf "${shm:?}"

# What cannot be created is 125, and says why. A command that is not
# there is docker's init failing to exec it, 127, as a shell would say.
printf '%s\n' '(machine (source "hull-no-such-image:0") (backend docker) (hostname "dtest") (command "x"))' \
	> "$root/noimage.conf"
expect 125 'create: .*hull-no-such-image' noimage
conf nocmd '(command "/nope")'
expect 127 'exec /nope failed' nocmd

# Nothing left on the daemon.
cases=$((cases + 1))
left=$(timeout 10 docker ps -a --filter name=hull-dtest- --format '{{.Names}}')
[ -z "$left" ] || fail "containers left behind" "$left"

# And the test's core leaves once nothing needs it.
cases=$((cases + 1))
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 80 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
[ ! -S "$XDG_RUNTIME_DIR/hull/core.sock" ] || fail "the core did not leave when idle" ""

printf 'docker: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
