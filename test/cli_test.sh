#!/bin/sh
# cli_test.sh -- build/hull against small files, held to its exit status
# and to a line of its output.
#
# Each command runs once, under `timeout 10`. The files it writes are the
# ones named in FILES, all in $BUILD_DIR/test/cli, which `make clean`
# removes by name, but for the secrets cases' runtime directory, which
# must be a tmpfs: one mktemp makes under /dev/shm, removed at the exit.
# The first argument is the hull binary.

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
# No daemon: the core this suite starts reaches no docker, so that its gc
# looks at nothing of the account's.
DOCKER_HOST=unix://$XDG_RUNTIME_DIR/no-docker.sock
export DOCKER_HOST
# And a store of its own, which gc would otherwise look in, empty at the
# start: what a run that went left would change what gc says it removed.
XDG_DATA_HOME=$(cd "${BUILD_DIR:?}" && pwd)/test/cli-data
export XDG_DATA_HOME
case $XDG_DATA_HOME in
*/test/cli-data) ;;
*) echo "cli_test: $XDG_DATA_HOME is not a test/cli-data directory" >&2; exit 2 ;;
esac
[ ! -e "$XDG_DATA_HOME" ] || chmod -R u+w "${XDG_DATA_HOME:?}"
rm -rf "${XDG_DATA_HOME:?}"
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
	out=$(timeout "${limit:-10}" "$@" 2>&1)
	got=$?
	limit=10
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
# The commit it was built from, on a line of its own after the first two,
# so a result can name the hull it rested on.
expect 0 '^commit ([0-9a-f]{12}(-dirty)?|unknown)$' "$hull" --version
cases=$((cases + 1))
[ "$(timeout 10 "$hull" --version | head -1 | grep -cE '^hull [0-9]+\.[0-9]+$')" = 1 ] ||
	fail "--version's first line lost its shape" "$(timeout 10 "$hull" --version)"
expect 2 '^usage: hull plan' "$hull"
expect 2 '^usage: hull plan' "$hull" frobnicate
expect 0 '^docker: ns \(runs as root\)$' "$hull" engines
expect 0 '^qemu: vm emu$' "$hull" engines
# hull systemd: a user service unit for a machine (section 9). Restart= is
# the spec's (restart ...), a readable symbol the engine's atom accessor
# would miss, so a non-default value is the one to check; the binary and
# conf are absolute, for a boot with neither this PATH nor cwd; a
# (machines ...) file needs the machine named.
printf '%s\n' '(machine (hostname "web") (source "dir:./rootfs") (restart always))' > "$dir/sdunit.conf"
expect 0 '^Restart=always$' "$hull" systemd -f "$dir/sdunit.conf"
expect 0 '^ExecStart=/.*/hull run -f /.*/sdunit\.conf$' "$hull" systemd -f "$dir/sdunit.conf"
expect 0 '^Description=hull machine web$' "$hull" systemd -f "$dir/sdunit.conf"
expect 0 '^WantedBy=default\.target$' "$hull" systemd -f "$dir/sdunit.conf"
printf '%s\n' '(machine (source "dir:./rootfs"))' > "$dir/sdnore.conf"
expect 0 '^Restart=no$' "$hull" systemd -f "$dir/sdnore.conf"
printf '%s\n' '(machines (web (source "dir:./w")) (db (source "dir:./d") (restart on-failure)))' > "$dir/sdmulti.conf"
expect 0 '^ExecStart=/.*/hull run -f /.*/sdmulti\.conf db$' "$hull" systemd -f "$dir/sdmulti.conf" db
expect 0 '^Restart=on-failure$' "$hull" systemd -f "$dir/sdmulti.conf" db
expect 125 'name the machine' "$hull" systemd -f "$dir/sdmulti.conf"
expect 125 'no such machine' "$hull" systemd -f "$dir/sdmulti.conf" nope
# hull import: a Dockerfile to a (build ...) form (section 9), best-effort,
# annotating in `;` comments what it cannot translate faithfully. The
# result must stay a valid spec: a trailing annotation must not comment out
# a closing paren, so it is PARSED here (hull plan on a build-only file
# parses it, then says there is only a build), not just grepped.
cat > "$dir/Dockerfile.in" <<'DF'
FROM debian:13-slim AS base
LABEL x=y
ENV A=1 B=2
WORKDIR /srv
RUN apt-get update && \
    apt-get install -y curl
COPY ./app /srv/app
ADD x.tar.gz /srv/
EXPOSE 80 53/udp
USER nobody
ENTRYPOINT ["/srv/app"]
CMD echo hi
HEALTHCHECK CMD true
DF
"$hull" import "$dir/Dockerfile.in" > "$dir/imported.build" 2>&1
expect 0 'no machine to plan, only a build' "$hull" plan -f "$dir/imported.build"
gb() {
	cases=$((cases + 1))
	grep -Eq -- "$1" "$dir/imported.build" ||
		fail "import: no line matches /$1/" "$(cat "$dir/imported.build")"
}
gb '\(base "debian:13-slim"\)'
gb '\(run "apt-get update'
gb '\(env \(A "1"\) \(B "2"\)\)'
gb '\(workdir "/srv"\)'
gb '\(ports "80/tcp" "53/udp"\)'
gb '\(copy \(path \(from "\./app"\) \(to "/srv/app"\)\)\)'
gb '\(entrypoint "/srv/app"\)'
gb '\(cmd "/bin/sh" "-c" "echo hi"\)'
gb '; not translated: LABEL'
gb '; ADD treated as COPY'
gb '; not translated: HEALTHCHECK'
printf 'RUN true\n' > "$dir/nofrom.in"
expect 2 'has no FROM' "$hull" import "$dir/nofrom.in"
# hull import of a compose file (by its .yml name) to a (machines ...) form.
# J.7's machines share one segment and section 4.8 plans them at once with
# no ordering, so depends_on and networks are noted, not invented.
cat > "$dir/docker-compose.yml" <<'YML'
version: "3.8"
services:
  web:
    image: nginx:1.27
    ports:
      - "8080:80"
      - "53:53/udp"
    environment:
      - FOO=bar
    volumes:
      - ./site:/usr/share/nginx/html:ro
    depends_on:
      - db
    restart: unless-stopped
  db:
    image: postgres:16
    container_name: thedb
    command: ["postgres", "-c", "max_connections=50"]
    build: ./db
    networks: [backend]
  cache:
    image: redis:7
    network_mode: bridge
  agent:
    image: busybox:1.36
    network_mode: host
    ports:
      - "9100:9100"
  sealed:
    image: busybox:1.36
    network_mode: none
  sidecar:
    image: busybox:1.36
    network_mode: "service:web"
networks:
  backend:
YML
"$hull" import "$dir/docker-compose.yml" > "$dir/compose.machines" 2>&1
# The output parses: hull plan reads the multi-machine file and reaches
# per-machine planning (a syntax error would not); the machines are rejected
# only for bare refs not in the store, which is proof it parsed.
cases=$((cases + 1))
"$hull" plan -f "$dir/compose.machines" --backend chroot 2>&1 | grep -q 'web on' ||
	fail "import: the compose output did not parse" "$(cat "$dir/compose.machines")"
gbc() {
	cases=$((cases + 1))
	grep -Eq -- "$1" "$dir/compose.machines" ||
		fail "import compose: no line matches /$1/" "$(cat "$dir/compose.machines")"
}
gbc '\(web \(source "nginx:1.27"\)'
gbc '\(port \(host 8080\) \(guest 80\)\)'
gbc '\(guest 53\) \(proto udp\)\)'
gbc '\(env \(FOO "bar"\)\)'
gbc '\(volume \(host "\./site"\) \(guest "/usr/share/nginx/html"\) \(mode ro\)\)'
gbc '\(command "postgres" "-c" "max_connections=50"\)'
gbc '\(hostname "thedb"\)'
gbc '\(restart always\)'
gbc '; depends_on: db'
gbc '; build not translated'
gbc '; top-level key not translated: networks'
# network_mode: bridge, docker's own network, is hull's nat; host and none
# are hull's as they are, a port under either dropped and said, as docker
# discards or refuses it; another container's network is noted.
gbc '\(cache \(source "redis:7"\) \(network nat\)'
gbc '; network_mode bridge mapped to nat'
gbc '\(agent \(source "busybox:1\.36"\) \(network host\)'
gbc '; ports not translated: network_mode host is the host.s network'
gbc '\(sealed \(source "busybox:1\.36"\) \(network none\)'
gbc '; network_mode not translated: service:web'
cases=$((cases + 1))
! grep -A3 '^  (agent ' "$dir/compose.machines" | grep -q '(ports' ||
	fail "import compose: a port was kept under network_mode host" "$(cat "$dir/compose.machines")"
printf 'version: "3"\n' > "$dir/noservices.yml"
expect 2 'no services' "$hull" import "$dir/noservices.yml"

# hull doctor: every line a check, its status first, or the fix under one.
cases=$((cases + 1))
out=$(timeout 20 "$hull" doctor 2>&1)
got=$?
bad=$(printf '%s\n' "$out" | grep -Ev '^(ok|missing|limited|absent) +[a-zA-Z/]|^  fix \(as root\): .')
[ "$got" = 0 ] && [ -z "$bad" ] && [ -n "$out" ] ||
	fail "hull doctor exited $got, or a line is not a check's" "$out"
expect 0 '^ok +user namespaces$' "$hull" doctor
# A tool is found on PATH, and said where.
mkdir -p "$dir/tools"
printf '#!/bin/sh\n' > "$dir/tools/squashfuse"
chmod 755 "$dir/tools/squashfuse"
expect 0 "^ok +squashfuse: a squashfs image read through FUSE \\($dir/tools/squashfuse\\)\$" \
	env PATH="$dir/tools:$PATH" "$hull" doctor
# Where user namespaces cannot be had, which every rootless engine needs:
# 1, and the line that gives them. The limit is the namespace's own.
expect 1 '^missing +user namespaces: ' unshare -r sh -c \
	'echo 0 > /proc/sys/user/max_user_namespaces && exec "$1" doctor' sh "$hull"
expect 1 '^  fix \(as root\): sysctl user\.max_user_namespaces=15000$' unshare -r sh -c \
	'echo 0 > /proc/sys/user/max_user_namespaces && exec "$1" doctor' sh "$hull"
# With no XDG_RUNTIME_DIR, what the frontend chose, in a namespace whose
# /run/user, /dev/shm and /tmp are its own, so the account's are not
# seen: a login's, else /dev/shm's in memory, else /tmp's on a disk;
# and where none will do, 1 and each reason.
# doctor_in, run as `sh -c "$doctor_in" sh HULL SETUP`: hull doctor
# there, after the shell text SETUP. Hull is reached through a descriptor
# opened before, since a tree under /tmp, as a scratch copy's is, is
# hidden by the tmpfs. `on_disk D` in SETUP mounts an overlay over D,
# which is not in memory whatever is under it.
doctor_in='
exec 3< "$(dirname "$1")" || exit 99
for d in /run/user /dev/shm /tmp; do mount -t tmpfs none "$d" || exit 99; done
on_disk() {
	mkdir -p "/run/user/layers$1/l" "/run/user/layers$1/u" "/run/user/layers$1/w" &&
	mount -t overlay none -o "lowerdir=/run/user/layers$1/l,upperdir=/run/user/layers$1/u,workdir=/run/user/layers$1/w" "$1"
}
unset XDG_RUNTIME_DIR
eval "$2" || exit 99
exec /proc/self/fd/3/hull doctor'
expect 0 '^ok +runtime directory: /run/user/0$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" 'mkdir -m 700 /run/user/0'
expect 0 '^ok +runtime directory: /dev/shm/hull-0$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" ''
expect 0 '^ok +runtime directory: /dev/shm/hull-0$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" 'mkdir -m 755 /run/user/0'
# An empty one is none.
expect 0 '^ok +runtime directory: /dev/shm/hull-0$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" 'export XDG_RUNTIME_DIR='
expect 0 '^limited +runtime directory: /tmp/hull-0, which is not in memory, so a machine.s secrets are refused$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" 'on_disk /dev/shm && on_disk /tmp'
expect 1 '^missing +runtime directory: none would do: /tmp/hull-0: Read-only file system$' \
	unshare -rm sh -c "$doctor_in" sh "$hull" 'mount -t tmpfs -o ro none /dev/shm && mount -t tmpfs -o ro none /tmp'
# gc starts a core for itself, which asks each engine that keeps anything:
# docker, which says it cannot look, and that is not gc failing.
expect 0 '^docker: the Docker daemon at .*/no-docker\.sock cannot be reached$' "$hull" gc
expect 0 '^  fidelity\.inotify +rejected: .* \(--accept vm:inotify-poll\)$' "$hull" engines

# plan: the spec's own backend, J.1's ns where it names none, whose
# engine is chroot, hull's own and rootless, rather than docker, which
# would reach for root first (section 7). An image not in the store is
# refused there with what to do about it; docker takes it as it is.
expect 0 'dir\.conf: machine on chroot \(ns\): ok$' "$hull" plan -f "$dir/dir.conf"
expect 1 'machine on chroot \(ns\): source\.ref\.overlay: rejected: an image runs from the store, and this one is not in it: .hull pull. it first$' \
	"$hull" plan -f "$dir/ref.conf"
expect 0 'ref\.conf: machine on docker \(ns\): ok$' "$hull" plan -f "$dir/ref.conf" --backend docker
refute 'native' "$hull" plan -f "$dir/ref.conf" --backend docker
expect 0 'source\.ref\.overlay: native$' "$hull" plan --explain -f "$dir/ref.conf" --backend docker
# A field the engine does not declare is refused, not ignored: qemu runs
# no image yet.
expect 1 ':1:10: machine on qemu \(vm\): source\.ref\.overlay: rejected: the engine does not declare this field$' \
	"$hull" plan -f "$dir/ref.conf" --backend vm
expect 1 ':1:10: machine on docker \(ns\): source\.dir\.direct: rejected: docker cannot' \
	"$hull" plan -f "$dir/dir.conf" --backend docker
expect 1 'machine on docker \(ns\): rejected, 1 field$' "$hull" plan -f "$dir/dir.conf" --backend docker
# Segments are qemu's and chroot's: docker refuses them, and says why.
printf '%s\n' '(machine (source "dir:./rootfs") (segments "wire"))' > "$dir/segmented.conf"
expect 0 'segments: native: a tap each' \
	"$hull" plan --explain -f "$dir/segmented.conf" --backend chroot
expect 1 'segments: rejected: a container.s NIC is the daemon.s, never on the core.s switch' \
	"$hull" plan -f "$dir/segmented.conf" --backend docker
expect 0 'machine on qemu \(vm\): ok|segments: native' \
	"$hull" plan --explain -f "$dir/segmented.conf" --backend vm
# Handing PID 1 to the image's init is chroot's and qemu's, not docker's.
printf '%s\n' '(machine (source "dir:./rootfs") (init "/sbin/init"))' > "$dir/inited.conf"
expect 0 'init: native: the image.s init is PID 1, the engine.s a child of it' \
	"$hull" plan --explain -f "$dir/inited.conf" --backend chroot
expect 1 'init: rejected: not yet: a container.s PID 1 is docker.s init' \
	"$hull" plan -f "$dir/inited.conf" --backend docker
expect 2 'plan takes one backend' "$hull" plan -f "$dir/ref.conf" --backend ns,vm
expect 0 '^\(machine \(source "debian:13"\) \(ports \(port \(host 8080\) \(guest 80\) \(proto tcp\)\)\) .*\(projection bind\)\)$' \
	"$hull" plan --resolved -f "$dir/ref.conf" --backend docker
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
	"$hull" plan -f "$dir/dir.conf" --backend docker --accept vm:inotify-poll

# check: every backend listed, and the worst of them decides.
expect 1 'machine on qemu \(vm\): ok$' "$hull" check -f "$dir/dir.conf" --backend docker,vm
expect 1 'machine on docker \(ns\): rejected' "$hull" check -f "$dir/dir.conf" --backend docker,vm
expect 0 'machine on chroot \(ns\): ok$' "$hull" check -f "$dir/dir.conf" --backend ns,vm
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
expect 1 'machines\.conf: web on docker \(ns\): ok$' "$hull" plan -f "$dir/machines.conf" --backend docker
expect 1 'machines\.conf: db on docker \(ns\): rejected, 1 field$' \
	"$hull" plan -f "$dir/machines.conf" --backend docker
expect 0 'no machine to plan, only a build$' "$hull" plan -f "$dir/build.conf"

# Files that cannot be planned at all.
expect 2 'bad\.conf:1:11: machine\.sauce: is not a key here$' "$hull" plan -f "$dir/bad.conf"
expect 2 'missing\.conf: No such file' "$hull" plan -f "$dir/missing.conf"

# Section 4's example is refused on every engine that runs. On ns it is
# refused for one field, (ssh true): the docker adapter provisions no
# sshd, and says so rather than run without one (section 8.1). On vm and
# emu it is one field, its image source: the qemu engine provisions
# (ssh true) from the image's own sshd since 2026-10-25, and its port is
# forwarded.
expect 1 'example\.conf: machine on qemu \(emu\): rejected, 1 field$' \
	"$hull" check -f "$dir/example.conf" --backend vm,emu
expect 1 'example\.conf: machine on docker \(ns\): rejected, 1 field$' \
	"$hull" check -f "$dir/example.conf" --backend docker
expect 1 'example\.conf:[0-9]+:[0-9]+: machine on docker \(ns\): ssh: rejected' \
	"$hull" check -f "$dir/example.conf" --backend docker
expect 1 'example\.conf:[0-9]+:[0-9]+: machine on qemu \(vm\): virtualbox: ignored' \
	"$hull" check -f "$dir/example.conf" --backend vm

