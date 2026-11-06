#!/bin/sh
# build_test.sh -- section 3.6's `hull build` and Appendix E's step
# contract, through build/hull on the chroot engine: a step's tree, its
# copies, its cache, what it leaves out, its network, and its provenance.
#
# Its arguments are build/hull, the probe, which stands for a step's
# /bin/sh, and the situc a built manifest is verified with. Everything is
# under $BUILD_DIR/test/build, which it removes at its start and its end;
# every command runs under `timeout`, and it loops only over fixed lists.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
situc=$3
here=$(cd "$(dirname "$0")" && pwd)
dir=${BUILD_DIR:?}/test/build
case $dir in
*/test/build) ;;
*) echo "build_test: $dir is not a test/build directory" >&2; exit 2 ;;
esac
[ ! -e "$dir" ] || chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
mkdir -p "$dir/data" "$dir/run" "$dir/home" "$dir/b/base/bin" "$dir/b/base/tmp" "$dir/b/src" \
	"$dir/outside"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_DATA_HOME=$root/data
XDG_RUNTIME_DIR=$root/run
DOCKER_HOST=unix://$root/run/no-docker.sock
HULL_CORE_IDLE=1
HOME=$root/home
export XDG_DATA_HOME XDG_RUNTIME_DIR DOCKER_HOST HULL_CORE_IDLE HOME
store=$root/data/hull/store
# The store's directory setgid to a group of the user's that is not their
# own, where the user has one, as a shared or inherited data directory may
# be: a step's scratch under it must still make its tree in the user's own
# group, which a machine's namespace maps, since overlayfs cannot copy up
# a file of an unmapped one. Wherever this suite's tree sits, the case is
# made here rather than left to where it happens to be.
other_group=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
mkdir -p "$root/data"
if [ -n "$other_group" ]; then
	chgrp "$other_group" "$root/data" && chmod g+s "$root/data"
