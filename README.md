# hull

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>

Some software can only be tested as part of a whole system: a daemon
that configures network interfaces and routes, an installer, an init
script, a program that loads kernel modules, a protocol that has to
survive a lossy link between five hosts. Testing it means having those
systems -- one machine, or a network of them, in a known state and
rebuilt for every run -- and each usual way of getting them covers only
part of the job. Containers start in milliseconds but share the host's
kernel; virtual machines have a kernel of their own but are set up in a
different tool; emulators reach other processors in a third; and wiring
several of them into a network to test against is a project of its
own, usually needing root. Older software has the same need from the
other side: it runs only on the system it was written for.

Hull runs machines from one short description. A machine is an ordinary
directory of files and a spec, and the same spec runs as a container on
the host's kernel, as a virtual machine with a kernel of its own, or
under full emulation, which can stand in for another processor or an
older system -- chosen by changing one line. Whichever it runs on, a
machine keeps the same address and the same way in, and where one way
cannot honour something the spec asks for, hull says so before anything
starts rather than letting the machine behave differently. Machines can
be joined into networks hull switches itself, whose links can be made
to lose, delay, reorder or damage traffic on purpose, and watched frame
by frame. None of it needs root or a daemon kept running, and published
container images can be used as they are, though a directory is enough.

# Emulators, virtual machines and containers are *not* for security

Hull is not a security tool. Containers, virtual machines and emulators
are often offered as a safe place to run a program you do not trust,
and the promise mostly does not hold. A machine has to share things
with its host to be useful -- a directory, a network, a device, and
always the processor and memory underneath -- and every one of those is
a way through. The software that draws the line has faults of its own,
and ways out of containers, virtual machines and emulators alike are
found every year. In practice, containers, virtual machines and
emulators often make matters worse: one host carrying many
machines is a single point of failure for all of them, and machines on
one host are joined to each other by more I/O -- virtual networks,
shared storage, the container runtime, hypervisor or emulator itself --
than programs on separate hardware, or even programs side by side on
one computer, each running as its own ordinary user rather than as
root.

The difference is in the interfaces. An operating system's interfaces,
and the line between one user and another, were designed for security
from the start. Virtual hardware was not: it began as a copy of devices
that only ever had to trust the driver talking to them, and over time
it is pulled between that legacy, the speed people want from it, and
whatever security can be fitted in around the two.

Part of the claim is true: a machine given no host files and no network
leaves a program less to reach, and a virtual machine's kernel keeps
some of its own faults from the host's. But that part comes with upkeep
that is easy to underestimate: a second system to keep patched, kernel
included, the program running it to keep patched as well, and settings
that undo the arrangement if one of them is wrong. The simpler measures
usually do more for less:

- not running what you do not trust;
- running a service as its own user that can read only what it needs;
- keeping software up to date;
- putting what truly matters on a computer of its own.

Use hull to test and to simulate, and if you use it to keep something
out, know which of these properties you are relying on and what it
costs to keep it true.

# How hull differs from its neighbours

Hull is not one more container runtime. Docker and Podman run containers;
Incus runs containers and virtual machines from images, through a
daemon running as root; Vagrant and libvirt set up virtual machines;
containerlab and Mininet build test networks, as root. Hull brings
their testing uses together in one file, and today adds what none of
them has:

- **One file, three ways to run it.** `--backend vm` or `--backend emu`
  moves a machine from namespaces on the host's kernel to a KVM guest or
  to TCG emulation; `--backend docker` hands it to a Docker daemon where
  one is wanted. A project copied with `rsync` runs unchanged on docker,
  vm and emu, with the same output and its port at the same address,
  which `make check-flip` holds it to.
- **A directory is a machine.** Point the spec at a directory and run;
  no image is built or stored. Images are there when wanted: `hull pull`
  and `hull push` speak to any OCI registry, `hull build` caches each
  step by its parent, its instructions and its inputs, and the store is
  plain files and readable manifests, which `hull diff` compares entry
  by entry.
