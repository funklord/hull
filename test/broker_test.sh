#!/bin/sh
# A pool's broker (project.md section 4.12), between two users on a real
# sshd: its keys forced to `hull broker-dial --as`, known_hosts strict, so
# what crosses is what a network would carry. Each user is a home, a
# runtime directory and a core of its own. One account plays both, which
# is what --as is for. What the kernel says of a second account is the last
# part: two real accounts, in a user namespace over /etc/subuid's range.
# Bounded by each command's timeout and the sshd's and broker's pids, which
# the trap ends.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/broker
# A run killed part-way leaves what only the namespace's root can remove.
rm -rf "${dir:?}" 2> /dev/null || unshare --map-auto --map-root-user rm -rf "${dir:?}"
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
chmod 750 "$broker"
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
chmod 640 "$broker/pools.conf"
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
as alice 0 '^gpu +slot +- +by-name +free +-$' pool ls bench
lease=$(run_as alice timeout 30 "$hull" reserve bench --member gpu < /dev/null)
as bob 0 "^gpu +slot +- +by-name +leased +$lease\$" pool ls bench
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
as bob 0 '^gpu +slot +- +by-name +free +-$' pool ls bench

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
as bob 0 '^b1 +board +- +5 +leased ' pool ls bench
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

# And to one a core that died kept: its end is in what the next core reads.
ending=$(run_as alice timeout 30 "$hull" reserve many --for 10s < /dev/null)
conf keptend '(backend chroot) (pool "many") (hostname "keptend") (on_disconnect keep)' \
	'(command "/probe" "say" "ready" "sleep" "60")'
: > "$root/keptend.out"
(run_as alice timeout -k 2 80 "$hull" run --lease "$ending" -f "$root/project/keptend.conf" > "$root/keptend.out" 2>&1 < /dev/null) &
keptend=$!
wait_ready "$root/keptend.out"
kill -KILL "$(head -1 "$root/alice/run/hull/core.lock")" 2> /dev/null
wait "$keptend" 2> /dev/null
as alice 0 '^keptend .*running$' ps
n=0
until ! run_as alice timeout 20 "$hull" ps < /dev/null | grep -q '^keptend .*running$' || [ "$n" -ge 30 ]; do
	n=$((n + 1))
	sleep 1
done
not_running=$(run_as alice timeout 20 "$hull" ps < /dev/null | grep -c '^keptend .*running$')
cases=$((cases + 1))
[ "$not_running" = 0 ] || {
	fail 'a kept machine ran past its broker lease' "$(run_as alice timeout 20 "$hull" ps 2>&1 < /dev/null)"
	run_as alice timeout 10 "$hull" stop --timeout 1 keptend > /dev/null 2>&1
}

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
as bob 0 '^b1 +board +- +5 +broken +-$' pool ls bench
cases=$((cases + 1))
env XDG_DATA_HOME="$broker" timeout 20 "$hull" board fix b1 ||
	fail 'a mark at the broker could not be taken off' ''
as bob 0 '^b1 +board +- +5 +free +-$' pool ls bench

# A grant nobody hears goes back: told, and gone or silent, a peer holds nothing.
# What is held stays, gone or not, and a lease is not another's to hold.
cat > "$root/unheld.py" <<'PY'
import os, socket, struct, sys, time
def connect():
	s = socket.socket(socket.AF_UNIX)
	d = os.open(sys.argv[1], os.O_PATH)
	s.connect("/proc/self/fd/%d/core.sock" % d)
	s.settimeout(10)
	return s
def send(s, m):
	s.sendall(struct.pack(">I", len(m)) + m)
def reply(s):
	n = struct.unpack(">I", s.recv(4))[0]
	data = b""
	while len(data) < n:
		data += s.recv(n - len(data))
	return data
def ask(s):
	send(s, b"(5:hello(2:ch1:0)(5:proto1:1))")
	reply(s)
	send(s, b"(5:lease(2:ch1:1)(5:count1:1)(4:cpus1:0)(3:for1:0)(4:kind0:)(7:members3:gpu)"
	        b"(6:memory1:0)(3:run1:0)(4:pool5:bench))")
	return reply(s)