# hull run, on the reference engine: a rootfs holding only the probe, which
# make builds. Paths in a file are the file's, so these run from /.
root=$(cd "$dir" && pwd)
mkdir -p "$root/rootfs/srv"
# A HOME of the test's own, never the account's: the core reads the user's
# ~/.ssh public keys for (ssh true)'s authorized_keys (section 8.1), and it
# must read a known, benign key rather than whatever the account has. Set
# before the first run below, which starts the core that reads it.
export HOME="$root/home"
mkdir -p "$root/home/.ssh"
chmod 700 "$root/home/.ssh"
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5hulltestkey hull-test\n' > "$root/home/.ssh/id_ed25519.pub"
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
# A client held open from the first run's core until the core is killed
# below, which ends it: the cases between read what that one core holds,
# an exited `box` and `kept` among it, and with no machine running a core
# leaves a second after its last client, which a loaded host can spend
# between two commands. It waits for the run's core to answer.
(timeout -k 2 900 python3 -c '
import os, socket, struct, time
for _ in range(200):
	try:
		s = socket.socket(socket.AF_UNIX)
		d = os.open(os.environ["XDG_RUNTIME_DIR"] + "/hull", os.O_PATH)
		s.connect("/proc/self/fd/%d/core.sock" % d)
		break
	except OSError:
		time.sleep(0.05)
m = b"(5:hello(2:ch1:0)(5:proto1:1))"
s.sendall(struct.pack(">I", len(m)) + m)
while s.recv(4096):
	pass
' > /dev/null 2>&1) &
core_holder=$!
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
from_root 125 'volume /absent: the guest path is not in the machine, and is made only above an overlay' \
	-f "$root/volmissing.conf"
# Above an overlay it is made, in the overlay's upper, and the dir keeps
# no trace of it.
conf volmade '(mutable overlay) (command "/probe" "read" "/absent/deeper/in")' \
	'(volumes (volume (host "./data") (guest "/absent/deeper")))'
from_root 0 '^read=from the host$' -f "$root/volmade.conf"
cases=$((cases + 1))
[ ! -e "$root/rootfs/absent" ] || fail "a volume's guest path was made in the dir itself" ""
conf volhost '(command "/probe")' '(volumes (volume (host "./nodata") (guest "/mnt")))'
from_root 125 'volume /.*/test/cli/\./nodata: No such file or directory' -f "$root/volhost.conf"

# A PID namespace, its /proc, and an init that reaps what is orphaned to it:
# a zombie would still be listed, making three.
mkdir -p "$root/rootfs/proc"
conf pid '(command "/probe" "pid" "orphan" "procs")'
from_root 0 '^pid=2$' -f "$root/pid.conf"
from_root 0 '^procs=2$' -f "$root/pid.conf"

# A bare ref the store holds runs that tree, J.1's image ref in the store:
# a dir source on its checkout, made once in the store under an overlay,
# so that a write reaches neither the checkout nor the tree committed. A
# plan names the checkout and makes none.
expect 0 '^stored/root:1 sha256:[0-9a-f]{64}$' "$hull" commit "$root/rootfs" stored/root:1
stored_hex=$(printf '%s\n' "$out" | sed -n 's/^stored\/root:1 sha256://p')
checkout=$XDG_DATA_HOME/hull/store/checkouts/sha256/$stored_hex
printf '(machine (source "stored/root:1") (backend chroot) (hostname "stored") %s)\n' \
	'(command "/probe" "write" "/srv/stored" "cwd")' > "$root/stored.conf"
expect 0 "\\(source \"dir:$checkout\"\\) \\(mutable overlay\\)" \
	"$hull" plan --resolved -f "$root/stored.conf"
cases=$((cases + 1))
[ ! -e "$checkout" ] || fail "a plan made the checkout it names" ""
from_root 0 '^write=ok$' -f "$root/stored.conf"
cases=$((cases + 1))
[ -d "$checkout/srv" ] && [ ! -e "$checkout/srv/stored" ] && [ ! -e "$root/rootfs/srv/stored" ] ||
	fail "a machine on a stored tree wrote its checkout or the tree" "$(ls "$checkout/srv" 2>&1)"
inode=$(stat -c %i "$checkout")
from_root 0 '^cwd=/$' -f "$root/stored.conf"
cases=$((cases + 1))
[ "$(stat -c %i "$checkout")" = "$inode" ] || fail "a second run checked the tree out again" ""
printf '(machine (source "stored/root:1") (backend chroot) (mutable direct) %s)\n' \
	'(hostname "stored") (command "/probe")' > "$root/stored-direct.conf"
from_root 125 'stored/root:1 is a stored tree, which nothing writes' -f "$root/stored-direct.conf"
# A ref the store does not hold is an image ref still, docker's.
printf '(machine (source "stored/none:1") (hostname "unstored"))\n' > "$root/unstored.conf"
expect 0 'machine on docker \(ns\): ok' "$hull" plan -f "$root/unstored.conf" --backend docker
# And gc keeps the checkout while a ref reaches its manifest, and takes
# it once none does.
expect 0 '^store: removed 0 manifests, 0 objects and 0 checkouts no ref reaches$' "$hull" gc
cases=$((cases + 1))
[ -d "$checkout" ] || fail "gc took a checkout a ref reaches" ""
rm "$XDG_DATA_HOME/hull/store/refs/stored/root:1"
expect 0 '^store: removed 1 manifests, [0-9]+ objects and 1 checkouts no ref reaches$' "$hull" gc
cases=$((cases + 1))
[ ! -e "$checkout" ] || fail "gc left a checkout no ref reaches" ""

# The core: what it holds, what it kept, who can follow a machine, and
# what (on_disconnect ...) does when the frontend that ran it is killed.
hull_abs="$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")"
expect 0 '^box +127\.107\.[0-9.]+ +chroot +ns +exited$' "$hull" ps
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
# hull stats --guest asks for the machine's own view from inside; the chroot
# engine shares the host's kernel, so there is no inside distinct from it and
# it has no guest_stats op -- the core says so (section 4.10).
expect 1 'no view of a machine from inside' "$hull" stats --guest gone
kill -KILL "$keeper" "$goner"
wait "$keeper" "$goner" 2>/dev/null
sleep 1
expect 0 '^kept +127\.107\.[0-9.]+ +chroot +ns +running$' "$hull" ps
expect 0 '^gone +127\.107\.[0-9.]+ +chroot +ns +exited$' "$hull" ps
# Section 8.1's address, from the id alone: FNV-1a of its bytes, modulo
# 256 * 254, as x * 254 + y - 1. Worked here in Python rather than taken
# from the core, and clash110098 is an id that comes to the same one.
address=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' kept)
expect 0 "^kept +$address +chroot" "$hull" ps
conf clash '(command "/probe" "say" "clash")' '(hostname "clash110098")'
from_root 125 "its address, $address, is machine kept's" -f "$root/clash.conf"
expect 0 '^kept$' "$hull" logs kept
(timeout 30 "$hull" attach kept > "$root/attach.out" 2>&1; echo "status=$?" >> "$root/attach.out") &
attacher=$!
sleep 0.5
expect 0 '' "$hull" stop kept
wait "$attacher"
cases=$((cases + 1))
grep -q '^status=143$' "$root/attach.out" ||
	fail "attach did not end with the stopped machine's 143" "$(cat "$root/attach.out")"
expect 0 '^kept +127\.107\.[0-9.]+ +chroot +ns +exited$' "$hull" ps

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
# ssh-config: the block before the first Host or Match, after the global
# lines, so that ssh itself, asked with -G, gives *.hull this hull's
# ProxyCommand over a Host * of the user's and nothing else changes; once
# however often it runs, the file's mode kept, and taken out to the byte.
mkdir -p "$root/home/.ssh"
printf 'ServerAliveInterval 30\n\nHost *\n\tProxyCommand /bin/false\n\nmatch host x\n\tPort 2\n' \
	> "$root/home/.ssh/config"
chmod 644 "$root/home/.ssh/config"
cp "$root/home/.ssh/config" "$root/ssh-config.before"
expect 0 '^hull: ssh-config: Host \*\.hull in ' env HOME="$root/home" "$hull" ssh-config
expect 0 '^hull: ssh-config: Host \*\.hull in ' env HOME="$root/home" "$hull" ssh-config
cases=$((cases + 1))
[ "$(grep -c '^Host \*\.hull$' "$root/home/.ssh/config")" = 1 ] &&
	[ "$(sed -n 3p "$root/home/.ssh/config" | cut -c1-24)" = "# hull ssh-config: begin" ] &&
	[ "$(stat -c %a "$root/home/.ssh/config")" = 644 ] ||
	fail "ssh-config's block is not once, in its place, in a file of the same mode" \
		"$(cat "$root/home/.ssh/config")"
cases=$((cases + 1))
said=$(timeout 10 ssh -G -F "$root/home/.ssh/config" web.hull < /dev/null 2>/dev/null)
other=$(timeout 10 ssh -G -F "$root/home/.ssh/config" example.org < /dev/null 2>/dev/null)
printf '%s\n' "$said" | grep -qx "proxycommand \"$(realpath "$hull")\" proxy-ssh %h" &&
	printf '%s\n' "$said" | grep -qx 'serveraliveinterval 30' &&
	printf '%s\n' "$other" | grep -qx 'proxycommand /bin/false' &&
	printf '%s\n' "$other" | grep -qx 'serveraliveinterval 30' ||
	fail "ssh does not read ssh-config's block as given" "$said"
expect 0 '^hull: ssh-config: took Host \*\.hull out of ' env HOME="$root/home" "$hull" ssh-config --remove
cases=$((cases + 1))
cmp -s "$root/ssh-config.before" "$root/home/.ssh/config" ||
	fail "ssh-config --remove did not give back the file as it was" "$(cat "$root/home/.ssh/config")"
expect 1 '^hull: ssh-config: HOME is not set$' env -u HOME "$hull" ssh-config

# A chroot machine without (ssh true) runs no sshd, so proxy-ssh has
# nothing to reach; and a name that is only the suffix names none.
expect 125 "^hull: that machine runs no sshd to reach$" "$hull" proxy-ssh execd.hull
expect 125 '^hull: \.hull names no machine$' "$hull" proxy-ssh .hull
expect 0 '^GREETING=hi$' "$hull" exec execd /probe env GREETING
# Small, and counted from 3 in the machine's own PID namespace, where a
# host pid would be neither.
expect 0 '^pid=([3-9]|[1-9][0-9])$' "$hull" exec execd /probe pid
expect 0 '^stdin=from the frontend$' sh -c 'echo from the frontend | "$0" exec execd /probe stdin' \
	"$hull_abs"
expect 0 '^stdin=\(none\)$' sh -c '"$0" exec execd /probe stdin < /dev/null' "$hull_abs"
expect 7 '' "$hull" exec execd /probe exit 7
# The machine's init holds nothing of the core's: none of its sockets,
# and not its lock, which a machine outliving its core would otherwise
# keep held. Its own sockets, its end of the pair it relays the workload
# on and a kept machine's init.sock, are in the core's hands nowhere.
cases=$((cases + 1))
workload=$(ps -eo pid=,args= | awk '$2 == "/probe" && $3 == "wait" {print $1; exit}')
init=$(ps -o ppid= -p "$workload" | tr -d ' ')
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
core_sockets=$(ls -l "/proc/$core/fd" 2>/dev/null | grep -o 'socket:\[[0-9]*\]')
held=$(ls -l "/proc/$init/fd" 2>/dev/null | grep 'core\.lock')
for socket in $(ls -l "/proc/$init/fd" 2>/dev/null | grep -o 'socket:\[[0-9]*\]'); do
	printf '%s\n' "$core_sockets" | grep -qxF "$socket" && held="$held $socket"
done
[ -n "$init" ] && [ -n "$core_sockets" ] && [ -z "$held" ] ||
	fail "the machine's init holds the core's descriptors" "$held"
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
# ssh: a login shell on a terminal, the frontend's TERM given as its
# argument. The probe, as the machine's /bin/sh, says what it was given.
mkdir -p "$root/rootfs/bin"
cp "$root/rootfs/probe" "$root/rootfs/bin/sh"
expect 0 '^sh 4=xterm-hull' script -qec 'TERM=xterm-hull "$HULL" ssh execd' /dev/null
expect 0 '^sh 2=TERM="\$1" exec /bin/sh -l' script -qec 'TERM=xterm-hull "$HULL" ssh execd' /dev/null
expect 0 '^tty=yes' script -qec 'TERM=xterm-hull "$HULL" ssh execd' /dev/null
expect 0 '^sh 1=-l' script -qec 'unset TERM; "$HULL" ssh execd' /dev/null
expect 125 'ssh needs a terminal' "$hull" ssh execd
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
# A machine's address is the core's, and a plan that gives one is refused.
expect 0 "a plan's address is the core's to give" python3 -c "$speak"'
say(b"(3:run(2:ch1:1)(7:machine4:here)(4:plan(7:address9:127.0.0.1)(5:class2:ns)"
    b"(6:engine6:chroot)(7:machine(6:source5:dir:.))))")
hear(b"6:reason")
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
expect 125 '^.*nope: No such file or directory' "$hull" exec execd nope
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
# The run that started a machine holds it back, as a pipe holds the
# command writing into it: a run read seven seconds late still gets every
# byte of a machine's 8 MB, eight times the log, and is told of nothing
# lost; where `logs -f` above, a follower and no more, lost what left the
# log. And a stop is not held up by a run that never reads: the machine
# ends at once, its output taken and dropped.
conf held '(command "/probe" "spew" "8000000")' '(hostname "held") (network none)'
want=$("$probe" spew 8000000 | "$probe" count)
cases=$((cases + 1))
got=$(cd / && timeout -k 2 60 "$hull_abs" run -f "$root/held.conf" 2> "$root/held.err" |
	(sleep 7; "$root/rootfs/probe" count))
[ "$got" = "$want" ] && ! grep -q 'output lost' "$root/held.err" ||
	fail "a run read late lost the machine's output" "$got, not $want: $(cat "$root/held.err")"
conf stuck '(command "/probe" "spew" "50000000")' '(hostname "stuck") (network none)'
# Into a fifo opened here and never read, then closed: the run's next
# write fails, and it ends then, rather than when a reader would.
rm -f "$root/stuck.fifo"
mkfifo "$root/stuck.fifo"
(cd / && exec timeout -k 2 40 "$hull_abs" run -f "$root/stuck.conf" > "$root/stuck.fifo" \
	2> /dev/null) &
stuckrun=$!
exec 8< "$root/stuck.fifo"
tries=0
until "$hull" ps 2>/dev/null | grep -q '^stuck .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
sleep 1
started=$(date +%s)
"$hull" stop --timeout 5 stuck > /dev/null 2>&1
took=$(($(date +%s) - started))
cases=$((cases + 1))
[ "$took" -lt 3 ] && ! "$hull" ps 2>/dev/null | grep -q '^stuck .*running$' ||
	fail "a machine held by a run that never read was not stopped at once" "${took}s"
exec 8<&-
wait "$stuckrun" 2> /dev/null
rm -f "$root/stuck.fifo"
# Eight machines writing at once, each a line per write, so each line is
# a frame of its own through its machine's reader thread in the engine:
# every run gets its own lines, whole and in order. The readers once
# shared a static buffer, and one machine's lines landed in another's
# output, 15 of 40 runs here. Under the log's MiB, so nothing is lost by
# design, and twice over, since a round can miss the race. (network
# none), so that no pasta staggers the starts and they write together:
# with one each, the same case on the broken build crossed nothing.
for n in 1 2 3 4 5 6 7 8; do
	conf "lines$n" "(command \"/probe\" \"lines\" \"20000\" \"machine-$n\")" \
		"(hostname \"lines$n\") (network none)"
	"$probe" lines 20000 "machine-$n" > "$root/lines$n.want"
done
cases=$((cases + 1))
crossed=0
for round in 1 2; do
	writers=
	for n in 1 2 3 4 5 6 7 8; do
		(cd / && exec timeout -k 2 60 "$hull_abs" run -f "$root/lines$n.conf" \
			> "$root/lines$n.out" 2> /dev/null) &
		writers="$writers $!"
	done
	for writer in $writers; do
		wait "$writer"
	done
	for n in 1 2 3 4 5 6 7 8; do
		cmp -s "$root/lines$n.out" "$root/lines$n.want" || crossed=$((crossed + 1))
	done
done
[ "$crossed" -eq 0 ] ||
	fail "machines writing at once got each other's output, $crossed of 16 runs" \
		"$(diff "$root/lines1.want" "$root/lines1.out" | head -5)"
# A frontend that sends and never reads what it is answered is dropped once
# 8 MiB of answers wait for it, and nobody else waits meanwhile. Queueing
# them took 10 seconds and more with the host's CPUs oversubscribed, so
# this case's ceiling is 60.
limit=60
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
rm -f "$root/killd.out"
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/killd.conf" > "$root/killd.out" 2>&1) &
killed=$!
await_ready "$root/killd.out"
expect 0 '' "$hull" kill -s sigterm killd
wait "$killed"
got=$?
[ "$got" = 0 ] && grep -q '^signal=15$' "$root/killd.out" && grep -q '^after$' "$root/killd.out" ||
	fail "a TERM to the workload: run exited $got, wanted 0 after it caught signal 15" \
		"$(cat "$root/killd.out"); core.log: $(tail -20 "$XDG_RUNTIME_DIR/hull/core.log" 2>&1)"
cases=$((cases + 1))
rm -f "$root/killd.out"
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

# What the host spends on a machine, as its engine measures it: 300 ms of
# the probe's CPU and 300 of a child it reaped, 32 MiB touched and 4 MiB
# written, then 400 ms and 4 MiB more in an exec, whose helper reaps it
# outside the machine's pid namespace. CPU time is the probe's own clock,
# so a loaded host makes it slower and not smaller.
conf statsd '(command "/probe" "spin" "300" "reap" "300" "hold" "32" "fill" "/written" "4" "trap")' \
	'(hostname "statsd")'
figure() { # NAME STATS: one figure of hull stats, the cpu in microseconds
	printf '%s\n' "$2" | awk -v name="$1" '$1 == name { gsub(/\./, "", $2); print $2 + 0 }'
}
rm -f "$root/statsd.out"
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/statsd.conf" > "$root/statsd.out" 2>&1) &
statsd=$!
await_ready "$root/statsd.out"
cases=$((cases + 1))
first=$(timeout 10 "$hull" stats statsd 2>&1)
cpu=$(figure cpu "$first")
[ "${cpu:-0}" -ge 590000 ] && [ "$cpu" -lt 5000000 ] ||
	fail "stats: cpu, wanted the probe's 0.3 s and its child's" "$first"
cases=$((cases + 1))
memory=$(figure memory "$first")
[ "${memory:-0}" -ge 33554432 ] && [ "$memory" -lt 268435456 ] ||
	fail "stats: memory, wanted the 32 MiB the probe holds" "$first"
cases=$((cases + 1))
io=$(figure io "$first")
[ "${io:-0}" -ge 4194304 ] && [ "$io" -lt 67108864 ] ||
	fail "stats: io, wanted the 4 MiB the probe wrote" "$first"
expect 0 '^filled=4$' "$hull" exec statsd /probe spin 400 fill /written 4
cases=$((cases + 1))
second=$(timeout 10 "$hull" stats statsd 2>&1)
[ "$(figure cpu "$second")" -ge $((${cpu:-0} + 390000)) ] ||
	fail "stats: an ended exec's cpu, wanted 0.4 s more than before it" "$first
$second"
cases=$((cases + 1))
[ "$(figure io "$second")" -ge $((${io:-0} + 4194304)) ] ||
	fail "stats: an ended exec's io, wanted 4 MiB more than before it" "$first
$second"
# A rate, over two readings: the probe waiting is idle, under 5 % of a
# CPU, and an exec spinning is one CPU's worth at most and, however loaded
# the host, much of one.
percent() { # STATS: the cpu line's percentage, as a whole number
	printf '%s\n' "$1" | awk '$1 == "cpu" && $3 == "%" { printf "%d\n", $2 }'
}
cases=$((cases + 1))
idle=$(timeout 10 "$hull" stats --over 1 statsd 2>&1)
[ -n "$(percent "$idle")" ] && [ "$(percent "$idle")" -lt 5 ] &&
	printf '%s\n' "$idle" | grep -Eq '^io +[0-9]+ bytes/s$' &&
	printf '%s\n' "$idle" | grep -Eq '^memory +[0-9]+ bytes$' ||
	fail "stats --over: a waiting probe, wanted under 5 % of a CPU" "$idle"
