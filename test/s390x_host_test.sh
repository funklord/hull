#!/bin/sh
# s390x_host_test.sh -- hull on an s390x host, big-endian, with no s390x
# hardware, as test/riscv64_host_test.sh is for riscv64 and in its shape:
# an s390x Debian booted by hull's own emu (qemu-system-s390x, TCG),
# hull's source shared into it read-only, hull built there natively and
# its own `make test` run, so every byte-exact format hull reads and
# writes is checked on a host of the other byte order; then hull's qemu
# engine on that host, a machine of its own arch under emu -- under KVM
# it would be a vm, but qemu's TCG gives an s390x guest no SIE to run
# KVM with (test/s390x_host_inner.sh is that half). Not part of `make
# check`: `make check-s390x-host` runs it. It needs the network, for
# Debian's s390x image, its build tools and Debian's s390x kernel, and
# says so and passes where qemu-system-s390x is not installed. What it
# keeps between runs is under $BUILD_DIR/test/s390x-host/data and cache/,
# so a second run fetches nothing; the run directory is made anew. What
# it starts is under `timeout`, the machine given eight hours.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
dir=$(mkdir -p "${BUILD_DIR:?}/test/s390x-host" && cd "${BUILD_DIR:?}/test/s390x-host" && pwd)
case $dir in
*/test/s390x-host) ;;
*) echo "s390x-host: $dir is not a test/s390x-host directory" >&2; exit 2 ;;
esac

if ! command -v qemu-system-s390x > /dev/null 2>&1; then
	echo "s390x-host: no qemu-system-s390x, nothing tried"
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
		printf 's390x-host: %d cases, %d failed\n' "$cases" "$failures"
		exit 1
	fi
}

step "Debian's s390x image pulled" timeout 900 "$hull" pull --arch s390x debian:13 host390/debian
step "Debian's s390x kernel fetched" timeout 900 "$hull" kernel fetch --arch s390x

# What hull's build and suite use, and a user of uid 1000 to run them as,
# the machine's root being the class root and the suite rootless.
cat > "$dir/tools.conf" <<'EOF'
(build (base "host390/debian")
  (steps
    (step (name "tools") (network true)
      (run "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc make libc6-dev libssl-dev zlib1g-dev libzstd-dev liblzma-dev python3 ca-certificates uidmap openssh-client busybox iproute2 mtools zstd qemu-utils openssl curl xz-utils qemu-system-s390x passt kmod e2fsprogs procps erofs-utils"))
    (step (name "tester")
      (run "touch /etc/subuid /etc/subgid && useradd -m -u 1000 -s /bin/sh tester"))))
EOF
step "the build image made" timeout 3600 "$hull" build -f "$dir/tools.conf" host390/tools

boot=$(ls -d "$dir"/cache/hull/kernel-s390x-*/boot | head -1)
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
# alone, KVM's among them where there is one; and /run/user made -- and the
# inner half runs as the user. /boot is the kernel the machine booted, as
# a host's is.
cat > "$dir/run/hull.conf" <<EOF
(machine (hostname "s390x-host") (arch s390x) (backend emu)
  (source "host390/tools") (mutable overlay)
  (memory "8G") (cpus 8)
  (env (HOME "/home/tester") (PYTHONDONTWRITEBYTECODE "1"))
  (tmpfs "/tmp")
  (volumes (volume (host "$src") (guest "/src") (mode ro))
           (volume (host "$dir/cache") (guest "/cache") (mode ro))
           (volume (host "$boot") (guest "/boot") (mode ro))
           (volume (block "$dir/run/build.img") (guest "/b")))
  (command "/bin/sh" "-c" "uname -m && modprobe -a tun fuse vhost_vsock vhost_net; modprobe kvm; for d in /dev/kvm /dev/net/tun /dev/fuse /dev/vhost-vsock /dev/vhost-net; do [ -e \$d ] && chmod 666 \$d; done; mkdir -p /run/user && exec setpriv --reuid=1000 --regid=1000 --init-groups sh /src/test/s390x_host_inner.sh"))
EOF
cases=$((cases + 1))
out=$(timeout 28800 "$hull" run -f "$dir/run/hull.conf" 2>&1)
status=$?
printf '%s\n' "$out" > "$dir/run/machine.log"
if [ "$status" != 0 ]; then
	fail "hull's make test on s390x exited $status (all of it in $dir/run/machine.log)" \
		"$(printf '%s\n' "$out" | grep -E 'FAIL|failed|Error|error:' | tail -40)"
fi
cases=$((cases + 1))
if ! printf '%s\n' "$out" | grep -qx s390x; then
	fail "the machine is not s390x" "$(printf '%s\n' "$out" | head -5)"
fi
# Each suite's summary line. Those of C and of the reader, and the
# small shell suites, must each be there with none failed: what they
# test does not wait on the clock, and the byte order is what this host
# is for. cli, store, pull and build are timed
# for a host running natively -- a probe waiting twenty seconds, a test
# registry living five minutes -- and under emulation run past those, so
# their counts are said and not held to: run 5, 2026-11-11, found each of
# their failures one of those or a tool the image lacked.
summaries=$(printf '%s\n' "$out" | grep -E '^[a-z0-9_]+: [0-9]+ cases, [0-9]+ failed$')
for suite in sexpr framing spec matrix registry plan wire sha256 verity idmap json runtime \
	segment hostdev iso tree media nss root; do
	cases=$((cases + 1))
	printf '%s\n' "$summaries" | grep -q "^$suite: [0-9]* cases, 0 failed$" ||
		fail "on s390x, $suite did not pass" "$(printf '%s\n' "$summaries" | grep "^$suite:")"
done
printf '%s\n' "$summaries" | sed 's/^/s390x: /'
# hull's qemu engine on the s390x host: its own arch under emu, held to
# saying it ran as s390x.
cases=$((cases + 1))
if ! printf '%s\n' "$out" | grep -qx "nested: own: arch=s390x"; then
	fail "on the s390x host, its own arch did not run under emu" \
		"$(printf '%s\n' "$out" | grep "^nested: own")"
fi

printf 's390x-host: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