def word(answer):
	return answer.split(b"(2:id14:")[1][:14]
held = lambda s, w: (send(s, b"(4:hold(2:ch1:1)(2:id14:" + w + b"))"), reply(s))[1]
mode = sys.argv[2]
s = connect()
w = word(ask(s))
print(w.decode())
if mode == "gone":
	s.close()
elif mode == "other":
	t = connect()
	send(t, b"(5:hello(2:ch1:0)(5:proto1:1))")
	reply(t)
	print(held(t, w))
	s.close()
elif mode == "silent":
	sys.stdout.flush()
	time.sleep(float(sys.argv[3]))
elif mode == "held":
	print(held(s, w))
	s.close()
PY
free_gpu() {
	n=0
	until run_as alice timeout 20 "$hull" pool ls bench < /dev/null | grep -q '^gpu .*free' || [ "$n" -ge "$1" ]; do
		n=$((n + 1))
		sleep 0.25
	done
}
cases=$((cases + 1))
python3 -I "$root/unheld.py" "$broker" gone > "$root/unheld.out" 2>&1
free_gpu 40
[ "$n" -lt 40 ] || fail 'a grant to one gone was kept' "$(cat "$root/unheld.out" "$root/broker.log" | tail -5)"
cases=$((cases + 1))
python3 -I "$root/unheld.py" "$broker" other > "$root/unheld.out" 2>&1
grep -q 'no such lease is held' "$root/unheld.out" || fail 'one connection held what another was told' "$(cat "$root/unheld.out")"
free_gpu 40
# Silent on a wire that stays up, as one cut and not yet known to be is.
cases=$((cases + 1))
# It outlives the hold by far: what ends it here must be the hold's own time,
# and not its connection closing.
python3 -I "$root/unheld.py" "$broker" silent 300 > "$root/unheld.out" 2>&1 &
silent=$!
sleep 1
as bob 75 'gpu is held' reserve bench --member gpu --no-wait
free_gpu 170
[ "$n" -lt 170 ] || fail 'a grant nobody held did not go back' "$(tail -5 "$root/broker.log")"
kill "$silent" 2> /dev/null
wait "$silent" 2> /dev/null
cases=$((cases + 1))
python3 -I "$root/unheld.py" "$broker" held > "$root/unheld.out" 2>&1
heldid=$(head -1 "$root/unheld.out")
as bob 0 "^gpu +slot +- +by-name +leased +$heldid\$" pool ls bench
kill -KILL "$broker_pid"
wait "$broker_pid" 2> /dev/null
broker_up
as bob 0 "^gpu +slot +- +by-name +leased +$heldid\$" pool ls bench
run_as admin timeout 20 "$hull" release "$heldid" > /dev/null 2>&1 < /dev/null
# Not held, a broker that dies has given nothing: it was on no disk.
python3 -I "$root/unheld.py" "$broker" silent 5 > "$root/unheld.out" 2>&1 &
silent=$!
sleep 1
kill -KILL "$broker_pid"
wait "$broker_pid" 2> /dev/null
broker_up
as bob 0 '^gpu +slot +- +by-name +free +-$' pool ls bench
kill "$silent" 2> /dev/null
wait "$silent" 2> /dev/null

# A full line leaves a way in: what frees an entry has to be able to say so,
# and one holder has a share of the connections, as of the leases.
cat > "$root/line.py" <<'PY'
import os, socket, struct, sys, time
def connect():
	s = socket.socket(socket.AF_UNIX)
	d = os.open(sys.argv[1], os.O_PATH)
	s.connect("/proc/self/fd/%d/core.sock" % d)
	os.close(d)
	s.settimeout(10)
	return s
def send(s, m):
	s.sendall(struct.pack(">I", len(m)) + m)
def reply(s):
	h = s.recv(4)
	if len(h) < 4:
		return b""
	n = struct.unpack(">I", h)[0]
	data = b""
	while len(data) < n:
		data += s.recv(n - len(data))
	return data
