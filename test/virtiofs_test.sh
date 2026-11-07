#!/bin/sh
# A vm's trees by virtiofs and not 9p, the default where it will work and
# (tuning (qemu (share virtiofs))) as a pin (project.md section 4.11), held to
# what 9p does for a machine, and auto's way back to 9p. Needs
# virtiofsd, installed or named by HULL_VIRTIOFSD, and says so where it is
# absent. Bounded by each run's timeout, and the runs by the machines' short
# commands; nothing of a daemon may outlive them, which is counted.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/virtiofs
rm -rf "${dir:?}"
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
cases=0
failures=0

daemon=${HULL_VIRTIOFSD:-}
for place in /usr/libexec/virtiofsd /usr/lib/qemu/virtiofsd /usr/lib/virtiofsd /usr/bin/virtiofsd; do
	[ -z "$daemon" ] && [ -x "$place" ] && daemon=$place
done
if [ -z "$daemon" ]; then
	printf 'virtiofs: not measured: no virtiofsd, installed or in HULL_VIRTIOFSD\n'
	exit 0
fi

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}
mkdir -p "$root/run" "$root/vol" "$root/ro" "$root/tree" "$root/upper" "$root/low"
chmod 700 "$root/run"
cp "$probe" "$root/tree/probe"
mkdir -p "$root/tree/proc" "$root/tree/sys" "$root/tree/dev"
echo from-host > "$root/vol/host-file"
echo kept > "$root/ro/file"
echo below > "$root/low/file"
host() { # COMMAND...
	env HOME="$root" XDG_RUNTIME_DIR="$root/run" XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data" \
		DOCKER_HOST="unix://$root/none.sock" HULL_CORE_IDLE=1 HULL_VIRTIOFSD="$daemon" "$@"
}
# on NAME ENTRIES... SCRIPT: a machine of the host's tree, its script run by sh.
on() {
	name=$1
	entries=$2
	script=$3
	printf '%s\n' "$script" > "$root/$name.sh"
	cat > "$root/$name.conf" <<CONF
(machine (source "dir:/") (backend ${backend:-vm}) (mutable overlay) (network none) (memory "1G")
  (hostname "$name") $entries
  (command "/bin/sh" "$root/$name.sh"))
CONF
	host timeout -k 3 ${limit:-90} "$hull" run -f "$root/$name.conf" 2>&1 < /dev/null | grep -v ': emulated: '
}
fs='(tuning (qemu (share virtiofs)))'
vols="(volumes (volume (host \"$root/vol\") (guest \"/vol\"))
                (volume (host \"$root/ro\") (guest \"/ro\") (mode ro)))"

# --- which it is ----------------------------------------------------------------

check='grep -E " /(vol|ro) " /proc/mounts'
out=$(on which "$fs $vols" "$check")
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q ' /vol virtiofs rw' && printf '%s\n' "$out" | grep -q ' /ro virtiofs ro' ||
	fail 'the volumes are not virtiofs' "$out"
out=$(on plain "$vols" "$check")
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q ' /vol virtiofs ' || fail 'no key is not virtiofs' "$out"
out=$(on asked "(tuning (qemu (share auto))) $vols" "$check")
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q ' /vol virtiofs ' || fail '(share auto) is not virtiofs' "$out"
out=$(on chosen "(tuning (qemu (share 9p))) $vols" "$check")
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q ' /vol 9p ' || fail '(share 9p) is not 9p' "$out"

# --- what 9p does for a machine, the same -----------------------------------------

out=$(on same "$fs $vols" '
cat /vol/host-file
echo written > /vol/from-guest && echo wrote
echo no > /ro/nope; echo "ro=$?"
stat -c "owner=%u:%g" /vol/host-file
mkdir /vol/d && ln -s from-guest /vol/link && ln /vol/from-guest /vol/hard && echo links
chmod 640 /vol/from-guest; stat -c "mode=%a" /vol/from-guest
mv /vol/hard /vol/d/moved && rm /vol/d/moved && echo moved
chown 1234:1234 /vol/from-guest; echo "chown=$?"
exit 7')
cases=$((cases + 1))
for want in '^from-host$' '^wrote$' 'ro=[1-9]' '^owner=0:' '^links$' '^mode=640$' '^moved$'; do
	printf '%s\n' "$out" | grep -Eq "$want" || fail "a volume on virtiofs: no /$want/" "$out"
done
cases=$((cases + 1))
[ "$(stat -c '%u' "$root/vol/from-guest" 2> /dev/null)" = "$(id -u)" ] && [ "$(cat "$root/vol/from-guest" 2> /dev/null)" = written ] ||
	fail 'a guest write did not land as the user' "$(ls -la "$root/vol")"
cases=$((cases + 1))
[ ! -e "$root/ro/nope" ] || fail 'a read-only volume was written' "$(ls -la "$root/ro")"
# 9p takes a chown and keeps nothing, and programs lean on that: dpkg, tar.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^chown=0$' || fail 'a chown in the guest fails, where 9p takes it' "$out"

# --- a root in place, and under an overlay ----------------------------------------

cat > "$root/inplace.conf" <<CONF
(machine (source "dir:$root/tree") (backend vm) (mutable direct) (network none) $fs
  (command "/probe" "write" "/made-here" "x"))
CONF
out=$(host timeout -k 3 60 "$hull" run -f "$root/inplace.conf" 2>&1 < /dev/null)
cases=$((cases + 1))
[ -e "$root/tree/made-here" ] || fail 'a root in place did not take a write' "$out"
cat > "$root/over.conf" <<CONF
(machine (source "dir:$root/tree") (backend vm) (mutable overlay) (network none) $fs
  (command "/probe" "write" "/made-above" "x"))
CONF
out=$(host timeout -k 3 60 "$hull" run -f "$root/over.conf" 2>&1 < /dev/null)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^write=ok' && [ ! -e "$root/tree/made-above" ] ||
	fail 'an overlay root reached its directory, or took no write' "$out $(ls "$root/tree")"
cat > "$root/kept.conf" <<CONF
(machine (source "dir:$root/tree") (backend vm) (mutable overlay) (network none)
  (layers "dir:$root/low") (upper "$root/upper") $fs
  (command "/probe" "read" "/file" "write" "/made-kept" "x"))
CONF
out=$(host timeout -k 3 60 "$hull" run -f "$root/kept.conf" 2>&1 < /dev/null)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read=below' && [ -e "$root/upper/made-kept" ] ||
	fail 'layers and a kept upper, on virtiofs' "$out $(ls -a "$root/upper")"

# A stored tree's owners, given back in the overlay qemu mounts host-side. The
# guest's ids come to the user on the way in; the host's must still show.
mkdir -p "$root/owned/proc" "$root/owned/dev" "$root/owned/sys"
cp "$probe" "$root/owned/probe"
printf 'given\n' > "$root/owned/given"
chgrp "$(id -g)" "$root/owned/given"
printf '101\000102\000given\000' > "$root/owners.list"
me=$(id -un)
if grep -q "^$me:" /etc/subuid 2> /dev/null && grep -q "^$me:" /etc/subgid 2> /dev/null; then
	owner='^owner=101:102 0'
else
	owner='^owner=0:0 0'
fi
cat > "$root/owned.conf" <<CONF
(machine (source "dir:$root/owned") (backend vm) (mutable overlay) (network none)
  (owners "$root/owners.list") $fs (command "/probe" "owner" "/given"))
CONF
out=$(host timeout -k 3 60 "$hull" run -f "$root/owned.conf" 2>&1 < /dev/null)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq "$owner" || fail 'a stored tree lost its owners' "$out"

# And the other way: a chown to an id the namespace holds is kept, which a
# build's next step reads. Squashing every id took it, and kept nothing.
if [ "$owner" = '^owner=101:102 0' ]; then
	mkdir -p "$root/bbase/bin" "$root/bbase/proc" "$root/bbase/dev" "$root/bbase/sys" "$root/bbase/tmp"
	cp "$probe" "$root/bbase/bin/sh"
	cat > "$root/built.conf" <<BUILD
(build (base "dir:./bbase")
  (steps (step (name "give-$$") (backend vm) (run "probe write /given chown /given 101"))
         (step (name "look-$$") (backend vm) (run "probe owner /given"))))
BUILD
	out=$(cd "$root" && host timeout -k 3 200 "$hull" build -f built.conf app/built 2>&1 < /dev/null)
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^owner=101:101 ' || fail "a step's owner was not kept" "$out"
fi

# hull's own kernel has it built in, where it has been built.
if [ -e "$(dirname "$hull")/vmlinuz-hull" ]; then
	cat > "$root/own.conf" <<CONF
(machine (source "dir:$root/tree") (backend vm) (mutable overlay) (network none)
  (tuning (vm (kernel hull)) (qemu (share virtiofs))) (command "/probe" "say" "on hull's own"))
CONF
	out=$(host timeout -k 3 60 "$hull" run -f "$root/own.conf" 2>&1 < /dev/null)
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q "^on hull's own" || fail "hull's own kernel has no virtiofs" "$out"
	# A kernel hull knows nothing of goes by 9p: its own, by path, is one,
	# with no modules here to say what it has, and it has both.
	out=$(on known "(tuning (vm (kernel hull))) $vols" "$check")
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q ' /vol virtiofs ' || fail "no key on hull's own kernel is not virtiofs" "$out"
	out=$(on unknown "(tuning (vm (kernel \"path:$(dirname "$hull")/vmlinuz-hull\"))) $vols" "$check")
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q ' /vol 9p ' || fail 'a kernel with no modules here is not 9p' "$out"
else
	printf 'virtiofs: hull kernel not measured: make kernel has not been run\n'
fi

# --- refused, on emu, and nothing left --------------------------------------------

mkdir -p "$root/run2"
chmod 700 "$root/run2"
out=$(env HOME="$root" XDG_RUNTIME_DIR="$root/run2" XDG_CONFIG_HOME="$root/config" XDG_DATA_HOME="$root/data2" \
	DOCKER_HOST="unix://$root/none.sock" HULL_CORE_IDLE=1 HULL_VIRTIOFSD=/nonexistent \
	timeout -k 3 30 "$hull" run -f "$root/over.conf" 2>&1 < /dev/null)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'needs virtiofsd' || fail 'no virtiofsd was not said' "$out"
# No daemon, or one too old for what hull gives it: no key is 9p, and a pin
# is refused before anything starts. Each run a core of its own, a core
# keeping the environment it was started with.
cat > "$root/old-virtiofsd" <<'OLD'
#!/bin/sh
echo 'usage: virtiofsd [options]'
echo '    -o source=PATH'
OLD
chmod +x "$root/old-virtiofsd"
at=3
for without in /nonexistent "$root/old-virtiofsd"; do
	mkdir -p "$root/run$at"
	chmod 700 "$root/run$at"
	alone() {
		env HOME="$root" XDG_RUNTIME_DIR="$root/run$at" XDG_CONFIG_HOME="$root/config" \
			XDG_DATA_HOME="$root/data$at" DOCKER_HOST="unix://$root/none.sock" HULL_CORE_IDLE=1 \
			HULL_VIRTIOFSD="$without" timeout -k 3 90 "$hull" run -f "$1" 2>&1 < /dev/null
	}
	out=$(alone "$root/plain.conf")
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q ' /vol 9p ' || fail "no key is not 9p, with $without" "$out"
	out=$(alone "$root/over.conf")
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q 'needs virtiofsd' || fail "a pin was not refused, with $without" "$out"
	at=$((at + 1))
done
# Descriptors too few for the daemon: no key is 9p, which goes on where
# virtiofs would refuse the guest everything past the limit.
without=$daemon
mkdir -p "$root/run$at"
chmod 700 "$root/run$at"
out=$(ulimit -Sn 4096 && ulimit -Hn 4096 && alone "$root/plain.conf")
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q ' /vol 9p ' || fail 'no key is not 9p, on a low limit of files' "$out"
out=$(backend=emu limit=120 on tcg "$fs $vols" 'cat /vol/host-file; grep -c " /vol virtiofs" /proc/mounts')
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^from-host$' && printf '%s\n' "$out" | grep -q '^1$' || fail 'virtiofs on emu' "$out"
# The file work is the daemon's and not qemu's: stats has to count it.
: > "$root/busy.out"
(on busy "$vols" 'dd if=/dev/zero of=/vol/busy bs=1M count=8 conv=fsync 2>/dev/null; sync; echo up > /vol/busy-up; sleep 20' \
	> "$root/busy.out" 2>&1) &
busy=$!
n=0
until [ -e "$root/vol/busy-up" ] || [ "$n" -ge 120 ]; do
	n=$((n + 1))
	sleep 0.5
done
out=$(host timeout 10 "$hull" stats busy 2>&1)
host timeout 20 "$hull" stop --timeout 1 busy > /dev/null 2>&1
wait "$busy" 2> /dev/null
rm -f "$root/vol/busy" "$root/vol/busy-up"
cases=$((cases + 1))
[ "$(printf '%s\n' "$out" | awk '$1 == "io" { print $2 + 0 }')" -ge 8388608 ] 2> /dev/null ||
	fail "stats left out the daemon's io" "$out"

# A machine killed: its daemons go with their connection.
: > "$root/killed.out"
(on killed "$fs $vols" 'echo up; sleep 40' > "$root/killed.out" 2>&1) &
killed=$!
n=0
until grep -q '^up$' "$root/killed.out" || [ "$n" -ge 120 ]; do
	n=$((n + 1))
	sleep 0.5
done
host timeout 20 "$hull" kill -s KILL killed > /dev/null 2>&1
host timeout 20 "$hull" stop --timeout 1 killed > /dev/null 2>&1
wait "$killed" 2> /dev/null
n=0
until [ -z "$(ps -eo args= | grep "[v]irtiofsd.*$root\|[v]irtiofsd --socket-path=vfs-")" ] || [ "$n" -ge 20 ]; do
	n=$((n + 1))
	sleep 0.5
done
cases=$((cases + 1))
left=$(ps -eo pid=,args= | grep '[v]irtiofsd --socket-path=vfs-' | cut -c1-100)
[ -z "$left" ] || fail 'a daemon outlived its machine' "$left"
# And the directories they were in, their sockets by name.
n=0
until [ -z "$(ls -d "$root"/run*/hull/qemu-* 2> /dev/null)" ] || [ "$n" -ge 20 ]; do
	n=$((n + 1))
	sleep 0.5
done
cases=$((cases + 1))
left=$(ls -A "$root"/run*/hull/qemu-* 2> /dev/null)
[ -z "$left" ] || fail "a machine's directory was left" "$left"

# What it is for: the same walk of a real tree, each way, here through hull.
walk='a=$(date +%s.%N); n=$(find /usr/lib /usr/include 2> /dev/null | wc -l); b=$(date +%s.%N); echo "walked $n in $(echo "$b - $a" | bc 2> /dev/null || echo "$a $b")"'
limit=200 on walk9 '(tuning (qemu (share 9p)))' "$walk" | grep '^walked' | sed 's/^/virtiofs: 9p: /'
limit=200 on walkv "$fs" "$walk" | grep '^walked' | sed 's/^/virtiofs: virtiofs: /'

printf 'virtiofs: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
