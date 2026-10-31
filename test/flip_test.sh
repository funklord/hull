#!/bin/sh
# flip_test.sh -- Phase 1's exit (project.md section 12): one hull.conf
# flips between docker and qemu, container to VM to emulation, and
# deploys as a plain rsync'd directory; and since 2026-10-31 Phase 4's,
# the same file on chroot, the native engine, held to the same, and
# chroot, vm and emu held with Docker absent. Not part of `make check`,
# which needs no KVM: `make check-flip` runs it.
#
# Its arguments are build/hull, the static probe and build/test/tree_tar.
# A project directory -- the file, a rootfs holding only the probe, and a
# data directory -- is made in $BUILD_DIR/test/flip/project, copied with
# `rsync -a` to deployed/, and run from there unchanged with --backend
# chroot, vm and emu, then docker where a daemon answers. The first three
# run on a core of their own whose DOCKER_HOST names no socket, as a host
# without Docker installed is, which a docker run on that core failing
# shows. The file names no backend. Each run serves a port,
# which a client on the host reaches on the machine's address, the same
# on every engine (section 8.1).
#
# Each run and each client is under `timeout`; the client's one loop
# gives up after 90 seconds. docker's import of the rootfs is removed by
# the name the test works out, unless it was there before. The files it
# writes are named in FILES, in $BUILD_DIR/test/flip.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
tree_tar=$3
dir=${BUILD_DIR:?}/test/flip
# A core of the test's own, as in cli_test.sh.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR" "$dir/project/rootfs/proc" "$dir/project/rootfs/dev" \
	"$dir/project/rootfs/sys" "$dir/project/rootfs/srv" "$dir/project/rootfs/etc" \
	"$dir/project/data"
chmod 700 "$XDG_RUNTIME_DIR"
root=$(cd "$dir" && pwd)
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'flip: needs /dev/kvm and /dev/vhost-vsock open to this user\n'
	exit 1
fi
# docker is run where its daemon answers and has alpine, which it does
# not pull; the other three are held without it either way.
docker_up=0
timeout 10 docker image inspect alpine:latest > /dev/null 2>&1 && docker_up=1

# A port free on the host now, for the machine's.
port=$(python3 -c '
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])')
# Section 8.1's address for the id flip, worked here as in qemu_test.sh.
address=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' flip)

cp "$probe" "$root/project/rootfs/probe"
printf 'from the directory\n' > "$root/project/rootfs/etc/flip"
# docker runs the workload as the user, so the place it writes in the
# machine is the user's.
chmod 755 "$root/project/rootfs/srv"
# The user's own group, not setgid: a tree here inherits a group that is
# not the user's primary one, which a chroot machine maps only where
# /etc/subgid grants it (project.md section 7), and this checks engines,
# not the host's groups.
chgrp -R "$(id -g)" "$root/project"
chmod -R g-s "$root/project"
printf '%s\n' \
	'(machine (source "dir:./rootfs") (mutable overlay) (hostname "flip")' \
	'  (workdir "/srv") (env (WHO "flip"))' \
	'  (volumes (volume (host "./data") (guest "/data")))' \
	"  (ports (port (host $port) (guest 8080)))" \
	'  (command "/probe" "env" "WHO" "cwd" "host" "read" "/etc/flip"' \
	'           "write" "/srv/flipped" "read" "/srv/flipped" "write" "/data/out"' \
	'           "serve" "tcp" "8080" "exit" "7"))' > "$root/project/hull.conf"

# Deployed as a plain directory: rsync, nothing else.
rm -rf -- "${root:?}/deployed"
timeout 60 rsync -a "$root/project/" "$root/deployed/"
deployed=$root/deployed
before=$(timeout 60 "$tree_tar" "$deployed/rootfs" | sha256sum | cut -c1-64)
had_image=0
[ "$docker_up" = 1 ] &&
	timeout 10 docker image inspect "hull-dir:$before" > /dev/null 2>&1 && had_image=1

