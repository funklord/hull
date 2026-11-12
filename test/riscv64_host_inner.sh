#!/bin/sh
# riscv64_host_inner.sh -- test/riscv64_host_test.sh's half inside the
# riscv64 machine, run as its user from the source shared at /src, as
# test/arm64_host_inner.sh is arm64's: hull built and `make test` run
# there, then hull's qemu engine on this riscv64 host, a vm of its own
# arch under KVM, which the machine has from the emulated CPU's H
# extension, and an amd64 machine under emu where x86_64's cross compiler
# is in the image. What it says is read by the host's half: each suite's
# summary line, and a `nested:` line a machine.

set -u
b=/b/build
cd /src || exit 2
make BUILD_DIR="$b" -j"$(nproc)" all || exit 2
make -i BUILD_DIR="$b" -j"$(nproc)" test
made=$?
echo "inner: make test exited $made"

n=$HOME/nested
rm -rf "$n"
mkdir -p "$n/run" "$n/own/rootfs" "$n/x86/rootfs"
chmod 700 "$n/run"
cp "$b/test/cli/rootfs/probe" "$n/own/rootfs/probe" || exit 2
printf '(machine (backend vm) (source "dir:./rootfs") (command "/probe" "arch"))\n' \
	> "$n/own/hull.conf"
which_ones=own
if x86_64-linux-gnu-gcc -static -Os -o "$n/x86/rootfs/probe" test/probe.c 2> /dev/null; then
	printf '(machine (arch amd64) (backend emu) (source "dir:./rootfs") (tuning (emu (boot_limit 1800))) (command "/probe" "arch"))\n' \
		> "$n/x86/hull.conf"
	which_ones="own x86"
else
	echo "nested: x86: not tried, no x86_64 cross compiler in the image"
fi
for which in $which_ones; do
	out=$(cd "$n/$which" && XDG_RUNTIME_DIR=$n/run XDG_CACHE_HOME=/cache HULL_CORE_IDLE=1 \
		timeout 3600 "$b/hull" run -f hull.conf 2>&1)
	echo "nested: $which exited $?"
	printf '%s\n' "$out" | sed "s/^/nested: $which: /"
done
exit "$made"
