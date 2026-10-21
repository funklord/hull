#!/bin/sh
# qemu_test.sh -- build/hull run on the qemu engine, as the user, booting
# the host's own kernel. Not part of `make check`, which needs no KVM:
# `make check-qemu` runs it.
#
# Each machine is booted once and every line it prints is checked, since
# a boot is two seconds; the emu case, under TCG, is about twenty. Every
# command runs under `timeout`, and the one loop, waiting for a machine
# to run, gives up after 40 tries. The files it writes are named in
# FILES, in $BUILD_DIR/test/qemu. The last cases require that no qemu of
# this suite's, no passt and none of its machine directories are left.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$2
dir=${BUILD_DIR:?}/test/qemu
# A core of the test's own, as in cli_test.sh.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR" "$dir/rootfs/proc" "$dir/rootfs/dev" "$dir/rootfs/sys" \
	"$dir/rootfs/srv" "$dir/rootfs/etc" "$dir/data"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$probe" "$dir/rootfs/probe"
# A resolver no machine can reach, which the agent's must cover.
printf 'nameserver 127.0.0.53\n' > "$dir/rootfs/etc/resolv.conf"
root=$(cd "$dir" && pwd)
# The engine's passt, by the arguments only it gives.
passt='^passt --quiet --fd 3 --one-off --ipv4-only --address 10\.0\.2\.15 '
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

conf() { # NAME ENTRIES...
	name=$1
	shift
	printf '(machine (source "dir:./rootfs") %s)\n' "$*" > "$root/$name.conf"
}

# boot NAME [SECONDS]: run it once, from /, as the files' paths are their
# own; its output is in $out and its status in $got.
boot() {
	out=$(cd / && timeout "${2:-60}" "$hull" run -f "$root/$1.conf" 2>&1)
	got=$?
}

# has STATUS PATTERN: the last boot exited STATUS and printed a line
# matching PATTERN.
has() {
	cases=$((cases + 1))
	if [ "$got" != "$1" ]; then
		fail "$name exited $got, wanted $1" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$2"; then
		fail "$name: no line matches /$2/" "$out"
	fi
}

