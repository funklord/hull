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
# A data home of the test's own too, never the account's: (ssh true) keeps a
# persistent per-machine host key under it (section 8.1 item 2).
XDG_DATA_HOME=$(cd "${BUILD_DIR:?}" && pwd)/test/qemu/data
export XDG_DATA_HOME
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR" "$dir/rootfs/proc" "$dir/rootfs/dev" "$dir/rootfs/sys" \
	"$dir/rootfs/srv" "$dir/rootfs/etc" "$dir/data"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$probe" "$dir/rootfs/probe"
# A resolver no machine can reach, which the agent's must cover.
printf 'nameserver 127.0.0.53\n' > "$dir/rootfs/etc/resolv.conf"
root=$(cd "$dir" && pwd)
# A HOME of the test's own, never the account's: the core reads the user's
# ~/.ssh public keys for (ssh true)'s authorized_keys, and it must read a
# fixture rather than the real ones. Exported so the core, started by the
# first hull call below, has it. The ssh-config case below already used
# this HOME per command.
export HOME="$root/home"
mkdir -p "$root/home/.ssh"
chmod 700 "$root/home/.ssh"
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5hulltestkey hull-test\n' > "$root/home/.ssh/id_ed25519.pub"
# The engine's passt, by the arguments only it gives.
passt='^passt --quiet --fd 3 --one-off --ipv4-only --address 10\.0\.2\.15 '
# passts_new OTHERS: the engine's passts now, less those in OTHERS, as
# `others=$(passts_now)` took them before the machine a check is about.
# Those arguments are every hull vm's, and passt runs non-dumpable, so
# neither its environment nor its descriptors say whose it is: another
# session's hull vm on the host is told apart only by having been there
# first, and one started within a check's own window is not.
passts_now() {
	pgrep -f "$passt" | tr '\n' ' '
}
passts_new() {
	for p in $(pgrep -f "$passt"); do
		case " $1 " in
		*" $p "*) ;;
		*) printf '%s\n' "$p" ;;
		esac
	done
}
others_at_start=$(passts_now)
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# The agent names the commit it was built from, as hull does, so a result
# resting on a vm can name the agent it ran under.
cases=$((cases + 1))
agent_version=$(timeout 5 "$(dirname "$hull")/hull-agent" --version 2>&1)
printf '%s\n' "$agent_version" | grep -Eq '^commit ([0-9a-f]{12}(-dirty)?|unknown)$' ||
	fail "hull-agent --version did not name its commit" "$agent_version"

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
xcase() { # STATUS PATTERN COMMAND...: an exec, as has() checks a boot
	want=$1
	pattern=$2
	shift 2
	name="exec $*"
	out=$(timeout 30 "$@" 2>&1)
	got=$?
	has "$want" "$pattern"
}

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
# A bare command is searched for in the machine's PATH, as docker does.
conf searched '(backend vm) (command "probe" "say" "searched") (env (PATH "/nowhere:/"))'
name=searched
boot searched
has 0 '^searched$'

# (network none): no NIC, so the host's gateway is unreachable, and lo up,
# so a dial to a port nothing holds there is refused; ports with it are
# refused, having no network to be on.
conf netnone '(backend vm) (network none) (command "/probe" "dial" "127.0.0.1" "9" "dial" "10.0.2.2" "9")'
name=netnone
boot netnone
has 0 '^dial 127\.0\.0\.1:9=Connection refused$'
has 0 '^dial 10\.0\.2\.2:9=Network is unreachable$'
conf netports '(backend vm) (network none) (ports (port (host 8080) (guest 80))) (command "/probe")'
name=netports
boot netports
has 125 'ports need the machine.s network, which \(network none\) has not'

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
# Ids as the guest sees them through qemu's user namespace, where the
# user's are 0 and any other is nobody's, as on chroot.
guest_id() { # HOST-ID USER-ID
	if [ "$1" = "$2" ]; then echo 0; else echo 65534; fi
}
top=$(guest_id "$(stat -c %u "$root/rootfs")" "$(id -u)"):$(guest_id "$(stat -c %g "$root/rootfs")" "$(id -g)")
group=0
[ -g "$root/rootfs" ] && group=$(guest_id "$(stat -c %g "$root/rootfs")" "$(id -g)")
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
# A setuid file of the user's is setuid root in the guest, as it is on
# chroot: qemu runs in a user namespace where the user is root, so its 9p
# server shows the user's files as root's. Outside one, root running it
# ran as the user, uid 1000 say, and mount, su and sudo with it.
cp "$probe" "$root/rootfs/suid"
chmod 4755 "$root/rootfs/suid"
conf suid '(backend vm) (command "/suid" "uid" "euid" "owner" "/suid")'
name=suid
boot suid
has 0 '^uid=0$'
has 0 '^euid=0$'
has 0 '^owner=0:[0-9]+ 4755$'
rm -f "$root/rootfs/suid"
# Read-only as well, it is the share itself, read-only.
conf overro '(backend vm) (mutable overlay) (security (readonly_root true))' \
	'(command "/probe" "write" "/again")'
name=overro
boot overro
has 0 '^write=Read-only file system$'
# The host's modules, which are the guest kernel's: where modprobe looks,
# read-only, and loadable, as a test of network software loads
# mac80211_hwsim and gets two radios. Its dependencies are loaded first,
# the last listed first, as modprobe does. Above an overlay their place is
# made; in a directory used in place, which has none, none is made.
release=$(uname -r)
hwsim=$(grep '/mac80211_hwsim\.ko[^:]*:' "/lib/modules/$release/modules.dep")
if [ -z "$hwsim" ]; then
	printf 'qemu: the host has no mac80211_hwsim, so its case is skipped\n'
else
	loads=
	for module in $(printf '%s\n' "${hwsim#*:}" | tr ' ' '\n' | sed '/^$/d' | tac) "${hwsim%%:*}"; do
		loads="$loads \"load\" \"$module\""
	done
	conf modules '(backend vm) (mutable overlay)' \
		"(command \"/probe\" $loads \"list\" \"/sys/class/ieee80211\"" \
		"\"write\" \"/lib/modules/$release/written\")"
	name=modules
	boot modules
	has 0 '^load=ok$'
	has 0 '^list= phy0 phy1$'
	has 0 '^write=Read-only file system$'
	refute_out() { # PATTERN: no line of the last boot's matches it
		cases=$((cases + 1))
		! printf '%s\n' "$out" | grep -Eq -- "$1" || fail "$name: a line matches /$1/" "$out"
	}
	refute_out '^load=[^o]'
fi
conf inplace '(backend vm)' '(command "/probe" "say" "booted")'
name=inplace
boot inplace
has 0 '^booted$'
cases=$((cases + 1))
[ ! -e "$root/rootfs/lib" ] || fail "a modules place was made in a directory used in place" \
	"$(ls -la "$root/rootfs" 2>&1)"
# (kernel "path:FILE"): a bring-your-own kernel, booted as given from any
# path, not /boot. Proven with the host's own kernel copied elsewhere: the
# engine reads the release from the image's boot header and shares that
# release's modules, so a load succeeds and the booted osrelease is the
# copy's. A native vector, so unlike bundled it prints no emulated line.
# netcfgd's own kernels, built CONFIG_MODULES=n with drivers built in, are
# the no-modules half (section 7), which the host cannot boot and it runs.
cp "/boot/vmlinuz-$release" "$root/mykernel"
if [ -n "$hwsim" ]; then
	# The release for a grep -E pattern: . and + are its metacharacters.
	release_re=$(printf '%s' "$release" | sed 's/[][\\.^$*+?(){}|]/\\&/g')
	printf '(machine (source "dir:./rootfs") (backend vm) (mutable overlay) %s %s)\n' \
		"(tuning (vm (kernel \"path:$root/mykernel\")))" \
		"(command \"/probe\" $loads \"list\" \"/sys/class/ieee80211\" \"read\" \"/proc/sys/kernel/osrelease\")" \
		> "$root/vkernel.conf"
	name=vkernel
	boot vkernel
	has 0 '^load=ok$'
	has 0 '^list= phy0 phy1$'
	has 0 "^read=$release_re\$"
	refute_out 'vm\.kernel\.bundled: emulated'
fi
# A kernel path that is not there is refused to the host, before any boot.
printf '(machine (source "dir:./rootfs") (backend vm) %s %s)\n' \
	"(tuning (vm (kernel \"path:$root/nokernel\")))" \
	'(command "/probe" "say" "no")' > "$root/vkernelno.conf"