fi
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# expect STATUS PATTERN COMMAND...: exits STATUS, a line matching PATTERN.
expect() {
	want=$1
	pattern=$2
	shift 2
	cases=$((cases + 1))
	out=$(cd / && timeout 120 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

# The base: the probe as /bin/sh, which runs a line `probe STEP ...` as
# the probe's steps, and a symlink out to a directory of the host's.
cp "$probe" "$root/b/base/bin/sh"
cp "$probe" "$root/b/base/probe"
ln -s "$root/outside" "$root/b/base/out"
mkdir -p "$root/b/base/usr/lib" "$root/b/base/dev"
ln -s /usr/lib "$root/b/base/lib"
printf 'hello\n' > "$root/b/src/hello"
chmod 750 "$root/b/src"
printf 'extra\n' > "$root/b/extra"

# A host listener, for the step that has a network and the one that has
# none.
(timeout -k 2 120 python3 -c '
import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(8)
s.settimeout(110)
print(s.getsockname()[1], flush=True)
while True:
	c, _ = s.accept()
	c.sendall(b"the host loopback\n")
	c.close()
' > "$root/listen.out" 2>&1) &
listener=$!
trap 'kill "$listener" 2> /dev/null' EXIT
tries=0
until [ -s "$root/listen.out" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.1
done
port=$(head -1 "$root/listen.out")

cat > "$root/b/hull.conf" <<EOF
(build
  (base "dir:./base")
  (steps
    (step (name "write")
          (run "probe write /made write /gone write /tmp/scratch write /dev/stray kind /dev/null dial 127.0.0.1 $port")
          (discard "/gone"))
    (step (name "copy")
          (copy (path (from "./src") (to "/app")) (path (from "./extra") (to "/etc/extra")))
          (run "probe read /app/hello read /etc/extra"))
    (step (name "online")
          (network true) (refresh "2s")
          (run "probe dial 127.0.0.1 $port"))))
EOF

# The build: each step run, its tree committed, the last tagged.
expect 0 '^app/1 sha256:[0-9a-f]{64}$' "$hull" build -f "$root/b/hull.conf" app/1
built=$out
first=$(printf '%s\n' "$built" | sed -n 's/^app\/1 sha256://p')
cases=$((cases + 1))
printf '%s\n' "$built" | grep -q '^read=hello$' && printf '%s\n' "$built" | grep -q '^read=extra$' ||
	fail "a step did not see its copies" "$built"
# No network unless asked: the first step's dial is the machine's own lo,
# the last step's the host's.
cases=$((cases + 1))
printf '%s\n' "$built" | grep -q "^dial 127\\.0\\.0\\.1:$port=Connection refused\$" &&
	printf '%s\n' "$built" | grep -q "^dial 127\\.0\\.0\\.1:$port=the host loopback\$" ||
	fail "a step's network was not as it said" "$built"

# The tree: what the steps wrote and copied, the copy its source's mode;
# /tmp emptied and a discard dropped; and a write through a symlink of
# the base's, out to the host, kept inside the tree.
expect 0 '' "$hull" materialize app/1 "$root/tree"
check() { # WHAT CONDITION...
	what=$1
	shift
	cases=$((cases + 1))
	"$@" || fail "$what" "$(cd "$root/tree" && find . | sort | head -30)"
}
check "a step's write" [ -e "$root/tree/made" ]
check "a copied directory" [ "$(cat "$root/tree/app/hello")" = hello ]
check "a copied directory's mode" [ "$(stat -c %a "$root/tree/app")" = "$(stat -c %a "$root/b/src")" ]
check "a copied file" [ "$(cat "$root/tree/etc/extra")" = extra ]
check "a discard dropped" [ ! -e "$root/tree/gone" ]
check "/tmp emptied" [ -z "$(ls -A "$root/tree/tmp")" ]
check "/tmp itself kept" [ -d "$root/tree/tmp" ]
# A step's /dev is the machine's, its null the host's device, and what it
# wrote there is not in the tree: as an empty /dev was, a write to
# /dev/null made a file that the build committed.
cases=$((cases + 1))
printf '%s\n' "$built" | grep -q '^kind=chr 1:3$' || fail "a step's /dev/null was not the device" "$built"
check "a step's /dev kept out of its tree" [ -z "$(ls -A "$root/tree/dev")" ]

# A built manifest is the schema's, its provenance A.5's: the build's
# inputs, its parent and its step, and the builder.
cases=$((cases + 1))
printf 'manifest built %s\n' "$(od -An -v -tx1 "$store/manifests/sha256/$first" |
	tr -s ' \n' '  ' | sed 's/^ //; s/ $//' | tr a-f A-F)" > "$root/built.vectors"
verified=$(timeout 300 python3 "$situc" verify "$here/../manifest/manifest.situ" "$root/built.vectors" 2>&1)
printf '%s\n' "$verified" | grep -q '^situc: 1 vectors conform to' ||
	fail "a built manifest is not the schema's" "$verified"
expect 0 '^  \(provenance \(build \(inputs\) \(parent #[0-9a-f]{64}#\) \(step \(step \(name online\)' \
	"$hull" manifest show app/1
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '(builder "hull ' || fail "the provenance names no builder" "$out"

# Built again: the pure steps cached. The network step is cached within
# its refresh period, which a build begun near a period's end may cross,
# so it is not counted here; past a whole period, it alone runs again.
expect 0 '' "$hull" build -f "$root/b/hull.conf" app/1
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^hull: build: step write: cached' &&
	printf '%s\n' "$out" | grep -q '^hull: build: step copy: cached' ||
	fail "a build of nothing changed ran a pure step" "$out"
sleep 2.1
expect 0 '' "$hull" build -f "$root/b/hull.conf" app/1
cases=$((cases + 1))
[ "$(printf '%s\n' "$out" | grep -c ': cached, sha256:')" = 2 ] &&
	printf '%s\n' "$out" | grep -q '^hull: build: step online$' ||
	fail "a network step past its refresh was not run again, or a pure one was" "$out"

# An input changed: the steps before it cached, its step and those after
# run again, the tree another.
printf 'changed\n' > "$root/b/src/hello"
expect 0 '^read=changed$' "$hull" build -f "$root/b/hull.conf" app/2
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^hull: build: step write: cached' &&
	printf '%s\n' "$out" | grep -q '^hull: build: step copy$' ||
	fail "an input's change did not run its step alone again" "$out"
cases=$((cases + 1))
[ "$(printf '%s\n' "$out" | sed -n 's/^app\/2 sha256://p')" != "$first" ] ||
	fail "a changed input made the same tree" "$out"

# --check: every step twice from the base, no cache read, and each step's
# manifests the same; and a step that writes the time, which no two runs
# write the same, named, its difference said, and nothing tagged.
expect 0 '^reproducible: every step the same manifest twice$' \
	"$hull" build --check -f "$root/b/hull.conf" app/checked
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^hull: build: pass 2 of 2$' &&
	! printf '%s\n' "$out" | grep -q ': cached, sha256:' ||
	fail "--check did not run every step twice, uncached" "$out"
# What an earlier step gave a user stays that user's through a step of copies,
# which gives no owner and takes none away: `RUN adduser`, then `COPY`.
printf 'copied\n' > "$root/b/later"
cat > "$root/b/keep.conf" <<'EOF'
(build (base "dir:./base") (steps
  (step (name "give") (run "probe write /kept chown /kept 4321"))
  (step (name "copy") (copy (path (from "later") (to "/later"))))))
EOF
expect 0 '' "$hull" build -f "$root/b/keep.conf" app/keep
expect 0 '\(uid 4321\)\) \(path "kept"\)' "$hull" manifest show app/keep

# A step's (user ...) is whom its run line runs as, as a Dockerfile's USER is.
cat > "$root/b/asuser.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "as") (run "probe uid") (user "4321"))))
EOF
expect 0 '^uid=4321$' "$hull" build -f "$root/b/asuser.conf" app/asuser
cat > "$root/b/stamp.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "pure") (run "probe write /pure"))
                                  (step (name "stamped") (run "probe stamp /stamp"))))
EOF
expect 1 '^step stamped is not reproducible: sha256:[0-9a-f]{64}, then sha256:[0-9a-f]{64}$' \
	"$hull" build --check -f "$root/b/stamp.conf" app/stamped
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^~ stamp: content$' && [ ! -e "$store/refs/app/stamped" ] &&
	! printf '%s\n' "$out" | grep -q '^step pure is not' ||
	fail "--check did not say the step and its difference, or tagged the build" "$out"

# A step's own env, over its parent's, and its own workdir, made in the
# tree where missing; and the build's runtime, laid over the base's as the
# image's: run, a machine on it takes its command and workdir.
cat > "$root/b/rt.conf" <<'EOF'
(build
  (base "dir:./base")
  (steps (step (name "here") (env (GREETING "hi")) (workdir "/work/dir")
               (run "probe env GREETING cwd write made-here")))
  (runtime (cmd "/probe" "say" "from the runtime" "cwd") (env (A "1") (C "3")) (ports "80/tcp")
           (workdir "/work")))
EOF
expect 0 '^GREETING=hi$' "$hull" build -f "$root/b/rt.conf" app/rt
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^cwd=/work/dir$' || fail "a step's workdir was not its own" "$out"
expect 0 '^  \(runtime \(cmd /probe say "from the runtime" cwd\) \(env \(A 1\) \(C 3\)\) \(ports 80/tcp\) \(workdir /work\)\)$' \
	"$hull" manifest show app/rt
expect 0 '' "$hull" materialize app/rt "$root/rt"
cases=$((cases + 1))
[ -e "$root/rt/work/dir/made-here" ] || fail "a step's workdir was not made, or not where it wrote" ""
printf '(machine (source "app/rt") (backend chroot) (hostname "built"))\n' > "$root/b/run.conf"
expect 0 '^from the runtime$' "$hull" run -f "$root/b/run.conf"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^cwd=/work$' || fail "a built image's workdir was not its runtime's" "$out"
# Over a base that has one: the env merged, the build's A over the base's
# and the base's C kept, an empty (cmd) clearing the base's, the ports
# joined and the workdir kept; the manifest still the schema's.
cat > "$root/b/over.conf" <<'EOF'
(build (base "app/rt") (steps) (runtime (cmd) (env (B "2") (A "one")) (ports "53/udp")))
EOF
expect 0 '' "$hull" build -f "$root/b/over.conf" app/over
expect 0 '^  \(runtime \(env \(A one\) \(B 2\) \(C 3\)\) \(ports 53/udp 80/tcp\) \(workdir /work\)\)$' \
	"$hull" manifest show app/over
over=$(timeout 10 "$hull" images | awk '$1 == "app/over" { sub("sha256:", "", $2); print $2 }')
cases=$((cases + 1))
printf 'manifest over %s\n' "$(od -An -v -tx1 "$store/manifests/sha256/$over" |
	tr -s ' \n' '  ' | sed 's/^ //; s/ $//' | tr a-f A-F)" > "$root/over.vectors"