# address_of ID: section 8.1's address, worked here rather than taken
# from the core: FNV-1a of the id, modulo 256 * 254, as x * 254 + y - 1.
address_of() {
	python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("127.107.%d.%d" % (h // 254, h % 254 + 1))' "$1"
}

# await ID: until the machine ID is listed running, 40 tries at most.
await() { # ID
	tries=0
	until "$hull" ps 2>/dev/null | grep -q "^$1 .*running\$" || [ "$tries" -ge 40 ]; do
		tries=$((tries + 1))
		sleep 0.5
	done
}

if [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'qemu: /dev/kvm or /dev/vhost-vsock is not open to this user\n'
	exit 1
fi
printf 'from the host\n' > "$root/data/in"
rm -f "$root/data/out" "$root/rootfs/written"

# Output, status, and what the machine looks like from inside, all
# applied by the agent from the spec.
conf run '(backend vm) (hostname "qtest") (workdir "/srv") (env (GREETING "hi")) (memory "256M") (cpus 2)' \
	'(command "/probe" "say" "out" "err" "to stderr" "env" "GREETING" "cwd" "host" "uid" "nnp"' \
	'"session" "read" "/sys/devices/system/cpu/online" "read" "/proc/meminfo"' \
	'"read" "/sys/devices/system/clocksource/clocksource0/available_clocksource" "exit" "3")'
name=run
boot run
has 3 '^out$'
has 3 '^to stderr$'
has 3 '^GREETING=hi$'
has 3 '^cwd=/srv$'
has 3 '^host=qtest$'
has 3 '^uid=0$'
has 3 '^nnp=1$'
has 3 '^leader=1$'
has 3 '^read=0-1$'
# 256 MiB, less what the kernel keeps for itself.
has 3 '^read=MemTotal: +(1[5-9][0-9]|2[0-6][0-9])[0-9]{3} kB$'
has 3 'vm\.kernel\.bundled: emulated: the host.s own kernel'
# Accelerated: KVM gives the guest its clock.
has 3 '^read=kvm-clock'
cases=$((cases + 1))
if (cd / && timeout 60 "$hull" run -f "$root/run.conf" 2>/dev/null) | grep -q '^to stderr$'; then
	fail "stderr reached stdout" ""
fi

# How a workload ends, exactly: a signal, and a command not there.
conf die '(backend vm) (command "/probe" "die")'
name=die
boot die
has 143 ''
conf nope '(backend vm) (command "/nope")'
name=nope
boot nope
has 127 '/nope: No such file or directory'

# The directory in place, as the user's, and read-only when asked.
conf write '(backend vm) (command "/probe" "write" "/written")'
name=write
boot write
has 0 '^write=ok$'
cases=$((cases + 1))
[ -f "$root/rootfs/written" ] && [ "$(stat -c %u "$root/rootfs/written")" = "$(id -u)" ] ||
	fail "a write in the machine did not land in its directory as the user's" \
		"$(ls -ln "$root/rootfs" 2>&1)"
conf readonly '(backend vm) (security (readonly_root true))' \
	'(command "/probe" "write" "/again" "write" "/etc/resolv.conf")'
name=readonly
boot readonly
has 0 '^write=Read-only file system$'
# Its resolv.conf, the agent's, read-only too.
cases=$((cases + 1))
[ "$(printf '%s\n' "$out" | grep -c '^write=Read-only file system$')" = 2 ] ||
	fail "a read-only machine's resolv.conf could be written" "$out"

# An overlay: the directory read-only beneath, what the machine writes
# held in its memory above. / keeps the directory's owner and mode, as
# in place, setgid included, so a file the machine's root writes there
# takes the directory's group when / has the bit. A file written is the
# machine's and not the directory's, and one removed is gone inside and
# still on the host. The upper layer is a tmpfs of half the machine's
# memory, so 200 MiB into 256 stops short.
printf 'kept\n' > "$root/rootfs/srv/kept"
conf overlay '(backend vm) (mutable overlay) (memory "256M")' \
	'(command "/probe" "owner" "/" "write" "/layered" "owner" "/layered"' \
	'"remove" "/srv/kept" "read" "/srv/kept" "fill" "/big" "200")'
name=overlay
boot overlay
has 0 'source\.dir\.overlay: emulated: an overlayfs in the guest'
top=$(stat -c %u:%g "$root/rootfs")
group=0
[ -g "$root/rootfs" ] && group=$(stat -c %g "$root/rootfs")
has 0 "^owner=$top $(printf '%04d' "$(stat -c %a "$root/rootfs")")\$"
has 0 '^write=ok$'
has 0 "^owner=0:$group 0644\$"
has 0 '^remove=ok$'
has 0 '^read=No such file or directory$'
has 0 '^filled=(9[0-9]|1[01][0-9]|12[0-7])$'
cases=$((cases + 1))
[ ! -e "$root/rootfs/layered" ] && [ ! -e "$root/rootfs/big" ] &&
	[ "$(cat "$root/rootfs/srv/kept" 2>&1)" = kept ] ||
	fail "an overlay's writes reached the directory" "$(ls -l "$root/rootfs" "$root/rootfs/srv" 2>&1)"
# Read-only as well, it is the share itself, read-only.
conf overro '(backend vm) (mutable overlay) (security (readonly_root true))' \
	'(command "/probe" "write" "/again")'
name=overro
boot overro
has 0 '^write=Read-only file system$'
rm -f "$root/rootfs/srv/kept"

# Volumes, each a share of its own, read-write and read-only.
conf vol '(backend vm) (volumes (volume (host "./data") (guest "/data")))' \
	'(command "/probe" "read" "/data/in" "write" "/data/out")'
name=vol
boot vol
has 0 '^read=from the host$'
has 0 '^write=ok$'
cases=$((cases + 1))
[ -f "$root/data/out" ] || fail "a write through a volume did not reach the host" ""
conf volro '(backend vm) (volumes (volume (host "./data") (guest "/data") (mode ro)))' \
	'(command "/probe" "write" "/data/again")'
name=volro
boot volro
has 0 '^write=Read-only file system$'

# The network, out, through passt as the user: the machine's own lo, the
# host's DNS, and the host's address; but not the host's loopback. Two
# listeners on the host, each taking one connection within 15 seconds,
# say their ports first and then who came.
hostaddr=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
[ -n "$hostaddr" ] || hostaddr=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -m1 -E '^[0-9.]+$')
rm -f "$root/listen.out"
timeout 20 python3 -c '
import socket, sys, threading
def listen(address):
    s = socket.socket()
    s.bind((address, 0))
    s.listen()
    s.settimeout(15)
    return s
def take(s, greeting, name):
    try:
        c, _ = s.accept()
        c.sendall(greeting)
        c.close()
        print(name, "came", flush=True)
    except OSError:
        pass
outside = listen(sys.argv[1] or "127.0.0.1")
inside = listen("127.0.0.1")
print(outside.getsockname()[1], inside.getsockname()[1], flush=True)
threads = [threading.Thread(target=take, args=(outside, b"hello from the host\n", "host")),
           threading.Thread(target=take, args=(inside, b"the host loopback\n", "loopback"))]
[t.start() for t in threads]
[t.join() for t in threads]' "$hostaddr" > "$root/listen.out" 2>&1 &
listener=$!
tries=0
until [ -s "$root/listen.out" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
read -r outside inside < "$root/listen.out"
conf net '(backend vm) (command "/probe" "read" "/etc/resolv.conf" "dial" "127.0.0.1" "9"' \
	"\"dial\" \"10.0.2.2\" \"$inside\" \"dial\" \"${hostaddr:-127.0.0.1}\" \"$outside\"" \
	'"resolve" "10.0.2.3" "hull-test.invalid")'
name=net
boot net
has 0 '^read=nameserver 10\.0\.2\.3$'
# lo is up: nothing listens, where with lo down the dial times out.
has 0 '^dial 127\.0\.0\.1:9=Connection refused$'
# Any answer: the host's resolver need not reach anything further.
has 0 '^resolve=rcode [0-9]+ answers [0-9]+$'
if [ -n "$hostaddr" ]; then
	has 0 "^dial ${hostaddr}:${outside}=hello from the host\$"
else
	printf 'qemu: the host has no address but loopback; the dial out is not tried\n'
fi
cases=$((cases + 1))
if printf '%s\n' "$out" | grep -q '=the host loopback$'; then
	fail "a machine reached the host's loopback through its gateway" "$out"
fi
# The machine has ended, so nothing more can come.
kill "$listener" 2>/dev/null
wait "$listener" 2>/dev/null
cases=$((cases + 1))
if grep -q '^loopback came' "$root/listen.out"; then
	fail "the host's loopback listener was reached" "$(cat "$root/listen.out")"
fi

# Ports, from the machine's address, each connection an agent channel to the
# machine's own address. Six free ports; the machine serves five in
# turn, and nothing holds the fourth's guest port. The client takes them
# in the same order and tries each again until the probe is listening,
# since a connection the agent is refused reaches the client as a close.
# The fifth's client goes after 100 kB of 20 MB, which the machine must
# hear; the sixth's holds its connection unread while the machine is
# stopped, which must end it.
at=$(address_of qports)
set -- $(python3 -c '
import socket, sys
held = [socket.socket() for _ in range(6)]
for s in held:
    s.bind((sys.argv[1], 0))
print(*[s.getsockname()[1] for s in held])' "$at")
greet=$1 pour=$2 drink=$3 nobody=$4 abort=$5 hold=$6
conf ports "(backend vm) (hostname \"qports\")" \
	"(ports (port (host $greet) (guest 80)) (port (host $pour) (guest 81))" \
	"(port (host $drink) (guest 82)) (port (host $nobody) (guest 9))" \
	"(port (host $abort) (guest 83)) (port (host $hold) (guest 84)))" \
	'(command "/probe" "serve" "tcp" "80" "pour" "81" "3000000" "drink" "82"' \
	'"pour" "83" "20000000" "pour" "84" "20000000")'
rm -f "$root/client.out"
# A machine of its own keeps the core running, so that what the ports
# machine leaves when it ends is not ended by the core's own exit.
conf keep '(backend vm) (hostname "qkeep") (command "/probe" "sleep" "80")'
(cd / && exec timeout -k 2 90 "$hull" run -f "$root/keep.conf" > /dev/null 2>&1) &
keeper=$!
await qkeep
(cd / && exec timeout 60 "$hull" run -f "$root/ports.conf" > "$root/ports.out" 2>&1) &
runner=$!
timeout 50 python3 -c '
import socket, sys, time
greet, pour, drink, nobody, abort, hold = map(int, sys.argv[1:7])
at = sys.argv[7]
def fnv(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xffffffff
    return "bytes=%d fnv=%08x" % (len(data), h)
def until_answered(port, talk, window=0):
    deadline = time.time() + 30
    while time.time() < deadline:
        try:
            s = socket.socket()
            if window:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, window)
            s.settimeout(10)
            s.connect((at, port))
            s.settimeout(10)
            got = talk(s)
            s.close()
            if got:
                return got
        except OSError:
            pass
        time.sleep(0.2)
    return "never answered"
def everything(s):
    data = b""
    while True:
        piece = s.recv(65536)
        if not piece:
            return data
        data += piece
def greeting(s):
    line = s.recv(100)
    if not line:
        return None
    s.sendall(b"hi from the host\n")
    return "greeted=" + line.decode().strip() + (" then closed" if not everything(s) else " then more")
def poured(s):
    time.sleep(1)
    data = everything(s)
    return data and "poured " + fnv(data)
sent = bytes((i * 7 + i // 251) & 0xff for i in range(3000000))
def drunk(s):
    try:
        s.sendall(sent)
        s.shutdown(socket.SHUT_WR)
    except OSError:
        return None
    reply = everything(s).decode().strip()
    return reply and "drunk " + reply + (" whole" if reply == fnv(sent) else " NOT " + fnv(sent))
print(until_answered(greet, greeting), flush=True)
s = socket.create_connection((at, nobody), timeout=10)
s.settimeout(10)
print("nobody=" + ("closed" if not everything(s) else "answered"), flush=True)
print(until_answered(pour, poured), flush=True)
print(until_answered(drink, drunk), flush=True)
def leave(s):
    got = 0
    while got < 100000:
        piece = s.recv(65536)
        if not piece:
            return None
        got += len(piece)
    return "left"
print(until_answered(abort, leave), flush=True)
def held(s):
    if not s.recv(1):
        return None
    print("holding", flush=True)
    time.sleep(16)
    s.settimeout(15)
    try:
        everything(s)
        return "held then ended"
    except socket.timeout:
        return "held and still open"
    except OSError:
        return "held then ended"
# A small window, so that the engine is soon blocked writing to it.
print(until_answered(hold, held, 4096), flush=True)
' "$greet" "$pour" "$drink" "$nobody" "$abort" "$hold" "$at" > "$root/client.out" 2>&1 &
client=$!
tries=0
until grep -q '^holding$' "$root/client.out" 2>/dev/null || [ "$tries" -ge 160 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
# Time for what the machine pours to fill all there is to hold it.
sleep 4
timeout 30 "$hull" stop qports > /dev/null 2>&1
# Its listeners close once the core has destroyed it, which a client
# holding a connection unread must not prevent: looked at before that
# client reads again, since its reading would free what it held.
cases=$((cases + 1))
tries=0
while python3 -c '
import socket, sys
socket.create_connection((sys.argv[2], int(sys.argv[1])), timeout=2)' "$greet" "$at" 2>/dev/null &&
	[ "$tries" -lt 16 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
[ "$tries" -lt 16 ] || fail "a client holding a connection unread held its stopped machine" ""
wait "$client"
wait "$runner"
got=$?
out=$(cat "$root/ports.out" "$root/client.out")
name=ports
has 143 '^greeted=hello from the machine then closed$'
has 143 '^served=hi from the host$'
has 143 '^nobody=closed$'
want=$("$probe" spew 3000000 | "$probe" count)
has 143 "^poured $want\$"
has 143 '^drunk bytes=3000000 fnv=[0-9a-f]{8} whole$'
has 143 '^drank bytes=3000000 '
# The client that left: the machine's writes then fail, short of 20 MB.
has 143 '^left$'
has 143 '^poured=(Broken pipe|Connection reset by peer)$'
has 143 '^held then ended$'
has 143 'ports: emulated: tcp and udp, forwarded over vsock'
# The machine gone, its listeners are closed, while the core runs on.
cases=$((cases + 1))
if python3 -c '
import socket, sys
socket.create_connection((sys.argv[2], int(sys.argv[1])), timeout=5)' "$greet" "$at" 2>/dev/null; then
	fail "a port was still listened on after its machine ended" ""
fi
timeout 30 "$hull" stop qkeep > /dev/null 2>&1
wait "$keeper"
# A port taken on another address leaves the machine its own; taken on
# the machine's address, it refuses the start.
hold_port() { # ADDRESS PORT
	timeout 30 python3 -c '
import socket, sys, time
s = socket.socket()
# The port may be in TIME_WAIT from the case above.
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind((sys.argv[1], int(sys.argv[2])))
s.listen()
time.sleep(25)' "$1" "$2" &
	holder=$!
	sleep 0.5
}
conf taken "(backend vm) (hostname \"qtaken\") (ports (port (host $greet) (guest 80)))" \
	'(command "/probe" "say" "ran")'
at=$(address_of qtaken)
hold_port 127.0.0.1 "$greet"
name=taken
boot taken
has 0 '^ran$'
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null
hold_port "$at" "$greet"
boot taken
has 125 "port $greet on $(printf '%s' "$at" | sed 's/\./\\./g'): Address already in use"
kill "$holder" 2>/dev/null
wait "$holder" 2>/dev/null

# hull proxy-ssh: the machine's own port 22, declared nowhere, reached as
# ssh's ProxyCommand would, by name with or without .hull. Nothing on a
# port is a refusal, 125 with its reason: dialled in the three seconds
# before the probe listens.
conf dial '(backend vm) (hostname "qdial") (command "/probe" "say" "up" "sleep" "3" "serve" "tcp" "22")'
cases=$((cases + 1))
rm -f "$root/dial.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/dial.conf" > "$root/dial.out" 2>&1) &
runner=$!
tries=0
until grep -q '^up$' "$root/dial.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
refused=$(cd / && timeout 20 "$hull" proxy-ssh qdial < /dev/null 2>&1)
refused_got=$?
tries=0
until grep -q '^serving$' "$root/dial.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
heard=$(printf 'hi from ssh\n' | (cd / && timeout 20 "$hull" proxy-ssh qdial.hull. 2>&1))
got=$?
wait "$runner"
[ "$got" = 0 ] && [ "$heard" = "hello from the machine" ] &&
	grep -q '^served=hi from ssh$' "$root/dial.out" ||
	fail "proxy-ssh did not carry the machine's port 22 both ways: $got" \
		"$heard; $(cat "$root/dial.out")"
cases=$((cases + 1))
[ "$refused_got" = 125 ] && [ "$refused" = "hull: port 22: Connection refused" ] ||
	fail "proxy-ssh to a port nothing listens on was not refused: $refused_got" "$refused"
# And ssh itself, through the block ssh-config writes: the probe on port
# 22 speaks no SSH, so the login fails, but the line it reads is ssh's own
# identification, which came by ssh, hull proxy-ssh and the agent.
conf sshed '(backend vm) (hostname "qsshed") (command "/probe" "serve" "tcp" "22")'
mkdir -p "$root/home"
rm -f "$root/home/.ssh/config"
HOME="$root/home" timeout 10 "$hull" ssh-config > /dev/null 2>&1
cases=$((cases + 1))
rm -f "$root/sshed.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/sshed.conf" > "$root/sshed.out" 2>&1) &
runner=$!
tries=0
until grep -q '^serving$' "$root/sshed.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
said=$(timeout 30 ssh -F "$root/home/.ssh/config" -o BatchMode=yes -o ConnectTimeout=10 \
	-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null qsshed.hull true < /dev/null 2>&1)
wait "$runner"
grep -q '^served=SSH-2\.0-' "$root/sshed.out" ||
	fail "ssh through ssh-config's block did not reach the machine's port 22" \
		"$said; $(cat "$root/sshed.out")"
# A udp port: each client its own flow, each datagram whole, an empty one
# too, and the machine's replies to the client that sent. A datagram the
# machine is not yet listening for is dropped, as udp's are, and the
# client sends again, 360 times a quarter of a second apart at most. The
# refusal that datagram meets in the machine does not end its flow: once
# the probe listens again, the client comes from the port it came from.
conf udp "(backend vm) (hostname \"qudp\") (ports (port (host $greet) (guest 53) (proto udp)))" \
	'(command "/probe" "echo" "53" "sleep" "2" "echo" "53")'
at=$(address_of qudp)
cases=$((cases + 1))
rm -f "$root/udp.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/udp.conf" > "$root/udp.out" 2>&1) &
runner=$!
heard=$(timeout 120 python3 -u -c '
import socket, sys, time
at = (sys.argv[1], int(sys.argv[2]))
def ask(s, data):
    for _ in range(360):
        s.sendto(data, at)
        s.settimeout(0.25)
        try:
            while True:
                reply = s.recv(65536)
                if reply == data:
                    return True
        except socket.timeout:
            pass
    return False
a = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
b = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print("a=%s b=%s" % (ask(a, b"from a"), ask(b, b"from b")))
for n in (0, 1, 1400, 65000):
    print("%d=%s" % (n, ask(a, bytes((i * 7) & 0xff for i in range(n)))))
print("end=%s" % ask(b, b"end"))
# Into the gap, then until the second echo answers.
time.sleep(0.5)
a.sendto(b"refused", at)
time.sleep(0.5)
print("again=%s" % ask(a, b"again"))
print("end=%s" % ask(a, b"end"))' "$at" "$greet" 2>&1)
wait "$runner"
got=$?
[ "$got" = 0 ] && [ "$heard" = "a=True b=True
0=True
1=True
1400=True
65000=True
end=True
again=True
end=True" ] && [ "$(grep -c '^echoed from' "$root/udp.out")" = 2 ] &&
	grep -q '^echoed from 2$' "$root/udp.out" && grep -q '^echoed from 1$' "$root/udp.out" &&
	[ "$(grep '^from ' "$root/udp.out" | sed -n 1p)" = "$(grep '^from ' "$root/udp.out" | sed -n 3p)" ] ||
	fail "a udp port's datagrams were not each client's, whole: $got" \
		"$heard; $(cat "$root/udp.out")"
cases=$((cases + 1))
grep -q 'ports: emulated: tcp and udp, forwarded over vsock' "$root/udp.out" ||
	fail "the udp machine did not say how its ports are forwarded" "$(cat "$root/udp.out")"

# A running machine: listed, signalled through the agent, and stopped by
# SIGTERM to its frontend.
conf trap '(backend vm) (hostname "qtrap") (command "/probe" "trap" "say" "after")'
cases=$((cases + 1))
rm -f "$root/trap.out"
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/trap.conf" > "$root/trap.out" 2>&1) &
runner=$!
await qtrap
# A machine booted and ended under the same core, which this one keeps
# running: its passt ends with it, and this one's is the one left.
conf brief '(backend vm) (command "/probe" "say" "brief")'
name=brief
boot brief
has 0 '^brief$'
cases=$((cases + 1))
left=$(pgrep -f "$passt")
[ "$(printf '%s\n' "$left" | grep -c .)" = 1 ] ||
	fail "a passt outlived its machine, or the running one has none" "$left"
listed=$("$hull" ps 2>&1)
printf '%s\n' "$listed" | grep -Eq "^qtrap +$(address_of qtrap | sed 's/\./\\./g') +qemu +vm +running\$" ||
	fail "a running vm is not listed" "$listed"
cases=$((cases + 1))
tries=0
until grep -q '^ready$' "$root/trap.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
timeout 10 "$hull" kill -s TERM qtrap > /dev/null 2>&1
wait "$runner"
got=$?
[ "$got" = 0 ] && grep -q '^signal=15$' "$root/trap.out" && grep -q '^after$' "$root/trap.out" ||
	fail "a TERM through the agent was not the workload's to catch: $got" \
		"$(cat "$root/trap.out"); core.log: $(cat "$XDG_RUNTIME_DIR/hull/core.log" 2>&1)"

conf wait '(backend vm) (hostname "qwait") (command "/probe" "say" "up" "wait")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/wait.conf" > /dev/null 2>&1) &
runner=$!
await qwait
started=$(date +%s)
kill -TERM "$runner"
wait "$runner"
got=$?
took=$(($(date +%s) - started))
[ "$got" = 143 ] && [ "$took" -lt 10 ] ||
	fail "SIGTERM to hull run ended a vm with $got after ${took}s, wanted 143" ""

# exec: commands in a running machine, each on a channel of the agent's.
# The machine sleeps rather than waits, since these take longer than
# wait's twenty seconds.
conf execd '(backend vm) (hostname "qexec") (env (GREETING "hi")) (command "/probe" "sleep" "120")'
(cd / && exec timeout -k 2 140 "$hull" run -f "$root/execd.conf" > /dev/null 2>&1) &
execer=$!
await qexec
xcase() { # STATUS PATTERN COMMAND...: an exec, as has() checks a boot
	want=$1
	pattern=$2
	shift 2
	name="exec $*"
	out=$(timeout 30 "$@" 2>&1)
	got=$?
	has "$want" "$pattern"
}
xcase 4 '^hello$' "$hull" exec qexec /probe say hello env GREETING host exit 4
xcase 4 '^GREETING=hi$' "$hull" exec qexec /probe say hello env GREETING host exit 4
xcase 4 '^host=qexec$' "$hull" exec qexec /probe say hello env GREETING host exit 4
xcase 127 '/nope: No such file or directory' "$hull" exec qexec /nope
xcase 0 '^stdin=\(none\)$' sh -c '"$0" exec qexec /probe stdin < /dev/null' "$hull"

# stdin on the agent's credit: whole and in order, and to a command that
# does not read for six seconds.
want=$(seq 1 200000 | python3 -c 'import sys
h = 2166136261
data = sys.stdin.buffer.read()
for b in data:
	h = ((h ^ b) * 16777619) & 0xffffffff
print("bytes=%d fnv=%08x" % (len(data), h))')
xcase 0 "^$want\$" sh -c 'seq 1 200000 | "$0" exec qexec /probe count' "$hull"
xcase 0 '^bytes=300000 ' sh -c 'head -c 300000 /dev/zero | "$0" exec qexec /probe sleep 6 count' "$hull"
# Many times the agent's 64 KiB, to a command not reading: held on the
# agent's credit, none of it refused.
xcase 0 '^bytes=3000000 ' sh -c 'head -c 3000000 /dev/zero | "$0" exec qexec /probe sleep 3 count' "$hull"

# Output on the host's credit: 3 MB whole, as the same probe on the host
# writes it; 20 MB to a reader seven seconds behind, whole, while the
# machine goes on serving another exec.
want=$("$probe" spew 3000000 | "$probe" count)
xcase 0 "^$want\$" sh -c '"$0" exec qexec /probe spew 3000000 | "$1" count' "$hull" "$probe"
# Held, not read into the host: two seconds in, the machine counts one
# process more, the command still writing.
cases=$((cases + 1))
before=$(timeout 10 "$hull" exec qexec /probe procs 2>&1)
(timeout -k 2 30 "$hull" exec qexec /probe spew 20000000 | (sleep 7; wc -c) > "$root/held.out" 2>&1) &
held=$!
sleep 2
during=$(timeout 10 "$hull" exec qexec /probe procs 2>&1)
started=$(date +%s)
out=$(timeout 10 "$hull" exec qexec /probe say free 2>&1)
took=$(($(date +%s) - started))
[ "${during#procs=}" = "$((${before#procs=} + 1))" ] && [ "$out" = free ] && [ "$took" -lt 3 ] ||
	fail "a command whose reader is behind was not held, or the machine with it" \
		"$before, then $during; $out after ${took}s"
wait "$held"
name="held"
out=$(cat "$root/held.out")
got=0
has 0 '^20000000$'

# A terminal of the size asked, resized; spoken by hand, as a frontend not
# ours might, so that the sizes are the test's own.
cases=$((cases + 1))
out=$(timeout 20 python3 -c '
import os, socket, struct, sys, time
s = socket.socket(socket.AF_UNIX)
s.settimeout(15)
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
say(b"(5:hello(2:ch1:0)(5:proto1:1))")
say(b"(4:exec(4:argv6:/probe3:tty5:winch)(2:ch1:1)(7:machine5:qexec)(3:tty(4:cols3:100)(4:rows2:30)))")
hear(b"ready")
say(b"(5:winch(2:ch1:1)(4:cols3:120)(4:rows2:40))")
hear(b"6:exited")
print(got.decode("latin-1"))
' 2>&1)
printf '%s\n' "$out" | grep -q 'tty=yes' && printf '%s\n' "$out" | grep -q 'size=100x30' &&
	printf '%s\n' "$out" | grep -q 'controlling=yes' && printf '%s\n' "$out" | grep -q 'size=120x40' ||
	fail "a terminal was not of the size asked, or not resized" "$out"

# ssh: the machine's /bin/sh, here the probe saying what it was given,
# on a terminal, with the frontend's TERM as its argument.
mkdir -p "$dir/rootfs/bin"
cp "$probe" "$dir/rootfs/bin/sh"
cases=$((cases + 1))
out=$(HULL=$hull SHELL=/bin/sh timeout 30 script -qec 'TERM=xterm-hull "$HULL" ssh qexec' \
	/dev/null 2>&1 | tr -d '\r')
printf '%s\n' "$out" | grep -q '^sh 4=xterm-hull$' && printf '%s\n' "$out" | grep -q '^tty=yes$' ||
	fail "ssh gave no shell on a terminal with the frontend's TERM" "$out"

# A TERM to hull exec is the command's, which catches it and goes on.
cases=$((cases + 1))
rm -f "$root/xtrap.out"
(timeout -k 2 30 "$hull" exec qexec /probe trap say after > "$root/xtrap.out" 2>&1) &
trapper=$!
tries=0
until grep -q '^ready$' "$root/xtrap.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
kill -TERM "$trapper"
wait "$trapper"
got=$?
[ "$got" = 0 ] && grep -q '^signal=15$' "$root/xtrap.out" && grep -q '^after$' "$root/xtrap.out" ||
	fail "a TERM to hull exec was not the command's to catch: $got" "$(cat "$root/xtrap.out")"

# A frontend killed takes its command with it: the machine counts as many
# processes after as before.
cases=$((cases + 1))
before=$(timeout 10 "$hull" exec qexec /probe procs 2>&1)
head -c 2000000 /dev/zero | timeout -s KILL 2 "$hull" exec qexec /probe sleep 30 > /dev/null 2>&1
sleep 1
after=$(timeout 10 "$hull" exec qexec /probe procs 2>&1)
[ -n "$before" ] && [ "$before" = "$after" ] ||
	fail "a command outlived its killed frontend" "before $before, after $after"

# What the host spends on a vm: qemu's own CPU time, io and memory, which
# hold the guest's. An exec's 300 ms of CPU, 300 more in a child it
# reaps, and 4 MiB written through 9p show in qemu's figures.
figure() { # NAME STATS: one figure of hull stats, the cpu in microseconds
	printf '%s\n' "$2" | awk -v name="$1" '$1 == name { gsub(/\./, "", $2); print $2 + 0 }'
}
first=$(timeout 10 "$hull" stats qexec 2>&1)
xcase 0 '^filled=4$' "$hull" exec qexec /probe spin 300 reap 300 fill /written 4
second=$(timeout 10 "$hull" stats qexec 2>&1)
rm -f "$root/rootfs/written"
cases=$((cases + 1))
[ "$(figure cpu "$second")" -ge $(($(figure cpu "$first") + 590000)) ] 2>/dev/null ||
	fail "stats: a vm's cpu, wanted the exec's 0.6 s more" "$first
$second"
cases=$((cases + 1))
[ "$(figure io "$second")" -ge $(($(figure io "$first") + 4194304)) ] 2>/dev/null ||
	fail "stats: a vm's io, wanted the exec's 4 MiB more" "$first
$second"
cases=$((cases + 1))
[ "$(figure memory "$second")" -ge 33554432 ] 2>/dev/null ||
	fail "stats: a vm's memory, wanted at least 32 MiB" "$second"

# A machine that stops takes its commands, and their frontends hear it at
# once, 137.
cases=$((cases + 1))
(timeout -k 2 30 "$hull" exec qexec /probe sleep 30 > /dev/null 2>&1; echo $? > "$root/xstop.out") &
waiter=$!
sleep 1
started=$(date +%s)
timeout 20 "$hull" stop qexec > /dev/null 2>&1
wait "$waiter"
got=$(cat "$root/xstop.out")
took=$(($(date +%s) - started))
[ "$got" = 137 ] && [ "$took" -lt 8 ] ||
	fail "an exec in a stopped machine ended $got after ${took}s, wanted 137 at once" ""
wait "$execer" 2>/dev/null
xcase 1 'the machine is not running' "$hull" stats qexec

# A machine dies with its core: nothing could manage it after.
conf kept '(backend vm) (hostname "qkept") (on_disconnect keep) (command "/probe" "wait")'
cases=$((cases + 1))
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/kept.conf" > /dev/null 2>&1) &
runner=$!
await qkept
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
if [ -z "$core" ]; then
	fail "no core to kill" ""
else
	kill -KILL "$core"
	sleep 1
	left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
	[ -z "$left" ] || fail "a vm outlived its core" "$(ps -o pid,args -p "$left")"
	cases=$((cases + 1))
	left=$(pgrep -f "$passt")
	[ -z "$left" ] || fail "a vm's passt outlived its core" "$(ps -o pid,args -p "$left")"
fi
wait "$runner" 2>/dev/null
# Its directory is left, the core gone: the next core's first machine
# removes it, which the last case requires.

# The emu class: the same machine under TCG.
conf emu '(backend emu) (mutable overlay) (command "/probe" "say" "under tcg" "read"' \
	'"/sys/devices/system/clocksource/clocksource0/available_clocksource" "write" "/emulated")'
name=emu
boot emu 120
has 0 '^under tcg$'
# An overlay under TCG too, its write the machine's.
has 0 '^write=ok$'
cases=$((cases + 1))
[ ! -e "$root/rootfs/emulated" ] || fail "an emu overlay's write reached the directory" ""
# Emulated: no KVM clock, which the vm case above has.
cases=$((cases + 1))
if printf '%s\n' "$out" | grep -q 'kvm-clock'; then
	fail "the emu class ran with KVM" "$out"
fi

# Secrets (J.3), placed by the agent: the core that writes them on the
# host needs a tmpfs, so they run in a runtime directory of their own,
# made under /dev/shm and removed once that core has left. A dir of their
# own too, as the agent needs /run, which rootfs/ has not, for that case.
shm=$(mktemp -d /dev/shm/hull-qemu-test.XXXXXX)
mkdir -p "$root/secret/run" "$root/secret/proc" "$root/secret/dev" "$root/secret/sys"
cp "$probe" "$root/secret/probe"
printf 'from a file\n' > "$root/pw.txt"
printf '(machine (source "dir:./secret") (backend vm) (command %s) (secrets %s))\n' \
	'"/probe" "list" "/run/hull/secrets" "read" "/run/hull/secrets/pw" "owner" "/run/hull/secrets/pw" "env" "made" "write" "/run/hull/secrets/new"' \
	'(pw (source "file:pw.txt")) (made (source "exec:printf made") (expose env))' > "$root/secret.conf"
name=secret
out=$(cd / && XDG_RUNTIME_DIR=$shm timeout 60 "$hull" run -f "$root/secret.conf" 2>&1)
got=$?
has 0 '^list= made pw$'
has 0 '^read=from a file$'
has 0 '^owner=0:0 0400$'
has 0 '^made=made$'
has 0 '^write=Read-only file system$'
printf '(machine (source "dir:./rootfs") (backend vm) (command "/probe" "say" "ran") %s)\n' \
	'(secrets (pw (source "file:pw.txt")))' > "$root/norun.conf"
name=norun
out=$(cd / && XDG_RUNTIME_DIR=$shm timeout 60 "$hull" run -f "$root/norun.conf" 2>&1)
got=$?
has 125 'no /run for its secrets'
tries=0
while [ -S "$shm/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
left=$(ls -A "$shm/hull/secrets" 2>&1; ls -d "$shm"/hull/qemu-* 2>/dev/null)
[ -z "$left" ] || fail "a vm's secrets or its directory were left" "$left"
rm -rf "${shm:?}"

# Nothing left: no qemu of this suite's, no passt, no machine directory.
cases=$((cases + 1))
sleep 2
left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
left=$left$(pgrep -f "$passt")
dirs=$(ls -d "$XDG_RUNTIME_DIR"/hull/qemu-* 2>/dev/null)
[ -z "$left" ] && [ -z "$dirs" ] || fail "a vm or its directory was left" "$left $dirs"

printf 'qemu: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
