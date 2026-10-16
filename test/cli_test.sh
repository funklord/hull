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
expect 0 ':1:31: machine on qemu \(vm\): ports: emulated: a host-side forwarder$' \
	"$hull" plan -f "$dir/ref.conf" --backend vm
expect 1 ':1:10: machine on docker \(ns\): source\.dir\.direct: rejected: docker cannot' \
	"$hull" plan -f "$dir/dir.conf"
expect 1 'machine on docker \(ns\): rejected, 1 field$' "$hull" plan -f "$dir/dir.conf"
expect 2 'plan takes one backend' "$hull" plan -f "$dir/ref.conf" --backend ns,vm
expect 0 '^\(machine \(source "debian:13"\) \(ports \(port \(host 8080\) \(guest 80\) \(proto tcp\)\)\) .*\(projection bind\)\)$' \
	"$hull" plan --resolved -f "$dir/ref.conf"
expect 0 '\(tuning \(vm \(profile micro\) \(kernel bundled\) \(cpu_profile host\)\) \(qemu \(accel auto\)\)\) \(projection passthrough\)\)$' \
	"$hull" plan --resolved -f "$dir/ref.conf" --backend vm

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
expect 1 'machine on qemu \(emu\): ok$' "$hull" check -f "$dir/foreign.conf" --backend vm,emu
expect 0 'machine on qemu \(emu\): ok$' "$hull" plan -f "$dir/foreign.conf" --backend qemu
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

# Section 4's example holds on vm and emu. On ns it is refused for one
# field, (ssh true): no engine provisions sshd yet (section 8.1), and the
# docker adapter, which runs, says so rather than run without one.
expect 0 'example\.conf: machine on qemu \(emu\): ok$' \
	"$hull" check -f "$dir/example.conf" --backend vm,emu
expect 1 'example\.conf: machine on docker \(ns\): rejected, 1 field$' \
	"$hull" check -f "$dir/example.conf" --backend ns
expect 1 'example\.conf:[0-9]+:[0-9]+: machine on docker \(ns\): ssh: rejected' \
	"$hull" check -f "$dir/example.conf" --backend ns
expect 0 'example\.conf:[0-9]+:[0-9]+: machine on qemu \(vm\): virtualbox: ignored' \
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

# What cannot be planned or started is 125, and says why.
conf ports '(command "/probe") (ports (port (host 8080) (guest 80)))'
from_root 125 'ports: rejected: chroot shares the host.s network' -f "$root/ports.conf"
conf noargs '(workdir "/srv")'
from_root 125 'needs \(command \.\.\.\)' -f "$root/noargs.conf"
conf nope '(command "/nope")'
from_root 125 '/nope: No such file or directory' -f "$root/nope.conf"
printf '%s\n' '(machine (source "dir:./rootfs") (backend qemu) (command "x"))' > "$root/qemu.conf"
from_root 125 'the qemu engine cannot run a machine yet' -f "$root/qemu.conf"
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
