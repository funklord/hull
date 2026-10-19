#!/bin/sh
# cli_test.sh -- build/hull against small files, held to its exit status
# and to a line of its output.
#
# Each command runs once, under `timeout 10`. The files it writes are the
# ones named in FILES, all in $BUILD_DIR/test/cli, which `make clean`
# removes by name. The first argument is the hull binary.

set -u

hull=$1
dir=${BUILD_DIR:?}/test/cli
mkdir -p "$dir"

# A core of the test's own: hull run starts one here, never in the
# account's own runtime directory, and it leaves a second after its last
# machine and client.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# expect STATUS PATTERN COMMAND...: exits STATUS, and some line of its
# stdout and stderr matches PATTERN (grep -E).
expect() {
	want=$1
	pattern=$2
	shift 2
	cases=$((cases + 1))
	out=$(timeout 10 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

# refute PATTERN COMMAND...: no line of its output matches PATTERN.
refute() {
	pattern=$1
	shift
	cases=$((cases + 1))
	out=$(timeout 10 "$@" 2>&1)
	if printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: a line matches /$pattern/" "$out"
	fi
}

printf '%s\n' '(machine (source "debian:13") (ports (port (host 8080) (guest 80))))' \
	> "$dir/ref.conf"
printf '%s\n' '(machine (source "dir:./rootfs"))' > "$dir/dir.conf"
printf '%s\n' '(machine (source "dir:.") (fidelity inotify))' > "$dir/inotify.conf"
printf '%s\n' '(machine (source "debian:13") (arch riscv64))' > "$dir/foreign.conf"
printf '%s\n' '(machines (web (source "debian:13")) (db (source "dir:./db")))' \
	> "$dir/machines.conf"
printf '%s\n' '(build (base "debian:13"))' > "$dir/build.conf"
printf '%s\n' '(machine (sauce "a"))' > "$dir/bad.conf"
# Section 4's example, as project.md shows it.
awk '/^## 4\. The spec/ { inside = 1 }
     inside && /^```scheme$/ { body = 1; next }
     body && /^```$/ { exit }
     body { print }' project.md > "$dir/example.conf"

# What it says about itself.
expect 0 '^hull [0-9]+\.[0-9]+$' "$hull" --version
expect 0 '^Copyright \(C\) 2026 Nabeel Sowan <nabeel@vibes\.se>$' "$hull" --version
expect 2 '^usage: hull plan' "$hull"
expect 2 '^usage: hull plan' "$hull" frobnicate
expect 0 '^docker: ns \(runs as root\)$' "$hull" engines
expect 0 '^qemu: vm emu$' "$hull" engines
expect 0 '^  fidelity\.inotify +rejected: .* \(--accept vm:inotify-poll\)$' "$hull" engines

# plan: the spec's own backend, J.1's ns where it names none.
expect 0 'ref\.conf: machine on docker \(ns\): ok$' "$hull" plan -f "$dir/ref.conf"
refute 'native' "$hull" plan -f "$dir/ref.conf"
expect 0 'source\.ref\.overlay: native$' "$hull" plan --explain -f "$dir/ref.conf"
# A field the engine does not declare is refused, not ignored: qemu runs
# no image yet.
expect 1 ':1:10: machine on qemu \(vm\): source\.ref\.overlay: rejected: the engine does not declare this field$' \
	"$hull" plan -f "$dir/ref.conf" --backend vm
expect 1 ':1:10: machine on docker \(ns\): source\.dir\.direct: rejected: docker cannot' \
	"$hull" plan -f "$dir/dir.conf"
expect 1 'machine on docker \(ns\): rejected, 1 field$' "$hull" plan -f "$dir/dir.conf"
expect 2 'plan takes one backend' "$hull" plan -f "$dir/ref.conf" --backend ns,vm
expect 0 '^\(machine \(source "debian:13"\) \(ports \(port \(host 8080\) \(guest 80\) \(proto tcp\)\)\) .*\(projection bind\)\)$' \
	"$hull" plan --resolved -f "$dir/ref.conf"
expect 0 '\(tuning \(vm \(profile micro\) \(kernel bundled\) \(cpu_profile host\)\) \(qemu \(accel auto\)\)\) \(projection passthrough\)\)$' \
	"$hull" plan --resolved -f "$dir/dir.conf" --backend vm
expect 0 'vm\.kernel\.bundled: emulated: the host.s own kernel and modules' \
	"$hull" plan -f "$dir/dir.conf" --backend vm

# --accept honours exactly the rejection that names it.
expect 1 'fidelity\.inotify: rejected: .* \(--accept vm:inotify-poll honours it\)$' \
	"$hull" plan -f "$dir/inotify.conf" --backend vm
expect 0 'fidelity\.inotify: accepted' \
	"$hull" plan -f "$dir/inotify.conf" --backend vm --accept vm:inotify-poll
expect 1 'source\.dir\.direct: rejected' \
	"$hull" plan -f "$dir/dir.conf" --accept vm:inotify-poll

# check: every backend listed, and the worst of them decides.
expect 1 'machine on qemu \(vm\): ok$' "$hull" check -f "$dir/dir.conf" --backend ns,vm
expect 1 'machine on docker \(ns\): rejected' "$hull" check -f "$dir/dir.conf" --backend ns,vm
expect 0 'machine on qemu \(emu\): ok$' "$hull" check -f "$dir/dir.conf" --backend vm,emu
expect 1 'machine on qemu \(vm\): arch: rejected: only the emu class' \
	"$hull" check -f "$dir/foreign.conf" --backend vm,emu
# A foreign arch is emu's to run, and qemu chooses emu for it; but booting
# the host's kernel, it runs the host's arch only so far.
expect 1 'machine on qemu \(emu\): arch: rejected: the engine does not run this arch$' \
	"$hull" check -f "$dir/foreign.conf" --backend vm,emu
expect 1 'machine on qemu \(emu\): arch: rejected' "$hull" plan -f "$dir/foreign.conf" --backend qemu
expect 2 'check needs --backend' "$hull" check -f "$dir/ref.conf"
expect 2 'lxc: no engine or class of that name' "$hull" check -f "$dir/ref.conf" --backend lxc

# Every machine of a multi-machine file, by name.
expect 1 'machines\.conf: web on docker \(ns\): ok$' "$hull" plan -f "$dir/machines.conf"
expect 1 'machines\.conf: db on docker \(ns\): rejected, 1 field$' \
	"$hull" plan -f "$dir/machines.conf"
expect 0 'no machine to plan, only a build$' "$hull" plan -f "$dir/build.conf"

# Files that cannot be planned at all.
expect 2 'bad\.conf:1:11: machine\.sauce: is not a key here$' "$hull" plan -f "$dir/bad.conf"
expect 2 'missing\.conf: No such file' "$hull" plan -f "$dir/missing.conf"

# Section 4's example is refused on every engine that runs. On ns it is
# refused for one field, (ssh true): no engine provisions sshd yet
# (section 8.1), and the docker adapter says so rather than run without
# one. On vm and emu it is two: its image source and ssh, neither of
# which the qemu engine does yet; its port is forwarded.
expect 1 'example\.conf: machine on qemu \(emu\): rejected, 2 fields$' \
	"$hull" check -f "$dir/example.conf" --backend vm,emu
expect 1 'example\.conf: machine on docker \(ns\): rejected, 1 field$' \
	"$hull" check -f "$dir/example.conf" --backend ns
expect 1 'example\.conf:[0-9]+:[0-9]+: machine on docker \(ns\): ssh: rejected' \
	"$hull" check -f "$dir/example.conf" --backend ns
expect 1 'example\.conf:[0-9]+:[0-9]+: machine on qemu \(vm\): virtualbox: ignored' \
	"$hull" check -f "$dir/example.conf" --backend vm

# hull run, on the reference engine: a rootfs holding only the probe, which
# make builds. Paths in a file are the file's, so these run from /.
root=$(cd "$dir" && pwd)
mkdir -p "$root/rootfs/srv"
conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "dir:./rootfs") (backend chroot) %s)\n' "$*" > "$root/$name.conf"
}
from_root() { # STATUS PATTERN ARGS...: hull run from /, so nothing is relative to here
	want=$1
	pattern=$2
	shift 2
	expect "$want" "$pattern" sh -c 'cd / && exec "$0" run "$@"' "$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")" "$@"
}

