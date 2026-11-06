#!/bin/sh
# A pool's broker across a real wire (project.md section 4.12): three vms on
# a segment of hull's own, each with a kernel, a TCP stack and an sshd or ssh
# of its own, the broker in one and a user in each of the others. What a
# network does to it is the switch's to do: delay, loss, and a cut, in the
# middle of a run and under a request waiting in line, which loopback cannot.
# Bounded by each command's timeout; the machines end with the run the trap
# stops, and sleep no longer than this suite's own ceiling.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/broker_wire
rm -rf "${dir:?}"
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
wire=bwire-$$
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

mkdir -p "$root/run" "$root/tree"
chmod 700 "$root/run"
cp "$probe" "$root/tree/probe"
host() { # COMMAND...: hull on the host, which runs the vms and the wire
	env HOME="$root" XDG_RUNTIME_DIR="$root/run" XDG_CONFIG_HOME="$root/config" \
		XDG_DATA_HOME="$root/data" DOCKER_HOST="unix://$root/none.sock" "$@"
}
# within USER MACHINE COMMAND...: as that user, in that machine.
within() {
	u=$1
	m=$2
	shift 2
	host timeout 150 "$hull" exec "$m" -- /usr/bin/env HOME="$root/$u" XDG_RUNTIME_DIR="/tmp/$u/run" \
		XDG_CONFIG_HOME="$root/$u/config" XDG_DATA_HOME="/tmp/$u/data" PATH="$root/$u/bin:/usr/bin:/bin" \
		HULL_CORE_IDLE=1 "$@"
}
hull_in() { # USER MACHINE STATUS PATTERN ARGS...
	u=$1
	m=$2
	want=$3
	pattern=$4
	shift 4
	cases=$((cases + 1))
	out=$(within "$u" "$m" timeout 120 "$hull" "$@" 2>&1 < /dev/null)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$u: hull $* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$u: hull $*: no line matches /$pattern/" "$out"
	fi
}
impair() { # MACHINE KEY=VALUE...: both ways of its port
	m=$1
	shift
	cases=$((cases + 1))
	out=$(host timeout 30 "$hull" segment "$wire" impair "$m" both "$@" 2>&1) ||
		fail "the wire to $m was not impaired: $*" "$out"
}

ssh-keygen -q -t ed25519 -N '' -f "$root/hostkey"
for u in alice bob; do
	mkdir -p "$root/$u/config/hull" "$root/$u/bin"
	ssh-keygen -q -t ed25519 -N '' -f "$root/$u/key"
	printf 'command="%s broker-dial --dir /tmp/b --as %s",restrict %s\n' "$hull" "$u" \
		"$(cat "$root/$u/key.pub")" >> "$root/authorized"
	cat > "$root/$u/bin/ssh" <<WRAP
#!/bin/sh
exec /usr/bin/ssh -F /dev/null -i "$root/$u/key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
	-o UserKnownHostsFile="$root/known_hosts" -o LogLevel=ERROR "\$@"
WRAP
	chmod +x "$root/$u/bin/ssh"
	cat > "$root/$u/config/hull/pools.conf" <<CONF
(pools
  (bench (broker "ssh://root@bsite/tmp/b"))
  (many (broker "ssh://root@bsite/tmp/b")))
CONF
done
printf 'bsite %s\n' "$(cat "$root/hostkey.pub")" > "$root/known_hosts"
cat > "$root/site.sh" <<SITE
#!/bin/sh
# The root is the host's tree, whose /run this kernel sees as nobody's, and
# sshd holds that against it.
mount -t tmpfs tmpfs /run
mkdir -p /run/sshd /tmp/b /tmp/k
chmod 750 /tmp/b
cp "$root/hostkey" /tmp/k/hostkey
cp "$root/authorized" /tmp/k/authorized
chmod 600 /tmp/k/hostkey
cat > /tmp/b/pools.conf <<'CONF'
(pools
  (bench (capacity 1) (members (slot "gpu" (by_name true))))
  (many (capacity 8)))
CONF
chmod 640 /tmp/b/pools.conf
cat > /tmp/k/sshd.conf <<'CONF'
Port 22
HostKey /tmp/k/hostkey
AuthorizedKeysFile /tmp/k/authorized
StrictModes no
UsePAM no
PermitRootLogin forced-commands-only
ClientAliveInterval 5
ClientAliveCountMax 2
CONF
/usr/sbin/sshd -f /tmp/k/sshd.conf -E /tmp/k/sshd.log
HOME=/tmp XDG_RUNTIME_DIR=/tmp/k exec "$hull" broker /tmp/b
SITE
cat > "$root/user.sh" <<'USER'
#!/bin/sh
mkdir -p /tmp/alice/run /tmp/bob/run /tmp/alice/data /tmp/bob/data
chmod 700 /tmp/alice/run /tmp/bob/run
echo ready
exec sleep 500
USER
cat > "$root/hull.conf" <<CONF
(machines
  (bsite (backend vm) (source "dir:/") (mutable overlay) (memory "1G") (network none)
    (segments ($wire (subnet "10.93.0.0/24")))
    (command "/bin/sh" "$root/site.sh"))
  (balice (backend vm) (source "dir:/") (mutable overlay) (memory "1G") (network none)
    (segments ($wire (subnet "10.93.0.0/24")))
    (command "/bin/sh" "$root/user.sh"))
  (bbob (backend vm) (source "dir:/") (mutable overlay) (memory "1G") (network none)
    (segments ($wire (subnet "10.93.0.0/24")))
    (command "/bin/sh" "$root/user.sh")))