def who(name):
	s = connect()
	send(s, b"(8:identify(2:ch1:1)(2:as%d:%s))" % (len(name), name))
	send(s, b"(5:hello(2:ch1:0)(5:proto1:1))")
	return s, reply(s)
ask = (b"(5:lease(2:ch1:1)(5:count1:1)(4:cpus1:0)(3:for1:0)(4:kind0:)(7:members3:gpu)"
       b"(6:memory1:0)(3:run1:0)(4:pool5:bench)(4:wait3:600))")
s, _ = who(b"first")
send(s, ask)
w = reply(s).split(b"(2:id14:")[1][:14]
send(s, b"(4:hold(2:ch1:1)(2:id14:" + w + b"))")
reply(s)
s.close()
line = []
for i in range(64):
	c, _ = who(b"job%d" % (i % 4))
	send(c, ask)
	line.append(c)
	time.sleep(0.01)
time.sleep(0.5)
s, _ = who(b"first")
send(s, b"(4:free(2:ch1:1)(2:id14:" + w + b"))")
print("release:", reply(s)[:7])
told = reply(line[0])
while told.startswith(b"(4:data"):
	told = reply(line[0])
print("next:", told[:7])
for c in line:
	c.close()
held = []
for i in range(80):
	c, r = who(b"greedy")
	if b"hello" not in r:
		print("refused at", i, r[:90])
		break
	held.append(c)
s, r = who(b"other")
print("other:", r[:7], "held", len(held))
quiet = connect()
quiet.settimeout(25)
at = time.time()
print("silent closed:", quiet.recv(10) == b"", round(time.time() - at))
PY
cases=$((cases + 1))
timeout 60 python3 -I "$root/line.py" "$broker" > "$root/line.out" 2>&1
grep -q "release: b'.*(4:done" "$root/line.out" && grep -q "next: b'.*(5:lea" "$root/line.out" ||
	fail 'a full line kept out what ends it' "$(cat "$root/line.out")"
cases=$((cases + 1))
grep -q 'refused at 64 .*as many connections' "$root/line.out" && grep -q "other: b'.*(5:hel.* held 64" "$root/line.out" ||
	fail 'one holder took more than its share of the connections' "$(cat "$root/line.out")"
cases=$((cases + 1))
grep -Eq 'silent closed: True 1[3-7]$' "$root/line.out" || fail 'a silent connection stayed' "$(cat "$root/line.out")"
run_as admin timeout 20 "$hull" lease ls < /dev/null 2> /dev/null | awk '/^lease-/ { print $1 }' > "$root/line.left"
while read -r id; do
	run_as admin timeout 20 "$hull" release "$id" > /dev/null 2>&1 < /dev/null
done < "$root/line.left"