- **No root, and no daemon.** Each user gets a small core that starts
  when needed and leaves when idle; the chroot and qemu engines run as
  the user. Packages install inside a machine with their owners kept,
  by the user's subordinate id range, and a stored image keeps them.
  `hull doctor` names any one-time setting a host is missing and the
  line that gives it.
- **Differences reported, not discovered.** `hull plan` lists every
  field of a spec as native, emulated at a stated cost, or rejected with
  the reason, for the backend chosen, before anything runs.
- **Networks to test against.** Machines on chroot and on vms join one
  layer-2 segment, switched by hull, as the user. Given a subnet, they
  are addressed, named to each other and answered by hull's own DNS.
  Each link can lose, delay, reorder, duplicate, corrupt or rate-limit
  traffic in netem's terms, changed while running; frames can be
  dropped, rewritten or replayed by pattern, or injected; and any link
  can be captured to a pcap file, with counts of what happened to every
  frame.
- **A kernel of its own when the test needs one.** A vm loads the host's
  modules, `mac80211_hwsim` radios or `pppoe`, or boots a kernel given
  as a file or carried by the image, and the image's own init can run
  as PID 1 on a vm or on chroot.
- **The host's devices.** USB by vendor and product or by port, serial
  and parallel ports, and PCI by VFIO, each held by one machine at a
  time.
- **Built for scripts.** A machine's exit status and output are `hull
  run`'s, and a killed `hull run`, `SIGKILL` included, takes its machine
  with it, where a Docker container belongs to the daemon and outlives
  the job that started it.

# A first machine

A rootless runtime for machines to test against, built on plain files. One
`hull.conf` describes a machine, and changing one line moves it between a
namespace on the host's kernel, a KVM virtual machine, and full software
emulation. The machine runs from the same directory of files on each, at
the same address, reached the same way.

```scheme
(machine
  (hostname "web")
  (source "dir:./rootfs")
  (mutable overlay)
  (command "/usr/sbin/nginx" "-g" "daemon off;")
  (ports (port (host 8080) (guest 80))))
