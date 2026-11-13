#!/bin/sh
# s390x_host_inner.sh -- test/s390x_host_test.sh's half inside the s390x
# machine, run as its user from the source shared at /src, as
# test/riscv64_host_inner.sh is riscv64's: hull built and `make test` run
# there, then hull's qemu engine on this s390x host, a machine of its own
# arch under emu, the guest having no KVM. What it says is read by the
# host's half: each suite's summary line, and a `nested:` line a machine.

set -u
b=/b/build
cd /src || exit 2
make BUILD_DIR="$b" -j"$(nproc)" all || exit 2
make -i BUILD_DIR="$b" -j"$(nproc)" test
made=$?
echo "inner: make test exited $made"

n=$HOME/nested
rm -rf "$n"
mkdir -p "$n/run" "$n/own/rootfs"
chmod 700 "$n/run"
cp "$b/test/cli/rootfs/probe" "$n/own/rootfs/probe" || exit 2
printf '(machine (backend emu) (source "dir:./rootfs") (tuning (emu (boot_limit 1800))) (command "/probe" "arch"))\n' \
	> "$n/own/hull.conf"
out=$(cd "$n/own" && XDG_RUNTIME_DIR=$n/run XDG_CACHE_HOME=/cache HULL_CORE_IDLE=1 \
	timeout 3600 "$b/hull" run -f hull.conf 2>&1)
echo "nested: own exited $?"
printf '%s\n' "$out" | sed "s/^/nested: own: /"
exit "$made"