verified=$(timeout 300 python3 "$situc" verify "$here/../manifest/manifest.situ" "$root/over.vectors" 2>&1)
printf '%s\n' "$verified" | grep -q '^situc: 1 vectors conform to' ||
	fail "a manifest with a laid runtime is not the schema's" "$verified"

# A step's own line changed: it and the steps after it run again, its
# parent the same and theirs not.
sed 's|probe write /made write /gone|probe write /made write /other write /gone|' "$root/b/hull.conf" \
	> "$root/b/edited.conf"
expect 0 '' "$hull" build -f "$root/b/edited.conf" app/3
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^hull: build: step write$' &&
	printf '%s\n' "$out" | grep -q '^hull: build: step copy$' ||
	fail "a step's changed line did not run it and the steps after it again" "$out"

# A base the store holds; a step that fails, tagging nothing; a copy to a
# path with `..`; a copy through the base's absolute symlink, followed as
# the tree's own; and one through its symlink to a directory of the
# host's, which the tree has not, refused, nothing written there.
cat > "$root/b/on.conf" <<'EOF'
(build (base "app/1") (steps (step (name "on") (run "probe read /made"))))
EOF
expect 0 '^read=$|^read=Success$' "$hull" build -f "$root/b/on.conf" app/on
cat > "$root/b/fails.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "fails") (run "probe exit 3"))))
EOF
expect 1 'step fails exited 3' "$hull" build -f "$root/b/fails.conf" app/fails
cases=$((cases + 1))
[ ! -e "$store/refs/app/fails" ] || fail "a failed build tagged its ref" ""
cat > "$root/b/up.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "up") (copy (path (from "./extra") (to "/a/../../escaped"))))))
EOF
expect 1 'not a path the tree can hold' "$hull" build -f "$root/b/up.conf" app/up
cat > "$root/b/through.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "through") (copy (path (from "./extra") (to "/out/landed"))))))
EOF
expect 1 '/out/landed: No such file or directory' "$hull" build -f "$root/b/through.conf" app/through
cases=$((cases + 1))
[ -z "$(ls -A "$root/outside")" ] || fail "a copy wrote through a symlink to the host" "$(ls "$root/outside")"
# Owners a step gives are kept, with no file of theirs on the user's disk:
# the step runs above an overlay of its parent, with many uids where the
# user has a range, its tree exported and committed; and the next step,
# on that, is given them back before it runs, so they last. Where one uid
# is mapped there are none to keep, and the step says so.
me=$(id -un)
if grep -q "^$me:" /etc/subuid 2>/dev/null && grep -q "^$me:" /etc/subgid 2>/dev/null &&
	[ -u /usr/bin/newuidmap ] && [ -u /usr/bin/newgidmap ]; then
	cat > "$root/b/owned.conf" <<'EOF'
