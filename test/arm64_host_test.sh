#!/bin/sh
# arm64_host_test.sh -- hull on an arm64 host, with no arm64 hardware: an
# arm64 Debian booted by hull's own emu (qemu-system-aarch64, TCG), hull's
# source shared into it read-only, and there hull built natively and its
# own `make test` run, as on an arm64 machine; then hull's qemu engine on
# that host, a vm of its own arch under KVM, which the emulated CPU's EL2
# gives it, and an amd64 machine under emu (test/arm64_host_inner.sh is
# that half). Not part of `make check`:
# `make check-arm64-host` runs it. It needs the network, for Debian's
# arm64 image, its build tools and Debian's arm64 and amd64 kernels, and
# says so and passes where qemu-system-aarch64 is not installed.
#
# Its argument is the host's build/hull. What it keeps between runs, the
# store holding the image and its build, is under
# $BUILD_DIR/test/arm64-host/data, and the kernels, under its cache/, so a
# second run fetches nothing. The run directory is made anew. What it
# starts is under `timeout`, and the core is the test's own, whose idle
# clock is a second. It takes hours, everything in the machine running
# translated: the run of 2026-11-11 spent two building hull and ran out
# of time, so the machine is given six.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
dir=$(mkdir -p "${BUILD_DIR:?}/test/arm64-host" && cd "${BUILD_DIR:?}/test/arm64-host" && pwd)
case $dir in
*/test/arm64-host) ;;
*) echo "arm64-host: $dir is not a test/arm64-host directory" >&2; exit 2 ;;
esac

if ! command -v qemu-system-aarch64 > /dev/null 2>&1; then
	echo "arm64-host: no qemu-system-aarch64, nothing tried"
	exit 0
fi

rm -rf "${dir:?}/run"
mkdir -p "$dir/run" "$dir/data" "$dir/cache"
chmod 700 "$dir/run"
XDG_RUNTIME_DIR=$dir/run
XDG_DATA_HOME=$dir/data
XDG_CACHE_HOME=$dir/cache
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR XDG_DATA_HOME XDG_CACHE_HOME HULL_CORE_IDLE
unset DISPLAY

cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | tail -40 | sed 's/^/    /'
}

# step WHAT COMMAND...: a step the rest needs, its failure the end.
step() {
	what=$1
	shift
	cases=$((cases + 1))
	if ! out=$("$@" 2>&1); then
		fail "$what" "$out"
		printf 'arm64-host: %d cases, %d failed\n' "$cases" "$failures"
		exit 1
	fi
}

step "Debian's arm64 image pulled" timeout 900 "$hull" pull --arch arm64 debian:13 host64/debian
step "Debian's arm64 kernel fetched" timeout 900 "$hull" kernel fetch --arch arm64
step "Debian's amd64 kernel fetched" timeout 900 "$hull" kernel fetch --arch amd64

# What hull's build and suite use, and a user of uid 1000 to run them as,
# the machine's root being the class root and the suite rootless.
cat > "$dir/tools.conf" <<'EOF'
(build (base "host64/debian")
  (steps
    (step (name "tools") (network true)
      (run "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc make libc6-dev libssl-dev zlib1g-dev libzstd-dev liblzma-dev python3 ca-certificates uidmap openssh-client busybox iproute2 mtools zstd qemu-utils openssl curl xz-utils qemu-system-arm qemu-system-x86 passt gcc-x86-64-linux-gnu libc6-dev-amd64-cross kmod e2fsprogs procps erofs-utils"))
    (step (name "tester")
      (run "touch /etc/subuid /etc/subgid && useradd -m -u 1000 -s /bin/sh tester"))))
EOF
step "the build image made" timeout 3600 "$hull" build -f "$dir/tools.conf" host64/tools

boot=$(ls -d "$dir"/cache/hull/kernel-arm64-*/boot | head -1)
# The source read-only, the build in the machine's memory under the
# user's home, which useradd gave the user: the case that found a uid of
# 1000 stored as root's. The machine's root does what a host's own setup
# would have done before the user logs in -- the modules for tun, fuse
# and vhost loaded, and their nodes and /dev/kvm opened to the user as
# udev's rules open them, where the kernel's devtmpfs makes them root's
# alone; and /run/user made -- and the inner half runs as the user. /boot
# is the kernel the machine booted, as a host's is.
cat > "$dir/run/hull.conf" <<EOF
(machine (hostname "arm64-host") (arch arm64) (backend emu)
  (source "host64/tools") (mutable overlay)
  (memory "8G") (cpus 8)
  (env (HOME "/home/tester") (PYTHONDONTWRITEBYTECODE "1"))
  (tmpfs "/tmp")
  (volumes (volume (host "$src") (guest "/src") (mode ro))
           (volume (host "$dir/cache") (guest "/cache") (mode ro))
           (volume (host "$boot") (guest "/boot") (mode ro)))
  (command "/bin/sh" "-c" "uname -m && modprobe -a tun fuse vhost_vsock vhost_net && chmod 666 /dev/kvm /dev/net/tun /dev/fuse /dev/vhost-vsock /dev/vhost-net && mkdir -p /run/user && exec setpriv --reuid=1000 --regid=1000 --init-groups sh /src/test/arm64_host_inner.sh"))
EOF
cases=$((cases + 1))
out=$(timeout 21600 "$hull" run -f "$dir/run/hull.conf" 2>&1)
status=$?
printf '%s\n' "$out" > "$dir/run/machine.log"
if [ "$status" != 0 ]; then
	fail "hull's make test on arm64 exited $status (all of it in $dir/run/machine.log)" \
		"$(printf '%s\n' "$out" | grep -E 'FAIL|failed|Error|error:' | tail -40)"
fi
cases=$((cases + 1))
if ! printf '%s\n' "$out" | grep -qx aarch64; then
	fail "the machine is not aarch64" "$(printf '%s\n' "$out" | head -5)"
fi
# Each suite's summary line. Those of C and of the reader, and the
# small shell suites, must each be there with none failed: what they
# test does not wait on the clock. cli, store, pull and build are timed
# for a host running natively -- a probe waiting twenty seconds, a test
# registry living five minutes -- and under emulation run past those, so
# their counts are said and not held to: run 5, 2026-11-11, found each of
# their failures one of those or a tool the image lacked.
summaries=$(printf '%s\n' "$out" | grep -E '^[a-z0-9_]+: [0-9]+ cases, [0-9]+ failed$')
for suite in sexpr framing spec matrix registry plan wire sha256 verity idmap json runtime \
	segment hostdev iso tree media nss root; do
	cases=$((cases + 1))
	printf '%s\n' "$summaries" | grep -q "^$suite: [0-9]* cases, 0 failed$" ||
		fail "on arm64, $suite did not pass" "$(printf '%s\n' "$summaries" | grep "^$suite:")"
done
printf '%s\n' "$summaries" | sed 's/^/arm64: /'
# hull's qemu engine on the arm64 host: its own arch as a vm, amd64 under
# emu, each machine's probe saying the arch it ran as.
for which in arm:aarch64 x86:x86_64; do
	cases=$((cases + 1))
	if ! printf '%s\n' "$out" | grep -qx "nested: ${which%%:*}: arch=${which#*:}"; then
		fail "on the arm64 host, the ${which%%:*} machine did not run as ${which#*:}" \
			"$(printf '%s\n' "$out" | grep "^nested: ${which%%:*}")"
	fi
done

printf 'arm64-host: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
