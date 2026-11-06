#!/bin/sh
# legacy_test.sh -- a real legacy system on the pc class (project.md section
# 5.0), where pc_test.sh boots only what it wrote itself. Not part of `make
# check`, needing the network once and qemu: `make check-legacy` runs it. Its
# argument is build/hull. The systems are FreeDOS 1.4's boot floppy and
# Debian 3.0's two, fetched once into $BUILD_DIR/legacy-media and held to their
# publishers' sums, then run as the user with nothing of hull's in them. It writes only under
# $BUILD_DIR, the test's own directory removed at its start and left on
# failure, a tmpfs directory of its own removed at its end. Everything it
# starts is under `timeout`, and the core is the test's, whose idle clock is
# two seconds.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
dir=${BUILD_DIR:?}/test/legacy
case $dir in
*/test/legacy) ;;
*) printf 'legacy: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
for tool in curl mcopy ssh ssh-keygen python3 qemu-system-x86_64; do
	if ! command -v "$tool" > /dev/null; then
		printf 'legacy: no %s here, nothing tried\n' "$tool"
		exit 0
	fi
done
if [ ! -x /usr/sbin/sshd ]; then
	printf 'legacy: no sshd here, nothing tried\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/cache" "$BUILD_DIR/legacy-media"
root=$(cd "$dir" && pwd)
media=$(cd "$BUILD_DIR/legacy-media" && pwd)
# The door's host key lives in memory only, so the runtime directory is a
# tmpfs of the user's.
XDG_RUNTIME_DIR=$(mktemp -d /dev/shm/hull-legacy.XXXXXX)
trap 'rm -rf "$XDG_RUNTIME_DIR"' EXIT
trap 'exit 1' INT TERM
XDG_CACHE_HOME=$root/cache
XDG_DATA_HOME=$root/data
HULL_CORE_IDLE=2
# The core keeps its starter's environment, and the door reads this HOME's key.
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

lacks() {
	cases=$((cases + 1))
	! printf '%s\n' "$2" | grep -qF "$3" || fail "$1" "$2"
}

status() {
	cases=$((cases + 1))
	[ "$2" = "$3" ] || fail "$1: exited $2, wanted $3" "$4"
}

# --- the system, as its publisher made it ------------------------------------

archive=FD14-FloppyEdition.zip
sum=45b1fa7c52dd996c3bfa5e352ffcd410781b952a6ad629f15a4c9ec4bbaefc5a
url=https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/distributions/1.4/$archive
if [ ! -f "$media/$archive" ]; then
	if ! curl -fsS -m 300 -o "$media/$archive.part" "$url"; then
		rm -f "$media/$archive.part"
		printf 'legacy: %s not fetched, nothing tried\n' "$archive"
		exit 0
	fi
	mv "$media/$archive.part" "$media/$archive"
fi
cases=$((cases + 1))
if ! printf '%s  %s\n' "$sum" "$media/$archive" | sha256sum -c --quiet > /dev/null 2>&1; then
	fail 'the medium is what its publisher published' "$media/$archive"
	exit 1
fi
python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive, open(sys.argv[2], "wb") as out:
	out.write(archive.read("144m/x86BOOT.img"))' "$media/$archive" "$root/boot.img"

# conf ENTRIES...: the machine of $root/hull.conf.
conf() {
	printf '(machine (backend pc) (source "media:floppy") %s)\n' "$*" > "$root/hull.conf"
}

# prompt FILE: waits for the end of FILE to be a DOS prompt.
prompt() {
	n=0
	while [ "$n" -lt 40 ] && ! tail -c 8 "$1" 2> /dev/null | grep -q 'A:.>'; do
		sleep 1
		n=$((n + 1))
	done
}

# --- as published: its own firmware, its own screen --------------------------

conf '(display vnc) (media (medium (type floppy) (source "image:./boot.img")))'
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/screen.out" 2>&1 &
pid=$!
sleep 20
address=$("$hull" ps | awk '$5 == "running" { print $2 }')
out=$(timeout 20 python3 -I "$here/rfb.py" "$address" 5900 --colours 2>&1)
# Text on a blue-and-green banner: a blank screen, or a BIOS's, has two.
cases=$((cases + 1))
[ "${out##* }" -ge 4 ] 2> /dev/null || fail 'the installer drew its screen' "$out"
timeout 30 "$hull" stop legacy > /dev/null 2>&1
wait "$pid"

# --- the door to a real DOS prompt -------------------------------------------

me=$(id -un)

