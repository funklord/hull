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
mkdir -p "$root/b/base/usr/lib"
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
          (run "probe write /made write /gone write /tmp/scratch dial 127.0.0.1 $port")
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
cat > "$root/b/inroot.conf" <<'EOF'
(build (base "dir:./base") (steps (step (name "inroot") (copy (path (from "./extra") (to "/lib/landed"))))))
EOF
expect 0 '' "$hull" build -f "$root/b/inroot.conf" app/inroot
expect 0 '' "$hull" materialize app/inroot "$root/inroot"
cases=$((cases + 1))
[ "$(cat "$root/inroot/usr/lib/landed" 2> /dev/null)" = extra ] && [ ! -e /usr/lib/landed ] ||
	fail "a copy through the base's symlink to /usr/lib did not land in the tree's" \
		"$(cd "$root/inroot" && find . | head)"
cases=$((cases + 1))
[ -z "$(ls -A "$store/tmp")" ] || fail "a build left its scratch in tmp/" "$(ls "$store/tmp")"

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
