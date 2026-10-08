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

Design only. Nothing is built yet. `project.md` is the design, including
the `hull.conf` schema reference, and `hull_engine.h` is a draft of the C
plugin interface that engines implement.

## Checks

```sh
make check   # the style gate, then the gate's own tests
make hooks   # install the commit-msg hook
```

## Copyright

Copyright (C) 2026 Nabeel Sowan <nabeel@vibes.se>