# floppy LINES IMAGE: a floppy whose console is COM1, as anyone would make
# it. FreeDOS 1.4's first read after CTTY waits on the BIOS keyboard's status
# and not COM1's; one key pending is never consumed and satisfies it for good.
# So the 8 bytes of KEY.COM, INT 16h AH=05 with a space, go first. The control
# below shows that without them a cold console stays silent.
floppy() {
	cp "$root/boot.img" "$2"
	printf '\264\005\271\040\071\315\026\303' > "$root/KEY.COM"
	mcopy -o -i "$2" "$root/KEY.COM" ::KEY.COM
	printf '@echo off\r\nSET OS_NAME=FreeDOS\r\nSET PATH=A:\\FREEDOS\\BIN\r\n%bCTTY COM1\r\n' "$1" \
		> "$root/FDAUTO.BAT"
	mcopy -o -i "$2" "$root/FDAUTO.BAT" ::FDAUTO.BAT
}

# door: ssh at the machine's address, its host key pinned.
door() {
	pub=$(find "$XDG_DATA_HOME/hull/machines/legacy" -name host_key.pub)
	printf 'legacy.hull %s\n' "$(cut -d' ' -f1,2 "$pub")" > "$root/known"
	timeout 25 ssh -F /dev/null -i "$HOME/.ssh/id_ed25519" -o IdentitiesOnly=yes \
		-o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile="$root/known" \
		-o ProxyCommand="$hull proxy-ssh %h" -tt "$me@legacy.hull" 2>&1
}

floppy 'A:\KEY.COM\r\n' "$root/door.img"
conf '(ssh true) (mutable direct) (media (medium (type floppy) (source "image:./door.img")))'
timeout 80 "$hull" run -f "$root/hull.conf" > "$root/door.out" 2>&1 &
pid=$!
prompt "$root/door.out"
out=$( { sleep 2; printf 'ver\r'; sleep 2; printf 'echo os-%%OS_NAME%%\r'; sleep 2
	printf 'echo kept > A:\\MARK.TXT\r'; sleep 3; printf 'fdapm poweroff\r'; sleep 8; } | door)
says 'ssh reaches DOS, cold, with no keyboard' "$out" 'FreeCom version'
says 'and DOS expands what is typed' "$out" 'os-FreeDOS'
wait "$pid"
status 'and the system powers the machine off' "$?" 0 "$(tail -5 "$root/door.out")"
out=$(mtype -i "$root/door.img" ::MARK.TXT 2>&1)
says 'and what DOS wrote is on its medium' "$out" 'kept'

# --- a set of floppies, changed under a running DOS --------------------------

python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive, open(sys.argv[2], "wb") as out:
	out.write(archive.read("144m/x86DSK01.img"))' "$media/$archive" "$root/disk1.img"
cp "$root/disk1.img" "$root/disk1.rw.img"
before=$(sha256sum < "$root/disk1.img")