CONF
cat > "$root/tree.conf" <<CONF
(machine (source "dir:$root/tree") (backend chroot) (network none) (pool "many") (hostname "work")
  (command "/probe" "say" "ready" "sleep" "8"))
CONF

host timeout -k 5 540 "$hull" run -f "$root/hull.conf" > "$root/vms.out" 2>&1 < /dev/null &
vms=$!
stop_all() {
	for m in bsite balice bbob; do
		host timeout 30 "$hull" stop --timeout 1 "$m" > /dev/null 2>&1
	done
	kill "$vms" 2> /dev/null
	wait "$vms" 2> /dev/null
}
trap stop_all EXIT
trap 'stop_all; exit 1' INT TERM

n=0
until [ "$(grep -c '^ready$' "$root/vms.out")" = 2 ] || [ "$n" -ge 240 ]; do
	n=$((n + 1))
	sleep 0.5
done
n=0
until within alice balice timeout 40 "$hull" pool ls < /dev/null 2>&1 | grep -q '^bench ' || [ "$n" -ge 30 ]; do
	n=$((n + 1))
	sleep 1
done

# --- on a wire, each from a kernel of its own ---------------------------------

hull_in alice balice 0 '^bench +2 +0 +0$' pool ls
lease=$(within alice balice timeout 60 "$hull" reserve bench --member gpu 2> "$root/r.err" < /dev/null)
cases=$((cases + 1))
case $lease in lease-*) ;; *) fail 'no lease across the wire' "$lease $(cat "$root/r.err")" ;; esac
hull_in bob bbob 0 "^$lease +bench +1 +0 +- +alice\$" lease ls
hull_in bob bbob 75 'gpu is held' reserve bench --member gpu --no-wait

# --- a bad line: slower, and nothing else --------------------------------------

impair balice delay=150 jitter=50 loss=0.1
impair bbob delay=150 jitter=50 loss=0.1
hull_in alice balice 0 '' release "$lease"
hull_in bob bbob 0 '^lease-' reserve many --no-wait
hull_in bob bbob 0 '^many +8 +1 +0$' pool ls
host timeout 30 "$hull" segment "$wire" > "$root/counts.out" 2>&1
# That the line was bad is the switch's to say, not this suite's to assume.
cases=$((cases + 1))
grep -Eq '^balice +out .* lost=[1-9]' "$root/counts.out" && grep -Eq '^bbob +in .* lost=[1-9]' "$root/counts.out" ||
	fail 'the wire lost nothing' "$(cat "$root/counts.out")"
impair balice
impair bbob

# --- cut in the middle of a run -----------------------------------------------

: > "$root/work.out"
(within alice balice timeout -k 2 100 "$hull" run -f "$root/tree.conf" > "$root/work.out" 2>&1 < /dev/null) &
work=$!
n=0
until grep -q '^ready$' "$root/work.out" || [ "$n" -ge 120 ]; do
	n=$((n + 1))
	sleep 0.5
done
impair balice down
wait "$work"
cases=$((cases + 1))
grep -q '^ready$' "$root/work.out" || fail 'the run did not run through the cut' "$(cat "$root/work.out")"
# Alice cannot say it is over, so bob's side of the wire still sees it held.
hull_in bob bbob 0 '^many +8 +2 +0$' pool ls
hull_in alice balice 75 'cannot be reached' pool ls many
impair balice
n=0
until within bob bbob timeout 40 "$hull" pool ls < /dev/null 2>&1 | grep -q '^many  *8  *1 ' || [ "$n" -ge 70 ]; do
	n=$((n + 1))
	sleep 1
done
hull_in bob bbob 0 '^many +8 +1 +0$' pool ls

# --- cut under a request waiting in line --------------------------------------

# The grant goes down a wire nobody is at the end of. Given to that, the entry
# would be held for good by one who never heard of it.
kept=$(within alice balice timeout 60 "$hull" reserve bench --member gpu < /dev/null)
: > "$root/waiting.out"
(within bob bbob timeout -k 2 120 "$hull" reserve bench --member gpu --wait 100s > "$root/waiting.out" 2>&1 < /dev/null
 echo "status $?" >> "$root/waiting.out") &
waiting=$!
sleep 4
impair bbob down
hull_in alice balice 0 '' release "$kept"
n=0
got=
until [ -n "$got" ] || [ "$n" -ge 90 ]; do
	got=$(within alice balice timeout 20 "$hull" reserve bench --member gpu --no-wait 2> /dev/null < /dev/null)
	n=$((n + 1))
	sleep 1
done
cases=$((cases + 1))
case $got in
lease-*) ;;
*) fail 'a grant to a cut wire kept the entry' "$(within alice balice timeout 20 "$hull" pool ls bench 2>&1)" ;;
esac
wait "$waiting"
cases=$((cases + 1))
grep -q '^status 75$' "$root/waiting.out" || fail 'a request cut off did not end 75' "$(cat "$root/waiting.out")"
impair bbob

# Nothing of three vms outlives the suite.
stop_all
trap - EXIT
cases=$((cases + 1))
n=0
until [ -z "$(ps -eo args= | grep "[q]emu-system.*$root")" ] || [ "$n" -ge 40 ]; do
	n=$((n + 1))
	sleep 0.5
done
left=$(ps -eo pid=,args= | grep "[q]emu-system.*$root" | cut -c1-100)
[ -z "$left" ] || fail 'a vm outlived the suite' "$left"

printf 'broker wire: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
