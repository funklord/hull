#!/bin/sh
# A pool's broker (project.md section 4.12), between two users on a real
# sshd: its keys forced to `hull broker-dial --as`, known_hosts strict, so
# what crosses is what a network would carry. Each user is a home, a
# runtime directory and a core of its own. One account plays both, which
# is what --as is for; what the kernel says of a second account is not
# measured here. Bounded by each command's timeout and the sshd's and
# broker's pids, which the trap ends.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/broker
rm -rf "${dir:?}"
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
broker=$root/b
port=$((21000 + $$ % 20000))
account=$(id -un)
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

run_as() { # USER COMMAND...
	u=$1
	shift
	env HOME="$root/$u" XDG_RUNTIME_DIR="$root/$u/run" XDG_CONFIG_HOME="$root/$u/config" \
		XDG_DATA_HOME="$root/$u/data" PATH="$root/$u/bin:$PATH" HULL_CORE_IDLE=1 \
		DOCKER_HOST="unix://$root/none.sock" "$@"
}

# as USER STATUS PATTERN ARGS...: hull as that user.
as() {
	who=$1
	want=$2
	pattern=$3
	shift 3
	cases=$((cases + 1))
	out=$(run_as "$who" timeout 60 "$hull" "$@" 2>&1 < /dev/null)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$who: hull $* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$who: hull $*: no line matches /$pattern/" "$out"
	fi
}

ssh-keygen -q -t ed25519 -N '' -f "$root/hostkey"
for u in alice bob; do
	mkdir -p "$root/$u/run" "$root/$u/config/hull" "$root/$u/data" "$root/$u/bin" "$root/$u/.ssh"
	chmod 700 "$root/$u/run"
	ssh-keygen -q -t ed25519 -N '' -f "$root/$u/key"
	printf 'command="%s broker-dial --dir %s --as %s",restrict %s\n' "$hull" "$broker" "$u" \
		"$(cat "$root/$u/key.pub")" >> "$root/authorized"
	cat "$root/$u/key.pub" >> "$root/board-authorized"
	cat > "$root/$u/bin/ssh" <<WRAP
#!/bin/sh
exec /usr/bin/ssh -F /dev/null -i "$root/$u/key" -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes \
	-o UserKnownHostsFile="$root/known_hosts" -o LogLevel=ERROR "\$@"
WRAP
	chmod +x "$root/$u/bin/ssh"
done
printf '[127.0.0.1]:%s %s\n[127.0.0.1]:%s %s\n' "$port" "$(cat "$root/hostkey.pub")" \
	"$((port + 1))" "$(cat "$root/hostkey.pub")" > "$root/known_hosts"

sshd_up() { # NAME PORT AUTHORIZED
	: > "$root/$1.pid"
	cat > "$root/$1.conf" <<CONF
Port $2
ListenAddress 127.0.0.1
HostKey $root/hostkey
AuthorizedKeysFile $3
PidFile $root/$1.pid
StrictModes no
UsePAM no
CONF
	/usr/sbin/sshd -f "$root/$1.conf"
	n=0
	until [ -s "$root/$1.pid" ] || [ "$n" -ge 50 ]; do
		n=$((n + 1))
		sleep 0.1
	done
}
sshd_down() { # NAME
	[ -s "$root/$1.pid" ] && kill "$(cat "$root/$1.pid")" 2> /dev/null
	: > "$root/$1.pid"
}
broker_up() {
	"$hull" broker "$broker" >> "$root/broker.log" 2>&1 &
	broker_pid=$!
	n=0
	until [ -S "$broker/core.sock" ] && [ -s "$broker/core.lock" ] || [ "$n" -ge 50 ]; do
		n=$((n + 1))
		sleep 0.1
	done
}
stop_all() {
	sshd_down gate
	sshd_down board
	[ -n "${broker_pid:-}" ] && kill "$broker_pid" 2> /dev/null
	for u in alice bob; do
		run_as "$u" timeout 10 "$hull" stop --timeout 1 onboard > /dev/null 2>&1
		run_as "$u" timeout 10 "$hull" stop --timeout 1 soft > /dev/null 2>&1
	done
}
trap stop_all EXIT
trap 'stop_all; exit 1' INT TERM