# Out of descriptors, a broker waits; it does not spin. And a core takes what
# its hard limit gives it, where a session's soft limit would be too few.
mkdir -p "$root/fds"
chmod 750 "$root/fds"
printf '(pools (many (capacity 8)))\n' > "$root/fds/pools.conf"
chmod 640 "$root/fds/pools.conf"
python3 -I - "$hull" "$root/fds" <<'PY' > "$root/fds.out" 2>&1
import os, socket, subprocess, sys, time
hull, d = sys.argv[1], sys.argv[2]
env = dict(os.environ, XDG_RUNTIME_DIR=d, HOME=d)
def start(limit):
	b = subprocess.Popen(["sh", "-c", limit + '; exec "$0" broker "$1"', hull, d], env=env,
	                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	for _ in range(100):
		if os.path.exists(d + "/core.sock"):
			break
		time.sleep(0.1)
	return b
def stop(b):
	b.terminate()
	b.wait(10)
b = start("ulimit -n 20")
held = []
for i in range(25):
	s = socket.socket(socket.AF_UNIX)
	a = os.open(d, os.O_PATH)
	s.connect("/proc/self/fd/%d/core.sock" % a)
	os.close(a)
	held.append(s)
	time.sleep(0.02)
ticks = lambda: sum(int(x) for x in open("/proc/%d/stat" % b.pid).read().rsplit(")", 1)[1].split()[11:13])
at = ticks()
time.sleep(3)
print("ticks", ticks() - at)
stop(b)
for s in held:
	s.close()
b = start("ulimit -Sn 30")
for line in open("/proc/%d/limits" % b.pid):
	if line.startswith("Max open files"):
		print("soft", line.split()[3])
stop(b)
PY
cases=$((cases + 1))
ticks=$(sed -n 's/^ticks //p' "$root/fds.out")
[ -n "$ticks" ] && [ "$ticks" -lt 60 ] || fail 'a broker with no descriptors left spun' "$(cat "$root/fds.out")"
cases=$((cases + 1))
soft=$(sed -n 's/^soft //p' "$root/fds.out")
[ -n "$soft" ] && [ "$soft" -gt 30 ] || fail 'a core kept a soft limit too low for its tables' "$(cat "$root/fds.out")"

# What a core owes is in its held directory. With that gone there is nothing
# it could settle, and it goes when idle, as any core: one stayed four hours.
conf owed '(backend chroot) (pool "many") (hostname "owed") (command "/probe" "say" "ready" "sleep" "2")'
: > "$root/owed.out"
(run_as bob timeout -k 2 40 "$hull" run -f "$root/project/owed.conf" > "$root/owed.out" 2>&1 < /dev/null) &
owed=$!
wait_ready "$root/owed.out"
owes=$(head -1 "$root/bob/run/hull/core.lock")
sshd_down gate
wait "$owed"
sleep 2
rm -rf "${root:?}/bob/run/hull/held"
n=0
while [ -r "/proc/$owes/cmdline" ] && [ -n "$(tr -d '\0' < "/proc/$owes/cmdline" 2> /dev/null)" ] && [ "$n" -lt 40 ]; do
	n=$((n + 1))
	sleep 0.25
done
cases=$((cases + 1))
[ "$n" -lt 40 ] || {
	fail 'a core stayed for what it had no record of' "$(ps -o pid,etime,args -p "$owes")"
	kill "$owes" 2> /dev/null
}
sshd_up gate "$port" "$root/authorized"
run_as admin timeout 20 "$hull" lease ls < /dev/null 2> /dev/null | awk '/^lease-/ { print $1 }' > "$root/owed.left"
while read -r id; do
	run_as admin timeout 20 "$hull" release "$id" > /dev/null 2>&1 < /dev/null
done < "$root/owed.left"

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

# --- two real accounts: what the kernel says ----------------------------------

# Everything above is one account playing both, which is what --as is for.
# A user namespace over /etc/subuid's range holds uids the kernel tells apart,
# so the gate itself is tried here, and only where that range is given.
accounts=$root/accounts
mkdir -p "$accounts"
# The tree may be setgid to a group the namespace does not map, and root there
# cannot chown what carries a gid it cannot name.
chgrp "$(id -g)" "$accounts"
chmod g-s "$accounts"
chmod 755 "$root" "$accounts"
cat > "$root/accounts.sh" <<'INNER'
hull=$1
a=$2
b=$a/b
n=0
said() { # NAME: what was run went as it should, or it did not
	n=$((n + 1))
	printf 'FAIL %s\n' "$1"
}
user() { # UID GROUPS COMMAND...
	u=$1
	g=$2
	shift 2
	mkdir -p "$a/$u/run"
	chown -R "$u:$u" "$a/$u"
	chmod 700 "$a/$u/run"
	setpriv --reuid "$u" --regid "$u" --groups "$g" env -i PATH="$PATH" HOME="$a/$u" ASAN_OPTIONS="${ASAN_OPTIONS:-}" \
		XDG_RUNTIME_DIR="$a/$u/run" XDG_CONFIG_HOME="$a/$u/config" XDG_DATA_HOME="$a/$u/data" \
		HULL_CORE_IDLE=1 DOCKER_HOST="unix://$a/none.sock" timeout 20 "$@"
}
for u in 3001 3002 3003 3004; do
	mkdir -p "$a/$u/config/hull"
	printf '(pools (bench (broker "%s")))\n' "$b" > "$a/$u/config/hull/pools.conf"
done
mkdir -p "$b"
printf '(pools (bench (capacity 3)))\n' > "$b/pools.conf"
chown -R 3001:3000 "$b"

# A directory its group can write is its leases and pools that group's.
chmod 770 "$b"
out=$(user 3001 3001,3000 "$hull" broker "$b" 2>&1)
[ $? != 0 ] && printf '%s\n' "$out" | grep -q 'writable by others' || said "a group-writable directory was taken: $out"
chmod 750 "$b"
chmod 664 "$b/pools.conf"
out=$(user 3001 3001,3000 "$hull" broker "$b" 2>&1)
[ $? != 0 ] && printf '%s\n' "$out" | grep -q 'writable by others' || said "a group-writable pools.conf was taken: $out"
chmod 640 "$b/pools.conf"

# The broker's primary group is not the directory's, nor is it setgid.
user 3001 3001,3000 "$hull" broker "$b" > "$a/log" 2>&1 &
i=0
until [ -S "$b/core.sock" ] || [ "$i" -ge 50 ]; do
	i=$((i + 1))
	sleep 0.1
done
[ "$(stat -c '%a %g' "$b/core.sock")" = '660 3000' ] || said "the socket is not the group's: $(stat -c '%a %g' "$b/core.sock")"

alice() { user 3002 3002,3000 "$@"; }
carol() { user 3004 3004,3000 "$@"; }
bob() { user 3003 3003 "$@"; }

lease=$(alice "$hull" reserve bench --no-wait 2>&1) || said "a member was refused: $lease"
case $lease in lease-*) ;; *) said "no lease: $lease" ;; esac
# Each holder is its account, whoever says what.
out=$(carol "$hull" lease ls 2>&1)
printf '%s\n' "$out" | grep -Eq "^$lease +bench .* uid-3002\$" || said "the holder is not the account: $out"
out=$(carol "$hull" release "$lease" 2>&1)
[ $? = 125 ] || said "a member ended another's lease: $out"
# Outside the group, the kernel says no before hull is asked anything.
out=$(bob "$hull" reserve bench --no-wait 2>&1)
[ $? = 75 ] || said "one outside the group got: $out"
out=$(bob sh -c "cat '$b/core.lock'" 2>&1) && said "one outside the group read the directory: $out"
# A member is a member: it cannot say it is another, or write what the broker keeps.
out=$(alice python3 -I -c '
import os, socket, struct, sys
s = socket.socket(socket.AF_UNIX)
s.settimeout(5)
s.connect(sys.argv[1])
m = b"(8:identify(2:ch1:1)(2:as5:carol))"
s.sendall(struct.pack(">I", len(m)) + m)
print(s.recv(200))
' "$b/core.sock" 2>&1)
printf '%s\n' "$out" | grep -q 'cannot say who it is' || said "a member said it was another: $out"
alice sh -c "echo > '$b/pools.conf'" 2> /dev/null && said 'a member wrote the pools'
alice touch "$b/leases/x" 2> /dev/null && said 'a member wrote a lease'
alice rm -f "$b/leases/$lease" 2> /dev/null
[ -e "$b/leases/$lease" ] || said 'a member removed a lease'
# The broker's own account ends what its holder cannot come back for.
out=$(user 3001 3001,3000 "$hull" release "$lease" 2>&1)
[ $? = 0 ] || said "the broker's account could not end a lease: $out"
pkill_pid=$(head -1 "$b/core.lock")
kill "$pkill_pid" 2> /dev/null
wait
rm -rf "${b:?}" "${a:?}"/3* "${a:?}/log"
[ -z "$(ls -A "$a")" ] || said "accounts left: $(ls "$a")"
printf 'done %s\n' "$n"
INNER
if grep -q "^$account:" /etc/subuid 2> /dev/null && command -v setpriv > /dev/null &&
   unshare --map-auto --map-root-user true 2> /dev/null; then
	cases=$((cases + 1))
	out=$(timeout -k 2 120 unshare --map-auto --map-root-user sh "$root/accounts.sh" "$hull" "$accounts" 2>&1)
	printf '%s\n' "$out" | grep -q '^done 0$' || fail 'two accounts' "$out"
else
	printf 'broker: no subuid range here, two real accounts not tried\n'
fi

printf 'broker: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
