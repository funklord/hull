#!/usr/bin/env bash
#
# build.sh -- build dosemu2 and its whole chain from pinned git commits,
# as an ordinary user, into one prefix. No root, no package installs.
#
#   build.sh PREFIX [WORK]
#
# PREFIX  where everything is installed; dosemu ends up at PREFIX/bin/dosemu.
#         Paths are compiled in (rpath, FDPP_KERNEL_DIR, DATADIR), so the
#         prefix cannot be moved after the build.
# WORK    scratch for clones and build trees (default PREFIX.work); it is
#         created here, and removed at the end unless KEEP_WORK=1.
#
# Host packages it expects (all Debian 13): build-essential meson ninja-build
# nasm pkgconf autoconf automake bison flex gawk python3-ply libelf-dev
# smallerc universal-ctags gcc-i686-linux-gnu binutils-i686-linux-gnu
# ncurses-bin, and for dosemu2's plugins whatever -dev libraries are present
# (configure drops a plugin whose library is missing).
#
# Order and sources follow upstream's Dockerfile.debian (dosemu2 devel):
# thunk_gen, fdpp, djstub, djdev64, dj64dev, comcom64, dosemu2.

set -euo pipefail

JOBS=${JOBS:-4}

[ $# -ge 1 ] || { echo "usage: $0 PREFIX [WORK]" >&2; exit 2; }
case "$1" in /*) ;; *) echo "PREFIX must be absolute" >&2; exit 2 ;; esac
P=${1%/}
W=${2:-$P.work}
case "$W" in /*) ;; *) echo "WORK must be absolute" >&2; exit 2 ;; esac
W=${W%/}
# W is deleted at the end: refuse anything that could be somebody's tree.
case "$W" in
	""|/|/usr|/usr/*|/etc|/etc/*|"$HOME"|/home|/tmp) echo "refusing WORK=$W" >&2; exit 2 ;;
esac
[ "$W" != "$P" ] || { echo "WORK and PREFIX must differ" >&2; exit 2; }

# repo  commit (pinned 2026-11-18 from each repo's default branch HEAD)
PINS="
stsp/thunk_gen    181683c6ec1a9446a14778c10f384e4cc4c9ea55
dosemu2/fdpp      f2e1f7d0f7eb32847e87a98de8f8cefdbabf6ec7
stsp/djstub       cabde3b3a54a9e9a4553f41958ed86c20a5b56ae
dosemu2/djdev64   0c9ec580ae8964acd5cfcdfa4f2a42daa2fbeb3e
stsp/dj64dev      70de4aa2cf7f32b10d5ead63d51117c64f5d46e1
dosemu2/comcom64  c55586e1a486a5e2930db47a14d0725e62b5feaa
dosemu2/dosemu2   7e7f3da9596ce8035274843f61dc6c71e9733d3a
"

mkdir -p "$P" "$W"

# ccache's preprocessing breaks fdpp's declaration parser; never use it.
export CCACHE_DISABLE=1
export PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/share/pkgconfig"
export PATH="$P/bin:$PATH"

# Clone each repo at exactly its pinned commit, one commit deep.
fetch() {
	local repo=$1 rev=$2 dir="$W/${1#*/}"
	if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$rev" ]; then
		return 0
	fi
	mkdir -p "$dir"
	git -C "$dir" init -q
	timeout 300 git -C "$dir" fetch -q --depth 1 "https://github.com/$repo" "$rev"
	git -C "$dir" checkout -q FETCH_HEAD
	[ "$(git -C "$dir" rev-parse HEAD)" = "$rev" ]
}
echo "$PINS" | while read -r repo rev; do
	[ -n "$repo" ] || continue
	echo "== fetch $repo $rev"
	fetch "$repo" "$rev"
done

# A native file naming plain gcc keeps meson from wrapping it in ccache.
printf "[binaries]\nc = 'gcc'\ncpp = 'g++'\n" > "$W/native.ini"

