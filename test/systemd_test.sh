#!/bin/sh
# systemd_test.sh -- systemd as a chroot machine's init, (init PATH), in a
# cgroup delegated to the user (project.md section 4.8). This host's user
# has none -- a login's processes are in a session scope root owns -- so
# the delegation is made where it can be: inside a vm of hull's own, from
# the host's tree, whose guest kernel's root makes a cgroup the user's, as
# systemd's Delegate= does for user@.service, and runs hull's core in it
# as the user. Nothing on the host is given root. Not part of `make
# check`, which needs no KVM: `make check-systemd` runs it.
#
# Its argument is build/hull. The machine inside is Debian's trixie-slim
# with systemd, which `hull build` makes once (its step's network
# refreshed monthly), as a directory, with systemd as PID 1; its workload asks
# PID 1, by systemctl, how far it got, under `timeout`. The vm runs under
# `timeout` too; the guest's script has no loop. It leaves the vm's conf,
# script and the build's, named in FILES, in $BUILD_DIR/test/systemd, the
# tree removed, and the image hull-test/systemd in the user's store.

set -u

hull=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
dir=${BUILD_DIR:?}/test/systemd
mkdir -p "$dir"
root=$(cd "$dir" && pwd)
XDG_RUNTIME_DIR=$(cd "${BUILD_DIR:?}" && pwd)/test/run
export XDG_RUNTIME_DIR
export HULL_CORE_IDLE=1
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cases=0
failures=0

fail() {
	failures=$((failures + 1))
	printf 'FAIL: %s\n' "$1"
	printf '%s\n' "$2" | sed 's/^/    /'
}

if [ ! -r /dev/kvm ] || [ ! -w /dev/kvm ] || [ ! -w /dev/vhost-vsock ]; then
	printf 'systemd: needs /dev/kvm and /dev/vhost-vsock open to this user\n'
	exit 1
fi
# The image: Debian's trixie-slim with systemd, built by hull itself with
# the network for its one step, and its tree made a directory of the
# user's for the machine inside the vm.
cat > "$root/build.conf" <<'CONF'
(build
  (base "docker.io/library/debian:trixie-slim")
  (steps
    (step
      (name "systemd")
      (run "apt-get -o APT::Sandbox::User=root update && apt-get -o APT::Sandbox::User=root install -y --no-install-recommends systemd systemd-sysv")
      (network true)
      (refresh "30d"))))
CONF
if ! built=$(timeout -k 2 900 "$hull" build -f "$root/build.conf" hull-test/systemd 2>&1); then
	printf 'systemd: the image could not be built:\n%s\n' "$built"
	exit 1
fi
tree=$root/tree
case $tree in
*/test/systemd/tree) chmod -R u+w -- "$tree" 2> /dev/null; rm -rf -- "$tree" ;;
*) printf 'systemd: %s is not this suite'"'"'s tree\n' "$tree"; exit 1 ;;
esac
if ! made=$("$hull" materialize hull-test/systemd "$tree" 2>&1); then
	printf 'systemd: the image could not be made a directory:\n%s\n' "$made"
	exit 1
fi
user=$(id -un)
uid=$(id -u)
gid=$(id -g)

# The guest's script, as its root: cgroup2 where it is not mounted, a
# cgroup the user's with its three files, as a manager delegates one, the
# script's own shell moved into it, and then, as the user, hull's core in
# it running the machine; then the control, the shell moved back to the
# root cgroup and a core of its own there, with no cgroup the user may
# write and no manager to ask. The workload waits, bounded, for PID 1's
# socket before asking it anything.
cat > "$root/run.sh" <<EOF
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || mount -t cgroup2 none /sys/fs/cgroup
mkdir -p /sys/fs/cgroup/delegated /tmp/run
chown $uid:$gid /sys/fs/cgroup/delegated /sys/fs/cgroup/delegated/cgroup.procs \
	/sys/fs/cgroup/delegated/cgroup.subtree_control /sys/fs/cgroup/delegated/cgroup.threads /tmp/run