conf run '(command "/probe" "say" "out" "err" "to stderr" "env" "GREETING" "cwd" "host" "uid" "nnp" "session" "exit" "3")' \
	'(workdir "/srv") (hostname "box") (env (GREETING "hi"))'
from_root 3 '^out$' -f "$root/run.conf"
from_root 3 '^to stderr$' -f "$root/run.conf"
from_root 3 '^GREETING=hi$' -f "$root/run.conf"
from_root 3 '^cwd=/srv$' -f "$root/run.conf"
from_root 3 '^host=box$' -f "$root/run.conf"
from_root 3 '^uid=0$' -f "$root/run.conf"
from_root 3 '^nnp=1$' -f "$root/run.conf"
from_root 3 '^leader=1$' -f "$root/run.conf"
refute 'machine on chroot' sh -c 'cd / && exec "$0" run -f "$1" 2>/dev/null' \
	"$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")" "$root/run.conf"
refute '^to stderr$' sh -c 'cd / && exec "$0" run -f "$1" 2>/dev/null' \
	"$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")" "$root/run.conf"

# Written in place, or refused read-only.
rm -f "$root/rootfs/written"
conf writable '(command "/probe" "write" "/written")'
from_root 0 '^write=ok$' -f "$root/writable.conf"
cases=$((cases + 1))
[ -f "$root/rootfs/written" ] || fail "a direct write did not reach the directory" ""
rm -f "$root/rootfs/written"
conf readonly '(command "/probe" "write" "/written")' '(security (readonly_root true))'
from_root 0 '^write=Read-only file system$' -f "$root/readonly.conf"

