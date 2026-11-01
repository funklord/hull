#!/bin/sh
# rootvm_test.sh -- root mode's native grants (project.md section 7), with
# hull-root really setuid root: inside a vm of hull's own, whose guest
# kernel's root is the only root this needs, so nothing on the host is
# given root. Not part of `make check`, which needs no KVM: `make
# check-root` runs it.
#
# Its arguments are build/hull, build/hull-root and the static probe. A
# rootfs is made in $BUILD_DIR/test/rootvm from the host's own binaries --
# hull, hull-root, a shell and the few tools the guest's script uses -- and
# the libraries ldd names for each; the vm boots it with no_new_privs off,
# so that a setuid bit means what it says. In the guest, as its root, the
# script installs hull-root setuid on a tmpfs, makes /etc/hull and
# /var/log/hull root's alone, and then as the user `tester` asks for a
# chroot machine on hull0 by root mode, and calls hull-root directly with
# what it must refuse. Every line the guest prints is checked here.
#
# The vm runs under `timeout`; the guest's script has no loop but the one
# over /proc, which ends with the list. It leaves vm.conf, named in FILES,
# in $BUILD_DIR/test/rootvm, its rootfs removed.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
helper=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
probe=$3
dir=${BUILD_DIR:?}/test/rootvm
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
# A core of the test's own, as in cli_test.sh.
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'rootvm: needs /dev/kvm and /dev/vhost-vsock open to this user\n'
	exit 1
fi

# The rootfs: each binary at the path the guest looks for it, and the
# libraries it links, where ldd says they are. Made anew, and removed at
# the end: what is in it is whatever ldd named, which nothing could list
# beforehand, and it is this suite's own directory, under its BUILD_DIR.
rootfs=$root/rootfs
case $rootfs in
*/test/rootvm/rootfs) rm -rf -- "$rootfs" ;;
*) printf 'rootvm: %s is not this suite'"'"'s rootfs\n' "$rootfs"; exit 1 ;;
esac
mkdir -p "$rootfs/bin" "$rootfs/sbin" "$rootfs/usr/sbin" "$rootfs/etc" "$rootfs/tmp" \
	"$rootfs/proc" "$rootfs/sys" "$rootfs/dev" "$rootfs/srv/mroot/proc" "$rootfs/srv/mroot/dev" \
	"$rootfs/srv/mroot/sys" "$rootfs/srv/mroot/etc"
