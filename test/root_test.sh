#!/bin/sh
# root_test.sh -- root mode's gate (project.md section 7): hull-root's
# listing and audit log, and the plan's root hash, acceptance and status
# 77. hull-root runs here as the user, as it honours a listing and a log
# of the test's own only when it is not setuid; nothing here needs root,
# a daemon or a machine. docker's grant, run for real, is check-docker's.
#
# Its arguments are build/hull and build/hull-root. Each command runs
# under `timeout`; there is no loop. The files it writes are named in
# FILES, in $BUILD_DIR/test/root.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
helper=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
dir=${BUILD_DIR:?}/test/root
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
cases=0
failures=0
me=$(id -un)

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

# helper STATUS PATTERN ARGS...: hull-root's status and a line of what it
# said, against the test's listing and log.
helper() {
	want=$1
	pattern=$2
	shift 2
	cases=$((cases + 1))
	out=$(HULL_ROOT_CONF="$root/root.conf" HULL_ROOT_LOG="$root/root.log" timeout 10 "$helper" "$@" 2>&1)
	got=$?
	if [ "$got" != "$want" ]; then
		fail "hull-root $* exited $got, wanted $want" "$out"
	elif [ -n "$pattern" ] && ! printf '%s\n' "$out" | grep -Eq -- "$pattern"; then
		fail "hull-root $*: no line matches /$pattern/" "$out"
	fi
}

hash=sha256:$(printf 'a plan' | sha256sum | cut -c1-64)
other=sha256:$(printf 'another' | sha256sum | cut -c1-64)
printf '# the host owner'"'"'s\n%s %s security.allow init # two fields\nsomeone %s segments\n' \
	"$me" "$hash" "$hash" > "$root/root.conf"
: > "$root/root.log"

# Granted only what a line grants this user, this plan and every field;
# an exec or a stop of a listed plan; anything else 77, with the line
# that would list it. Each call a line of the log, the verdict in it.
helper 0 '' start "$hash" security.allow
helper 0 '' start "$hash" security.allow init
helper 77 "^$me $hash security.allow segments\$" start "$hash" security.allow segments
helper 77 "^$me $other security.allow\$" start "$other" security.allow
helper 0 '' exec "$hash"
helper 0 '' stop "$hash"
helper 77 '' stop "$other"
cases=$((cases + 1))
[ "$(wc -l < "$root/root.log")" -eq 7 ] &&
	[ "$(grep -c " $me uid=$(id -u) pid=[0-9]* start $hash granted security.allow" "$root/root.log")" -eq 2 ] &&
	grep -q " start $hash refused security.allow segments\$" "$root/root.log" &&
	grep -q " exec $hash granted\$" "$root/root.log" &&
	grep -q " stop $other refused\$" "$root/root.log" ||
	fail "the audit log is not each call with its verdict" "$(cat "$root/root.log")"
# What is not a hash or a field is refused before anything is read.
helper 1 'not a plan.s hash' start sha256:XYZ security.allow
helper 1 'not a field: bad;one' start "$hash" 'bad;one'
helper 1 'usage' exec "$hash" extra
# A log it cannot write grants nothing.
cases=$((cases + 1))
out=$(HULL_ROOT_CONF="$root/root.conf" HULL_ROOT_LOG="$root/nowhere/root.log" timeout 10 "$helper" start "$hash" security.allow 2>&1)
[ $? = 1 ] && printf '%s\n' "$out" | grep -q 'audit log cannot be written, so nothing is granted' ||
	fail "an unwritable log granted" "$out"

# The plan: a field only root mode honours is refused with the token that
# would, this machine's hash; --root-hash prints it; accepted, the plan is
# ok and ROOT; a token for another plan is status 77, naming this one's
# and the line of the listing that would grant it; changed, the machine
# has a new hash. No daemon is asked: a plan is the planner's alone.
printf '(machine (source "alpine:latest") (backend docker) (security (allow net_admin)) (command "true"))\n' \
	> "$root/cap.conf"
printf '(machine (source "alpine:latest") (backend docker) (security (allow net_raw)) (command "true"))\n' \
	> "$root/changed.conf"
cases=$((cases + 1))
token=$(cd / && timeout 10 "$hull" plan --root-hash -f "$root/cap.conf" 2> /dev/null | sed -n 's/.*: --accept //p')
changed=$(cd / && timeout 10 "$hull" plan --root-hash -f "$root/changed.conf" 2> /dev/null | sed -n 's/.*: --accept //p')
printf '%s\n' "$token" | grep -Eq '^root:sha256:[0-9a-f]{64}$' && [ "$changed" != "$token" ] ||
	fail "a machine's root hash was not printed, or a changed one kept it" "$token $changed"
cases=$((cases + 1))
out=$(cd / && timeout 10 "$hull" plan -f "$root/cap.conf" 2>&1)
[ $? = 1 ] && printf '%s\n' "$out" | grep -qF "security.allow: rejected: docker grants capabilities only to root" &&
	printf '%s\n' "$out" | grep -qF "(--accept $token honours it)" ||
	fail "a root field was not refused with this machine's token" "$out"
cases=$((cases + 1))
out=$(cd / && timeout 10 "$hull" plan --accept "$token" -f "$root/cap.conf" 2>&1)
[ $? = 0 ] && printf '%s\n' "$out" | grep -q 'security.allow: accepted' &&
	printf '%s\n' "$out" | grep -q 'machine on docker (ns): ok, ROOT$' ||
	fail "an accepted root field was not ok and ROOT" "$out"
cases=$((cases + 1))
out=$(cd / && timeout 10 "$hull" plan --accept "$token" -f "$root/changed.conf" 2>&1)
[ $? = 77 ] && printf '%s\n' "$out" | grep -qF "this one is --accept $changed" &&
	printf '%s\n' "$out" | grep -qx "$me ${changed#root:} security.allow" ||
	fail "a changed plan was not status 77 with its hash and listing line" "$out"
# The same in a run, before any core is asked.
cases=$((cases + 1))
out=$(cd / && XDG_RUNTIME_DIR="$root/nocore" timeout 10 "$hull" run --accept "$token" -f "$root/changed.conf" 2>&1)
[ $? = 77 ] && [ ! -e "$root/nocore" ] ||
	fail "a run of a changed plan was not status 77 before its core" "$out"

printf 'root: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