(timeout 20 "$hull" exec statsd /probe spin 3000 > /dev/null 2>&1) &
spinner=$!
sleep 0.5
cases=$((cases + 1))
busy=$(timeout 10 "$hull" stats --over 2 statsd 2>&1)
[ -n "$(percent "$busy")" ] && [ "$(percent "$busy")" -ge 30 ] && [ "$(percent "$busy")" -le 110 ] ||
	fail "stats --over: a spinning exec, wanted 30 to 110 % of a CPU" "$busy"
wait "$spinner"
expect 2 '^hull: stats --over takes 1 to 3600 seconds$' "$hull" stats --over 0 statsd
expect 2 '^hull: stats --over takes 1 to 3600 seconds$' "$hull" stats --over 2s statsd
expect 0 '' "$hull" kill -s TERM statsd
wait "$statsd"
rm -f "$root/rootfs/written"
expect 1 'the machine is not running' "$hull" stats statsd

# An overlay: the directory beneath, what the machine writes in a tmpfs
# above, in the machine's own mount namespace. / keeps the directory's
# mode, as in place; a write and a removal are the machine's, and the
# directory is as it was. An exec sees the machine's root, the overlay.
# A change to a file needs it copied up, with its parents, and the
# namespace maps only the user and the user's group, so where a group
# is another, as /srv's is when the build tree's is, the change fails
# as EOVERFLOW. mapped/ is the user's group's, which maps.
mkdir -p "$root/rootfs/mapped"
chgrp "$(id -g)" "$root/rootfs/mapped"
chmod 755 "$root/rootfs/mapped"
printf 'kept\n' > "$root/rootfs/mapped/kept"
mkdir -p "$root/rootfs/mapped/d"
printf 'f\n' > "$root/rootfs/mapped/d/f"
printf 'kept\n' > "$root/rootfs/srv/kept"
# A machine's /dev, where its tree has one: the host's devices bound into a
# tmpfs of its own, and what is written there not on the tree's disk.
mkdir -p "$root/devroot/dev"
cp "$root/rootfs/probe" "$root/devroot/probe"
printf '(machine (source "dir:./devroot") (backend chroot) (command "/probe" %s))\n' \
	'"kind" "/dev/null" "kind" "/dev/zero" "kind" "/dev/full" "kind" "/dev/urandom" "kind" "/dev/fd" "owner" "/dev/shm" "write" "/dev/stray"' \
	> "$root/devs.conf"
for want in '^kind=chr 1:3$' '^kind=chr 1:5$' '^kind=chr 1:7$' '^kind=chr 1:9$' '^kind=lnk$' \
	'^owner=0:0 1777$' '^write=ok$'; do
	from_root 0 "$want" -f "$root/devs.conf"
done
# (init PATH) on chroot, as on a vm: PID 1 is the image's init, here the
# probe as one, which reaps for ever, and hull's init goes on as its
# child; the workload is not PID 1, and when it ends the image's
# /sbin/poweroff, the probe as one, ends the machine at once. A stop
# reaches the workload by hull's init, as without the key, not after the
# kill. Without a poweroff the machine ends after the grace; an init that
# is not there is refused.
mkdir -p "$root/initroot/sbin" "$root/initroot/proc" "$root/initroot/dev" \
	"$root/initbare/sbin" "$root/initbare/proc" "$root/initbare/dev"
for d in initroot initbare; do
	cp "$probe" "$root/$d/probe"
	cp "$probe" "$root/$d/sbin/init"
done
cp "$probe" "$root/initroot/sbin/poweroff"
printf '(machine (source "dir:./initroot") (backend chroot) (init "/sbin/init") %s)\n' \
	'(command "/probe" "target" "/proc/1/exe" "pid")' > "$root/inited.conf"
started=$(date +%s)
from_root 0 '^target=/sbin/init$' -f "$root/inited.conf"
took=$(( $(date +%s) - started ))
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq '^pid=([2-9]|[1-9][0-9]+)$' ||
	fail "a machine handed to its init ran its workload as PID 1" "$out"
cases=$((cases + 1))
[ "$took" -lt 10 ] || fail "a machine handed to its init took ${took}s to end, its poweroff unused" "$out"
printf '(machine (source "dir:./initroot") (backend chroot) (init "/sbin/init") %s)\n' \
	'(hostname "initexec") (command "/probe" "say" "ready" "wait")' > "$root/initexec.conf"
rm -f "$root/initexec.out"
init_hull=$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")
(cd / && exec timeout 60 "$init_hull" run -f "$root/initexec.conf" > "$root/initexec.out" 2>&1) &
initexec=$!
tries=0
until grep -q '^ready$' "$root/initexec.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 '^target=/sbin/init$' "$hull" exec initexec /probe target /proc/1/exe
started=$(date +%s)
timeout 30 "$hull" stop --timeout 8 initexec > /dev/null 2>&1
took=$(( $(date +%s) - started ))
wait "$initexec"
cases=$((cases + 1))
[ "$took" -lt 6 ] || fail "a stop of a machine handed to its init took ${took}s: its TERM missed the workload" \
	"$(cat "$root/initexec.out")"
rm -f "$root/initexec.out"
printf '(machine (source "dir:./initbare") (backend chroot) (init "/sbin/init") %s)\n' \
	'(command "/probe" "say" "bare")' > "$root/initbare.conf"
started=$(date +%s)
limit=60
from_root 0 '^bare$' -f "$root/initbare.conf"
took=$(( $(date +%s) - started ))
cases=$((cases + 1))
[ "$took" -ge 18 ] && [ "$took" -lt 45 ] ||
	fail "a machine with no poweroff ended in ${took}s, not after the grace" "$out"
printf '(machine (source "dir:./initroot") (backend chroot) (init "/sbin/nothing") %s)\n' \
	'(command "/probe" "say" "never")' > "$root/noinit.conf"
from_root 125 'no init it can run' -f "$root/noinit.conf"
cases=$((cases + 1))
[ -z "$(ls -A "$root/devroot/dev")" ] || fail "a write to a machine's /dev reached its tree" "$(ls -A "$root/devroot/dev")"
# A machine whose root is the host's own / keeps the host's /dev, whose
# devices are more than the six bound in elsewhere.
extra=
for f in /dev/*; do
	case ${f#/dev/} in null|zero|full|random|urandom|tty) continue ;; esac
	[ -c "$f" ] && extra=$f && break
done
printf '(machine (source "dir:/") (backend chroot) (command "%s" "kind" "%s"))\n' \
	"$root/devroot/probe" "$extra" > "$root/hostdev.conf"
from_root 0 "^kind=chr $(stat -c '%Hr:%Lr' "$extra")\$" -f "$root/hostdev.conf"
conf overlay '(mutable overlay)' \
	'(command "/probe" "owner" "/" "write" "/layered" "remove" "/mapped/kept" "read" "/mapped/kept"' \
	'"remove" "/srv/kept")'
from_root 0 'source\.dir\.overlay: emulated: an overlayfs in the machine.s mount namespace' \
	-f "$root/overlay.conf"
# In place, the directory's group may be one the namespace does not map,
# the overflow id, which an overlay's top cannot be given: root's then.
conf ownerin '(command "/probe" "owner" "/")'
inplace=$(cd / && timeout 10 "$hull_abs" run -f "$root/ownerin.conf" 2>&1 | grep '^owner=')
case $inplace in
owner=*:65534\ *) layered="owner=${inplace#owner=}"; layered="${layered%%:*}:0 ${inplace##* }" ;;
*) layered=$inplace ;;
esac
from_root 0 "^$layered\$" -f "$root/overlay.conf"
from_root 0 '^write=ok$' -f "$root/overlay.conf"
from_root 0 '^remove=ok$' -f "$root/overlay.conf"
from_root 0 '^read=No such file or directory$' -f "$root/overlay.conf"
# A directory of the directory's, emptied, removed and made again, is
# empty, which overlayfs keeps in an xattr: user.overlay.*, since the
# namespace cannot write trusted.*, so the mount must say userxattr.
conf opaque '(mutable overlay)' \
	'(command "/probe" "remove" "/mapped/d/f" "rmdir" "/mapped/d" "mkdir" "/mapped/d" "list" "/mapped/d")'
from_root 0 '^rmdir=ok$' -f "$root/opaque.conf"
from_root 0 '^list=$' -f "$root/opaque.conf"
if [ "$(stat -c %g "$root/rootfs/srv")" = "$(id -g)" ]; then
	from_root 0 '^remove=ok$' -f "$root/overlay.conf"
else
	from_root 0 '^remove=Value too large for defined data type$' -f "$root/overlay.conf"
fi
cases=$((cases + 1))
[ ! -e "$root/rootfs/layered" ] && [ "$(cat "$root/rootfs/mapped/kept" 2>&1)" = kept ] &&
	[ -f "$root/rootfs/mapped/d/f" ] &&
	[ "$(cat "$root/rootfs/srv/kept" 2>&1)" = kept ] ||
	fail "an overlay's writes reached the directory" "$(ls -l "$root/rootfs" "$root/rootfs/srv" 2>&1)"
conf overro '(mutable overlay) (security (readonly_root true))' '(command "/probe" "write" "/again")'
from_root 0 '^write=Read-only file system$' -f "$root/overro.conf"
conf overx '(mutable overlay) (hostname "overx")' '(command "/probe" "write" "/layered" "trap")'
rm -f "$root/overx.out"
(cd / && exec timeout -k 2 20 "$hull_abs" run -f "$root/overx.conf" > "$root/overx.out" 2>&1) &
overx=$!
await_ready "$root/overx.out"
expect 0 '^owner=0:0 0644$' "$hull" exec overx /probe owner /layered
expect 0 '^read=kept$' "$hull" exec overx /probe read /mapped/kept
expect 0 '' "$hull" kill -s TERM overx
wait "$overx"
cases=$((cases + 1))
[ ! -e "$root/rootfs/layered" ] || fail "an overlay's write reached the directory through exec's machine" ""
rm -f "$root/rootfs/srv/kept" "$root/rootfs/mapped/kept" "$root/rootfs/mapped/d/f"
rmdir "$root/rootfs/mapped/d" "$root/rootfs/mapped"
# Appendix D's segments on chroot: a tap each, eth0 first, in the
# machine's own network namespace, which (network none) gives it, relayed
# to the core's switch. Two machines on one hear each other in raw frames,
# from a MAC of qemu's prefix, and the shouter is answered; a third, on
# another segment at the same time, hears nothing. The name is this run's.
# Without (network none) a segment is refused, the host's namespace being
# none a tap can be made in.
wire="cwire-$$"
whole_hull=$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")
conf chearer "(hostname \"chearer\") (network none) (segments \"$wire\")" '(command "/probe" "link" "eth0" "hear" "eth0")'
conf cshouter "(hostname \"cshouter\") (network none) (segments \"$wire\")" \
	'(command "/probe" "link" "eth0" "shout" "eth0" "over the wire")'
conf cloner "(hostname \"cloner\") (network none) (segments \"elsewhere-$$\")" '(command "/probe" "link" "eth0" "hear" "eth0")'
(cd / && exec timeout 60 "$whole_hull" run -f "$root/chearer.conf" > "$root/chearer.out" 2>&1) &
heard=$!
(cd / && exec timeout 60 "$whole_hull" run -f "$root/cloner.conf" > "$root/cloner.out" 2>&1) &
lone=$!
limit=40
from_root 0 '^shout=answered$' -f "$root/cshouter.conf"
wait "$heard"
wait "$lone"
cases=$((cases + 1))
grep -q '^hear=over the wire from 52:54:00:' "$root/chearer.out" ||
	fail "a chroot machine on a segment did not hear the other" "$(cat "$root/chearer.out")"
cases=$((cases + 1))
grep -q '^hear=nothing$' "$root/cloner.out" ||
	fail "a chroot machine on another segment heard something" "$(cat "$root/cloner.out")"
# Appendix D's sibling names on chroot: a segment given a subnet is
# hull's to address, each machine's NIC up at its drawn address, every
# sibling named in an /etc/hosts bound over the tree's, and a datagram
# sent by that name answered by the sibling's own hostname. A bare segment
# in the same file is left as it was: down, no address, no hosts.
named="named-$$"
printf '(machines (web %s) (db %s) (raw %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network none) (segments ($named (subnet \"10.77.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"hosts\" \"db\" \"hosts\" \"db.hull\" \"hosts\" \"web\" \"ask\" \"db\" \"7000\" \"read\" \"/etc/resolv.conf\" \"line\" \"/etc/resolv.conf\" \"options edns0\" \"dns\" \"db\" \"dns\" \"db.hull\" \"dns\" \"nobody\")" \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network none) (segments ($named (subnet \"10.77.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"answer\" \"7000\" \"sleep\" \"4\")" \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network none) (segments \"raw-$$\") (command \"/probe\" \"addr\" \"eth0\" \"read\" \"/etc/hosts\" \"read\" \"/etc/resolv.conf\")" \
	> "$root/named.conf"
limit=60
from_root 0 '^web +\| ask=db at 10\.77\.0\.[0-9]+: db$' -f "$root/named.conf"
named_out=$out
web_at=$(printf '%s\n' "$named_out" | sed -n 's#^web *| addr=eth0 \(10\.77\.0\.[0-9]*\)/24 up$#\1#p')
db_at=$(printf '%s\n' "$named_out" | sed -n 's#^db *| addr=eth0 \(10\.77\.0\.[0-9]*\)/24 up$#\1#p')
cases=$((cases + 1))
[ -n "$web_at" ] && [ -n "$db_at" ] && [ "$web_at" != "$db_at" ] &&
	printf '%s\n' "$named_out" | grep -q "^web *| hosts=db $db_at\$" &&
	printf '%s\n' "$named_out" | grep -q "^web *| hosts=db.hull $db_at\$" &&
	printf '%s\n' "$named_out" | grep -q "^web *| hosts=web $web_at\$" &&
	printf '%s\n' "$named_out" | grep -q "^db *| answer=from $web_at\$" ||
	fail "addressed siblings were not named to each other at their own addresses" "$named_out"
# hull answers at the segment's .1, which resolv.conf names, with edns0 so
# glibc asks with room for 1200 bytes: the sibling by id and by id.hull,
# and a name it has not refused as one it has not.
cases=$((cases + 1))
printf '%s\n' "$named_out" | grep -q '^web *| read=nameserver 10\.77\.0\.1$' &&
	printf '%s\n' "$named_out" | grep -q '^web *| line=yes$' &&
	printf '%s\n' "$named_out" | grep -q "^web *| dns=db $db_at\$" &&
	printf '%s\n' "$named_out" | grep -q "^web *| dns=db.hull $db_at\$" &&
	printf '%s\n' "$named_out" | grep -q '^web *| dns=nobody rcode 3$' ||
	fail "hull at an addressed segment's .1 did not name the siblings" "$named_out"
cases=$((cases + 1))
printf '%s\n' "$named_out" | grep -q '^raw *| addr=eth0 none/0 down$' &&
	[ "$(printf '%s\n' "$named_out" | grep -c '^raw *| read=No such file or directory$')" -eq 2 ] ||
	fail "a bare segment beside an addressed one was addressed" "$named_out"
# The same file drawn again gives the same addresses: the draw is the
# machine's id and the segment's name, not the run's.
limit=60
from_root 0 "^web +\| addr=eth0 $web_at/24 up\$" -f "$root/named.conf"
# A tree whose /etc has a group the machine's namespace does not map --
# one of the user's own others, which neither the one-id map nor the
# subordinate range covers, as a setgid shared directory gives -- with no
# /etc/hosts and resolv.conf a link: hull's two files are made above the
# overlay all the same, rather than copying /etc up with an owner it
# cannot give, EOVERFLOW, and so is a volume's guest path below /srv,
# which hull makes nothing else in;
# /etc's own file still seen, and the tree not written.
other_group=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
if [ -n "$other_group" ]; then
	mkdir -p "$root/grouproot/etc" "$root/grouproot/srv" "$root/groupdata"
	cp "$probe" "$root/grouproot/probe"
	printf 'shared\n' > "$root/groupdata/given"
	printf 'kept\n' > "$root/grouproot/etc/motd"
	ln -sfn ../run/resolv.conf "$root/grouproot/etc/resolv.conf"
	chgrp "$other_group" "$root/grouproot/etc" "$root/grouproot/srv" &&
		chmod 2775 "$root/grouproot/etc" "$root/grouproot/srv"
	printf '(machines (gweb %s))\n' \
		"(source \"dir:./grouproot\") (backend chroot) (mutable overlay) (network none) (segments (grouped-$$ (subnet \"10.83.0.0/24\"))) (volumes (volume (host \"$root/groupdata\") (guest \"/srv/data/in\"))) (command \"/probe\" \"hosts\" \"gweb\" \"read\" \"/etc/resolv.conf\" \"read\" \"/etc/motd\" \"read\" \"/srv/data/in/given\")" \
		> "$root/grouped.conf"
	from_root 0 '^gweb +\| hosts=gweb 10\.83\.0\.[0-9]+$' -f "$root/grouped.conf"
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^gweb *| read=nameserver 10\.83\.0\.1$' &&
		printf '%s\n' "$out" | grep -q '^gweb *| read=kept$' &&
		printf '%s\n' "$out" | grep -q '^gweb *| read=shared$' &&
		[ ! -e "$root/grouproot/etc/hosts" ] && [ ! -e "$root/grouproot/srv/data" ] &&
		[ -L "$root/grouproot/etc/resolv.conf" ] ||
		fail "a tree whose /etc's group is not mapped did not take hull's names and a volume, or was written" "$out"
else
	echo "cli: an unmapped group's /etc not measured: the user is in no group but their own"
fi
# A tree used in place has no /etc/hosts to bind over, and is not written
# to make one; a subnet for one segment twice, and a segment addressed
# for one machine and bare for another, are refused before anything runs.
conf inplace "(network none) (segments (inplace-$$ (subnet \"10.78.0.0/24\"))) (command \"/probe\" \"say\" \"ran\")"
from_root 125 'names go over /etc/hosts, which is not in the machine' -f "$root/inplace.conf"
printf '(machines (a %s) (b %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments (two-$$ (subnet \"10.79.0.0/24\"))) (command \"/probe\" \"say\" \"ran\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments (two-$$ (subnet \"10.80.0.0/24\"))) (command \"/probe\" \"say\" \"ran\")" \
	> "$root/twosub.conf"
from_root 125 "segment two-$$ is given two subnets" -f "$root/twosub.conf"
printf '(machines (a %s) (b %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments (half-$$ (subnet \"10.81.0.0/24\"))) (command \"/probe\" \"say\" \"ran\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"half-$$\") (command \"/probe\" \"say\" \"ran\")" \
	> "$root/half.conf"
from_root 125 "names it without its subnet" -f "$root/half.conf"
# Segments beside NAT, as a vm has them beside passt: pasta's eth0 first,
# the segments' NICs from eth1; an addressed segment's siblings named by
# /etc/hosts and reached by name across the core's switch, the machine
# asking hull at .1, which names the sibling and asks the host's resolvers
# the rest, so a name outside is answered rather than refused; and a bare
# segment's eth1 left down and given no address, as under (network none).
natseg="natseg-$$"
printf '(machines (nweb %s) (ndb %s) (nraw %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (segments ($natseg (subnet \"10.84.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"addr\" \"eth1\" \"read\" \"/etc/resolv.conf\" \"hosts\" \"ndb\" \"ask\" \"ndb\" \"7000\" \"dns\" \"ndb\" \"dns\" \"hull-test.invalid\")" \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (segments ($natseg (subnet \"10.84.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"addr\" \"eth1\" \"answer\" \"7000\")" \
	"(source \"dir:./rootfs\") (backend chroot) (segments \"natraw-$$\") (command \"/probe\" \"addr\" \"eth0\" \"addr\" \"eth1\")" \
	> "$root/natseg.conf"
limit=60
from_root 0 '^nweb +\| ask=ndb at 10\.84\.0\.[0-9]+: ndb$' -f "$root/natseg.conf"
natseg_out=$out
ndb_at=$(printf '%s\n' "$natseg_out" | sed -n 's#^ndb *| addr=eth1 \(10\.84\.0\.[0-9]*\)/24 up$#\1#p')
cases=$((cases + 1))
[ -n "$ndb_at" ] &&
	printf '%s\n' "$natseg_out" | grep -q '^nweb *| addr=eth0 10\.0\.2\.15/24 up$' &&
	printf '%s\n' "$natseg_out" | grep -q '^ndb *| addr=eth0 10\.0\.2\.15/24 up$' &&
	printf '%s\n' "$natseg_out" | grep -q '^nweb *| addr=eth1 10\.84\.0\.[0-9]*/24 up$' &&
	printf '%s\n' "$natseg_out" | grep -q "^nweb *| hosts=ndb $ndb_at\$" &&
	printf '%s\n' "$natseg_out" | grep -q '^ndb *| answer=from 10\.84\.0\.[0-9]*$' ||
	fail "addressed siblings beside NAT were not on eth1, named and reached, with pasta's eth0" "$natseg_out"