place() { # FROM TO: a binary, and every library it links
	cp -L "$1" "$rootfs$2" || return 1
	for lib in $(ldd "$1" 2> /dev/null | grep -oE '/[^ ]+\.so[^ ]*'); do
		mkdir -p "$rootfs$(dirname "$lib")"
		[ -e "$rootfs$lib" ] || cp -L "$lib" "$rootfs$lib"
	done
}
missing=""
for tool in sh cp chown chmod mkdir cat sed mount setpriv ip env unshare sleep tr head; do
	at=$(command -v "$tool" 2> /dev/null || ls /usr/sbin/$tool /sbin/$tool 2> /dev/null | head -1)
	case $at in
	/*) place "$at" "/bin/$tool" ;;
	*) missing="$missing $tool" ;;
	esac
done
for tool in nft modprobe; do
	at=$(ls /usr/sbin/$tool /sbin/$tool 2> /dev/null | head -1)
	[ -n "$at" ] && place "$(readlink -f "$at")" "/usr/sbin/$tool" ||
		missing="$missing $tool"
done
if [ -n "$missing" ]; then
	printf 'rootvm: the host has none of:%s, which the guest needs\n' "$missing"
	exit 1
fi
cp "$rootfs/usr/sbin/modprobe" "$rootfs/sbin/modprobe"
place "$hull" /bin/hull
place "$helper" /bin/hull-root
cp "$probe" "$rootfs/srv/mroot/probe"
printf 'root:x:0:0::/root:/bin/sh\ntester:x:1000:1000::/tmp:/bin/sh\n' > "$rootfs/etc/passwd"
printf 'root:x:0:\ntester:x:1000:\n' > "$rootfs/etc/group"

# A plan of hand: a machine on a bridge the host has not, and its root
# hash as hull's planner and hull-root work it out, for the cases that
# call hull-root directly.
handmade='(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(6:bridge6:nosuch)(7:network6:bridge)))'
handmade_hash=sha256:$(printf 'hull root 1\nchroot\000ns\000(7:machine(6:bridge6:nosuch)(7:network6:bridge))' |
	sha256sum | cut -c1-64)
printf '%s' "$handmade" > "$rootfs/srv/handmade.plan"

cat > "$rootfs/srv/run.sh" <<EOF
modprobe -a bridge veth nf_tables nft_chain_nat nft_masq && echo modules=ok
mkdir -p /opt /run/t /etc/hull /var/log/hull
mount -t tmpfs none /opt
cp /bin/hull-root /opt/hull-root && chown 0:0 /opt/hull-root && chmod 4755 /opt/hull-root
chown 0:0 /etc/hull /var/log/hull && chmod 755 /etc/hull && chmod 700 /var/log/hull
: > /etc/hull/root.conf
chmod 644 /etc/hull/root.conf
printf 'nameserver 10.0.2.3\n' > /etc/resolv.conf
chown 1000:1000 /run/t && chmod 700 /run/t
cp -r /srv/mroot /tmp/mroot && chown -R 1000:1000 /tmp/mroot
printf '(machine (source "dir:/tmp/mroot") (backend chroot) (mutable overlay) (network bridge) (command "/probe" "addr" "eth0" "ping" "172.31.0.1"))\n' > /tmp/b.conf
chown 1000:1000 /tmp/b.conf
as() { setpriv --reuid 1000 --regid 1000 --clear-groups env XDG_RUNTIME_DIR=/run/t HULL_CORE_IDLE=1 HULL_ROOT_HELPER=/opt/hull-root "\$@"; }
cd /
token=\$(as /bin/hull plan --root-hash -f /tmp/b.conf 2> /dev/null | sed -n 's/.*: --accept //p')
echo "token=\$token"
echo "== unlisted"
as /bin/hull run --accept "\$token" -f /tmp/b.conf 2>&1
echo "status=\$?"
printf 'tester %s network.bridge\n' "\${token#root:}" > /etc/hull/root.conf
echo "== listed"
as /bin/hull run --accept "\$token" -f /tmp/b.conf 2>&1
echo "status=\$?"
echo "== left=\$(ip -br link | sed -n 's/^\(hb[0-9]*\) .*/\1/p' | head -1)"
# A process of the user's in network and user namespaces of its own, as a
# machine's init is, for hull-root to be asked about directly.
as unshare -rn sleep 20 &
sleep 1
target=""
for p in /proc/[0-9]*; do
	case \$(cat \$p/cmdline 2> /dev/null | tr '\000' ' ') in
	"sleep 20 ") target=\${p#/proc/} ;;
	esac
done
echo "target=\$target"
printf 'tester %s network.bridge\n' "$handmade_hash" >> /etc/hull/root.conf
echo "== another plan's bytes"
printf '(4:plan(5:class2:ns)(6:engine6:chroot)(7:machine(7:network6:bridge)))' | as /opt/hull-root bridge "$handmade_hash" "\$target" 2>&1
echo "status=\$?"
echo "== not the user's"
as /opt/hull-root bridge "$handmade_hash" "\$\$" < /srv/handmade.plan 2>&1
echo "status=\$?"
echo "== no such bridge"
as /opt/hull-root bridge "$handmade_hash" "\$target" < /srv/handmade.plan 2>&1
echo "status=\$?"
echo "== listing not root's"
chown 1000:1000 /etc/hull/root.conf
as /bin/hull run --accept "\$token" -f /tmp/b.conf 2>&1
echo "status=\$?"
echo "== log"
cat /var/log/hull/root.log
EOF

printf '(machine (source "dir:./rootfs") (backend vm) (mutable overlay) (security (no_new_privs false)) (command "/bin/sh" "/srv/run.sh"))\n' \
	> "$root/vm.conf"
out=$(cd / && timeout -k 2 300 "$hull" run -f "$root/vm.conf" 2>&1)
got=$?
token=$(printf '%s\n' "$out" | sed -n 's/^token=root://p')

cases=$((cases + 1))
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -q '^modules=ok$' &&
	printf '%s\n' "$token" | grep -Eq '^sha256:[0-9a-f]{64}$' ||
	fail "the guest did not run its script, or had no token" "exit $got
$out"
# Unlisted, refused before anything is done, with the line that lists it.
cases=$((cases + 1))
printf '%s\n' "$out" | sed -n '/^== unlisted/,/^status=/p' | grep -qx "tester $token network.bridge" &&
	printf '%s\n' "$out" | sed -n '/^== unlisted/,/^status=/p' | grep -qx 'status=125' ||
	fail "an unlisted root machine was not refused with its listing line" "$out"
# Listed, the user's chroot machine on hull0, its gateway answering; and
# nothing of its left on the host's side when it has gone.
cases=$((cases + 1))
listed=$(printf '%s\n' "$out" | sed -n '/^== listed/,/^status=/p')
printf '%s\n' "$listed" | grep -Eq '^addr=eth0 172\.(30|31)\.[0-9]+\.[0-9]+/15 up$' &&
	printf '%s\n' "$listed" | grep -qx 'ping=172.31.0.1 ok' &&
	printf '%s\n' "$listed" | grep -qx 'status=0' &&
	printf '%s\n' "$out" | grep -qx '== left=' ||
	fail "a listed root machine was not put on hull0, or its veth was left" "$out"
# hull-root, asked directly: the bytes of another plan than the hash
# listed, a process not a machine of the user's, and a bridge the host
# has not are each refused; the listing not root's alone, too.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq '^target=[0-9]+$' &&
	printf '%s\n' "$out" | sed -n "/^== another plan's bytes/,/^status=/p" | grep -q 'the plan on stdin is not the plan asked for' &&
	printf '%s\n' "$out" | sed -n "/^== not the user's/,/^status=/p" | grep -q 'not a machine of this user.s' &&
	printf '%s\n' "$out" | sed -n '/^== no such bridge/,/^status=/p' | grep -q 'names no bridge on the host' &&
	printf '%s\n' "$out" | sed -n '/^== listing not root/,/^status=/p' | grep -q 'is not root.s alone' ||
	fail "hull-root did what it must refuse" "$out"
# The log: every grant and refusal it acted on, the bridge with its
# machine; a refusal for another plan's bytes or another's process comes
# before the listing is read, and is not a line.
cases=$((cases + 1))
log=$(printf '%s\n' "$out" | sed -n '/^== log/,$p')
printf '%s\n' "$log" | grep -q " start $token refused network.bridge\$" &&
	printf '%s\n' "$log" | grep -q " start $token granted network.bridge\$" &&
	printf '%s\n' "$log" | grep -Eq " bridge $token granted network.bridge machine=[0-9]+\$" &&
	printf '%s\n' "$log" | grep -Eq " bridge $handmade_hash granted network.bridge machine=[0-9]+\$" ||
	fail "the audit log is not what was granted and refused" "$log"

case $rootfs in
*/test/rootvm/rootfs) rm -rf -- "$rootfs" ;;
esac

printf 'rootvm: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