(build (base "dir:./base")
  (steps (step (name "give") (run "probe write /given chown /given 101"))
         (step (name "look") (run "probe owner /given"))))
EOF
	expect 0 '^owner=101:101 ' "$hull" build -f "$root/b/owned.conf" app/owned
	expect 0 '\(owner \(gid 101\) \(uid 101\)\) \(path "given"\)' "$hull" manifest show app/owned
else
	printf 'build: this user has no subordinate range, so the owners case is skipped\n'
fi

cat > "$root/b/inroot.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "inroot") (copy (path (from "./extra") (to "/lib/landed"))))))
EOF
expect 0 '' "$hull" build -f "$root/b/inroot.conf" app/inroot
expect 0 '' "$hull" materialize app/inroot "$root/inroot"
cases=$((cases + 1))
[ "$(cat "$root/inroot/usr/lib/landed" 2> /dev/null)" = extra ] && [ ! -e /usr/lib/landed ] ||
	fail "a copy through the base's symlink to /usr/lib did not land in the tree's" \
		"$(cd "$root/inroot" && find . | head)"
# A base of another arch: its steps run as it, translated on chroot, and
# what they make is stored as it, a step's programs being the base's.
arm64_probe=${BUILD_DIR:?}/test/arm64/rootfs/probe
if [ -x "$arm64_probe" ] && [ -x /usr/bin/qemu-aarch64 ]; then
	mkdir -p "$root/armbase/bin" "$root/armbase/proc" "$root/armbase/dev"
	cp "$arm64_probe" "$root/armbase/bin/sh"
	cp "$arm64_probe" "$root/armbase/probe"
	expect 0 '^armbase/1 sha256:' "$hull" commit --arch arm64 "$root/armbase" armbase/1
	printf '(build (base "armbase/1") (steps (step (name "arm") (run "probe arch write /made"))))\n' \
		> "$root/b/arm.conf"
	expect 0 '^arch=aarch64$' "$hull" build -f "$root/b/arm.conf" app/arm
	expect 0 '^\(manifest .*\(arch arm64\)|^  \(arch arm64\)' "$hull" manifest show app/arm
