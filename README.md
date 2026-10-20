# hull

A rootless runtime for isolated machines, built on plain files. One
`hull.conf` describes a machine, and changing one line moves it between
namespaces (`ns`), a hardware-virtualized machine (`vm`) and full software
emulation (`emu`). The machine runs from the same directory of files on
each of them.

- **Files first.** A plain directory is the deployable unit. An image
  store, for snapshots, signing and deduplication, is optional.
- **No root daemon.** The resident part is a per-user, unprivileged core.
- **No silent differences between backends.** Every spec field is checked
  against each backend's declared capabilities at plan time, and refused
  with a reason where it cannot be honoured.
- **Images decompose into files.** Container images, disk images and
  discs are stored as structured files plus a verified binary patch.

## Status

Mostly design. `project.md` is the design, including the `hull.conf`
schema reference. The first code is the s-expression reader and the
canonical writer in `sexpr/` and the guest agent protocol's framing in
`agent/`, both generated in part by [situ](https://github.com/funklord/situ)
from schemas; the `hull.conf` check in `spec/`; and in `plan/`, the
engine registry that picks what `(backend ...)` names, the planner that
writes every default into a machine, and the capability matrix that
judges it against that engine's declaration. `build/hull plan` and
`build/hull check --backend ns,vm,emu` run them against a `hull.conf`,
with docker and qemu declared but not yet driven. `build/hull run` runs
a dir source with `(backend chroot)`, the rootless reference engine in
`engine/`, and an image ref through the docker adapter, the default for
`ns`. Machines run in a per-user core in `core/`, which `hull run`
starts when none is running and which leaves once idle; `hull ps`,
`hull stop`, `hull kill`, `hull logs`, `hull attach` and `hull exec`
reach what it holds; `hull exec -t` gives a command a terminal of its own.
`hull_engine.h` is a draft of the C plugin interface that engines
implement.

## Building

```sh
make         # build/hull; fetches vendor/situ the first time
make check   # the style gate, the situ contracts, then the tests
make check-docker  # hull run on the docker adapter, against the daemon
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

Build with make for now: fmake builds hull's own schemas, but cannot yet
give each one the situc options the Makefile does.

## Copyright

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>