echo "== thunk_gen"
cd "$W/thunk_gen"
rm -rf -- "$W/thunk_gen/build"
timeout 300 meson setup --native-file "$W/native.ini" --prefix="$P" --libdir=lib build .
timeout 300 meson install -C build

echo "== fdpp"
# What fdpp's configure.meson does, minus its network 'subprojects update':
# the i386 cross file is what lets the kernel subproject build 16/32-bit
# objects while libfdpp stays a host library. A cross build ignores
# PKG_CONFIG_PATH, so the path is given as meson options too.
cd "$W/fdpp"
rm -rf -- "$W/fdpp/build"
timeout 300 meson setup --cross-file fdpp/kernel/toolchain.ini --native-file "$W/native.ini" \
	--buildtype=debugoptimized -Db_lto=true --prefix="$P" --libdir=lib \
	-Dpkg_config_path="$PKG_CONFIG_PATH" -Dbuild.pkg_config_path="$PKG_CONFIG_PATH" build .
timeout 1800 ninja -j"$JOBS" -C build
timeout 300 meson install -C build

echo "== djstub"
cd "$W/djstub"
timeout 600 make -j"$JOBS" prefix="$P"
timeout 300 make prefix="$P" install

echo "== djdev64"
cd "$W/djdev64"
rm -rf -- "$W/djdev64/build"
timeout 300 meson setup --native-file "$W/native.ini" --prefix="$P" --libdir=lib build .
timeout 1200 ninja -j"$JOBS" -C build
timeout 300 meson install -C build

echo "== dj64dev"
cd "$W/dj64dev"
timeout 300 autoreconf -i
rm -rf -- "$W/dj64dev/build"
mkdir build
cd build
timeout 600 ../configure --prefix="$P"
timeout 2400 make -j"$JOBS"
timeout 600 make install

echo "== comcom64"
cd "$W/comcom64"
timeout 1200 make -j"$JOBS" prefix="$P"
timeout 300 make prefix="$P" install

echo "== dosemu2"
# --with-*-dir points the fdpp and dj64 plugins at the prefix: include and
# rpath, FDPP_KERNEL_DIR, and the dirs landlock lets dosemu2 read. The
# rpath in LDFLAGS is for dosemu2.bin itself, which otherwise finds
# libdosemu2.so.0 only through ldconfig or LD_LIBRARY_PATH.
cd "$W/dosemu2"
timeout 600 ./autogen.sh
rm -rf -- "$W/dosemu2/build"
mkdir build
cd build
timeout 1200 ../configure --prefix="$P" LDFLAGS="-Wl,-rpath,$P/lib" \
	--with-fdpp-lib-dir="$P/lib" --with-fdpp-include-dir="$P/include" \
	--with-fdpp-kernel-dir="$P/share/fdpp" --with-fdpp-pkgconf-dir="$P/lib/pkgconfig" \
	--with-dj64-lib-dir="$P/lib" --with-dj64-include-dir="$P/include" \
	--with-dj64-pkgconf-dir="$P/lib/pkgconfig"
timeout 2400 make -j"$JOBS"
timeout 600 make install

echo "== smoke test"
T="$W/smoke"
rm -rf -- "$T"
mkdir -p "$T/home" "$T/dir"
printf 'ver\r\necho DOSEMU-SMOKE-OK\r\nexitemu 7\r\n' > "$T/dir/T.BAT"
set +e
timeout 60 env -i HOME="$T/home" PATH="$P/bin:/usr/bin:/bin" TERM=dumb \
	dosemu -n -I 'sound off' -K "$T/dir" -td -ks -E T.BAT < /dev/null > "$T/out" 2>&1
rc=$?
set -e
tr -d '\r' < "$T/out" | sed -n '/^Welcome/,$p'
if [ "$rc" != 7 ] || ! grep -q '^DOSEMU-SMOKE-OK' "$T/out"; then
	echo "smoke test FAILED (exit $rc)" >&2
	exit 1
fi
echo "smoke test passed"

if [ "${KEEP_WORK:-0}" != 1 ]; then
	rm -rf -- "$W"
fi
du -sh "$P"