```

```sh
hull run                    # in namespaces of its own, on the host's kernel
hull run --backend vm       # the same machine, in a KVM guest
hull run --backend emu      # the same, under TCG, with no KVM at all
curl web.hull:8080          # the same address on each
hull ssh web                # a shell in it, whichever it runs on
```

- **Files first.** A plain directory is the deployable unit. An image
  store, for snapshots, builds, registries and deduplication, is there
  when you want it and out of the way when you do not.
- **No root, and no root daemon.** Each user gets a small core that
  starts when needed and leaves when idle. Nothing is installed as root to
  run a machine.
- **No silent differences between backends.** Every field of a spec is
  checked against what the chosen backend declares it can do, and refused
  with a reason where it cannot be honoured, before anything runs.
- **Built for scripts.** CI servers, test rigs and a developer's shell
  drive it the same way: a machine's exit status is `hull run`'s, its
  output is `hull run`'s, and a killed `hull run` takes its machine with
  it.

# Backends

A backend is a **class**, which says how a machine is run -- on the
host's kernel, on a kernel of its own, or emulated -- implemented by one
or more **engines**. `(backend vm)` asks for a class and lets hull
choose the engine; `(backend chroot)` names one. With
neither, a machine is on the ns class, whose default engine is chroot,
hull's own and rootless; docker is used only when a file names it. `hull
engines` prints every engine's declared capabilities, and `hull plan`
shows what a given file would get on a given backend, field by field, as
`native`, `emulated` (honoured, at a stated cost) or `rejected` (with the
reason, and where one exists, the `--accept` token that takes it anyway).

| Engine | Class | Runs as | Kernel | Needs | Status |
|---|---|---|---|---|---|
| `chroot` | ns | namespaces | the host's | user namespaces; pasta for its own network | written |
| `qemu` | vm | hardware virtualization | its own | `/dev/kvm`, `/dev/vhost-vsock`, qemu, passt | written |
| `qemu` | emu | software emulation | its own | `/dev/vhost-vsock`, qemu, passt | written |
| `docker` | ns | namespaces | the host's | a Docker daemon | written, optional |
| crun | ns | namespaces | the host's | user namespaces | designed |
| virtualbox, vmware | vm | hardware virtualization | its own | the hypervisor | designed |
| hyperv | vm | hardware virtualization | its own | Windows | reserved |
| `hw` | hw | a real machine | its own | a board and its leases | designed |

## chroot — namespaces, as the user

The reference engine. A machine is a set of Linux namespaces of its own --
user, mount, pid, uts, and network unless it asks for the host's -- over
its directory, entered with `pivot_root`, run by an init of hull's that
reaps and passes signals, or under the image's own init. It is a process
tree on the host's kernel, so it costs almost nothing to start.

**When you want it:** running a build, a test suite or a tool from a
directory or an image, quickly and often; `hull build`'s steps, which run
on it; several machines on a network of their own, cheaply; any host
where nothing else is available, since it needs only user namespaces.

**Pros**
- Starts in milliseconds, with no guest to boot and no memory reserved.
- Needs little: user namespaces, on by default on Debian and Fedora, and
  pasta, passt's own package, for a network of the machine's own.
- A network of its own by default, `(network nat)`: `eth0` behind pasta's
  NAT, the host's resolver, and `ports` forwarded from the machine's
  stable address, as a vm's are. `(network host)` shares the host's
  instead, and `(network none)` gives `lo` alone.
- On `(segments …)` beside NAT or `(network none)`, a NIC on each, so
  chroot machines and vms share one wire, with addresses and names where
  the segment has a subnet.
- Files in the machine are the files on disk: no protocol in between, so
  inotify, locks and mmap behave exactly as on the host.
- Writes in place, `(mutable direct)`, or into an overlay,
  `(mutable overlay)`, which leaves the directory untouched; its writes
  are held in memory, or kept in a directory of yours with
  `(upper PATH)`, and `(layers …)` stacks more directories beneath.
- Many uids where the host gives the user a subordinate range: a
  package's own system users work, apt needs nothing special, and a
  stored image's owners are given back in the machine.
- The image's own init as PID 1 with `(init PATH)`, hull's init going on
  as its child.
- Host devices with `(devices …)`, bound in by their nodes, at the host's
  own timing.
- `cpus` and `memory` as ceilings, by a systemd scope from the user's
  manager.
- Nested namespaces work inside it: `unshare -rn` and rootless tools run
  in a machine.

**Cons**
- The host's kernel, shared: a machine can load no module, set no global
  sysctl, and a kernel fault in it is the host's.
- The host kernel's faults are the machine's too: whatever a user
  namespace can reach, the machine can.

**Caveats**
- **Without a subordinate range, one uid is mapped**, the user's, as
  root, and the machine says so on its stderr with the way round: a
  chown to another of its users fails, and apt wants
  `-o APT::Sandbox::User=root`. `hull doctor` gives the line that adds a
  range.
- **Writes under `(mutable overlay)` are memory unless kept**: installing
  a gigabyte of packages in a run holds a gigabyte. Keep them with
  `(upper PATH)`, or put them in the image with `hull build`, whose steps
  are stored on disk.
- `cpus` and `memory` are refused, not run unlimited, where the user's
  systemd manager gives no scope; a host without systemd has none yet.
- `(network bridge)` needs a core run as root, and `(segments …)` cannot
  go with `(network host)`, a NIC needing a namespace of the user's own.
- A machine whose root is the host's own `/` cannot nest a user
  namespace; any other root can.

## qemu as vm — a KVM machine of its own

A q35 virtual machine under KVM, booting the host's kernel, or one given
as a file or carried by the image, with hull's agent as its init. The
machine's directory and each volume are 9p shares of qemu's own, the
agent mounts them, and everything hull asks of a machine -- exec, logs,
stats, a terminal, a forwarded port -- goes to the agent over vsock. By
default the network is passt, in user space, so no bridge, tap or root
is involved. It boots in about two seconds.

**When you want it:** anything the host's kernel must not be shared for,
or cannot be: loading modules (`mac80211_hwsim` radios, `hci_vhci`,
`pppoe`), changing sysctls, building network topologies with netns and
bridges as the guest's real root, testing an init system, or running
software that needs a kernel other than the host's. Several machines on
a wire of their own, `(segments …)`, vms and chroot machines together,
for testing network software against real layer 2.

**Pros**
- A kernel of its own: root in the guest is real root of that kernel,
  with no namespace's limits, so `modprobe`, `sysctl`, `ip netns`, `tc`,
  `nftables` and raw sockets all work as on hardware.
- The host's modules are the guest's, shared read-only at
  `/lib/modules/<release>`, so a workload's own `modprobe` finds them.
- A guest kernel that crashes ends the guest, not the host, so a test may
  break the kernel on purpose.
- A kernel other than the host's: `(kernel "path:FILE")` boots one of
  your own, and `(kernel image)` the image's `/hull/kernel`, so a guest
  can run another kernel version, or one built without a feature.
- Its own network, by passt, as the user, with ports forwarded from the
  machine's stable address and UDP as well as TCP; `(network none)` for
  no NIC but its segments', and `(network bridge)` from a core run as
  root.
- `(segments "wire" …)`: a NIC per named segment, after passt's, on a
  learning switch in the user's core, shared with chroot machines on the
  same segment. Bare, the guest sets its own address; given a subnet,
  hull addresses each machine, names them to each other, and answers DNS
  at the segment's `.1`.
- `(devices …)`: USB devices, serial and parallel ports, and PCI by VFIO,
  each held by one machine at a time. USB and the parallel port are
  passed by qemu, so their timing is not the host's, and the plan says
  so.
- `(init "/sbin/init")`: the image's own init, systemd or OpenRC, as PID
  1, with the agent going on beside it.
- `(mutable overlay)` keeps every write in the guest's memory, so a
  machine can be broken freely and starts clean; `(upper PATH)` keeps
  them in a directory of yours instead, through 9p.

**Cons**
- Memory is reserved: `(memory "1G")` is a gigabyte the host gives up
  while the machine runs, where on chroot it is a ceiling.
- 9p between the machine and its files: slower than a bind mount for
  heavy I/O, and changes made on the host are not seen by inotify in the
  guest, so `(fidelity inotify)` is refused unless accepted as polling.
- x86_64 hosts only, so far.
- Needs `/dev/kvm` and `/dev/vhost-vsock` open to the user, and passt and
  `qemu-system-x86_64` installed. `hull doctor` checks each.

**Caveats**
- **The host's kernel unless one is given**: a distribution image boots
  on the kernel the host runs, unless the spec names another, until hull
  ships a small kernel of its own as the default.
- **The guest sees ids as a user namespace maps them**: qemu runs in
  one where you are root, so your files are root's in the guest and
  setuid-root programs work, and a file of any other owner or group is
  nobody's, 65534, as on chroot -- except in a stored image, whose owners
  are given back where your subordinate id range holds them. A chown to
  another id in a directory used in place is not kept on the host.
- **`(mutable direct)` writes through 9p as the user**, so a chown in the
  guest fails, and an apt install with it; use `(mutable overlay)`, or
  put packages in the image.
- A volume is a directory: naming a file is refused by qemu.
- With `(init …)`, the workload starts at once, not when the init says
  it is up; one that needs the init's services waits for them itself,
  `systemctl is-system-running --wait` say.
- A machine on a segment cannot outlive the core, `(on_disconnect keep)`,
  since the switch is the core's.
- `(network host)` is refused: a vm shares no network with the host.

## qemu as emu — the same machine, with no KVM

The vm class with hardware acceleration taken away: the same engine,
agent, shares and network, under qemu's TCG. A spec that runs on vm runs
here unchanged.

**When you want it:** a host with no `/dev/kvm`, as many CI runners and
nested environments are; a check that a machine behaves the same
without acceleration; and, once foreign architectures are in, an ARM or
RISC-V tree on an x86 laptop.

**Pros**
- No KVM needed: it runs wherever qemu does.
- Identical to vm in what it offers and how it is reached, so a file
  written for one runs on the other.

**Cons**
- Roughly 5 to 20 times slower than vm. A boot takes tens of seconds.
- `(tuning (emu (cores_scale 2)))` can over-provision vCPUs against TCG's
  cost, since one emulated core does less than a real one.

**Caveats**
- The host's architecture only, so far: a foreign kernel and rootfs
  under emu are designed, not written.
- Everything in vm's caveats applies.

## docker — an adapter for a daemon you already have

Drives a Docker daemon through its API, turning the spec into a
container. Present for the projects and hosts that already have Docker
and want hull's spec, address and SSH in front of it. **hull does not
need Docker**: the other three engines are hull's own.

**When you want it:** a host where Docker is the sanctioned runtime; an
image that must run in exactly docker's environment; or comparing a
machine on docker with the same machine on chroot or vm.

**Pros**
- Runs any image the daemon can, with docker's own exec, logs and
  networking under hull's names: `(network bridge)` is docker's own
  bridge, or the docker network `(bridge NAME)` names.
- `(devices …)` by docker's `--device`: USB, serial and parallel ports,
  plain nodes, and PCI by its VFIO group.
- A container is the daemon's, so it can be kept across a hull core's
  restart and taken back.

**Cons**
- **It inherits root**, from the daemon: whoever may drive dockerd is
  root by another name, which is the reason hull exists. Its declaration
  says `runs as root`. The workload itself runs as the user, so
  capabilities, which docker grants only to root, are refused, and an
  image that needs root inside, an `apt-get` say, fails here; chroot maps
  root to the user instead.
- A directory cannot run in place: `(mutable direct)` is refused, and an
  overlay is a `docker import` of the tree, kept by its hash.
- No `(init …)` yet, and no `(segments …)`: chroot gives a container a
  wire of its own without a root daemon.

**Caveats**
- `/` in a container is root's, as docker makes every image's, while the
  files in it keep the user's ownership.
- An exposed secret shows in `docker inspect`, as all container
  environment does.

## Designed, not written

- **crun**, ns: crun under user namespaces, with idmapped mounts for
  volume ownership, cgroup v2 limits and pasta networking, starting in
  tens of milliseconds.
- **A micro vm profile with its own kernel**: a minimal guest kernel
  bundled with hull, booting in a few hundred milliseconds and making a
  machine independent of the host's kernel version.
- **virtualbox, vmware**: full-profile vm engines for hosts where those
  hypervisors are what is there, through a block image built from the
  tree.
- **hyperv**: the Windows-host slot, reserved so that Windows support
  arrives as an engine.
- **qemu-user on ns**, `(tuning (ns (binfmt true)))`: a foreign-arch
  process rather than a foreign-arch machine, faster than full emulation
  and declared emulated, since syscall translation has edge cases.
- **hw**: real machines, a board on a lab bench reached through leases
  from a pool, with the same spec and commands as a software one.

## Choosing one

| You need | Use |
|---|---|
| speed, a build step, a test of user-space code | chroot |
| your own kernel, modules, sysctls, netns, an init system | vm |
| several machines on a wire of their own | chroot or vm, with `(segments …)`, mixed freely |
| the same, on a host with no KVM | emu |
| exactly what docker gives, on a host that has it | docker |
| to keep a machine running after its terminal closes | any, with `(on_disconnect keep)` |

`hull doctor` reports what this host gives a user, each setting that is
missing with the one-time line an administrator would run, and exits 1
only where user namespaces or a runtime directory cannot be had, so a CI
job can run it first.

# Reaching a machine

- **A stable address.** Each machine has `127.107.x.y`, derived from its
  name and the same on every backend, with its declared ports on it.
- **A name.** `web.hull` resolves to it for every program on the host,
  once the NSS module is installed (below).
- **SSH.** `hull ssh web` opens a shell in any machine, whether or not it
  runs sshd. `hull ssh-config` adds a `Host *.hull` block to
  `~/.ssh/config`, so `ssh`, `scp`, `rsync` and an editor's remote mode
  reach `web.hull` directly.
- **ps, exec, logs, attach, stats, pause, unpause, stop, kill.** The
  same commands on every backend.
- **A service.** `hull systemd` writes a user unit that runs a machine
  under the user's own systemd.

# Images, when you want them

The store is optional, and a machine never needs it: `dir:` is a source
on its own. When you want it:

```sh
hull pull debian:13                  # any OCI registry, as the user
hull build -f build.conf app/1       # steps, each cached by what it was made from
hull build --check -f build.conf app/1   # rebuild and compare: is it reproducible?
hull push app/1 registry.example/app:1
hull materialize app/1 ./rootfs      # a stored tree back to a plain directory
hull commit ./rootfs app/2
hull diff app/1 app/2
hull images                          # what the store holds
hull tag app/2 app/latest
hull manifest show app/2             # a tree as readable text, an entry a line
hull gc
```

`hull import Dockerfile` prints the same image as a `(build …)` form, and
`hull import docker-compose.yml` a `(machines …)` file, each saying what
it could not translate.

A machine whose source names a stored tree runs it, taking the image's
command, workdir and environment where the machine gives none of its own.
Packages go into an image as a build step:

```scheme
(build
  (base "debian:13")
  (steps
    (step (name "tools")
          (network true)
          (run "apt-get update && apt-get install -y --no-install-recommends iproute2 kmod"))))
