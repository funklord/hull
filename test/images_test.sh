#!/bin/sh
# images_test.sh -- published images run as they are (project.md section
# 4.11), where a suite's own tree cannot say what a real one needs. Not part
# of `make check`, needing the network and qemu: `make check-images` runs it.
# Its argument is build/hull. nginx-unprivileged's USER and STOPSIGNAL, and a
# compose project of redis and postgres, on chroot and on a vm, the one file
# with one flag changed. Tags and not digests, so a new release is met as a
# user meets it: a failure here may be theirs. It writes only under
# $BUILD_DIR/test/images, removed at its start and left on failure, each run
# under `timeout`, and the core is the test's own.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/images
case $dir in
*/test/images) ;;
*) printf 'images: refusing to clear %s\n' "$dir"; exit 1 ;;
esac
rm -rf "$dir"
mkdir -p "$dir/run" "$dir/nginx" "$dir/app"
chmod 700 "$dir/run"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$root/run
XDG_DATA_HOME=$root/data
XDG_CACHE_HOME=$root/cache
HULL_CORE_IDLE=5
export XDG_RUNTIME_DIR XDG_DATA_HOME XDG_CACHE_HOME HULL_CORE_IDLE
unset DISPLAY
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

says() {
	cases=$((cases + 1))
	printf '%s\n' "$2" | grep -qF -- "$3" || fail "$1" "$2"
}

# address ID: the machine's, once it is running, or nothing.
address() {
	n=0
	found=
	while [ "$n" -lt 150 ] && [ -z "$found" ]; do
		found=$("$hull" ps 2> /dev/null | awk -v id="$1" '$1 == id && $5 == "running" { print $2 }')
		[ -n "$found" ] || sleep 0.2
		n=$((n + 1))
	done
	printf '%s' "$found"
}

for image in nginxinc/nginx-unprivileged:alpine redis:7-alpine postgres:16-alpine; do
	if ! timeout 300 "$hull" pull "$image" "$image" > /dev/null 2>&1; then
		printf 'images: %s not fetched, nothing tried\n' "$image"
		exit 0
	fi
done

for backend in chroot vm; do
	# nginx: a USER of its own, its log a link to /dev/stderr, and SIGQUIT.
	cat > "$root/nginx/hull.conf" <<CONF
(machine (backend $backend) (source "nginxinc/nginx-unprivileged:alpine")
  (ports (port (host 8080) (guest 8080))))
CONF
	(cd "$root/nginx" && timeout 90 "$hull" run -f hull.conf > "$root/nginx.$backend" 2>&1
	 printf 'ended %s\n' "$?" >> "$root/nginx.$backend") &
	pid=$!
	at=$(address nginx)
	n=0
	page=
	while [ "$n" -lt 40 ] && [ -z "$page" ]; do
		page=$(curl -s -m 2 "http://$at:8080/" 2> /dev/null)
		[ -n "$page" ] || sleep 1
		n=$((n + 1))
	done
	says "$backend: nginx answers at its address" "$page" 'Welcome to nginx'
	says "$backend: as its USER" "$(timeout 15 "$hull" exec nginx -- id 2>&1)" 'uid=101'
	timeout 30 "$hull" stop nginx > /dev/null 2>&1
	wait "$pid"
	says "$backend: stopped by its own signal" "$(cat "$root/nginx.$backend")" 'signal 3 (SIGQUIT)'
	says "$backend: and ended 0" "$(tail -1 "$root/nginx.$backend")" 'ended 0'

	# A compose project: rootless postgres dropping to its own user, redis
	# from the host, and one finding the other by its service name.
	cat > "$root/app/compose.yaml" <<'COMPOSE'
services:
  cache:
    image: redis:7-alpine
    ports:
      - "6380:6379"
  db:
    image: postgres:16-alpine
    environment:
      POSTGRES_PASSWORD: secret
    stop_grace_period: 20s
COMPOSE
	(cd "$root/app" && timeout 150 "$hull" up --backend "$backend" > "$root/up.$backend" 2>&1
	 printf 'ended %s\n' "$?" >> "$root/up.$backend") &
	pid=$!
	n=0
	while [ "$n" -lt 90 ] && [ "$(grep -c 'ready to accept connections' "$root/up.$backend" 2> /dev/null)" -lt 2 ]; do
		sleep 1
		n=$((n + 1))
	done
	out=$(timeout 15 "$hull" exec db -- psql -U postgres -tAc \
		'create table t(x int); insert into t values (42); select x, current_user from t' 2>&1)
	says "$backend: postgres takes SQL as its own user" "$out" '42|postgres'
	at=$(address cache)
	out=$( { printf 'PING\r\nQUIT\r\n'; sleep 1; } | timeout 4 nc -w 2 "$at" 6380 2>&1)
	says "$backend: redis answers from the host" "$out" '+PONG'
	out=$(timeout 15 "$hull" exec db -- sh -c '{ printf "PING\r\n"; sleep 1; } | nc -w 2 cache 6379' 2>&1)
	says "$backend: one finds the other by its service name" "$out" '+PONG'
	timeout 40 "$hull" stop db > /dev/null 2>&1
	timeout 40 "$hull" stop cache > /dev/null 2>&1
	wait "$pid"
	says "$backend: postgres shut down clean" "$(cat "$root/up.$backend")" 'database system is shut down'
	says "$backend: and the project ended 0" "$(tail -1 "$root/up.$backend")" 'ended 0'
	sleep 3
done

printf 'images: %s cases, %s failed\n' "$cases" "$failures"
[ "$failures" = 0 ] && rm -rf "$dir"
[ "$failures" = 0 ]