# dos LINE...: each line typed at COM1's console, what came back on stdout.
dos() {
	{
		sleep 1
		for line in "$@"; do
			printf '%s\r' "$line"
			sleep 3
		done
	} | timeout $((2 + 4 * $#)) "$hull" exec legacy -- console 2>&1 | tr -d '\r'
}

floppy 'A:\KEY.COM\r\n' "$root/set.img"
conf '(media (medium (type floppy) (source "image:./set.img")))'
timeout 100 "$hull" run -f "$root/hull.conf" > "$root/set.out" 2>&1 &
pid=$!
prompt "$root/set.out"
says 'the first disk of the set is in' "$(dos 'dir a:')" 'FD14-BOOT'
out=$("$hull" media change legacy floppy "$root/disk1.img" 2>&1)
status 'the next disk goes in' "$?" 0 "$out"
says 'and DOS sees it, unasked' "$(dos 'dir a:')" 'FD14DSK01'
# Overlay's promise holds for a disk put in later: it is write-protected.
out=$(dos 'echo x > A:\W.TXT' 'a')
says 'under overlay it is write-protected' "$out" 'rite'
cases=$((cases + 1))
[ "$(sha256sum < "$root/disk1.img")" = "$before" ] || fail 'and its file is unchanged' ''
out=$("$hull" media change legacy floppy 2>&1)
status 'a disk comes out' "$?" 0 "$out"
# With its current drive empty DOS asks again at every prompt, ignoring what
# is not an answer: only Retry, once a disk is back, ends it.
out=$(dos 'dir a:')
says 'and DOS asks for one' "$out" 'general failure'
lacks 'and finds no directory' "$out" 'FD14DSK01'
out=$("$hull" media change legacy floppy "$root/set.img" 2>&1)
status 'the first goes back' "$?" 0 "$out"
says 'and is read again' "$(dos 'r' 'dir a:')" 'FD14-BOOT'
timeout 30 "$hull" stop legacy > /dev/null 2>&1
wait "$pid"

# (mutable direct): a disk put in later is written, as the first would be.
conf '(mutable direct) (media (medium (type floppy) (source "image:./set.img")))'
timeout 60 "$hull" run -f "$root/hull.conf" > "$root/direct.out" 2>&1 &
pid=$!
prompt "$root/direct.out"
# FreeCOM reads its messages from COMMAND.COM on the disk it booted from, once:
# swapped before that, as on a real PC, it prints the new disk's sectors. So a
# `dir` goes first, and the boot disk returns for FDAPM to be found.
says 'under direct the boot disk is in' "$(dos 'dir a:')" 'FD14-BOOT'
out=$("$hull" media change legacy floppy "$root/disk1.rw.img" 2>&1)
status 'and the next disk goes in' "$?" 0 "$out"
says 'and is read' "$(dos 'dir a:' 'echo written > A:\W.TXT')" 'FD14DSK01'
"$hull" media change legacy floppy "$root/set.img" > /dev/null 2>&1
dos 'dir a:' 'fdapm poweroff' > /dev/null
wait "$pid"
status 'and the system powers off' "$?" 0 "$(tail -3 "$root/direct.out")"
says 'with what it wrote on that disk' "$(mtype -i "$root/disk1.rw.img" ::W.TXT 2>&1)" 'written'

# The control: the same floppy without the key.
floppy '' "$root/silent.img"
conf '(ssh true) (media (medium (type floppy) (source "image:./silent.img")))'
timeout 50 "$hull" run -f "$root/hull.conf" > "$root/silent.out" 2>&1 &
pid=$!
prompt "$root/silent.out"
out=$( { sleep 2; printf 'ver\r'; sleep 4; } | door)
says 'without a key waiting the door still opens' "$out" 'Connection to legacy.hull closed'
says 'and DOS still prompts' "$(cat "$root/silent.out")" 'A:\>'
lacks 'but the prompt never reads' "$out" 'FreeCom version'
timeout 30 "$hull" stop legacy > /dev/null 2>&1
wait "$pid"

# --- a period Linux that stops and asks for its next disk --------------------

# fetch NAME SHA256: Debian 3.0's floppy, from its archive. The sums were cut
# from files that matched the MD5s Debian publishes beside them.
fetch() {
	if [ ! -f "$media/$1" ]; then
		curl -fsS -m 120 -o "$media/$1.part" \
			"https://archive.debian.org/debian/dists/woody/main/disks-i386/current/images-1.44/$1" ||
			return 1
		mv "$media/$1.part" "$media/$1"
	fi
	printf '%s  %s\n' "$2" "$media/$1" | sha256sum -c --quiet > /dev/null 2>&1
}

if fetch rescue.bin 103ce4634234e98e1f7fde27630297b1f99ddc9ef9562ca7c0dcf4dd728a6881 && fetch root.bin 6b96edeb080159ca4fb017cba60f1f9f6924c383255a5977ae98cff7793f3267; then
	cp "$media/rescue.bin" "$root/rescue.img"
	conf '(memory "64M") (display vnc) (media (medium (type floppy) (source "image:./rescue.img")))'
	timeout 90 "$hull" run -f "$root/hull.conf" > "$root/woody.out" 2>&1 &
	pid=$!
	sleep 6
	address=$("$hull" ps | awk '$5 == "running" { print $2 }')
	timeout 20 python3 -I "$here/rfb.py" "$address" 5900 --type 'linux console=ttyS0\n' > /dev/null 2>&1
	n=0
	while [ "$n" -lt 40 ] && ! grep -qa 'Insert root floppy' "$root/woody.out"; do
		sleep 1
		n=$((n + 1))
	done
	says 'a kernel of 2002 boots, and asks for its disk' "$(cat "$root/woody.out")" 'Linux version 2.2.20'
	says 'by name' "$(cat "$root/woody.out")" 'Insert root floppy'
	out=$("$hull" media change legacy floppy "$media/root.bin" 2>&1)
	status 'its root floppy goes in' "$?" 0 "$out"
	# The port is its console by now, so the ENTER it asks for goes there.
	{ sleep 1; printf '\r'; sleep 1; } | timeout 4 "$hull" exec legacy -- console > /dev/null 2>&1
	n=0
	while [ "$n" -lt 30 ] && ! grep -qa 'Debian GNU/Linux 3.0' "$root/woody.out"; do
		sleep 1
		n=$((n + 1))
	done
	says 'and its installer greets on the serial port' "$(cat "$root/woody.out")" 'Debian GNU/Linux 3.0'
	timeout 30 "$hull" stop legacy > /dev/null 2>&1
	wait "$pid"
else
	fail 'the Debian floppies are what Debian published, or were not fetched' "$media"
fi

printf 'legacy: %s cases, %s failed\n' "$cases" "$failures"
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
