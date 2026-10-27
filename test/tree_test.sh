#!/bin/sh
# tree_test.sh -- engine/tree.c's tar, read back by Python's tarfile and
# GNU tar, which share none of its code, and written twice to the same
# bytes. Its argument is build/test/tree_tar. It writes only under
# $BUILD_DIR/test/tree, which it removes at its start and its end; nothing
# in it loops but over a fixed list, and every command is bounded.

set -u

tar_of=$1
dir=${BUILD_DIR:?}/test/tree
case $dir in
*/test/tree) ;;
*) echo "tree_test: $dir is not a test/tree directory" >&2; exit 2 ;;
esac
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

clear() {
	[ -d "$dir" ] && chmod -R u+rwx "$dir" 2>/dev/null
	rm -rf "${dir:?}"
}

# make_tree ROOT ORDER: the same tree, its entries made in ORDER (forward
# or backward), with the same modes and mtimes either way.
make_tree() {
	timeout 60 python3 - "$1" "$2" <<'PY'
import os, socket, sys
root, order = sys.argv[1], sys.argv[2]
long_dir = "d" * 60 + "/" + "e" * 60
long_name = long_dir + "/" + "f" * 40
files = [
    ("a/file", b"hello\n", 0o640),
    ("a/empty", b"", 0o644),
    ("a/exact", b"x" * 512, 0o644),
    ("a/run", b"#!/bin/sh\n", 0o755),
    ("big", bytes((i * 7 + i // 251) & 0xff for i in range(3000000)), 0o600),
    (long_name, b"a long name\n", 0o644),
]
links = [("a/link", "file"), ("far", "/" + "t" * 200)]
dirs = ["a", "d" * 60, long_dir]
items = [("dir", d) for d in dirs] + [("file", f) for f in files] + \
        [("link", l) for l in links] + [("fifo", "a/pipe"), ("socket", "a/sock")]
os.makedirs(root)
ordered = items if order == "forward" else list(reversed(items))
# Directories first in either order, since their entries need them.
for kind, what in sorted(ordered, key=lambda i: i[0] != "dir"):
    if kind == "dir":
        os.makedirs(os.path.join(root, what), exist_ok=True)
    elif kind == "file":
        path = os.path.join(root, what[0])
        with open(path, "wb") as f:
            f.write(what[1])
        os.chmod(path, what[2])
    elif kind == "link":
        os.symlink(what[1], os.path.join(root, what[0]))
    elif kind == "fifo":
        os.mkfifo(os.path.join(root, what))
    else:
        # Bound by its name from its own directory, since a socket's
        # path is at most 108 bytes and a build directory may be deeper.
        here = os.getcwd()
        os.chdir(os.path.dirname(os.path.join(root, what)))
        s = socket.socket(socket.AF_UNIX)
        s.bind(os.path.basename(what))
        s.close()
        os.chdir(here)
os.chmod(os.path.join(root, "a"), 0o750)
for top, subdirs, names in os.walk(root, topdown=False):
    for name in names + subdirs:
        os.utime(os.path.join(top, name), (1700000000, 1700000000), follow_symlinks=False)
PY
}

clear
mkdir -p "$dir"
make_tree "$dir/one" forward
make_tree "$dir/two" backward
timeout 60 "$tar_of" "$dir/one" > "$dir/one.tar" 2> "$dir/err"
got=$?
cases=$((cases + 1))
[ "$got" = 0 ] || fail "tree_tar exited $got" "$(cat "$dir/err")"

# The same bytes twice, and for the same tree made in the other order.
timeout 60 "$tar_of" "$dir/one" > "$dir/again.tar" 2>/dev/null
cases=$((cases + 1))
cmp -s "$dir/one.tar" "$dir/again.tar" || fail "two writes of one tree differ" ""
timeout 60 "$tar_of" "$dir/two" > "$dir/two.tar" 2>/dev/null
cases=$((cases + 1))
cmp -s "$dir/one.tar" "$dir/two.tar" || fail "the same tree made in another order differs" ""

# What Python's tarfile reads: every entry, its kind, mode, owner, mtime,
# link and content, against the tree itself, in bytewise order.
cases=$((cases + 1))
out=$(timeout 60 python3 - "$dir/one" "$dir/one.tar" <<'PY'
import hashlib, os, stat, sys, tarfile
root, path = sys.argv[1], sys.argv[2]
want = []
for top, subdirs, names in os.walk(root):
    for name in subdirs + names:
        full = os.path.join(top, name)
        want.append(os.path.relpath(full, root))
st_of = {w: os.lstat(os.path.join(root, w)) for w in want}
want = [w for w in want if not stat.S_ISSOCK(st_of[w].st_mode)]
problems = []
with tarfile.open(path, format=tarfile.PAX_FORMAT) as t:
    members = t.getmembers()
    names = [m.name.rstrip("/") for m in members]
    if sorted(names) != sorted(want):
        problems.append("names differ: %s" % sorted(set(names) ^ set(want)))
    for m in members:
        name = m.name.rstrip("/")
        st = st_of.get(name)
        if st is None:
            continue
        if stat.S_IMODE(st.st_mode) != m.mode:
            problems.append("%s: mode %o, wanted %o" % (name, m.mode, stat.S_IMODE(st.st_mode)))
        if (m.uid, m.gid) != (st.st_uid, st.st_gid):
            problems.append("%s: owner %d:%d" % (name, m.uid, m.gid))
        if m.mtime != int(st.st_mtime):
            problems.append("%s: mtime %d" % (name, m.mtime))
        full = os.path.join(root, name)
        if stat.S_ISDIR(st.st_mode) != m.isdir() or stat.S_ISREG(st.st_mode) != m.isfile() or \
           stat.S_ISLNK(st.st_mode) != m.issym() or stat.S_ISFIFO(st.st_mode) != m.isfifo():
            problems.append("%s: the wrong kind" % name)
        if m.issym() and m.linkname != os.readlink(full):
            problems.append("%s: link %s" % (name, m.linkname))
        if m.isfile():
            data = t.extractfile(m).read()
            if hashlib.sha256(data).digest() != hashlib.sha256(open(full, "rb").read()).digest():
                problems.append("%s: content differs" % name)
print("\n".join(problems) or "read back whole, %d entries" % len(members))
PY
)
printf '%s\n' "$out" | grep -q '^read back whole, ' || fail "tarfile read it back wrong" "$out"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q '^read back whole, 12 entries$' ||
	fail "the entries were not the twelve made, less the socket" "$out"

# Each directory's entries in bytewise order, after the directory.
cases=$((cases + 1))
out=$(timeout 60 python3 - "$dir/one.tar" <<'PY'
import sys, tarfile
names = [m.name for m in tarfile.open(sys.argv[1]).getmembers()]
keys = [n.rstrip("/").encode().split(b"/") for n in names]
print("ordered" if keys == sorted(keys) else "out of order: %s" % names)
PY
)
[ "$out" = ordered ] || fail "the entries are not in bytewise order" "$out"

# GNU tar extracts it to the same tree.
cases=$((cases + 1))
mkdir "$dir/out"
if ! out=$(timeout 60 tar -xf "$dir/one.tar" -C "$dir/out" 2>&1); then
	fail "GNU tar refused it" "$out"
else
	# diff compares no fifos, so the pipe, which tarfile checked above,
	# goes from both, as the socket the tar left out goes from the source.
	rm -f "$dir/one/a/sock" "$dir/one/a/pipe" "$dir/out/a/pipe"
	out=$(diff -r --no-dereference "$dir/one" "$dir/out" 2>&1) ||
		fail "GNU tar extracted another tree" "$out"
fi

# A changed byte changes the stream; an unreadable file is named.
printf 'Hello\n' > "$dir/two/a/file"
touch -h -d @1700000000 "$dir/two/a/file"
timeout 60 "$tar_of" "$dir/two" > "$dir/changed.tar" 2>/dev/null
cases=$((cases + 1))
cmp -s "$dir/one.tar" "$dir/changed.tar" && fail "a changed byte left the stream the same" ""
chmod 000 "$dir/two/a/file"
out=$(timeout 60 "$tar_of" "$dir/two" 2>&1 > /dev/null)
got=$?
cases=$((cases + 1))
[ "$got" = 1 ] && printf '%s\n' "$out" | grep -q '^tree_tar: a/file: Permission denied$' ||
	fail "an unreadable file exited $got" "$out"

# xattrs: a file's and a directory's user.* go with them, in pax records,
# as a file capability must, read back by tarfile, which shares nothing
# with hull's writer.
mkdir -p "$dir/xa/d"
printf 'x\n' > "$dir/xa/f"
cases=$((cases + 1))
if timeout 30 python3 -c '
import os, sys
os.setxattr(sys.argv[1] + "/f", "user.note", b"on the file")
os.setxattr(sys.argv[1] + "/d", "user.note", b"on the dir")' "$dir/xa" 2>/dev/null; then
	timeout 60 "$tar_of" "$dir/xa" > "$dir/xa.tar" 2>&1
	said=$(timeout 30 python3 -c '
import sys, tarfile
for m in sorted(tarfile.open(sys.argv[1]), key=lambda m: m.name):
	print(m.name, m.pax_headers.get("SCHILY.xattr.user.note", "-"))' "$dir/xa.tar" 2>&1)
	[ "$said" = "$(printf 'd on the dir\nf on the file')" ] ||
		fail "a tree's xattrs did not go with it" "$said"
else
	printf 'tree: the filesystem holds no user xattrs, so their case is skipped\n'
fi

# --one-fs: in a user and mount namespace of its own, a tmpfs mounted in
# the tree and a file bound over one of it. The mount point is written,
# empty, and what is on the tmpfs and the bound file are left out; the
# rest is as it was.
mkdir -p "$dir/fs/kept" "$dir/fs/mnt"
printf 'kept\n' > "$dir/fs/kept/file"
printf 'mine\n' > "$dir/fs/bound"
printf 'theirs\n' > "$dir/other"
out=$(timeout 60 unshare -rm sh -c '
mount -t tmpfs none "$1/fs/mnt" && printf "hidden\n" > "$1/fs/mnt/inside" &&
mount --bind "$1/other" "$1/fs/bound" && exec "$2" --one-fs "$1/fs"' sh "$dir" "$tar_of" 2>&1 \
	> "$dir/fs.tar")
got=$?
cases=$((cases + 1))
names=$(python3 -c 'import sys, tarfile; print(" ".join(sorted(m.name for m in tarfile.open(sys.argv[1]))))' \
	"$dir/fs.tar" 2>&1)
[ "$got" = 0 ] && [ "$names" = "kept kept/file mnt" ] ||
	fail "--one-fs gave $names, exiting $got" "$out"

clear
printf 'tree: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