name=vkernelno
boot vkernelno
has 125 'the kernel at .kernel .*cannot be read'
# A readable file that is not a kernel is given to qemu as it stands, which
# refuses it: it reaches qemu rather than falling back to the host's, where a
# bundled boot would have run the command and said "booted". It carries no
# release, so write_initramfs makes a module-free initramfs for it (the
# no-modules branch) before qemu rejects the image.
head -c 4096 /dev/urandom > "$root/badkernel"
printf '(machine (source "dir:./rootfs") (backend vm) %s %s)\n' \
	"(tuning (vm (kernel \"path:$root/badkernel\")))" \
	'(command "/probe" "say" "booted")' > "$root/vkernelbad.conf"
name=vkernelbad
boot vkernelbad
cases=$((cases + 1))
[ "$got" != 0 ] || fail "vkernelbad booted a non-kernel (a bundled fallback?)" "$out"
cases=$((cases + 1))
! printf '%s\n' "$out" | grep -q '^booted$' ||
	fail "vkernelbad ran the command, so qemu did not get the given file" "$out"
# (kernel image): the image's own /hull/kernel, booted as given -- a
# different kernel version per machine, which Docker cannot do. Proven, as
# the path case is, with the host's own kernel put in the image tree: it
# boots from there, its release read and its modules loaded, and no bundled
# emulated line. A tree with no /hull/kernel is refused, and a /hull/kernel
# that is not a kernel reaches qemu rather than falling back to the host's.
rm -rf "${root:?}/vkimg"
mkdir -p "$root/vkimg/proc" "$root/vkimg/dev" "$root/vkimg/sys" "$root/vkimg/hull"
cp "$probe" "$root/vkimg/probe"
cp "/boot/vmlinuz-$release" "$root/vkimg/hull/kernel"
if [ -n "$hwsim" ]; then
	printf '(machine (source "dir:%s/vkimg") (backend vm) (mutable overlay) %s %s)\n' "$root" \
		'(tuning (vm (kernel image)))' \
		"(command \"/probe\" $loads \"list\" \"/sys/class/ieee80211\" \"read\" \"/proc/sys/kernel/osrelease\")" \
		> "$root/vkimage.conf"
	name=vkimage
	boot vkimage
	has 0 '^load=ok$'
	has 0 '^list= phy0 phy1$'
	has 0 "^read=$release_re\$"
	refute_out 'vm\.kernel\.bundled: emulated'
fi
# No /hull/kernel in the tree: refused before any boot.
printf '(machine (source "dir:./rootfs") (backend vm) %s %s)\n' \
	'(tuning (vm (kernel image)))' '(command "/probe" "say" "no")' > "$root/vkimageno.conf"
name=vkimageno
boot vkimageno
has 125 'kernel image.*/hull/kernel.*has none'
# A /hull/kernel that is not a kernel reaches qemu, not a bundled fallback.
head -c 4096 /dev/urandom > "$root/vkimg/hull/kernel"
printf '(machine (source "dir:%s/vkimg") (backend vm) %s %s)\n' "$root" \
	'(tuning (vm (kernel image)))' '(command "/probe" "say" "booted")' > "$root/vkimagebad.conf"
name=vkimagebad
boot vkimagebad
cases=$((cases + 1))
[ "$got" != 0 ] || fail "vkimagebad booted a non-kernel (a bundled fallback?)" "$out"
cases=$((cases + 1))
! printf '%s\n' "$out" | grep -q '^booted$' ||
	fail "vkimagebad ran the command, so qemu did not get the image's /hull/kernel" "$out"
rm -rf "${root:?}/vkimg"
# Appendix D's segments: a NIC each, after passt's, on the core's switch,
# with no address given, as a test of network software sets its own.
# Two vms on one hear each other in raw frames, the shouter's own MAC
# the source the hearer is given, and are answered; a third, on another
# segment at the same time, hears nothing. The name is this run's, so
# two runs at once are not on one wire.
wire="wire-$$"
conf hearer "(backend vm) (mutable overlay) (hostname \"hearer\") (segments \"$wire\")" \
	'(command "/probe" "list" "/sys/class/net" "read" "/sys/class/net/eth1/address"' \
	'"link" "eth1" "hear" "eth1")'
conf shouter "(backend vm) (mutable overlay) (hostname \"shouter\") (segments \"$wire\")" \
	'(command "/probe" "read" "/sys/class/net/eth1/address" "link" "eth1"' \
	'"shout" "eth1" "over the wire")'
conf loner "(backend vm) (mutable overlay) (hostname \"loner\") (segments \"elsewhere-$$\")" \
	'(command "/probe" "link" "eth1" "hear" "eth1")'
# The core's descriptors, counted before and after: each port goes once
# its qemu has, which it does only if the core keeps no end of its own.
# A chroot machine that waits holds the core meanwhile, which would
# otherwise leave a second after the last machine, and be counted gone.
# Its ceiling is well above the cases' own: at a load of 212 they outlived
# its 120 s, and the core idled out before its descriptors were counted.
conf anchor '(backend chroot) (hostname "anchor") (command "/probe" "say" "ready" "wait")'
(cd / && exec timeout 300 "$hull" run -f "$root/anchor.conf" > "$root/anchor.out" 2>&1) &
anchored=$!
tries=0
until grep -q '^ready$' "$root/anchor.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
core=$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock" 2>/dev/null)
fds_before=$(ls "/proc/$core/fd" 2>/dev/null | wc -l)
(cd / && exec timeout 90 "$hull" run -f "$root/hearer.conf" > "$root/hearer.out" 2>&1) &
heard=$!
(cd / && exec timeout 90 "$hull" run -f "$root/loner.conf" > "$root/loner.out" 2>&1) &
lone=$!
name=shouter
boot shouter 90
wait "$heard"
heard_status=$?
wait "$lone"
has 0 '^link=up$'
has 0 '^shout=answered$'
shouter_mac=$(printf '%s\n' "$out" | sed -n 's/^read=//p')
out=$(cat "$root/hearer.out")
got=$heard_status
name=hearer
has 0 '^list= eth0 eth1 lo$'
hearer_mac=$(printf '%s\n' "$out" | sed -n 's/^read=//p')
has 0 "^hear=over the wire from $shouter_mac\$"
cases=$((cases + 1))
[ -n "$shouter_mac" ] && [ "$shouter_mac" != "$hearer_mac" ] ||
	fail "two machines on a segment share a MAC, or have none" "shouter $shouter_mac, hearer $hearer_mac"
out=$(cat "$root/loner.out")
got=0
name=loner
has 0 '^hear=nothing$'
tries=0
fds_after=$(ls "/proc/$core/fd" 2>/dev/null | wc -l)
while [ "$fds_after" -ne "$fds_before" ] && [ "$tries" -lt 20 ]; do
	tries=$((tries + 1))
	sleep 0.25
	fds_after=$(ls "/proc/$core/fd" 2>/dev/null | wc -l)
done
cases=$((cases + 1))
[ -n "$core" ] && [ -d "/proc/$core" ] && [ "$fds_before" -gt 0 ] &&
	[ "$fds_after" -eq "$fds_before" ] ||
	fail "the core kept descriptors of a segment's machines" \
		"core ${core:-none}: $fds_before before, $fds_after after"
# A chroot machine and a vm on one segment: the chroot's tap and the
# vm's NIC frame alike on the core's switch, so the chroot hears the vm
# from the vm's own MAC; and the core keeps no end of the chroot's port.
conf mixhear "(backend chroot) (hostname \"mixhear\") (network none) (segments \"$wire\")" \
	'(command "/probe" "link" "eth0" "hear" "eth0")'
(cd / && exec timeout 90 "$hull" run -f "$root/mixhear.conf" > "$root/mixhear.out" 2>&1) &
mixheard=$!
name=shouter
boot shouter 90
wait "$mixheard"
has 0 '^shout=answered$'
shouter_mac=$(printf '%s\n' "$out" | sed -n 's/^read=//p')
cases=$((cases + 1))
[ -n "$shouter_mac" ] && grep -q "^hear=over the wire from $shouter_mac\$" "$root/mixhear.out" ||
	fail "a chroot machine did not hear a vm on its segment" "vm $shouter_mac: $(cat "$root/mixhear.out")"
tries=0
fds_after=$(ls "/proc/$core/fd" 2>/dev/null | wc -l)
while [ "$fds_after" -ne "$fds_before" ] && [ "$tries" -lt 20 ]; do
	tries=$((tries + 1))
	sleep 0.25
	fds_after=$(ls "/proc/$core/fd" 2>/dev/null | wc -l)
done
cases=$((cases + 1))
[ -d "/proc/$core" ] && [ "$fds_after" -eq "$fds_before" ] ||
	fail "the core kept descriptors of a chroot machine's segment" \
		"core ${core:-none}: $fds_before before, $fds_after after"
