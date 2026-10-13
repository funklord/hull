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

# Section 4's example holds on every class, which is the point of it.
expect 0 'example\.conf: machine on docker \(ns\): ok$' \
	"$hull" check -f "$dir/example.conf" --backend ns,vm,emu
expect 0 'example\.conf: machine on qemu \(emu\): ok$' \
	"$hull" check -f "$dir/example.conf" --backend ns,vm,emu
expect 0 'example\.conf:[0-9]+:[0-9]+: machine on qemu \(vm\): virtualbox: ignored' \
	"$hull" check -f "$dir/example.conf" --backend vm

printf 'cli: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