# A signal is 128 and its number, from the workload or sent to hull. The
# workload has a session of its own, so timeout's signal to its process
# group reaches hull alone, and only hull's stop can end the probe.
conf die '(command "/probe" "die")'
from_root 143 '' -f "$root/die.conf"
conf wait '(command "/probe" "wait")'
cases=$((cases + 1))
out=$(timeout --preserve-status -s TERM 2 "$hull" run -f "$root/wait.conf" 2>&1)
got=$?
[ "$got" = 143 ] && printf '%s\n' "$out" | grep -q '^waiting$' ||
	fail "SIGTERM to hull run exited $got, wanted 143 after the probe said waiting" "$out"

# Volumes: a host dir bound in, written through or read-only, and its
# guest path resolved inside the machine. rootfs/mnt-link points at /mnt:
# resolved on the host it would reach the host's own /mnt, and the probe
# would find nothing there.
mkdir -p "$root/data" "$root/rootfs/mnt"
printf 'from the host\n' > "$root/data/in"
rm -f "$root/data/out" "$root/rootfs/mnt-link"
ln -s /mnt "$root/rootfs/mnt-link"
conf vol '(command "/probe" "read" "/mnt/in" "write" "/mnt/out")' \
	'(volumes (volume (host "./data") (guest "/mnt")))'
from_root 0 '^read=from the host$' -f "$root/vol.conf"
from_root 0 '^write=ok$' -f "$root/vol.conf"
cases=$((cases + 1))
[ -f "$root/data/out" ] || fail "a write through a volume did not reach the host" ""
conf volro '(command "/probe" "read" "/mnt/in" "write" "/mnt/out")' \
	'(volumes (volume (host "./data") (guest "/mnt") (mode ro)))'
from_root 0 '^write=Read-only file system$' -f "$root/volro.conf"
from_root 0 '^read=from the host$' -f "$root/volro.conf"
conf vollink '(command "/probe" "read" "/mnt/in")' \
	'(volumes (volume (host "./data") (guest "/mnt-link")))'
from_root 0 '^read=from the host$' -f "$root/vollink.conf"
conf volmissing '(command "/probe")' '(volumes (volume (host "./data") (guest "/absent")))'
from_root 125 'volume /absent: No such file or directory' -f "$root/volmissing.conf"
conf volhost '(command "/probe")' '(volumes (volume (host "./nodata") (guest "/mnt")))'
from_root 125 'volume /.*/test/cli/\./nodata: No such file or directory' -f "$root/volhost.conf"

# A PID namespace, its /proc, and an init that reaps what is orphaned to it:
# a zombie would still be listed, making three.
mkdir -p "$root/rootfs/proc"
conf pid '(command "/probe" "pid" "orphan" "procs")'
from_root 0 '^pid=2$' -f "$root/pid.conf"
from_root 0 '^procs=2$' -f "$root/pid.conf"

# The core: what it holds, what it kept, who can follow a machine, and
# what (on_disconnect ...) does when the frontend that ran it is killed.
hull_abs="$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")"
expect 0 '^box +chroot +ns +exited$' "$hull" ps
expect 0 '^out$' "$hull" logs box
expect 0 '^GREETING=hi$' "$hull" logs box
expect 1 'no such machine' "$hull" logs nobody
expect 1 'no such machine' "$hull" stop nobody

conf keep '(command "/probe" "say" "kept" "wait")' '(hostname "kept") (on_disconnect keep)'
conf gone '(command "/probe" "say" "gone" "wait")' '(hostname "gone")'
(cd / && exec "$hull_abs" run -f "$root/keep.conf" > /dev/null 2>&1) &
keeper=$!
(cd / && exec "$hull_abs" run -f "$root/gone.conf" > /dev/null 2>&1) &
goner=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^gone .*running$' &&
      "$hull" ps 2>/dev/null | grep -q '^kept .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 125 'a machine of that name is running' sh -c 'cd / && exec "$0" run -f "$1"' \
	"$hull_abs" "$root/keep.conf"
kill -KILL "$keeper" "$goner"
wait "$keeper" "$goner" 2>/dev/null
sleep 1
expect 0 '^kept +chroot +ns +running$' "$hull" ps
expect 0 '^gone +chroot +ns +exited$' "$hull" ps
expect 0 '^kept$' "$hull" logs kept
(timeout 30 "$hull" attach kept > "$root/attach.out" 2>&1; echo "status=$?" >> "$root/attach.out") &
attacher=$!
sleep 0.5
expect 0 '' "$hull" stop kept
wait "$attacher"
cases=$((cases + 1))
grep -q '^status=143$' "$root/attach.out" ||
	fail "attach did not end with the stopped machine's 143" "$(cat "$root/attach.out")"
