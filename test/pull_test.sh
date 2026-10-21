#!/bin/sh
# pull_test.sh -- section 3.5's `hull pull` and `hull push` against a
# registry of the test's own, test/registry.py, over TLS with a certificate
# the test makes,
# so that nothing reaches the network: the token, the redirect to another
# host, chunks, an index, whiteouts, an opaque directory, a read-only one,
# a symlinked one, a hardlink, a long name, an xattr, a device, a layer not
# compressed, and what a pull refuses. The pulled image is then run.
#
# Its arguments are build/hull, the probe, which the image carries, and the
# situc a pulled manifest is verified with, against manifest/manifest.situ.
# Everything is under $BUILD_DIR/test/pull, removed at its start and its
# end; the registries serve 300 seconds at most and are stopped at the end,
# and every command runs under `timeout`.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
probe=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
situc=$3
here=$(cd "$(dirname "$0")" && pwd)
dir=${BUILD_DIR:?}/test/pull
case $dir in
*/test/pull) ;;
*) echo "pull_test: $dir is not a test/pull directory" >&2; exit 2 ;;
esac
[ ! -e "$dir" ] || chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
mkdir -p "$dir/data" "$dir/run" "$dir/registry" "$dir/wrong"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_DATA_HOME=$root/data
XDG_RUNTIME_DIR=$root/run
DOCKER_HOST=unix://$root/run/no-docker.sock
HULL_CORE_IDLE=1
export XDG_DATA_HOME XDG_RUNTIME_DIR DOCKER_HOST HULL_CORE_IDLE
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
	out=$(timeout 120 "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "$* exited $got, wanted $want" "$out"
	elif ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "$*: no line matches /$pattern/" "$out"
	fi
}

case $(uname -m) in
x86_64) arch=amd64 ;;
aarch64) arch=arm64 ;;
*) arch=$(uname -m) ;;
esac

# A CA of the test's own, and two certificates it signs: the registry's,
# for 127.0.0.1 and localhost, and one for a name the registry is not.
make_cert() { # NAME SAN
	timeout 30 openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
		-subj "/CN=$1" -keyout "$root/$1.key" -out "$root/$1.csr" 2> /dev/null &&
	printf 'subjectAltName=%s\n' "$2" > "$root/$1.ext" &&
	timeout 30 openssl x509 -req -in "$root/$1.csr" -CA "$root/ca.pem" -CAkey "$root/ca.key" \
		-CAcreateserial -days 1 -extfile "$root/$1.ext" -out "$root/$1.pem" 2> /dev/null
}
timeout 30 openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
	-subj /CN=hull-test-ca -keyout "$root/ca.key" -out "$root/ca.pem" 2> /dev/null &&
	make_cert registry 'IP:127.0.0.1,DNS:localhost' &&
	make_cert wrong 'DNS:wrong.invalid' ||
	{ echo "pull_test: the test's certificates could not be made" >&2; exit 2; }
# And a CA nobody signed the registry with.
timeout 30 openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1 \
	-subj /CN=other-ca -keyout "$root/other.key" -out "$root/other.pem" 2> /dev/null

timeout 60 python3 "$here/registry.py" build "$root/registry" "$probe" "$arch" ||
	{ echo "pull_test: the test's images could not be built" >&2; exit 2; }
ln -s ../registry/blobs "$root/wrong/blobs"
ln -s ../registry/repos "$root/wrong/repos"
(exec timeout -k 2 300 python3 "$here/registry.py" serve "$root/registry" "$root/registry.pem" \
	"$root/registry.key" 2> "$root/registry.log") &
server=$!
(exec timeout -k 2 300 python3 "$here/registry.py" serve "$root/wrong" "$root/wrong.pem" \
	"$root/wrong.key" 2> "$root/wrong.log") &