else
	echo "build: a base of another arch not measured: no arm64 probe or no qemu-aarch64"
fi
cases=$((cases + 1))
[ -z "$(ls -A "$store/tmp")" ] || fail "a build left its scratch in tmp/" "$(ls "$store/tmp")"

# A multi-stage Dockerfile: hull import keeps every stage, a later one's
# FROM and COPY --from reach an earlier one's tree, by name and by number,
# the last is a scratch image holding only what it copied, and all of it
# is cached as any build is.
mkdir -p "$root/ms/src"
cp -a "$root/b/base" "$root/ms/base"
printf 'hello\n' > "$root/ms/src/app.txt"
cat > "$root/ms/Dockerfile" <<'DOCKERFILE'
FROM --platform=linux/amd64 dir:./base AS build
ENV STAGE=one
WORKDIR /w
COPY src /w/src
FROM build AS tested
COPY src/app.txt /tested
FROM scratch
COPY --from=tested /w/src/app.txt /app/
COPY --from=0 /w/src /all
CMD ["/app/app.txt"]
DOCKERFILE
timeout 10 "$hull" import "$root/ms/Dockerfile" > "$root/ms/hull.conf"
expect 0 '^staged/1 sha256:[0-9a-f]{64}$' "$hull" build -f "$root/ms/hull.conf" staged/1
expect 0 '' "$hull" materialize staged/1 "$root/ms/tree"
check 'a scratch image holds what it copied and nothing else' \
	[ "$(cd "$root/ms/tree" && find . -type f | sort | tr '\n' ' ')" = './all/app.txt ./app/app.txt ' ]
check 'a file copied to a path with a slash is in it' grep -q hello "$root/ms/tree/app/app.txt"
expect 0 'step copy-3: cached' "$hull" build -f "$root/ms/hull.conf" staged/2
printf 'changed\n' > "$root/ms/src/app.txt"
expect 0 '^staged/3 sha256:' "$hull" build -f "$root/ms/hull.conf" staged/3
expect 0 '' "$hull" materialize staged/3 "$root/ms/tree3"
check 'a stage changing reaches what copied from it' grep -q changed "$root/ms/tree3/app/app.txt"
printf '(build (base "scratch") (steps (step (name "s") (copy (path (from "/x") (to "/y") (stage "none"))))))\n' \
	> "$root/ms/none.conf"
expect 1 'stage none: there is no such stage' "$hull" build -f "$root/ms/none.conf" staged/4
printf '(build (base "scratch") (stages (stage (name "a") (base "scratch"))) (steps (step (name "s") (copy (path (from "/../../x") (to "/y") (stage "a"))))))\n' \
	> "$root/ms/up.conf"
expect 1 'a stage.s path holds \.\.' "$hull" build -f "$root/ms/up.conf" staged/5