expect 0 '^kept +chroot +ns +exited$' "$hull" ps

# exec: a command in the running machine, with its environment and
# workdir, this process's stdin, and its own status; it ends with the
# frontend that ran it, and an exited machine takes none.
conf execd '(command "/probe" "wait")' '(hostname "execd") (on_disconnect keep) (workdir "/srv")' \
	'(env (GREETING "hi"))'
(cd / && exec "$hull_abs" run -f "$root/execd.conf" > /dev/null 2>&1) &
execer=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^execd .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 '^host=execd$' "$hull" exec execd -- /probe host cwd env GREETING pid
expect 0 '^cwd=/srv$' "$hull" exec execd -- /probe cwd
expect 0 '^GREETING=hi$' "$hull" exec execd /probe env GREETING
# Small, and counted from 3 in the machine's own PID namespace, where a
# host pid would be neither.
expect 0 '^pid=([3-9]|[1-9][0-9])$' "$hull" exec execd /probe pid
expect 0 '^stdin=from the frontend$' sh -c 'echo from the frontend | "$0" exec execd /probe stdin' \
	"$hull_abs"
expect 0 '^stdin=\(none\)$' sh -c '"$0" exec execd /probe stdin < /dev/null' "$hull_abs"
expect 7 '' "$hull" exec execd /probe exit 7
# The machine's init holds nothing of the core's: no socket, and not its
# lock, which a machine outliving its core would otherwise keep held.
cases=$((cases + 1))
workload=$(ps -eo pid=,args= | awk '$2 == "/probe" && $3 == "wait" {print $1; exit}')
init=$(ps -o ppid= -p "$workload" | tr -d ' ')
held=$(ls -l "/proc/$init/fd" 2>/dev/null | grep -E 'socket:|core\.lock')
[ -n "$init" ] && [ -z "$held" ] || fail "the machine's init holds the core's descriptors" "$held"
# -t: a terminal of the size this one has, the command's controlling
# terminal, with stderr on it too; a resize reaches it as SIGWINCH. script
# gives the frontend a terminal, which the test does not have.
HULL=$hull_abs SHELL=/bin/sh
export HULL SHELL
expect 0 '^size=101x31' script -qec 'stty cols 101 rows 31; "$HULL" exec -t execd /probe tty' \
	/dev/null
expect 0 '^controlling=yes' script -qec '"$HULL" exec -t execd /probe tty' /dev/null
expect 0 '^merged' script -qec '"$HULL" exec -t execd /probe err merged 2> /dev/null' /dev/null
expect 0 '^size=90x20' script -qec '"$HULL" exec -t execd /probe winch < /dev/tty & sleep 1;
	stty cols 90 rows 20; kill -WINCH $!; wait $!' /dev/null
# The frontend's terminal is raw while it runs: Ctrl-C and Ctrl-D are
# bytes there, and SIGINT and the end of a read on the command's. Cooked,
# the Ctrl-D would end the frontend's input instead, which does not end a
# read on a terminal.
expect 0 'signal=2' sh -c '(sleep 1; printf "\003"; sleep 1) |
	script -qec "\"\$HULL\" exec -t execd /probe trap" /dev/null'
expect 0 '^stdin=\(none\)' sh -c '(sleep 1; printf "\004"; sleep 1) |
	script -qec "\"\$HULL\" exec -t execd /probe stdin" /dev/null'
expect 125 'needs a terminal' "$hull" exec -t execd /probe tty
# What the core refuses that no frontend of ours sends, sent as another
# frontend might, framed by hand: a 4-byte length, then the message. The
# reply is read until the text asked for, or the core closes.
speak='
import os, socket, struct, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(5)
# Through the directory, as hull itself does, where the path would be
# longer than a socket address holds.
d = os.open(os.environ["XDG_RUNTIME_DIR"] + "/hull", os.O_PATH)
s.connect("/proc/self/fd/%d/core.sock" % d)
got = b""
def say(m):
	s.sendall(struct.pack(">I", len(m)) + m)
def hear(text):
	global got
	while text not in got:
		chunk = s.recv(4096)
		if not chunk:
			break
		got += chunk
	print(repr(got))