mkdir -p "$broker" "$root/project/tree/srv" "$root/on-board"
printf '#!/bin/sh\nexit 0\n' > "$root/health"
chmod +x "$root/health"
chmod 770 "$broker"
cp "$probe" "$root/project/tree/probe"
cat > "$broker/pools.conf" <<'CONF'
(pools
  (bench (capacity 1)
    (members (board "b1" (priority 5) (address "127.0.0.1"))
             (board "unknown" (priority -5) (address "127.0.0.1"))
             (slot "gpu" (by_name true))))
  (many (capacity 60))
  (budget (capacity 4) (memory "1G")))
CONF
for u in alice bob; do
	cat > "$root/$u/config/hull/pools.conf" <<CONF
(pools
  (bench (broker "ssh://$account@127.0.0.1:$port$broker")
    (members (board "b1" (address "127.0.0.1") (port $((port + 1))) (root "$root/on-board")
           (health "$root/health"))))
  (many (broker "ssh://$account@127.0.0.1:$port$broker"))
  (budget (broker "ssh://$account@127.0.0.1:$port$broker"))
  (mine (capacity 1)))
CONF
done
sshd_up gate "$port" "$root/authorized"
sshd_up board "$((port + 1))" "$root/board-authorized"
broker_up

conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "dir:./tree") %s)\n' "$*" > "$root/project/$name.conf"
}
wait_ready() { # FILE
	n=0
	until grep -q '^ready$' "$1" || [ "$n" -ge 80 ]; do
		n=$((n + 1))
		sleep 0.25
	done
}

# --- hull's part in the network is none ---------------------------------------