```

A build step has no network unless it says `(network true)`, and
`apt-get update` exits 0 when it cannot fetch, so a step without it finds
only the packages some earlier cache knew. Steps map your subordinate id
range, so packages keep their own system users; on a host that gives
you none, apt wants `-o APT::Sandbox::User=root`.

# Building

A C11 compiler, Python 3, and OpenSSL's and zlib's development files,
`libssl-dev` and `zlib1g-dev` on Debian, which `hull pull` links.

```sh
make         # build/hull, its agent and its NSS module; fetches vendor/situ the first time
fmake        # ./hull, ./hull-agent and ./libnss_hull.so, as fmake.toml describes
make check   # the style gate, the situ contracts, then the tests
make check-qemu    # the vm and emu engines, booting real guests
make check-flip    # one project, unchanged, on docker, vm and emu
make check-docker  # the docker adapter, against the daemon
make hooks   # install the commit-msg hook
```

`web.hull` and every other machine's name resolve, for any program on
the host, once `build/libnss_hull.so.2` is installed. That is the one step
that needs root:

```sh
sudo make install-nss   # into /usr/lib/<multiarch>/
```

then add `hull` to the `hosts:` line of `/etc/nsswitch.conf`, before
`dns`, as in `hosts: files hull dns`.

On a host that restricts unprivileged user namespaces (Ubuntu 24.04), a
profile lets hull make them without turning the restriction off for
everything; `hull doctor` says whether you need it. It attaches to the
hull binary by path, so point it at yours:

```sh
sudo make install-apparmor HULL_BIN=./build/hull   # into /etc/apparmor.d/
sudo apparmor_parser -r /etc/apparmor.d/hull
```

# More

`project.md` is the design and the record of what is built and measured,
including the `hull.conf` schema reference in its appendix J; its section
12, *Adopting hull for testing*, says on what terms another project can
rely on it today. `hull_engine.h` is the C interface engines implement.
