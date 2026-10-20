#!/bin/sh
# nss_test.sh -- nss/nss_hull.c through glibc's own lookups, as `getent`
# makes them, without installing anything: each lookup runs in a user and
# mount namespace of its own, where an nsswitch.conf saying `hosts: hull`
# is bound over /etc's and the library is found on LD_LIBRARY_PATH. Then
# the hosts file the core keeps, for a machine it runs.
#
# Its arguments are build/hull and build/libnss_hull.so.2. It writes only
# under $BUILD_DIR/test/nss, which it removes at its start and its end; it
# loops only over a fixed list and the one wait, which gives up after 40
# tries, and every command is bounded.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
lib_dir=$(cd "$(dirname "$2")" && pwd)
probe=${BUILD_DIR:?}/test/cli/rootfs/probe
dir=${BUILD_DIR:?}/test/nss
case $dir in
*/test/nss) ;;
*) echo "nss_test: $dir is not a test/nss directory" >&2; exit 2 ;;
esac
rm -rf "${dir:?}"
mkdir -p "$dir/fixed/hull" "$dir/run" "$dir/rootfs/proc" "$dir/rootfs/dev" "$dir/rootfs/sys"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
cp "$probe" "$root/rootfs/probe"
printf 'hosts: hull\n' > "$root/nsswitch.conf"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# look RUNTIME DATABASE KEY: getent's answer, with only hull to ask, as
# the user whose runtime directory is RUNTIME; its status in $got.
look() {
	out=$(timeout 20 unshare -rm sh -c '
mount --bind "$1" /etc/nsswitch.conf || exit 99
LD_LIBRARY_PATH=$2 XDG_RUNTIME_DIR=$3 exec getent "$4" "$5"' sh \
		"$root/nsswitch.conf" "$lib_dir" "$1" "$2" "$3" 2>&1)
	got=$?
}

# expect RUNTIME DATABASE KEY STATUS ANSWER: getent exits STATUS and says
# ANSWER, its whitespace squeezed to one space.
expect() {
	look "$1" "$2" "$3"
	cases=$((cases + 1))
	squeezed=$(printf '%s\n' "$out" | tr -s ' \t' '  ' | sed 's/ $//')
	[ "$got" = "$4" ] && [ "$squeezed" = "$5" ] ||
		fail "getent $2 $3 exited $got, wanted $4 and '$5'" "$out"
}

# Without one, nothing is found.
expect "$root/fixed" hosts web.hull 2 ''

# A file as the core writes it, a line that is not one among them.
printf 'web 127.107.3.4\nnot a line\ndb 127.107.9.10\n' > "$root/fixed/hull/hosts"
expect "$root/fixed" hosts web.hull 0 '127.107.3.4 web.hull'
expect "$root/fixed" hosts WEB.Hull. 0 '127.107.3.4 web.hull'
expect "$root/fixed" ahostsv4 db.hull 0 '127.107.9.10 STREAM db.hull
127.107.9.10 DGRAM
127.107.9.10 RAW'
expect "$root/fixed" hosts 127.107.9.10 0 '127.107.9.10 db.hull'
# A machine not held, a name that is not ours, and an address of
# 127.107/16 that no machine has, nor one outside it.
expect "$root/fixed" hosts nope.hull 2 ''
expect "$root/fixed" hosts web 2 ''
expect "$root/fixed" hosts web.hull.example 2 ''
expect "$root/fixed" hosts .hull 2 ''
expect "$root/fixed" hosts 127.107.3.5 2 ''
expect "$root/fixed" hosts 127.0.0.1 2 ''
# IPv4 only, as the addresses are: asked for IPv6 with mapped addresses
# allowed, as ahostsv6 asks, glibc maps the IPv4 one.
expect "$root/fixed" ahostsv6 web.hull 0 '::ffff:127.107.3.4 STREAM web.hull
::ffff:127.107.3.4 DGRAM
::ffff:127.107.3.4 RAW'
# A runtime directory that is not absolute is not believed: the file is
# then /run/user/UID's, which in the namespace is root's, and none.
expect "fixed" hosts web.hull 2 ''

# The core's own: a file that a core which died left is not true once a
# core runs, a machine it runs has its address there, and the file goes
# with the core.
mkdir -p "$root/run/hull"
printf 'ghost 127.107.1.1\n' > "$root/run/hull/hosts"
printf '(machine (source "dir:./rootfs") (backend chroot) (hostname "named") %s)\n' \
	'(command "/probe" "say" "ready" "wait")' > "$root/named.conf"
(cd / && XDG_RUNTIME_DIR="$root/run" HULL_CORE_IDLE=1 \
	exec timeout -k 2 30 "$hull" run -f "$root/named.conf" > "$root/named.out" 2>&1) &
runner=$!
tries=0
until grep -q '^ready$' "$root/named.out" 2>/dev/null || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
at=$(XDG_RUNTIME_DIR="$root/run" timeout 10 "$hull" ps 2>&1 | awk '$1 == "named" { print $2 }')
cases=$((cases + 1))
[ -n "$at" ] || fail "the machine is not listed" "$(cat "$root/named.out")"
expect "$root/run" hosts named.hull 0 "$at named.hull"
expect "$root/run" hosts "$at" 0 "$at named.hull"
expect "$root/run" hosts ghost.hull 2 ''
XDG_RUNTIME_DIR="$root/run" timeout 10 "$hull" kill -s TERM named > /dev/null 2>&1
wait "$runner"
tries=0
while [ -S "$root/run/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
cases=$((cases + 1))
[ ! -e "$root/run/hull/hosts" ] || fail "the core left its hosts file" "$(cat "$root/run/hull/hosts")"
expect "$root/run" hosts named.hull 2 ''

rm -rf "${dir:?}"
printf 'nss: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
