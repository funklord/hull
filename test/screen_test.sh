#!/bin/sh
# screen_test.sh -- a vm's screen shown on this host (project.md section
# 5.2). Its local display: a 2D (display vnc) machine's qemu D-Bus display,
# reached by the monitor linked as hull/display/ADDRESS, read by a plain
# GIO client (test/display_client.py) and by hull-gui's own screen window
# (gui/test/screen_window_test), which must get qemu's framebuffer shared
# rather than copied, whose typed key the guest must read from its own
# keyboard, and whose new size the guest must take as its preferred mode.
# A (display local) machine with (gpu virgl) handing its frames over as
# the GPU's dmabufs, and serving no VNC. Its 3D: (tuning (qemu (gpu
# virgl))) a guest's OpenGL rendered by the host's GPU through
# virglrenderer, its renderer read in the guest;
# (gpu venus) its Vulkan by Venus where /dev/udmabuf is open to the user,
# and refused naming it where it is not; 3D without a screen refused at
# plan. And no qemu, nor anything it forked, left after any of it. Not
# part of `make check`, needing KVM, a host GPU with a render node, and the
# network once: `make check-screen`.
#
# Its arguments are build/hull and the screen window's test. Debian 13, a
# build of it with Mesa's drivers and tools, and the kernel hull fetches
# are kept in $BUILD_DIR/test/screen-data, so only the first run fetches;
# everything else is under $BUILD_DIR/test/screen, removed at the start.
# Every run is under `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
window_test=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(cd "$(dirname "$0")" && pwd)
dir=${BUILD_DIR:?}/test/screen
case $dir in
*/test/screen) ;;
*) printf 'screen: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
if [ ! -w /dev/kvm ] || [ ! -e /dev/dri/renderD128 ]; then
	printf 'screen: not tried: no /dev/kvm or no render node\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run" "${BUILD_DIR:?}/test/screen-data"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_DATA_HOME=$(cd "${BUILD_DIR:?}/test/screen-data" && pwd)
XDG_CACHE_HOME=$XDG_DATA_HOME/cache
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR XDG_DATA_HOME XDG_CACHE_HOME HULL_CORE_IDLE
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# left: this user's processes still holding the suite's run directory in
# their arguments -- a qemu -- or Venus's render server, which qemu forks
# and which outlived it once (project.md 5.2), counted against those
# before the suite.
left() {
	printf '%s\n' $(( $(ps -u "$(id -u)" -o args= | grep -c "^[q]emu-system.*$root/run") +
		$(ps -u "$(id -u)" -o comm= | grep -c '^[v]irgl_render') - servers ))
}
servers=$(ps -u "$(id -u)" -o comm= | grep -c '^[v]irgl_render')

cat > "$root/mesa.conf" <<'EOF'
(build (base "gpu/debian")
  (steps
    (step (name "mesa") (network true)
      (run "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends mesa-vulkan-drivers vulkan-tools mesa-utils-bin libgl1-mesa-dri"))))
EOF
out=$(timeout 900 "$hull" pull debian:13 gpu/debian 2>&1) &&
	out=$(cd "$root" && timeout 1800 "$hull" build -f "$root/mesa.conf" gpu/mesa 2>&1)
if [ $? -ne 0 ]; then
	printf 'screen: not tried: the image could not be made\n%s\n' "$out"
	exit 0
fi

# machine NAME GPU COMMAND: a vm of the image with a screen and GPU, run.
machine() {
	printf '(machine (source "gpu/mesa") (backend vm) (mutable overlay) (memory "2G") (hostname "%s") (display vnc) (tuning (qemu (gpu %s))) (command "/bin/sh" "-c" "%s"))\n' \
		"$1" "$2" "$3" > "$root/$1.conf"
	(cd "$root" && timeout -k 2 300 "$hull" run -f "$root/$1.conf" 2>&1)
}

# The local display: a 2D screen by VNC, whose guest waits for a key on
# its own virtio keyboard while the clients look.
printf '(machine (source "gpu/mesa") (backend vm) (mutable overlay) (memory "1G") (hostname "local") (display vnc) (command "/bin/sh" "-c" "%s"))\n' \
	'for e in /sys/class/input/event*; do grep -q Keyboard $e/device/name && k=/dev/input/${e##*/}; done; echo keyboard=$k; timeout 40 dd if=$k bs=24 count=2 2> /dev/null | od -An -tx1 > /tmp/keys; grep -q \" 1e 00\" /tmp/keys && echo key-a-read; sleep 4; echo mode=$(head -1 /sys/class/drm/card0-Virtual-1/modes)' \
	> "$root/local.conf"