# (devices (serial ...)): a host serial port behind the guest's ttyS0. A
# pty stands in for the port, its far end held here: the guest's line
# reaches it through qemu, as a board's console would. And a device no
# host has, refused before qemu starts.
cat > "$root/serialpty.py" <<'PY'
import os, pty, select, sys, time
master, slave = pty.openpty()
with open(sys.argv[1], "w") as f:
    f.write(os.ttyname(slave) + "\n")
heard = b""
end = time.time() + float(sys.argv[3])
while time.time() < end and b"hello from ttyS0" not in heard:
    r, _, _ = select.select([master], [], [], 0.5)
    if r:
        try:
            heard += os.read(master, 1024)
        except OSError:
            break
with open(sys.argv[2], "wb") as f:
    f.write(heard)
PY
rm -f "$root/serialpty.name" "$root/serialpty.heard"
timeout 100 python3 "$root/serialpty.py" "$root/serialpty.name" "$root/serialpty.heard" 90 &
serial_pid=$!
tries=0
until [ -s "$root/serialpty.name" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
conf vmserial "(backend vm) (devices (serial \"$(cat "$root/serialpty.name" 2>/dev/null)\")) (command \"/probe\" \"tell\" \"/dev/ttyS0\" \"hello from ttyS0\")"
boot vmserial 90
has 0 '^tell=ok$'
wait "$serial_pid"
cases=$((cases + 1))
grep -q 'hello from ttyS0' "$root/serialpty.heard" 2>/dev/null ||
	fail "a vm's ttyS0 did not reach the host's serial port" "$(cat "$root/serialpty.heard" 2>&1)"
rm -f "$root/serialpty.name" "$root/serialpty.heard" "$root/serialpty.py"
conf vmusb '(backend vm) (devices (usb (device (vendor "ffff") (product "fffe")))) (command "/probe")'
boot vmusb
has 125 'no USB device ffff:fffe is plugged in'

# (network bridge) on a vm: a tap on a host bridge, which only root may
# put there, so only a core run as root takes it; as this user, refused.
# As root in a user namespace of its own, unshare -rmn, whose network is
# its own: a vm on hull0 given its drawn address, the gateway and an
# address beyond the bridge reached by its route out, and a chroot
# machine's port on the same bridge reached, vm and chroot on one wire; a
# vm on a bridge of its owner's naming, eth0 up and unaddressed; and its
# tap, persistent, removed with each machine.
printf '(machine (source "dir:./rootfs") (backend vm) (mutable overlay) (network bridge) (command "/probe"))\n' \
	> "$root/vbridged.conf"
cases=$((cases + 1))
out=$(cd / && timeout 60 "$hull" run -f "$root/vbridged.conf" 2>&1)
printf '%s\n' "$out" | grep -q '(network bridge) needs hull.s core to run as root' ||
	fail "a vm on a bridge was not refused to a core not run as root" "$out"
vcdb_at=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("172.30.%d.%d" % (h // 254, h % 254 + 1))' vcdb)
printf '(machines (vbweb %s) (vcdb %s))\n' \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (network bridge) (command \"/probe\" \"addr\" \"eth0\" \"dial\" \"172.31.0.1\" \"9\" \"dial\" \"10.200.0.1\" \"8081\" \"dial\" \"$vcdb_at\" \"80\" \"read\" \"/etc/resolv.conf\" \"dns\" \"anything.example\")" \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network bridge) (command \"/probe\" \"serve\" \"tcp\" \"80\")" \
	> "$root/vbridge2.conf"
printf '(machine (source "dir:./rootfs") (backend vm) (mutable overlay) (network bridge) (bridge "brtest") (command "/probe" "addr" "eth0"))\n' \
	> "$root/vbrnamed.conf"
# A resolver of the test's own on the namespace's loopback, which is all a
# host with systemd-resolved gives: hull0's relay is what reaches it. It
# answers every question with one A, 192.0.2.7.
printf 'nameserver 127.0.0.53\n' > "$root/relay-resolv.conf"
cat > "$root/fakedns.py" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.53", 53))
s.settimeout(float(sys.argv[1]))
while True:
    try:
        q, a = s.recvfrom(4096)
    except socket.timeout:
        break
    end = 12
    while q[end] != 0:
        end += 1 + q[end]
    end += 5
    r = q[:2] + b"\x81\x80" + q[4:6] + b"\x00\x01\x00\x00\x00\x00" + q[12:end]
    r += b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x05\x00\x04" + bytes([192, 0, 2, 7])
    s.sendto(r, a)
PY
rm -rf "${root:?}/vbrrun"
mkdir -p "$root/vbrrun"
chmod 700 "$root/vbrrun"
cases=$((cases + 1))
vbridge_out=$(timeout 300 unshare -rmn --kill-child sh -c '
export XDG_RUNTIME_DIR="$2/vbrrun" HULL_CORE_IDLE=1
ip=/usr/sbin/ip
[ -x "$ip" ] || ip=/sbin/ip
mount --bind "$2/relay-resolv.conf" /etc/resolv.conf
timeout 260 python3 "$2/fakedns.py" 250 &
"$ip" link set lo up
"$ip" addr add 10.200.0.1/32 dev lo
timeout 200 python3 -c "
import socket
s = socket.socket()
s.bind((\"10.200.0.1\", 8081))
s.listen(1)
s.settimeout(190)
c, _ = s.accept()
c.sendall(b\"hello from beyond the bridge\\n\")
c.close()
" &
cd /
echo "== pair"; timeout 150 "$1" run -f "$2/vbridge2.conf" 2>&1
echo "== taps $("$ip" -br link | grep -c "^hv")"
"$ip" link add brtest type bridge && "$ip" link set brtest up
echo "== named"; timeout 100 "$1" run -f "$2/vbrnamed.conf" 2>&1
echo "== taps $("$ip" -br link | grep -c "^hv")"
' sh "$hull" "$root" 2>&1)
rm -rf "${root:?}/vbrrun"
vbweb_at=$(python3 -c '
import sys
h = 2166136261
for b in sys.argv[1].encode():
	h = ((h ^ b) * 16777619) & 0xffffffff
h %= 256 * 254
print("172.30.%d.%d" % (h // 254, h % 254 + 1))' vbweb)
printf '%s\n' "$vbridge_out" | grep -q "^vbweb *| addr=eth0 $(printf '%s' "$vbweb_at" | sed 's/\./\\./g')/15 up\$" &&
	printf '%s\n' "$vbridge_out" | grep -q '^vbweb *| dial 172\.31\.0\.1:9=Connection refused$' &&
	printf '%s\n' "$vbridge_out" | grep -q '^vbweb *| dial 10\.200\.0\.1:8081=hello from beyond the bridge$' &&
	printf '%s\n' "$vbridge_out" | grep -q '^vbweb *| read=nameserver 172\.31\.0\.1$' &&
	printf '%s\n' "$vbridge_out" | grep -q '^vbweb *| dns=anything\.example 192\.0\.2\.7$' &&
	printf '%s\n' "$vbridge_out" | grep -q "^vbweb *| dial $(printf '%s' "$vcdb_at" | sed 's/\./\\./g'):80=hello from the machine\$" ||
	fail "a vm on hull0, as root, was not addressed, routed out and on one wire with a chroot machine" "$vbridge_out"
cases=$((cases + 1))
printf '%s\n' "$vbridge_out" | grep -q '^addr=eth0 none/0 up$' &&
	[ "$(printf '%s\n' "$vbridge_out" | grep -c '^== taps 0$')" -eq 2 ] ||
	fail "a vm on a named bridge was not up and unaddressed, or its tap was left" "$vbridge_out"
# Appendix D's sibling names on a vm: a segment given a subnet is hull's
# to address, the agent setting the drawn address on the segment's NIC,
# eth1 after passt's, and binding every sibling's name over /etc/hosts; a
# datagram sent by name is answered by the sibling's own hostname.
vnamed="vnamed-$$"
printf '(machines (vweb %s) (vdb %s))\n' \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (segments ($vnamed (subnet \"10.86.0.0/24\"))) (command \"/probe\" \"addr\" \"eth1\" \"hosts\" \"vdb\" \"read\" \"/etc/resolv.conf\" \"ask\" \"vdb\" \"7000\" \"dns\" \"vdb\" \"dns\" \"hull-test.invalid\")" \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (segments ($vnamed (subnet \"10.86.0.0/24\"))) (command \"/probe\" \"addr\" \"eth1\" \"answer\" \"7000\")" \
	> "$root/vnamed.conf"
name=vnamed
boot vnamed 150
has 0 '^vweb +\| ask=vdb at 10\.86\.0\.[0-9]+: vdb$'
vdb_at=$(printf '%s\n' "$out" | sed -n 's#^vdb *| addr=eth1 \(10\.86\.0\.[0-9]*\)/24 up$#\1#p')
cases=$((cases + 1))
[ -n "$vdb_at" ] && printf '%s\n' "$out" | grep -q "^vweb *| hosts=vdb $vdb_at\$" ||
	fail "a vm was not named to its sibling at the sibling's own address" "$out"
# With passt too the vm asks hull at .1, which names its sibling and asks
# the host's resolvers the rest: a name outside answered, not REFUSED.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^vweb *| read=nameserver 10\.86\.0\.1$' &&
	printf '%s\n' "$out" | grep -q "^vweb *| dns=vdb $vdb_at\$" &&
	printf '%s\n' "$out" | grep -Eq '^vweb +\| dns=hull-test\.invalid rcode [0-4]$' ||
	fail "a vm with passt did not ask .1, for its sibling and, asked on, for a name outside" "$out"
# A chroot machine and a vm, both of (network none), so the vm's eth0 is
# its segment's and not passt's: each named to the other across the
# core's switch, and the chroot's datagram answered by the vm. A third, a
# vm of (network none) on a bare segment, has its eth0 left as the
# workload's: no address, not passt's, which the agent gave every eth0
# before the engine said on the command line whether passt's was there.
mixed="mixed-$$"
printf '(machines (cweb %s) (vdb %s) (vraw %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network none) (segments ($mixed (subnet \"10.87.0.0/24\") (subnet6 \"fd42:87::/64\"))) (command \"/probe\" \"addr\" \"eth0\" \"addr6\" \"eth0\" \"ask\" \"vdb\" \"7000\")" \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (network none) (segments ($mixed (subnet \"10.87.0.0/24\") (subnet6 \"fd42:87::/64\"))) (command \"/probe\" \"addr\" \"eth0\" \"addr6\" \"eth0\" \"hosts\" \"cweb\" \"read\" \"/etc/resolv.conf\" \"dns\" \"cweb\" \"dns6\" \"cweb\" \"ptr\" \"10.87.0.0\" \"ping6\" \"cweb\" \"answer\" \"7000\")" \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (network none) (segments \"vraw-$$\") (command \"/probe\" \"addr\" \"eth0\")" \
	> "$root/mixed.conf"
name=mixed
boot mixed 150
has 0 '^cweb +\| ask=vdb at 10\.87\.0\.[0-9]+: vdb$'
has 0 '^vdb +\| addr=eth0 10\.87\.0\.[0-9]+/24 up$'
has 0 '^vraw +\| addr=eth0 none/0 down$'
cweb_at=$(printf '%s\n' "$out" | sed -n 's#^cweb *| addr=eth0 \(10\.87\.0\.[0-9]*\)/24 up$#\1#p')
cases=$((cases + 1))
[ -n "$cweb_at" ] && printf '%s\n' "$out" | grep -q "^vdb *| hosts=cweb $cweb_at\$" &&
	printf '%s\n' "$out" | grep -q "^vdb *| answer=from $cweb_at\$" ||
	fail "a vm and a chroot machine were not named to each other across the switch" "$out"
# A vm of (network none) asks hull at its segment's .1, which names the
# chroot machine there; a reverse lookup of the subnet's own network is
# a name hull has not.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^vdb *| read=nameserver 10\.87\.0\.1$' &&
	printf '%s\n' "$out" | grep -q "^vdb *| dns=cweb $cweb_at\$" &&
	printf '%s\n' "$out" | grep -q '^vdb *| ptr=10\.87\.0\.0 rcode 3$' ||
	fail "a vm of (network none) was not named its sibling by hull at .1" "$out"
# The segment's IPv6 network as well: each given its IPv6 address on its
# NIC, the vm asking hull at .1 for the chroot machine's AAAA, which is
# that address, and reaching it over IPv6 across the switch.
cweb6=$(printf '%s\n' "$out" | sed -n 's#^cweb *| addr6=eth0 \(fd42:87:[0-9a-f:]*\)/64$#\1#p')
cases=$((cases + 1))
[ -n "$cweb6" ] && printf '%s\n' "$out" | grep -q '^vdb *| addr6=eth0 fd42:87:[0-9a-f:]*/64$' &&
	printf '%s\n' "$out" | grep -q "^vdb *| dns6=cweb $cweb6\$" &&
	printf '%s\n' "$out" | grep -q "^vdb *| ping=$cweb6 ok\$" ||
	fail "a vm and a chroot machine did not reach each other over IPv6" "$out"
# A segment of IPv6 alone: the vm's NIC up with its IPv6 address and no
# IPv4 one, hull at its network's ::1 its nameserver, asked there over UDP
# and TCP for the chroot machine's AAAA, which it then reaches; ::1 itself
# answering ping.
only6="only6-$$"
printf '(machines (c6 %s) (v6 %s))\n' \
	"(source \"dir:./rootfs\") (backend chroot) (mutable overlay) (network none) (segments ($only6 (subnet6 \"fd42:85::/64\"))) (command \"/probe\" \"addr6\" \"eth0\" \"sleep\" \"50\")" \
	"(source \"dir:./rootfs\") (backend vm) (mutable overlay) (network none) (segments ($only6 (subnet6 \"fd42:85::/64\"))) (command \"/probe\" \"addr6\" \"eth0\" \"read\" \"/etc/resolv.conf\" \"dns6\" \"c6\" \"dnstcp6\" \"c6\" \"ping6\" \"c6\" \"ping\" \"fd42:85::1\")" \
	> "$root/only6.conf"
name=only6
boot only6 150
has 0 '^v6 +\| read=nameserver fd42:85::1$'
c6at=$(printf '%s\n' "$out" | sed -n 's#^c6 *| addr6=eth0 \(fd42:85:[0-9a-f:]*\)/64$#\1#p')
cases=$((cases + 1))
[ -n "$c6at" ] && printf '%s\n' "$out" | grep -q '^v6 *| addr6=eth0 fd42:85:[0-9a-f:]*/64$' &&
	printf '%s\n' "$out" | grep -q "^v6 *| dns6=c6 $c6at\$" &&
	printf '%s\n' "$out" | grep -q "^v6 *| dnstcp6=c6 $c6at\$" &&
	printf '%s\n' "$out" | grep -q "^v6 *| ping=$c6at ok\$" &&
	printf '%s\n' "$out" | grep -q '^v6 *| ping=fd42:85::1 ok$' ||
	fail "a vm on a segment of IPv6 alone was not named and reached by hull at ::1" "$out"
timeout 10 "$hull" kill -s TERM anchor > /dev/null 2>&1
wait "$anchored"
# (init PATH): PID 1 is the image's init, here the probe as one, which
# reaps for ever, and the agent goes on beside it; the workload is not
# PID 1, and when it ends the image's /sbin/poweroff, the probe as one,
# ends the machine. Without one the kernel's power-off follows the grace.
mkdir -p "$dir/initroot/sbin" "$dir/initroot/proc" "$dir/initroot/dev" "$dir/initroot/sys" "$dir/initroot/run" \
	"$dir/initbare/sbin" "$dir/initbare/proc" "$dir/initbare/dev" "$dir/initbare/sys"
for d in initroot initbare; do
	cp "$probe" "$dir/$d/probe"
	cp "$probe" "$dir/$d/sbin/init"
done
cp "$probe" "$dir/initroot/sbin/poweroff"
printf '(machine (source "dir:./initroot") (backend vm) (mutable overlay) (init "/sbin/init") %s)\n' \
	'(memory "512M") (command "/probe" "target" "/proc/1/exe" "pid" "fsflags" "/run")' > "$root/inited.conf"
name=inited
started=$(date +%s)
boot inited
took=$(( $(date +%s) - started ))
has 0 '^target=/sbin/init$'
has 0 '^pid=([2-9]|[1-9][0-9]+)$'
# /run as a distribution's initramfs leaves it for its init: nosuid,
# nodev and noexec, a tenth of memory, which a unit's exec restriction
# rests on.
has 0 '^fsflags=nosuid,nodev,noexec size=(4[0-9]|5[0-9])$'
cases=$((cases + 1))
[ "$took" -lt 15 ] || fail "a machine handed to its init took ${took}s to end, its poweroff unused" "$out"
# Through an exec, which the agent beside the init serves, and a stop.
printf '(machine (source "dir:./initroot") (backend vm) (mutable overlay) (init "/sbin/init") %s)\n' \
	'(hostname "initexec") (command "/probe" "say" "ready" "wait")' > "$root/initexec.conf"
rm -f "$root/initexec.out"
(cd / && exec timeout 60 "$hull" run -f "$root/initexec.conf" > "$root/initexec.out" 2>&1) &
initexec=$!
tries=0
until grep -q '^ready$' "$root/initexec.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
xcase 0 '^target=/sbin/init$' "$hull" exec initexec /probe target /proc/1/exe
timeout 30 "$hull" stop initexec > /dev/null 2>&1
wait "$initexec"
# hull stats --guest: the machine's own view from inside (section 4.10,
# B.3's (sub (what stats))), against the host's whole cost. The agent reads
# the guest's /proc: cpu the guest kernel's busy time, memory the used, io 0
# since a 9p root has no block device. Both positive, and the guest's used
# memory below the host's whole qemu.
printf '(machine (source "dir:./rootfs") (backend vm) (hostname "vstats") %s)\n' \
	'(command "/probe" "say" "ready" "wait")' > "$root/vstats.conf"
rm -f "$root/vstats.out"
(cd / && exec timeout 60 "$hull" run -f "$root/vstats.conf" > "$root/vstats.out" 2>&1) &
vstats=$!
tries=0
until grep -q '^ready$' "$root/vstats.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
host_mem=$(timeout 30 "$hull" stats vstats 2>/dev/null | sed -n 's/^memory *\([0-9][0-9]*\) bytes$/\1/p')
guest_out=$(timeout 30 "$hull" stats --guest vstats 2>&1)
guest_mem=$(printf '%s\n' "$guest_out" | sed -n 's/^memory *\([0-9][0-9]*\) bytes$/\1/p')
cases=$((cases + 1))
printf '%s\n' "$guest_out" | grep -q '^cpu ' || fail "a guest's stats have no cpu line" "$guest_out"
cases=$((cases + 1))
{ [ -n "$guest_mem" ] && [ "$guest_mem" -gt 0 ]; } ||
	fail "a guest's used memory is not a positive figure" "$guest_out"
cases=$((cases + 1))
{ [ -n "$host_mem" ] && [ "$guest_mem" -lt "$host_mem" ]; } ||
	fail "a guest's used memory is not below the host's whole qemu" "guest=$guest_mem host=$host_mem"
timeout 30 "$hull" stop vstats > /dev/null 2>&1
wait "$vstats"
cases=$((cases + 1))
grep -q '^ready$' "$root/initexec.out" || fail "a machine handed to its init did not run" \
	"$(cat "$root/initexec.out")"
# No /sbin/poweroff: the kernel's, after the grace.
printf '(machine (source "dir:./initbare") (backend vm) (mutable overlay) (init "/sbin/init") %s)\n' \
	'(command "/probe" "say" "bare")' > "$root/initbare.conf"
name=initbare
started=$(date +%s)
boot initbare 90
took=$(( $(date +%s) - started ))
has 0 '^bare$'
cases=$((cases + 1))
[ "$took" -ge 18 ] && [ "$took" -lt 60 ] ||
	fail "a machine with no poweroff ended in ${took}s, not after the grace" "$out"
# An init that is not there is refused, and said.
printf '(machine (source "dir:./initroot") (backend vm) (mutable overlay) (init "/sbin/nothing") %s)\n' \
	'(command "/probe" "say" "never")' > "$root/noinit.conf"
name=noinit
boot noinit
has 125 'no init it can run'
# (layers ...) and (upper PATH) on a vm: the overlay is mounted host-side,
# in the user and mount namespace qemu runs in, and the guest's share is
# its merged tree. The layer wins, a write lands in the upper and a
# removal there as a whiteout, the template untouched, and a second
# machine sees what the first left.
rm -rf "${root:?}/vtemplate" "${root:?}/vsite" "${root:?}/vdata" "${root:?}/.vdata.hull-work"
mkdir -p "$root/vtemplate/proc" "$root/vtemplate/dev" "$root/vtemplate/sys" "$root/vsite" "$root/vdata"
cp "$probe" "$root/vtemplate/probe"
printf 'template\n' > "$root/vtemplate/a"
printf 'site\n' > "$root/vsite/a"
printf 'b\n' > "$root/vsite/b"
printf 'c\n' > "$root/vdata/c"
printf '(machine (source "dir:%s/vtemplate") (backend vm) (mutable overlay) %s %s)\n' "$root" \
	"(layers \"dir:$root/vsite\") (upper \"$root/vdata\")" \
	'(command "/probe" "read" "/proc/mounts" "read" "/a" "read" "/c" "write" "/d" "remove" "/b")' \
	> "$root/vlayered.conf"
name=vlayered
boot vlayered
# A kept (upper PATH) wants its writes on the host's disk, so the guest does
# not lay a tmpfs overlay of its own: its root is the merged 9p share written
# through to the host-side overlay's kept upper, not an overlayfs. Were it a
# guest overlay, /d below would land in a tmpfs and not in vdata. Its root,
# /proc/mounts' first line, is the share, 9p and not overlay.
has 0 '^read=root / 9p'
has 0 '^read=site$'
has 0 '^read=c$'
has 0 '^remove=ok$'
cases=$((cases + 1))
[ "$(cat "$root/vtemplate/a")" = template ] && [ -e "$root/vsite/b" ] && [ -e "$root/vdata/d" ] &&
	[ -c "$root/vdata/b" ] ||
	fail "a layered vm's writes did not land in its upper alone" \
		"$(ls -la "$root/vtemplate" "$root/vsite" "$root/vdata" 2>&1)"
printf '(machine (source "dir:%s/vtemplate") (backend vm) (mutable overlay) %s %s)\n' "$root" \
	"(layers \"dir:$root/vsite\") (upper \"$root/vdata\")" \
	'(command "/probe" "read" "/b" "read" "/d")' > "$root/vrelayered.conf"
name=vrelayered
boot vrelayered
has 0 '^read=No such file or directory$'
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read=No such file' && printf '%s\n' "$out" | grep -qv '^read=d' ||
	fail "a second layered vm did not see what the first left" "$out"
# (owners PATH) on a vm: a stored tree's owners given back in the overlay
# qemu mounts host-side, before the guest boots, where the user's
# subordinate range is mapped into qemu's namespace from outside by
# newuidmap. Owners ask for that host-side overlay themselves, as layers
# and an upper do, so (mutable overlay) alone is enough -- no layers. A
# file the checkout holds as the user's is the manifest's owner, 101:102,
# in the guest, and still the user's on the host's disk: the chown copies
# it up into the overlay's tmpfs, so no disk keeps the given owner. Where
# the host gives the user no range it stays root's, as on chroot, the
# refusal said on qemu's log. bbq-predictor found a package's uid 997
# root's in a guest before this.
rm -rf "${root:?}/vowntmpl"
mkdir -p "$root/vowntmpl/proc" "$root/vowntmpl/dev" "$root/vowntmpl/sys"
cp "$probe" "$root/vowntmpl/probe"
printf 'given\n' > "$root/vowntmpl/given"
chgrp "$(id -g)" "$root/vowntmpl/given"
printf '101\000102\000given\000' > "$root/vowners.list"
me=$(id -un)
if grep -q "^$me:" /etc/subuid 2>/dev/null && grep -q "^$me:" /etc/subgid 2>/dev/null &&
	[ -u /usr/bin/newuidmap ] && [ -u /usr/bin/newgidmap ]; then
	vowned_owner='^owner=101:102 0[0-7]{3}$'
else
	vowned_owner='^owner=0:0 0[0-7]{3}$'
fi
printf '(machine (source "dir:%s/vowntmpl") (backend vm) (mutable overlay) %s %s)\n' "$root" \
	"(owners \"$root/vowners.list\")" \
	'(command "/probe" "owner" "/given" "read" "/proc/mounts")' > "$root/vowned.conf"
name=vowned
boot vowned
has 0 "$vowned_owner"
# The host-side overlay owners force is a read-only lower the guest overlays
# itself, so its root is an overlayfs with a tmpfs upper, not the merged 9p
# share written through: writes are fast and in the guest's memory, and the
# module place can be made. /proc/mounts' first line is that root.
has 0 '^read=overlay / overlay'
cases=$((cases + 1))
[ "$(stat -c %u "$root/vowntmpl/given")" = "$(id -u)" ] ||
	fail "an owner given in a vm reached the user's disk" "$(ls -ln "$root/vowntmpl/given")"
# Refused in place, where an owner would be one on the user's own disk and
# there is no overlay tmpfs to hold it.
printf '(machine (source "dir:%s/vowntmpl") (backend vm) %s)\n' "$root" \
	"(owners \"$root/vowners.list\") (command \"/probe\" \"say\" \"no\")" > "$root/vownedno.conf"
name=vownedno
boot vownedno
has 125 "owners are given in the overlay.s tmpfs"
rm -rf "${root:?}/vowntmpl"
# (export PATH) on a vm, by a build whose steps run on one: each step an
# overlay qemu mounts host-side, the guest writing through to it, its tree
# taken there as a tar once the step ends -- so an owner a step gives is
# kept in the manifest, as on chroot, and given back to the next step on
# a vm, with no file of it on the user's disk. Where the host gives the
# user no range there is none to keep, as for owners above.
rm -rf "${root:?}/vbase"
mkdir -p "$root/vbase/bin" "$root/vbase/proc" "$root/vbase/dev" "$root/vbase/sys" \
	"$root/vbase/tmp"
cp "$probe" "$root/vbase/bin/sh"
# Named for this run, so that the store a suite before it left does not
# hand back both steps cached and the second never runs to look.
cat > "$root/vbuilt.conf" <<BUILD
(build (base "dir:./vbase")
  (steps (step (name "give-$$") (backend vm) (run "probe write /given chown /given 101"))
         (step (name "look-$$") (backend vm) (run "probe owner /given"))))
BUILD
if [ "$vowned_owner" = '^owner=101:102 0[0-7]{3}$' ]; then
	cases=$((cases + 1))
	out=$(cd "$root" && timeout 300 "$hull" build -f vbuilt.conf app/vbuilt 2>&1)
	got=$?
	[ "$got" = 0 ] && printf '%s\n' "$out" | grep -q '^owner=101:101 ' ||
		fail "a vm step's owner was not kept for the next" "$out"
	cases=$((cases + 1))
	out=$(timeout 30 "$hull" manifest show app/vbuilt 2>&1)
	printf '%s\n' "$out" | grep -q '(owner (gid 101) (uid 101)) (path "given")' ||
		fail "a vm step's owner is not in the manifest" "$out"
else
	printf 'qemu: this user has no subordinate range, so the vm export case is skipped\n'
fi
rm -rf "${root:?}/vbase"
# A volume that is a stack of its own, at /stk, staged host-side as the
# root's is and shared: the layer wins, writes and a whiteout in its upper
# alone; with no upper its writes are memory; read-only, it is written to
# by nothing.
rm -rf "${root:?}/vdata" "${root:?}/.vdata.hull-work"
mkdir -p "$root/vdata"
printf 'c\n' > "$root/vdata/c"
conf vstack '(backend vm) (mutable overlay)' \
	"(volumes (volume (guest \"/stk\") (layers \"dir:$root/vtemplate\" \"dir:$root/vsite\") (upper \"$root/vdata\")))" \
	'(command "/probe" "read" "/stk/a" "read" "/stk/c" "write" "/stk/d" "remove" "/stk/b")'
name=vstack
boot vstack
has 0 '^read=site$'
has 0 '^read=c$'
has 0 '^remove=ok$'
cases=$((cases + 1))
[ "$(cat "$root/vtemplate/a")" = template ] && [ -e "$root/vsite/b" ] && [ -e "$root/vdata/d" ] &&
	[ -c "$root/vdata/b" ] && [ ! -e "$root/rootfs/stk" ] ||
	fail "a vm's stacked volume's writes did not land in its upper alone" \
		"$(ls -la "$root/vtemplate" "$root/vsite" "$root/vdata" 2>&1)"
# What overlayfs left in the work directory beside the upper, mode 000,
# is given back to the user, who could not otherwise remove it.
cases=$((cases + 1))
[ -x "$root/.vdata.hull-work/work" ] ||
	fail "a kept upper's work directory was left unremovable" "$(ls -la "$root/.vdata.hull-work" 2>&1)"
conf vstackmem '(backend vm) (mutable overlay)' \
	"(volumes (volume (guest \"/stk\") (layers \"dir:$root/vtemplate\" \"dir:$root/vsite\")))" \
	'(command "/probe" "write" "/stk/e" "read" "/stk/a")'
name=vstackmem
boot vstackmem
has 0 '^write=ok$'
has 0 '^read=site$'
cases=$((cases + 1))
[ ! -e "$root/vtemplate/e" ] && [ ! -e "$root/vsite/e" ] && [ ! -e "$root/vdata/e" ] ||
	fail "a vm's stacked volume's write in memory reached a disk" ""
conf vstackro '(backend vm) (mutable overlay)' \
	"(volumes (volume (guest \"/stk\") (layers \"dir:$root/vtemplate\" \"dir:$root/vsite\") (mode ro)))" \
	'(command "/probe" "read" "/stk/a" "write" "/stk/f")'
name=vstackro
boot vstackro
has 0 '^read=site$'
has 0 '^write=Read-only file system$'
rm -rf "${root:?}/vtemplate" "${root:?}/vsite" "${root:?}/vdata" "${root:?}/.vdata.hull-work"
# A segment's port is the core's, so a machine on one cannot outlive it.
conf keptseg "(backend vm) (on_disconnect keep) (segments \"$wire\")" '(command "/probe" "say" "no")'
name=keptseg
boot keptseg
has 125 'cannot outlive the core'
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
# A host port below the lowest a user may bind is refused before the
# machine boots, naming the setting and the way round.
lowest=$(cat /proc/sys/net/ipv4/ip_unprivileged_port_start 2>/dev/null)
if [ "${lowest:-0}" -gt 80 ]; then
	conf lowport '(backend vm) (ports (port (host 80) (guest 80))) (command "/probe" "say" "never")'
	name=lowport
	boot lowport
	has 125 "port 80 on .*: below $lowest, the lowest a user may bind .*: give a host port of $lowest or more"
fi

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
# (ssh true): the agent supervises the image's own sshd on the guest's port
# 22. A root of its own carries a stand-in sshd (the probe run as sshd,
# which records how hull invoked it in /sshd.stub and answers the SSH
# banner), so this checks hull's whole job -- generate a host key, write the
# user's authorized_keys and a config, find the image's sshd and start it on
# 22 -- without a real OpenSSH in the guest. proxy-ssh reaches the banner;
# the machine carries the three files the agent placed; the stub was started
# with the host key.
# The host key is private, so the core keeps the ssh material in memory and
# refuses a runtime directory that is not a tmpfs, as it does for secrets.
# $BUILD_DIR is on disk here, so these cases run a core of their own under a
# tmpfs in /dev/shm, as cli_test's secrets cases do. Both roots carry /run
# for the material's tmpfs and proc/dev/sys as mountpoints.
sshrt=$(mktemp -d /dev/shm/hull-qssh.XXXXXX 2>/dev/null)
if [ -z "$sshrt" ]; then
	printf 'qemu: ssh cases not tried: no tmpfs under /dev/shm for the ssh material\n'
else
	chmod 700 "$sshrt"
	rm -rf "$root/sshroot" "$root/nosshroot"
	for d in sshroot nosshroot; do
		mkdir -p "$root/$d/run" "$root/$d/proc" "$root/$d/dev" "$root/$d/sys"
		cp "$probe" "$root/$d/probe"
	done
	mkdir -p "$root/sshroot/usr/sbin"
	cp "$probe" "$root/sshroot/usr/sbin/sshd"
	printf '(machine (source "dir:./sshroot") (backend vm) (hostname "qssh") (ssh true) %s)\n' \
		'(command "/probe" "say" "ready" "wait")' > "$root/qssh.conf"
	rm -f "$root/qssh.out"
	(cd / && exec env XDG_RUNTIME_DIR="$sshrt" timeout -k 2 90 "$hull" run -f "$root/qssh.conf" \
		> "$root/qssh.out" 2>&1) &
	runner=$!
	tries=0
	until grep -q '^ready$' "$root/qssh.out" 2>/dev/null || [ "$tries" -ge 60 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	banner=$(cd / && XDG_RUNTIME_DIR="$sshrt" timeout 20 "$hull" proxy-ssh qssh < /dev/null 2>&1 | head -1)
	akeys=$(XDG_RUNTIME_DIR="$sshrt" timeout 20 "$hull" exec qssh /probe read /run/hull/ssh/authorized_keys 2>&1)
	invoked=$(XDG_RUNTIME_DIR="$sshrt" timeout 20 "$hull" exec qssh /probe read /sshd.stub 2>&1)
	# hull ssh prefers the machine's own sshd where (ssh true) gave it one:
	# on a terminal of its own (script), it reaches the stub over proxy-ssh
	# rather than giving the agent's exec shell. The stub speaks the banner
	# and closes, so the ssh client reports the connection, not a shell.
	sshout=$(HULL="$hull" XDG_RUNTIME_DIR="$sshrt" timeout 30 \
		script -qec '"$HULL" ssh qssh' /dev/null 2>&1 < /dev/null | tr -d '\r')
	XDG_RUNTIME_DIR="$sshrt" timeout 30 "$hull" stop qssh > /dev/null 2>&1
	wait "$runner"
	cases=$((cases + 1))
	case $banner in
	SSH-2.0-hull-stub*) ;;
	*) fail "(ssh true) did not start the image's sshd on the guest's port 22" \
		"$banner; $(cat "$root/qssh.out")" ;;
	esac
	cases=$((cases + 1))
	case $akeys in
	*hulltestkey*) ;;
	*) fail "(ssh true) did not write the user's authorized_keys into the machine" "$akeys" ;;
	esac
	cases=$((cases + 1))
	case $invoked in
	*host_key*) ;;
	*) fail "the agent did not start the image's sshd with the host key" "$invoked" ;;
	esac
	cases=$((cases + 1))
	case $sshout in
	*SSH-2.0-hull-stub*|*"Connection closed"*|*kex_exchange_identification*) ;;
	*) fail "hull ssh did not use the machine's own sshd for an (ssh true) machine" "$sshout" ;;
	esac
	# The host key is machine state: a second run reuses the one kept under
	# the data home, so repeated ssh does not trip known_hosts (8.1 item 2).
	keyfile="$root/data/hull/machines/qssh/host_key"
	cases=$((cases + 1))
	if [ -f "$keyfile" ]; then
		key1=$(sha256sum "$keyfile" | cut -d' ' -f1)
		rm -f "$root/qssh.out"
		(cd / && exec env XDG_RUNTIME_DIR="$sshrt" timeout -k 2 90 "$hull" run -f "$root/qssh.conf" \
			> "$root/qssh.out" 2>&1) &
		runner=$!
		tries=0
		until grep -q '^ready$' "$root/qssh.out" 2>/dev/null || [ "$tries" -ge 60 ]; do
			tries=$((tries + 1))
			sleep 0.25
		done
		key2=$(sha256sum "$keyfile" | cut -d' ' -f1)
		XDG_RUNTIME_DIR="$sshrt" timeout 30 "$hull" stop qssh > /dev/null 2>&1
		wait "$runner"
		[ "$key1" = "$key2" ] ||
			fail "(ssh true) did not reuse the machine's host key on a second run" \
				"run1 $key1, run2 $key2"
	else
		fail "(ssh true) kept no persistent host key for the machine" "$keyfile"
	fi
	# hull ssh also finds an image's OWN sshd, with no (ssh true) and so no
	# persistent key: a machine whose workload is the stub sshd serving :22.
	# hull ssh probes the port, finds it, and runs ssh (unpinned, the user's
	# own known_hosts) rather than the exec shell. The stub speaks the banner
	# and closes, so the ssh client reports the connection.
	printf '(machine (source "dir:./sshroot") (backend vm) (hostname "qimg") %s)\n' \
		'(command "/usr/sbin/sshd")' > "$root/qimg.conf"
	rm -f "$root/qimg.out"
	(cd / && exec env XDG_RUNTIME_DIR="$sshrt" timeout -k 2 90 "$hull" run -f "$root/qimg.conf" \
		> "$root/qimg.out" 2>&1) &
	runner=$!
	tries=0
	until XDG_RUNTIME_DIR="$sshrt" timeout 20 "$hull" exec qimg /probe kind /sshd.stub 2>/dev/null |
	      grep -q '^kind=file$' || [ "$tries" -ge 60 ]; do
		tries=$((tries + 1))
		sleep 0.25
	done
	imgout=$(HULL="$hull" XDG_RUNTIME_DIR="$sshrt" timeout 30 \
		script -qec '"$HULL" ssh qimg' /dev/null 2>&1 < /dev/null | tr -d '\r')
	XDG_RUNTIME_DIR="$sshrt" timeout 30 "$hull" stop qimg > /dev/null 2>&1
	wait "$runner"
	cases=$((cases + 1))
	case $imgout in
	*SSH-2.0-hull-stub*|*"Connection "*|*kex_exchange_identification*|*ssh_dispatch_run_fatal*) ;;
	*) fail "hull ssh did not find an image's own sshd with no (ssh true)" "$imgout" ;;
	esac
	# And refused when the image has no sshd: (ssh true) must fail at start,
	# not run the machine silently without one (section 6). nosshroot has
	# /run, so the material is placed and the refusal is for the sshd alone.
	printf '(machine (source "dir:./nosshroot") (backend vm) (hostname "qnossh") (ssh true) %s)\n' \
		'(command "/probe" "say" "ready")' > "$root/qnossh.conf"
	noout=$(cd / && XDG_RUNTIME_DIR="$sshrt" timeout -k 2 90 "$hull" run -f "$root/qnossh.conf" 2>&1)
	nocode=$?
	cases=$((cases + 1))
	{ [ "$nocode" != 0 ] && printf '%s\n' "$noout" | grep -q 'has no sshd'; } ||
		fail "(ssh true) on an image with no sshd was not refused at start" "$nocode: $noout"
	rm -rf "$sshrt"
fi
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
others=$(passts_now)
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
left=$(passts_new "$others")
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

# The workload's own output on the same credit, since 2026-10-29: a run
# read seven seconds late gets all 8 MB, eight times the log, with
# nothing said lost, where it had lost all but the last MiB or so; and
# while a run is held, the machine answers an exec, the agent reading
# every other channel, and a stop ends it at once.
conf vheld '(backend vm) (mutable overlay) (network none) (command "/probe" "spew" "8000000")'
want=$("$probe" spew 8000000 | "$probe" count)
cases=$((cases + 1))
got_held=$(timeout -k 2 120 "$hull" run -f "$root/vheld.conf" 2> "$root/vheld.err" |
	(sleep 7; "$probe" count))
[ "$got_held" = "$want" ] && ! grep -q 'output lost' "$root/vheld.err" ||
	fail "a vm's run read late lost its output" "$got_held, not $want: $(cat "$root/vheld.err")"
conf vstuck '(backend vm) (mutable overlay) (network none) (hostname "vstuck") (command "/probe" "spew" "50000000")'
# Into a fifo opened here and never read, then closed: the run's next
# write fails, and it ends then, rather than when a reader would.
rm -f "$root/vstuck.fifo"
mkfifo "$root/vstuck.fifo"
(timeout -k 2 90 "$hull" run -f "$root/vstuck.conf" > "$root/vstuck.fifo" 2> /dev/null) &
vstuckrun=$!
exec 8< "$root/vstuck.fifo"
tries=0
until "$hull" ps 2>/dev/null | grep -q '^vstuck .*running$' || [ "$tries" -ge 120 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
sleep 2
cases=$((cases + 1))
started=$(date +%s)
out=$(timeout 20 "$hull" exec vstuck /probe say answered 2>&1)
took=$(($(date +%s) - started))
[ "$out" = answered ] && [ "$took" -lt 5 ] ||
	fail "a vm whose run was held did not answer an exec" "$out after ${took}s"
cases=$((cases + 1))
started=$(date +%s)
"$hull" stop --timeout 5 vstuck > /dev/null 2>&1
took=$(($(date +%s) - started))
[ "$took" -lt 4 ] && ! "$hull" ps 2>/dev/null | grep -q '^vstuck .*running$' ||
	fail "a vm held by a run that never read was not stopped at once" "${took}s"
exec 8<&-
wait "$vstuckrun" 2> /dev/null
rm -f "$root/vstuck.fifo"

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

# A vm paused: qemu stopped whole, so a command in the guest ticks no
# more while it is and goes on after; the machine is shown paused.
(timeout 60 "$hull" exec qexec /probe ticks 300 > "$root/qticks.out" 2>&1) &
ticker=$!
qticks() { grep -c '^tick=' "$root/qticks.out"; }
tries=0
until [ "$(qticks)" -ge 3 ] || [ "$tries" -ge 80 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
xcase 0 '' "$hull" pause qexec
xcase 0 '^qexec +127\.107\.[0-9.]+ +qemu +vm +paused$' "$hull" ps
sleep 0.5
before=$(qticks)
sleep 1.5
cases=$((cases + 1))
[ "$(qticks)" = "$before" ] || fail "a paused vm went on ticking" "$before, then $(qticks)"
xcase 0 '' "$hull" unpause qexec
sleep 1.5
cases=$((cases + 1))
[ "$(qticks)" -ge $((before + 5)) ] || fail "an unpaused vm did not go on" "$before, then $(qticks)"
kill "$ticker" 2>/dev/null
wait "$ticker" 2>/dev/null

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

# A machine not kept dies with its core: nothing could manage it after.
conf kept '(backend vm) (hostname "qkept") (command "/probe" "wait")'
cases=$((cases + 1))
others=$(passts_now)
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
	left=$(passts_new "$others")
	[ -z "$left" ] || fail "a vm's passt outlived its core" "$(ps -o pid,args -p "$left")"
fi
wait "$runner" 2>/dev/null
# Its directory is left, the core gone: the next core's first machine
# removes it, which the last case requires.

# A kept one outlives a core killed outright, and the core `hull ps`
# starts takes it back: what the workload wrote while no core held it, an
# exec, its port forwarded again, and its exit; then nothing left. Before
# that, a host that cannot give the token is not believed, and the agent
# listens on for the real one. The workload writes `during` two seconds
# in, while no core holds it, as the test waits four after the kill; it
# then waits up to 20 s to be served on its port, and 12 s more, for an
# exec and a second machine's boot.
conf adopt '(backend vm) (hostname "qadopt") (on_disconnect keep)' \
	'(ports (port (host 18097) (guest 97)))' \
	'(command "/probe" "say" "before" "sleep" "2" "say" "during" "serve" "tcp" "97" "say" "after" "sleep" "12" "exit" "5")'
(cd / && exec timeout -k 2 90 "$hull" run -f "$root/adopt.conf" > /dev/null 2>&1) &
runner=$!
await qadopt
cases=$((cases + 1))
kept_file=$XDG_RUNTIME_DIR/hull/machines/qadopt
[ -f "$kept_file" ] || fail "a kept vm has no file for the next core" "$(ls "$XDG_RUNTIME_DIR/hull")"
kill -KILL "$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock")" 2> /dev/null
wait "$runner" 2>/dev/null
sleep 4
cases=$((cases + 1))
left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
[ -n "$left" ] || fail "a kept vm died with its core" ""
cid=$(grep -ao 'cid [0-9]*' "$kept_file" | cut -d' ' -f2)
cases=$((cases + 1))
out=$(timeout 20 python3 -c '
import socket, struct, sys
s = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
s.settimeout(10)
s.connect((int(sys.argv[1]), 1))
def say(m):
	s.sendall(struct.pack(">I", len(m)) + m)
# The agent may close as soon as it has read the false token, before the
# exec is sent: a closed connection, whenever it closes, is the answer.
try:
	say(b"(5:hello(2:ch1:0)(8:features)(5:proto1:1))")
	say(b"(6:resume(2:ch1:0)(5:token32:" + b"x" * 32 + b"))")
	say(b"(4:exec(4:argv6:/probe)(2:ch1:7)(3:tty5:false))")
except (BrokenPipeError, ConnectionResetError):
	pass
got = b""
while True:
	try:
		piece = s.recv(4096)
	except ConnectionResetError:
		break
	if not piece:
		break
	got += piece
# Told nothing but hello: not started, nothing the workload wrote, no exit.
leaked = any(word in got for word in (b"started", b"data", b"exited"))
print("believed" if leaked else "closed", repr(got[:200]))
' "$cid" 2>&1)
printf '%s\n' "$out" | grep -q '^closed ' || fail "a host without the token was believed" "$out"
cases=$((cases + 1))
out=$(timeout 60 "$hull" ps 2>&1)
printf '%s\n' "$out" | grep -q '^qadopt .*running$' &&
	grep -q '^hull core: machine qadopt is taken back$' "$XDG_RUNTIME_DIR/hull/core.log" ||
	fail "the next core did not take the kept vm back" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
at=$(address_of qadopt)
out=$(timeout 30 python3 -c '
import socket, sys, time
deadline = time.time() + 20
while time.time() < deadline:
	# Until the machine greets: before its serve listens, the forwarded
	# connection is refused inside, and the host side just closes.
	try:
		s = socket.create_connection((sys.argv[1], 18097), timeout=5)
		if s.recv(100).startswith(b"hello from the machine"):
			s.sendall(b"hi again\n")
			s.close()
			print("sent")
			break
		s.close()
	except OSError:
		pass
	time.sleep(0.2)
' "$at" 2>&1)
sent=$out
cases=$((cases + 1))
out=$(timeout 30 "$hull" exec qadopt /probe say "in it" 2>&1)
[ "$out" = "in it" ] || fail "an exec in a taken-back vm said '$out'" ""
# A machine started meanwhile sweeps dead cores' directories, and the
# one the taken-back machine uses, named for the core that went, is not
# one of them.
conf brief2 '(backend vm) (command "/probe" "say" "brief")'
name=brief2
boot brief2
has 0 '^brief$'
cases=$((cases + 1))
adopted_dir=$(grep -a '^dir ' "$kept_file" | cut -d' ' -f2-)
[ -f "$adopted_dir/console.log" ] ||
	fail "a taken-back vm's directory was swept" "$adopted_dir: $(ls "$adopted_dir" 2>&1)"
cases=$((cases + 1))
logs=$(timeout 30 "$hull" logs -f qadopt 2>&1)
[ "$sent" = sent ] && printf '%s\n' "$logs" | grep -q '^during$' &&
	printf '%s\n' "$logs" | grep -q '^served=hi again$' && printf '%s\n' "$logs" | grep -q '^after$' ||
	fail "a taken-back vm's port or what it wrote meanwhile was lost" "$sent; $logs"
cases=$((cases + 1))
out=$(timeout 30 "$hull" attach qadopt 2>&1)
got=$?
[ "$got" = 5 ] || fail "a taken-back vm's exit was $got, wanted 5" "$out"
cases=$((cases + 1))
sleep 2
left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
[ ! -e "$kept_file" ] && [ -z "$left" ] ||
	fail "a taken-back vm's file or qemu is left" "$(ls "$XDG_RUNTIME_DIR/hull/machines") $left"
# A kept vm whose workload ends while no core holds it waits for the next
# core, and its exit is that core's to report: 7 here, a second after its
# core is killed.
conf ended '(backend vm) (hostname "qended") (on_disconnect keep)' \
	'(command "/probe" "say" "up" "sleep" "2" "exit" "7")'
(cd / && exec timeout -k 2 60 "$hull" run -f "$root/ended.conf" > /dev/null 2>&1) &
runner=$!
await qended
kill -KILL "$(head -1 "$XDG_RUNTIME_DIR/hull/core.lock")" 2> /dev/null
wait "$runner" 2>/dev/null
sleep 4
cases=$((cases + 1))
out=$(timeout 60 "$hull" attach qended 2>&1)
got=$?
[ "$got" = 7 ] || fail "a kept vm that ended with no core exited $got, wanted 7" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
cases=$((cases + 1))
sleep 2
left=$(pgrep -f "^qemu-system-x86_64 .*$XDG_RUNTIME_DIR/hull/qemu-")
[ -z "$left" ] && [ ! -e "$XDG_RUNTIME_DIR/hull/machines/qended" ] ||
	fail "a kept vm that ended with no core was left" "$left"

# A kept file whose pid is not its machine's qemu, as after the pid has
# been given to another process, is forgotten, and that process is let
# be: here a sleep of the test's own, read by a core that starts once the
# last has left, 10 s at most.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
sleep 30 &
stranger=$!
state=$(printf 'pid %s\ncid 3\npasst 0\ndir %s/hull/qemu-1-1\ntoken %064d\n' \
	"$stranger" "$XDG_RUNTIME_DIR" 0)
printf '(4:kept(7:machine6:qfaked)(6:engine4:qemu)(5:class2:vm)(5:state%d:%s))' \
	"${#state}" "$state" > "$XDG_RUNTIME_DIR/hull/machines/qfaked"
cases=$((cases + 1))
out=$(timeout 30 "$hull" ps 2>&1)
! printf '%s\n' "$out" | grep -q '^qfaked ' &&
	grep -q '^hull core: machine qfaked is forgotten: its qemu is gone$' "$XDG_RUNTIME_DIR/hull/core.log" &&
	kill -0 "$stranger" 2> /dev/null ||
	fail "a kept file naming another process was believed, or it was harmed" "$out
$(cat "$XDG_RUNTIME_DIR/hull/core.log")"
kill "$stranger" 2> /dev/null
wait "$stranger" 2> /dev/null

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
left=$left$(passts_new "$others_at_start")
dirs=$(ls -d "$XDG_RUNTIME_DIR"/hull/qemu-* 2>/dev/null)
[ -z "$left" ] && [ -z "$dirs" ] || fail "a vm or its directory was left" "$left $dirs"

printf 'qemu: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
