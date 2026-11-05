#!/bin/sh
# remote_test.sh -- `hull -H ssh://...` (project.md section 4.10): a core
# on another host, reached over ssh. Not part of `make check`, which starts
# no sshd: `make check-remote` runs it. Its arguments are build/hull and
# the static probe. "Another host" is a private sshd on 127.0.0.1, run as
# the user on a port of its own, whose sessions have a runtime directory
# this frontend's does not, so a machine that ran at the far end cannot
# have run here. The sshd is killed by its pid at the end and on a signal,
# what starts is under `timeout`, and everything is under $BUILD_DIR/test/remote.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
dir=${BUILD_DIR:?}/test/remote
case $dir in
*/test/remote) ;;
*) printf 'remote: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir/near" "$dir/far" "$dir/bin" "$dir/site/tree/proc" "$dir/site/tree/dev" \
	"$dir/site/tree/sys" "$dir/here"
chmod 700 "$dir/near" "$dir/far"
root=$(cd "$dir" && pwd)
sshd=/usr/sbin/sshd
real_ssh=$(command -v ssh)
if [ ! -x "$sshd" ] || [ -z "$real_ssh" ]; then
	printf 'remote: no sshd or ssh here\n'
	exit 1
fi
cases=0
failures=0
XDG_RUNTIME_DIR=$root/near
HULL_REMOTE_HULL=$hull
PATH=$root/bin:$PATH
export XDG_RUNTIME_DIR HULL_REMOTE_HULL PATH

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
	out=$(timeout 60 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

port=$((20000 + $$ % 20000))
ssh-keygen -q -t ed25519 -N '' -f "$root/hostkey"
ssh-keygen -q -t ed25519 -N '' -f "$root/userkey"
cp "$root/userkey.pub" "$root/authorized"
cat > "$root/sshd_config" <<CONF
Port $port
ListenAddress 127.0.0.1
HostKey $root/hostkey
AuthorizedKeysFile $root/authorized
PidFile $root/sshd.pid
StrictModes no
UsePAM no
SetEnv XDG_RUNTIME_DIR=$root/far HULL_CORE_IDLE=1
CONF
"$sshd" -D -e -f "$root/sshd_config" > "$root/sshd.log" 2>&1 &
server=$!
stop_server() {
	kill "$server" 2> /dev/null
}
trap stop_server EXIT
trap 'stop_server; exit 1' INT TERM

# The frontend's `ssh`: the user's, told of this sshd.
cat > "$root/bin/ssh" <<WRAP
#!/bin/sh
exec "$real_ssh" -i "$root/userkey" -o IdentitiesOnly=yes -o BatchMode=yes \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "\$@"
WRAP
chmod +x "$root/bin/ssh"
n=0
while [ "$n" -lt 50 ] && ! ssh -p "$port" 127.0.0.1 true 2> /dev/null; do
	sleep 0.2
	n=$((n + 1))
done
if [ "$n" = 50 ]; then
	fail 'the private sshd answers' "$(cat "$root/sshd.log")"
	exit 1
fi

url=ssh://127.0.0.1:$port
cp "$probe" "$root/site/tree/probe"
printf 'from the far side\n' > "$root/site/tree/where"
printf '(machine (source "dir:./tree") (mutable overlay) (command "/probe" "read" "/where"))\n' \
	> "$root/here/hull.conf"

# A bad address is said, and an unreachable host ends rather than hangs.
expect 2 'ssh://' "$hull" -H nonsense ps
expect 2 'no host' "$hull" -H ssh:// ps
expect 2 'not a number' "$hull" -H ssh://h:x ps
expect 2 'Connection refused' "$hull" -H ssh://127.0.0.1:1 ps

expect 0 '^ID' "$hull" -H "$url" ps
cases=$((cases + 1))
[ -S "$root/far/hull/core.sock" ] || fail 'a core was started at the far end' "$(ls -la "$root/far")"
cases=$((cases + 1))
[ ! -e "$root/near/hull/core.sock" ] || fail 'and none here' "$(ls -la "$root/near/hull")"

# The file is here, the tree there, and the machine's output and status
# come back.
cases=$((cases + 1))
[ ! -e "$root/here/tree" ] || fail 'the tree is not here' ''
expect 0 'from the far side' "$hull" -H "$url$root/site" run -f "$root/here/hull.conf"
printf '(machine (source "dir:./tree") (mutable overlay) (command "/probe" "exit" "7"))\n' \
	> "$root/here/seven.conf"
expect 7 '' "$hull" -H "$url$root/site" run -f "$root/here/seven.conf"

# HULL_HOST is -H's value, and a running machine is reached by exec, ps,
# stop, as a local one.
printf '(machine (source "dir:./tree") (mutable overlay) (command "/probe" "sleep" "60"))\n' \
	> "$root/here/idle.conf"
HULL_HOST=$url$root/site
export HULL_HOST
timeout 60 "$hull" run -f "$root/here/idle.conf" > "$root/idle.out" 2>&1 &
run=$!
n=0
until "$hull" ps 2> /dev/null | grep -q running || [ "$n" -ge 50 ]; do
	sleep 0.2
	n=$((n + 1))
done
id=$("$hull" ps | awk '$NF == "running" { print $1 }')
expect 0 'from the far side' "$hull" exec "$id" -- /probe read /where
expect 0 '' "$hull" stop --timeout 1 "$id"
wait "$run"

# A frontend killed: its ssh goes with it, the far end reads the end of
# the connection, and the machine goes, as (on_disconnect stop) says.
timeout -s KILL 8 "$hull" run -f "$root/here/idle.conf" > "$root/killed.out" 2>&1
n=0
while "$hull" ps 2> /dev/null | grep -q running && [ "$n" -lt 50 ]; do
	sleep 0.2
	n=$((n + 1))
done
cases=$((cases + 1))
out=$("$hull" ps 2>&1)
if printf '%s\n' "$out" | grep -q running; then
	fail 'a killed frontend took its far machine with it' "$out"
fi
unset HULL_HOST

# What works on this host's store does not pretend otherwise.
expect 2 'on this host' "$hull" -H "$url" build -f "$root/here/hull.conf" some/ref
expect 2 'on this host' "$hull" -H "$url" images

printf 'remote: %s cases, %s failed\n' "$cases" "$failures"
stop_server
trap - EXIT INT TERM
wait "$server" 2> /dev/null
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
