#!/bin/sh
# docker_test.sh -- build/hull run on the docker adapter, against the
# Docker daemon this account can reach. Not part of `make check`, which
# needs no daemon: `make check-docker` runs it.
#
# Its arguments are build/hull, the static probe, build/test/tree_tar and
# build/hull-root.
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
root_helper=$(cd "$(dirname "$4")" && pwd)/$(basename "$4")
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
# Root mode's helper, run as the user against a listing and a log of the
# suite's own (section 7); the core takes them from its environment when
# it starts, so they are set before anything starts it.
HULL_ROOT_HELPER=$root_helper
HULL_ROOT_CONF=$root/root.conf
HULL_ROOT_LOG=$root/root.log
export HULL_ROOT_HELPER HULL_ROOT_CONF HULL_ROOT_LOG
: > "$root/root.conf"
: > "$root/root.log"
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
# (user ...) is the daemon's User, its root refused as the host's.
conf asuser '(command "sh" "-c" "echo uid=$(id -u):$(id -g)") (user "4321:4322")'
expect 0 '^uid=4321:4322$' run -f "$root/asuser.conf"
conf tmpfsro '(command "sh" "-c" "echo x > /scratch/x && cat /scratch/x") (tmpfs "/scratch")' \
	'(security (readonly_root true))'
expect 0 '^x$' run -f "$root/tmpfsro.conf"
# (stop_signal ...) is the daemon's StopSignal.
conf stopq '(command "sh" "-c" "trap \"echo got-quit; exit 0\" QUIT; echo up; sleep 20 & wait")' \
	'(stop_signal "SIGQUIT")'
timeout 40 "$hull" run -f "$root/stopq.conf" > "$root/stopq.out" 2>&1 &
pid=$!
n=0
while [ "$n" -lt 30 ] && ! grep -q '^up$' "$root/stopq.out" 2> /dev/null; do
	sleep 1
	n=$((n + 1))
done
"$hull" stop dtest > /dev/null 2>&1
wait "$pid"
cases=$((cases + 1))
grep -q 'got-quit' "$root/stopq.out" || fail 'a container stopped by (stop_signal ...)' "$(tail -3 "$root/stopq.out")"
# (hosts ...), (dns ...), (sysctls ...), (ulimits ...) as the daemon's.
conf names '(command "sh" "-c" "grep db.internal /etc/hosts; grep -c 9.9.9.9 /etc/resolv.conf; cat /proc/sys/net/core/somaxconn; ulimit -n")' \
	'(hosts "db.internal:10.0.0.5") (dns "9.9.9.9") (sysctls "net.core.somaxconn=1234") (ulimits "nofile=2048:4096")'
expect 0 '^10.0.0.5' run -f "$root/names.conf"
expect 0 '^1234$' run -f "$root/names.conf"
expect 0 '^2048$' run -f "$root/names.conf"
conf asroot '(command "true") (user "root")'
expect 125 'a container.s root is the host.s' run -f "$root/asroot.conf"
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
# (export PATH): the container's root as a tar once its workload ends,
# docker export's, with what the workload wrote, as its uid, and no
# volume; it commits as chroot's does. A file that cannot be made is
# refused before anything runs.
rm -f "$root/exported.tar"
conf exported "(export \"$root/exported.tar\")" \
	'(command "sh" "-c" "echo made here > /tmp/made; cat /data/in")' \
	'(volumes (volume (host "./data") (guest "/data")))'