# REFUSED is what .1 gave before it asked on; .invalid is NXDOMAIN by RFC
# 6761, but any answer of the host's resolver will do.
cases=$((cases + 1))
printf '%s\n' "$natseg_out" | grep -q '^nweb *| read=nameserver 10\.84\.0\.1$' &&
	printf '%s\n' "$natseg_out" | grep -q "^nweb *| dns=ndb $ndb_at\$" &&
	printf '%s\n' "$natseg_out" | grep -Eq '^nweb +\| dns=hull-test\.invalid rcode [0-4]$' ||
	fail "a machine beside NAT did not ask .1, for its sibling and, asked on, for a name outside" "$natseg_out"
cases=$((cases + 1))
printf '%s\n' "$natseg_out" | grep -q '^nraw *| addr=eth0 10\.0\.2\.15/24 up$' &&
	printf '%s\n' "$natseg_out" | grep -q '^nraw *| addr=eth1 none/0 down$' ||
	fail "a bare segment beside NAT was not eth1, down and unaddressed" "$natseg_out"
# Each file draws only its own machines' addresses, so the switch is where
# two files on one addressed segment meet: with a /29's five hosts all held
# by one file, a machine of another file on the same subnet is refused,
# as is another subnet for the segment; a bare machine joins as before.
full="full-$$"
held='(source "dir:./rootfs") (backend chroot) (mutable overlay) (network none) (segments ('"$full"' (subnet "10.82.0.0/29"))) (command "/probe" "sleep" "8")'
printf '(machines (h1 %s) (h2 %s) (h3 %s) (h4 %s) (h5 %s))\n' "$held" "$held" "$held" "$held" "$held" \
	> "$root/full.conf"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/full.conf" > "$root/full.out" 2>&1) &
fullrun=$!
tries=0
until "$hull" segment "$full" > "$root/full.counts" 2>&1 &&
	[ "$(grep -Ec '^h[1-5] +out ' "$root/full.counts")" -eq 5 ]; do
	tries=$((tries + 1))
	[ "$tries" -lt 60 ] || break
	sleep 0.25
done
conf another "(mutable overlay) (network none) (segments ($full (subnet \"10.82.0.0/29\"))) (command \"/probe\" \"say\" \"ran\")"
from_root 125 "its address on segment $full, 10\\.82\\.0\\.[2-6], is machine h[1-5]'s" -f "$root/another.conf"
conf widened "(mutable overlay) (network none) (segments ($full (subnet \"10.82.0.0/24\"))) (command \"/probe\" \"say\" \"ran\")"
from_root 125 "segment $full is 10\\.82\\.0\\.0/29, as machine h[1-5] has it, and this machine gives it 10\\.82\\.0\\.0/24" -f "$root/widened.conf"
conf barejoin "(network none) (segments \"$full\") (command \"/probe\" \"say\" \"ran\")"
from_root 0 '^ran$' -f "$root/barejoin.conf"
cases=$((cases + 1))
wait "$fullrun" ||
	fail "a file holding a segment's addresses did not run whole" "$(cat "$root/full.out")"
# A machine joining a running segment is named by hull at .1 as soon as
# its port is, though no file put it in another's /etc/hosts: one file's
# early, another's late, each looking the other up by name.
dyn="dyn-$$"
conf early "(hostname \"early\") (mutable overlay) (network none) (segments ($dyn (subnet \"10.83.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"sleep\" \"5\" \"dns\" \"late\" \"hosts\" \"late\")"
conf late "(hostname \"late\") (mutable overlay) (network none) (segments ($dyn (subnet \"10.83.0.0/24\"))) (command \"/probe\" \"addr\" \"eth0\" \"dns\" \"early\" \"sleep\" \"8\")"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/early.conf" > "$root/early.out" 2>&1) &
earlyrun=$!
tries=0
until "$hull" segment "$dyn" > "$root/dyn.counts" 2>&1 && grep -Eq '^early +out ' "$root/dyn.counts"; do
	tries=$((tries + 1))
	[ "$tries" -lt 60 ] || break
	sleep 0.25
done
limit=30
from_root 0 '^dns=early 10\.83\.0\.[0-9]+$' -f "$root/late.conf"
late_out=$out
wait "$earlyrun"
early_at=$(sed -n 's#^addr=eth0 \(10\.83\.0\.[0-9]*\)/24 up$#\1#p' "$root/early.out")
late_at=$(printf '%s\n' "$late_out" | sed -n 's#^addr=eth0 \(10\.83\.0\.[0-9]*\)/24 up$#\1#p')
cases=$((cases + 1))
[ -n "$early_at" ] && [ -n "$late_at" ] &&
	printf '%s\n' "$late_out" | grep -q "^dns=early $early_at\$" &&
	grep -q "^dns=late $late_at\$" "$root/early.out" &&
	grep -q '^hosts=late none$' "$root/early.out" ||
	fail "a machine joining a running segment was not named both ways" "$(cat "$root/early.out"; printf '%s\n' "$late_out")"
# Impairment in the switch, changed during a run: a hearer and a shouter
# on one segment, the shouter's way out cut before it shouts, its frames
# counted lost, then healed, so a later shout is heard and answered.
cut="cut-$$"
printf '(machines (hearer %s) (shouter %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$cut\") (command \"/probe\" \"link\" \"eth0\" \"hear\" \"eth0\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$cut\") (command \"/probe\" \"link\" \"eth0\" \"sleep\" \"3\" \"shout\" \"eth0\" \"through\")" \
	> "$root/cut.conf"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/cut.conf" > "$root/cut.out" 2>&1) &
cutrun=$!
cases=$((cases + 1))
tries=0
until "$hull" segment "$cut" impair shouter out loss=1 2> "$root/impair.err"; do
	tries=$((tries + 1))
	[ "$tries" -lt 40 ] || break
	sleep 0.25
done
[ "$tries" -lt 40 ] || fail "a segment's impairment could not be set" "$(cat "$root/impair.err")"
cases=$((cases + 1))
tries=0
until "$hull" segment "$cut" > "$root/counts.out" 2>&1 &&
	grep -Eq '^shouter +out frames=[1-9][0-9]* .* lost=[1-9]' "$root/counts.out"; do
	tries=$((tries + 1))
	[ "$tries" -lt 60 ] || break
	sleep 0.25
done
[ "$tries" -lt 60 ] || fail "a cut link's frames were not counted lost" "$(cat "$root/counts.out")"
expect 0 '' "$hull" segment "$cut" impair shouter out
wait "$cutrun"
cases=$((cases + 1))
grep -q '^shouter | shout=answered$' "$root/cut.out" && grep -q '^hearer  | hear=through from 52:54:00:' "$root/cut.out" ||
	fail "a healed link was not heard across" "$(cat "$root/cut.out")"
# An impairment the spec gives, from the machine's first frame: the
# shouter's way out down, so that not even its kernel's frames reach the
# hearer, its loss counted; healed during the run, its shout is heard.
cutin="cutin-$$"
printf '(machines (hearer %s) (shouter %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$cutin\") (command \"/probe\" \"link\" \"eth0\" \"hear\" \"eth0\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$cutin\") (impair ($cutin (out (down true)))) (command \"/probe\" \"link\" \"eth0\" \"sleep\" \"1\" \"shout\" \"eth0\" \"through\")" \
	> "$root/cutin.conf"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/cutin.conf" > "$root/cutin.out" 2>&1) &
cutinrun=$!
cases=$((cases + 1))
tries=0
until "$hull" segment "$cutin" > "$root/cutin.counts" 2>&1 &&
	grep -Eq '^shouter +out frames=[1-9][0-9]* .* lost=[1-9]' "$root/cutin.counts"; do
	tries=$((tries + 1))
	[ "$tries" -lt 60 ] || break
	sleep 0.25
done
[ "$tries" -lt 60 ] && grep -Eq '^hearer +in +frames=0 ' "$root/cutin.counts" ||
	fail "a spec's impairment did not hold from the first frame" "$(cat "$root/cutin.counts")"
expect 0 '' "$hull" segment "$cutin" impair shouter out
wait "$cutinrun"
cases=$((cases + 1))
grep -q '^shouter | shout=answered$' "$root/cutin.out" ||
	fail "a spec's impairment healed at runtime was not heard across" "$(cat "$root/cutin.out")"
# The run says so on stderr, for the machine whose spec gives it alone,
# since nothing in a machine's own output shows its link impaired.
cases=$((cases + 1))
grep -q "/cutin\\.conf: shouter: segment $cutin impaired on the way out from the first frame; hull segment $cutin counts it\$" "$root/cutin.out" &&
	! grep -q '/cutin\.conf: hearer: segment' "$root/cutin.out" ||
	fail "a spec's impairment was not noticed for its machine alone" "$(cat "$root/cutin.out")"
# A spec's gemodel given only its p takes netem's defaults: r 1-p and a
# bad state's loss 1. With p 1 the first frame finds the good state and
# passes, the link goes bad and r 0 keeps it there, so every frame after
# it is lost: counted while the machine runs, its kernel's own frames
# being enough. A wrong r alternates, a wrong bad_loss loses none.
gem="gem-$$"
conf gemodel "(network none) (segments \"$gem\") (impair ($gem (out (gemodel (p 1))))) (command \"/probe\" \"link\" \"eth0\" \"sleep\" \"6\")"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/gemodel.conf" > "$root/gemodel.out" 2>&1) &
gemrun=$!
tries=0
until "$hull" segment "$gem" > "$root/gemodel.counts" 2>&1 &&
	grep -Eq '^[a-z0-9]+ +out +frames=([3-9]|[1-9][0-9]+) ' "$root/gemodel.counts"; do
	tries=$((tries + 1))
	[ "$tries" -lt 80 ] || break
	sleep 0.25
done
gem_frames=$(sed -n 's/^[a-z0-9]* *out *frames=\([0-9]*\) .*/\1/p' "$root/gemodel.counts")
gem_lost=$(sed -n 's/^[a-z0-9]* *out .* lost=\([0-9]*\).*/\1/p' "$root/gemodel.counts")
wait "$gemrun"
cases=$((cases + 1))
[ -n "$gem_frames" ] && [ -n "$gem_lost" ] && [ "$gem_frames" -ge 3 ] &&
	[ "$gem_lost" -eq $((gem_frames - 1)) ] ||
	fail "a spec's gemodel of p alone did not take netem's defaults" "$(cat "$root/gemodel.counts")"
# One for a segment the machine is not on is refused before it runs, and
# docker has no link of the core's to impair.
conf offwire "(network none) (segments \"on-$$\") (impair (off-$$ (out (loss 0.5)))) (command \"/probe\" \"say\" \"ran\")"
from_root 125 'impair names a segment the machine is not on' -f "$root/offwire.conf"
expect 1 'impair: rejected: a link is impaired in the core.s switch, which a container is never on' "$hull" plan --backend docker -f "$root/offwire.conf"
expect 1 'no machine is on that segment' "$hull" segment "nowhere-$$"

# A capture of the shouter's way out, started by hull segment during the
# run with a path relative to where it is asked, the core writing it whole:
# a pcap file of Ethernet whose frames include the shout, the probe's
# EtherType and its text; and one asked of a segment no machine is on
# refused.
capseg="capseg-$$"
printf '(machines (hearer %s) (shouter %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$capseg\") (command \"/probe\" \"link\" \"eth0\" \"hear\" \"eth0\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$capseg\") (command \"/probe\" \"link\" \"eth0\" \"sleep\" \"3\" \"shout\" \"eth0\" \"captured\")" \
	> "$root/capseg.conf"
rm -f "$root/capseg.pcap"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/capseg.conf" > "$root/capseg.out" 2>&1) &
caprun=$!
tries=0
until (cd "$root" && "$whole_hull" segment "$capseg" capture shouter out capseg.pcap frames=50) 2> "$root/capseg.err"; do
	tries=$((tries + 1))
	[ "$tries" -lt 40 ] || break
	sleep 0.25
done
wait "$caprun"
cases=$((cases + 1))
shouts=$(timeout 30 python3 - "$root/capseg.pcap" <<'PY' 2>&1
import struct, sys
data = open(sys.argv[1], "rb").read()
magic, major, minor, _, _, snap, link = struct.unpack("<IHHiIII", data[:24])
assert (magic, major, minor, link) == (0xa1b23c4d, 2, 4, 1), "not pcap of Ethernet, ns"
at, shouts = 24, 0
while at < len(data):
    sec, ns, size, whole = struct.unpack("<IIII", data[at:at + 16])
    frame = data[at + 16:at + 16 + size]
    assert size == whole == len(frame) and ns < 10**9, "a record not whole"
    shouts += frame[12:14] == b"\x88\xb5" and b"captured" in frame
    at += 16 + size
print(shouts)
PY
)
[ "$tries" -lt 40 ] && [ "$shouts" -ge 1 ] 2>/dev/null &&
	grep -q '^hearer  | hear=captured from 52:54:00:' "$root/capseg.out" ||
	fail "a capture of a machine's way out did not hold its shout" "$shouts $(cat "$root/capseg.err" "$root/capseg.out")"
expect 1 'no machine is on that segment' "$hull" segment "nowhere-$$" capture all in "$root/none.pcap"

# A targeted fault set at runtime: the shouter's frames whose payload has
# 'o' (its text's first byte) at offset 1 dropped, so the hearer hears
# nothing and the shouter's way out counts them matched and tampered.
tam="tam-$$"
printf '(machines (hearer %s) (shouter %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$tam\") (command \"/probe\" \"link\" \"eth0\" \"hear\" \"eth0\")" \
	"(source \"dir:./rootfs\") (backend chroot) (network none) (segments \"$tam\") (command \"/probe\" \"link\" \"eth0\" \"sleep\" \"2\" \"shout\" \"eth0\" \"over\")" \
	> "$root/tam.conf"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/tam.conf" > "$root/tam.out" 2>&1) &
tamrun=$!
cases=$((cases + 1))
tries=0
until "$hull" segment "$tam" impair shouter out 2>/dev/null &&
      "$hull" segment "$tam" tamper shouter out match=1:6f drop=1 2> "$root/tam.err"; do
	tries=$((tries + 1))
	[ "$tries" -lt 40 ] || break
	sleep 0.25
done
[ "$tries" -lt 40 ] || fail "a runtime tamper could not be set" "$(cat "$root/tam.err")"
tries=0
until "$hull" segment "$tam" > "$root/tam.counts" 2>&1 &&
      grep -Eq '^shouter +out .* matched=[1-9][0-9]* tampered=[1-9]' "$root/tam.counts"; do
	tries=$((tries + 1))
	[ "$tries" -lt 120 ] || break
	sleep 0.25
done
[ "$tries" -lt 120 ] || fail "a tamper's matched frames were not counted" "$(cat "$root/tam.counts")"
# A fault's bad shape is refused by the core while the segment is live.
expect 1 'OFFSET:HEX' "$hull" segment "$tam" tamper shouter out match=notanumber
"$hull" stop --timeout 5 hearer > /dev/null 2>&1
"$hull" stop --timeout 5 shouter > /dev/null 2>&1
wait "$tamrun" 2>/dev/null
# And in the spec, before the machine runs.
conf badtam "(network none) (segments \"bt-$$\") (tamper (bt-$$ (out (set \"0:ff\")))) (command \"/probe\" \"say\" \"ran\")"
from_root 125 'a set needs a rewrite probability' -f "$root/badtam.conf"

# The switch injecting a frame of its own: a broadcast of EtherType
# 0x88b5 carrying "test", from a source MAC no machine has, reaches the
# hearer and is counted injected, apart from what crosses the link.
inj="inj-$$"
conf injhear "(hostname \"injhear\") (network none) (segments \"$inj\") (command \"/probe\" \"link\" \"eth0\" \"hear\" \"eth0\" \"sleep\" \"5\")"
(cd / && exec timeout 60 "$whole_hull" run -f "$root/injhear.conf" > "$root/injhear.out" 2>&1) &
injrun=$!
cases=$((cases + 1))
tries=0
until "$hull" segment "$inj" > /dev/null 2>&1; do
	tries=$((tries + 1))
	[ "$tries" -lt 40 ] || break
	sleep 0.25
done
[ "$tries" -lt 40 ] || fail "the inject segment never came up" ""
# A machine not on the segment is refused while it is up.
expect 1 'that machine is not on the segment' "$hull" segment "$inj" inject nobody frame=ffffffffffff02000000009988b50474
"$hull" segment "$inj" inject all frame=ffffffffffff02000000009988b50474657374 count=5 every=40 2> "$root/inj.err" ||
	fail "inject was refused" "$(cat "$root/inj.err")"
