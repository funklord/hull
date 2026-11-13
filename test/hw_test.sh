#!/bin/sh
# hw_test.sh -- the hw class (project.md section 4.12) with no hardware, a
# board being whatever hull reaches by ssh with commands for its power. Not
# part of `make check`, which starts no sshd: `make check-hw` runs it. Its
# arguments are build/hull and the static probe. Each board here is a private
# sshd on 127.0.0.1, which its power hook starts and stops, so a board that is
# off does not answer, as one on a bench does not. All of it is under
# $BUILD_DIR/test/hw, removed at the start and left on failure, the sshds
# killed by their pids at the end and on a signal.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/hw
case $dir in
*/test/hw) ;;
*) printf 'hw: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
sshd=/usr/sbin/sshd
real_ssh=$(command -v ssh)
if [ ! -x "$sshd" ] || [ -z "$real_ssh" ] || ! unshare -r true 2> /dev/null; then
	printf 'hw: no sshd, ssh or user namespaces here, nothing tried\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run" "$dir/bin" "$dir/config/hull" "$dir/project/tree/srv"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_DATA_HOME=$root/data
XDG_CACHE_HOME=$root/cache
XDG_CONFIG_HOME=$root/config
HULL_CORE_IDLE=2
PATH=$root/bin:$PATH
export XDG_RUNTIME_DIR XDG_DATA_HOME XDG_CACHE_HOME XDG_CONFIG_HOME HULL_CORE_IDLE PATH
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# expect STATUS PATTERN COMMAND...
expect() {
	want=$1
	pattern=$2
	shift 2
	cases=$((cases + 1))
	out=$(timeout 90 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

# check NAME COMMAND...: it succeeds.
check() {
	name=$1
	shift
	cases=$((cases + 1))
	"$@" || fail "$name" ''
}

ssh-keygen -q -t ed25519 -N '' -f "$root/hostkey"
ssh-keygen -q -t ed25519 -N '' -f "$root/userkey"
cp "$root/userkey.pub" "$root/authorized"
cat > "$root/bin/ssh" <<WRAP
#!/bin/sh
exec "$real_ssh" -i "$root/userkey" -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "\$@"
WRAP
chmod +x "$root/bin/ssh"

# The boards' power: NAME PORT, then on, off or cycle. A dead one stays dark.
cat > "$root/power" <<'POWER'
#!/bin/sh
name=$1
port=$2
word=$3
base=$(dirname "$0")
printf '%s %s\n' "$name" "$word" >> "$base/power.log"
off() {
	[ -f "$base/$name.pid" ] && kill "$(cat "$base/$name.pid")" 2> /dev/null
	rm -f "$base/$name.pid"
	return 0
}
on() {
	[ -e "$base/$name.dead" ] && return 0
	[ -f "$base/$name.pid" ] && return 0
	cat > "$base/$name.conf" <<CONF
Port $port
ListenAddress 127.0.0.1
HostKey $base/hostkey
AuthorizedKeysFile $base/authorized
PidFile $base/$name.pid
StrictModes no
UsePAM no
CONF
	/usr/sbin/sshd -f "$base/$name.conf"
}
case $word in
on) on ;;
off) off ;;
cycle) off; sleep 0.2; on ;;
esac
POWER
chmod +x "$root/power"
stop_boards() {
	for pid in "$root"/*.pid; do
		[ -f "$pid" ] && kill "$(cat "$pid")" 2> /dev/null
	done
	"$hull" stop --timeout 1 onb > /dev/null 2>&1
	XDG_RUNTIME_DIR=$root/inner "$hull" stop --timeout 1 vmboard > /dev/null 2>&1
}
trap stop_boards EXIT
trap 'stop_boards; exit 1' INT TERM

port=$((20000 + $$ % 20000))
for board in a b; do
	mkdir -p "$root/on-$board"
done
cat > "$XDG_CONFIG_HOME/hull/pools.conf" <<CONF
(pools
  (bench (members
    (board "a" (address "127.0.0.1") (port $port) (root "$root/on-a")
           (power "$root/power a $port") (console "echo console of a") (health "true"))
    (board "b" (address "127.0.0.1") (port $((port + 1))) (root "$root/on-b")
           (power "$root/power b $((port + 1))"))))
  (sick (members
    (board "s" (address "127.0.0.1") (port $((port + 2))) (root "$root/on-a")
           (power "$root/power s $((port + 2))") (health "false"))))
  (dark (members
    (board "d" (address "127.0.0.1") (port $((port + 3))) (root "$root/on-a") (wait 3)
           (power "$root/power d $((port + 3))"))))
  (steer (capacity 1) (members
    (board "low" (priority -1) (address "127.0.0.1") (port $((port + 4))) (root "$root/on-a")
           (power "$root/power low $((port + 4))"))
    (board "high" (priority 3) (address "127.0.0.1") (port $((port + 5))) (root "$root/on-b")
           (power "$root/power high $((port + 5))"))))
  (soft (capacity 1))
  (flashed (members
    (board "img" (address "127.0.0.1") (port $((port + 7))) (root "$root/on-a")
           (deploy "$root/flash") (power "$root/power img $((port + 7))"))))
  (badflash (members
    (board "bf" (address "127.0.0.1") (port $((port + 8))) (root "$root/on-a") (deploy "false")
           (power "$root/power bf $((port + 8))"))))
  (noflash (members
    (board "nf" (address "127.0.0.1") (port $((port + 9))) (root "$root/on-a") (deploy "true")
           (power "$root/power nf $((port + 9))"))))
  (arches (members
    (board "ax" (arch "arm64") (address "127.0.0.1") (port $((port + 10))) (root "$root/on-a")
           (power "$root/power ax $((port + 10))"))
    (board "rv" (arch "riscv64") (address "127.0.0.1") (port $((port + 11))) (root "$root/on-b")
           (power "$root/power rv $((port + 11))"))))
  (lab (members
    (board "kept" (by_name true) (address "127.0.0.1") (port $((port + 6))) (root "$root/on-a")
           (power "$root/power kept $((port + 6))")))))
CONF
: > "$root/d.dead"

cp "$probe" "$root/project/tree/probe"
printf 'from the tree\n' > "$root/project/tree/srv/file"
conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "dir:./tree") %s)\n' "$*" > "$root/project/$name.conf"
}

# --- the plan ----------------------------------------------------------------

conf nopool '(backend hw) (command "/probe" "say" "no")'
expect 125 'a board comes from a pool' "$hull" run -f "$root/project/nopool.conf"
conf memory '(backend hw) (pool "bench") (memory "1G") (command "/probe" "say" "no")'
expect 1 'memory: rejected' "$hull" plan -f "$root/project/memory.conf"

# --- a run: power, deploy, command, clean, power ------------------------------

conf run '(backend hw) (pool "bench") (workdir "/srv") (env (GREETING "hi"))' \
	'(command "/probe" "say" "from the board" "env" "GREETING" "cwd" "read" "/srv/file" "exit" "3")'
expect 3 '^from the board$' "$hull" run -f "$root/project/run.conf"
says=$out
for line in '^GREETING=hi$' '^cwd=/srv$' '^read=from the tree$' 'console of a'; do
	cases=$((cases + 1))
	printf '%s\n' "$says" | grep -Eq "$line" || fail "the run said $line" "$says"
done
cases=$((cases + 1))
[ "$(cat "$root/power.log")" = "a on
a off" ] || fail 'the board was powered on, then off' "$(cat "$root/power.log")"
check 'its tree went with the run' test -z "$(ls -A "$root/on-a")"
check 'and the power off took it down' test ! -e "$root/a.pid"

# --- the one-line flip: the same file on chroot -------------------------------

printf '(machine (source "dir:./tree") (mutable overlay) (workdir "/srv") (env (GREETING "hi")) (command "/probe" "env" "GREETING" "cwd" "read" "/srv/file"))\n' \
	> "$root/project/flip.conf"
wanted=$(timeout 30 "$hull" run -f "$root/project/flip.conf" --backend chroot 2> /dev/null)
sed 's/(mutable overlay)/(mutable overlay) (pool "bench")/' "$root/project/flip.conf" > "$root/project/fliphw.conf"
got=$(timeout 60 "$hull" run -f "$root/project/fliphw.conf" --backend hw 2> /dev/null)
cases=$((cases + 1))
[ -n "$wanted" ] && [ "$wanted" = "$got" ] || fail 'a file says the same on chroot and on a board' "$wanted
---
$got"

# --- running: exec, a stop by its signal, the capacity ------------------------

conf onb '(backend hw) (pool "bench") (hostname "onb") (stop_signal "SIGQUIT") (command "/probe" "say" "up" "trap")'
timeout 60 "$hull" run -f "$root/project/onb.conf" > "$root/onb.out" 2>&1 &
pid=$!
n=0
while [ "$n" -lt 150 ] && ! grep -q '^ready' "$root/onb.out" 2> /dev/null; do
	sleep 0.2
	n=$((n + 1))
done
expect 0 '^in the tree$' "$hull" exec onb -- /probe say 'in the tree'
expect 0 '^cwd=/$' "$hull" exec onb -- /probe cwd
# Two boards, a run on each, and a third has none.
conf second '(backend hw) (pool "bench") (hostname "second") (command "/probe" "say" "on the other" "wait")'
timeout 60 "$hull" run -f "$root/project/second.conf" > "$root/second.out" 2>&1 &
second=$!
n=0
while [ "$n" -lt 150 ] && ! grep -q '^on the other' "$root/second.out" 2> /dev/null; do
	sleep 0.2
	n=$((n + 1))
done
conf third '(backend hw) (pool "bench") (hostname "third") (command "/probe" "say" "never")'
expect 75 'no slot of pool bench' "$hull" run --no-wait -f "$root/project/third.conf"
"$hull" stop second > /dev/null 2>&1
wait "$second"
"$hull" stop onb > /dev/null 2>&1
wait "$pid"
expect 0 '^signal=3$' cat "$root/onb.out"

# --- a port, at the machine's own address -------------------------------------

guest=$((port + 7))
hostport=$((port + 8))
conf served "(backend hw) (pool \"bench\") (hostname \"served\") (ports (port (host $hostport) (guest $guest)))" \
	"(command \"/probe\" \"serve\" \"tcp\" \"$guest\")"
timeout 40 "$hull" run -f "$root/project/served.conf" > "$root/served.out" 2>&1 &
pid=$!
n=0
address=
while [ "$n" -lt 100 ] && [ -z "$address" ]; do
	address=$("$hull" ps 2> /dev/null | awk '$1 == "served" && $5 == "running" { print $2 }')
	sleep 0.2
	n=$((n + 1))
done
out=
n=0
while [ "$n" -lt 40 ] && [ -z "$out" ]; do
	out=$(printf 'through the port\n' | timeout 3 nc -w 2 "$address" "$hostport" 2>&1)
	sleep 0.3
	n=$((n + 1))
done
wait "$pid"
cases=$((cases + 1))
[ -n "$out" ] || fail 'a port at the machine address reached the board' "$(tail -3 "$root/served.out")"
expect 0 'through the port' cat "$root/served.out"
cases=$((cases + 1))
! ps -eo args= | grep -F -- "$address:$hostport" | grep -qv grep || fail 'its forward went with the run' ''

# --- (ssh true): the board's sshd, and the board's key ------------------------

# ssh strict here, a known_hosts of the test's own, which the wrapper above
# is not: this is the one place the key is the point. The key is held under
# the board's address, as hull's own ssh to the board needs it, and a stale
# host key of the machine's, from a run on another backend, is planted to be
# passed over.
mkdir -p "$root/strict" "$root/data/hull/machines/door"
ssh-keygen -q -t ed25519 -N '' -f "$root/otherkey"
cp "$root/otherkey.pub" "$root/data/hull/machines/door/host_key.pub"
cat > "$root/strict/ssh" <<WRAP
#!/bin/sh
exec "$real_ssh" -i "$root/userkey" -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
	-o GlobalKnownHostsFile=/dev/null -o UserKnownHostsFile="$root/known" -o LogLevel=ERROR "\$@"
WRAP
chmod +x "$root/strict/ssh"
printf '[127.0.0.1]:%s %s\ndoor.hull %s\n' "$port" "$(cut -d' ' -f1,2 "$root/hostkey.pub")" \
	"$(cut -d' ' -f1,2 "$root/hostkey.pub")" > "$root/known"
conf door '(backend hw) (pool "bench") (hostname "door") (ssh true) (command "/probe" "sleep" "25")'
expect 0 'ssh +true|emulated' "$hull" plan -f "$root/project/door.conf"
timeout 40 "$hull" run -f "$root/project/door.conf" > "$root/door.out" 2>&1 &
pid=$!
n=0
state=
while [ "$n" -lt 100 ] && [ "$state" != running ]; do
	state=$("$hull" ps 2> /dev/null | awk '$1 == "door" { print $5 }')
	sleep 0.2
	n=$((n + 1))
done
# Into the board: `hull ssh`, a shell whose answer only a login there gives.
out=$( (printf 'echo door-$((6 * 7)); exit\n'; sleep 3) | PATH=$root/strict:$PATH timeout 15 \
	script -qec "$hull ssh door" /dev/null 2>&1 | tr -d '\r')
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'door-42$' || fail 'hull ssh reached the board on its own key' "$out"
# The same, the key a stranger's: refused, and no shell fallen back to.
cp "$root/known" "$root/known.good"
printf '[127.0.0.1]:%s %s\n' "$port" "$(cut -d' ' -f1,2 "$root/otherkey.pub")" > "$root/known"
out=$( (printf 'echo door-$((6 * 7)); exit\n'; sleep 2) | PATH=$root/strict:$PATH timeout 15 \
	script -qec "$hull ssh door" /dev/null 2>&1 | tr -d '\r')
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'Host key verification failed\|REMOTE HOST IDENTIFICATION' &&
	! printf '%s\n' "$out" | grep -q 'door-42$' || fail 'a known_hosts holding another key was refused' "$out"
cp "$root/known.good" "$root/known"
# `ssh door.hull`, by the ProxyCommand, its key under that name.
out=$(PATH=$root/strict:$PATH timeout 15 ssh -o ProxyCommand="$hull proxy-ssh %h" door.hull 'echo door-$((6 * 7))' 2>&1)
cases=$((cases + 1))
[ "$out" = door-42 ] || fail 'ssh door.hull reached the board' "$out"
cases=$((cases + 1))
[ ! -e "$root/data/hull/machines/door/host_key" ] || fail 'a host key was made for a board' ''
timeout 20 "$hull" stop --timeout 1 door > /dev/null 2>&1
wait "$pid"

# --- what a pool holds: a machine's spec chooses, the pool only supplies -------

conf fill '(backend hw) (pool "steer") (command "/probe" "say" "ran")'
conf soft '(backend chroot) (pool "steer") (command "/probe" "say" "soft")'
powered() { # WORD...: what went on since the log was emptied, in order
	awk '$2 == "on" { print $1 }' "$root/power.log" | tr '\n' ' '
}
: > "$root/power.log"
expect 0 '^ran$' "$hull" run -f "$root/project/fill.conf"
cases=$((cases + 1))
[ "$(powered)" = 'high ' ] || fail 'the board of the highest priority did not fill first' "$(cat "$root/power.log")"
# Named, the lease holds that board, and a run under it lands there.
lease=$(timeout 20 "$hull" reserve steer --member low)
expect 0 "^low +board +- +-1 +leased +$lease\$" "$hull" pool ls steer
: > "$root/power.log"
expect 0 '^ran$' "$hull" run --lease "$lease" -f "$root/project/fill.conf"
cases=$((cases + 1))
[ "$(powered)" = 'low ' ] || fail 'a run under a lease by name was not on that board' "$(cat "$root/power.log")"
expect 125 "lease $lease holds no slot for a software machine" "$hull" run --lease "$lease" -f "$root/project/soft.conf"
# With every board held the hw run is told 75, and a software machine still
# has the pool's capacity: neither is counted against the other.
any=$(timeout 20 "$hull" reserve steer --kind board)
expect 0 "^high +board +- +3 +leased +$any\$" "$hull" pool ls steer
expect 75 'no slot of pool steer: 2 of 2 in use' "$hull" run --no-wait -f "$root/project/fill.conf"
expect 0 '^soft$' "$hull" run --no-wait -f "$root/project/soft.conf"
expect 0 '' "$hull" release "$lease"
expect 0 '' "$hull" release "$any"
# A lease of a slot holds no board, so a hw run under it is told.
lease=$(timeout 20 "$hull" reserve steer --kind slot)
expect 125 "lease $lease holds no board free" "$hull" run --lease "$lease" -f "$root/project/fill.conf"
expect 0 '^high +board +- +3 +free +-$' "$hull" pool ls steer
expect 0 '^ran$' "$hull" run --no-wait -f "$root/project/fill.conf"
expect 0 '' "$hull" release "$lease"
# A board given only by name: a run is refused, naming the way; a lease by
# its name runs on it.
conf lab '(backend hw) (pool "lab") (command "/probe" "say" "ran")'
expect 125 'pool lab gives what this asks only by name' "$hull" run -f "$root/project/lab.conf"
lease=$(timeout 20 "$hull" reserve lab --member kept)
: > "$root/power.log"
expect 0 '^ran$' "$hull" run --lease "$lease" -f "$root/project/lab.conf"
cases=$((cases + 1))
[ "$(powered)" = 'kept ' ] || fail 'a lease of a board by name did not run on it' "$(cat "$root/power.log")"
expect 0 '' "$hull" release "$lease"
# What the pool has nothing for is refused: a chroot machine in a pool of
# boards would hold a board it does not use, a hw machine in a pool of
# software slots would have nowhere to go.
conf stray '(backend chroot) (pool "bench") (command "/probe" "say" "stray")'
expect 125 'pool bench has nothing for a ns machine' "$hull" run -f "$root/project/stray.conf"
conf noboard '(backend hw) (pool "soft") (command "/probe" "say" "no")'
expect 125 'pool soft has no boards for a hw machine' "$hull" run -f "$root/project/noboard.conf"
# A board is its own arch: a machine of another arch than the host's is
# planned for hw, and the pool gives it a board of that arch, which
# pools.conf says by (arch ...); a machine of an arch no board is, the
# host's by default, is refused rather than left waiting, and a lease's
# board of another arch is refused to it. A board's arch here is only a
# word, every board being this host.
conf onarm '(backend hw) (pool "arches") (arch arm64) (command "/probe" "say" "ran")'
conf onrv '(backend hw) (pool "arches") (arch riscv64) (command "/probe" "say" "ran")'
conf onhost '(backend hw) (pool "arches") (command "/probe" "say" "ran")'
expect 0 'machine on board \(hw\): ok' "$hull" check -f "$root/project/onarm.conf" --backend hw
expect 0 '^ax +board +arm64 +0 +free +-$' "$hull" pool ls arches
: > "$root/power.log"
expect 0 '^ran$' "$hull" run -f "$root/project/onarm.conf"
expect 0 '^ran$' "$hull" run -f "$root/project/onrv.conf"
cases=$((cases + 1))
[ "$(powered)" = 'ax rv ' ] || fail 'a machine was not given the board of its arch' "$(cat "$root/power.log")"
case $(uname -m) in
x86_64) host_arch=amd64 ;;
aarch64) host_arch=arm64 ;;
*) host_arch=$(uname -m) ;;
esac
[ "$host_arch" = arm64 ] || [ "$host_arch" = riscv64 ] ||
	expect 125 "pool arches has no board of arch $host_arch" "$hull" run -f "$root/project/onhost.conf"
# A lease of a board of an arch holds one of it, and none of an arch the
# pool has none of.
lease=$(timeout 20 "$hull" reserve arches --arch riscv64)
expect 0 "^rv +board +riscv64 +0 +leased +$lease\$" "$hull" pool ls arches
expect 0 '' "$hull" release "$lease"
cases=$((cases + 1))
said=$(timeout 20 "$hull" reserve arches --arch s390x --no-wait 2>&1) &&
	fail "a lease of a board of an arch no board is was given" "$said"
printf '%s\n' "$said" | grep -q 'pool arches has no board of arch s390x' ||
	fail "a lease of a board of an arch no board is was not refused naming it" "$said"
lease=$(timeout 20 "$hull" reserve arches --member rv)
expect 125 "board rv of lease $lease is riscv64, and the machine arm64" \
	"$hull" run --lease "$lease" -f "$root/project/onarm.conf"
expect 0 '' "$hull" release "$lease"

# --- a deploy of the board's own: an image flashed, a netboot served ----------

cat > "$root/flash" <<'FLASH'
#!/bin/sh
base=$(dirname "$0")
printf 'img flash\n' >> "$base/power.log"
printf '%s\n' "$1 $2" > "$base/flash.args"
cp -a "$1" "$2"
FLASH
chmod +x "$root/flash"
conf flashed '(backend hw) (pool "flashed") (workdir "/srv") (command "/probe" "say" "flashed" "read" "/srv/file")'
: > "$root/power.log"
expect 0 '^flashed$' "$hull" run -f "$root/project/flashed.conf"
cases=$((cases + 1))
[ "$(cat "$root/power.log")" = "img flash
img on
img off" ] || fail 'a deploy hook did not run with the board off, before its power' "$(cat "$root/power.log")"
cases=$((cases + 1))
grep -q "/tree $root/on-a/" "$root/flash.args" ||
	fail 'a deploy hook was not given the source and the tree' "$(cat "$root/flash.args")"
check 'and its tree went with the run' test -z "$(ls -A "$root/on-a")"
conf badflash '(backend hw) (pool "badflash") (command "/probe" "say" "never")'
expect 125 'marked broken: its deploy hook failed' "$hull" run -f "$root/project/badflash.conf"
expect 0 '' "$hull" board fix bf
conf noflash '(backend hw) (pool "noflash") (command "/probe" "say" "never")'
expect 125 'marked broken: the tree could not be deployed' "$hull" run -f "$root/project/noflash.conf"
expect 0 '' "$hull" board fix nf

# --- recovery: cycled, then marked, and out of its pool -----------------------

conf sick '(backend hw) (pool "sick") (command "/probe" "say" "never")'
expect 125 'marked broken: its health check failed' "$hull" run -f "$root/project/sick.conf"
expect 0 '^s +127.0.0.1 +broken' "$hull" board ls sick
# Every board broken is no wait's to end: said at once, naming what fixes it.
expect 125 'every board of pool sick is broken' "$hull" run --no-wait -f "$root/project/sick.conf"
expect 125 'member s of pool sick is broken' "$hull" reserve sick --member s
expect 0 '^s +board +- +0 +broken +-$' "$hull" pool ls sick
expect 0 '' "$hull" board fix s
expect 0 '^s +127.0.0.1 +up' "$hull" board ls sick
: > "$root/power.log"
conf dark '(backend hw) (pool "dark") (command "/probe" "say" "never")'
expect 125 'marked broken: no sshd' "$hull" run -f "$root/project/dark.conf"
cases=$((cases + 1))
grep -q '^d cycle$' "$root/power.log" || fail 'a board that did not answer was cycled first' "$(cat "$root/power.log")"

# A tree that will not deploy: cycled, tried again, then marked. Its root is a
# file, which no power cycle mends.
: > "$root/notadir"
python3 -I - "$XDG_CONFIG_HOME/hull/pools.conf" "$root" "$port" <<'PY'
import sys
path, root, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
text = open(path).read().rstrip()
assert text.endswith(")")
text = text[:-1] + """
  (nodeploy (members
    (board "n" (address "127.0.0.1") (port %d) (root "%s/notadir")
           (power "%s/power n %d"))))
  (bench2 (members
    (board "m" (address "127.0.0.1") (port %d) (root "%s/on-a")
           (power "%s/power m %d")))))
""" % (port + 4, root, root, port + 4, port + 5, root, root, port + 5)
open(path, "w").write(text)
PY
: > "$root/power.log"
conf nodeploy '(backend hw) (pool "nodeploy") (command "/probe" "say" "never")'
expect 125 'marked broken: the tree could not be deployed' "$hull" run -f "$root/project/nodeploy.conf"
cases=$((cases + 1))
grep -q '^n cycle$' "$root/power.log" || fail 'a failed deploy was cycled first' "$(cat "$root/power.log")"

# A board going away under its command: said, marked, and not the command's
# exit. Its sshd killed from outside, as a board losing power would be.
conf gone '(backend hw) (pool "bench2") (hostname "gone") (command "/probe" "say" "up" "wait")'
timeout 60 "$hull" run -f "$root/project/gone.conf" > "$root/gone.out" 2>&1 &
pid=$!
n=0
while [ "$n" -lt 150 ] && ! grep -q '^up' "$root/gone.out" 2> /dev/null; do
	sleep 0.2
	n=$((n + 1))
done
kill "$(cat "$root/m.pid")" 2> /dev/null
rm -f "$root/m.pid"
: > "$root/m.dead"
for p in $(ps -eo pid=,args= | awk -v want="$root/m.conf" 'index($0, want) { print $1 }'); do
	kill -9 "$p" 2> /dev/null
done
wait "$pid"
cases=$((cases + 1))
grep -q 'marked broken: it went away' "$root/gone.out" || fail 'a board that went away was marked' "$(tail -3 "$root/gone.out")"

# --- a hull vm as the board --------------------------------------------------

# Hull on both sides: the bench's core runs a machine on a board that is a vm
# of an inner core, whose power hook is `hull run` and `hull stop`. It needs
# KVM and an image: said and left where either is missing.
if [ -w /dev/kvm ] && timeout 200 "$hull" pull alpine:3.20 alpine:3.20 > "$root/pull.out" 2>&1; then
	key=$(cat "$root/userkey.pub")
	cat > "$root/project/board.build" <<CONF
(build (base "alpine:3.20") (steps
  (step (name "sshd") (network true) (run "apk add --no-cache openssh-server util-linux-misc tar && ssh-keygen -A && mkdir -p /root/.ssh && chmod 700 /root/.ssh && echo '$key' > /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys && sed -i 's/^root:!:/root:*:/' /etc/shadow; printf 'PermitRootLogin prohibit-password\nStrictModes no\n' >> /etc/ssh/sshd_config"))))
CONF
	timeout 200 "$hull" build -f "$root/project/board.build" boardimg > "$root/build.out" 2>&1
	expect 0 '' "$hull" tag boardimg boardimg2
	mkdir -p "$root/inner" "$root/vb"
	chmod 700 "$root/inner"
	vmport=$((port + 6))
	cat > "$root/vb/hull.conf" <<CONF
(machine (backend vm) (source "boardimg") (memory "256M") (hostname "vmboard") (mutable overlay)
  (ports (port (host $vmport) (guest 22))) (command "/usr/sbin/sshd" "-D" "-e"))
CONF
	cat > "$root/vmpower" <<POWER
#!/bin/sh
printf 'vm %s\n' "\$1" >> "$root/vm.log"
export XDG_RUNTIME_DIR="$root/inner" HULL_CORE_IDLE=5
on() {
	cd "$root/vb" && nohup timeout 120 "$hull" run -f hull.conf > "$root/vb.out" 2>&1 < /dev/null &
}
off() {
	"$hull" stop --timeout 3 vmboard > /dev/null 2>&1
}
case \$1 in
on) on ;;
off) off ;;
cycle) off; sleep 1; on ;;
esac
exit 0
POWER
	chmod +x "$root/vmpower"
	# Its address is hull's to give, so asked of a first boot.
	"$root/vmpower" on
	n=0
	address=
	while [ "$n" -lt 100 ] && [ -z "$address" ]; do
		address=$(XDG_RUNTIME_DIR=$root/inner "$hull" ps 2> /dev/null | awk '$1 == "vmboard" { print $2 }')
		sleep 0.2
		n=$((n + 1))
	done
	"$root/vmpower" off
	sleep 3
	: > "$root/vm.log"
	python3 -I - "$XDG_CONFIG_HOME/hull/pools.conf" "$root" "$address" "$vmport" <<'PY'
import sys
path, root, address, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
text = open(path).read().rstrip()
text = text[:-1] + """
  (vmbench (members
    (board "vmb" (address "%s") (port %s) (login "root") (power "%s/vmpower")
           (console "echo console of a vm")))))
""" % (address, port, root)
open(path, "w").write(text)
PY
	conf onvm '(backend hw) (pool "vmbench") (workdir "/srv") (env (GREETING "from the bench"))' \
		'(command "/probe" "say" "on a hull vm" "env" "GREETING" "read" "/srv/file" "exit" "7")'
	expect 7 '^on a hull vm$' "$hull" run -f "$root/project/onvm.conf"
	says=$out
	for line in '^GREETING=from the bench$' '^read=from the tree$' 'console of a vm'; do
		cases=$((cases + 1))
		printf '%s\n' "$says" | grep -Eq "$line" || fail "on a vm board, $line" "$says"
	done
	cases=$((cases + 1))
	[ "$(cat "$root/vm.log")" = "vm on
vm off" ] || fail 'the vm was powered on, then off' "$(cat "$root/vm.log")"
	sleep 4
	out=$(XDG_RUNTIME_DIR=$root/inner "$hull" ps 2> /dev/null | awk '$1 == "vmboard" { print $5 }')
	cases=$((cases + 1))
	[ "$out" != running ] || fail 'power off stopped the vm' "$out"
else
	printf 'hw: no KVM or no alpine image, a hull vm as a board not tried\n'
fi

printf 'hw: %s cases, %s failed\n' "$cases" "$failures"
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