# hull load: docker save's tar and an OCI layout's, with no registry, the
# image's defaults kept, each blob held to its digest.
mkdir -p "$root/ld"
printf 'from the archive\n' > "$root/ld/content"
python3 -I "$(dirname "$0")/oci_archive.py" docker "$root/ld/saved.tar" "$root/ld/content"
python3 -I "$(dirname "$0")/oci_archive.py" oci "$root/ld/layout.tar" "$root/ld/content"
expect 0 '^test/saved:1 sha256:' "$hull" load "$root/ld/saved.tar"
expect 0 '^loaded/oci sha256:' "$hull" load "$root/ld/layout.tar" loaded/oci
expect 0 '' "$hull" materialize test/saved:1 "$root/ld/tree"
check 'an archive.s layer is the tree' grep -q 'from the archive' "$root/ld/tree/greeting"
expect 0 '' "$hull" materialize loaded/oci "$root/ld/tree2"
check 'and an OCI layout.s, of the host.s platform' grep -q 'from the archive' "$root/ld/tree2/greeting"
expect 0 'SAVED' "$hull" manifest show loaded/oci
python3 -I -c '
import sys
data = bytearray(open(sys.argv[1], "rb").read())
at = data.find(b"from the archive")
data[at] ^= 1
open(sys.argv[1], "wb").write(data)' "$root/ld/saved.tar"
expect 1 'not the digest that names it' "$hull" load "$root/ld/saved.tar" tampered/1
# An image's USER is whom its command runs as, unless the machine says.
python3 -I "$(dirname "$0")/oci_archive.py" user "$root/ld/user.tar" "$probe"
expect 0 '^loaded/user sha256:' "$hull" load "$root/ld/user.tar" loaded/user
printf '(machine (backend chroot) (source "loaded/user"))\n' > "$root/ld/user.conf"
expect 0 '^uid=4321$' "$hull" run -f "$root/ld/user.conf"
expect 0 '\(stop_signal "SIGQUIT"\)' "$hull" plan --resolved -f "$root/ld/user.conf"
printf '(machine (backend chroot) (source "loaded/user") (user "4400"))\n' > "$root/ld/mine.conf"
expect 0 '^uid=4400$' "$hull" run -f "$root/ld/mine.conf"
printf 'nothing' > "$root/ld/short"
expect 1 'not a tar' "$hull" load "$root/ld/short"
gzip -c "$root/ld/layout.tar" > "$root/ld/layout.tar.gz"
expect 1 'uncompressing' "$hull" load "$root/ld/layout.tar.gz"

# ADD unpacks a local tar, as a base image opens, and .dockerignore keeps
# what a COPY of the context leaves out, `!` bringing a file back.
mkdir -p "$root/ad/ctx/logs" "$root/ad/ctx/.git" "$root/ad/ctx/deep/er" "$root/ad/base/etc"
printf 'in the base\n' > "$root/ad/base/etc/from-tar"
printf 'in the app\n' > "$root/ad/base/app"
tar -cf "$root/ad/ctx/rootfs.tar" -C "$root/ad/base" etc
tar -czf "$root/ad/ctx/app.tar.gz" -C "$root/ad/base" app
: > "$root/ad/ctx/.git/config"
: > "$root/ad/ctx/logs/one"
: > "$root/ad/ctx/deep/er/secret"
: > "$root/ad/ctx/a.log"
: > "$root/ad/ctx/keep.log"
: > "$root/ad/ctx/deep/stays"
cat > "$root/ad/ctx/.dockerignore" <<'IGNORE'
# what no image wants
.git
logs/
*.log
!keep.log
**/secret
*.tar
*.tar.gz
IGNORE
cat > "$root/ad/ctx/Dockerfile" <<'DOCKERFILE'
FROM scratch
ADD rootfs.tar /
ADD app.tar.gz /opt/app
COPY . /src
DOCKERFILE
timeout 10 "$hull" import "$root/ad/ctx/Dockerfile" > "$root/ad/ctx/hull.conf"
expect 0 '^added/1 sha256:' "$hull" build -f "$root/ad/ctx/hull.conf" added/1
expect 0 '' "$hull" materialize added/1 "$root/ad/tree"
check 'ADD unpacked a tar at /' grep -q 'in the base' "$root/ad/tree/etc/from-tar"
check 'and a gzipped one at a path' grep -q 'in the app' "$root/ad/tree/opt/app/app"
check 'the context came' [ -f "$root/ad/tree/src/deep/stays" ]
check 'the ignored did not' [ "$(cd "$root/ad/tree/src" && ls -A | LC_ALL=C sort | tr '\n' ' ')" = '.dockerignore Dockerfile deep hull.conf keep.log ' ]
check 'nor a deep match' [ ! -e "$root/ad/tree/src/deep/er/secret" ]
cp -a "$root/b/base" "$root/ad/full"
mkdir "$root/ad/full/held"
: > "$root/ad/full/held/already"
printf 'FROM dir:./full\nADD rootfs.tar /held\n' > "$root/ad/over"
cp "$root/ad/ctx/rootfs.tar" "$root/ad/rootfs.tar"
timeout 10 "$hull" import "$root/ad/over" > "$root/ad/over.conf"
expect 1 'a tar is unpacked into a new or empty directory' "$hull" build -f "$root/ad/over.conf" added/2

