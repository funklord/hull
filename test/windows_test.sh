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
for tool in curl mcopy nc ssh ssh-keygen qemu-system-x86_64 qemu-img swtpm sha256sum python3 iconv base64; do
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
# With a screen, (display vnc), so that hull-gui's way to it is measured
# on Windows too: qemu's D-Bus display beside VNC (project.md section 5.2).
sed 's/^  (ports /  (display vnc)\n  (ports /' "$root/w/machine.conf" > "$root/w/screen.conf"
cases=$((cases + 1))
grep -q '(display vnc)' "$root/w/screen.conf" || fail 'the screen spec was not made' "$(cat "$root/w/screen.conf")"
(cd / && timeout -k 10 1800 "$hull" run -f "$root/w/screen.conf" < /dev/null > "$root/machine.out" 2>&1) &
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
# Its graphics: the display adapter Windows runs and its mode, from
# inside; and from outside, the screen hull screenshot takes, a PNG of
# that size showing more than one colour -- a desktop or its sign-in,
# not a firmware's blank frame. The PNG is read here with zlib alone.
# Encoded, as cmd, which runs what sshd is given, would take its pipe.
query=$(printf '%s' 'Get-CimInstance Win32_VideoController | ForEach-Object { "{0} {1}x{2}" -f $_.Name, $_.CurrentHorizontalResolution, $_.CurrentVerticalResolution }' |
	iconv -t UTF-16LE | base64 -w 0)
out=$(door powershell -NoProfile -EncodedCommand "$query")
cases=$((cases + 1))
mode=$(printf '%s\n' "$out" | tr -d '\r' | grep -Eo '[1-9][0-9]{2,4}x[1-9][0-9]{2,4}' | head -1)
[ -n "$mode" ] || fail 'Windows runs a display adapter in a mode' "$out"
cases=$((cases + 1))
if [ -n "$id" ] && timeout 30 "$hull" screenshot "$id" "$root/screen.png" > /dev/null 2>&1; then
	seen=$(python3 -I -c '
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n"
at, idat = 8, b""
while at < len(data):
	length, kind = struct.unpack(">I4s", data[at:at + 8])
	body = data[at + 8:at + 8 + length]
	if kind == b"IHDR":
		width, height, depth, colour = struct.unpack(">IIBB", body[:10])
	elif kind == b"IDAT":
		idat += body
	at += 12 + length
assert depth == 8 and colour in (2, 6)
step = 3 if colour == 2 else 4
raw, row, prev, colours = zlib.decompress(idat), width * step, bytearray(width * step), set()
for y in range(height):
	kind, line = raw[y * (row + 1)], bytearray(raw[y * (row + 1) + 1:(y + 1) * (row + 1)])
	for x in range(row):
		a = line[x - step] if x >= step else 0
		b = prev[x]
		c = prev[x - step] if x >= step else 0
		if kind == 1: line[x] = (line[x] + a) & 255
		elif kind == 2: line[x] = (line[x] + b) & 255
		elif kind == 3: line[x] = (line[x] + (a + b) // 2) & 255
		elif kind == 4:
			p = a + b - c
			pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
			line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
	if y % 4 == 0:
		colours.update(bytes(line[x:x + 3]) for x in range(0, row, step * 4))
	prev = line
print("%dx%d %d" % (width, height, len(colours)))' "$root/screen.png" 2>&1)
	case $seen in
	"$mode "*) [ "${seen##* }" -ge 16 ] 2> /dev/null || fail 'the screen hull screenshot takes shows a picture' "$seen" ;;
	*) fail "the screen hull screenshot takes is Windows' mode, $mode" "$seen" ;;
	esac
else
	fail 'hull screenshot takes the screen of the Windows machine' "id=$id"
fi
# The screen as hull-gui reaches it: VNC at the machine's address, and
# qemu's D-Bus display through its link, its frame Windows' own mode with
# something drawn in it.
out=$(timeout 5 nc -w 2 "$address" 5900 < /dev/null | head -c 12)
says 'the screen answers VNC at the machine'"'"'s address' "$out" 'RFB 003'
if python3 -c 'import gi; gi.require_version("Gio", "2.0")' 2> /dev/null; then
	out=$(timeout 30 python3 "$(cd "$(dirname "$0")" && pwd)/display_client.py" \
		"$XDG_RUNTIME_DIR/hull/display/$address" 15 2>&1)
	cases=$((cases + 1))
	lit=$(printf '%s\n' "$out" | sed -n 's/^lit //p')
	printf '%s\n' "$out" | grep -q "^frame $mode\$" && [ "${lit:-0}" -gt 0 ] ||
		fail "hull-gui's D-Bus display gives Windows' frame, $mode, drawn" "$out"
fi
door shutdown /s /t 0 > /dev/null
# A machine that never answered is stopped rather than left to its timeout.
[ "$failures" = 0 ] || [ -z "$id" ] || "$hull" stop "$id" > /dev/null 2>&1
wait "$run"
status 'the machine powers off and its run ends' "$?" 0 "$(tail -40 "$root/machine.out")"
finish