tries=0
until grep -q '^hear=test from 02:00:00:00:00:99$' "$root/injhear.out" || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
grep -q '^hear=test from 02:00:00:00:00:99$' "$root/injhear.out" ||
	fail "an injected frame was not heard" "$(cat "$root/injhear.out")"
cases=$((cases + 1))
"$hull" segment "$inj" 2> /dev/null | grep -Eq '^injhear +in .* injected=[1-9]' ||
	fail "an injected frame was not counted" "$("$hull" segment "$inj" 2>&1)"
"$hull" stop --timeout 5 injhear > /dev/null 2>&1
wait "$injrun" 2>/dev/null
expect 1 'no machine is on that segment' "$hull" segment "gone-$$" inject all size=64 count=10
# A machine paused: every process of it stopped where it is, its ticks,
# a child's, ceasing while it is and going on after; a paused machine is
# shown so, and stopped at once, having been let go on first.
conf pausing '(hostname "pausing") (command "/probe" "ticks" "300")'
(cd / && exec timeout 60 "$whole_hull" run -f "$root/pausing.conf" > "$root/pausing.out" 2>&1) &
pauser=$!
ticks() { grep -c '^tick=' "$root/pausing.out"; }
tries=0
until [ "$(ticks)" -ge 3 ] || [ "$tries" -ge 80 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 '' "$hull" pause pausing
expect 0 '^pausing +127\.107\.[0-9.]+ +chroot +ns +paused$' "$hull" ps
expect 1 'the machine is paused already' "$hull" pause pausing
# Every process stopped, the ticking child's too, as the host sees them:
# the ticks reach here through the init, which relays them, so the init
# alone stopped would stop them too.
cases=$((cases + 1))
states=$(ps -eo stat=,args= | awk '$2 == "/probe" && $3 == "ticks" { print $1 }')
[ "$(printf '%s\n' "$states" | grep -c '^T')" = 2 ] && ! printf '%s\n' "$states" | grep -qv '^T' ||
	fail "a paused machine's processes were not all stopped" "$states"
sleep 0.3
before=$(ticks)
sleep 1.5
cases=$((cases + 1))
[ "$(ticks)" = "$before" ] || fail "a paused machine went on ticking" "$before, then $(ticks)"
expect 0 '' "$hull" unpause pausing
expect 1 'the machine is not paused' "$hull" unpause pausing
sleep 1.5
cases=$((cases + 1))
[ "$(ticks)" -ge $((before + 5)) ] || fail "an unpaused machine did not go on" "$before, then $(ticks)"
expect 0 '' "$hull" pause pausing
# The stop's TERM is heard at once, where a machine left stopped would
# hear only the KILL its timeout ends in, five seconds on.
started=$(date +%s)
expect 0 '' "$hull" stop --timeout 5 pausing
took=$(($(date +%s) - started))
cases=$((cases + 1))
[ "$took" -lt 3 ] || fail "a paused machine was stopped only by its timeout" "${took}s"
wait "$pauser"
cases=$((cases + 1))
! grep -q '^tick=300$' "$root/pausing.out" || fail "a paused machine was not stopped" "$(tail -3 "$root/pausing.out")"
expect 1 'no such machine' "$hull" pause nobody
expect 2 'is not an impairment' "$hull" segment "nowhere-$$" impair all in wobble=1
expect 2 'a direction is in, out or both' "$hull" segment "nowhere-$$" impair all sideways
conf csegment "(network host) (segments \"$wire\")" '(command "/probe" "say" "never")'
from_root 125 'a chroot machine on a segment needs a network namespace of its own: not \(network host\)' -f "$root/csegment.conf"
# Where the host gives the user no subordinate range, one uid is mapped
# and said once, with its way round: root in a user namespace of the
# user's own has none here, so hull run there, with a core of its own in
# a runtime directory of its own, which the suite's core outside would
# otherwise map from. Where many are mapped, nothing is said.
mkdir -p "$root/onerun"
chmod 700 "$root/onerun"
conf onemap '(mutable overlay) (command "/probe" "say" "ran")'
expect 0 '^hull: one uid is mapped, .* hull doctor gives the line that adds a range$' \
	unshare -r env XDG_RUNTIME_DIR="$root/onerun" sh -c 'cd / && exec "$0" run -f "$1"' \
	"$(cd "$(dirname "$hull")" && pwd)/$(basename "$hull")" "$root/onemap.conf"
from_root 0 '^ran$' -f "$root/onemap.conf"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'one uid is mapped' &&
	fail "one uid was said where many are mapped" "$out"
sleep 2
rm -rf "${root:?}/onerun"
expect 1 'no such machine' "$hull" stats nobody
expect 2 'usage' "$hull" stats

# One core: a second gives way to the first, says nothing, and leaves the
# first answering on its socket.
expect 0 '' "$hull" core
expect 0 '^kept +127\.107\.[0-9.]+ +chroot +ns +exited$' "$hull" ps

# A machine not kept dies with its core: nothing could manage it after.
# The probe is found by its arguments, which only this one has.
conf lone '(command "/probe" "say" "lone" "wait")' '(hostname "lone")'
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/lone.conf" > /dev/null 2>&1) &
keeper=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^lone .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
if ! "$hull" ps 2>/dev/null | grep -q '^lone .*running$' || [ -z "$core" ]; then
	fail "the machine never ran, so its core's death could not be tried" \
		"$("$hull" ps 2>&1)"
else
	# Looked for before the client is reaped: without the parent-death
	# signal the probe was measured alive a second after the core, and
	# gone only once the client was reaped, for a reason not found
	# (project.md section 4.10).
	kill -KILL "$core"
	sleep 0.5
	left=$(ps -eo pid=,args= | awk '$2 == "/probe" && $4 == "lone"')
	[ -z "$left" ] || fail "a machine not kept outlived its core" "$left"
	wait "$keeper" 2>/dev/null
fi
kill "$core_holder" 2>/dev/null
wait "$core_holder" 2>/dev/null

# A kept one outlives a core killed outright, and the core `hull ps`
# starts takes it back: what the workload wrote while no core held it,
# an exec, and its exit; then its file and directory gone. It writes
# `during` two seconds in, while no core holds it, as the test waits
# three after the kill, and ends eight seconds after that: four failed
# once on a loaded host, the core that took it back started after it
# had already ended, so it was taken back exited.
conf adopt '(command "/probe" "say" "before" "sleep" "2" "say" "during" "sleep" "8" "say" "after" "exit" "6")' \
	'(hostname "cadopt") (on_disconnect keep)'
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/adopt.conf" > /dev/null 2>&1) &
keeper=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^cadopt .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
kept_file=$XDG_RUNTIME_DIR/hull/machines/cadopt
cases=$((cases + 1))
[ -f "$kept_file" ] || fail "a kept machine has no file for the next core" "$(ls "$XDG_RUNTIME_DIR/hull")"
kill -KILL "$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock")" 2> /dev/null
wait "$keeper" 2>/dev/null
sleep 3
cases=$((cases + 1))
left=$(ps -eo pid=,args= | awk '$2 == "/probe" && $4 == "before"')
[ -n "$left" ] || fail "a kept machine died with its core" ""
cases=$((cases + 1))
out=$(timeout 10 "$hull" ps 2>&1)
printf '%s\n' "$out" | grep -q '^cadopt .*running$' &&
	grep -q '^hull core: machine cadopt is taken back$' "$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "the next core did not take the kept machine back" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
expect 0 '^in it$' "$hull" exec cadopt /probe say "in it"
# Another kept machine started meanwhile sweeps dead cores' directories,
# and the one the taken-back machine uses, named for the core that went,
# is not one of them.
conf kbrief '(command "/probe" "say" "brief")' '(hostname "kbrief") (on_disconnect keep)'
from_root 0 '^brief$' -f "$root/kbrief.conf"
cases=$((cases + 1))
adopted_dir=$(grep -a '^dir ' "$kept_file" | cut -d' ' -f2-)
[ -S "$adopted_dir/init.sock" ] ||
	fail "a taken-back machine's directory was swept" "$adopted_dir: $(ls "$adopted_dir" 2>&1)"
cases=$((cases + 1))
logs=$(timeout 20 "$hull" logs -f cadopt 2>&1)
printf '%s\n' "$logs" | grep -q '^during$' && printf '%s\n' "$logs" | grep -q '^after$' ||
	fail "what a kept machine wrote while no core held it was lost" "$logs"
expect 6 '' "$hull" attach cadopt
cases=$((cases + 1))
sleep 1
dirs=$(ls -d "$XDG_RUNTIME_DIR"/hull/chroot-* 2>/dev/null)
[ ! -e "$kept_file" ] && [ -z "$dirs" ] ||
	fail "a taken-back machine's file or directory is left" "$(ls "$XDG_RUNTIME_DIR/hull/machines") $dirs"
# A kept machine whose workload ends while no core holds it waits for the
# next, and its exit, 7, is that core's to tell.
conf cended '(command "/probe" "say" "up" "sleep" "2" "exit" "7")' \
	'(hostname "cended") (on_disconnect keep)'
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/cended.conf" > /dev/null 2>&1) &
keeper=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^cended .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
kill -KILL "$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock")" 2> /dev/null
wait "$keeper" 2>/dev/null
sleep 3
expect 7 '' "$hull" attach cended
cases=$((cases + 1))
sleep 1
[ ! -e "$XDG_RUNTIME_DIR/hull/machines/cended" ] && [ -z "$(ls -d "$XDG_RUNTIME_DIR"/hull/chroot-* 2>/dev/null)" ] ||
	fail "a kept machine that ended with no core was left" "$(ls "$XDG_RUNTIME_DIR/hull")"
# A kept file whose pid is not its machine's init, as once the pid has
# gone to another process, is forgotten, and that process let be: a
# sleep of the test's own, read by a core that starts once the last has
# left, 10 s at most.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
sleep 30 &
stranger=$!
state=$(printf 'pid %s\ndir %s/hull/chroot-1-1\nplan\n%s' "$stranger" "$XDG_RUNTIME_DIR" \
	'(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(7:command6:/probe)(6:source12:dir:./rootfs)(7:workdir1:/)))')
printf '(4:kept(7:machine6:cfaked)(6:engine6:chroot)(5:class2:ns)(5:state%d:%s))' \
	"${#state}" "$state" > "$XDG_RUNTIME_DIR/hull/machines/cfaked"
cases=$((cases + 1))
out=$(timeout 10 "$hull" ps 2>&1)
! printf '%s\n' "$out" | grep -q '^cfaked ' &&
	grep -q '^hull core: machine cfaked is forgotten: its init is gone$' "$XDG_RUNTIME_DIR/hull/core.log" &&
	kill -0 "$stranger" 2> /dev/null ||
	fail "a kept file naming another process was believed, or it was harmed" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
kill "$stranger" 2> /dev/null
wait "$stranger" 2> /dev/null