(cd "$root" && exec timeout -k 2 120 "$hull" run -f "$root/local.conf" > "$root/local.out" 2>&1) &
runner=$!
tries=0
until grep -q '^keyboard=' "$root/local.out" 2> /dev/null || [ "$tries" -ge 240 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
link=$(ls "$XDG_RUNTIME_DIR"/hull/display/* 2> /dev/null | head -1)
cases=$((cases + 1))
[ -S "$link" ] ||
	fail "a 2D (display vnc) machine linked no D-Bus display monitor" \
		"$(ls -l "$XDG_RUNTIME_DIR/hull/display" 2>&1; cat "$root/local.out")"
if python3 -c 'import gi; gi.require_version("Gio", "2.0")' 2> /dev/null; then
	out=$(timeout 30 python3 "$here/display_client.py" "$link" 20 2>&1)
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^frame [0-9]*x[0-9]*$' ||
		fail "a GIO client of the D-Bus display had no frame" "$out"
else
	printf 'screen: the GIO client not run: no Python GObject\n'
fi
out=$(QT_QPA_PLATFORM=offscreen timeout 30 "$window_test" "$link" 20 2>&1)
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^frame [0-9]*x[0-9]* shared$' ||
	fail "hull-gui's screen window had no frame of qemu's shared memory" "$out"
wait "$runner"
cases=$((cases + 1))
grep -q '^key-a-read$' "$root/local.out" ||
	fail "a key typed in hull-gui's screen window did not reach the guest's keyboard" \
		"$(cat "$root/local.out")"
cases=$((cases + 1))
grep -q '^mode=1000x700$' "$root/local.out" ||
	fail "the screen window's size was not the guest's preferred mode after it was resized" \
		"$(cat "$root/local.out")"
cases=$((cases + 1))
[ ! -e "$link" ] && [ ! -L "$link" ] ||
	fail "the display monitor's link outlived its machine" "$(ls -l "$link" 2>&1)"

# A 3D screen kept for hull-gui: (display local) with (gpu virgl), whose
# frames qemu renders on the host's GPU and hands over as dmabufs.
printf '(machine (source "gpu/mesa") (backend vm) (memory "1G") (hostname "local3d") (display local) (tuning (qemu (gpu virgl))) (command "/bin/sleep" "30"))\n' \
	> "$root/local3d.conf"
(cd "$root" && exec timeout -k 2 120 "$hull" run -f "$root/local3d.conf" > "$root/local3d.out" 2>&1) &
runner=$!
tries=0
until [ -S "$(ls "$XDG_RUNTIME_DIR"/hull/display/* 2> /dev/null | head -1)" ] || [ "$tries" -ge 120 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
link=$(ls "$XDG_RUNTIME_DIR"/hull/display/* 2> /dev/null | head -1)
if python3 -c 'import gi; gi.require_version("Gio", "2.0")' 2> /dev/null; then
	out=$(timeout 60 python3 "$here/display_client.py" "$link" 45 2>&1)
	cases=$((cases + 1))
	printf '%s\n' "$out" | grep -q '^dmabuf ' ||
		fail "a (display local) machine with (gpu virgl) handed over no GPU buffer" \
			"$out$(printf '\n'; cat "$root/local3d.out")"
fi
cases=$((cases + 1))
banner=$(timeout 5 python3 -c '
import socket, sys
try:
	s = socket.create_connection((sys.argv[1], 5900), 2)
	print("answered")
except OSError:
	print("none")' "$(basename "$link")" 2>&1)
[ "$banner" = none ] || fail "a (display local) machine answered VNC" "$banner"
wait "$runner"

out=$(machine gpugl virgl 'eglinfo.x86_64-linux-gnu -B -p surfaceless 2>&1 | grep -i renderer')
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'renderer: virgl (' ||
	fail "a guest's OpenGL was not rendered by the host's GPU through virgl" "$out"
sleep 2
cases=$((cases + 1))
[ "$(left)" = 0 ] ||
	fail "a qemu was left after its machine ended" "$(ps -eo pid,ppid,args | grep '[q]emu-system')"

out=$(machine gpuvk venus 'vulkaninfo --summary 2>&1 | grep -i deviceName')
cases=$((cases + 1))
if [ -r /dev/udmabuf ] && [ -w /dev/udmabuf ]; then
	printf '%s\n' "$out" | grep -q 'Venus' ||
		fail "a guest's Vulkan did not reach the host's GPU through Venus" "$out"
	sleep 2
	cases=$((cases + 1))
	[ "$(left)" = 0 ] ||
		fail "Venus's render server, or its qemu, was left after its machine ended" \
			"$(ps -eo pid,ppid,args | grep '[q]emu-system')"
else
	printf '%s\n' "$out" | grep -q '/dev/udmabuf, which is not open to this user' ||
		fail "(gpu venus) was not refused naming /dev/udmabuf" "$out"
	printf 'screen: Venus not run: /dev/udmabuf is not open to this user\n'
fi

printf '(machine (source "gpu/mesa") (backend vm) (tuning (qemu (gpu virgl))) (command "/bin/true"))\n' \
	> "$root/noscreen.conf"
cases=$((cases + 1))
out=$(cd "$root" && timeout 60 "$hull" run -f "$root/noscreen.conf" 2>&1)
printf '%s\n' "$out" | grep -q 'needs (display window) or (display vnc)' ||
	fail "3D without a screen was not refused" "$out"

printf 'screen: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