chmod 700 /tmp/run
echo \$\$ > /sys/fs/cgroup/delegated/cgroup.procs && echo delegated=ok
as() { setpriv --reuid $uid --regid $gid --init-groups env HOME=/tmp XDG_RUNTIME_DIR=/tmp/run HULL_CORE_IDLE=1 "\$@"; }
cat > /tmp/s.conf <<CONF
(machine (source "dir:$tree") (backend chroot) (mutable overlay) (network none) (init "/sbin/init")
  (command "/bin/sh" "-c" "timeout 30 sh -c 'until test -S /run/systemd/private; do sleep 1; done'; timeout 60 systemctl is-system-running --wait; echo pid1=\\\$(cat /proc/1/comm); echo cgroup=\\\$(cat /proc/1/cgroup)"))
CONF
chmod 644 /tmp/s.conf
echo "== delegated"
as timeout -k 2 120 $hull run -f /tmp/s.conf 2>&1
echo "status=\$?"
echo "== left=\$(cd /sys/fs/cgroup/delegated && ls -d */ 2> /dev/null)"
echo \$\$ > /sys/fs/cgroup/cgroup.procs
mkdir -p /tmp/run2 && chown $uid:$gid /tmp/run2 && chmod 700 /tmp/run2
echo "== undelegated"
as env XDG_RUNTIME_DIR=/tmp/run2 timeout -k 2 120 $hull run -f /tmp/s.conf 2>&1
echo "status=\$?"
EOF

printf '(machine (source "dir:/") (backend vm) (mutable overlay) (network none) (memory "2G") (cpus 2) (command "/bin/sh" "%s/run.sh"))\n' \
	"$root" > "$root/vm.conf"
out=$(cd / && timeout -k 2 300 "$hull" run -f "$root/vm.conf" 2>&1 < /dev/null)
got=$?

cases=$((cases + 1))
[ "$got" = 0 ] && printf '%s\n' "$out" | grep -qx 'delegated=ok' ||
	fail "the guest did not delegate a cgroup to the user" "exit $got
$out"
# systemd as PID 1 in the machine, in a cgroup namespace whose root is the
# machine's own cgroup, made below the delegated one: its own init.scope
# made there, and the system up, if degraded, as a container's often is.
cases=$((cases + 1))
delegated=$(printf '%s\n' "$out" | sed -n '/^== delegated/,/^status=/p')
printf '%s\n' "$delegated" | grep -qx 'pid1=systemd' &&
	printf '%s\n' "$delegated" | grep -qx 'cgroup=0::/init.scope' &&
	printf '%s\n' "$delegated" | grep -Eqx '(running|degraded)' &&
	printf '%s\n' "$delegated" | grep -qx 'status=0' ||
	fail "systemd did not come up as the machine's init in a delegated cgroup" "$out"
# And the machine's cgroup gone with it, its children too; one systemd
# made in the core's own cgroup, the machine given none of its own, would
# be left here.
cases=$((cases + 1))
printf '%s\n' "$out" | grep -qx '== left=' ||
	fail "the machine's cgroup was left" "$out"
# The control: with no cgroup the user may write, systemd never comes up
# -- as PID 1 it gives up, ending the machine -- which is what the
# delegation buys.
cases=$((cases + 1))
undelegated=$(printf '%s\n' "$out" | sed -n '/^== undelegated/,/^status=/p')
printf '%s\n' "$undelegated" | grep -q '^status=' &&
	! printf '%s\n' "$undelegated" | grep -qx 'status=0' &&
	! printf '%s\n' "$undelegated" | grep -Eqx '(running|degraded)' &&
	! printf '%s\n' "$undelegated" | grep -qx 'cgroup=0::/init.scope' ||
	fail "systemd came up with no cgroup delegated: the delegated case proves nothing" "$out"

case $tree in
*/test/systemd/tree) chmod -R u+w -- "$tree" 2> /dev/null; rm -rf -- "$tree" ;;
esac

printf 'systemd: %u cases, %u failed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