# A core keeps its place: its socket removed while it runs a machine, it
# binds it again within a second or two, so frontends still reach the
# machine rather than starting a core of their own; its lock file
# replaced, which would let a second core start beside it, it says so in
# core.log. Seen once as a machine running in one core while ps and pause
# reached another, from a cause not found (project.md section 4.10).
conf placed '(command "/probe" "wait")' '(hostname "placed") (network none)'
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/placed.conf" > /dev/null 2>&1) &
placer=$!
tries=0
until "$hull" ps 2>/dev/null | grep -q '^placed .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
rm -f "$XDG_RUNTIME_DIR/hull/core.sock"
tries=0
until "$hull" ps 2>/dev/null | grep -q '^placed .*running$' || [ "$tries" -ge 20 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
"$hull" ps 2>/dev/null | grep -q '^placed .*running$' &&
	grep -q '^hull core [0-9]*: core\.sock was removed while this core ran; bound again$' \
		"$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "a core whose socket was removed was not reached again" \
		"$("$hull" ps 2>&1); $(tail -5 "$XDG_RUNTIME_DIR/hull/core.log")"
rm -f "$XDG_RUNTIME_DIR/hull/core.lock"
: > "$XDG_RUNTIME_DIR/hull/core.lock"
tries=0
until grep -q '^hull core [0-9]*: core\.lock is no longer the file this core locked' \
	"$XDG_RUNTIME_DIR/hull/core.log" || [ "$tries" -ge 20 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
grep -q '^hull core [0-9]*: core\.lock is no longer the file this core locked' \
	"$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "a core whose lock file was replaced did not say so" \
		"$(tail -5 "$XDG_RUNTIME_DIR/hull/core.log")"
"$hull" stop --timeout 2 placed > /dev/null 2>&1
wait "$placer" 2>/dev/null
# Its core, idle, goes, so the next starts on the new lock file and
# writes its pid there, which the cases after read.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done

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
conf noargs '(workdir "/srv")'
from_root 125 'needs \(command \.\.\.\)' -f "$root/noargs.conf"
conf nope '(command "/nope")'
from_root 125 '/nope: No such file or directory' -f "$root/nope.conf"
# A bare command is searched for in the machine's PATH, as docker does:
# past a directory not there and a file of its name that will not run, to
# the one that will; a file that will not run, alone, is EACCES, and a name
# found nowhere, docker's default PATH searched, ENOENT. exec searches so.
printf 'not a program\n' > "$root/rootfs/srv/searchme"
chmod 644 "$root/rootfs/srv/searchme"
cp "$root/rootfs/probe" "$root/rootfs/bin/searchme"
conf searched '(command "searchme" "say" "found") (env (PATH "/nowhere:/srv:/bin"))'
from_root 0 '^found$' -f "$root/searched.conf"
conf denied '(command "searchme") (env (PATH "/srv"))'
from_root 125 'searchme: Permission denied' -f "$root/denied.conf"
conf unfound '(command "nowhere-at-all")'
from_root 125 'nowhere-at-all: No such file or directory' -f "$root/unfound.conf"

# (network host) is the host's, asked for: a listener on the host's
# loopback is reached. (network none) is a namespace of the machine's own,
# lo up and nothing else, so the same dial is refused, and an exec in it
# joins it. A vm cannot share the host's network.
rm -f "$root/listen.out"
(timeout -k 2 60 python3 -c '
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(4)
s.settimeout(50)
print(s.getsockname()[1], flush=True)
while True:
	c, _ = s.accept()
	c.sendall(b"the host loopback\n")
	c.close()
' > "$root/listen.out" 2>&1) &
host_listener=$!
tries=0
until [ -s "$root/listen.out" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
listen_port=$(head -1 "$root/listen.out")
conf nethost "(command \"/probe\" \"dial\" \"127.0.0.1\" \"$listen_port\") (network host)"
from_root 0 "^dial 127\\.0\\.0\\.1:$listen_port=the host loopback\$" -f "$root/nethost.conf"
conf netnone "(command \"/probe\" \"dial\" \"127.0.0.1\" \"$listen_port\") (network none)"
from_root 0 "^dial 127\\.0\\.0\\.1:$listen_port=Connection refused\$" -f "$root/netnone.conf"
# No (network ...) is a network of the machine's own, pasta's NAT out, as a
# vm's is passt's (section 8): its own lo, so a dial to its loopback is
# refused; the host's loopback reached neither directly nor through the
# gateway, --no-map-gw; the host's own address reached; and the host's
# resolver at 10.0.2.3, which its resolv.conf names. A second listener, on
# the host's own address, takes one connection and says who came.
hostaddr=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
[ -n "$hostaddr" ] || hostaddr=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -m1 -E '^[0-9.]+$')
rm -f "$root/natlisten.out"
(timeout -k 2 40 python3 -c '
import socket, sys
s = socket.socket()
s.bind((sys.argv[1], 0))
s.listen(1)
s.settimeout(30)
print(s.getsockname()[1], flush=True)
c, _ = s.accept()
c.sendall(b"hello from the host\n")
c.close()
' "${hostaddr:-127.0.0.1}" > "$root/natlisten.out" 2>&1) &
nat_listener=$!
tries=0
until [ -s "$root/natlisten.out" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
outside_port=$(head -1 "$root/natlisten.out")
conf netnat "(mutable overlay) (command \"/probe\" \"read\" \"/etc/resolv.conf\" \"dial\" \"127.0.0.1\" \"$listen_port\" \"dial\" \"10.0.2.2\" \"$listen_port\" \"dial\" \"${hostaddr:-127.0.0.1}\" \"$outside_port\" \"resolve\" \"10.0.2.3\" \"hull-test.invalid\" \"addr\" \"eth0\")"
from_root 0 "^dial 127\\.0\\.0\\.1:$listen_port=Connection refused\$" -f "$root/netnat.conf"
nat_out=$out
cases=$((cases + 1))
printf '%s\n' "$nat_out" | grep -q '^read=nameserver 10\.0\.2\.3$' &&
	printf '%s\n' "$nat_out" | grep -q '^resolve=rcode [0-9]* answers [0-9]*$' &&
	printf '%s\n' "$nat_out" | grep -q '^addr=eth0 10\.0\.2\.15/24 up$' &&
	! printf '%s\n' "$nat_out" | grep -q '=the host loopback$' ||
	fail "a chroot machine of no network key was not given pasta's NAT, or reached the host's loopback" "$nat_out"
if [ -n "$hostaddr" ]; then
	cases=$((cases + 1))
	printf '%s\n' "$nat_out" | grep -q "^dial ${hostaddr}:${outside_port}=hello from the host\$" ||
		fail "a chroot machine under NAT did not reach the host's own address" "$nat_out"
else
	printf 'cli: the host has no address but loopback; the dial out under NAT is not tried\n'
fi
kill "$nat_listener" 2>/dev/null
wait "$nat_listener" 2>/dev/null
# (network nat) says the default outright: on chroot the same network of
# the machine's own, pasta's eth0 and its own loopback; and every engine
# takes it, as passt's on a vm and docker's bridge.
conf natsaid "(network nat) (command \"/probe\" \"addr\" \"eth0\" \"dial\" \"127.0.0.1\" \"$listen_port\")"
from_root 0 '^addr=eth0 10\.0\.2\.15/24 up$' -f "$root/natsaid.conf"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q "^dial 127\\.0\\.0\\.1:$listen_port=Connection refused\$" ||
	fail "(network nat) on chroot was not a network of the machine's own" "$out"
conf natplan '(network nat) (mutable overlay) (ports (port (host 8080) (guest 80))) (command "/probe")'
expect 0 'machine on docker \(ns\): ok' "$hull" plan -f "$root/natplan.conf" --backend docker
expect 0 'machine on qemu \(vm\): ok' "$hull" plan -f "$root/natplan.conf" --backend vm
# Its ports, forwarded by pasta from the machine's own 127.107 address into
# its network: a connection from the host answered by what serves inside;
# and ports refused with (network host), where a port is the host's own,
# and with (network none), which has no network to put one on.
nat_port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
nat_at=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' natports)
conf natports "(hostname \"natports\") (ports (port (host $nat_port) (guest 80))) (command \"/probe\" \"serve\" \"tcp\" \"80\")"
rm -f "$root/natports.out"
(cd / && exec timeout -k 2 40 "$hull_abs" run -f "$root/natports.conf" > "$root/natports.out" 2>&1) &
natrun=$!
cases=$((cases + 1))
answer=$(timeout 30 python3 -c '
import socket, sys, time
deadline = time.time() + 25
while time.time() < deadline:
    try:
        s = socket.create_connection((sys.argv[1], int(sys.argv[2])), 2)
        s.sendall(b"hello\n")
        s.settimeout(5)
        got = s.recv(200)
        if got:
            print(got.decode().strip())
            break
    except OSError:
        time.sleep(0.2)
' "$nat_at" "$nat_port")
wait "$natrun"
[ "$answer" = "hello from the machine" ] && grep -q '^served=hello$' "$root/natports.out" ||
	fail "a chroot machine's port under NAT was not reached at its address" "$answer $(cat "$root/natports.out")"
conf hostports '(network host) (command "/probe") (ports (port (host 8080) (guest 80)))'
from_root 125 'ports need a network of the machine.s own: \(network host\) is the host.s' -f "$root/hostports.conf"
conf noneports '(network none) (command "/probe") (ports (port (host 8080) (guest 80)))'
from_root 125 'ports need the machine.s network, which \(network none\) has not' -f "$root/noneports.conf"
# Where neither the spec nor the image names a PATH, the workload is
# given the one it was searched for on, sbins and all, as docker gives a
# container one: a shell's own is not exported, and gcc, which finds cc1
# from where it was found, could not without it. One the spec names is
# its own.
conf pathless '(command "/probe" "env" "PATH")'
from_root 0 '^PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin$' -f "$root/pathless.conf"
conf pathed '(env (PATH "/opt/only")) (command "/probe" "env" "PATH")'
from_root 0 '^PATH=/opt/only$' -f "$root/pathed.conf"
# Many uids where the user has a subordinate range and the helpers that
# map one: a chown inside an overlay machine to an id of the range works,
# as a postinst's to its package's own user must, and the file is that
# id's there. A directory used in place keeps one uid, an owner there
# being one on the user's disk; and without a range, so does an overlay.
me=$(id -un)
if grep -q "^$me:" /etc/subuid 2>/dev/null && grep -q "^$me:" /etc/subgid 2>/dev/null &&
	[ -u /usr/bin/newuidmap ] && [ -u /usr/bin/newgidmap ]; then
	owned_chown='^chown=ok$'
	owned_owner='^owner=101:101 0644$'
else
	owned_chown='^chown=Invalid argument$'
	owned_owner='^owner=0:0 0644$'
fi
conf owned '(mutable overlay) (command "/probe" "write" "/owned" "chown" "/owned" "101" "owner" "/owned")'
from_root 0 "$owned_chown" -f "$root/owned.conf"
from_root 0 "$owned_owner" -f "$root/owned.conf"
conf ownedin '(command "/probe" "chown" "/probe" "101")'
from_root 0 '^chown=Invalid argument$' -f "$root/ownedin.conf"
# (export PATH): an overlay machine's tree as a tar once its workload
# ends, each owner as the machine saw it, so a file it gave another
# owner is 101's in the tar and on no disk as 101's; its /proc is an empty
# directory there, its own mount being left out. The tar commits with the
# owner kept. In place there is nothing to export: the tree is the
# user's own.
rm -f "$root/exported.tar"
conf exported "(mutable overlay) (export \"$root/exported.tar\")" \
	'(command "/probe" "write" "/made" "chown" "/made" "101")'
from_root 0 "$owned_chown" -f "$root/exported.conf"
cases=$((cases + 1))
said=$(timeout 30 python3 -c '
import sys, tarfile
t = tarfile.open(sys.argv[1])
names = {m.name: m for m in t}
print("made", names["made"].uid if "made" in names else "-")
print("probe", names["probe"].uid if "probe" in names else "-")
print("proc", "empty" if "proc" in names and not any(n.startswith("proc/") for n in names) else "not")' \
	"$root/exported.tar" 2>&1)
case $owned_chown in
*ok*) made_uid=101 ;;
*) made_uid=0 ;;
esac
[ "$said" = "$(printf 'made %s\nprobe 0\nproc empty' "$made_uid")" ] ||
	fail "an export's tree is not as the machine saw it" "$said"
expect 0 '^exported/1 sha256:' "$hull" commit --tar "$root/exported.tar" exported/1
cases=$((cases + 1))
timeout 30 "$hull" manifest show exported/1 > "$root/exported.view" 2>&1
if [ "$made_uid" = 101 ]; then
	grep -qF '(owner (gid 101) (uid 101)) (path "made")' "$root/exported.view" ||
		fail "an export's owner was not kept by its commit" "$(grep made "$root/exported.view")"
fi
# (owners PATH): owners given back before the workload, in the overlay,
# the file on the user's disk staying the user's: what lets a stored
# tree, all the user's in its checkout, run with the owners its manifest
# keeps. Where one uid is mapped they cannot be, and it is said.
# At the root, in the user's own group, as a stored tree's checkout is:
# the build directory here is setgid to a group the namespace does not
# map, and overlayfs cannot copy up a file, nor the directories above
# it, of an unmapped group.
printf 'given\n' > "$root/rootfs/given"
chgrp "$(id -g)" "$root/rootfs/given"
printf '101\000102\000given\000' > "$root/owners.list"
conf owned_back "(mutable overlay) (owners \"$root/owners.list\")" '(command "/probe" "owner" "/given")'
case $owned_chown in
*ok*) from_root 0 '^owner=101:102 0[0-7]{3}$' -f "$root/owned_back.conf" ;;
*) from_root 0 'hull: owners: 1 of 1 not given' -f "$root/owned_back.conf" ;;
esac
cases=$((cases + 1))
[ "$(stat -c %u "$root/rootfs/given")" = "$(id -u)" ] ||
	fail "an owner given in a machine reached the user's disk" "$(ls -ln "$root/rootfs/given")"
rm -f "$root/rootfs/given"
conf ownedin_back "(owners \"$root/owners.list\")" '(command "/probe" "say" "no")'
from_root 125 'owners are given above an overlay' -f "$root/ownedin_back.conf"
# The same, end to end from a stored image: `hull run` of a bare ref the
# store holds writes the owners list from the manifest itself, not a
# hand-written (owners ...), so an image runs with the owners its manifest
# keeps rather than every file the user's. A tar with a file of 101:102 is
# committed, so the manifest holds it; the run checks the tree out, all the
# user's, and gives the owner back above the overlay. bbq-predictor's case,
# which the engine half alone did not reach.
timeout 30 python3 - "$root/rootfs/probe" "$root/ownedrun.tar" <<'PYEOF'
import io, sys, tarfile
probe, out = sys.argv[1], sys.argv[2]
t = tarfile.open(out, "w", format=tarfile.PAX_FORMAT)
def add(name, kind, uid, gid, mode, data=b""):
	i = tarfile.TarInfo(name)
	i.type, i.uid, i.gid, i.mode, i.size = kind, uid, gid, mode, len(data)
	t.addfile(i, io.BytesIO(data) if data else None)
for d in ("proc", "dev", "sys"):
	add(d, tarfile.DIRTYPE, 0, 0, 0o755)
with open(probe, "rb") as f:
	add("probe", tarfile.REGTYPE, 0, 0, 0o755, f.read())
add("given", tarfile.REGTYPE, 101, 102, 0o644, b"given\n")
t.close()
PYEOF
expect 0 '^ownedrun/1 sha256:' "$hull" commit --tar "$root/ownedrun.tar" ownedrun/1
printf '(machine (source "ownedrun/1") (backend chroot) (command "/probe" "owner" "/given"))\n' \
	> "$root/ownedrun.conf"
case $owned_chown in
*ok*) from_root 0 '^owner=101:102 0[0-7]{3}$' -f "$root/ownedrun.conf" ;;
*) from_root 0 '^owner=0:0 0[0-7]{3}$' -f "$root/ownedrun.conf" ;;
esac
# (layers ...) and (upper PATH): a template read-only beneath, a layer
# above it, and the writes kept in a directory of the user's that has
# files of its own, as unionfs did. The top layer wins; what is written
# lands in the upper, a removal there as a whiteout, and the template is
# untouched; a second run sees what the first left.
rm -rf "${root:?}/template" "${root:?}/site" "${root:?}/data" "${root:?}/.data.hull-work"
mkdir -p "$root/template/proc" "$root/site" "$root/data"
cp "$probe" "$root/template/probe"
printf 'template\n' > "$root/template/a"
printf 'site\n' > "$root/site/a"
printf 'b\n' > "$root/site/b"
printf 'c\n' > "$root/data/c"
# In the user's own group, as a directory of the user's is: this build
# tree is setgid to a group the namespace does not map, and a file of such
# a group cannot be copied up, EOVERFLOW, as a removal or a write over it
# does. The upper's own group is no matter; its work directory's is,
# which hull makes, and the case after this holds that.
chgrp -R "$(id -g)" "$root/template" "$root/site" "$root/data"
chmod g-s "$root/template" "$root/site" "$root/data" "$root"
printf '(machine (source "dir:%s/template") (backend chroot) (mutable overlay) %s %s)\n' "$root" \
	"(layers \"dir:$root/site\") (upper \"$root/data\")" \
	'(command "/probe" "read" "/a" "read" "/b" "read" "/c" "write" "/d" "remove" "/b")' > "$root/layered.conf"
from_root 0 '^read=site$' -f "$root/layered.conf"
cases=$((cases + 1))
[ "$(cat "$root/template/a")" = template ] && [ -e "$root/site/b" ] && [ -e "$root/data/d" ] &&
	[ -c "$root/data/b" ] ||
	fail "a layered machine's writes did not land in its upper alone" \
		"$(ls -la "$root/template" "$root/site" "$root/data" 2>&1)"
printf '(machine (source "dir:%s/template") (backend chroot) (mutable overlay) %s %s)\n' "$root" \
	"(layers \"dir:$root/site\") (upper \"$root/data\")" \
	'(command "/probe" "read" "/b" "list" "/")' > "$root/relayered.conf"
from_root 0 '^read=No such file or directory$' -f "$root/relayered.conf"
from_root 0 '^list=.* a c d .*probe' -f "$root/relayered.conf"
# An upper inside a directory setgid to another of the user's groups, as
# a shared one is: overlayfs refuses a work directory of a group the
# namespace does not map, and the one hull makes beside the upper would
# inherit it, so hull gives it the user's own. The machine runs.
other_group=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
if [ -n "$other_group" ]; then
	mkdir -p "$root/shared"
	chgrp "$other_group" "$root/shared" && chmod g+s "$root/shared"
	printf '(machine (source "dir:%s/template") (backend chroot) (mutable overlay) %s %s)\n' "$root" \
		"(upper \"$root/shared/data\")" '(command "/probe" "write" "/kept")' > "$root/sharedupper.conf"
	from_root 0 '^write=ok$' -f "$root/sharedupper.conf"
	cases=$((cases + 1))
	[ -e "$root/shared/data/kept" ] || fail "a write did not land in an upper in a shared directory" \
		"$(ls -la "$root/shared" "$root/shared/data" 2>&1)"
	rm -rf "${root:?}/shared"
fi
conf layeredin "(layers \"dir:$root/site\")" '(command "/probe" "say" "no")'
from_root 125 'layers lie under an overlay' -f "$root/layeredin.conf"
# A volume that is a stack of its own, at a path in the machine, on a
# root used in place: the same template, site and upper as above, at
# /mnt. The top layer wins, writes and a whiteout land in the upper
# alone, and a later run sees them: each from_root is a run of its own,
# so the removal is seen by the run after the first.
rm -rf "${root:?}/data" "${root:?}/.data.hull-work"
mkdir -p "$root/data"
printf 'c\n' > "$root/data/c"
chgrp -R "$(id -g)" "$root/data"
conf volstack '(command "/probe" "read" "/mnt/a" "read" "/mnt/c" "write" "/mnt/d" "remove" "/mnt/b")' \
	"(volumes (volume (guest \"/mnt\") (layers \"dir:$root/template\" \"dir:$root/site\") (upper \"$root/data\")))"
from_root 0 '^read=site$' -f "$root/volstack.conf"
from_root 0 '^read=c$' -f "$root/volstack.conf"
cases=$((cases + 1))
[ "$(cat "$root/template/a")" = template ] && [ -e "$root/site/b" ] && [ -e "$root/data/d" ] &&
	[ ! -e "$root/template/d" ] && [ ! -e "$root/site/d" ] && [ ! -e "$root/rootfs/mnt/d" ] ||
	fail "a stacked volume's writes did not land in its upper alone" \
		"$(ls -la "$root/template" "$root/site" "$root/data" "$root/rootfs/mnt" 2>&1)"
cases=$((cases + 1))
[ -x "$root/.data.hull-work/work" ] ||
	fail "a kept upper's work directory was left unremovable" "$(ls -la "$root/.data.hull-work" 2>&1)"
conf volrestack '(command "/probe" "read" "/mnt/b" "read" "/mnt/d")' \
	"(volumes (volume (guest \"/mnt\") (layers \"dir:$root/template\" \"dir:$root/site\") (upper \"$root/data\")))"
from_root 0 '^read=No such file or directory$' -f "$root/volrestack.conf"
# With no upper, its writes are memory: written, and nowhere on disk.
conf volmem '(command "/probe" "write" "/mnt/e" "read" "/mnt/a")' \
	"(volumes (volume (guest \"/mnt\") (layers \"dir:$root/template\" \"dir:$root/site\")))"
from_root 0 '^write=ok$' -f "$root/volmem.conf"
cases=$((cases + 1))
[ ! -e "$root/template/e" ] && [ ! -e "$root/site/e" ] && [ ! -e "$root/data/e" ] &&
	[ ! -e "$root/rootfs/mnt/e" ] || fail "a stacked volume's write in memory reached a disk" ""
# Read-only, of two layers and of one, the second being the layer bound.
conf volstackro '(command "/probe" "read" "/mnt/a" "write" "/mnt/f")' \
	"(volumes (volume (guest \"/mnt\") (layers \"dir:$root/template\" \"dir:$root/site\") (mode ro)))"
from_root 0 '^read=site$' -f "$root/volstackro.conf"
from_root 0 '^write=Read-only file system$' -f "$root/volstackro.conf"
conf volstackone '(command "/probe" "read" "/mnt/a" "write" "/mnt/f")' \
	"(volumes (volume (guest \"/mnt\") (layers \"dir:$root/template\") (mode ro)))"
from_root 0 '^read=template$' -f "$root/volstackone.conf"
from_root 0 '^write=Read-only file system$' -f "$root/volstackone.conf"
cases=$((cases + 1))
[ ! -e "$root/template/f" ] && [ ! -e "$root/site/f" ] || fail "a read-only stack was written" ""
# Each machine's directory, where its stacks were staged, is gone with it.
cases=$((cases + 1))
left=$(ls "$XDG_RUNTIME_DIR/hull" 2>/dev/null | grep '^chroot-')
[ -z "$left" ] || fail "a stacked volume's machine directory was left" "$left"
rm -rf "${root:?}/template" "${root:?}/site" "${root:?}/data" "${root:?}/.data.hull-work"
conf exportin "(export \"$root/never.tar\")" '(command "/probe" "say" "no")'
from_root 125 'an export is of an overlay.s tree' -f "$root/exportin.conf"
conf nested '(command "/probe" "nest")'
from_root 0 '^nest=ok$' -f "$root/nested.conf"
conf nestover '(mutable overlay) (command "/probe" "nest")'
from_root 0 '^nest=ok$' -f "$root/nestover.conf"
conf netexec '(command "/probe" "say" "ready" "wait") (network none) (hostname "netexec") (on_disconnect stop)'
rm -f "$root/netexec.out"
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/netexec.conf" > "$root/netexec.out" 2>&1) &
netexec=$!
tries=0
until grep -q '^ready$' "$root/netexec.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 "^dial 127\\.0\\.0\\.1:$listen_port=Connection refused\$" \
	"$hull" exec netexec /probe dial 127.0.0.1 "$listen_port"
# Inside a machine, and inside an exec, a user namespace of its own can
# be made, as `unshare -rn` makes one: the machine's root is its mount
# namespace's, by pivot_root, where under chroot(2) the kernel refused.
expect 0 '^nest=ok$' "$hull" exec netexec /probe nest
expect 0 '' "$hull" kill -s KILL netexec
wait "$netexec" 2>/dev/null
# An exec in a machine under NAT joins its network namespace too: it sees
# the machine's eth0 at pasta's address, not the host's interfaces.
conf natexec '(command "/probe" "say" "ready" "wait") (hostname "natexec") (on_disconnect stop)'
rm -f "$root/natexec.out"
(cd / && exec timeout -k 2 30 "$hull_abs" run -f "$root/natexec.conf" > "$root/natexec.out" 2>&1) &
natexec=$!
tries=0
until grep -q '^ready$' "$root/natexec.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 '^addr=eth0 10\.0\.2\.15/24 up$' "$hull" exec natexec /probe addr eth0
expect 0 '' "$hull" kill -s KILL natexec
wait "$natexec" 2>/dev/null
# cpus and memory: a systemd scope the init is moved into before its
# workload, asked of the manager by busctl. Faked here, with a core of its
# own so that it sees the fake on its PATH: what it is asked, a manager
# that refuses, and one that says yes and never moves the init, which is
# refused rather than run unlimited. Measured for real where a manager
# answers on the user's bus.
conf limited '(cpus 2) (memory "64M") (command "/probe" "read" "/proc/self/cgroup")'
expect 0 'cpus: native: a systemd scope.s CPUQuota' \
	"$hull" plan --explain -f "$root/limited.conf"
expect 0 'memory: native: a ceiling, a systemd scope.s MemoryMax' \
	"$hull" plan --explain -f "$root/limited.conf"
rm -rf "${root:?}/limrun" "${root:?}/limtools"
mkdir -p "$root/limrun" "$root/limtools"
chmod 700 "$root/limrun"
printf '#!/bin/sh\necho "Failed to connect to bus: No such file or directory" >&2\nexit 1\n' \
	> "$root/limtools/busctl"
chmod +x "$root/limtools/busctl"
limit=20
expect 125 'cpus and memory are a systemd scope, and the user.s manager gave none: Failed to connect to bus' \
	env PATH="$root/limtools:$PATH" XDG_RUNTIME_DIR="$root/limrun" "$hull" run -f "$root/limited.conf"
printf '#!/bin/sh\nprintf "%%s\\n" "$*" > "%s/busctl.args"\necho "o \\"/org/freedesktop/systemd1/job/1\\""\n' \
	"$root" > "$root/limtools/busctl"
limit=20
expect 125 'cpus and memory: the scope hull-[0-9]+\.scope did not take the machine.s init in 5 s' \
	env PATH="$root/limtools:$PATH" XDG_RUNTIME_DIR="$root/limrun" "$hull" run -f "$root/limited.conf"
cases=$((cases + 1))
grep -Eq '^--user call org\.freedesktop\.systemd1 /org/freedesktop/systemd1 org\.freedesktop\.systemd1\.Manager StartTransientUnit ssa\(sv\)a\(sa\(sv\)\) hull-[0-9]+\.scope fail 3 PIDs au 1 [0-9]+ CPUQuotaPerSecUSec t 2000000 MemoryMax t 67108864 0$' \
	"$root/busctl.args" 2>/dev/null ||
	fail "cpus and memory: the manager asked for something else" "$(cat "$root/busctl.args" 2>&1)"
# The probe reads a file's first line, which is the unified hierarchy's
# only where no v1 controller is mounted beside it.
if busctl --user status > /dev/null 2>&1 && [ "$(wc -l < /proc/self/cgroup)" = 1 ]; then
	from_root 0 '^read=0::/.*hull-[0-9]+\.scope$' -f "$root/limited.conf"
else
	printf 'cli: cpus and memory not measured for real: no systemd manager on the user.s bus, or cgroup v1 beside v2\n' >&2
fi
sleep 2
rm -rf "${root:?}/limrun" "${root:?}/limtools" "${root:?}/busctl.args"

devconf() { # NAME ENTRIES...: a machine of devroot, whose tree has a /dev
	name=$1
	shift
	printf '(machine (source "dir:./devroot") (backend chroot) %s)\n' "$*" > "$root/$name.conf"
}
# (devices ...): the host's nodes bound into the machine's /dev, found
# when it starts. /dev/net/tun is a device every user may open, and read
# with no interface attached says EBADFD, which a plain file cannot; and
# /dev/char/10:200 a link to it, as /dev/serial/by-id is to a port.
devconf devnode '(devices (node "/dev/net/tun")) (command "/probe" "read" "/dev/net/tun" "kind" "/dev/net/tun")'
expect 0 'devices.node: native: the host.s node, bound into /dev' \
	"$hull" plan --explain -f "$root/devnode.conf"
# Rejected for its direct dir source, which docker cannot run, and not
# for its device.
expect 1 'devices.node: native: the host.s node by --device' \
	"$hull" plan --explain -f "$root/devnode.conf" --backend docker
expect 1 'devices.node: rejected: a host.s device node has no device of a guest.s to be' \
	"$hull" plan -f "$root/devnode.conf" --backend vm
conf devserial '(devices (serial "/dev/ttyS0") (parallel "/dev/parport0") (usb (device (vendor "0403") (product "6001")))) (command "/probe")'
expect 0 'devices.serial: native: an ISA serial port' \
	"$hull" plan --explain -f "$root/devserial.conf" --backend vm
# What may not work is said where it is planned and run, without asking:
# a vm's USB and parallel port are qemu's, their timing not the host's.
expect 0 'devices.usb: emulated: usb-host on the guest.s xHCI, every transfer re-issued by qemu.*chroot is the host.s own timing$' \
	"$hull" plan -f "$root/devserial.conf" --backend vm
expect 0 'devices.parallel: emulated: .*a bit-banged device, a JTAG cable, may not work; chroot reaches ppdev directly$' \
	"$hull" plan -f "$root/devserial.conf" --backend emu
cases=$((cases + 1))
"$hull" plan -f "$root/devnode.conf" 2>&1 | grep -q 'devices' &&
	fail "chroot's devices, the host's own timing, warned of" "$("$hull" plan -f "$root/devnode.conf" 2>&1)"
conf devbare '(devices (node "/dev/net/tun")) (command "/probe")'
if [ -r /dev/net/tun ] && [ -w /dev/net/tun ]; then
	from_root 0 '^read=File descriptor in bad state$' -f "$root/devnode.conf"
	from_root 0 '^kind=chr 10:200$' -f "$root/devnode.conf"
	if [ -L /dev/char/10:200 ]; then
		devconf devlink '(devices (node "/dev/char/10:200")) (command "/probe" "read" "/dev/char/10:200" "kind" "/dev/char/10:200")'
		from_root 0 '^read=File descriptor in bad state$' -f "$root/devlink.conf"
		from_root 0 '^kind=lnk$' -f "$root/devlink.conf"
	fi
	# One machine's at a time: a port named (serial ...) is held while its
	# machine runs, a second refused, and given once the first is gone;
	# the same node as (node ...) is shared.
	devconf devheld '(hostname "held") (devices (serial "/dev/net/tun")) (command "/probe" "say" "holding" "sleep" "4")'
	devconf devsecond '(hostname "second") (devices (serial "/dev/net/tun")) (command "/probe" "say" "second")'
	devconf devshared '(hostname "shared") (devices (node "/dev/net/tun")) (command "/probe" "say" "shared")'
	(cd / && timeout 20 "$whole_hull" run -f "$root/devheld.conf" > "$root/devheld.out" 2>&1 &)
	tries=0
	until grep -q '^holding$' "$root/devheld.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	from_root 125 '/dev/net/tun is another running machine.s: a USB device, serial or parallel port is one machine.s at a time' \
		-f "$root/devsecond.conf"
	from_root 0 '^shared$' -f "$root/devshared.conf"
	tries=0
	until ! "$hull" ps 2>/dev/null | grep -q '^held ' || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	from_root 0 '^second$' -f "$root/devsecond.conf"
	rm -f "$root/devheld.out"
	# Refused, not dropped, where the tree has no /dev to bind them into.
	from_root 125 '\(devices \.\.\.\) are bound into the machine.s /dev, and its tree has no /dev' \
		-f "$root/devbare.conf"
fi
conf devfile "(devices (node \"$root/devnode.conf\")) (command \"/probe\")"
from_root 125 'devnode.conf is not a device under /dev' -f "$root/devfile.conf"
conf devnone '(devices (serial "/dev/hull-none")) (command "/probe")'
from_root 125 '/dev/hull-none: No such file or directory' -f "$root/devnone.conf"
if [ "$(id -u)" != 0 ] && [ -e /dev/ttyS0 ] && [ ! -w /dev/ttyS0 ]; then
	conf devlocked '(devices (serial "/dev/ttyS0")) (command "/probe")'
	from_root 125 '/dev/ttyS0 is root:[a-z]+ [0-7]+, which the user may not read and write' \
		-f "$root/devlocked.conf"
fi
# PCI by VFIO: planned on every engine; a device of the host's own bound
# to its host driver refused, naming it, as nothing here is vfio-pci's.
conf devpci '(devices (pci "0000:00:00.0")) (command "/probe")'
expect 0 'devices.pci: native: its VFIO group.s node and /dev/vfio/vfio bound in' \
	"$hull" plan --explain -f "$root/devpci.conf"
expect 0 'devices.pci: native: vfio-pci: the device itself' \
	"$hull" plan --explain -f "$root/devpci.conf" --backend vm
bound_pci=
for d in /sys/bus/pci/devices/*; do
	driver=$(readlink "$d/driver" 2>/dev/null) || continue
	[ "${driver##*/}" != vfio-pci ] && [ -L "$d/iommu_group" ] && { bound_pci=${d##*/}; break; }