# A compose build's args and target reach the Dockerfile, and a service
# of a profile not asked for is left out, as `docker compose up` does.
mkdir -p "$root/ca/sub"
cp -a "$root/b/base" "$root/ca/base"
cp "$probe" "$root/ca/base/probe"
printf 'from the arg\n' > "$root/ca/sub/chosen"
cat > "$root/ca/Dockerfile" <<'DOCKERFILE'
ARG BASE=none
FROM dir:./${BASE} AS first
ARG FILE=wrong
COPY sub/$FILE /greeting
FROM scratch AS after
COPY --from=first /greeting /nothing-runs-here
DOCKERFILE
cat > "$root/ca/compose.yaml" <<'COMPOSE'
services:
  web:
    build:
      context: .
      target: first
      args:
        BASE: base
        FILE: chosen
    command: ["/probe", "read", "/greeting"]
  debug:
    image: nothing/of/this/name
    profiles: [debug]
COMPOSE
limit=120 expect 0 'from the arg' "$hull" up -f "$root/ca/compose.yaml" --backend chroot
limit=120 expect 0 'debug: left out: none of its profiles' \
	"$hull" up -f "$root/ca/compose.yaml" --backend chroot
limit=60 expect 125 'nothing/of/this/name|debug' "$hull" up -f "$root/ca/compose.yaml" \
	--backend chroot --profile debug

# A compose file whose services build: `hull up` imports each Dockerfile,
# builds it and runs the service from the result, as `docker compose up`
# does; a block form with its own Dockerfile too, and a second up cached.
mkdir -p "$root/cp/site/sub"
cp -a "$root/b/base" "$root/cp/site/base"
cp "$probe" "$root/cp/site/base/probe"
printf 'FROM dir:./base\nCOPY sub/greeting /greeting\n' > "$root/cp/site/Dockerfile"
printf 'FROM dir:./base\nCOPY sub/greeting /other\n' > "$root/cp/site/Other.dockerfile"
printf 'from the build\n' > "$root/cp/site/sub/greeting"
cat > "$root/cp/compose.yaml" <<'COMPOSE'
services:
  web:
    build: ./site
    command: ["/probe", "read", "/greeting"]
  api:
    build:
      context: site
      dockerfile: Other.dockerfile
      args:
        A: b
    command: ["/probe", "read", "/other"]
COMPOSE
expect 0 'web +\| read=from the build' sh -c 'cd "$1/cp" && exec "$2" up 2>&1' sh "$root" "$hull"
expect 0 'api +\| read=from the build' sh -c 'cd "$1/cp" && exec "$2" up 2>&1' sh "$root" "$hull"
expect 0 'step copy-1: cached' sh -c 'cd "$1/cp" && exec "$2" up 2>&1' sh "$root" "$hull"
expect 0 'built by hull up from its Dockerfile' "$hull" plan -f "$root/cp/compose.yaml"
expect 0 'source "compose/cp/web"' "$hull" import "$root/cp/compose.yaml"