say(b"(5:hello(2:ch1:0)(5:proto1:1))")
'
# A terminal's size is 1..65535, which the core holds to itself.
expect 0 'a window is 1 to 65535' python3 -c "$speak"'
say(b"(4:exec(4:argv6:/probe)(2:ch1:1)(7:machine5:execd)(3:tty(4:cols1:0)(4:rows2:24)))")
hear(b"6:reason")
'
# An exec answers started, and what is refused after that -- here a
# resize of a command with no terminal -- leaves it running.
expect 0 '7:started.*the command has no terminal' python3 -c "$speak"'
say(b"(4:exec(4:argv6:/probe4:wait)(2:ch1:1)(7:machine5:execd)(3:tty5:false))")
hear(b"7:started")
say(b"(5:winch(2:ch1:1)(4:cols2:80)(4:rows2:24))")
hear(b"6:reason")
'
expect 0 '^tty=no' "$hull" exec execd /probe tty
# A signal to the frontend is the command's: it caught this one and went
# on, and its status is what it made of it. Each frontend is in the
# background, outside expect's bound, so it has its own: timeout passes a
# TERM or HUP on to it, and a frontend that will not end on its TERM is
# killed two seconds after.
expect 0 '^signal=15$' sh -c 'timeout -k 2 8 "$0" exec execd /probe trap & sleep 1; kill -TERM $!;
	wait $!' "$hull_abs"
expect 0 '^signal=1$' sh -c 'timeout -k 2 8 "$0" exec execd /probe trap & sleep 1; kill -HUP $!; wait $!' \
	"$hull_abs"
expect 143 '' sh -c 'timeout -k 2 8 "$0" exec execd /probe wait & sleep 1; kill -TERM $!; wait $!' "$hull_abs"
expect 125 '/nope: No such file or directory' "$hull" exec execd /nope
expect 125 'no such machine' "$hull" exec nobody /probe
cases=$((cases + 1))
timeout -s KILL 1 "$hull" exec execd /probe say exec wait > /dev/null 2>&1
sleep 0.5
count=$(ps -eo args= | grep -c '^/probe ')
[ "$count" = 1 ] || fail "a command outlived the frontend that ran it" "$(ps -eo pid=,args= | awk '$2 == "/probe"')"
# A command ends with its machine, and its frontend hears so: 137, the
# SIGKILL it got as the helper went. The core first said nothing, and the
# frontend waited for good. timeout's own SIGKILL is 137 as well, ten
# seconds on, so the frontend must have ended within five of the stop.
cases=$((cases + 1))
timeout -k 2 8 "$hull" exec execd /probe wait > /dev/null 2>&1 &
ender=$!
sleep 1
stopped=$(date +%s)
"$hull" stop execd > /dev/null 2>&1
wait "$ender"
got=$?
took=$(($(date +%s) - stopped))
[ "$got" = 137 ] && [ "$took" -lt 5 ] ||
	fail "an exec in a stopped machine ended $got after ${took}s, wanted 137 at once" ""
wait "$execer" 2>/dev/null
expect 125 'the machine is not running' "$hull" exec execd /probe

