#!/bin/sh
# arm64_host_inner.sh -- test/arm64_host_test.sh's half inside the arm64
# machine, run as its user from the source shared at /src: hull built and
# `make test` run there, then hull's qemu engine on this arm64 host, a vm
# of its own arch under KVM, which the machine has from the emulated
# CPU's EL2, and an amd64 machine under emu, booting the kernels the host
# fetched into /cache. What it says is read by the host's half: each
# suite's summary line, and a `nested:` line a machine.

set -u
# The build on a disk of its own, /b (test/arm64_host_test.sh says why).
b=/b/build
cd /src || exit 2
# Every suite, a failing one not stopping the next: the host's half
# counts each one's summary line.
make BUILD_DIR="$b" -j"$(nproc)" all || exit 2
make -i BUILD_DIR="$b" -j"$(nproc)" test
made=$?
echo "inner: make test exited $made"

n=$HOME/nested
rm -rf "$n"
mkdir -p "$n/run" "$n/arm/rootfs" "$n/x86/rootfs"
chmod 700 "$n/run"
cp "$b/test/cli/rootfs/probe" "$n/arm/rootfs/probe" || exit 2
if ! x86_64-linux-gnu-gcc -static -Os -o "$n/x86/rootfs/probe" test/probe.c; then
	echo "inner: no amd64 probe built"
	exit 2
fi
printf '(machine (backend vm) (source "dir:./rootfs") (command "/probe" "arch"))\n' \
	> "$n/arm/hull.conf"
# amd64 translated inside an arm64 translated: its kernel alone measured
# 527 s to its root mount here, so it is given half an hour to answer.
printf '(machine (arch amd64) (backend emu) (source "dir:./rootfs") (tuning (emu (boot_limit 1800))) (command "/probe" "arch"))\n' \
	> "$n/x86/hull.conf"
for which in arm x86; do
	out=$(cd "$n/$which" && XDG_RUNTIME_DIR=$n/run XDG_CACHE_HOME=/cache HULL_CORE_IDLE=1 \
		timeout 3600 "$b/hull" run -f hull.conf 2>&1)
	echo "nested: $which exited $?"
	printf '%s\n' "$out" | sed "s/^/nested: $which: /"
done
exit "$made"
