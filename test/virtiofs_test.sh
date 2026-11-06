#!/bin/sh
# (tuning (qemu (share virtiofs))): a vm's trees by virtiofs and not 9p
# (project.md section 4.11), held to what 9p does for a machine. Needs
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
for place in /usr/libexec/virtiofsd /usr/lib/qemu/virtiofsd /usr/bin/virtiofsd; do
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
printf '%s\n' "$out" | grep -q ' /vol 9p ' || fail 'no key is not 9p' "$out"
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

# hull's own kernel has it built in, where it has been built.
if [ -e "$(dirname "$hull")/vmlinuz-hull" ]; then
	cat > "$root/own.conf" <<CONF
(machine (source "dir:$root/tree") (backend vm) (mutable overlay) (network none)
  (tuning (vm (kernel hull)) (qemu (share virtiofs))) (command "/probe" "say" "on hull's own"))
CONF
	out=$(host timeout -k 3 60 "$hull" run -f "$root/own.conf" 2>&1 < /dev/null)
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q "^on hull's own" || fail "hull's own kernel has no virtiofs" "$out"
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
out=$(backend=emu limit=120 on tcg "$fs $vols" 'cat /vol/host-file; grep -c " /vol virtiofs" /proc/mounts')
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^from-host$' && printf '%s\n' "$out" | grep -q '^1$' || fail 'virtiofs on emu' "$out"
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

# What it is for: the same walk of a real tree, each way, here through hull.
walk='a=$(date +%s.%N); n=$(find /usr/lib /usr/include 2> /dev/null | wc -l); b=$(date +%s.%N); echo "walked $n in $(echo "$b - $a" | bc 2> /dev/null || echo "$a $b")"'
limit=200 on walk9 '' "$walk" | grep '^walked' | sed 's/^/virtiofs: 9p: /'
limit=200 on walkv "$fs" "$walk" | grep '^walked' | sed 's/^/virtiofs: virtiofs: /'

printf 'virtiofs: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
