#!/bin/sh
# screenshots.sh -- the README's picture, made from the current build
# (harmonization.md, "Show it"): hull-gui's window, drawn headless, on
# demo machines in a home of its own, so nothing of the user's -- no
# machine, host or file -- is in it.
#
# Usage: tool/screenshots.sh HULL HULL-GUI OUT-DIR, run by `make
# screenshots`. Everything it makes is under $BUILD_DIR/screenshots,
# cleared at its start and its end; the window is under `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
gui=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
out=$3
work=${BUILD_DIR:?}/screenshots
case $work in
*/screenshots) ;;
*) printf 'screenshots: refusing to clear %s\n' "$work"; exit 1 ;;
esac
rm -rf "$work"
mkdir -p "$work/home/hull/web/rootfs" "$work/home/hull/winxp" "$work/home/hull/board" \
	"$work/run" "$work/config" "$out"
chmod 700 "$work/run"
root=$(cd "$work" && pwd)

# Three machines as a person might keep them, one per class: a service
# from a directory, a whole PC from its disk, a microcontroller board.
printf '%s\n' '; A web service from a plain directory, on any engine.' \
	'(machine (hostname "web") (source "dir:./rootfs") (mutable overlay)' \
	'  (command "/srv/web") (ports (port (host 8080) (guest 80))))' \
	> "$root/home/hull/web/hull.conf"
truncate -s 1M "$root/home/hull/winxp/disk.qcow2"
printf '%s\n' '; An old PC, booted from its own disk.' \
	'(machine (backend pc) (hostname "winxp") (source "media:disk") (memory "1G")' \
	'  (network nat) (display local)' \
	'  (media (medium (type disk) (source "image:./disk.qcow2")))' \
	'  (tuning (pc (machine pc) (nic e1000) (sound ac97) (pointer tablet))))' \
	> "$root/home/hull/winxp/hull.conf"
truncate -s 4K "$root/home/hull/board/firmware.elf"
printf '%s\n' '; A Cortex-M3 board running its firmware.' \
	'(machine (backend pc) (hostname "board") (source "image:./firmware.elf")' \
	'  (tuning (pc (machine mps2-an385))))' \
	> "$root/home/hull/board/hull.conf"

(
	cd "$root" &&
	HOME=$root/home XDG_RUNTIME_DIR=$root/run XDG_CONFIG_HOME=$root/config \
	XDG_DATA_HOME=$root/data XDG_CACHE_HOME=$root/cache HULL_CORE_IDLE=1 HULL_THEME=light \
	QT_QPA_PLATFORM=offscreen timeout 60 "$gui" --hull "$hull" --gui --select web \
		--picture "$root/hull-gui.png"
) || { printf 'screenshots: hull-gui drew no picture\n'; exit 1; }
cp "$root/hull-gui.png" "$out/hull-gui.png"
rm -rf "$work"
printf 'screenshots: %s/hull-gui.png\n' "$out"
