#!/bin/sh
# windows_test.sh -- `hull setup windows` end to end on Windows itself:
# Microsoft's Windows 11 Enterprise evaluation, the disc fmake pinned on
# 2026-11-12 (its section 633, the holder's choice of image), installed
# with nobody at it on the pc class and then logged into by ssh. Not part
# of `make check`, needing the network once, 8 GB of disc, about 30 GB of
# disk while it runs and over an hour: `make check-windows` runs it. Its
# argument is build/hull.
#
# The disc is fetched once into $BUILD_DIR/legacy-media and held to the
# digest fmake measured, Microsoft publishing none for its evaluation
# images. Everything else is under $BUILD_DIR/test/windows, removed at the
# start and on success, left on failure; the runtime directory is a tmpfs
# directory of its own. Every run is under `timeout`, and the core is the
# test's, whose idle clock is two seconds.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/windows
case $dir in
*/test/windows) ;;
*) printf 'windows: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
for tool in curl mcopy ssh ssh-keygen qemu-system-x86_64 qemu-img swtpm sha256sum; do
	if ! command -v "$tool" > /dev/null; then
		printf 'windows: no %s here, nothing tried\n' "$tool"
		exit 0
	fi
done
if [ ! -f /usr/share/OVMF/OVMF_CODE_4M.secboot.fd ]; then
	printf 'windows: no OVMF built for Secure Boot here, nothing tried\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/cache" "$BUILD_DIR/legacy-media"
root=$(cd "$dir" && pwd)
media=$(cd "$BUILD_DIR/legacy-media" && pwd)
XDG_RUNTIME_DIR=$(mktemp -d /dev/shm/hull-windows.XXXXXX)
trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
trap 'exit 1' INT TERM
XDG_CACHE_HOME=$root/cache
XDG_DATA_HOME=$root/data
HULL_CORE_IDLE=2
# setup authorises this HOME's public key, and the core keeps its
# starter's environment.
HOME=$root/home
export XDG_RUNTIME_DIR XDG_CACHE_HOME XDG_DATA_HOME HULL_CORE_IDLE HOME
unset DISPLAY
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME" "$HOME/.ssh"
ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/id_ed25519"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

says() {
	cases=$((cases + 1))
	printf '%s\n' "$2" | grep -qF "$3" || fail "$1" "$2"
}

status() {
	cases=$((cases + 1))
	[ "$2" = "$3" ] || fail "$1: exited $2, wanted $3" "$4"
}

finish() {
	printf 'windows: %s cases, %s failed\n' "$cases" "$failures"
	[ "$failures" = 0 ] && rm -rf "$dir"
	[ "$failures" = 0 ]
	exit
}

disc=Win11-26H2-Enterprise-Eval.iso
sum=bc3f24086ebadc94489066b5ad78089e2cf5c3491e90e790bb81a2b199c10e38
if [ ! -f "$media/$disc" ]; then
	curl -fsSL -m 3600 -A hull -o "$media/$disc.part" \
		'https://go.microsoft.com/fwlink/?LinkId=2382600&clcid=0x409&culture=en-us&country=us' &&
		mv "$media/$disc.part" "$media/$disc"
	rm -f "$media/$disc.part"
fi
if [ ! -f "$media/$disc" ]; then
	printf 'windows: %s not fetched, nothing tried\n' "$disc"
	exit 0
fi
cases=$((cases + 1))
if ! printf '%s  %s\n' "$sum" "$media/$disc" | sha256sum -c --quiet > /dev/null 2>&1; then
	fail 'the disc is the one fmake measured' "$(ls -l "$media/$disc")"
	finish
fi

out=$(timeout 120 "$hull" setup windows "$media/$disc" "$root/w" 2>&1)
status 'hull setup windows' "$?" 0 "$out"
[ "$failures" = 0 ] || finish

# The install: Setup from the disc, its answer from the stick, two
# restarts, the first logon installing OpenSSH Server (Windows fetches it)
# and powering off. fmake measured 73 minutes by qemu directly.
start=$(date +%s)
(cd / && timeout -k 10 10800 "$hull" run -f "$root/w/install.conf" < /dev/null > "$root/install.out" 2>&1)
got=$?
printf 'windows: the install took %s s\n' "$(($(date +%s) - start))"
status 'the install ran to its power-off' "$got" 0 "$(tail -40 "$root/install.out")"
[ "$failures" = 0 ] || finish

# The installed disk alone, its TPM and variables those it was installed
# with. Its address comes from `hull ps`; its sshd is the guest's own, at
# port 2222 of that address, with this HOME's key.
(cd / && timeout -k 10 1800 "$hull" run -f "$root/w/machine.conf" < /dev/null > "$root/machine.out" 2>&1) &
run=$!
address=
id=
for _ in $(seq 120); do
	set -- $("$hull" ps 2>/dev/null | awk '$5 == "running" { print $1, $2; exit }')
	id=${1:-} address=${2:-}
	[ -n "$address" ] && break
	[ -d "/proc/$run" ] || break
	sleep 1
done
says 'the installed machine is running' "address=$address" 'address=1'

# Windows' sshd hands a command to cmd /c already, so the command is
# cmd's own: a nested cmd /c loses its quotes. LogLevel keeps ssh's
# own warnings out of what the guest said.
door() {
	timeout 60 ssh -F /dev/null -p 2222 -i "$HOME/.ssh/id_ed25519" -o IdentitiesOnly=yes \
		-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		-o ConnectTimeout=10 -o LogLevel=ERROR "hull@$address" "$@" 2>&1
}

out=
if [ -n "$address" ]; then
	for _ in $(seq 90); do
		out=$(door ver) && break
		[ -d "/proc/$run" ] || break
		sleep 10
	done
fi
says 'ssh reaches Windows with the key setup authorised' "$out" 'Microsoft Windows [Version 10.0.'
out=$(door whoami)
says 'as the account the answer made, an administrator' "$out" 'hull-windows\hull'
out=$(door net localgroup administrators)
says 'in the Administrators group' "$out" 'hull'
out=$(door powershell -NoProfile -Command '(Get-Tpm).TpmPresent; Confirm-SecureBootUEFI')
says 'with a TPM' "$(printf '%s\n' "$out" | sed -n 1p)" 'True'
says 'and Secure Boot on' "$(printf '%s\n' "$out" | sed -n 2p)" 'True'
door shutdown /s /t 0 > /dev/null
# A machine that never answered is stopped rather than left to its timeout.
[ "$failures" = 0 ] || [ -z "$id" ] || "$hull" stop "$id" > /dev/null 2>&1
wait "$run"
status 'the machine powers off and its run ends' "$?" 0 "$(tail -40 "$root/machine.out")"
finish
