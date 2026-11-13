#!/bin/sh
# gpu_test.sh -- a vm's 3D on the host's own GPU (project.md section 5.2):
# (tuning (qemu (gpu virgl))) a guest's OpenGL rendered by the host's GPU
# through virglrenderer, its renderer read in the guest; (gpu venus) its
# Vulkan by Venus where /dev/udmabuf is open to the user, and refused
# naming it where it is not; 3D without a screen refused at plan; and no
# qemu, nor anything it forked, left after any of it. Not part of `make
# check`, needing KVM, a host GPU with a render node, and the network
# once: `make check-gpu`.
#
# Its argument is build/hull. Debian 13, a build of it with Mesa's
# drivers and tools, and the kernel hull fetches are kept in $BUILD_DIR/test/gpu-data, so only the
# first run fetches; everything else is under $BUILD_DIR/test/gpu,
# removed at the start. Every run is under `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/gpu
case $dir in
*/test/gpu) ;;
*) printf 'gpu: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
if [ ! -w /dev/kvm ] || [ ! -e /dev/dri/renderD128 ]; then
	printf 'gpu: not tried: no /dev/kvm or no render node\n'
	exit 0
fi
rm -rf "$dir"
mkdir -p "$dir/run" "${BUILD_DIR:?}/test/gpu-data"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_DATA_HOME=$(cd "${BUILD_DIR:?}/test/gpu-data" && pwd)
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
	printf 'gpu: not tried: the image could not be made\n%s\n' "$out"
	exit 0
fi

# machine NAME GPU COMMAND: a vm of the image with a screen and GPU, run.
machine() {
	printf '(machine (source "gpu/mesa") (backend vm) (mutable overlay) (memory "2G") (hostname "%s") (display vnc) (tuning (qemu (gpu %s))) (command "/bin/sh" "-c" "%s"))\n' \
		"$1" "$2" "$3" > "$root/$1.conf"
	(cd "$root" && timeout -k 2 300 "$hull" run -f "$root/$1.conf" 2>&1)
}

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
	printf 'gpu: Venus not run: /dev/udmabuf is not open to this user\n'
fi

printf '(machine (source "gpu/mesa") (backend vm) (tuning (qemu (gpu virgl))) (command "/bin/true"))\n' \
	> "$root/noscreen.conf"
cases=$((cases + 1))
out=$(cd "$root" && timeout 60 "$hull" run -f "$root/noscreen.conf" 2>&1)
printf '%s\n' "$out" | grep -q 'needs (display window) or (display vnc)' ||
	fail "3D without a screen was not refused" "$out"

printf 'gpu: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