cases=$((cases + 1))
inet=$(for fd in /proc/"$broker_pid"/fd/*; do
	inode=$(readlink "$fd" | sed -n 's/^socket:\[\([0-9]*\)\]$/\1/p')
	[ -n "$inode" ] && grep -hw "$inode" /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6
done 2> /dev/null)
[ -z "$inet" ] || fail 'a broker holds a network socket of its own' "$inet"
cases=$((cases + 1))
[ "$(stat -c '%a' "$broker/core.sock")" = 660 ] ||
	fail 'a broker socket is not its group only' "$(stat -c '%a %A' "$broker/core.sock")"
# A key is the broker and nothing else: no shell, whatever is asked for.
cases=$((cases + 1))
out=$(run_as alice timeout 20 ssh -p "$port" "$account@127.0.0.1" id < /dev/null 2>&1)
printf '%s\n' "$out" | grep -q 'uid=' && fail 'a broker key gave a shell' "$out"

# --- a pool between two users -------------------------------------------------

as alice 0 '^bench +4 +0 +0$' pool ls
as alice 0 '^mine +1 +0 +0$' pool ls
as alice 0 '^gpu +slot +by-name +free +-$' pool ls bench
lease=$(run_as alice timeout 30 "$hull" reserve bench --member gpu < /dev/null)
as bob 0 "^gpu +slot +by-name +leased +$lease\$" pool ls bench
as bob 75 'no slot of pool bench: gpu is held' reserve bench --member gpu --no-wait
as bob 0 "^$lease +bench +1 +0 +- +alice\$" lease ls
# What is alice's is not bob's to end, or to run under.
as bob 125 'no such lease is held' release "$lease"
as alice 0 "^$lease " lease ls
conf soft '(backend chroot) (pool "bench") (hostname "soft") (command "/probe" "say" "ready" "sleep" "6")'
conf quick '(backend chroot) (pool "bench") (hostname "quick") (command "/probe" "say" "quick")'
as bob 125 'no such lease is held' run --lease "$lease" -f "$root/project/quick.conf"
as alice 0 '^quick$' run --lease "$lease" -f "$root/project/quick.conf"
as alice 0 '' release "$lease"
as bob 0 '^gpu +slot +by-name +free +-$' pool ls bench

# --- a run: asked there, run here, given back ---------------------------------

: > "$root/soft.out"
(run_as alice timeout -k 2 40 "$hull" run -f "$root/project/soft.conf" > "$root/soft.out" 2>&1 < /dev/null) &
soft=$!
wait_ready "$root/soft.out"
as bob 75 'no slot of pool bench: 1 of 1 in use' run --no-wait -f "$root/project/quick.conf"
wait "$soft"
n=0
until run_as bob timeout 30 "$hull" pool ls < /dev/null | grep -q '^bench  *4  *0 ' || [ "$n" -ge 40 ]; do
	n=$((n + 1))
	sleep 0.25
done
as bob 0 '^quick$' run --no-wait -f "$root/project/quick.conf"

# --- a board: one holder, whatever two cores think ----------------------------

conf board '(backend hw) (pool "bench") (hostname "onboard") (command "/probe" "say" "ready" "sleep" "40")'
conf board2 '(backend hw) (pool "bench") (hostname "onboard2") (command "/probe" "say" "never")'
: > "$root/board.out"
(run_as alice timeout -k 2 70 "$hull" run -f "$root/project/board.conf" > "$root/board.out" 2>&1 < /dev/null) &
onboard=$!
wait_ready "$root/board.out"
as bob 0 '^b1 +board +5 +leased ' pool ls bench
# bob's --no-wait takes the board left, which his file does not describe:
# told, and it goes back.
as bob 125 'which pools.conf here does not describe' run --no-wait -f "$root/project/board2.conf"
run_as alice timeout 20 "$hull" stop --timeout 1 onboard > /dev/null 2>&1
wait "$onboard"
cases=$((cases + 1))
grep -q '^ready$' "$root/board.out" || fail 'a run on a broker board did not run' "$(cat "$root/board.out")"
n=0
until run_as alice timeout 30 "$hull" pool ls < /dev/null | grep -q '^bench  *4  *0 ' || [ "$n" -ge 40 ]; do
	n=$((n + 1))
	sleep 1
done
as alice 0 '^bench +4 +0 +0$' pool ls

# --- a lease's end reaches a machine a network away ---------------------------

ending=$(run_as alice timeout 30 "$hull" reserve many --for 3s < /dev/null)
conf ends '(backend chroot) (pool "many") (hostname "ends") (command "/probe" "say" "ready" "sleep" "25")'
as alice 79 'its broker.s lease ended: its time ran out' run --lease "$ending" -f "$root/project/ends.conf"

# --- a budget is the broker's, across users -----------------------------------

conf nobudget '(backend chroot) (pool "budget") (hostname "nb") (command "/probe" "say" "no")'
as alice 125 'pool budget has a budget of what machines declare' run -f "$root/project/nobudget.conf"

# --- the link gone: nothing is freed, and nothing is lost ---------------------

conf cut '(backend chroot) (pool "many") (hostname "soft") (command "/probe" "say" "ready" "sleep" "3")'
: > "$root/cut.out"
(run_as alice timeout -k 2 40 "$hull" run -f "$root/project/cut.conf" > "$root/cut.out" 2>&1 < /dev/null) &
cutrun=$!
wait_ready "$root/cut.out"
sshd_down gate
wait "$cutrun"
cases=$((cases + 1))
grep -q '^ready$' "$root/cut.out" || fail 'the machine did not run through the cut' "$(cat "$root/cut.out")"
# Out of reach is 75, at once, and nothing prompts.
as alice 75 'cannot be reached' reserve many --no-wait
as alice 75 'cannot be reached' run --no-wait -f "$root/project/cut.conf"
# A list shows what it can reach, and its status says it is short.
as alice 75 '^mine +1 +0 +0$' pool ls
# The broker still has it as held: what it was never told, it does not give.
mkdir -p "$root/admin/config/hull" "$root/admin/run" "$root/admin/data" "$root/admin/bin"
chmod 700 "$root/admin/run"
printf '(pools (many (broker "%s")))\n' "$broker" > "$root/admin/config/hull/pools.conf"
as admin 0 '^many +60 +1 +0$' pool ls
# One broker gone hides no other: it comes first here, and the list goes on.
cp "$root/admin/config/hull/pools.conf" "$root/admin/pools.kept"
printf '(pools (gone (broker "%s/nobroker")) (many (broker "%s")))\n' "$root" "$broker" \
	> "$root/admin/config/hull/pools.conf"
as admin 75 '^many +60 +1 +0$' pool ls
cp "$root/admin/pools.kept" "$root/admin/config/hull/pools.conf"
sshd_up gate "$port" "$root/authorized"
# Within a settle's interval, the word arrives.
n=0
until run_as admin timeout 20 "$hull" pool ls < /dev/null | grep -q '^many  *60  *0 ' || [ "$n" -ge 50 ]; do
	n=$((n + 1))
	sleep 1
done
as admin 0 '^many +60 +0 +0$' pool ls
cases=$((cases + 1))
[ -z "$(ls -A "$root/alice/run/hull/held" 2> /dev/null)" ] ||
	fail 'a settled lease left its record' "$(ls "$root/alice/run/hull/held")"

# --- a broker that dies keeps what it held ------------------------------------

kept=$(run_as alice timeout 30 "$hull" reserve bench --member gpu < /dev/null)
second=$(run_as bob timeout 30 "$hull" reserve many < /dev/null)
kill -KILL "$broker_pid"
wait "$broker_pid" 2> /dev/null
broker_up
as bob 0 "^$kept +bench +1 +0 +- +alice\$" lease ls
as bob 0 "^$second +many +1 +0 +- +bob\$" lease ls
as bob 125 'no such lease is held' release "$kept"
as alice 0 '' release "$kept"
# What a holder cannot come back for, the broker's own account can end.
as admin 0 '' release "$second"

# --- what a peer can say ------------------------------------------------------

python3 -I - "$broker/core.sock" <<'PY' > "$root/peer.out" 2>&1
import os, socket, struct, sys

def connect():
	s = socket.socket(socket.AF_UNIX)
	d = os.open(os.path.dirname(sys.argv[1]), os.O_PATH)
	s.connect("/proc/self/fd/%d/core.sock" % d)
	s.settimeout(5)
	return s

def send(s, m):
	s.sendall(struct.pack(">I", len(m)) + m)

def reply(s):
	try:
		head = s.recv(4)
		if len(head) < 4:
			return b""
		n = struct.unpack(">I", head)[0]
		data = b""
		while len(data) < n:
			data += s.recv(n - len(data))
		return data
	except OSError:
		return b""

s = connect()
send(s, b"(5:hello(2:ch1:0)(5:proto1:1))")
reply(s)
send(s, b"(5:pause(2:ch1:3)(7:machine1:x))")
print("machine verb:", reply(s))
# Past hello, this connection cannot say who it is.
send(s, b"(8:identify(2:ch1:1)(2:as5:alice))")
print("late identify:", reply(s))
s = connect()
send(s, b"(8:identify(2:ch1:1)(2:as6:a/../b))")
print("bad name:", reply(s))
# Not a frame of the schema: the connection goes, the broker stays.
s = connect()
s.sendall(struct.pack(">I", 5) + b"(1:a)")
print("junk:", reply(s))
s = connect()
s.sendall(struct.pack(">I", 0x7fffffff))
print("huge:", reply(s))
PY
cases=$((cases + 1))
grep -q 'a broker runs no machines' "$root/peer.out" &&
	grep -q 'late identify:.*cannot say who it is' "$root/peer.out" &&
	grep -q 'bad name:.*cannot say who it is' "$root/peer.out" ||
	fail 'a broker took what it should refuse' "$(cat "$root/peer.out")"
cases=$((cases + 1))
kill -0 "$broker_pid" 2> /dev/null || fail 'a broken frame ended the broker' "$(cat "$root/broker.log")"
as alice 0 '^bench +4 +0 +0$' pool ls
# A forced key cannot say it is another: its dial spoke first.
cases=$((cases + 1))
out=$(python3 -I -c '
import struct, sys
m = b"(8:identify(2:ch1:1)(2:as3:bob))"
sys.stdout.buffer.write(struct.pack(">I", len(m)) + m)
' | run_as alice timeout 20 ssh -p "$port" "$account@127.0.0.1" x 2>&1 | strings)
printf '%s\n' "$out" | grep -q 'cannot say who it is' || fail 'a key said it was another holder' "$out"

# A board one holder finds broken is out of the pool for everybody.
printf '#!/bin/sh\nexit 1\n' > "$root/health"
conf sick '(backend hw) (pool "bench") (hostname "sick") (command "/probe" "say" "never")'
as alice 125 'marked broken: its health check failed' run -f "$root/project/sick.conf"
n=0
until run_as bob timeout 20 "$hull" pool ls bench < /dev/null | grep -q '^b1 .*broken' || [ "$n" -ge 40 ]; do
	n=$((n + 1))
	sleep 0.5
done
as bob 0 '^b1 +board +5 +broken +-$' pool ls bench
cases=$((cases + 1))
env XDG_DATA_HOME="$broker" timeout 20 "$hull" board fix b1 ||
	fail 'a mark at the broker could not be taken off' ''
as bob 0 '^b1 +board +5 +free +-$' pool ls bench

# A broker that lies: what it answers goes into a file and a plan here, so a
# name that is no name is refused, and the machine does not run.
mkdir -p "$root/evil"
python3 -I - "$root/evil" <<'PY' &
import os, socket, struct, sys
s = socket.socket(socket.AF_UNIX)
os.chdir(sys.argv[1])
s.bind("core.sock")
s.listen(1)
s.settimeout(20)
c, _ = s.accept()
c.settimeout(20)
def read():
	n = struct.unpack(">I", c.recv(4))[0]
	return c.recv(n)
def send(m):
	c.sendall(struct.pack(">I", len(m)) + m)
read()
send(b"(5:hello(2:ch1:0)(12:core_version1:x)(5:proto1:1))")
read()
send(b"(5:lease(2:ch1:1)(2:id14:lease-3f7792d0)(4:left1:0)(6:member9:../../x\n1))")
PY
evil=$!
n=0
until [ -S "$root/evil/core.sock" ] || [ "$n" -ge 50 ]; do
	n=$((n + 1))
	sleep 0.1
done
conf lied '(backend chroot) (pool "evil") (hostname "lied") (command "/probe" "say" "ran")'
cp "$root/alice/config/hull/pools.conf" "$root/alice/pools.kept"
printf '(pools (evil (broker "%s/evil")))\n' "$root" > "$root/alice/config/hull/pools.conf"
as alice 125 'answered what no lease is' run -f "$root/project/lied.conf"
cp "$root/alice/pools.kept" "$root/alice/config/hull/pools.conf"
wait "$evil" 2> /dev/null

# One holder has no more than a share of what a broker keeps.
cases=$((cases + 1))
held=0
while [ "$held" -lt 20 ]; do
	run_as bob timeout 20 "$hull" reserve many --no-wait > /dev/null 2> "$root/quota.err" < /dev/null || break
	held=$((held + 1))
done
[ "$held" = 16 ] && grep -q 'bob holds or waits for as much as one holder may' "$root/quota.err" ||
	fail "a holder was given $held leases" "$(cat "$root/quota.err")"
as alice 0 'lease-' reserve many --no-wait

printf 'broker: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