done < /dev/null
if [ -n "$bound_pci" ]; then
	devconf devpcibound "(devices (pci \"$bound_pci\")) (command \"/probe\")"
	from_root 125 "PCI device $bound_pci is bound to ${driver##*/}, not vfio-pci" -f "$root/devpcibound.conf"
fi
conf devpcinone '(devices (pci "00ff:ff:1f.7")) (command "/probe")'
from_root 125 'no PCI device 00ff:ff:1f.7 is on the host' -f "$root/devpcinone.conf"
conf devusb '(devices (usb (device (vendor "ffff") (product "fffe")))) (command "/probe")'
from_root 125 'no USB device ffff:fffe is plugged in' -f "$root/devusb.conf"
# A USB device of the host's that only root, or a group, may write.
locked_usb=
for d in /sys/bus/usb/devices/[0-9]*; do
	case $d in *:*) continue ;; esac
	[ -r "$d/busnum" ] && [ -r "$d/devnum" ] || continue
	n=$(printf '/dev/bus/usb/%03d/%03d' "$(cat "$d/busnum")" "$(cat "$d/devnum")")
	if [ -e "$n" ] && [ ! -w "$n" ]; then
		locked_usb=${d##*/}
		break
	fi
done < /dev/null
if [ -n "$locked_usb" ] && [ "$(id -u)" != 0 ]; then
	conf devusblocked "(devices (usb (device (port \"$locked_usb\")))) (command \"/probe\")"
	from_root 125 '/dev/bus/usb/[0-9]+/[0-9]+ is root:[a-z]+ [0-7]+, which the user may not read and write' \
		-f "$root/devusblocked.conf"
fi

# (network bridge): a veth on a host bridge, which only root may put
# there, so only a core run as root takes it; as this user, refused. The
# root case runs as root in a user namespace of its own, `unshare -rmn`,
# whose network is its own too, so hull0, its masquerade and forwarding
# are made there and not on the host: two machines on hull's own bridge,
# each at 172.30.x.y for its 127.107.x.y, one dialling the other's port
# and both the gateway; the bridge, its nft table and forwarding left as
# docker leaves docker0; a port of a machine on hull0 reached from its
# 127.107 address, pasta splicing it to the machine's bridge address; a
# machine on a bridge of its owner's naming, up and unaddressed; one
# naming none refused; and ports on a named bridge refused, the machine's
# address there being its own.
conf bridged '(mutable overlay) (network bridge) (command "/probe" "say" "never")'
from_root 125 '\(network bridge\) needs hull.s core to run as root: \(network nat\)' -f "$root/bridged.conf"
bridge_at() { # ID: the address hull's bridge gives it, from its 127.107 one
	python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("172.30.%d.%d" % (h // 254, h % 254 + 1))' "$1"
}
bdb_at=$(bridge_at bdb)
printf '(machines (bweb %s) (bdb %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network bridge) (command \"/probe\" \"addr\" \"eth0\" \"sleep\" \"1\" \"dial\" \"$bdb_at\" \"80\" \"dial\" \"172.31.0.1\" \"9\" \"dial\" \"10.200.0.1\" \"8081\" \"read\" \"/etc/resolv.conf\" \"dns\" \"anything.example\")" \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network bridge) (command \"/probe\" \"addr\" \"eth0\" \"serve\" \"tcp\" \"80\")" \
	> "$root/bridge2.conf"
printf '(machine (source "dir:./rootfs") (backend chroot) (network bridge) (bridge "brtest") (command "/probe" "addr" "eth0"))\n' \
	> "$root/brnamed.conf"
printf '(machine (source "dir:./rootfs") (backend chroot) (network bridge) (bridge "nosuch") (command "/probe"))\n' \
	> "$root/brnone.conf"
printf '(machine (source "dir:./rootfs") (backend chroot) (network bridge) (bridge "brtest") (ports (port (host 8080) (guest 80))) (command "/probe"))\n' \
	> "$root/brports.conf"
bport_at=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' bport)
# The image's sshd, the probe standing in for it, on hull0: reached from
# the machine's 127.107 address at its port, as under NAT.
mkdir -p "$root/brssh/run" "$root/brssh/proc" "$root/brssh/usr/sbin"
cp "$root/rootfs/probe" "$root/brssh/probe"
cp "$root/rootfs/probe" "$root/brssh/usr/sbin/sshd"
printf '(machine (source "dir:./brssh") (backend chroot) (hostname "bsshd") (network bridge) (ssh true) (command "/probe" "sleep" "8"))\n' \
	> "$root/brsshd.conf"
printf '(machine (source "dir:./rootfs") (backend chroot) (hostname "bport") (network bridge) (ports (port (host 18096) (guest 80))) (command "/probe" "serve" "tcp" "80"))\n' \
	> "$root/brserve.conf"
# A resolver of the test's own on the namespace's loopback, which is all a
# host with systemd-resolved gives: hull0's relay is what reaches it. It
# answers every question with one A, 192.0.2.7.
printf 'nameserver 127.0.0.53\n' > "$root/relay-resolv.conf"
cat > "$root/fakedns.py" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.53", 53))
s.settimeout(float(sys.argv[1]))
while True:
    try:
        q, a = s.recvfrom(4096)
    except socket.timeout:
        break
    end = 12
    while q[end] != 0:
        end += 1 + q[end]
    end += 5
    r = q[:2] + b"\x81\x80" + q[4:6] + b"\x00\x01\x00\x00\x00\x00" + q[12:end]
    r += b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x05\x00\x04" + bytes([192, 0, 2, 7])
    s.sendto(r, a)
PY
rm -rf "${root:?}/brrun"
mkdir -p "$root/brrun"
chmod 700 "$root/brrun"
cases=$((cases + 1))
bridge_out=$(timeout 120 unshare -rmn --kill-child sh -c '
export XDG_RUNTIME_DIR="$2/brrun" HULL_CORE_IDLE=1
ip=$(command -v ip || echo /usr/sbin/ip)
[ -x "$ip" ] || ip=/sbin/ip
mount --bind "$2/relay-resolv.conf" /etc/resolv.conf
timeout 100 python3 "$2/fakedns.py" 90 &
"$ip" link set lo up
# Beyond the bridge, reached only by the route out through the gateway
# that the machine is given: an address of this namespace, a listener.
"$ip" addr add 10.200.0.1/32 dev lo
timeout 60 python3 -c "
import socket
s = socket.socket()
s.bind((\"10.200.0.1\", 8081))
s.listen(1)
s.settimeout(50)
c, _ = s.accept()
c.sendall(b\"hello from beyond the bridge\\n\")
c.close()
" &
cd /
echo "== pair"; timeout 40 "$1" run -f "$2/bridge2.conf" 2>&1
echo "== bridge"; "$ip" -br addr show hull0
echo "== nft"; /usr/sbin/nft list table ip hull 2>&1 | grep masquerade
echo "== forward=$(cat /proc/sys/net/ipv4/ip_forward)"
"$ip" link add brtest type bridge && "$ip" link set brtest up
echo "== served"
(timeout 30 "$1" run -f "$2/brserve.conf" > "$2/brserve.out" 2>&1 &)
timeout 25 python3 -c "
import socket, sys, time
deadline = time.time() + 20
while time.time() < deadline:
    try:
        s = socket.create_connection((sys.argv[1], 18096), 2)
        s.sendall(b\"hello\\n\")
        print(\"bridged port:\", s.recv(100).decode().strip())
        break
    except OSError:
        time.sleep(0.2)
" "$3"
sleep 1
cat "$2/brserve.out"
echo "== sshd"
mkdir -p "$2/brrun/shm" && mount -t tmpfs none "$2/brrun/shm" && chmod 700 "$2/brrun/shm"
(XDG_RUNTIME_DIR="$2/brrun/shm" timeout 30 "$1" run -f "$2/brsshd.conf" > /dev/null 2>&1 &)
tries=0
until XDG_RUNTIME_DIR="$2/brrun/shm" "$1" ps 2>/dev/null | grep -q "^bsshd .*running" || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
XDG_RUNTIME_DIR="$2/brrun/shm" timeout 10 "$1" proxy-ssh bsshd < /dev/null 2>&1 | head -1
XDG_RUNTIME_DIR="$2/brrun/shm" timeout 10 "$1" kill -s KILL bsshd > /dev/null 2>&1
echo "== named"; timeout 30 "$1" run -f "$2/brnamed.conf" 2>&1
echo "== none"; timeout 30 "$1" run -f "$2/brnone.conf" 2>&1
echo "== ports"; timeout 30 "$1" run -f "$2/brports.conf" 2>&1
' sh "$hull_abs" "$root" "$bport_at" 2>&1)
rm -rf "${root:?}/brrun"
bweb_at=$(bridge_at bweb)
printf '%s\n' "$bridge_out" | grep -q "^bweb *| addr=eth0 $(printf '%s' "$bweb_at" | sed 's/\./\\./g')/15 up\$" &&
	printf '%s\n' "$bridge_out" | grep -q "^bdb *| addr=eth0 $(printf '%s' "$bdb_at" | sed 's/\./\\./g')/15 up\$" &&
	printf '%s\n' "$bridge_out" | grep -q "^bweb *| dial $(printf '%s' "$bdb_at" | sed 's/\./\\./g'):80=hello from the machine\$" &&
	printf '%s\n' "$bridge_out" | grep -q '^bweb *| dial 172\.31\.0\.1:9=Connection refused$' &&
	printf '%s\n' "$bridge_out" | grep -q '^bweb *| dial 10\.200\.0\.1:8081=hello from beyond the bridge$' &&
	printf '%s\n' "$bridge_out" | grep -q '^bweb *| read=nameserver 172\.31\.0\.1$' &&
	printf '%s\n' "$bridge_out" | grep -q '^bweb *| dns=anything\.example 192\.0\.2\.7$' &&
	printf '%s\n' "$bridge_out" | grep -Eq '^hull0 +UP +172\.31\.0\.1/15' &&
	printf '%s\n' "$bridge_out" | grep -q 'ip saddr 172\.30\.0\.0/15 oifname != "hull0" masquerade' &&
	printf '%s\n' "$bridge_out" | grep -q '^== forward=1$' ||
	fail "machines on hull's own bridge, as root, were not addressed, reached and NATed" "$bridge_out"
cases=$((cases + 1))
printf '%s\n' "$bridge_out" | grep -q '^addr=eth0 none/0 up$' &&
	printf '%s\n' "$bridge_out" | grep -q '(bridge NAME) names no interface on the host' &&
	printf '%s\n' "$bridge_out" | grep -q 'ports on a bridge (bridge NAME) names are not forwarded' ||
	fail "a named bridge, a missing one, or ports on a named bridge were not as said" "$bridge_out"
cases=$((cases + 1))
printf '%s\n' "$bridge_out" | grep -q '^bridged port: hello from the machine$' &&
	printf '%s\n' "$bridge_out" | grep -q '^served=hello$' ||
	fail "a port of a machine on hull0 was not reached at its 127.107 address" "$bridge_out"
cases=$((cases + 1))
printf '%s\n' "$bridge_out" | grep -A1 '^== sshd$' | grep -q '^SSH-2\.0-hull-stub' ||
	fail "an sshd on hull0 was not reached at the machine's 127.107 address" "$bridge_out"
conf brdocker '(mutable overlay) (network bridge) (bridge "mynet") (command "/probe")'
expect 0 'machine on docker \(ns\): ok' "$hull" plan -f "$root/brdocker.conf" --backend docker
expect 0 'machine on qemu \(vm\): ok' "$hull" plan -f "$root/brdocker.conf" --backend vm
kill "$host_listener" 2>/dev/null
wait "$host_listener" 2>/dev/null
printf '(machine (source "dir:./rootfs") (backend vm) (network host) (command "/probe"))\n' \
	> "$root/vmhost.conf"
expect 1 'network\.host: rejected: a vm shares no network namespace with the host' \
	"$hull" plan -f "$root/vmhost.conf"
printf '%s\n' '(machines (alpha (source "dir:./rootfs") (backend chroot) (command "/probe" "host")))' \
	> "$root/pair.conf"
from_root 0 '^host=alpha$' -f "$root/pair.conf" alpha
from_root 125 'no such machine in \(machines \.\.\.\)' -f "$root/pair.conf" omega
# With no name, every machine of the file at once, a line at a time
# behind its name, the names padded to the longest.
m='(source "dir:./rootfs") (backend chroot)'
printf '(machines (alpha %s (command "/probe" "host")) (beta %s (command "/probe" "host" "err" "to stderr")) (gamma %s (command "/probe" "bare" "unended")))\n' \
	"$m" "$m" "$m" > "$root/whole.conf"
from_root 0 '^alpha \| host=alpha$' -f "$root/whole.conf"
from_root 0 '^beta  \| host=beta$' -f "$root/whole.conf"
from_root 0 '^beta  \| to stderr$' -f "$root/whole.conf"
# The machine's stderr is hull's stderr, not mixed into its stdout.
refute 'to stderr' sh -c 'cd / && exec "$0" run -f "$1" 2>/dev/null' "$hull_abs" "$root/whole.conf"
# A last line with no newline is not lost when its machine ends.
from_root 0 '^gamma \| unended$' -f "$root/whole.conf"
# The first to fail ends the run with its status, and stops the rest
# rather than leaving them to run out.
printf '(machines (quitter %s (command "/probe" "sleep" "1" "exit" "3")) (sleeper %s (command "/probe" "sleep" "60" "say" "woke")))\n' \
	"$m" "$m" > "$root/quit.conf"
limit=20
from_root 3 '' -f "$root/quit.conf"
refute 'woke' sh -c 'cd / && exec "$0" run -f "$1"' "$hull_abs" "$root/quit.conf"
# One the file cannot run starts none of it.
printf '(machines (alpha %s (command "/probe" "host")) (omega (source "dir:./rootfs") (backend vm) (network host) (command "/probe")))\n' \
	"$m" > "$root/refused.conf"
from_root 125 'omega on qemu \(vm\): not run: the plan is rejected' -f "$root/refused.conf"
refute 'host=alpha' sh -c 'cd / && exec "$0" run -f "$1"' "$hull_abs" "$root/refused.conf"
from_root 125 'run takes one backend' -f "$root/pair.conf" --backend ns,vm alpha

# And the core leaves once nothing needs it.
cases=$((cases + 1))
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
[ ! -S "$XDG_RUNTIME_DIR/hull/core.sock" ] || fail "the core did not leave when idle" ""
# And not before its second after its last client: five cores of a
# directory of their own, each left by a client 0.7 s after its hello. An
# idle clock of whole seconds starts at the second of the hello, so once a
# tick falls before the client leaves, the core leaves with it.
cases=$((cases + 1))
mkdir -p "$root/idle"
chmod 700 "$root/idle"
lived=$(timeout 60 python3 -c '
import os, socket, struct, subprocess, sys, time
run = sys.argv[2]
env = dict(os.environ, XDG_RUNTIME_DIR=run, HULL_CORE_IDLE="1")
for _ in range(5):
	core = subprocess.Popen([sys.argv[1], "core"], env=env, cwd="/",
	                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	s = socket.socket(socket.AF_UNIX)
	s.settimeout(5)
	for _ in range(100):
		try:
			d = os.open(run + "/hull", os.O_PATH)
			s.connect("/proc/self/fd/%d/core.sock" % d)
			break
		except OSError:
			time.sleep(0.05)
	m = b"(5:hello(2:ch1:0)(5:proto1:1))"
	s.sendall(struct.pack(">I", len(m)) + m)
	s.recv(4096)
	time.sleep(0.7)
	s.close()
	left = time.monotonic()
	core.wait(timeout=10)
	print("%d" % ((time.monotonic() - left) * 1000))
' "$hull_abs" "$root/idle" 2>&1)
[ "$(printf '%s\n' "$lived" | awk '$1 >= 900 && $1 < 5000' | wc -l)" = 5 ] ||
	fail "an idle core did not live its second after its last client, or lived past it" "$lived"

# What a core that went kept for the next, which `hull ps` starts, though
# it starts none otherwise. No daemon answers here, so a docker machine is
# left for a later core; a file no core wrote, one whose name is not its
# machine's, and one whose state is not its engine's are forgotten.
# core.log says which.
kept=$XDG_RUNTIME_DIR/hull/machines
printf 'not a kept machine' > "$kept/junk"
printf '(4:kept(7:machine5:other)(6:engine6:docker)(5:class2:ns)(5:state2:ab))' > "$kept/named"
printf '(4:kept(7:machine6:chroot)(6:engine6:chroot)(5:class2:ns)(5:state1:x))' > "$kept/chroot"
printf '(4:kept(7:machine6:docker)(6:engine6:docker)(5:class2:ns)(5:state4:abcd))' > "$kept/docker"
printf '(4:kept' > "$kept/half.new"
expect 0 '^ID ' "$hull" ps
cases=$((cases + 1))
said=$(cat "$XDG_RUNTIME_DIR/hull/core.log")
printf '%s\n' "$said" | grep -q '^hull core: machine junk is forgotten: it is not what a core writes$' &&
	printf '%s\n' "$said" | grep -q '^hull core: machine named is forgotten: it is not what a core writes$' &&
	printf '%s\n' "$said" | grep -q '^hull core: machine chroot is forgotten: the state is not this engine.s$' &&
	printf '%s\n' "$said" | grep -q '^hull core: machine docker is left for the next core: the Docker daemon at .* cannot be reached$' &&
	[ "$(ls "$kept")" = docker ] ||
	fail "the kept machines were not read as they are" "$(ls "$kept"); core.log: $said"
rm -f "$kept/docker"
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done

# Secrets (J.3), resolved by the frontend and written by the core, on a
# tmpfs: so a core of their own, in a runtime directory under /dev/shm,
# removed at the end. In this suite's runtime directory, when it is not a
# tmpfs, the run is refused; where $BUILD_DIR is one, that is said rather
# than tried.
conf nosecret '(command "/probe" "say" "ran")' '(secrets (pw (source "file:pw.txt")))'
printf 'from a file\n' > "$root/pw.txt"
case $(findmnt -no FSTYPE --target "$XDG_RUNTIME_DIR") in
tmpfs | ramfs)
	printf 'cli: not tried, as %s is in memory: a runtime directory that is not refuses secrets\n' \
		"$XDG_RUNTIME_DIR" ;;
*)
	from_root 125 'secrets are kept in memory only' -f "$root/nosecret.conf" ;;
esac
shm=$(mktemp -d /dev/shm/hull-cli-test.XXXXXX)
trap 'rm -rf "${shm:?}"' EXIT
mkdir -p "$root/secret/run" "$root/secret/proc"
cp "$root/rootfs/probe" "$root/secret/probe"
head -c 70000 /dev/zero > "$root/big.bin"
secret_conf() { # NAME COMMAND SECRETS: a machine of the secret dir
	printf '(machine (source "dir:./secret") (backend chroot) (hostname "sec") (command %s) (secrets %s))\n' \
		"$2" "$3" > "$root/$1.conf"
}
secret_run() { # STATUS PATTERN CONF: hull run of CONF in the shm core
	expect "$1" "$2" env XDG_RUNTIME_DIR="$shm" sh -c 'cd / && exec "$0" run -f "$1"' \
		"$hull_abs" "$root/$3.conf"
}
if [ "$(findmnt -no FSTYPE --target "$shm")" != tmpfs ]; then
	cases=$((cases + 1))
	fail "/dev/shm is not a tmpfs, so secrets cannot be tried" "$(findmnt --target "$shm")"
fi
# A stale directory, as a core that died leaves, is gone once a core
# starts.
mkdir -p "$shm/hull/secrets/ghost"
printf 'old\n' > "$shm/hull/secrets/ghost/key"
secret_conf shown '"/probe" "list" "/run/hull/secrets" "read" "/run/hull/secrets/pw" "read" "/run/hull/secrets/made" "owner" "/run/hull/secrets/pw" "env" "made" "write" "/run/hull/secrets/new"' \
	'(pw (source "file:pw.txt")) (made (source "exec:printf made; echo to the terminal >&2") (expose env))'
secret_run 0 '^list= made pw$' shown
secret_run 0 '^read=from a file$' shown
# made has no newline, so the read runs into the owner's line.
secret_run 0 '^read=madeowner=0:0 0400$' shown
secret_run 0 '^made=made$' shown
secret_run 0 '^write=Read-only file system$' shown
secret_run 0 '^to the terminal$' shown
secret_run 0 '^hull: warning: secret made is in the machine.s environment' shown
cases=$((cases + 1))
left=$(ls -A "$shm/hull/secrets")
[ -z "$left" ] || fail "secrets were left after their machine, or a stale one kept" "$left"
# What cannot be had is the run refused, before anything runs.
secret_conf failing '"/probe" "say" "ran"' '(bad (source "exec:exit 3"))'
secret_run 125 'secret bad: /bin/sh exited 3' failing
refute '^ran$' env XDG_RUNTIME_DIR="$shm" sh -c 'cd / && exec "$0" run -f "$1"' "$hull_abs" "$root/failing.conf"
secret_conf nofile '"/probe" "say" "ran"' '(gone (source "file:nope.txt"))'
secret_run 125 'secret gone: .*nope.txt: No such file' nofile
secret_conf big '"/probe" "say" "ran"' '(big (source "file:big.bin"))'
secret_run 125 'secret big: .*more than 65536 bytes' big
secret_conf sealed '"/probe" "say" "ran"' '(sealed (source "age:nope.age"))'
secret_run 125 'secret sealed: age' sealed
# A dir with no /run has nowhere for them.
printf '(machine (source "dir:./rootfs") (backend chroot) (command "/probe" "say" "ran") %s)\n' \
	'(secrets (pw (source "file:pw.txt")))' > "$root/norun.conf"
secret_run 125 'a tmpfs at /run, which the dir must have' norun
# What the core refuses that our frontend never sends, said by hand to a
# core of the tmpfs's own, held five idle seconds: a value twice, one the
# plan does not name, one missing, values for a plan with no secrets, and
# a plan that brings its own directory.
(env XDG_RUNTIME_DIR="$shm" HULL_CORE_IDLE=5 "$hull_abs" core > /dev/null 2>&1 &)
tries=0
until [ -S "$shm/hull/core.sock" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
plan_with='(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(7:command6:/probe)(7:secrets(2:pw(6:source10:file:x.txt)))(6:source12:dir:./secret)))'
plan_without='(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(7:command6:/probe)(6:source12:dir:./secret)))'
raw() { # PATTERN PLAN SECRETS: a run of PLAN with SECRETS after it
	expect 0 "$1" env XDG_RUNTIME_DIR="$shm" python3 -c "$speak
say(b'(3:run(2:ch1:1)(7:machine3:raw)$2$3)')
hear(b'6:reason')
"
}
raw "a secret's value is given twice" "$plan_with" '(7:secrets(2:pw1:a)(2:pw1:b))'
raw 'the run gives a secret the plan does not name' "$plan_with" '(7:secrets(2:pw1:a)(5:other1:b))'
raw 'a secret the plan names has no value' "$plan_with" '(7:secrets(5:other1:b))'
raw 'the run gives a secret the plan does not name' "$plan_without" '(7:secrets(2:pw1:a))'
raw "a plan's secrets' directory is the core's to give" \
	'(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(6:source12:dir:./secret))(7:secrets4:/tmp))' ''
cases=$((cases + 1))
left=$(ls -A "$shm/hull/secrets")
[ -z "$left" ] || fail "a refused run left its secrets" "$left"
# Its core leaves, as this suite's did, before its directory is removed.
tries=0
while [ -S "$shm/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done

# (ssh true) on the chroot engine, on a tmpfs core of its own (the host key
# is kept in memory, so it needs one). The image's own sshd is supervised on
# the machine's own 127.107 address at a high port (section 8.1): the
# unprivileged user cannot bind 22 on the shared host net. proxy-ssh reaches
# it over the host loopback directly, with no agent, and the core writes the
# user's ~/.ssh public key in as the machine's authorized_keys. Refused with
# (network none), which leaves no host network to reach it on.
mkdir -p "$root/sshroot/run" "$root/sshroot/proc" "$root/sshroot/usr/sbin"
cp "$root/rootfs/probe" "$root/sshroot/probe"
cp "$root/rootfs/probe" "$root/sshroot/usr/sbin/sshd"
printf '(machine (source "dir:./sshroot") (backend chroot) (hostname "csshd") (ssh true) (on_disconnect keep) (command "/probe" "wait"))\n' \
	> "$root/csshd.conf"
(cd / && exec env XDG_RUNTIME_DIR="$shm" "$hull_abs" run -f "$root/csshd.conf" > /dev/null 2>&1) &
csshder=$!
tries=0
until env XDG_RUNTIME_DIR="$shm" "$hull" ps 2>/dev/null | grep -q '^csshd .*running$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect 0 '^SSH-2\.0-hull-stub' env XDG_RUNTIME_DIR="$shm" sh -c 'cd / && exec "$0" proxy-ssh csshd < /dev/null' \
	"$hull_abs"
expect 0 'hulltestkey' env XDG_RUNTIME_DIR="$shm" "$hull" exec csshd /probe read /run/hull/ssh/authorized_keys
env XDG_RUNTIME_DIR="$shm" "$hull" stop csshd > /dev/null 2>&1
kill -KILL "$csshder" 2>/dev/null
wait "$csshder" 2>/dev/null
printf '(machine (source "dir:./sshroot") (backend chroot) (hostname "cnossh") (network none) (ssh true) (command "/probe" "wait"))\n' \
	> "$root/cnossh.conf"
expect 125 '\(ssh true\) needs a network to be reached on: not with \(network none\)' \
	env XDG_RUNTIME_DIR="$shm" sh -c 'cd / && exec "$0" run -f "$1"' "$hull_abs" "$root/cnossh.conf"
tries=0
while [ -S "$shm/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done

# The dos class, on each engine of it installed here, DOSBox and dosemu2:
# the directory as C:, a program's output through DOS and its exit status,
# a read-only volume as the drive its guest path names, env by SET, a
# command not there 127, a stop at once, and nothing left in the user's
# HOME. The programs are a few bytes of 8086: HELLO prints a line by INT
# 21h, EXIT3 ends with 3, LOOP jumps to itself. The lines end CR LF, as
# DOS's do, so the patterns anchor at the start alone. What differs is
# each shell's word for a command not there, and dosemu2 keeping D: and
# E: for itself.
mkdir -p "$root/dosroot" "$root/dosvol" "$root/home"
printf '\264\011\272\014\001\315\041\270\000\114\315\041hello from dos\r\n$' > "$root/dosroot/HELLO.COM"
printf '\270\003\114\315\041' > "$root/dosroot/EXIT3.COM"
printf '\353\376' > "$root/dosroot/LOOP.COM"
printf 'from a volume\r\n' > "$root/dosvol/IN.TXT"
for dosengine in dosbox dosemu; do
	if ! command -v "$dosengine" > /dev/null 2>&1; then
		echo "cli: the dos class on $dosengine not measured: $dosengine is not installed"
		continue
	fi
	case $dosengine in
	dosbox) dosnone='^Illegal command: NOSUCH\.' ;;
	dosemu) dosnone='^Bad command or file name - NOSUCH' ;;
	esac
	dosconf() { # NAME ENTRIES...
		name=$1
		shift
		printf '(machine (source "dir:./dosroot") (backend %s) %s)\n' "$dosengine" "$*" \
			> "$root/$name.conf"
	}
	dosconf doshello '(command "HELLO")'
	dosconf dosexit '(command "EXIT3")'
	dosconf dosvol '(volumes (volume (host "./dosvol") (guest "/g") (mode ro))) (command "TYPE" "G:\\IN.TXT")'
	dosconf dosenv '(env (GREETING "hi")) (command "SET")'
	dosconf dosnone '(command "NOSUCH")'
	dosconf dosloop '(command "LOOP")'
	dosconf dosmnt '(volumes (volume (host "./dosvol") (guest "/mnt"))) (command "HELLO")'
	dosconf dosport '(ports (port (host 8080) (guest 80))) (command "HELLO")'
	expect 0 '^.*source\.dir\.direct: native: the directory is drive C:$' \
		"$hull" plan --explain -f "$root/doshello.conf"
	expect 1 'ports: rejected' "$hull" plan -f "$root/dosport.conf"
	limit=90
	from_root 0 '^hello from dos' -f "$root/doshello.conf"
	from_root 3 '' -f "$root/dosexit.conf"
	from_root 0 '^from a volume' -f "$root/dosvol.conf"
	from_root 0 '^GREETING=hi' -f "$root/dosenv.conf"
	from_root 127 "$dosnone" -f "$root/dosnone.conf"
	from_root 125 'guest path is its drive, /d to /z' -f "$root/dosmnt.conf"
	# (mutable overlay): C: a copy, which the command writes and the
	# user's tree does not see.
	dosconf doscopy '(mutable overlay) (command "COPY" "HELLO.COM" "NEW.COM")'
	from_root 0 '[Cc]opied' -f "$root/doscopy.conf"
	cases=$((cases + 1))
	[ ! -e "$root/dosroot/NEW.COM" ] ||
		fail "an overlay dos machine on $dosengine wrote the user's tree" "$(ls "$root/dosroot")"
	if [ "$dosengine" = dosemu ]; then
		dosconf dosown '(volumes (volume (host "./dosvol") (guest "/d"))) (command "HELLO")'
		from_root 125 'whose own commands and shell are D: and E:' -f "$root/dosown.conf"
	fi
	(cd / && exec timeout -k 2 60 "$hull_abs" run -f "$root/dosloop.conf" > /dev/null 2>&1) &
	dosrun=$!
	tries=0
	until "$hull" ps 2>/dev/null | grep -q " $dosengine +dos +running\$" || [ "$tries" -ge 80 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	dosid=$("$hull" ps 2>/dev/null | awk -v e="$dosengine" '$3 == e && $5 == "running" { print $1; exit }')
	started=$(date +%s)
	"$hull" stop --timeout 3 "$dosid" > /dev/null 2>&1
	took=$(($(date +%s) - started))
	wait "$dosrun" 2> /dev/null
	cases=$((cases + 1))
	[ -n "$dosid" ] && [ "$took" -lt 3 ] && ! "$hull" ps 2>/dev/null | grep -q " $dosengine +dos +running\$" ||
		fail "a dos machine on $dosengine was not stopped at once" "'$dosid' after ${took}s"
	cases=$((cases + 1))
	[ ! -e "$HOME/.dosbox" ] && [ ! -e "$HOME/.dosemu" ] && [ ! -e "$HOME/.config/pulse" ] ||
		fail "$dosengine wrote into the user's HOME" "$(ls -A "$HOME")"
	cases=$((cases + 1))
	left=$(ls -d "$XDG_RUNTIME_DIR"/hull/"$dosengine"-* 2> /dev/null)
	[ -z "$left" ] || fail "$dosengine machines left their directories" "$left"
done

printf 'cli: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
