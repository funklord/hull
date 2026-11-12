#!/bin/sh
# riscv64_host_test.sh -- hull on a riscv64 host, with no riscv64 hardware,
# as test/arm64_host_test.sh is for arm64 and in its shape: a riscv64
# Debian booted by hull's own emu (qemu-system-riscv64, TCG, the H
# extension given), hull's source shared into it read-only, hull built
# there natively and its own `make test` run; then hull's qemu engine on
# that host, a vm of its own arch under KVM, which the H extension gives
# it, and an amd64 machine under emu where the image has x86_64's cross
# compiler (test/riscv64_host_inner.sh is that half). Not part of `make
# check`: `make check-riscv64-host` runs it. It needs the network, for
# Debian's riscv64 image, its build tools and Debian's riscv64 and amd64
# kernels, and says so and passes where qemu-system-riscv64 is not
# installed. What it keeps between runs is under
# $BUILD_DIR/test/riscv64-host/data and cache/, so a second run fetches
# nothing; the run directory is made anew. What it starts is under
# `timeout`, the machine given six hours, as arm64's is.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
dir=$(mkdir -p "${BUILD_DIR:?}/test/riscv64-host" && cd "${BUILD_DIR:?}/test/riscv64-host" && pwd)
case $dir in
*/test/riscv64-host) ;;
*) echo "riscv64-host: $dir is not a test/riscv64-host directory" >&2; exit 2 ;;
esac

if ! command -v qemu-system-riscv64 > /dev/null 2>&1; then
	echo "riscv64-host: no qemu-system-riscv64, nothing tried"
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
		printf 'riscv64-host: %d cases, %d failed\n' "$cases" "$failures"
		exit 1
	fi
}

step "Debian's riscv64 image pulled" timeout 900 "$hull" pull --arch riscv64 debian:13 hostrv/debian
step "Debian's riscv64 kernel fetched" timeout 900 "$hull" kernel fetch --arch riscv64
step "Debian's amd64 kernel fetched" timeout 900 "$hull" kernel fetch --arch amd64

# What hull's build and suite use, and a user of uid 1000 to run them as,
# the machine's root being the class root and the suite rootless.
cat > "$dir/tools.conf" <<'EOF'
(build (base "hostrv/debian")
  (steps
    (step (name "tools") (network true)
      (run "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc make libc6-dev libssl-dev zlib1g-dev libzstd-dev liblzma-dev python3 ca-certificates uidmap openssh-client busybox iproute2 mtools zstd qemu-utils openssl curl xz-utils qemu-system-misc qemu-system-x86 passt kmod e2fsprogs procps erofs-utils"))
    (step (name "tester")
      (run "touch /etc/subuid /etc/subgid && useradd -m -u 1000 -s /bin/sh tester"))))
EOF
step "the build image made" timeout 3600 "$hull" build -f "$dir/tools.conf" hostrv/tools

boot=$(ls -d "$dir"/cache/hull/kernel-riscv64-*/boot | head -1)
# The build's own disk, ext4 of the user's: not the machine's memory,
# which runtime_test holds a base must not be, nor its overlay root, on
# which the suites' own overlays cannot stack. Made anew each run.
mke2fs=$(command -v mke2fs || echo /usr/sbin/mke2fs)
rm -f "$dir/run/build.img"
truncate -s 8G "$dir/run/build.img"
step "the build's disk made" timeout 120 "$mke2fs" -q -F -t ext4 -E root_owner=1000:1000 \
	"$dir/run/build.img"

# The source read-only, the build in the machine's memory under the
# user's home, which useradd gave the user: the case that found a uid of
# 1000 stored as root's. The machine's root does what a host's own setup
# would have done before the user logs in -- the modules for tun, fuse
# and vhost loaded, and their nodes and /dev/kvm opened to the user as
# udev's rules open them, where the kernel's devtmpfs makes them root's
# alone; and /run/user made -- and the inner half runs as the user. /boot
# is the kernel the machine booted, as a host's is.
cat > "$dir/run/hull.conf" <<EOF
(machine (hostname "riscv64-host") (arch riscv64) (backend emu)
  (source "hostrv/tools") (mutable overlay)
  (memory "8G") (cpus 8)
  (env (HOME "/home/tester") (PYTHONDONTWRITEBYTECODE "1"))
  (tmpfs "/tmp")
  (volumes (volume (host "$src") (guest "/src") (mode ro))
           (volume (host "$dir/cache") (guest "/cache") (mode ro))
           (volume (host "$boot") (guest "/boot") (mode ro))
           (volume (block "$dir/run/build.img") (guest "/b")))
  (command "/bin/sh" "-c" "uname -m && modprobe -a kvm tun fuse vhost_vsock vhost_net && chmod 666 /dev/kvm /dev/net/tun /dev/fuse /dev/vhost-vsock /dev/vhost-net && mkdir -p /run/user && exec setpriv --reuid=1000 --regid=1000 --init-groups sh /src/test/riscv64_host_inner.sh"))
EOF
cases=$((cases + 1))
out=$(timeout 21600 "$hull" run -f "$dir/run/hull.conf" 2>&1)
status=$?
printf '%s\n' "$out" > "$dir/run/machine.log"
if [ "$status" != 0 ]; then
	fail "hull's make test on riscv64 exited $status (all of it in $dir/run/machine.log)" \
		"$(printf '%s\n' "$out" | grep -E 'FAIL|failed|Error|error:' | tail -40)"
fi
cases=$((cases + 1))
if ! printf '%s\n' "$out" | grep -qx riscv64; then
	fail "the machine is not riscv64" "$(printf '%s\n' "$out" | head -5)"
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
		fail "on riscv64, $suite did not pass" "$(printf '%s\n' "$summaries" | grep "^$suite:")"
done
printf '%s\n' "$summaries" | sed 's/^/riscv64: /'
# hull's qemu engine on the riscv64 host: its own arch as a vm under KVM,
# held to saying it ran as riscv64. And amd64 under emu, said and not
# held to, two layers of TCG costing what arm64's run measured.
cases=$((cases + 1))
if ! printf '%s\n' "$out" | grep -qx "nested: own: arch=riscv64"; then
	fail "on the riscv64 host, its own arch did not run as a vm" \
		"$(printf '%s\n' "$out" | grep "^nested: own")"
fi
printf '%s\n' "$out" | grep '^nested: x86' | grep -v 'machine on' | sed 's/^/riscv64: /'

printf 'riscv64-host: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