# flip BACKEND: run the deployed file on BACKEND while a client on the
# host takes the port at the machine's address; the machine's output in
# $out, its status in $got, and what the client heard in $heard.
flip() {
	rm -f "$deployed/data/out"
	(cd / && exec timeout -k 2 120 "$hull" run -f "$deployed/hull.conf" --backend "$1" \
		> "$root/$1.out" 2>&1) &
	runner=$!
	# A connection the machine is not yet listening for reaches the
	# client as a close, so it tries again until it is greeted.
	heard=$(timeout 100 python3 -c '
import socket, sys, time
end = time.time() + 90
while time.time() < end:
	try:
		c = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
		line = c.makefile().readline()
		if line:
			c.sendall(b"from the host\n")
			print(line.strip())
			c.close()
			sys.exit(0)
		c.close()
	except OSError:
		pass
	time.sleep(0.25)
print("nobody greeted")' "$address" "$port" 2>&1)
	wait "$runner"
	got=$?
	out=$(cat "$root/$1.out")
}

# Docker made absent: the core these runs start has a runtime directory
# of its own, so it is not one already serving, and no daemon to reach.
own_runtime=$XDG_RUNTIME_DIR
own_host=${DOCKER_HOST-}
XDG_RUNTIME_DIR=$root/run
DOCKER_HOST=unix://$root/no-docker.sock
export DOCKER_HOST
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# That it is absent: the same file on docker fails on that core.
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$deployed/hull.conf" --backend docker \
	> "$root/nodocker.out" 2>&1)
got=$?
[ "$got" != 0 ] && [ "$got" != 7 ] && [ "$got" != 124 ] && [ ! -e "$deployed/data/out" ] ||
	fail "docker was not absent: a run on it exited $got" "$(cat "$root/nodocker.out")"

backends="chroot vm emu"
[ "$docker_up" = 1 ] && backends="$backends docker"
for backend in $backends; do
	# docker on the daemon this account reaches, from the test's own core.
	if [ "$backend" = docker ]; then
		XDG_RUNTIME_DIR=$own_runtime
		if [ -n "$own_host" ]; then DOCKER_HOST=$own_host; else unset DOCKER_HOST; fi
	fi
	flip "$backend"
	eval "out_$backend=\$out"
	cases=$((cases + 1))
	[ "$got" = 7 ] || fail "$backend exited $got, wanted 7" "$out"
	cases=$((cases + 1))
	[ "$heard" = "hello from the machine" ] ||
		fail "$backend: the port on $address:$port was not the machine's" "$heard"
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^served=from the host$' ||
		fail "$backend: the machine did not hear the client" "$out"
	# The volume is the host's directory, written as the user's.
	cases=$((cases + 1))
	[ -f "$deployed/data/out" ] && [ "$(stat -c %u "$deployed/data/out")" = "$(id -u)" ] ||
		fail "$backend: a write through the volume did not land as the user's" \
			"$(ls -ln "$deployed/data" 2>&1)"
done

# The machine's own lines, without the plan's notes, which name the
# engine: the same on every one.
own() { printf '%s\n' "$1" | grep -v ': machine on '; }
cases=$((cases + 1))
want=$(printf '%s\n' 'WHO=flip' 'cwd=/srv' 'host=flip' 'read=from the directory' \
	'write=ok' 'read=Success' 'write=ok' 'serving' 'served=from the host')
[ "$(own "$out_chroot")" = "$want" ] ||
	fail "the chroot machine saw otherwise" "$(own "$out_chroot")"
cases=$((cases + 1))
[ "$(own "$out_vm")" = "$(own "$out_chroot")" ] ||
	fail "the vm's machine differs from chroot's" "$(own "$out_vm")"
cases=$((cases + 1))
[ "$(own "$out_emu")" = "$(own "$out_chroot")" ] ||
	fail "the emu machine differs from chroot's" "$(own "$out_emu")"
if [ "$docker_up" = 1 ]; then
	cases=$((cases + 1))
	[ "$(own "$out_docker")" = "$(own "$out_chroot")" ] ||
		fail "docker's machine differs from chroot's" "$(own "$out_docker")"
fi
# Each said which engine and class it was.
cases=$((cases + 1))
printf '%s\n' "$out_chroot" | grep -q 'machine on chroot (ns)' &&
	printf '%s\n' "$out_vm" | grep -q 'machine on qemu (vm)' &&
	printf '%s\n' "$out_emu" | grep -q 'machine on qemu (emu)' &&
	{ [ "$docker_up" = 0 ] || printf '%s\n' "$out_docker" | grep -q 'machine on docker (ns)'; } ||
	fail "a run did not say its engine" "$out_chroot
$out_vm
$out_emu
${out_docker-}"

# The overlay kept every write in the machine: the deployed tree is the
# same bytes as before the runs.
cases=$((cases + 1))
after=$(timeout 60 "$tree_tar" "$deployed/rootfs" | sha256sum | cut -c1-64)
[ "$after" = "$before" ] && [ ! -e "$deployed/rootfs/srv/flipped" ] ||
	fail "the runs changed the deployed directory" "$before then $after"

if [ "$docker_up" = 1 ]; then
	[ "$had_image" = 1 ] || timeout 30 docker rmi "hull-dir:$before" > /dev/null 2>&1
	cases=$((cases + 1))
	left=$(timeout 10 docker ps -a --filter name=hull-flip- --format '{{.Names}}' 2>&1)
	[ -z "$left" ] || fail "a container of this suite's was left" "$left"
else
	printf 'flip: no Docker daemon with alpine here: docker not run\n'
fi

printf 'flip: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
