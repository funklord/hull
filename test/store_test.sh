#!/bin/sh
# store_test.sh -- section 3's store through build/hull: commit,
# materialize, diff, images and the store's part of gc, against a store of
# the test's own, in $BUILD_DIR/test/store, which it removes at its start
# and its end.
#
# Its arguments are build/hull and the situc to verify manifests with,
# against manifest/manifest.situ. Every command runs under `timeout`, and
# it loops only over fixed lists.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
situc=$2
dir=${BUILD_DIR:?}/test/store
case $dir in
*/test/store) ;;
*) echo "store_test: $dir is not a test/store directory" >&2; exit 2 ;;
esac
# Writable first: a checkout's or a materialized tree's directories may
# not be, and rm cannot empty one that is not.
[ ! -e "$dir" ] || chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
mkdir -p "$dir/data"
root=$(cd "$dir" && pwd)
XDG_DATA_HOME=$root/data
export XDG_DATA_HOME
store=$root/data/hull/store
# `hull gc` asks a core too: one of the test's own, reaching no daemon,
# so that nothing of the account's is touched.
XDG_RUNTIME_DIR=$root/run
DOCKER_HOST=unix://$root/run/no-docker.sock
HULL_CORE_IDLE=1
export XDG_RUNTIME_DIR DOCKER_HOST HULL_CORE_IDLE
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
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
	out=$(timeout 60 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

# hash_of REF: the manifest a ref names, as `hull images` lists it.
hash_of() {
	timeout 10 "$hull" images | awk -v ref="$1" '$1 == ref { sub("sha256:", "", $2); print $2 }'
}

# listing DIR: what a tree holds, but for mtimes: each entry's type,
# mode, size and link target, each file's sha256, and its user xattrs.
listing() {
	(cd "$1" && find . -mindepth 1 -printf '%y %m %s %l %P\n' | LC_ALL=C sort &&
		find . -type f -exec sha256sum {} + | LC_ALL=C sort -k2 &&
		timeout 10 python3 -c '
import os
for top, dirs, files in sorted(os.walk(".")):
	for name in sorted(dirs + files):
		path = os.path.join(top, name)
		for key in sorted(os.listxattr(path, follow_symlinks=False)):
			if key.startswith("user."):
				print(path, key, os.getxattr(path, key, follow_symlinks=False))
')
}

# A tree of every kind a manifest holds and a commit keeps: a file, an
# empty one, an executable, a hardlink, a symlink, a fifo, directories of
# their own modes, a name of more than ASCII, and an xattr; and a socket,
# which it leaves out.
tree=$root/tree
mkdir -p "$tree/usr/bin" "$tree/etc" "$tree/srv/deep/er"
printf 'hello\n' > "$tree/etc/greeting"
printf '#!/bin/sh\necho hi\n' > "$tree/usr/bin/hi"
chmod 755 "$tree/usr/bin/hi"
# setuid and setgid, which a chown of the user's own file clears.
printf '#!/bin/sh\necho id\n' > "$tree/usr/bin/setid"
chmod 6755 "$tree/usr/bin/setid"
: > "$tree/empty"
ln -s ../etc/greeting "$tree/usr/link"
ln "$tree/etc/greeting" "$tree/etc/again"
mkfifo "$tree/pipe"
printf 'deep\n' > "$tree/srv/deep/er/file"
# A path that is UTF-8 and not ASCII: a-ring, a-umlaut, o-umlaut.
printf 'unicode\n' > "$tree/srv/$(printf '\303\245\303\244\303\266')"
chmod 750 "$tree/etc"
chmod 711 "$tree/srv/deep"
timeout 10 python3 -c '
import os, socket, sys
os.setxattr(sys.argv[1] + "/etc/greeting", "user.note", b"a note")
# Bound by a name relative to its directory, which a deep $BUILD_DIR
# would otherwise make longer than a socket address holds.
os.chdir(sys.argv[1] + "/srv")
s = socket.socket(socket.AF_UNIX)
s.bind("sock")
' "$tree"

expect 0 '^one/tree:1 sha256:[0-9a-f]{64}$' "$hull" commit "$tree" one/tree:1
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q 'srv/sock: a socket, which a manifest cannot hold, left out' ||
	fail "a socket was not said to be left out" "$out"
first=$(hash_of one/tree:1)

# The manifest is the schema's: the whole of it, and each entry, which
# situc does not read inside the whole, verified apart, and the entries
# in the order of their paths.
cases=$((cases + 2))
manifest=$store/manifests/sha256/$first
timeout 30 python3 -c '
import sys
data = open(sys.argv[1], "rb").read()
def atom(at):
	colon = data.index(b":", at)
	n = int(data[at:colon])
	return colon + 1 + n
def form(at):
	if data[at:at + 1] != b"(":
		return atom(at)
	at += 1
	while data[at:at + 1] != b")":
		at = form(at)
	return at + 1
hexed = lambda b: " ".join("%02X" % x for x in b)
out = open(sys.argv[2], "w")
out.write("manifest whole %s\n" % hexed(data))
start = data.index(b"(7:entries") + len(b"(7:entries")
at = start
count = 0
paths = []
while data[at:at + 1] == b"(":
	end = form(at)
	out.write("entry e%d %s\n" % (count, hexed(data[at:end])))
	key = data.index(b"(4:path", at) + len(b"(4:path")
	paths.append(data[data.index(b":", key) + 1:atom(key)])
	count += 1
	at = end
# A.2 orders the entries by path, bytewise, each path once.
sys.exit(0 if paths == sorted(set(paths)) else 1)
' "$manifest" "$root/manifest.vectors" ||
	fail "the manifest's entries are not in the order of their paths" ""
entries=$(grep -c '^entry ' "$root/manifest.vectors")
verified=$(timeout 300 python3 "$situc" verify manifest/manifest.situ "$root/manifest.vectors" 2>&1)
printf '%s\n' "$verified" | grep -q "^situc: $((entries + 1)) vectors conform to manifest/manifest.situ" &&
	[ "$entries" = 15 ] ||
	fail "the manifest, or one of its $entries entries, is not the schema's" "$verified"

# Its identity is the tree's: the same for a copy, and for one whose
# mtimes have moved; another for one whose content has.
cp -a "$tree" "$root/copy"
touch -d '2001-01-01' "$root/copy/etc/greeting" "$root/copy/usr"
expect 0 '' "$hull" commit "$root/copy" one/copy
cases=$((cases + 1))
[ "$(hash_of one/copy)" = "$first" ] ||
	fail "a copy of the tree, its mtimes moved, is another manifest" "$(hash_of one/copy) $first"
objects=$(find "$store/objects" -type f | wc -l)
printf 'changed\n' > "$root/copy/etc/greeting"
expect 0 '1 new' "$hull" commit "$root/copy" one/changed
cases=$((cases + 1))
[ "$(hash_of one/changed)" != "$first" ] &&
	[ "$(find "$store/objects" -type f | wc -l)" = $((objects + 1)) ] ||
	fail "a change made no new manifest, or more than one new object" \
		"$(hash_of one/changed); $objects then $(find "$store/objects" -type f | wc -l)"
# Objects are read-only, executable where their file was.
cases=$((cases + 1))
modes=$(find "$store/objects" -type f -printf '%m\n' | sort -u | tr '\n' ' ')
[ "$modes" = "444 555 " ] || fail "objects have modes $modes, wanted 444 and 555" ""

# Materialized, it is the tree again, but for the socket left out and the
# hardlink, which is two files of the same content.
expect 0 '' "$hull" materialize one/tree:1 "$root/out"
cases=$((cases + 1))
rm -f "$tree/srv/sock"
committed=$(listing "$tree")
got=$(listing "$root/out")
[ "$committed" = "$got" ] || fail "the materialized tree is not the committed one" \
	"$(printf '%s\n' "$committed" > "$root/want"; printf '%s\n' "$got" > "$root/got"; diff "$root/want" "$root/got")"
cases=$((cases + 1))
[ "$(stat -c %i "$root/out/etc/greeting")" != "$(stat -c %i "$root/out/etc/again")" ] ||
	fail "a hardlink came back as one" ""
# Every path is the user's, group too, where a setgid directory above
# would give its own: a machine's namespace maps the user's group alone.
mkdir -p "$root/sgid"
chmod 2775 "$root/sgid"
other=$(id -G | tr ' ' '\n' | grep -vx "$(id -g)" | head -1)
[ -n "$other" ] && chgrp "$other" "$root/sgid"
expect 0 '' "$hull" materialize one/tree:1 "$root/sgid/out"
cases=$((cases + 1))
groups=$(find "$root/sgid/out" -mindepth 1 ! -group "$(id -g)" | head -3)
[ -z "$groups" ] || fail "a materialized path kept a group other than the user's" "$groups"

# What is refused: a target that is not empty, a ref that is not, and a
# name that is not one.
expect 1 'it is not empty' "$hull" materialize one/tree:1 "$root/out"
expect 1 'no such ref' "$hull" materialize one/nope "$root/out2"
expect 1 'a ref is names of' "$hull" commit "$tree" ../out
expect 1 'a ref is names of' "$hull" commit "$tree" 'one/a b'
expect 1 'No such file' "$hull" commit "$root/nowhere" one/nowhere

# What the store holds is checked as it is read: an object that is not
# its name, and a manifest that is not its name.
corrupt() { # FILE: one byte of it changed, the file being read-only
	chmod u+w "$1"
	printf 'X' | dd of="$1" bs=1 seek=0 conv=notrunc 2> /dev/null
	chmod u-w "$1"
}
greeting=$(sha256sum < "$tree/etc/greeting" | cut -d' ' -f1)
object=$store/objects/sha256/$(printf '%.2s' "$greeting")/$greeting
cp -p "$object" "$root/object.saved"
corrupt "$object"
expect 1 'its content is not the object its name says' "$hull" materialize one/tree:1 "$root/bad1"
chmod u+w "$object"
cp -p "$root/object.saved" "$object"
cp -p "$manifest" "$root/manifest.saved"
corrupt "$manifest"
expect 1 'its bytes are not the manifest its name says' "$hull" materialize one/tree:1 "$root/bad2"
chmod u+w "$manifest"
cp -p "$root/manifest.saved" "$manifest"

# A manifest cannot write outside its target: one whose symlink leads out
# and whose next entry goes through it is refused there, and nothing is
# written where it led; nor is a path with `..` taken.
mkdir -p "$root/outside"
crafted() { # NAME CASE: a manifest of CASE, stored, as ref NAME
	timeout 10 python3 -c '
import hashlib, os, sys
store, ref, case, object_hex, outside = sys.argv[1:6]
obj = b"32:" + bytes.fromhex(object_hex)
atom = lambda b: str(len(b)).encode() + b":" + b
reg = lambda path: (b"(5:entry(4:kind3:reg)(4:mode3:420)(6:object" + obj +
                    b")(5:owner4:root)(4:path" + atom(path) + b")(4:size1:6))")
if case == "link":
	entries = (b"(5:entry(4:kind7:symlink)(4:mode3:511)(5:owner4:root)(4:path1:a)(6:target" +
	           atom(outside.encode()) + b"))" + reg(b"a/f"))
elif case == "unsorted":
	entries = reg(b"b") + reg(b"a")
else:
	entries = reg(b"../f")
body = (b"(8:manifest(4:arch5:amd64)(7:entries" + entries +
        b")(6:schema1:1)(5:stats(10:file_count1:0)(12:object_count1:0)(10:total_size1:0)))")
name = hashlib.sha256(body).hexdigest()
with open(os.path.join(store, "manifests/sha256", name), "wb") as f:
	f.write(body)
link = os.path.join(store, "refs", ref)
os.makedirs(os.path.dirname(link), exist_ok=True)
os.symlink("../../manifests/sha256/" + name, link)
' "$store" "$1" "$2" "$greeting" "$root/outside"
}
crafted evil/link link
expect 1 'a/f: its directory is not one beneath the target' "$hull" materialize evil/link "$root/evil1"
crafted evil/up up
expect 1 'an entry that is not one' "$hull" materialize evil/up "$root/evil2"
cases=$((cases + 1))
[ -z "$(ls -A "$root/outside")" ] && [ ! -e "$root/f" ] ||
	fail "a manifest wrote outside its target" "$(ls -A "$root/outside"; ls "$root")"
rm -f "$store/refs/evil/link" "$store/refs/evil/up"

# gc: what no ref reaches goes, and what one still does stays, the
# objects the two trees share among them.
expect 0 '^REF ' "$hull" images
expect 0 'one/changed' "$hull" images
rm -f "$store/refs/one/changed"
# And what a checkout that went left in tmp/, its directories read-only.
mkdir -p "$store/tmp/checkout-left/d"
printf 'x\n' > "$store/tmp/checkout-left/d/f"
chmod 555 "$store/tmp/checkout-left/d" "$store/tmp/checkout-left"
changed=$(sha256sum < "$root/copy/etc/greeting" | cut -d' ' -f1)
expect 0 '^store: removed 3 manifests, 1 objects and 0 checkouts no ref reaches$' "$hull" gc
cases=$((cases + 1))
[ ! -e "$store/tmp/checkout-left" ] || fail "gc left a read-only tree in tmp/" ""
cases=$((cases + 1))
[ ! -e "$store/objects/sha256/$(printf '%.2s' "$changed")/$changed" ] && [ -e "$object" ] &&
	[ -e "$manifest" ] ||
	fail "gc took what a ref reaches, or left what none does" "$(find "$store" -type f | sort)"
expect 0 '' "$hull" materialize one/tree:1 "$root/after"
cases=$((cases + 1))
got=$(listing "$root/after")
[ "$got" = "$committed" ] || fail "a tree materialized after gc is not the tree" \
	"$(printf '%s\n' "$committed" > "$root/want"; printf '%s\n' "$got" > "$root/got"; diff "$root/want" "$root/got")"

# diff: nothing between a tree and its copy, nor between a tree and the
# tree materialized from it, committed again; a manifest named by its hash
# as a ref is.
expect 0 '' "$hull" commit "$root/after" one/after
cases=$((cases + 1))
out=$(timeout 60 "$hull" diff one/tree:1 one/after 2>&1)
got=$?
[ "$got" = 0 ] && [ -z "$out" ] ||
	fail "a materialized tree, committed again, differs from its own: $got" "$out"
cases=$((cases + 1))
out=$(timeout 60 "$hull" diff "sha256:$first" one/copy 2>&1)
got=$?
[ "$got" = 0 ] && [ -z "$out" ] || fail "a manifest named by its hash differs from its copy: $got" "$out"
expect 0 '' "$hull" materialize "sha256:$first" "$root/byhash"
# Each kind of change, said once for its path, in path order: a kind, a
# mode, a content, a path gone and three come, one with a newline in its
# name and one with a quote and a backslash, an xattr, and a symlink's
# target.
cp -a "$root/after" "$root/more"
rm "$root/more/empty" "$root/more/srv/deep/er/file" "$root/more/usr/link"
mkdir "$root/more/empty"
chmod 600 "$root/more/etc/again"
printf 'other\n' > "$root/more/etc/greeting"
printf 'new\n' > "$root/more/srv/new"
printf 'quoted\n' > "$root/more/srv/q\"b\\s"
printf 'two\n' > "$root/more/srv/two
lines"
ln -s ../etc/again "$root/more/usr/link"
timeout 10 python3 -c '
import os, sys
os.setxattr(sys.argv[1], "user.note", b"another")
' "$root/more/usr/bin/hi"
expect 0 '' "$hull" commit "$root/more" one/more
cases=$((cases + 1))
out=$(timeout 60 "$hull" diff one/after one/more 2>&1)
got=$?
mode() { printf '%04o' "0$(stat -c %a "$1")"; }
wanted=$(printf '%s\n' \
	"~ empty: kind reg -> dir, mode $(mode "$root/after/empty") -> $(mode "$root/more/empty")" \
	"~ etc/again: mode $(mode "$root/after/etc/again") -> 0600" \
	'~ etc/greeting: content' \
	'- srv/deep/er/file' \
	'+ srv/new' \
	'+ srv/q"b\134s' \
	'+ srv/two\012lines' \
	'~ usr/bin/hi: xattrs' \
	'~ usr/link: target ../etc/greeting -> ../etc/again')
[ "$got" = 1 ] && [ "$out" = "$wanted" ] ||
	fail "diff said other than each change, or did not exit 1: $got" \
		"$(printf '%s\n' "$wanted" > "$root/want"; printf '%s\n' "$out" > "$root/got"; diff "$root/want" "$root/got")"
# The tree's own arch, said on a line of its own.
expect 0 '' "$hull" commit --arch armhf "$root/after" one/arm
cases=$((cases + 1))
out=$(timeout 60 "$hull" diff one/after one/arm 2>&1)
got=$?
[ "$got" = 1 ] && printf '%s\n' "$out" | grep -Eqx 'arch [a-z0-9_]+ -> armhf' &&
	[ "$(printf '%s\n' "$out" | wc -l)" = 1 ] || fail "diff did not say the arch alone: $got" "$out"
# What diff refuses, with 2: a ref that is not, a hash that is not one, and
# one the store has no manifest of; and no ref is named as a hash is.
expect 2 'no such ref' "$hull" diff one/nope one/copy
expect 2 'a manifest is sha256: and 64 lowercase hex digits' "$hull" diff sha256:abc one/copy
expect 2 'No such file' "$hull" diff \
	sha256:0000000000000000000000000000000000000000000000000000000000000000 one/copy
expect 1 'a ref is names of' "$hull" commit "$tree" sha256:mine
# Nor does it take a manifest whose entries are not in path order, which
# is not one of ours, rather than say a change it could not have.
crafted evil/unsorted unsorted
expect 2 'an entry with no path, or out of order' "$hull" diff one/copy evil/unsorted
rm -f "$store/refs/evil/unsorted"

# manifest show: A.1's readable view, read back by a reader of the test's
# own, apart from hull's, as the canonical bytes the manifest's name is the
# hash of; one/more has a name with a newline and an xattr that is not
# text. The view is text, printable, and an entry a line, so that two views
# diff by entry.
cases=$((cases + 1))
timeout 60 "$hull" manifest show one/more > "$root/view" 2>&1
read_back=$(timeout 30 python3 -c '
import hashlib, sys
t = open(sys.argv[1], "rb").read()
at = 0
def blank():
	global at
	while at < len(t) and t[at:at + 1] in (b" ", b"\n", b"\t"):
		at += 1
def form():
	global at
	blank()
	c = t[at:at + 1]
	if c == b"(":
		at += 1
		out = b"("
		blank()
		while t[at:at + 1] != b")":
			out += form()
			blank()
		at += 1
		return out + b")"
	if c == b"\"":
		at += 1
		value = b""
		while t[at:at + 1] != b"\"":
			if t[at:at + 1] == b"\\":
				value += {b"n": b"\n", b"t": b"\t"}.get(t[at + 1:at + 2], t[at + 1:at + 2])
				at += 2
			else:
				value += t[at:at + 1]
				at += 1
		at += 1
	elif c == b"#":
		end = t.index(b"#", at + 1)
		value = bytes.fromhex(t[at + 1:end].decode())
		at = end + 1
	else:
		start = at
		while at < len(t) and t[at:at + 1] not in b" \n\t()\";":
			at += 1
		value = t[start:at]
	return str(len(value)).encode() + b":" + value
# Text to be read: UTF-8, and no control byte but the line breaks.
text = t.decode("utf-8", "replace")
printable = "\ufffd" not in text and all(c == "\n" or ord(c) >= 0x20 and c != "\x7f" for c in text)
print(hashlib.sha256(form()).hexdigest() if printable else "unprintable")
' "$root/view" 2>&1)
[ "$read_back" = "$(hash_of one/more)" ] &&
	[ "$(grep -c '^    (entry ' "$root/view")" = "$(find "$root/more" -mindepth 1 -printf x | wc -c)" ] ||
	fail "the view does not read back as its manifest, or is not an entry a line" \
		"$read_back $(hash_of one/more); $(head -5 "$root/view")"
# A path is a string, as A.2 has it, though a token would read back too.
cases=$((cases + 1))
grep -q '(path "etc")' "$root/view" || fail "the view gives a path other than as a string" ""
# tag: a second ref to a manifest, by its ref or its hash, replacing what
# the ref named; -d removes a ref and leaves its manifest, for gc.
expect 0 '^one/tagged sha256:[0-9a-f]{64}$' "$hull" tag one/more one/tagged
cases=$((cases + 1))
[ "$(hash_of one/tagged)" = "$(hash_of one/more)" ] || fail "a tag names another manifest" ""
expect 0 '' "$hull" tag "sha256:$first" one/tagged
cases=$((cases + 1))
[ "$(hash_of one/tagged)" = "$first" ] || fail "a tag by hash, over a ref, did not replace it" ""
expect 0 '' "$hull" tag -d one/tagged
cases=$((cases + 1))
[ -z "$(hash_of one/tagged)" ] && [ -e "$store/manifests/sha256/$first" ] ||
	fail "tag -d left the ref, or took its manifest" "$(hash_of one/tagged)"
expect 1 'no such ref' "$hull" tag -d one/tagged
expect 1 'no such ref' "$hull" tag one/nope one/tagged
expect 1 'a ref is names of' "$hull" tag one/more ../out
expect 1 'a ref is names of' "$hull" tag one/more sha256:mine
expect 1 'No such file' "$hull" tag \
	sha256:0000000000000000000000000000000000000000000000000000000000000000 one/tagged
expect 1 'no such ref' "$hull" manifest show one/nope

# The test's core leaves once idle, before its directory goes.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
cases=$((cases + 1))
[ ! -e "$dir" ] || fail "the test's own directory could not be removed" "$(ls -R "$dir" | head)"
printf 'store: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
