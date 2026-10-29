# hull

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>

Hull gives you computers to test against without the hardware: a single
machine, or many of them wired together into a network, each made from
an ordinary folder of files and described in a short text file. It is
for the testing that needs whole systems rather than one program --
software that sets up a network, installs itself, starts when a
computer boots, or talks to other computers -- and for running older
software on the system it was written for. The same description can be
run in several ways: one that shares your computer's own core and
starts almost at once, a virtual machine with a core of its own, or a
full simulation that can stand in for a different kind of computer, at
the cost of speed. You choose by changing one line, the machines are
reached the same way whichever you choose, and where one way cannot do
what another can, hull tells you before anything runs. Machines run
without administrator rights, and the ready-made images many projects
already publish can be used as they are, though none is needed. Hull
runs on Linux, and it is not a security tool.

Containers, virtual machines and emulators are often offered as a safe
place to run a program you do not trust. Hull makes no such promise,
because the promise mostly does not hold. A machine has to share things
with the computer it runs on to be useful -- a folder, a network, a
device, and always the processor and memory underneath -- and every
one of those is a way through. The software that draws the line has
faults of its own, and ways out of containers, virtual machines and
emulators alike are found every year. Part of the claim is true: a
machine given no folder of yours and no network leaves a program less
to reach, and a virtual machine's own core keeps some of the faults in
that core from reaching yours. But that part comes with work that is
easy to underestimate: a second system to keep updated, core included, the
program running it to keep updated as well, and settings that undo the
whole arrangement if one of them is wrong. The simpler measures usually
do more for less: not running what you do not trust, running a service
as its own user account that can read only what it needs, keeping
software up to date, and putting what truly matters on a computer of
its own. Use hull to test and to simulate, and if you use it to keep
something out, know which of these properties you are relying on and
what it costs to keep it true.

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
curl web.hull:8080          # the same address on each (vm and emu; chroot shares the host's ports)
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
| `chroot` | ns | namespaces | the host's | user namespaces | written |
| `qemu` | vm | hardware virtualization | its own | `/dev/kvm`, `/dev/vhost-vsock`, qemu, passt | written |
| `qemu` | emu | software emulation | its own | `/dev/vhost-vsock`, qemu, passt | written |
| `docker` | ns | namespaces | the host's | a Docker daemon | written, optional |
| crun | ns | namespaces | the host's | user namespaces | designed |
| virtualbox, vmware | vm | hardware virtualization | its own | the hypervisor | designed |
| hyperv | vm | hardware virtualization | its own | Windows | reserved |
| `hw` | hw | a real machine | its own | a board and its leases | designed |

## chroot — namespaces, as the user

The reference engine. A machine is a set of Linux namespaces of its own --
user, mount, pid, uts, and optionally network -- over its directory,
entered with `pivot_root`, run by an init of hull's that reaps and passes
signals. It is a process tree on the host's kernel, so it costs almost
nothing to start.

**When you want it:** running a build, a test suite or a tool from a
directory or an image, quickly and often; `hull build`'s steps, which run
on it; a machine that should see the host's network as its own; any host
where nothing else is available, since it needs only user namespaces.

**Pros**
- Starts in milliseconds, with no guest to boot and no memory reserved.
- Needs nothing installed and no device opened: user namespaces, on by
  default on Debian and Fedora, are the whole requirement.
- Files in the machine are the files on disk: no protocol in between, so
  inotify, locks and mmap behave exactly as on the host.
- Writes in place, `(mutable direct)`, or into an overlay held in memory,
  `(mutable overlay)`, which leaves the directory untouched.
- Nested namespaces work inside it: `unshare -rn` and rootless tools run
  in a machine.

**Cons**
- The host's kernel, shared: a machine can load no module, set no global
  sysctl, and a kernel fault in it is the host's.
- The host kernel's faults are the machine's too: whatever a user
  namespace can reach, the machine can.
- One network, the host's or none: `(network host)` shares the host's,
  and `(network none)` gives `lo` alone. There is no network of its own to
  forward ports into, so `ports` and `segments` are refused.

**Caveats**
- **One uid is mapped today**, the user's, as root. A package whose
  install script gives a file to its own system user, `dnsmasq-base` for
  one, fails in a machine and in a build step; apt itself needs
  `-o APT::Sandbox::User=root`. Mapping the user's subordinate range, with
  an image's owners kept, is decided and being written.
- **Writes under `(mutable overlay)` are memory**: installing a gigabyte
  of packages in a run holds a gigabyte. Put them in the image with
  `hull build`, whose steps are stored on disk.
