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
           (power "$root/power d $((port + 3))")))))
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

# --- recovery: cycled, then marked, and out of its pool -----------------------

conf sick '(backend hw) (pool "sick") (command "/probe" "say" "never")'
expect 125 'marked broken: its health check failed' "$hull" run -f "$root/project/sick.conf"
expect 0 '^s +127.0.0.1 +broken' "$hull" board ls sick
# Every board broken is no wait's to end: said at once, naming what fixes it.
expect 125 'no board free and not broken' "$hull" run --no-wait -f "$root/project/sick.conf"
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