# stdin's flow: the core holds 64 KiB of an exec's input and gives credit
# back as it reaches the command, so input many times that arrives whole
# and in order; a command that does not read for six seconds still gets
# all of it, where the core once refused it after five; and while one
# command is not reading, the core goes on serving everything else. The
# machine sleeps rather than waits, since these take longer than wait's
# twenty seconds.
conf flowd '(command "/probe" "sleep" "90")' '(hostname "flowd") (on_disconnect keep)'
(cd / && exec timeout -k 2 100 "$hull_abs" run -f "$root/flowd.conf" > /dev/null 2>&1) &
flower=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^flowd .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
want=$(seq 1 200000 | python3 -c 'import sys
h = 2166136261
data = sys.stdin.buffer.read()
for b in data:
	h = ((h ^ b) * 16777619) & 0xffffffff
print("bytes=%d fnv=%08x" % (len(data), h))')
expect 0 "^$want\$" sh -c 'seq 1 200000 | "$0" exec flowd /probe count' "$hull_abs"
expect 0 '^bytes=300000 ' sh -c 'head -c 300000 /dev/zero | "$0" exec flowd /probe sleep 6 count' \
	"$hull_abs"
cases=$((cases + 1))
(head -c 2000000 /dev/zero | timeout -k 2 12 "$hull" exec flowd /probe sleep 8 > /dev/null 2>&1) &
stuck=$!
sleep 1
started=$(date +%s)
out=$(timeout 10 "$hull" exec flowd /probe say free 2>&1)
took=$(($(date +%s) - started))
[ "$out" = free ] && [ "$took" -lt 2 ] ||
	fail "an exec beside one not reading took ${took}s" "$out"
wait "$stuck"
# A frontend killed while its command is not reading: the core ends the
# write under way, then the command, and goes on answering.
cases=$((cases + 1))
head -c 2000000 /dev/zero | timeout -s KILL 2 "$hull" exec flowd /probe sleep 30 > /dev/null 2>&1
sleep 1
out=$(timeout 3 "$hull" ps 2>&1)
count=$(ps -eo args= | grep -c '^/probe sleep 30')
printf '%s\n' "$out" | grep -q '^flowd ' && [ "$count" = 0 ] ||
	fail "a frontend killed with input pending left the core stuck or its command running" \
		"$out; $count left"
# The credit a frontend starts with, and more than that refused.
expect 0 '6:credit\(5:bytes5:65536\).*more input than its credit' python3 -c "$speak"'
say(b"(4:exec(4:argv6:/probe5:count)(2:ch1:1)(7:machine5:flowd)(3:tty5:false))")
hear(b"5:65536")
say(b"(4:data(3:buf65537:" + b"x" * 65537 + b")(2:ch1:1)(2:fd1:0))")
hear(b"6:reason")
'
# Messages of 10000 bytes, so that some straddle the end of the core's
# 64 KiB ring, which the frontend's 16 KiB reads never do; sent within the
# credit given, then compared with what the command counted.
expect 0 '^match$' python3 -c "$speak"'
import random, re
say(b"(4:exec(4:argv6:/probe5:count)(2:ch1:1)(7:machine5:flowd)(3:tty5:false))")
# Random, since a pattern repeating in a divisor of 64 KiB reads the same
# from a ring whose wrap is broken.
data = random.Random(1).randbytes(300000)
sent = 0
while sent < len(data):
	credit = sum(int(n) for n in re.findall(rb"\(6:credit\(5:bytes\d+:(\d+)\)", got))
	if credit - sent < 10000 and credit - sent < len(data) - sent:
		got += s.recv(4096)
		continue
	piece = data[sent:sent + 10000]
	say(b"(4:data(3:buf%d:" % len(piece) + piece + b")(2:ch1:1)(2:fd1:0))")
	sent += len(piece)
say(b"(3:eof(2:ch1:1))")
h = 2166136261
for b in data:
	h = ((h ^ b) * 16777619) & 0xffffffff
want = b"bytes=%d fnv=%08x" % (len(data), h)
while want not in got and b"exited" not in got:
	chunk = s.recv(4096)
	if not chunk:
		break
	got += chunk
print("match" if want in got else "mismatch")
'
# A frontend with no credit waits for it, rather than spinning on a stdin
# it cannot send: three seconds held back cost well under one of CPU.
expect 0 '^cpu=0\.' python3 -c '
import resource, subprocess, sys
subprocess.run(["sh", "-c", "head -c 2000000 /dev/zero | \"$0\" exec flowd /probe sleep 3 > /dev/null 2>&1",
               sys.argv[1]])
u = resource.getrusage(resource.RUSAGE_CHILDREN)
print("cpu=%.2f" % (u.ru_utime + u.ru_stime))
' "$hull_abs"

# Output's flow, the other way: the frontend gives credit back as it
# writes the output out, and the core holds a command's reader once 64 KiB
# is unasked for. So output many times that arrives whole, as the same
# probe on the host writes it; a reader seven seconds behind still gets
# all of it, where the core once stalled for five and dropped its
# frontend; meanwhile the command is held, not read into the core, and
# the core serves everything else.
probe=$dir/rootfs/probe
want=$("$probe" spew 3000000 | "$probe" count)
expect 0 "^$want\$" sh -c '"$0" exec flowd /probe spew 3000000 | "$1" count' "$hull_abs" "$probe"
cases=$((cases + 1))
(timeout -k 2 20 "$hull" exec flowd /probe spew 20000000 | (sleep 7; wc -c) > "$dir/held.out" 2>&1) &
held=$!
sleep 2
count=$(ps -eo args= | grep -c '^/probe spew 20000000')
started=$(date +%s)
out=$(timeout 10 "$hull" exec flowd /probe say free 2>&1)
took=$(($(date +%s) - started))
[ "$count" = 1 ] && [ "$out" = free ] && [ "$took" -lt 2 ] ||
	fail "a command whose reader is behind was not held, or the core with it" \
		"$count running; $out after ${took}s"
wait "$held"
expect 0 '^20000000$' cat "$dir/held.out"
# By hand: a frontend that gives no credit has at most the 64 KiB it
# started with, and as much again for crediting that; credit for more
# than it was sent is refused.
expect 0 '^first=6[0-9]{4} second=6[0-9]{4} refused$' python3 -c "$speak"'
import re
say(b"(4:exec(4:argv6:/probe4:spew7:1000000)(2:ch1:1)(7:machine5:flowd)(3:tty5:false))")
def output():
	total = at = 0
	while at + 4 <= len(got):
		n = struct.unpack(">I", got[at:at + 4])[0]
		if at + 4 + n > len(got):
			break
		m = re.match(rb"\(4:data\(3:buf(\d+):", got[at + 4:at + 4 + n])
		total += int(m.group(1)) if m else 0
		at += 4 + n
	return total
def soak(seconds):
	global got
	s.settimeout(seconds)
	try:
		while True:
			chunk = s.recv(65536)
			if not chunk:
				break
			got += chunk
	except socket.timeout:
		pass
soak(1.5)
first = output()
say(b"(6:credit(5:bytes%d:%d)(2:ch1:1))" % (len(str(first)), first))
soak(1.5)
second = output() - first
say(b"(6:credit(5:bytes10:1000000000)(2:ch1:1))")
soak(0.5)
print("first=%d second=%d %s" % (first, second,
      "refused" if b"more credit than it was sent" in got else "taken"))
'
timeout 10 "$hull" stop flowd > /dev/null 2>&1
wait "$flower" 2>/dev/null

# A machine's own output is read from its log by each follower at that
# follower's pace, and never holds the machine back. So a follower seven
# seconds behind a machine writing 20 MB costs the core nothing -- it once
# stalled for five seconds and dropped the follower -- and hears how many
# bytes left the log before it read them: those and the bytes it read are
# all 20 MB.
conf spewd '(command "/probe" "sleep" "2" "spew" "20000000" "sleep" "4")' '(hostname "spewd")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/spewd.conf" > /dev/null 2>&1) &
spewer=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^spewd .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
(timeout -k 2 25 "$hull" logs -f spewd 2> "$dir/lost.err" | (sleep 7; wc -c) > "$dir/behind.out") &
behind=$!
sleep 3
started=$(date +%s)
out=$(timeout 10 "$hull" ps 2>&1)
took=$(($(date +%s) - started))
printf '%s\n' "$out" | grep -q '^spewd ' && [ "$took" -lt 2 ] ||
	fail "the core stalled behind a follower that was not reading" "$out after ${took}s"
# A client held open, so that the core, idle once the machine has gone,
# does not leave before the cases below; killed after them.
(timeout -k 2 40 python3 -c "$speak"'
import time
time.sleep(35)
' > /dev/null 2>&1) &
holder=$!
wait "$behind"
wait "$spewer"
read_bytes=$(cat "$dir/behind.out")
# Told once each time it was lapped, which can be more than once.
lost=$(sed -n 's/^hull: \([0-9]*\) bytes of output lost: this reader fell behind$/\1/p' \
	"$dir/lost.err" | awk '{ total += $1 } END { if (NR > 0) print total }')
# What it read past the log's last MiB is what the sockets held when it
# fell behind, not a second log queued in the core.
[ -n "$lost" ] && [ "$lost" -gt 0 ] && [ $((read_bytes + lost)) = 20000000 ] &&
	[ "$read_bytes" -lt 2097152 ] ||
	fail "a follower behind read $read_bytes bytes and lost '$lost', of 20000000" \
		"$(cat "$dir/lost.err")"
# Twelve replays of the machine's full 1 MiB log, asked for before any is
# read: each is read from the log as the socket drains, rather than queued
# whole, which would be 12 MiB and drop the frontend.
expect 0 '^eofs=12 bytes=12[0-9]{6}$' python3 -c "$speak"'
import re
for ch in range(1, 13):
	say(b"(4:logs(2:ch%d:%d)(6:follow5:false)(7:machine5:spewd))" % (len(str(ch)), ch))
s.settimeout(10)
eofs = 0
tail = b""
while eofs < 12:
	chunk = s.recv(1 << 20)
	if not chunk:
		break
	got += chunk
	eofs += (tail + chunk).count(b"(3:eof")
	tail = chunk[-5:]
total = sum(int(n) for n in re.findall(rb"\(4:data\(3:buf(\d+):", got))
print("eofs=%d bytes=%d" % (eofs, total))
'
# A frontend that sends and never reads what it is answered is dropped once
# 8 MiB of answers wait for it, and nobody else waits meanwhile.
expect 0 '^dropped after [0-9]+ requests, and the core answers$' python3 -c "$speak"'
import subprocess
n = 0
try:
	while n < 200000:
		say(b"(4:list(2:ch1:6))")
		n += 1
	print("kept after %d requests" % n)
except (BrokenPipeError, ConnectionResetError):
	r = subprocess.run([sys.argv[1], "ps"], capture_output=True, timeout=5)
	print("dropped after %d requests, and the core %s" % (n,
	      "answers" if r.returncode == 0 else "does not"))
' "$hull_abs"
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null

# kill: a signal for the workload, which its init forwards. A TERM the
# probe catches is the probe's to handle, and it goes on to exit 0; a KILL,
# the default as in docker, ends the machine, 137. The run's output is in
# a file, and each signal waits for the probe's "ready", ten seconds at
# most, so it never lands before the probe has a handler.
conf killd '(command "/probe" "trap" "say" "after")' '(hostname "killd")'
await_ready() { # FILE
	tries=0
	until grep -q '^ready$' "$1" 2>/dev/null || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
}
cases=$((cases + 1))
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/killd.conf" > "$root/killd.out" 2>&1) &
killed=$!
await_ready "$root/killd.out"
expect 0 '' "$hull" kill -s sigterm killd
wait "$killed"
got=$?
[ "$got" = 0 ] && grep -q '^signal=15$' "$root/killd.out" && grep -q '^after$' "$root/killd.out" ||
	fail "a TERM to the workload: run exited $got, wanted 0 after it caught signal 15" \
		"$(cat "$root/killd.out")"
cases=$((cases + 1))
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/killd.conf" > "$root/killd.out" 2>&1) &
killed=$!
await_ready "$root/killd.out"
expect 0 '' "$hull" kill killd
wait "$killed"
got=$?
[ "$got" = 137 ] && ! grep -q '^signal=' "$root/killd.out" ||
	fail "a KILL to the workload: run exited $got, wanted 137" "$(cat "$root/killd.out")"
expect 1 'the machine is not running' "$hull" kill killd
expect 1 'no such machine' "$hull" kill nobody
expect 1 'not a signal the core forwards' "$hull" kill -s STOP killd
expect 2 'usage' "$hull" kill -s SIGTERMINATE killd

# One core: a second gives way to the first, says nothing, and leaves the
# first answering on its socket.
expect 0 '' "$hull" core
expect 0 '^kept +chroot +ns +exited$' "$hull" ps

# A machine dies with its core, even a kept one: nothing could manage it
# after. The probe is found by its argv[0], which only ours carries.
(cd / && exec "$hull_abs" run -f "$root/keep.conf" > /dev/null 2>&1) &
keeper=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^kept .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
if ! "$hull" ps 2>/dev/null | grep -q '^kept .*running$' || [ -z "$core" ]; then
	fail "the kept machine never ran, so its core's death could not be tried" \
		"$("$hull" ps 2>&1)"
else
	# Looked for before the client is reaped: without the parent-death
	# signal the probe was measured alive a second after the core, and
	# gone only once the client was reaped, for a reason not found
	# (project.md section 4.10).
	kill -KILL "$core"
	sleep 0.5
	left=$(ps -eo pid=,args= | awk '$2 == "/probe"')
	[ -z "$left" ] || fail "a machine outlived its core" "$left"
	wait "$keeper" 2>/dev/null
fi

# A core told to go stops its machines first, and their frontends hear
# how each ended: the probe's 143 for the TERM, not "the core went away".
cases=$((cases + 1))
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/keep.conf" > "$root/told.out" 2>&1) &
keeper=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^kept .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
[ -n "$core" ] && kill -TERM "$core"
wait "$keeper"
got=$?
[ "$got" = 143 ] || fail "a frontend whose core was told to go exited $got, wanted 143" \
	"core $core; $(cat "$root/told.out"); core.log: $(cat "$XDG_RUNTIME_DIR/hull/core.log")"

# A frontend that comes while a core is going reaches a new core: the
# going core has stopped listening, and the new one waits for its lock.
# The going core takes two seconds over its machine, which catches the
# TERM it is stopped with and sleeps.
conf slow '(command "/probe" "trap" "sleep" "2" "say" "slow")' '(hostname "slow")'
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/slow.conf" > "$root/slow.out" 2>&1) &
slow=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^slow .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
[ -n "$core" ] && kill -TERM "$core"
sleep 0.3
from_root 0 '^pid=2$' -f "$root/pid.conf"
wait "$slow"
cases=$((cases + 1))
grep -q '^slow$' "$root/slow.out" ||
	fail "the going core's machine did not end as it chose" "$(cat "$root/slow.out")"

# What cannot be planned or started is 125, and says why.
conf ports '(command "/probe") (ports (port (host 8080) (guest 80)))'
from_root 125 'ports: rejected: chroot shares the host.s network' -f "$root/ports.conf"
conf noargs '(workdir "/srv")'
from_root 125 'needs \(command \.\.\.\)' -f "$root/noargs.conf"
conf nope '(command "/nope")'
from_root 125 '/nope: No such file or directory' -f "$root/nope.conf"
printf '%s\n' '(machines (alpha (source "dir:./rootfs") (backend chroot) (command "/probe" "host")))' \
	> "$root/pair.conf"
from_root 0 '^host=alpha$' -f "$root/pair.conf" alpha
from_root 125 'name the machine to run' -f "$root/pair.conf"
from_root 125 'run takes one backend' -f "$root/pair.conf" --backend ns,vm alpha

# And the core leaves once nothing needs it.
cases=$((cases + 1))
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
[ ! -S "$XDG_RUNTIME_DIR/hull/core.sock" ] || fail "the core did not leave when idle" ""

printf 'cli: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