- A machine whose root is the host's own `/` cannot nest a user
  namespace; any other root can.
- No `(init …)`: PID 1 is hull's.
- No `(cpus …)` or `(memory …)` yet: a machine asking for either is
  refused on chroot, since a limit needs cgroup v2 delegated to the user,
  which `hull doctor` reports, and the engine does not yet use it.

## qemu as vm — a KVM machine of its own

A q35 virtual machine under KVM, booting the host's kernel with hull's
agent as its init. The machine's directory and each volume are 9p shares
of qemu's own, the agent mounts them, and everything hull asks of a
machine -- exec, logs, stats, a terminal, a forwarded port -- goes to the
agent over vsock. The network is passt, in user space, so no bridge, tap
or root is involved. It boots in about two seconds.

**When you want it:** anything the host's kernel must not be shared for,
or cannot be: loading modules (`mac80211_hwsim` radios, `hci_vhci`,
`pppoe`), changing sysctls, building network topologies with netns and
bridges as the guest's real root, testing an init system, or running
software that needs a kernel other than the host's. Several machines on a wire
of their own, `(segments …)`, for testing network software against real
layer 2.

**Pros**
- A kernel of its own: root in the guest is real root of that kernel,
  with no namespace's limits, so `modprobe`, `sysctl`, `ip netns`, `tc`,
  `nftables` and raw sockets all work as on hardware.
- The host's modules are the guest's, shared read-only at
  `/lib/modules/<release>`, so a workload's own `modprobe` finds them.
- A guest kernel that crashes ends the guest, not the host, so a test may
  break the kernel on purpose.
- Its own network, by passt, as the user, with ports forwarded from the
  machine's stable address and UDP as well as TCP.
- `(segments "wire" …)`: a NIC per named segment, after passt's, on a
  learning switch in the user's core, for several vms on one layer 2
  that nothing else is on. No address is given; the guest sets its own.
- `(init "/sbin/init")`: the image's own init, systemd or OpenRC, as PID
  1, with the agent going on beside it.
- `(mutable overlay)` keeps every write in the guest's memory, so a
  machine can be broken freely and starts clean.

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
- **The host's kernel, not the image's**: a distribution image boots on
  the kernel the host runs, until hull ships one of its own.
- **The guest sees ids as a user namespace maps them**: qemu runs in
  one where you are root, so your files are root's in the guest and
  setuid-root programs work, and a file of any other owner or group is
  nobody's, 65534, as on chroot. A chown to another id inside is not
  kept on the host.
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
  networking under hull's names.
- A container is the daemon's, so it can be kept across a hull core's
  restart and taken back.

**Cons**
- **It inherits root**, from the daemon: a container's root is the
  host's, which is the reason hull exists. Its declaration says
  `runs as root`, and capabilities beyond docker's default are refused.
- A directory cannot run in place: `(mutable direct)` is refused, and an
  overlay is a `docker import` of the tree, kept by its hash.
- No `(init …)` or `(segments …)` yet.

**Caveats**
- `/` in a container is root's, as docker makes every image's, while the
  files in it keep the user's ownership.
- An exposed secret shows in `docker inspect`, as all container
  environment does.

## Designed, not written

- **crun**, ns: the native rootless container engine, crun under user
  namespaces with idmapped mounts and pasta networking, giving ns-class
  machines a network of their own, and so ports.
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
| several machines on a wire of their own | vm, with `(segments …)` |
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
- **exec, logs, attach, stats, stop, kill.** The same commands on every
  backend.

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
hull gc
```

A machine whose source names a stored tree runs it, taking the image's
command, workdir and environment where the machine gives none of its own.
Packages go into an image as a build step:

```scheme
(build
  (base "debian:13")
  (steps
    (step (name "tools")
          (network true)
          (run "apt-get -o APT::Sandbox::User=root update && apt-get -o APT::Sandbox::User=root install -y --no-install-recommends iproute2 kmod"))))
```

A build step has no network unless it says `(network true)`, and
`apt-get update` exits 0 when it cannot fetch, so a step without it finds
only the packages some earlier cache knew.

# Building

A C11 compiler, Python 3, and OpenSSL's and zlib's development files,
`libssl-dev` and `zlib1g-dev` on Debian, which `hull pull` links.

```sh
make         # build/hull; fetches vendor/situ the first time
fmake        # ./hull, ./hull-agent and ./libnss_hull.so, as fmake.toml describes
make check   # the style gate, the situ contracts, then the tests
make check-qemu    # the vm and emu engines, booting real guests
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
