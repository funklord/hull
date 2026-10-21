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

`project.md` is the design, including the `hull.conf` schema reference,
and the record of what is built. Phase 1 is met: one `hull.conf` with a
dir source runs unchanged on docker, on qemu with KVM, and on qemu's TCG
emulation, and on chroot, the rootless reference engine. `hull plan` and
`hull check` judge a file against an engine's declared capabilities,
`hull run` runs it in a per-user core that starts when needed and leaves
once idle, and `hull ps`, `stop`, `kill`, `logs`, `attach`, `exec` and
`stats` reach what it holds; `hull ssh` and the `*.hull` names reach a
machine by its stable address. The optional store keeps trees by content:
`hull commit`, `materialize`, `diff`, `tag`, `images`, `manifest show`,
`gc`, and `hull pull`, which brings an image from an OCI registry, so
that a machine whose source names it runs it.
`hull_engine.h` is a draft of the C plugin interface that engines
implement.

## Building

A C11 compiler, Python 3, and OpenSSL's and zlib's development files,
`libssl-dev` and `zlib1g-dev` on Debian, which `hull pull` links.

```sh
make         # build/hull; fetches vendor/situ the first time
fmake        # ./hull, ./hull-agent and ./libnss_hull.so, as fmake.toml describes
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

## Copyright

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>