wrong_server=$!
# Each stopped at the end by its pid, timeout's, which passes the TERM on;
# the pid's command is read again first, in case it has gone to another.
stop_servers() {
	for pid in $server $wrong_server; do
		if [ -r "/proc/$pid/cmdline" ] &&
		   tr '\0' ' ' < "/proc/$pid/cmdline" | grep -q '^timeout .*registry\.py serve'; then
			kill "$pid" 2> /dev/null
		fi
	done
	wait "$server" "$wrong_server" 2> /dev/null
}
trap stop_servers EXIT
tries=0
until [ -s "$root/registry/port" ] && [ -s "$root/wrong/port" ] || [ "$tries" -ge 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
port=$(cat "$root/registry/port" 2> /dev/null)
wrong_port=$(cat "$root/wrong/port" 2> /dev/null)
[ -n "$port" ] && [ -n "$wrong_port" ] ||
	{ echo "pull_test: the registry did not start" >&2; cat "$root/registry.log" >&2; exit 2; }
SSL_CERT_FILE=$root/ca.pem
export SSL_CERT_FILE
unset SSL_CERT_DIR
image=127.0.0.1:$port/test/image:1

# The pull: a token for the manifest, the index narrowed to this arch,
# each blob redirected to localhost, which refuses the token, three
# layers, the last not compressed, and the device said and left out.
expect 0 "^127\\.0\\.0\\.1:$port/test/image:1 sha256:[0-9a-f]{64}\$" "$hull" pull "$image"
pulled=$out
cases=$((cases + 1))
printf '%s\n' "$pulled" | grep -q 'layer 3 of 3' &&
	printf '%s\n' "$pulled" | grep -q 'dev/null: a device, which is not made rootless, left out' ||
	fail "the pull did not apply three layers, or did not say the device" "$pulled"
cases=$((cases + 1))
grep -q 'a token sent where it was not given' "$root/registry.log" &&
	fail "the token went to a host that was not the registry" "" || :
expect 0 '' "$hull" materialize "$image" "$root/out"

# What the layers made, in order: a file replaced and its hardlink keeping
# the old content, a whiteout, an opaque directory, a write through a
# symlinked directory and one through an absolute symlink, both in the
# tree, a read-only directory written by a later layer, a long name, an
# xattr, an uncompressed layer's file, and no device.
check() { # WHAT CONDITION...
	what=$1
	shift
	cases=$((cases + 1))
	"$@" || fail "$what" "$(cd "$root/out" && find . | sort | head -40)"
}
check "a file a later layer replaced" [ "$(cat "$root/out/etc/greeting")" = replaced ]
check "the replaced file's mode" [ "$(stat -c %a "$root/out/etc/greeting")" = 600 ]
check "a hardlink to a replaced file kept its content" [ "$(cat "$root/out/etc/again")" = hello ]
check "a whiteout" [ ! -e "$root/out/etc/gone" ]
check "an opaque directory" [ "$(ls "$root/out/opaque" | tr '\n' ' ')" = "early new " ]
check "a file through a symlinked directory" [ "$(cat "$root/out/usr/lib/libx")" = libx ]
check "the symlinked directory" [ "$(readlink "$root/out/lib")" = usr/lib ]
check "a file through an absolute symlink, in the tree's root" \
	[ "$(cat "$root/out/rooted")" = rooted ]
check "a read-only directory's mode" [ "$(stat -c %a "$root/out/ro")" = 555 ]
check "a read-only directory written by a later layer" [ -e "$root/out/ro/added" ]
check "a read-only directory's own file" [ -e "$root/out/ro/inside" ]
check "a long name" [ "$(cat "$root/out"/deep/d*/file)" = long ]
check "an xattr" [ "$(timeout 10 python3 -c 'import os, sys
print(os.getxattr(sys.argv[1], "user.note").decode())' "$root/out/etc/noted" 2>&1)" = "a note" ]
check "an uncompressed layer" [ "$(cat "$root/out/srv/plain")" = plain ]
check "a device made" [ ! -e "$root/out/dev/null" ]

# The config's defaults are A.4's runtime, in canonical order, and A.5's
# provenance says who made it and from where: the six digests fetched,
# index, manifest, config and three layers, and the registry as named. The
# manifest is the schema's, verified apart from the code that wrote it.
expect 0 '^  \(runtime \(cmd from-config cwd env GREETING\) \(entrypoint /probe say\) \(env \(GREETING hi\) \(PATH /bin\)\) \(ports 53/udp 80/tcp\) \(stop_signal SIGTERM\) \(user 1000\) \(volumes /data\) \(workdir /etc\)\)$' \
	"$hull" manifest show "$image"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -Eq "^  \(provenance \(builder \"hull [^\"]+\"\) \(oci \(digests( sha256:[0-9a-f]{64}){6}\) \(registry 127\.0\.0\.1:$port/test/image\)\)\)\$" ||
	fail "the provenance is not the builder, six digests and the registry" "$out"
cases=$((cases + 1))
tree_hex=$(printf '%s\n' "$pulled" | sed -n 's/^.* sha256:\([0-9a-f]\{64\}\)$/\1/p' | tail -1)
printf 'manifest pulled %s\n' "$(od -An -v -tx1 "$store/manifests/sha256/$tree_hex" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//' | tr a-f A-F)" \
	> "$root/pulled.vectors"
verified=$(timeout 300 python3 "$situc" verify "$here/../manifest/manifest.situ" "$root/pulled.vectors" 2>&1)
printf '%s\n' "$verified" | grep -q '^situc: 1 vectors conform to' ||
	fail "the pulled manifest is not the schema's" "$verified"
# A config that names no workdir: `/`, which the schema requires one of.
expect 0 '' "$hull" pull "127.0.0.1:$port/test/bad:real" plain/1
expect 0 '^  \(runtime \(cmd /probe\) \(workdir /\)\)$' "$hull" manifest show plain/1
# A machine that names no command runs the image's, entrypoint and then
# command, in its workdir, with its env; its user is said and not taken.
printf '(machine (source "%s") (backend chroot) (hostname "defaults"))\n' "$image" \
	> "$root/defaults.conf"
expect 0 '^cwd=/etc$' sh -c 'cd / && exec "$0" run -f "$1"' "$hull" "$root/defaults.conf"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -qx 'from-config' && printf '%s\n' "$out" | grep -qx 'GREETING=hi' &&
	printf '%s\n' "$out" | grep -q 'the image runs as 1000, which is not taken' ||
	fail "the image's command, env or user was not as its config says" "$out"
# And what the machine says is its own.
printf '(machine (source "%s") (backend chroot) (hostname "own") %s)\n' "$image" \
	'(workdir "/") (env (GREETING "mine"))' > "$root/own.conf"
expect 0 '^GREETING=mine$' sh -c 'cd / && exec "$0" run -f "$1"' "$hull" "$root/own.conf"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -qx 'cwd=/' || fail "the machine's own workdir did not win" "$out"
# A diff says the runtime and the provenance, against the same tree
# committed again, which has neither, and nothing of its entries.
expect 0 '' "$hull" commit "$root/out" again/1
expect 1 '' "$hull" diff "$image" again/1
cases=$((cases + 1))
[ "$out" = "$(printf 'runtime removed\nprovenance')" ] ||
	fail "diff did not say the runtime and the provenance alone" "$out"

# push: the tree as one layer, its runtime as the config, put under a tag;
# pulled back, it is the tree it was, its provenance alone another. Pushed
# again, the same digest, and no blob sent, the registry having each.
pushed=127.0.0.1:$port/test/pushed:1
expect 0 "^127\\.0\\.0\\.1:$port/test/pushed:1 sha256:[0-9a-f]{64}\$" "$hull" push "$image" "$pushed"
push_digest=$(printf '%s\n' "$out" | sed -n 's/^.* \(sha256:[0-9a-f]\{64\}\)$/\1/p')
uploads=$(wc -l < "$root/registry/uploads")
# The layer as a reader apart from hull's sees it, Python's gzip and
# tarfile: every member root's at time 0, directories as `name/`, the
# xattr there, and the config's diff_id the tar's own hash.
cases=$((cases + 1))
seen=$(timeout 30 python3 -c '
import gzip, hashlib, io, json, os, sys, tarfile
blobs = sys.argv[1]
manifest = json.load(open(os.path.join(blobs, sys.argv[2])))
config = json.load(open(os.path.join(blobs, manifest["config"]["digest"])))
raw = gzip.decompress(open(os.path.join(blobs, manifest["layers"][0]["digest"]), "rb").read())
problems = []
if config["rootfs"]["diff_ids"] != ["sha256:" + hashlib.sha256(raw).hexdigest()]:
	problems.append("diff_id")
names = []
for m in tarfile.open(fileobj=io.BytesIO(raw)):
	names.append(m.name)
	if m.uid or m.gid or m.mtime:
		problems.append("owner or time of " + m.name)
	if m.name == "etc/noted" and m.pax_headers.get("SCHILY.xattr.user.note") != "a note":
		problems.append("xattr")
if "etc/noted" not in names or "ro" not in names:
	problems.append("names " + " ".join(names[:5]))
print(" ".join(problems) or "ok")
' "$root/registry/blobs" "$push_digest" 2>&1)
[ "$seen" = ok ] || fail "the pushed layer, read apart from hull, is not the tree" "$seen"
expect 0 '' "$hull" pull "$pushed" back/1
expect 1 '' "$hull" diff "$image" back/1
cases=$((cases + 1))
[ "$out" = provenance ] || fail "an image pushed and pulled back is another tree" "$out"
expect 0 'the registry has the layer already' "$hull" push "$image" "$pushed"
cases=$((cases + 1))
printf '%s\n' "$out" | grep -q " $push_digest\$" && [ "$(wc -l < "$root/registry/uploads")" = "$uploads" ] ||
	fail "a second push was another image, or sent a blob again" "$out; $uploads then $(wc -l < "$root/registry/uploads")"
# A tree with no runtime pushes with an empty config, and comes back with
# the workdir a pull gives.
expect 0 '' "$hull" push again/1 "127.0.0.1:$port/test/plain:1"
expect 0 '' "$hull" pull "127.0.0.1:$port/test/plain:1" plain/back
expect 1 '' "$hull" diff again/1 plain/back
cases=$((cases + 1))
[ "$out" = "$(printf 'runtime added\nprovenance')" ] ||
	fail "a tree with no runtime did not come back as it went" "$out"
# An object not its name is not pushed: the layer is checked as it is read.
mkdir -p "$root/tiny"
printf 'tiny\n' > "$root/tiny/file"
expect 0 '' "$hull" commit "$root/tiny" tiny/1
tiny=$(sha256sum < "$root/tiny/file" | cut -d' ' -f1)
tiny_object=$store/objects/sha256/$(printf '%.2s' "$tiny")/$tiny
chmod u+w "$tiny_object"
printf 'TINY\n' > "$tiny_object"
expect 1 'its content is not the object its name says' "$hull" push tiny/1 \
	"127.0.0.1:$port/test/tiny:1"
expect 1 'a push names a tag' "$hull" push "$image" "127.0.0.1:$port/test/pushed@$push_digest"
expect 1 'no such ref' "$hull" push nobody/1 "$pushed"
expect 1 'certificate' env SSL_CERT_FILE="$root/other.pem" "$hull" push "$image" "$pushed"

# The image runs, its name its source, as J.1's image ref in the store.
printf '(machine (source "%s") (backend chroot) (hostname "pulled") %s)\n' "$image" \
	'(command "/probe" "read" "/etc/greeting")' > "$root/pulled.conf"
expect 0 '^read=replaced$' sh -c 'cd / && exec "$0" run -f "$1"' "$hull" "$root/pulled.conf"

# Pulled again, nothing is new. By the manifest's digest, the same tree
# and defaults, its provenance alone another, having no index to name.
expect 0 ', 0 new,' "$hull" pull "$image"
manifest_digest=$(printf '%s\n' "$pulled" | sed -n 's/^hull: pull: .* is \(sha256:[0-9a-f]*\)$/\1/p')
expect 0 '^by/digest sha256:[0-9a-f]{64}$' "$hull" pull "127.0.0.1:$port/test/image@$manifest_digest" \
	by/digest
expect 1 '' "$hull" diff "$image" by/digest
cases=$((cases + 1))
[ "$out" = provenance ] || fail "a pull by digest is another tree than by tag" "$out"

# What a pull refuses, each before anything is committed: a layer not
# its digest; a symlink out of the tree written through, nothing written
# where it led; a `..`; a zstd layer; an index with no platform for this
# arch, saying those it has; a config for another arch; a layer longer
# than its size; a manifest that is not its digest; a tag not there;
# a peer no CA here trusts; and a certificate for another name.
bad=127.0.0.1:$port/test/bad
expect 1 'its content is not the digest that names it' "$hull" pull "$bad:corrupt"
expect 1 '' "$hull" pull "$bad:escape"
cases=$((cases + 1))
[ -z "$(ls -A "$root/registry/outside")" ] ||
	fail "a layer wrote outside its tree" "$(ls -A "$root/registry/outside")"
expect 1 "a layer's path with \`.\` or \`..\` in it, refused" "$hull" pull "$bad:dotdot"
expect 1 'a zstd layer, which hull does not yet read' "$hull" pull "$bad:zstd"
expect 1 'linux/s390x, linux/ppc64le' "$hull" pull "$bad:elsewhere"
expect 1 'the image is for s390x' "$hull" pull "$bad:foreign"
expect 1 'its length is not the size the manifest gives' "$hull" pull "$bad:short"
expect 1 'the manifest is not the digest that names it' "$hull" pull "$bad:lying"
expect 1 'not found there' "$hull" pull "127.0.0.1:$port/test/image:nope"
expect 1 'certificate' env SSL_CERT_FILE="$root/other.pem" "$hull" pull "$image" other/ca
expect 1 'certificate|hostname|mismatch' "$hull" pull "127.0.0.1:$wrong_port/test/image:1" wrong/name
expect 1 'its repository is not one' "$hull" pull "127.0.0.1:$port/Test/Image:1"
cases=$((cases + 1))
for ref in other/ca wrong/name; do
	[ ! -e "$store/refs/$ref" ] || fail "a refused pull left a ref, $ref" ""
done
cases=$((cases + 1))
[ -z "$(ls -A "$store/tmp" 2> /dev/null)" ] || fail "a pull left its scratch in tmp/" "$(ls "$store/tmp")"

stop_servers
trap - EXIT
# The test's core leaves once idle, before its directory goes.
tries=0
while [ -S "$XDG_RUNTIME_DIR/hull/core.sock" ] && [ "$tries" -lt 40 ]; do
	tries=$((tries + 1))
	sleep 0.25
done
chmod -R u+w "${dir:?}"
rm -rf "${dir:?}"
cases=$((cases + 1))
[ ! -e "$dir" ] || fail "the test's own directory could not be removed" ""
printf 'pull: %d cases, %d failed\n' "$cases" "$failures"
[ "$failures" = 0 ]