expect 0 '^from the host$' exported
cases=$((cases + 1))
said=$(timeout 30 python3 -c '
import sys, tarfile
t = tarfile.open(sys.argv[1])
names = {m.name.rstrip("/"): m for m in t}
made = names.get("tmp/made")
print("made", t.extractfile(made).read().decode().strip() if made else "-", made.uid if made else "-")
print("volume", "data/in" in names)
print("shell", "bin/sh" in names)
print("docker", ".dockerenv" in names or "sbin/docker-init" in names)' "$root/exported.tar" 2>&1)
[ "$said" = "$(printf 'made made here %s\nvolume False\nshell True\ndocker False' "$(id -u)")" ] ||
	fail "an export's tree is not the container's" "$said"
cases=$((cases + 1))
committed=$(timeout 60 "$hull" commit --tar "$root/exported.tar" dexported/1 2>&1)
printf '%s\n' "$committed" | grep -q '^dexported/1 sha256:' ||
	fail "a docker export did not commit" "$committed"
# hull commit of a running container: docker's export of it now,
# filtered as (export PATH)'s -- its write there, its volume's file not,
# and none of docker's own files.
conf dlive '(command "sh" "-c" "echo live > /tmp/live; echo ready; sleep 30")' \
	'(volumes (volume (host "./data") (guest "/data")))'
rm -f "$root/dlive.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/dlive.conf" > "$root/dlive.out" 2>&1) &
runner=$!
tries=0
until grep -q '^ready$' "$root/dlive.out" 2>/dev/null || [ "$tries" -ge 80 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
committed=$(timeout 60 "$hull" commit dtest dlive/1 2>&1)
shown=$(timeout 30 "$hull" manifest show dlive/1 2>&1)
printf '%s\n' "$committed" | grep -q '^dlive/1 sha256:' &&
	printf '%s\n' "$shown" | grep -q '(path "tmp/live")' &&
	printf '%s\n' "$shown" | grep -q '(path "bin/sh")' &&
	! printf '%s\n' "$shown" | grep -qE '"data/in"|"\.dockerenv"|"sbin/docker-init"' ||
	fail "a running container was not committed as its export is" "$committed
$(printf '%s\n' "$shown" | grep -oE '\(path "[^"]*"\)' | head -20)"
timeout 10 "$hull" kill -s KILL dtest > /dev/null 2>&1
wait "$runner" 2>/dev/null
conf unexported '(export "/proc/hull-no-such/export.tar") (command "true")'
expect 125 "the export.s file /proc/hull-no-such/export\\.tar: No such file or directory" unexported
# (owners PATH): owners given in the dir's import, its tar carrying them,
# so the container sees the file as 101:102 while on the user's disk it
# stays the user's; on an image ref, refused, which keeps its own.
mkdir -p "$root/rootfs"
cp "$probe" "$root/rootfs/probe"
printf 'given\n' > "$root/rootfs/given"
printf '101\000102\000given\000' > "$root/owners.list"
printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	"(owners \"$root/owners.list\") (command \"/probe\" \"owner\" \"/given\" \"owner\" \"/probe\")" \
	> "$root/owned.conf"
expect 0 '^owner=101:102 0[0-7]{3}$' owned
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq "^owner=$(id -u):[0-9]+ 0[0-7]+\$" &&
	[ "$(stat -c %u "$root/rootfs/given")" = "$(id -u)" ] ||
	fail "an owner not listed was not the user's, or a given one reached the disk" "$out"
rm -f "$root/rootfs/given"
conf ownedref "(owners \"$root/owners.list\") (command \"true\")"
expect 125 'owners are given to a dir.s import' ownedref
# A stored image's owners, end to end: its tar committed with alpine's
# tree, the probe and a file of 101:102; run on docker it is checked out,
# all the user's, and the owners list hull writes from its manifest gives
# the file back its owner. Then a build of two steps on docker over it:
# each an import given its parent's owners and docker export, so the
# second sees 101:102 as the first did, and what the workload wrote as
# the user it ran as is the commit's root, as chroot's namespace root is.
cid=$(timeout 30 docker create alpine:latest true 2>/dev/null)
timeout 60 docker export "$cid" > "$root/alpine.tar"
timeout 20 docker rm "$cid" > /dev/null 2>&1
timeout 60 python3 - "$root/alpine.tar" "$root/rootfs/probe" "$root/ownedbase.tar" <<'PYEOF'
import io, sys, tarfile
src, probe, out = sys.argv[1:]
t = tarfile.open(out, "w", format=tarfile.PAX_FORMAT)
for m in tarfile.open(src):
	if m.name.lstrip("./") in ("", ".dockerenv", "etc/hosts") or m.islnk():
		continue
	t.addfile(m, tarfile.open(src).extractfile(m) if m.isfile() else None)
def add(name, uid, gid, mode, data):
	i = tarfile.TarInfo(name)
	i.uid, i.gid, i.mode, i.size = uid, gid, mode, len(data)
	t.addfile(i, io.BytesIO(data))
with open(probe, "rb") as f:
	add("probe", 0, 0, 0o755, f.read())
add("given", 101, 102, 0o644, b"given\n")
add("etc/hosts", 0, 0, 0o644, b"127.0.0.1 from the base\n")
t.close()
PYEOF
committed=$(timeout 60 "$hull" commit --tar "$root/ownedbase.tar" ownedbase/1 2>&1)
printf '(machine (source "ownedbase/1") (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "owner" "/given")' > "$root/ownedrun.conf"
expect 0 '^owner=101:102 0[0-7]{3}$' ownedrun
mkdir -p "$root/b"
# Run $$ in the first step's line, so that this suite's store, kept from
# run to run, holds no cache of it.
cat > "$root/b/hull.conf" <<EOF
(build
  (base "ownedbase/1")
  (steps
    (step (name "write") (backend docker)
          (run "/probe owner /given write /srv/made say run-$$"))
    (step (name "read") (backend docker)
          (run "/probe owner /given read /srv/made"))))
EOF
cases=$((cases + 1))
built=$(cd "$root/b" && timeout 300 "$hull" build -f hull.conf dbuilt/1 2>&1)
view=$(timeout 30 "$hull" manifest show dbuilt/1 2>&1)
[ "$(printf '%s\n' "$built" | grep -Ec '^owner=101:102 0[0-7]{3}$')" = 2 ] &&
	printf '%s\n' "$built" | grep -q '^read=Success$' &&
	printf '%s\n' "$built" | grep -q '^dbuilt/1 sha256:' &&
	printf '%s\n' "$view" | grep -qF '(owner (gid 102) (uid 101)) (path "given")' &&
	printf '%s\n' "$view" | grep -qF '(owner root) (path "srv/made")' ||
	fail "a build on docker did not keep its owners, or its writes as root's" "$committed
$built
$(printf '%s\n' "$view" | grep -E 'given|made')"
# And none of docker's own: no marker, no init, and the base's /etc/hosts
# rather than the empty one docker lays over it.
cases=$((cases + 1))
hosts=$(timeout 30 "$hull" manifest show dbuilt/1 2>&1 | grep -F '(path "etc/hosts")')
! printf '%s\n' "$view" | grep -qE '"\.dockerenv"|"sbin/docker-init"' &&
	printf '%s\n' "$hosts" | grep -qF '(size 24)' ||
	fail "a build on docker committed docker's own files" \
		"$(printf '%s\n' "$view" | grep -E 'dockerenv|docker-init|etc/hosts')"
# (layers "dir:..." ...): an image of as many layers, the source's tar
# beneath and each layer's above it, docker's union making the top one
# win; a write is the container's and lands in no layer; the same trees
# run again from the same image; and an image ref is refused them.
mkdir -p "$root/layered/template" "$root/layered/site/sub"
cp "$probe" "$root/layered/template/probe"
printf 'template\n' > "$root/layered/template/a"
printf 'site\n' > "$root/layered/site/a"
printf 'b\n' > "$root/layered/site/sub/b"
printf '(machine (source "dir:./layered/template") (layers "dir:./layered/site") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "read" "/a" "read" "/sub/b" "write" "/sub/w" "write" "/w")' > "$root/layered.conf"
expect 0 'layers: emulated: an image of as many layers' layered
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read=site$' && printf '%s\n' "$out" | grep -q '^read=b$' &&
	[ "$(printf '%s\n' "$out" | grep -c '^write=')" = 2 ] &&
	[ ! -e "$root/layered/site/sub/w" ] && [ ! -e "$root/layered/template/w" ] ||
	fail "a layered machine did not see its top layer, or wrote into one" "$out"
layered_images() {
	timeout 10 docker images --filter label=hull.path --format '{{.ID}} {{.Repository}}:{{.Tag}}' |
		grep ' hull-dir:' | while read -r id ref; do
		at=$(timeout 10 docker image inspect -f '{{index .Config.Labels "hull.path"}}' "$id" |
			python3 -c 'import sys; print(bytes.fromhex(sys.stdin.read().strip()).decode())')
		[ "$at" = "$(realpath "$root/layered/template")" ] && printf '%s\n' "$ref"
	done
}
first_layered=$(layered_images)
expect 0 '^read=site$' layered
cases=$((cases + 1))
[ -n "$first_layered" ] && [ "$(layered_images)" = "$first_layered" ] ||
	fail "the same layers were not run from the same image" "$first_layered then $(layered_images)"
conf layeredref '(layers "dir:/tmp") (command "true")'
expect 125 'layers and an upper lie above a dir.s import' layeredref
# (upper PATH): the import's top layer, and the container's changes
# written back into it as it ends -- a write kept, a removal a whiteout,
# the user's -- so a second run sees what the first left. Docker's own
# mounts and a volume's destination are not the workload's writes and
# land in no upper.
rm -f "$root/layered/data/srv/d" "$root/layered/data/srv/x" "$root/layered/vol/v"
rmdir "$root/layered/data/srv" 2> /dev/null
mkdir -p "$root/layered/data" "$root/layered/site/srv" "$root/layered/vol"
printf 'c\n' > "$root/layered/data/c"
printf 'x\n' > "$root/layered/site/srv/x"
# And an opaque directory, as an overlay marks one made again over one
# below: what lies beneath it hidden.
mkdir -p "$root/layered/site/opq" "$root/layered/data/opq"
printf 'x\n' > "$root/layered/site/opq/x"
printf 'y\n' > "$root/layered/data/opq/y"
timeout 10 python3 -c 'import os, sys; os.setxattr(sys.argv[1], "user.overlay.opaque", b"y")' \
	"$root/layered/data/opq"
printf '(machine (source "dir:./layered/template") (layers "dir:./layered/site") (upper "./layered/data") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(volumes (volume (host "./layered/vol") (guest "/vol"))) (command "/probe" "read" "/c" "write" "/srv/d" "remove" "/srv/x" "write" "/vol/v")' \
	> "$root/uppered.conf"
expect 0 'upper: emulated' uppered
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read=c$' && [ "$(printf '%s\n' "$out" | grep -c '^write=ok$')" = 2 ] &&
	printf '%s\n' "$out" | grep -q '^remove=ok$' &&
	[ -f "$root/layered/data/srv/d" ] && [ -O "$root/layered/data/srv/d" ] &&
	[ -c "$root/layered/data/srv/x" ] && [ -e "$root/layered/site/srv/x" ] &&
	[ ! -e "$root/layered/template/srv" ] && [ ! -e "$root/layered/data/vol" ] &&
	[ ! -e "$root/layered/data/sbin" ] && [ ! -e "$root/layered/data/.dockerenv" ] &&
	[ ! -e "$root/layered/data/etc" ] && [ -e "$root/layered/vol/v" ] ||
	fail "a machine's writes did not land in its upper alone, as an upper keeps them" \
		"$out
$(ls -laR "$root/layered/data" 2>&1)"
printf '(machine (source "dir:./layered/template") (layers "dir:./layered/site") (upper "./layered/data") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "list" "/srv" "list" "/opq")' > "$root/reuppered.conf"
expect 0 '^list= d$' reuppered
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^list= y$' ||
	fail "an upper's opaque directory did not hide what lay below it" "$out"
# (volumes (volume (guest G) (layers ...) [(upper U)] [(mode ro)])): each
# stack laid into the import beneath G, hiding the root's own G as a
# mount does; a kept one's writes and removals given back to its upper
# alone; one in memory writing nowhere on disk; a read-only one refusing
# a write, ReadonlyPaths; and an export holding G empty, as chroot's.
rm -f "$root/stacked/data/d" "$root/stacked/data/sub/b" "$root/stacked/out.tar"
rmdir "$root/stacked/data/sub" 2> /dev/null
mkdir -p "$root/stacked/root/proc" "$root/stacked/root/mnt" "$root/stacked/data" \
	"$root/stacked/seed"
cp "$probe" "$root/stacked/root/probe"
printf 'hidden\n' > "$root/stacked/root/mnt/under"
printf 'c\n' > "$root/stacked/data/c"
printf 's\n' > "$root/stacked/seed/s"
printf '(machine (source "dir:./stacked/root") (mutable overlay) (backend docker) (hostname "dtest") %s %s)\n' \
	'(volumes (volume (guest "/mnt") (layers "dir:./layered/template" "dir:./layered/site") (upper "./stacked/data")) (volume (guest "/var/seed") (layers "dir:./stacked/seed")) (volume (guest "/ro") (layers "dir:./layered/template") (mode ro))) (export "./stacked/out.tar")' \
	'(command "/probe" "read" "/mnt/a" "read" "/mnt/c" "read" "/mnt/under" "write" "/mnt/d" "remove" "/mnt/sub/b" "write" "/var/seed/t" "write" "/ro/x")' \
	> "$root/stacked.conf"
expect 0 'volumes.layers: emulated' stacked
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read=site$' && printf '%s\n' "$out" | grep -q '^read=c$' &&
	printf '%s\n' "$out" | grep -q '^read=No such file or directory$' &&
	printf '%s\n' "$out" | grep -q '^remove=ok$' &&
	[ "$(printf '%s\n' "$out" | grep -c '^write=ok$')" = 2 ] &&
	printf '%s\n' "$out" | grep -q '^write=Read-only file system$' &&
	[ -f "$root/stacked/data/d" ] && [ -c "$root/stacked/data/sub/b" ] &&
	[ -e "$root/layered/site/sub/b" ] && [ ! -e "$root/layered/site/d" ] &&
	[ ! -e "$root/stacked/seed/t" ] && [ ! -e "$root/stacked/root/mnt/d" ] ||
	fail "a volume's stack was not laid, kept and read-only as on chroot" \
		"$out
$(ls -la "$root/stacked/data" "$root/stacked/seed" 2>&1)"
cases=$((cases + 1))
inside=$(timeout 30 tar -tf "$root/stacked/out.tar" 2>&1 | grep -E '^(mnt|var/seed|ro)/')
[ "$(printf '%s\n' "$inside" | sort | tr '\n' ' ')" = "mnt/ ro/ var/seed/ " ] ||
	fail "an export held what a volume's stack holds" "$inside"
conf upperref "(upper \"$root/layered/refupper\") (command \"true\")"
expect 125 'layers and an upper lie above a dir.s import' upperref
rmdir "$root/layered/refupper" 2> /dev/null
for image in $(layered_images); do timeout 30 docker rmi "$image" > /dev/null 2>&1; done
# A volume inside another, from the user's directories: docker makes a
# missing mount point as root, so one not there in the outer volume's
# directory is refused, and nothing is made; one there runs.
mkdir -p "$root/nest/a" "$root/nest/b"
rmdir "$root/nest/a/x" 2> /dev/null
printf 'inner\n' > "$root/nest/b/in"
conf nested '(volumes (volume (host "./nest/a") (guest "/mnt")) (volume (host "./nest/b") (guest "/mnt/x"))) (command "cat" "/mnt/x/in")'
expect 125 '/mnt/x: the mount point is in volume /mnt.s directory and is not there' nested
cases=$((cases + 1))
[ ! -e "$root/nest/a/x" ] || fail "a nested volume's mount point was made in the user's directory" \
	"$(ls -la "$root/nest/a")"
mkdir "$root/nest/a/x"
expect 0 '^inner$' nested
rmdir "$root/nest/a/x"
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
# Root mode (section 7): the same grant accepted for this machine's hash
# is refused until the host's listing names it, then run as the
# container's root with that capability and no other, each start in the
# log with its verdict. A root machine is ROOT in hull ps, an exec into it
# and its stop are logged too, and one kept past its run is refused
# before anything is granted.
conf rootcap '(security (allow chown))' '(command "sh" "-c" "id -u; grep CapEff /proc/self/status; touch /tmp/f && chown 1 /tmp/f && echo chown=ok")'
token=$(cd / && timeout 10 "$hull" plan --root-hash -f "$root/rootcap.conf" 2> /dev/null | sed -n 's/.*: --accept //p')
expect 125 'hull-root: [a-z0-9._-]+ may not start sha256:' rootcap --accept "$token"
printf '%s %s security.allow\n' "$(id -un)" "${token#root:}" > "$root/root.conf"
expect 0 '^chown=ok$' rootcap --accept "$token"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -qx '0' && printf '%s\n' "$out" | grep -q '^CapEff:.0000000000000001$' &&
	[ "$(grep -c " start ${token#root:} refused security.allow\$" "$root/root.log")" = 1 ] &&
	[ "$(grep -c " start ${token#root:} granted security.allow\$" "$root/root.log")" = 1 ] ||
	fail "a root machine was not granted as listed, its root's with chown alone, and logged" \
		"$out
$(cat "$root/root.log")"
printf '(machine (source "alpine:latest") (backend docker) (hostname "dtest-root") %s)\n' \
	'(security (allow chown)) (command "sleep" "30")' > "$root/rootheld.conf"
held=$(cd / && timeout 10 "$hull" plan --root-hash -f "$root/rootheld.conf" 2> /dev/null | sed -n 's/.*: --accept //p')
printf '%s %s security.allow\n' "$(id -un)" "${held#root:}" >> "$root/root.conf"
(cd / && exec timeout -k 2 60 "$hull" run --accept "$held" -f "$root/rootheld.conf" > /dev/null 2>&1) &
holder=$!
tries=0
until timeout 10 "$hull" ps 2> /dev/null | grep -q '^dtest-root .*running ROOT$' || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
expect_ps=$(timeout 10 "$hull" ps 2>&1)
timeout 10 "$hull" exec dtest-root true > /dev/null 2>&1
timeout 30 "$hull" stop dtest-root > /dev/null 2>&1
wait "$holder"
cases=$((cases + 1))
printf '%s\n' "$expect_ps" | grep -q '^dtest-root .*running ROOT$' &&
	grep -q " exec ${held#root:} granted\$" "$root/root.log" &&
	grep -q " stop ${held#root:} granted\$" "$root/root.log" ||
	fail "a root machine was not ROOT in ps, or its exec and stop not logged" \
		"$expect_ps
$(cat "$root/root.log")"
conf rootkept '(on_disconnect keep) (security (allow chown))' '(command "true")'
kept=$(cd / && timeout 10 "$hull" plan --root-hash -f "$root/rootkept.conf" 2> /dev/null | sed -n 's/.*: --accept //p')
printf '%s %s security.allow\n' "$(id -un)" "${kept#root:}" >> "$root/root.conf"
expect 125 'a root machine is not kept past its run' rootkept --accept "$kept"
cases=$((cases + 1))
! grep -q "${kept#root:}" "$root/root.log" ||
	fail "a kept root machine was granted before it was refused" "$(cat "$root/root.log")"

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
# (devices ...) by docker's --device: /dev/net/tun, which read with no
# interface says EBADFD, and /dev/char/10:200, a link to it, a node at
# that path in the container, docker making nodes and not links. A node
# the user may not open is refused although the daemon could.
if [ -r /dev/net/tun ] && [ -w /dev/net/tun ] && [ -L /dev/char/10:200 ]; then
	printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
		'(devices (node "/dev/char/10:200")) (command "/probe" "read" "/dev/char/10:200" "kind" "/dev/char/10:200")' \
		> "$root/devnode.conf"
	expect 0 '^read=File descriptor in bad state$' devnode
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^kind=chr 10:200$' ||
		fail "a link's device was not at its path in the container" "$out"
fi
if [ "$(id -u)" != 0 ] && [ -e /dev/ttyS0 ] && [ ! -w /dev/ttyS0 ]; then
	printf '(machine (source "dir:./rootfs") (mutable overlay) (backend docker) (hostname "dtest") %s)\n' \
		'(devices (serial "/dev/ttyS0")) (command "/probe")' > "$root/devlocked.conf"
	expect 125 '/dev/ttyS0 is root:[a-z]+ [0-7]+, which the user may not read and write' devlocked
fi
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
# A dir in place, (mutable direct), a dir's default: an image of its frame,
# the tree's entries bound in, so a write lands in the user's directory
# while the machine runs. Where docker mounts something -- its init,
# here through a /sbin linked to usr/sbin as a merged /usr has it, and
# the files over /etc -- the directory above is the image's, so docker
# makes nothing in the user's tree, and a new file there, or at the top,
# is refused rather than kept nowhere. An export, and a volume whose
# guest path is not in the tree, are refused, as on chroot.
rm -f "$root/direct/root/srv/new" "$root/direct/live"
mkdir -p "$root/direct/root/proc" "$root/direct/root/etc" "$root/direct/root/usr/sbin" \
	"$root/direct/root/srv"
cp "$probe" "$root/direct/root/probe"
printf 'here\n' > "$root/direct/root/etc/motd"
ln -sfn usr/sbin "$root/direct/root/sbin"
printf '(machine (source "dir:./direct/root") (backend docker) (hostname "dtest") %s)\n' \
	'(command "/probe" "read" "/etc/motd" "write" "/srv/new" "sleep" "4" "write" "/etc/new" "write" "/top" "list" "/sbin")' \
	> "$root/dirin.conf"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/dirin.conf" > "$root/direct/out" 2>&1) &
runner=$!
tries=0
until [ -e "$root/direct/root/srv/new" ] || [ "$tries" -ge 60 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
# Still running, its cmdline there, which a zombie's is not.
tr -d '\000' < "/proc/$runner/cmdline" 2> /dev/null | grep -q . &&
	[ -e "$root/direct/root/srv/new" ] && : > "$root/direct/live"
wait "$runner"
got=$?
out=$(cat "$root/direct/out")
cases=$((cases + 1))
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -q 'source\.dir\.direct: emulated' &&
	printf '%s\n' "$out" | grep -q '^read=here$' &&
	[ "$(printf '%s\n' "$out" | grep -c '^write=Permission denied$')" = 2 ] &&
	printf '%s\n' "$out" | grep -q '^list= docker-init$' &&
	[ -e "$root/direct/live" ] && [ -O "$root/direct/root/srv/new" ] &&
	[ "$(ls -A "$root/direct/root/etc")" = motd ] && [ -z "$(ls -A "$root/direct/root/usr/sbin")" ] &&
	[ ! -e "$root/direct/root/top" ] ||
	fail "a dir in place did not take its writes as they were made, or docker made files in it" \
		"exit $got, live $([ -e "$root/direct/live" ] && echo yes || echo no)
$out
$(ls -la "$root/direct/root" "$root/direct/root/etc" "$root/direct/root/usr/sbin" 2>&1)"
printf '(machine (source "dir:./direct/root") (backend docker) (hostname "dtest") %s)\n' \
	'(export "./direct/out.tar") (command "true")' > "$root/dexported.conf"
cases=$((cases + 1))
said=$(cd / && timeout 60 "$hull" run -f "$root/dexported.conf" 2>&1)
[ $? = 125 ] && printf '%s\n' "$said" | grep -q 'needs (mutable overlay)' &&
	[ ! -e "$root/direct/out.tar" ] ||
	fail "an export of a dir in place was not refused" "$said"
printf '(machine (source "dir:./direct/root") (backend docker) (hostname "dtest") %s)\n' \
	'(volumes (volume (host "./data") (guest "/nowhere"))) (command "true")' \
	> "$root/dvolumed.conf"
cases=$((cases + 1))
said=$(cd / && timeout 60 "$hull" run -f "$root/dvolumed.conf" 2>&1)
[ $? = 125 ] && printf '%s\n' "$said" | grep -q 'volume /nowhere: the guest path is not in the machine' &&
	[ ! -e "$root/direct/root/nowhere" ] ||
	fail "a volume's guest path missing from a dir in place was made or not refused" "$said"

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

# The imports of the store's checkouts, which this suite's store holds and
# gc therefore keeps: removed by their hull.path, under this suite's data.
for image in $(timeout 10 docker images --filter reference=hull-dir --format '{{.ID}}' | sort -u); do
	at=$(timeout 10 docker image inspect -f '{{index .Config.Labels "hull.path"}}' "$image" 2>/dev/null |
		python3 -c 'import sys; print(bytes.fromhex(sys.stdin.read().strip()).decode())' 2>/dev/null)
	case $at in
	"$root"/data/* | "$root"/stacked/* | "$root"/direct/*) timeout 30 docker rmi -f "$image" > /dev/null 2>&1 ;;
	esac
done

# (ssh true) on docker, section 8.1: the dropbear `hull sshd fetch` keeps,
# run as the container's root with SETGID and SETUID in the container's
# bounding set, which it needs to log a session in; the workload, as the
# user, holds none of them. Real ssh logs in as root by a key of the
# test's with the machine's host key pinned, scp sends a file, sftp lists
# it. Refused, saying why, with (mutable direct). A core of its own on a
# tmpfs, the key being private, with a HOME and cache of the test's. The
# fetch reaches Debian's archive: where it cannot, this is said, not run.
sshrt=$(mktemp -d /dev/shm/hull-dssh.XXXXXX 2>/dev/null)
fetched=$(XDG_CACHE_HOME="$root/cache" timeout 120 "$hull" sshd fetch 2>&1)
if [ -z "$sshrt" ] || [ ! -x "$(printf '%s\n' "$fetched" | tail -1)/usr/sbin/dropbear" ]; then
	printf 'docker: (ssh true) not run: %s\n' "${sshrt:-no tmpfs}; $(printf '%s\n' "$fetched" | tail -1)"
	[ -z "$sshrt" ] || rmdir "$sshrt"
else
	chmod 700 "$sshrt"
	ssh_env="XDG_RUNTIME_DIR=$sshrt HOME=$root/sshhome XDG_CACHE_HOME=$root/cache"
	rm -rf -- "${root:?}/sshhome"
	mkdir -p "$root/sshhome/.ssh"
	chmod 700 "$root/sshhome/.ssh"
	ssh-keygen -q -t ed25519 -N '' -C hull-docker -f "$root/sshhome/.ssh/id_ed25519" < /dev/null
	printf '(machine (source "alpine:latest") (backend docker) (hostname "dtest") %s %s)\n' \
		'(ssh true)' '(command "/bin/sh" "-c" "echo ready; exec sleep 120")' > "$root/dssh.conf"
	{
		printf 'Host dtest.hull\n'
		printf '  ProxyCommand env XDG_RUNTIME_DIR=%s %s proxy-ssh %%h\n' "$sshrt" "$hull"
		printf '  User root\n  IdentityFile %s\n  IdentitiesOnly yes\n' "$root/sshhome/.ssh/id_ed25519"
		printf '  BatchMode yes\n  ConnectTimeout 10\n  LogLevel error\n'
		printf '  StrictHostKeyChecking yes\n  UserKnownHostsFile %s\n' "$root/dssh.known"
	} > "$root/dssh.ssh"
	rm -f "$root/dssh.out" "$root/dssh.known"
	(cd / && exec env $ssh_env timeout -k 2 150 "$hull" run -f "$root/dssh.conf" \
		> "$root/dssh.out" 2>&1) &
	runner=$!
	tries=0
	until grep -q '^ready$' "$root/dssh.out" 2>/dev/null || [ "$tries" -ge 80 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	printf 'dtest.hull %s\n' "$(cut -d' ' -f1,2 "$root/data/hull/machines/dtest/host_key.pub" 2>&1)" \
		> "$root/dssh.known"
	tries=0
	until timeout 10 ssh -F "$root/dssh.ssh" dtest.hull true < /dev/null 2>/dev/null ||
	      [ "$tries" -ge 20 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	who=$(timeout 10 ssh -F "$root/dssh.ssh" dtest.hull 'echo "$(id -u) [$LD_LIBRARY_PATH]"' \
		< /dev/null 2>&1)
	printf 'sent by scp\n' > "$root/dssh.sent"
	timeout 10 scp -F "$root/dssh.ssh" "$root/dssh.sent" dtest.hull:/tmp/sent < /dev/null > /dev/null 2>&1
	landed=$(timeout 10 ssh -F "$root/dssh.ssh" dtest.hull cat /tmp/sent < /dev/null 2>&1)
	listed=$(printf 'ls /tmp\n' | timeout 10 sftp -F "$root/dssh.ssh" -b - dtest.hull 2>&1)
	caps=$(env $ssh_env timeout 10 "$hull" exec dtest grep CapEff /proc/self/status 2>&1)
	env $ssh_env timeout 30 "$hull" stop dtest > /dev/null 2>&1
	wait "$runner"
	cases=$((cases + 1))
	[ "$who" = '0 []' ] ||
		fail "(ssh true) on docker did not log in as root, its loader's path unseen" \
			"$who; $(cat "$root/dssh.out")"
	cases=$((cases + 1))
	[ "$landed" = 'sent by scp' ] || fail "scp to docker's dropbear did not land" "$landed"
	cases=$((cases + 1))
	printf '%s\n' "$listed" | grep -q '/tmp/sent' || fail "sftp to docker's dropbear did not list" "$listed"
	cases=$((cases + 1))
	printf '%s\n' "$caps" | grep -q 'CapEff:.0000000000000000$' ||
		fail "the workload of an (ssh true) container holds a capability" "$caps"
	# A directory used in place is refused: the sshd's root-owned files
	# would be made in the user's own tree.
	mkdir -p "$root/direct/sshdir"
	printf '(machine (source "dir:./direct/sshdir") (backend docker) (mutable direct) (hostname "dtest") %s)\n' \
		'(ssh true) (command "/bin/sh" "-c" "true")' > "$root/dsshdir.conf"
	cases=$((cases + 1))
	out=$(cd / && env $ssh_env timeout -k 2 60 "$hull" run -f "$root/dsshdir.conf" 2>&1)
	got=$?
	{ [ "$got" = 125 ] && printf '%s\n' "$out" | grep -q 'with (mutable direct) its sshd.s root-owned'; } ||
		fail "(ssh true) on docker with (mutable direct) was not refused" "$got: $out"
	rm -rf -- "${sshrt:?}" "${root:?}/sshhome/.ssh"
fi

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