# A Vagrantfile and a box: the box a tar around a VMDK, as vagrantcloud's
# are, of mke2fs's and qemu-img's making; the file's declarations read, its
# Ruby refused, and a shell provisioner built on the box by `hull up`.
mke2fs=$(command -v mke2fs || ls /usr/sbin/mke2fs /sbin/mke2fs 2>/dev/null | head -1)
if [ -n "$mke2fs" ] && command -v qemu-img > /dev/null 2>&1; then
	mkdir -p "$root/vg/rootfs/sbin" "$root/vg/rootfs/bin" "$root/vg/rootfs/etc" "$root/vg/rootfs/tmp" \
		"$root/vg/box"
	cp "$probe" "$root/vg/rootfs/sbin/init"
	cp "$probe" "$root/vg/rootfs/bin/sh"
	printf 'root:x:0:0:root:/root:/bin/sh\n' > "$root/vg/rootfs/etc/passwd"
	"$mke2fs" -q -F -t ext4 -d "$root/vg/rootfs" "$root/vg/disk.raw" 16M
	qemu-img convert -f raw -O vmdk "$root/vg/disk.raw" "$root/vg/box/box-disk001.vmdk"
	printf '{"provider":"virtualbox"}\n' > "$root/vg/box/metadata.json"
	tar -C "$root/vg/box" -cf "$root/vg/test.box" box-disk001.vmdk metadata.json
	expect 0 '^vagrant/test/box$' "$hull" vagrant box add test/box "$root/vg/test.box"
	expect 0 '^vagrant/test/box' "$hull" images
	expect 1 'is not a box' "$hull" vagrant box add test/none "$root/vg/disk.raw.none"
	mkdir -p "$root/vg/project"
	cat > "$root/vg/project/Vagrantfile" <<'VAGRANTFILE'
# -*- mode: ruby -*-
VAGRANTFILE_API_VERSION = "2"
$setup = <<-SCRIPT
say provisioned
SCRIPT
Vagrant.configure(VAGRANTFILE_API_VERSION) do |config|
  config.vm.box = "test/box"
  config.vm.box_check_update = false
  config.vm.hostname = "vagrantbox"
  config.vm.network "forwarded_port", guest: 80, host: 8088
  config.vm.network :forwarded_port, guest: 53, host: 5353, protocol: "udp"
  config.vm.network "private_network", ip: "192.168.33.10"
  config.vm.synced_folder "./data", "/data"
  config.vm.provider "virtualbox" do |vb|
    vb.memory = "1024"
    vb.cpus = 2
    vb.customize ["modifyvm", :id, "--ioapic", "on"]
  end
  config.vm.provision "shell", inline: $setup
end
VAGRANTFILE
	mkdir -p "$root/vg/project/data"
	expect 1 '' "$hull" plan -f "$root/vg/project/Vagrantfile" --backend chroot
	said=$("$hull" plan --resolved -f "$root/vg/project/Vagrantfile" --backend vm 2>&1)
	check 'a Vagrantfile is read' sh -c 'printf "%s\n" "$1" | grep -q "private_network" &&
		printf "%s\n" "$1" | grep -q "customize"' sh "$said"
	vagrant_up=$(cd "$root/vg/project" && timeout 60 "$hull" up --backend chroot 2>&1)
	check 'a provisioner is built on the box' sh -c 'printf "%s\n" "$1" | grep -q "step provision"' \
		sh "$vagrant_up"
	expect 0 '^vagrant/project/default' "$hull" images
	# Ruby is refused, by line, rather than guessed at.
	for ruby in '3.times do |i|' 'if ENV["X"]' 'config.vm.box = "a#{1}"' 'require "yaml"'; do
		printf 'Vagrant.configure("2") do |config|\n  %s\nend\n' "$ruby" > "$root/vg/Vagrantfile"
		expect 2 'Vagrantfile:2:' "$hull" plan -f "$root/vg/Vagrantfile"
	done
	# Several machines, each with what config. says and its own.
	cat > "$root/vg/Vagrantfile" <<'VAGRANTFILE'
Vagrant.configure("2") do |config|
  config.vm.box = "test/box"
  config.vm.synced_folder ".", "/vagrant", disabled: true
  config.vm.define "web" do |web|
    web.vm.hostname = "web"
  end
  config.vm.define :db do |db|
    db.vm.hostname = "db"
  end
end
VAGRANTFILE
	expect 0 'web on qemu' "$hull" plan -f "$root/vg/Vagrantfile" --backend vm
	expect 0 'db on qemu' "$hull" plan -f "$root/vg/Vagrantfile" --backend vm
else
	echo 'build: a Vagrantfile not measured: no mke2fs or qemu-img'
fi

kill "$listener" 2> /dev/null
wait "$listener" 2> /dev/null
trap - EXIT
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
cases=$((cases + 1))
[ ! -e "$dir" ] || fail "the test's own directory could not be removed" ""
printf 'build: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
