#!/bin/sh
# docker_test.sh -- build/hull run on the docker adapter, against the
# Docker daemon this account can reach. Not part of `make check`, which
# needs no daemon: `make check-docker` runs it.
#
# Uses alpine:latest, which must be present already: nothing is pulled.
# Every machine is hostname dtest, so its container is hull-dtest-...,
# and the last case requires none of those to be left. Each command runs
# under `timeout`; the one loop, waiting for a port, gives up after 20
# tries. The files it writes are named in FILES, in $BUILD_DIR/test/docker.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/docker
mkdir -p "$dir/data"
root=$(cd "$dir" && pwd)
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
conf run '(command "sh" "-c" "echo out; echo to stderr >&2; echo GREETING=$GREETING; pwd; hostname; exit 3")' \
	'(workdir "/tmp") (env (GREETING "hi"))'
expect 3 '^out$' run
expect 3 '^to stderr$' run
expect 3 '^GREETING=hi$' run
expect 3 '^/tmp$' run
expect 3 '^dtest$' run
cases=$((cases + 1))
if (cd / && timeout 60 "$hull" run -f "$root/run.conf" 2>/dev/null) | grep -q '^to stderr$'; then
	fail "stderr reached stdout" ""
fi

# Volumes, read-write and read-only, and a read-only root. Only the
# reading of a read-write volume is tested: rootful docker writes as host
# root, which section 7's empty capabilities leave without the
# CAP_DAC_OVERRIDE a dir of this account's needs (project.md section 4.9).
conf vol '(command "cat" "/data/in")' '(volumes (volume (host "./data") (guest "/data")))'
expect 0 '^from the host$' vol
# Refused for being read-only, by name: a write into the read-write mount
# is refused too, for want of a capability, so "refused" alone tells the
# two apart not at all.
conf volro '(command "sh" "-c" "echo x > /data/out || true")' \
	'(volumes (volume (host "./data") (guest "/data") (mode ro)))'
expect 0 'Read-only file system' volro
conf rootro '(command "sh" "-c" "touch /x || true")' '(security (readonly_root true))'
expect 0 'Read-only file system' rootro

# Section 7: no capability unless granted.
conf nocap '(command "sh" "-c" "touch /f && chown 1 /f && echo chown=ok || echo chown=refused")'
expect 0 '^chown=refused$' nocap
conf cap '(command "sh" "-c" "touch /f && chown 1 /f && echo chown=ok || echo chown=refused")' \
	'(security (allow chown))'
expect 0 '^chown=ok$' cap

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

# A port bound on the host, reached from it.
conf port '(command "sh" "-c" "echo hello from the machine | nc -l -p 80")' \
	'(ports (port (host 18080) (guest 80)))'
cases=$((cases + 1))
(cd / && timeout 60 "$hull" run -f "$root/port.conf" > /dev/null 2>&1) &
server=$!
got=
tries=0
while [ -z "$got" ] && [ "$tries" -lt 20 ]; do
	tries=$((tries + 1))
	got=$(timeout 2 bash -c 'cat < /dev/tcp/127.0.0.1/18080' 2>/dev/null)
	[ -n "$got" ] || sleep 0.5
done
wait "$server"
[ "$got" = "hello from the machine" ] || fail "port 18080 answered '$got'" ""

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

printf 'docker: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
