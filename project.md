# Hull

**Status:** Draft v0.1 · Planning only · August 2026
**Name:** Hull — the hull is the vessel's isolation boundary. (Prior "Hull" CDP by hull.io was acquired by MessageBird in 2021 and discontinued; name considered safe for an open-source systems tool. Formerly working-titled "Crate".)

---

## 1. Motivation

Docker got the developer experience right and almost everything underneath it wrong for the long term:

- **Opaque formats.** Images are stacks of gzipped tarballs glued together by JSON manifests and a daemon-managed overlayfs graph. You cannot `ls` an image, diff two images with standard tools, dedupe files across images, or back up the store incrementally without Docker-specific tooling.
- **Root everywhere.** `dockerd` runs as root, owns a root-equivalent socket (`docker` group membership *is* root), and is a single point of failure for every container on the host.
- **Backend lock-in.** The isolation mechanism (shared-kernel namespaces) is baked into the product. When you need a real kernel boundary — untrusted code, kernel modules, a different kernel version — you leave the ecosystem entirely (Vagrant, raw QEMU, cloud VMs) and lose all your configuration.
- **Images where none are needed.** In the typical CI/deployment pipeline the image is pure ceremony: the build already produced the files, which get tarred into layers, pushed to a registry, pulled back down, and unpacked — five steps to move a directory. The image adds a second versioning system (with its own auth, GC policy, and layer-cache folklore) on top of the one you already have in VCS and CI artifacts.

Hull is a design for a runtime that fixes all four: a **files-first model** where a plain directory is the deployable unit and images are an optional module; a **rootless model with no privileged daemon** (the resident component is a per-user, unprivileged, socket-activated core — §2); and an **open, pluggable backend layer** where the same spec — and, crucially, the same files — runs under namespaces (`ns`), a hardware-virtualized machine (`vm`), full software emulation (`emu`), or any further engine that implements the driver interface, by changing one line.

Beneath all four sits one mechanism, Hull's core idea: **every image-shaped artifact — container image, filesystem image, disk, disc — is decomposed into plain structured files plus a verified binary patch** (§3.11). Opacity is never load-bearing; it is quarantined into patch files and measured.

### Goals

1. Every on-disk artifact is inspectable with standard Unix tools.
2. Zero root privileges required for install, build, or run (given `/dev/kvm` group access for the vm class). This holds fully for native engines; adapter engines inherit the wrapped runtime's posture (dockerd's root daemon, notably) and their matrix column says so.
3. One declarative spec; an **open set** of interchangeable backends — engine *classes* ns / vm / emu, implemented by concrete engines (docker, qemu, virtualbox, vmware first; §5.0) — with explicit, plan-time-checked capability differences — never silent behavior drift — plus namespaced per-backend sections for tuning that is legitimately backend-specific.
4. Full interoperability with the OCI ecosystem at the border (pull/push any registry image).
5. Migration paths from Dockerfiles and compose files.
6. **Files first.** A plain directory is the *primary* machine source on every backend: unpack files, point the spec at the directory, run. The image store is an optional module for snapshots/signing/dedup — Hull is fully functional with it absent, and a CI/deployment pipeline never needs to touch an image, a registry, or a layer.
7. **Identical ingress.** SSH and network access to a machine work exactly the same way on every backend — same name, same address, same host key. Switching backends must never break a shell, a mount, or an IDE connection recipe.

### Non-goals (v1)

- Being a Kubernetes CRI runtime (the design keeps the door open; v1 does not walk through it).
- Windows containers.
- Swarm-style multi-host orchestration.
- Reimplementing low-level runtimes: Hull orchestrates Docker, QEMU, VirtualBox, VMware, and crun; it does not replace them.

---

## 2. Architecture overview

```
        frontends: CLI · TUI · Qt/Widgets GUI
                            │  (shared encrypted protocol — per project guidelines)
              hull core (C; per-user, unprivileged, socket-activated)
                            │
        ┌───────────────────┼──────────────────────┐
        │                   │                      │
   spec compiler       sources               engine plugins (by class)
   (hull.conf →       (dir: trees, or       ┌───────────────────────┐
    ExecutionPlan)      CAS image store      │ ns:  docker → native  │
        │               + composefs)         │ vm:  qemu, virtualbox,│
        └── capability      │                │      vmware           │
            matrix check    │                │ emu: qemu (tcg)       │
                            │                │ …reserved slots       │
                            │                └───────────────────────┘
                            │                        │
                     OCI border tools          guest agent
                     (pull/push/convert)       (vsock, VM
                                                backends only)
```

Lifecycle of `hull run`:

1. Parse `hull.conf` → abstract **MachineSpec**.
2. Resolve the source: a `dir:` path is used as-is (§3.7); an image ref is resolved against the local store (pull + convert from OCI if absent).
3. **Plan:** compile MachineSpec + chosen backend into an ExecutionPlan; run every spec field through the capability matrix; refuse with a precise error if any field can't be honored on this backend.
4. **Execute:** hand the plan to the engine. Machines run as children of the per-user core (or of a generated systemd user unit) — there is no root daemon anywhere; the core is an unprivileged service belonging to the invoking user, and the CLI, TUI, and Qt GUI are peer frontends speaking the shared encrypted protocol to it (local or remote). *(The exact frontend/core split is finalized against the project guidelines — see Handoff notes.)*

The first-wave engines (docker, qemu, virtualbox, vmware — §5.0) ship with the core, but the driver interface is a stable plugin API: further engines — DOSBox, Wine, user-mode Linux, wasm runtimes — register themselves together with a capability declaration and become `(backend …)` values like any other.

---

## 3. Image store (deep dive) — an *optional module*

Read this section knowing its place: **nothing in Hull requires the store.** Dir sources (§3.7) are the primary mode and work with the store code entirely absent — the store exists for those who want snapshots, dedup across many machines, signing, and OCI interop. §3.8 describes the image-free deployment workflow that is Hull's main use case.

### 3.1 On-disk layout

Everything lives under `~/.local/share/hull/` (XDG-compliant, overridable):

```
store/
  objects/sha256/ab/abcdef…        # content-addressed regular files, mode 0444
  manifests/sha256/…               # image manifests (canonical, A.1)
  refs/
    registry.example.com/webapp/2.1 → ../manifests/sha256/…   # symlinks; tags are just symlinks
  mounts/                          # composefs images generated from manifests (cache, disposable)
machines/
  <machine-id>/
    spec.resolved.conf             # frozen copy of the spec at creation time
    upper/                         # writable layer (plain directory)
    state.conf                     # pid/cid, backend, ports, timestamps
```

Key properties:

- **`objects/` is a plain CAS.** One copy of each unique file across *all* images and *all* snapshots, hard-link-friendly, `rsync`-able, `du`-honest. Executable bit is the only mode stored on the object itself; full metadata lives in manifests.
- **A manifest is the image.** It's a small table mapping `path → (object hash, mode, uid/gid mapping class, xattrs, symlink target | directory | hardlink group)`. Pulling an image whose files you mostly have transfers only the missing objects — dedup is global and automatic, not layer-accidental.
- **Tags are symlinks.** `hull tag`, `hull images`, and rollbacks are filesystem operations you can audit with `ls -l`.
- **No layer stack at rest.** Layers are a *transport and build-cache* concept, not a storage concept. Two images that share 95% of files share 95% of objects regardless of how their Dockerfile-era layers were arranged.

**Written 2026-10-21, the first of it: `store/store.c`, behind `hull commit DIR REF`, `hull materialize REF DIR`, `hull images`, and the store's part of `hull gc`.** The layout is the one above, at `$XDG_DATA_HOME/hull/store`; `machines/` and `mounts/` are not made, since nothing uses them yet.
- **A commit** walks the directory, puts each regular file into `objects/` by its sha256, written under `tmp/` and linked in, so an object is whole or absent, 0444 or 0555 as its file was executable, and writes Appendix A's canonical manifest by the same rule, named by its own sha256, and the ref as a symlink to it, replaced by a rename. A copy of the tree, or the tree with its mtimes moved, is the same manifest.
- **Decided here, where Appendix A is open, and each the holder's to change:**
  - **owner:** a file the user owns is `(owner root)`, since that is what a dir source's files are inside its machine (§4.8, §4.11); any other is the explicit `((gid n) (uid n))`. Nothing is yet `(owner user)`.
  - **hardlinks** are committed as one `reg` entry per path, sharing their object. **Open, the holder's to decide:** A.2's `hardlink` kind has a `group` and no `object`, and `group` belongs to `kind hardlink` only, so no entry says which member's content a group has; a set is either all `hardlink` with nowhere for the content, or one `reg` that the group cannot name. Until that is settled the store keeps the content and loses the inode's sharing, and materializing gives each path a file of its own.
  - **stats:** `file_count` counts the entries, `object_count` the distinct objects, and `total_size` the regular files' sizes, each counted once per path. A.2's example has two entries and a `file_count` of 3, which fits none of these.
  - **paths** must be UTF-8; NFC is not checked. A socket is left out and said to be; so is a path that is not UTF-8.
- **Materializing** checks the manifest against its name and every object against its own as it copies. It makes each path with its parent opened by `openat2` beneath the target and through no symlink, so a manifest whose symlink leads out cannot write through it, and a path with `..` is refused before that. A target must be absent or empty. Directories are made 0700 and given their modes last, deepest first. What the user cannot make rootless is said and left: a device, an explicit owner, which becomes the user's, and an xattr the kernel refuses, as `security.*` is.
- **gc** takes the store's lock exclusively, where a commit or a materialize takes it shared, so it cannot take an object between its write and the ref that reaches it. A ref whose manifest cannot be read stops it rather than let it take that manifest's objects. It also empties `tmp/`.
- **`hull diff A B`, written 2026-10-21,** says what differs from one tree to another, each named by a ref or by `sha256:HEX`, as `materialize` now also takes; no ref may begin `sha256:`. Both manifests are in path order, so one walk of the two says each path once, in that order: `+ path` come, `- path` gone, and `~ path: what` changed, where what is the fields, as `kind reg -> dir`, `mode 0644 -> 0755`, `owner root -> 1000:1000`, `content` for an object or a size, `target a -> b`, `device`, `xattrs`, and `other` for a field no manifest of this version writes. A changed kind is said with the mode and owner alone, since the other fields belong to the kinds. The tree's own arch, where it differs, is a line of its own. A path's bytes below a space, DEL and the backslash are printed as `\` and three octal digits, so a newline in a name cannot begin a line. It exits as diff(1) does, 0 for the same, 1 for different, 2 for trouble, and refuses, with 2, a manifest whose entries are not in path order, which none of its writer's is, rather than say a change it could not have.
  - **Measured** in `test/store_test.sh`: nothing between a tree and its copy, nor between a tree and the tree materialized from it and committed again, the round trip being the same manifest; a manifest named by its hash the same as by its ref; every change above but owner and device, which rootless cannot make, said exactly, in path order, one name with a newline in it, and 1; an arch alone; a ref that is not, a hash that is not one, a hash with no manifest, and a manifest out of order refused with 2; and a commit to a ref named as a hash refused.
  - **Sabotage,** baseline first, in scratch copies of the store suite: the order unchecked, a kind said with the fields of its kinds, the mode unsaid, a newline printed as it is, the arch uncompared, every path said changed, come and gone swapped, no `sha256:` names, a ref named as a hash taken, the xattrs uncompared and the target uncompared each failed a named case.
- **Not written yet:** commit from a machine, `hull tag`, `hull manifest show`, composefs mounting, fs-verity, `(source "ref:…")` of a stored tree, and pull and push.
- **Measured** in `test/store_test.sh`, which `make test` runs: a tree of every kind committed, a socket left out; the manifest and each of its 14 entries verified against `manifest/manifest.situ` by `situc`, apart from the code that wrote them, and the entries in bytewise order of their paths, read apart too; a copy with its mtimes moved the same manifest, a change a new one and one new object; objects 0444 and 0555; the tree materialized as it was, its hardlink as two files; a target not empty, a ref not there, a name not one, and a directory not there refused; an object and a manifest each altered by a byte refused; a crafted manifest whose symlink leads out refused at the entry through it, and one with `..` refused, nothing written outside; and gc removing the three manifests and the one object no ref reaches and keeping the rest, the tree materializing whole after it. The test's `hull gc` reaches a core of its own with no daemon, so nothing of the account's is touched.
- **Sabotage,** baseline first, in scratch copies: leaving the entries unsorted, an object's hash or a manifest's unchecked, `openat2`'s resolve flags off, `..` allowed, gc taking an object a ref reaches or leaving one none does, xattrs dropped, and directory modes not set each failed the store suite. The unsorted entries first survived it: a copy of a tree reads back in the order it was made, and situc checks each entry's shape, not their order. Two of the ten whole-suite runs also failed in the cli suite, on the core's idle time (§4.10's open items), which `make test` runs first, so those two were run again with the store suite alone.

### 3.2 Mounting

- **ns backend:** the manifest is compiled (once, cached in `mounts/`) into a **composefs/EROFS** image — standard, in-kernel, mountable read-only with fs-verity integrity if enabled — and mounted with the machine's `upper/` directory as an overlayfs upper layer. No custom graph driver, no daemon bookkeeping: unmount + delete the directory and the machine is gone.
- **vm backend:** the *same* composefs mount is exported into the guest over **virtiofs**. Container and VM literally see the same bytes; there is no image conversion step when switching backends.
- **emu backend:** identical to vm — QEMU TCG serves virtiofs exactly the same way; no conversion when switching. (Only engines without virtiofs — virtualbox, vmware, hyperv — need a materialized disk image: the §3.9 block projection, cached by manifest hash and shared across those engines rather than reimplemented per engine.)

### 3.3 Writable layer & snapshots

The machine's `upper/` is a plain directory — the overlayfs upper under `bind`/`passthrough` projections. Under a `block` projection (§3.9) writes land in a qcow2 overlay instead and reach the source tree only via declared `writeback`; `hull commit` on such a machine walks the overlay's filesystem rather than `upper/`. In the common case, `hull commit` walks `upper/`, hashes new files into `objects/`, and emits a new manifest — a snapshot is just another manifest, ~kilobytes. `hull diff machineA imageB` is a manifest comparison: exact, fast, and printable as a normal file list.

### 3.4 Garbage collection

`hull gc` computes reachability: refs → manifests → objects; everything unreachable is deleted. Because objects are immutable and mode-0444, GC is safe to run concurrently with pulls (writes land under a tempname and are hard-linked in atomically). No "dangling layer" mystery accumulation.

### 3.5 OCI interop (the border)

- `hull pull`: fetch OCI image, apply layers in order *in memory of a scratch dir*, hash resulting files into the CAS, emit a manifest. Original OCI digests are recorded in the manifest's provenance block for supply-chain audit.
- `hull push`: re-serialize a manifest into a single-layer (or chunked-by-size) OCI image. Round-tripping is lossless for file content and metadata; by default it does not preserve the original layer structure (documented, and irrelevant to registries). Workflows that need **digest-exact** round-trip get it via the §3.11 patch model applied to OCI: a layering sidecar records layer boundaries and compression parameters, and a patch covers whatever deterministic recompression can't reproduce — the original layer digests reconstruct verifiably.
- Signature story: fs-verity digests per object + a signed manifest gives per-file integrity that OCI's per-layer signing can't express; cosign-compatible signing of the OCI export keeps the ecosystem happy.

### 3.6 Builds

`hull build` consumes a `(build …)` form (in the same file or in `hull.build.conf`):

```scheme
(build
  (base "docker.io/library/debian:13-slim")
  (steps
    (step
      (name "deps")
      (run "apt-get update && apt-get install -y --no-install-recommends python3")
      (inputs))                 ; cache key: base manifest + this command only
    (step
      (name "app")
      (copy (path (from "./src") (to "/app")))
      (run "pip install -e /app")
      (inputs "./src"))))       ; cache key includes a content hash of ./src
```

Each step runs in a throwaway machine (any backend; ns by default) and commits to a manifest. The cache key of a step is `hash(parent manifest, step definition, content hash of declared inputs)` — caching is **correct by construction**, not by Dockerfile line-order heuristics. Undeclared inputs simply aren't visible to the step (the build mount namespace only maps declared paths), which is what makes the cache sound rather than hopeful.

### 3.7 Directory sources — the primary mode

`(source "dir:./rootfs")` runs a plain directory as the machine's root. This is Hull's default way of working, not an escape hatch: the store, manifests, and pulls are bypassed entirely; the directory *is* the machine's filesystem, on every backend, with no conversion step: ns idmap-bind-mounts it as rootfs, and vm/emu export the same directory as the rootfs virtiofs share.

Write semantics are per machine:

- `(mutable direct)` (the default for dir sources): writes land in the directory itself. Edit a config file on the host, restart the service in the guest — or the reverse. `git status` your rootfs if you like; the files are the truth. (This describes `bind` and `passthrough` projections; a block projection cannot write through to the directory, so the matrix rejects `direct` there unless `writeback` is declared — §3.9.)
- `(mutable overlay)`: the directory stays pristine; writes go to the machine's `upper/` exactly as with image sources.

Ownership: a single-uid tree — the common case for a service rootfs — needs nothing special; everything maps through the root/user ownership classes (A.3). Multi-uid trees store ownership in xattrs (`user.hull.owner`), written naturally by chown *inside* a running machine or seeded by `hull dir init`; backends map them via idmapped mounts (ns) or virtiofs xattr mapping (vm/emu). A tree without ownership xattrs simply appears root-owned, which is correct for most rootfs trees.

Images and directories are interconvertible: `hull commit` on a dir-source machine produces a manifest, and `hull materialize <ref> ./dir` unpacks any image — including any pulled Docker Hub image — into a directory you can then run directly, edit, and re-commit. Directories are the working currency; manifests are optional packaging for shipping, signing, snapshots, and dedup.

### 3.8 Deployment & CI without images

The dominant pipeline today is: build → `docker build` (re-tar the artifacts you just built) → push to a registry → pull on the target → unpack → run. Hull's equivalent deletes the middle entirely:

```sh
# CI job
make build DESTDIR=out/rootfs                       # or unpack any release tarball
rsync -a --delete out/rootfs/ deploy@host:/srv/app/rootfs/
ssh deploy@host hull restart app                    # hull.conf: (source "dir:/srv/app/rootfs")
```

- **The deployable unit is a directory.** Transport is anything that moves files; rsync's delta transfer is strictly better granularity than image layers, and `hull check` in CI guarantees the tree runs on every backend you care about — without a registry, auth tokens, image GC policy, or tag-mutation surprises anywhere in the pipeline.
- **Rollback is a directory too:** keep N previous trees and point the spec (or a symlink) back at one. Instant, and you can diff the two trees with standard tools to see exactly what a rollback changes.
- **Versioning lives where it already lives** — your VCS and CI artifact storage — instead of in a second content-addressed system with its own lifecycle.
- **Staging equals production mechanics:** the same rsync'd tree runs under `ns` on a laptop, `vm` on the server, `emu` in a cross-arch CI matrix. The files never change shape.

When a team later wants dedup across many machines, signed snapshots, or to publish to an OCI registry, the optional store (§§3.1–3.6) bolts on without changing this workflow — `hull commit` bridges a directory into it at any time.

### 3.9 Projections — when a real image is genuinely needed

Hull's model admits an honest tension: composefs is a binary metadata blob over files stored elsewhere, and virtiofs is a filesystem-shaped protocol over host files — both *simulate* a filesystem while the truth lives in a directory. Some consumers need the real thing. The reconciliation is to make how a tree is **presented** a first-class, explicit concept, governed by one principle:

> **The files are the single source of truth. Everything image-shaped is a derived projection: content-addressed, cached, regenerable, never authored directly, and never allowed to diverge silently.**

One tree, three projections:

- **`bind`** (ns) — the files themselves, via idmapped bind mount. Nothing simulated.
- **`passthrough`** (vm/emu) — virtiofs; a protocol in the middle, but still the same live files. Metadata blobs used here (the composefs image in `mounts/`) are disposable caches, regenerable from the manifest or tree at any time — they never hold state of their own, and `hull verify` regenerates and compares if one is ever in doubt.
- **`block`** — a real filesystem image, actually built (deterministic mkfs from the tree, systemd-repart-style), for consumers that legitimately need one: the virtualbox/vmware/hyperv engines (no virtiofs), `full`-profile boots wanting a partition table and bootloader, and workloads needing block-level fidelity — swap, O_DIRECT, loop mounts inside the guest, filesystem-specific ioctls, mandatory locking.

Rules that keep block projections honest:

1. **Derived, never authoritative.** A block image is keyed by the content hash of its source tree/manifest; identical tree ⇒ identical image bytes (reproducible mkfs), so caching is sound and `hull verify` can prove a cached image still matches. Deleting every projection loses nothing.
2. **The write path must be explicit.** Under a block projection, guest writes land in a qcow2 overlay — not in your directory. Therefore `(mutable direct)` + block projection is **rejected** by the matrix (your files would silently go stale) unless you opt into `(writeback sync-on-stop)`, which syncs the overlay back to the directory on clean shutdown and documents its crash window. There is no configuration in which the directory and the image can quietly disagree about which one is real.
3. **Need is declared, not discovered.** `(projection auto)` (default) lets the plan step choose per engine; a spec that needs passthrough-hostile semantics declares them — `(fidelity inotify flock odirect)` — and the plan either selects a projection satisfying every declared need or rejects with the reason. Fidelity gaps become matrix rows, found at plan time instead of by 2 a.m. breakage.
4. **Self-containment is an export, not a mode.** When a machine must leave Hull — boot under plain QEMU, ship to a cloud, run under VirtualBox on someone else's laptop — `hull export --bootable <machine> disk.img` emits a standalone image (partition table, bootloader, kernel, tree) as a deliberate border artifact, exactly like OCI push: a copy that leaves the truth behind, and says so.

### 3.10 Media sources — file collections for what we used to store as images

The motivating case: DOS-era games installed from CD-ROM, frequently with copy protection that reads deliberately damaged sectors, subchannel data, or a nonstandard TOC. Until now the only faithful container was a *more* powerful image format — BIN/CUE, raw dumps — an opaque blob precisely because a filesystem can't express those quirks. Hull generalizes §3.9 from filesystems to **media**: store the disc as a collection of plain files, where the structure a filesystem can't hold — and the quirks and unstructured binary data — are *also* files.

```
game/
  hull.conf                 # (backend dosbox)
  c/                        # the installed game: plain files, the DOS C: drive
  cd/
    files/                  # the disc's ISO9660 tree, plain files
    media.conf              # TOC: track layout, modes, pregaps, volume id, timestamps
    tracks/02-audio.wav     # audio tracks as ordinary audio files
    quirks/
      subchannel.bin        # subchannel stream (the copy-protection carrier)
      bad-sectors.conf      # addresses expected to error, and how
      sectors/0x1A40.raw    # raw overrides where mastered bytes ≠ file bytes
```

```scheme
(machine
  (source "dir:./c")            ; the saves (below)
  (media
    (medium
      (type cdrom)
      (source "dir:./cd"))))    ; or "tar:./cd.tar"
```

- **Projection does the reconciling.** The engine needs a disc, so Hull compiles the collection into a bit-faithful virtual disc on demand — derived, content-keyed, disposable; §3.9's rules apply unchanged. Copy-protection checks pass because bad sectors, subchannel data, and the TOC are replayed from the quirk files at the right addresses. And read-only media has no write-path problem at all — the game's *saves* go to `c/`, which is an ordinary dir source with `direct` writes.
- **Quirks are formally a patch.** The definition is exact: `naive = project(files, media.conf)` is deterministic; `quirks = delta(original, naive)`, computed at import and stored as addressed byte/sector patches; reconstruction is `naive + quirks`, verified against the recorded hash of the original dump. Three consequences. *Import is automatic:* `hull media import disc.bin` extracts the files, projects naively, diffs at sector level — whatever fails to round-trip **is** the quirk set; nobody authors quirk files by hand. *Losslessness is provable:* `hull media verify` reconstructs and compares hashes, so converting an image to a file collection is demonstrably exact, not hopefully exact. *Projector versions are frozen:* a patch is only stable against a stable base, so projection algorithms are versioned and immutable once shipped — `media.conf` names the projector version its quirks patch against, new versions ship alongside old ones (like compression formats), and `hull media rebase` re-derives the quirk set against a newer projector as an explicit, hash-verified operation.
- **Graceful degradation ladder.** Best case, everything is meaningful files. Middle case, files plus small binary quirk sidecars. Worst case — a protection scheme that defeats reconstruction — one honest `raw.bin` blob *as a file inside the collection*. The worst case equals today's status quo; the typical case is a diffable, greppable, dedup-friendly directory where the game's files are just files.
- **`tar:` sources.** Any dir-shaped source can be a tarball (`(source "tar:./game.tar")`), transparently unpacked to a content-keyed cache — a transport convenience, not another format.
- **The pattern generalizes.** Floppies (files + boot sector + interleave quirks), cartridge dumps (blob + header metadata as files), and media nobody has thought of yet: sidecar schemas are per media type and versioned, and each engine declares which media types it accepts in its capability table — a dosbox engine accepts `cdrom` and `floppy`; a vm engine accepts `cdrom` (as an emulated drive) and `disk`. §3.11 states the generalization in full.

### 3.11 The universal decomposition — structure + patch

§3.9 and §3.10 are two instances of one law, and the law is Hull's actual foundation:

> **Every opaque binary artifact is stored as `project(structured files) + patch`, where the projector is deterministic and version-frozen, the patch is the recorded delta to the original, and `hash(project(structure) + patch) == hash(original)` is checkable at any time.**

| Artifact | Structure (plain files) | Projector | Patch typically covers |
|---|---|---|---|
| Container image (OCI) | file tree + manifest | layer serializer | layer boundaries + compression quirks (only when digest-exact round-trip is wanted, §3.5) |
| Filesystem image | file tree + fs metadata (type, UUID, label, flags) | deterministic mkfs | bootloader gaps, hidden/unallocated residue, layout the rebuild doesn't reproduce |
| Partitioned disk (appliance firmware, SD-card image) | per-partition trees + partition-table file | repart-style builder | MBR gap bytes, vendor blobs between partitions |
| CD-ROM | ISO tree + TOC + audio track files | ISO/cue builder | subchannel data, bad sectors, raw sector overrides |
| Floppy / cartridge | files (or blob) + metadata | per-format builder | interleave, headers, undecoded regions |

What the generalization buys:

- **One import algorithm for everything.** `hull import <artifact>` is always the same shape: identify format → extract structure → project deterministically → diff against the original → store structure + patch. The reverse — reassembly — *is* the projector. New formats are new projector plugins, not new architectures.
- **The patch size is a measurement of understanding.** A perfect projector yields an empty patch; an unknown format yields patch = whole artifact (the honest blob, no worse than the status quo). Everything between is quantified opacity. Projector improvements shrink patches monotonically via `rebase`, and can never lose data — every rebase is hash-verified against the reconstructed original.
- **Dedup and transport come free.** Structure dedupes in the CAS like all other files; patches are usually tiny; rsync moves both efficiently. A thousand appliance images that share a base OS share it file-by-file, which no image-granularity store can do.
- **Everything is files all the way down.** Greppable, diffable, `git`-able, auditable with standard tools — the project's first goal, now holding even for artifacts that were designed to be opaque.

Invariants, restated once: projectors are deterministic; projector versions are immutable once shipped and named by the collections that depend on them; every decomposition is round-trip verified at import and re-verifiable forever; and patches are themselves ordinary addressed files — opacity is contained, never structural.

### 3.12 Projectors as descriptions — the situ layer

Projectors are not primarily code. A related project, **situ**, describes arbitrary protocols, file formats, and memory structures exhaustively in a description file, and generates accessor code (and more) from the description. Hull adopts it as the substrate of §3.11:

- **A projector = a situ description + an optional policy module.** The situ spec supplies the parser (import/decompose), the serializer (reassemble), and the very definition of "structure" for a format. Purely declarative formats — partition tables, cue/TOC layouts, headers, cartridge formats — need *no* procedural code at all: a new projector is a new situ file. Only formats with genuine policy (ext4's block allocation in mkfs, ISO9660 sector layout choices) add a small deterministic policy module alongside.
- **The closure:** the patch is, by construction, *exactly what the situ description does not yet describe*. Patch size across a corpus becomes a live completeness metric for the description; refining a situ spec measurably shrinks patches (via `rebase`, hash-verified, risk-free). "Quantified opacity" stops being a metaphor and becomes a number attached to a description file.
- **Projector versioning inherits from situ:** the frozen-version invariant maps to pinned situ description versions; a collection's `media.conf` names the description (and its hash) it was decomposed against.
- **Everything else that speaks bytes uses the same layer.** The agent control protocol (Appendix B), the manifest canonical encoding (Appendix A), and sidecar schemas can all be situ descriptions, with accessors generated for every component — C core, Qt GUI, TUI, guest agent — from one source of truth. One description language ends the codec-drift problem between components, and situ's ancillary generators (validators, pretty-printers, fuzzers) give `hull inspect <artifact>` and protocol fuzzing essentially for free.

**Decided 2026-10-09: Appendices A and B are situ descriptions.** The manifest is `manifest/manifest.situ` (A.2), the agent's messages are `agent/protocol.situ` (B.3) and their framing `agent/message.situ` (B.2), each held to its committed capability map by `make contract` and, for the first two, to bytes it must accept and refuse by `make test`. The appendices summarise the schemas and carry what a layout cannot state. Generator targets wait for the code that consumes them: so far only the framing generates C.

### 3.13 Representations — one identity, three forms

A machine's content has exactly one identity (its manifest hash / tree hash) and three interchangeable representations. Conversions between them are lossless, deterministic, and hash-verified — §3.11 applied to Hull's own formats.

**1. The in-fs format (the collection).** The formalized on-filesystem layout — a versioned, situ-described spec, not a convention: the payload tree as plain files, plus a `.hull/` metadata directory carrying what the filesystem itself can't: ownership records (mirroring the `user.hull.owner` xattrs, for filesystems or transports that strip xattrs), media sidecars and quirks (§3.10), manifest fragments, and the format version. Rule: **the payload tree must remain useful with `.hull/` deleted** — metadata enriches, never captures. This is the working representation: rsync it, `git` it, edit it live.

**2. The Hull image file (`.hull` pack) — the efficient single-file form.** For when a directory is inconvenient (release artifacts, download links, cold storage, email-a-machine). Not a tarball with a new suffix — a format designed for its job:

- **Canonical:** deterministic serialization — the same collection always produces byte-identical packs, so the pack's hash *is* the content identity (tar can't promise this; ordering, padding, and metadata make it non-canonical).
- **Structured:** header + manifest index + content-defined-chunked, zstd-compressed object pack + patch section. situ-described end to end, so accessors, validators, and `hull inspect` come from the description.
- **Seekable & mountable:** the index is upfront; single files are extractable without reading the pack, and the pack is directly mountable (composefs/FUSE) — a pack can *be* a machine source (`(source "pack:./app.hull")`) without unpacking.
- **Dedup-aware transport:** chunk boundaries are content-defined (§C), so transferring a pack to a host that has a previous version moves only changed chunks; two packs sharing content share chunks in the store.
- `hull pack <dir> <file.hull>` / `hull unpack` round-trip exactly; `tar:` remains accepted as a lowest-common-denominator input, never an output format.

**3. Foreign image files — load and import other software's formats.** Two modes, both engine-visible in the matrix:

- **Import** — the §3.11 one-algorithm path: qcow2, VMDK, VHD/VHDX, VDI, raw disks, ISO/BIN-CUE, OVA/OVF appliances, OCI/docker-save tarballs, Vagrant boxes → decompose into a collection (files + metadata + patch), provably lossless, re-exportable to the original format bit-exact when the projector permits.
- **Attach** — run a foreign image *in place*, no conversion: `(source "image:./disk.vmdk")` hands the file to an engine that speaks it natively (vmware↔VMDK, hyperv↔VHDX, qemu↔qcow2/raw, virtualbox↔VDI). Attach trades away inspectability and dir-workflows for zero-cost onboarding of existing VMs — declared per engine in its capability table, and `hull import` is always one command away when the user wants the files-first benefits.

### 3.14 Why files beat loop-mounted images — the technical case

Hull supports image workflows fully (§3.9, §3.13). But the architecture's strength is the **strict separation of files from loop-mounted images**, and the case rests on mechanism, not taste:

**Safer.** Loop-mounting an image makes the host kernel's filesystem driver parse untrusted metadata — ext4/xfs/btrfs parsers are a notorious privilege-escalation surface, which is why mounting requires root, why rootless runtimes refuse it, and why auto-mounting images has been a recurring CVE class. A directory of files requires no filesystem parser at all: reading files is just reading files. Where Hull does need a mounted image format, it uses exactly one — read-only EROFS/composefs with fs-verity — a single, small, audited, unwritable parser instead of the full read-write zoo. And files are transparent to scanning, auditing, and integrity tooling; a block image is a blob those tools must be taught to crack open.

**Faster.** The host page cache dedupes at file granularity: twenty machines sharing a base tree over virtiofs/CAS hold **one** copy of each shared file in host RAM, while twenty loop-mounted images cache twenty copies of the same bytes — the kernel cannot know identical blocks in distinct block devices are identical. Files avoid the fs-in-file-on-fs stack: no nested journaling (journal-on-journal write amplification), no image-internal fragmentation, working host readahead, reflink/hardlink dedup for free. And directories have no size: no pre-allocated disk, no full-disk-inside-half-full-image waste, no resize2fs ceremony, no fsck of a 40 GB blob after a crash.

**More useful.** Files are *shareable between machines while live* — the same read-only tree mounted into many VMs and containers simultaneously, which a block image fundamentally cannot do safely (multi-attach of writable images corrupts; even read-only multi-attach is fraught). Files are incrementally backed up by any tool ever written — rsync, borg, git, tape — at file granularity, where an image is an all-or-nothing blob whose smallest change invalidates the whole artifact for dumb tools and demands specialized block-diffing for smart ones. Files are indexable, greppable, deduplicable, patchable in place, diffable across versions and across *machines*. Every one of these properties exists because the unit of storage matches the unit of meaning.

The design consequence, stated once: **block projections are a border facility for consumers that genuinely need block semantics (§3.9) — never the resting state.** Data at rest in Hull is files; images are made on demand, used, and discarded.

---

## 4. The spec: `hull.conf`

One file describes a machine. Multi-machine files, compose-style, are the same schema under `(machines (<name> …) …)`.

```scheme
(machine
  (source "registry.example.com/webapp:2.1")  ; an image ref…
  ; (source "dir:./rootfs")                   ; …or just a directory of files (§3.7)
  ; (mutable direct)                          ; direct | overlay (dir sources default to direct)
  (arch host)                    ; host | amd64 | arm64 | riscv64 … (foreign arch ⇒ emu, or ns+binfmt)
  (command "./server" "--port" "80")
  (workdir "/app")
  (env (RUST_LOG "info"))
  (env_file ".env")              ; optional, gitignored secrets

  (ports (port (host 8080) (guest 80) (proto tcp)))
  (volumes
    (volume (host "./data") (guest "/var/data") (mode rw))
    (volume (host "./cfg")  (guest "/etc/app")  (mode ro)))

  (cpus 2)
  (memory "2G")

  (backend ns)                   ; a CLASS (ns | vm | emu) or a CONCRETE ENGINE
                                 ; (docker, qemu, virtualbox, vmware, …) — the only line that changes;
                                 ; a class picks the host's default engine of that class

  (ssh true)                     ; Hull provisions sshd + your authorized_keys, all backends (§8.1)
  (restart on-failure)           ; consumed by the systemd generator, not a daemon

  (security
    (readonly_root true)
    (no_new_privs true)          ; default true
    (allow))                     ; additive capability grants (ns) / device passthrough (VMs)

  (tuning
    (vm                          ; CLASS-level tuning: applies to whichever vm engine runs
      (profile micro)            ; micro (microVM; qemu & native engines) | full (full machine type)
      (kernel bundled))          ; bundled | image | "path:./bzImage"
    (qemu                        ; ENGINE-level tuning: overrides class-level when this engine runs
      (accel auto))              ; auto | kvm | tcg
    (virtualbox
      (gui false))
    (emu                         ; emu class (qemu with accel=tcg); adds emulation tuning
      (cores_scale 2))           ; over-provision vCPUs to claw back TCG speed
    (ns
      (binfmt false))))          ; opt in to qemu-user for foreign-arch sources (matrix: emulated)
```

Design rules: the `(machine …)` form is **backend-neutral by definition** — nothing may appear there that can't be expressed on every backend or cleanly rejected by the capability matrix. Backend-specific tuning is quarantined under `(tuning …)`, where entries may be named for a *class* (applies to any engine of that class) or a *concrete engine* (overrides the class entry); entries not applicable to the selected backend are ignored with a warning. The complete key inventory is Appendix J.

### 4.1 Syntax: s-expressions, not TOML

**Decided 2026-10-09: hull's own file formats are s-expressions, in files named `*.conf`.** That covers the machine spec (`hull.conf`), the build form, media sidecars (`media.conf`, `bad-sectors.conf`), host policy (`policy.conf`), per-machine state (`state.conf`, `spec.resolved.conf`) and the readable view of a manifest (A.1). Tool configuration carried from elsewhere, such as `.style-gate.toml`, is not a hull format and is unaffected.

TOML was never chosen deliberately: it was what the planning draft wrote in, and no reason for it was recorded. The holder's reason for the change is familiarity: TOML was new to them, and s-expressions are known to substantially more people. The technical case points the same way:

- **The parser is small enough to own.** A complete s-expression reader in C is a few hundred lines with no dependency, where TOML means vendoring a library. That matters twice: for the core, and for the guest agent, a static binary with no libc (Appendix B).
- **Nesting is uniform.** Multi-machine files, build steps and per-engine tuning nest the same way at every depth. TOML spreads one machine across headers that repeat its full path (`[machines.db]`, then `[machines.db.backend.vm]`) and expresses a list of tables as `[[build.step]]`, its least obvious construct and the one hull would lean on most.
- **One family serves both reading and hashing.** Canonical s-expressions (Rivest's SPKI form) length-prefix every atom and allow no optional whitespace, so a value has exactly one byte encoding. TOML could not be hashed reliably, which is why the draft paired it with CBOR; with s-expressions the readable form and the hashed form are two spellings of one format.

The conventions, settled with the decision and followed by every example in this document:

- **A record is a list of `(key value…)` entries**: `(machine (source "dir:./rootfs") (ssh true))`. A key takes zero or more values, so `(command "./server" "--port" "80")` is a list and `(allow)` an empty one.
- **A list of records is a plural key holding singular entries**: `(ports (port …) (port …))`, `(volumes (volume …))`, `(media (medium …))`, `(steps (step …))`, and `(copy (path …))` for what a build step copies in.
- **A map whose keys the user chooses has the same shape**, the name standing as the key: `(env (RUST_LOG "info"))`, `(machines (web …) (db …))`, `(secrets (db_password …))`.
- **Atoms.** Numbers are bare (`8080`). Strings are double-quoted (`"./data"`), and so are sizes and durations (`"2G"`, `"7d"`) and anything carrying a scheme prefix (`"dir:./rootfs"`). Enumerated values are bare symbols (`ns`, `rw`, `on-failure`). Booleans are the symbols `true` and `false`, recognised as booleans by the schema rather than by the reader. A comment runs from `;` to the end of the line.
- **Keys are `snake_case`**, as `code-style.md` requires of every name this project defines; they become field ids and `--accept` tokens, which cannot be renamed later.
- **Per-backend tuning lives under `tuning`, not under `backend`.** The TOML draft put it at `[machine.backend.vm]` beside `backend = "ns"`, which TOML itself rejects, since one key cannot be both a string and a table. As separate keys, `(backend ns)` and `(tuning (vm …))` sit side by side.

Rejected, so the comparison is not repeated:

- **TOML** -- specified types and an INI-like flat file are real strengths, but deep nesting is awkward, a C parser is a dependency, and it needs a second format for hashing.
- **INI** -- no standard, no lists, no nesting; `ports` and `volumes` would need a home-made encoding.
- **JSON** -- no comments, and noisy to write by hand.
- **YAML** -- a very large specification, implicit typing (`no` reads as false, `3.10` as 3.1), and meaning that depends on indentation.
- **systemd unit syntax** -- familiar, but no typed lists and no nesting.

**The files are data, never code.** No evaluation, variables, includes or conditionals: the reader refuses anything that is not a datum. This is not a new rule but an existing one applied to the new syntax. The planner must see every field to check it against the capability matrix (§6), which is also why the Vagrant dialect rejects arbitrary Ruby (§9). It needs stating because s-expressions invite exactly that growth, as Guix's configuration did.

**Decided 2026-10-09 as well: canonical s-expressions replace CBOR everywhere the draft used it.** That is the manifest's hashed and signed form (A.1), the guest agent's messages (B.2), and so the encoded buffers that cross the engine ABI, which are defined as the manifest's encoding (`hull_engine.h`). One reader and one writer serve every byte format hull defines. Rivest's syntax lets a length-prefixed atom (`4:root`) stand anywhere an ordinary atom can, so a reader that accepts it parses the canonical form and the readable form alike; hull's reader accepts it.

### 4.2 The reader: situ frames, hull walks

**Decided 2026-10-09: the reader is generated by situ.** `sexpr/readable.situ` and `sexpr/canonical.situ` each describe one form, and situ generates C that frames it: its kind and extent, whether a string, comment or octets reached their closing byte, and whether a canonical length is minimal decimal with its bytes inside the buffer. situ is a submodule at `vendor/situ`, pinned to a commit its remote has, and the build calls `vendor/situ/bin/situc` by path: the packaged `/usr/bin/situc` is from 2026-08-05 and prints the same `situc 1.0` as the source tree, so nothing would show an old one running. fmake ran that packaged situc on hull until its section 402, which now looks in declared submodules. Its section 404 (`e786d4d`) then let `agent/message.situ` win over situ's example of the same name rather than asking hull to rename a file in situ's checkout. fmake's section 414 (`281a713`) then stopped compiling the schemas of situ's own examples, whose only requesters were situ's own test programs, so a run asks situc for hull's schemas alone. `fmake.toml` carries the one thing fmake asked for, `HULL_VERSION` read from `VERSION`. Measured 2026-10-16 in a clone with fmake at `b683bd8`, it then stops on three things the Makefile says per schema and `[situ] flags` can only say for the whole tree: `--layer frame` for `agent/message.situ`, whose `message_frame.h` `agent/framing.c` includes; a `--prefix` per schema, without which `sexpr/sexpr.c` meets `situ_form_view` twice; and the `-include` of `core/frontend_view.h` for situ's finding 18, which goes when situ fixes it. That was signalled to fmake's inbox. **fmake builds hull since 2026-10-21,** by fmake's answer of that day: `fmake.toml` now gives each schema a `[generate.*]` rule carrying the Makefile's situc options, for which fmake's own schema pass stands down, and the frontend and protocol rules write the view header's `#include` into the generated source, since each has its own and no tree-wide flag can say it. `[target.agent]` names the agent `hull-agent`, beside `hull`, where the qemu engine looks for it, and links it `-static`, as the Makefile does, for an initrd with no libc. Measured in a copy of the tree with fmake at `5ce637f`: `hull` and `hull-agent` built, exit 0, and the fmake-built `hull` passing the store suite, 28 cases, and the cli suite, 217, with the probe from make, and the qemu suite, 110, its machines booting the fmake-built `hull-agent` as their init. Not parity with `make`: `libnss_hull.so.2` has no `main()`, so fmake finds no target in it, and fmake builds no test programs by default. The packaged `/usr/bin/fmake` of that day, `5af02348`, still compiles every schema in the tree, the vendored situ's `std/kernels.situ` among them, and stops on it, so the line in the README wants an fmake of `5ce637f` or later. The per-schema key for situc options that hull asked for is still fmake's holder's to spell; nothing waits on it.

`sexpr/sexpr.c` is the rest, and the split is situ's own. A generated list check validates neither the list's elements nor that it reached its `)`, and says so: both are left to the caller, as for any run of structs. So `hull_sexpr_open()` walks every form with an explicit stack, asks situ to frame each one, and nothing else may read a document until that walk has passed. The walk is also a second witness: where situ's extent for a list and the `)` the walk found disagree, the document is refused rather than believed.

What the reader enforces beyond §4.1's conventions:

- string escapes are `\\`, `\"`, `\n` and `\t`, and no others;
- `#…#` octets hold an even number of hexadecimal digits, either case;
- a `;` comment runs to a newline and must have one, on the last line too, because situ's `until` has no end-of-input alternative;
- lists nest at most 32 deep in either syntax (`HULL_SEXPR_MAX_DEPTH`); situ bounds the same depth, counted in structs;
- a canonical document holds exactly one form and nothing after it.

Measured, by `make check` and by sabotage in a scratch copy of the tree:

- `test/sexpr_test.c` runs 66 cases and checks every refusal's offset as well as its status. Dropping `[escape]` from the schema, removing the unclosed-list check, removing the escape whitelist and breaking the witness comparison each fail the cases aimed at them.
- That last sabotage proves the comparison runs on every list. No case yet makes situ and the walk genuinely disagree, so it is a guard that has been seen to run, not one seen to catch.
- The walk's own depth limit cannot be reached while situ's nesting bound holds: situ refuses 33 levels first, and with the walk's check removed the suite stays green. It stays because it bounds the walk's fixed-size stack, and is what stands between a situ that stopped counting and an overflow.
- `make contract` compares each schema's capability map, committed beside it, against situ. A situ update that changes what a form costs to frame or reach fails there before it reaches the reader; altering a committed map was seen to fail it.

**situ was told what adopting it found**, in situ's suggestion file for hull: nine findings, four of which reproduce on situ's own example. A list closed after whitespace gains a phantom empty element; the example frames an escaped quote as two forms; a run of structs never has its terminator checked; and `situc verify` reports bytes as conforming whose elements it never validated. The reader does not depend on any of them: it skips leads itself before deciding between an element and a `)`, the schema carries `[escape]`, and the walk checks every terminator. A fixed situ leaves it correct.

**The canonical writer is built on situ's build layer, decided 2026-10-09.** The holder first chose to have situ build the rung rather than hand-write a writer meanwhile (situ findings 10 and 11); once situ built a run of structs the caller has encoded (situ `ce7aa67`) and then variants, the holder chose to write it. `sexpr/writer.c` has three calls, each appending at `*at` in the caller's buffer and moving `*at` only on success. `hull_sexpr_put_atom` and `hull_sexpr_put_list` write through situ's generated `atom` and `list` builders, from `sexpr/canonical.situ` at the edit layer. `hull_sexpr_put_sorted` appends a record's entries sorted bytewise by key and refuses a key given twice, which is hull's (A.1). A list is built from the inside out: its elements are written first, into a buffer of their own.

- **Every result is read back** with `hull_sexpr_open` before `*at` moves, because situ's list builder validates the list and not its elements. It returns success for `( 1:a)`, the unclosed `((1:a)` and the non-minimal `(01:a)`, which went to situ as finding 17. The read-back is also what makes the writer refuse a 33rd level exactly as the reader does.
- **situ's variant builders are not used.** A canonical form's first byte is its own arm, so the list and atom builders plus the read-back already do what their arm check does.
- **The edit header** generated beside the builders is still not compiled (finding 10), and nothing includes it.
- **`hull_sexpr_to_canonical()` turns a readable form into canonical form** (`sexpr/convert.c`), since A.1 says "the readable form parses to the canonical one with no help". Symbols, strings and octets become atoms of their bytes. The one rule beyond that is order, and the shape decides it. A list opening with an atom whose other elements are all lists opening with atoms, no two alike, is a record, and its entries are sorted. Any other list keeps its order: arguments, and plural lists like `ports`, whose items share a key. For J's shapes this is exact: §4.3's check refuses a key given twice in a record, and a plural list repeats its singular key. Every piece goes through the writer, so it is read back as it is made. `test/sexpr_test.c` holds 15 conversions to exact bytes. In a scratch copy, breaking the sorting, the plural rule, the atom head, the decoding of values or the room check each failed it; the atom head failed only after a case for a list-headed list was added.
- **Measured:** `test/sexpr_test.c` gains 28 cases for exact bytes, `*at` left alone on every refusal, ordering with a key that prefixes another, and depth 32 against 33. Sabotage in a scratch copy fails the cases aimed at the read-back, the overlap check, the duplicate check, the prefix rule and the space check. Removing the check of each entry before placing it made the suite loop rather than fail, so the placing pass now checks each entry itself, and both test programs bound themselves with `alarm(60)`.


### 4.3 The spec check: Appendix J as tables

**`spec/spec.c` holds a `hull.conf` to Appendix J**, as the first part of Phase 1's spec parser (§12). `hull_spec_open()` reads the file with the readable reader, then walks every form against tables, one per record, of keys and their value types. A key added to J is a row in a table, not a function. Like the reader it copies and allocates nothing: a spec is the reader's document plus where its top-level `machine`, `machines` and `build` forms are, and `hull_spec_find()` hands out an entry for the reader's own calls. Every refusal carries an offset, line and column, and the key's path as written, such as `machine.ports.port.host` or `machines.web.env.RUST_LOG`.

What it refuses is everything J lets a file decide alone: an unknown key (J.8), a key or name given twice, a required key missing, a value of the wrong kind or count, and a value outside its set or form. What it leaves to the planner is what needs a backend or a manifest: whether an engine supports a field (§6), and the defaults the manifest supplies (`command`, `workdir`).

J left these open, so the check settles them, and each is a row to change if it is wrong:

- **Optional where J gives no default:** `port.proto`, `volume.mode`, `secret.expose`. The Appendix D example omits `proto`. `medium.type` and `.source`, `port.host` and `.guest`, `volume.host` and `.guest`, `path.from` and `.to`, `step.name` and `build.base` are required.
- **Forms:**
  - A size is digits above zero, then `K`, `M`, `G`, `T` or nothing (`"2G"`).
  - A duration is digits above zero, then `s`, `m`, `h`, `d` or `w` (`"7d"`).
  - A count (`cpus`) or port is minimal decimal, from 1; a port stops at 65535.
  - `cores_scale` is a positive decimal, `1.5` allowed.
  - Guest paths, `workdir` and `discard` are absolute.
  - `medium.source` and a secret's `source` must carry one of the schemes J lists. `machine.source` need not, because a bare string is an image ref.
- **Names:**
  - A machine's name and `hostname` are DNS labels, since both become `<name>.hull`.
  - An `env` name is letters, digits and `_`, not starting with a digit.
  - A secret's name is letters, digits, `_` and `-`, because it becomes one path component under `/run/hull/secrets/`.
  - `arch`, `backend`, `fidelity` and a medium's `type` are lowercase words.
  - `allow` takes any symbol, since a PCI address is a device id.
- **`refresh` needs `(network true)`**, the one rule across keys, which J states.
- **A file** holds `machine` or `machines`, not both, and `build` beside either or alone. `(machines)` must name at least one.

**Open: `tuning` and plugin engines.** J.5 defines entries for `vm`, `emu`, `ns`, `qemu` and `virtualbox`. The check refuses `docker`, `vmware` and `hyperv` as reserved, and any other name as unknown. But the engine set is open (§5.0), so a plugin's tuning keys will need a schema from somewhere, presumably its capability declaration in `hull_engine.h`. Until that exists, a plugin engine cannot be tuned.

**Open: J says its schema "will be a situ description", and for the readable file that does not fit situ as it stands.** situ describes bytes in a fixed order. A readable record takes its keys in any order, with whitespace and comments anywhere. A manifest's canonical bytes fit, which is why A.2 could become `manifest/manifest.situ`, and a canonical `spec.resolved.conf` would too. Until it is decided whether the readable inventory is generated from a description, these tables are the inventory.

**Every ```` ```scheme ```` block in this document is a whole `hull.conf`**, and `make test` holds it to the check: `test/spec_test.c` reads `project.md` and opens each one. It fails on fewer than four, the count when it was written, so a missed block cannot pass as an empty scan. The §3.10 media example had no `source` and gained the dir source its own prose names.

**Measured:** `test/spec_test.c` runs 113 cases, each refusal checked for its status, offset and path. In a scratch copy, sabotaging each of the twelve checks failed the suite: duplicate key, missing key, unknown key, choice, port range, the refresh rule, duplicate name, the path, the size rule, the label rule, the entry head and the extra value. Giving the example scan a file with no blocks failed it too.

### 4.4 The matrix: a machine against one engine's column

**`plan/matrix.c` resolves a checked machine against one engine's capability declaration**, the step §6 calls "consulted at plan time". `hull_matrix_resolve()` takes a machine, either `(machine …)` or a named entry of `(machines …)`, together with the engine's `hull_capabilities`, the class the engine runs it as, and the user's `--accept` tokens. It reports every field the machine asks for, in the order the spec writes them, as native, emulated, accepted, rejected or ignored, so `hull plan --explain` can print the lot. A field the engine does not declare is rejected, never ignored (§5.0). A class the engine does not implement is refused before anything is reported.

**Field ids are derived from the spec as written**, which is where `hull_engine.h` said the list lives:

- **Below the machine, a key's id is its path:** `command`, `security.readonly_root`. For a `tuning` entry it is J.5's form, `vm.profile`, without `tuning.`.
- **A value chosen from a set is carried in the id:** `projection.block`, `on_disconnect.keep`, `vm.profile.micro`, `qemu.accel.tcg`. An engine can then support one choice and refuse another, as virtualbox must refuse `vm.profile.micro` (J.5). `vm.kernel` carries `bundled`, `image` or `path`.
- **`source` carries its scheme and its mutability:** `source.dir.direct`, `source.ref.overlay` (`ref` is a bare image ref). This is what lets docker refuse a dir source in place and emulate one with an overlay (§5.0). `mutable` is folded into it, with J.1's default where it is not written.
- **Each `fidelity` need is a field:** `fidelity.inotify`.
- **`false`, and a list with nothing in it, ask nothing.** Numbers and strings ask for the key itself: `cpus`, `memory`.
- **`ports`, `volumes`, `env`, `secrets` and `security.allow` are asked for once, by key.**
- **`backend`, `restart` and `env_file` are hull's own:** the first chose the engine, the systemd generator reads the second, and the core folds the third into `env`. They are reported native without being looked up.

Two keys are checked against lists the declaration already has rather than against ids:

- **A medium's type is checked against `media_types`**, since J.4 says so. The key is looked up as `media`. `hull_engine.h` used `media.cdrom` as an id example, which would have been a second way to say the same thing, and it is gone.
- **An arch is a class's question before it is an engine's.** The host's own asks nothing, written as `host` or by name, which is why the matrix is given the host's arch. Any other is refused outright for `ns` and `vm`, which share the host's kernel or accelerate it (§6). For `emu` it must be in the engine's `arches`. A per-engine list alone was wrong: qemu lists foreign arches for emu and would have offered them to vm as well.

**`--accept` tokens are their own field.** §6's rule 1 lets the user honour a rejection, and `hull_cap_entry` gained `accept` for the token that does it. The draft had put the token inside the free-text `note`, which would have had to be parsed out. A rejection with no token cannot be honoured.

**`tuning`** applies an entry that names the class the engine runs the machine as, or the engine itself, and `vm`'s under emu as well. §4's example calls emu's entry the one that "adds emulation tuning", and §5.3 gives emu "same profiles, same kernel strategy". Any other entry is reported ignored, the warning §4 promises. J.5 also says an engine's entry overrides its class's, but no key of J sits under both, and §4.3's check refuses any other key. Nothing is there to override yet, so there is no code for it until a key is shared.

Open, each for the planner's next step:

- **`(ns (binfmt true))` does not change `arches` yet.** An ns engine that can run foreign arches with qemu-user (§5.3) has no way to say "only with binfmt".
- **Choosing the engine is §4.5's.** What an engine's `create()` receives, the plan itself, comes after both.

**The matrix judges what it is given and supplies no defaults.** §4.7's plan writes them in first, so through `hull plan` a default asks of an engine what the same value written would.

**Measured:** `test/matrix_test.c` runs 37 cases against two made-up columns, shaped like §5.0's qemu and docker. Each case states every field it expects reported, so an extra field fails as surely as a missing one. In a scratch copy, sabotaging each of thirteen rules failed the suite: undeclared rejected, a token honoured, a token required, the class check, ignoring other tuning, `false` asking nothing, the choice in the id, the source's default mutability, arches, media types, `allow`, the returned status, and hull's own keys. So did breaking the class rule for a foreign arch and the host's arch by name, added after the first version took `(arch amd64)` on an amd64 host for a foreign one.

### 4.5 The registry: which engine a backend chooses

**`plan/registry.c` holds the loaded engines and makes the choice `(backend …)` asks for**, producing the engine and class that §4.4's matrix takes. It holds pointers to what `hull_engine_register()` returned, up to 32, and allocates nothing.

**Registering checks a declaration once**, so the matrix and everything after it can read one without checking it again. `hull_registry_add()` refuses, each with a reason:

- an engine built for another major ABI;
- a `hull_engine` or `hull_capabilities` shorter than this build's. Minor 1 is the first version, so there is no older struct to read short yet. When the minor moves, this becomes a check per field.
- a missing declaration, operations table or entry table;
- a name that is not a lowercase word, or that is a class's name;
- no class, or a class other than `ns`, `vm` and `emu` (§5.0);
- a name already registered.

**A class chooses the host's default engine of it.** That is the engine host policy names through `hull_registry_set_default()`, which `policy.conf` (§4.1) will call. Until a default is set, it is the first engine registered that implements the class, so registration order is the host's preference.

**An engine named outright runs as the first class it lists**, unless the machine's arch is foreign and the engine implements `emu`, which J.1 says a foreign arch means. "Foreign" is anything but `host` and the registry's own host arch: `(arch amd64)` on an amd64 host is not foreign. A class named outright is not second-guessed: `(backend vm)` with `(arch arm64)` stays vm, and the matrix refuses the arch. With no `backend` written, J.1's `ns` applies. A caller may name the backend instead of the spec, as `hull check --backend a,b,c` (J.8) must.

**Open:**

- **`(qemu (accel tcg))` does not make qemu run as emu.** J.5 says tcg "⇒ emu class semantics", but that is a fact about one engine's tuning key, and the registry knows no engine's keys. Either the declaration says which of its tuning values imply a class, or `(backend emu)` is the only way to ask. For now it is the latter.
- **Nothing calls `set_default` yet**: `policy.conf` has no schema.

**Measured:** `test/registry_test.c` runs 37 cases, and every refusal must carry a reason. In a scratch copy, sabotaging each of thirteen rules failed the suite: the major ABI, the struct sizes, the name rule, the class list, duplicates, the limit, the policy default, first-registered order, foreign means emu, the host's own arch, `ns` when unwritten, an exact name match, and a default that must implement its class.

### 4.6 `hull plan` and `hull check`

**`build/hull` is the first binary**, and it reaches as far as planning: §10's `plan` and `check`, and `engines`.

- **`hull plan`** resolves each machine in a file for the backend the machine names, or for one given with `--backend`. It prints what was rejected, emulated, accepted or ignored, and with `--explain` every field. `--resolved` prints the machine as §4.7's plan wrote it, every default in.
- **`hull check --backend ns,vm,emu`** does the same for every backend listed. It is J.8's CI gate: it exits 1 if any machine is rejected on any of them.
- **Every message names the file, line and column** of the entry that asked, as a compiler's would. A rejection that a token can lift says which token.
- **Exit status:** 0 when nothing is rejected, 1 when something is, 2 when the file cannot be read or checked, or the command is wrong.

**The two engines that ship with the core are declared in `engine/`**, docker and qemu. Each entry comes from what §5 and §6 say of that engine, with a comment naming the section. A field the document does not say an engine honours is left out, and so is rejected:

- **docker** has no `fidelity.odirect` and no media.
- **qemu** offers `fidelity.inotify` only with `--accept vm:inotify-poll`.

Their operations were not written then: every slot was NULL, which was enough for planning and nothing else. Both run machines now (§4.9, §4.11), and each declaration shrank to what its engine does. `hull --version` prints `VERSION` and the copyright line.

**The host's arch comes from `uname`**, in the spec's spelling: `x86_64` is `amd64` and `aarch64` is `arm64`.

**Measured:** `test/cli_test.sh` runs 35 cases against `build/hull`, each an exit status and a line of output. Among them, §4's example passes `check --backend ns,vm,emu`, read straight out of this document. In a scratch copy, breaking the exit status, the passing of `--accept` tokens, the loop over backends, the loop over machines and the location of a message each failed it.

### 4.7 The plan: every default written in

**Decided 2026-10-13: the planner resolves defaults, and engines assume none.** The question was open in §4.4. It turned out to be settled already under another name: §2's lifecycle compiles "MachineSpec + chosen backend into an ExecutionPlan" and runs "every spec field" through the matrix, and §3.1 keeps a `spec.resolved.conf` beside each machine.

**`plan/plan.c` writes the resolved machine as readable text**, the form `spec.resolved.conf` takes (§4.1). Writing it is copying:

- Every entry the user wrote is copied byte for byte. Comments between entries are dropped, and those inside an entry stay.
- The planner adds only what was left out: a default inside a record that was written, or a whole entry that was not. Three records are opened up to add to: `security`, each `port` and `volume`, and the `tuning` sections that apply.
- Tuning for another class or engine is copied as it stands, and the matrix reports it ignored.

**The text is then judged as if the user had written it.** It is opened again with `hull_spec_open()`, which holds the planner's own output to Appendix J; a failure there is a bug in hull and is reported as one. It is then resolved by §4.4's matrix, so a default asks of an engine exactly what the same value written would, by the same ids. Every report is moved back to the user's file. A position inside a copied entry maps to its own byte, and an added default maps to the entry it was added for: `(proto tcp)` points at its `(port`, and `(writeback none)` at the machine.

**Defaults written in**, from J or from §4.3's choices:

- `mutable` direct for a dir source, overlay otherwise; `writeback none`, `ssh false`, `on_disconnect stop`.
- `workdir "/"` for every source but a bare image ref, whose manifest supplies one (A.4).
- `hostname` as the machine's name, in a `(machines …)` file.
- `security` with `readonly_root false` and `no_new_privs true`.
- `proto tcp` and `mode rw`.
- The tuning sections that apply, with J.5's defaults.

**Left out, because it is not the plan's to give:**

- `command` and a ref's `workdir`, which are the manifest's; the plan reads no store yet.
- `cpus` and `memory`, which J leaves to the engine.
- `restart`, `env_file` and `backend`, which are hull's own (§4.4).

**`(projection auto)` is §3.9's rule 3.** The plan tries bind, passthrough and block in that order and keeps the first that leaves nothing rejected. If none does, it keeps the first the engine declares, so the report names what is actually wrong. No engine is asked for `auto`, so neither built-in declares it. A projection written by name is kept.

**§3.9's rule 2 is checked here**, since it spans keys and the matrix judges one field at a time. `(mutable direct)` under a block projection is rejected, as `writeback`, unless `(writeback sync-on-stop)` is set.

Open:

- **A fidelity need is not judged per projection.** Rule 3 has the plan pick a projection that satisfies every declared need, but `fidelity.inotify` is one id whatever the projection, so a need never moves `auto`. Fixing that means ids like `fidelity.inotify.passthrough`, or a projection field on an entry. Either changes what engines declare.

**What crosses the ABI is `(plan (class vm) (machine …))` in canonical form** (A.1, `hull_engine.h`): the resolved machine, converted as §4.2 describes, and the class it runs as, which qemu needs because it runs as vm or emu. Two spellings of one machine encode to the same bytes, as A.1 promises: differing in comments, key order, `(projection auto)` and defaults written out.

A.1's "no floats" sits oddly with J.5's `cores_scale`, which takes `1.5`. Canonical atoms carry no type, so it is just the atom `3:1.5`, and nothing breaks. But A.1 was written for the manifest, and a decimal in the plan is a decision J.5 made without saying so.

**Measured:** `test/plan_test.c` runs 23 cases, each stating the whole resolved text or every report with its offset, against made-up engines that force each choice both ways. In a scratch copy, breaking each of twelve rules failed it:

- keeping what a record wrote;
- emu taking vm's tuning;
- copying other engines' tuning untouched;
- direct for a dir source;
- no workdir for a ref;
- the hostname from the name;
- auto trying each projection in turn;
- the writeback rule;
- copied offsets;
- added offsets;
- the size limit;
- moving reports to the user's file.

Two of these had first passed: copying other tuning and copied offsets. Each was caught only after a case was added for it, one with a section missing a key and one with an offset inside a copied entry, not at its start.

### 4.8 The reference engine, and `hull run`

**`engine/chroot.c` is the first engine that runs a machine**: §5.0's "chroot — near-trivial, useful as the reference plugin implementation", done rootless. A machine is one process.

- **Namespaces:** it is cloned into new user, mount, UTS and PID namespaces, with the invoking user mapped to root. The dir source is bound onto itself, read-only for `(readonly_root true)`, and chrooted into. `/proc` is mounted where the dir has a `/proc` to mount it on; nothing is created in the user's dir.
- **Then, in order:** it changes to `workdir`, sets `hostname` and `no_new_privs`, and execs `command` with the environment `env` names and nothing else.
- **Its declaration says exactly that.** It is an `ns`-class engine. It shares the host's network, so `ports` is declared rejected with that reason. `cpus`, `memory` and an image ref are left out, so they are rejected.
- **Registration order:** it registers after docker, so `(backend ns)` still picks docker, and `(backend chroot)` asks for it.
- **The bind remount must keep the flags its mount was copied with.** A user namespace locks `nosuid`, `nodev` and the rest, so the engine reads them with `statvfs` and keeps them. bwrap meets the same rule.

**A dir source as an overlay is an overlayfs in the machine's mount namespace, rootless, its upper layer a tmpfs.** It is declared emulated, as qemu's is (§4.11): writes are held in memory and go with the machine.
- **No other place on the host:** the directory is cloned with `open_tree()`, then a tmpfs is mounted over the directory's own path, which only this namespace sees. In it the clone becomes `lower/`, beside `upper/` and `work/`, and the overlay is mounted at `merged/`, which is chrooted into. An exec's helper enters the init's root, so it sees the overlay.
- **The overlay's paths are relative,** so a comma or colon in the directory's path cannot change its options. It takes `userxattr`, so overlayfs keeps its own records in `user.overlay.` xattrs, which tmpfs holds. Without it the mount still succeeds, since the namespace cannot write `trusted.` xattrs and overlayfs silently does without. It then keeps no records at all: the upper copy of a directory carried no xattr where `userxattr` gave it `user.overlay.origin`. And removing a directory of the lower layer fails with EIO.
- **`/` keeps the directory's mode.** No chown is needed: the namespace maps only the user and the user's group, both to root, and root made the upper layer, so the directory's owner is root's here already or cannot be given.
- **The cost, measured: a change to a file whose owner or group the namespace does not map fails with EOVERFLOW.** A change copies the file up, with its parents, keeping their owners, and an unmapped one cannot be. In place, the same change works under the host's permissions. Found by the suite: the build tree is setgid to `users`, which is not the user's primary group, so removing a file in `/srv` failed. Mapping more ids takes `newuidmap` and `newgidmap`, which are setuid root. §7 holds both sides of this: its ns bullet maps root in a machine to "an unprivileged subordinate uid", which needs those helpers, and its first bullet says "no setuid helpers". Which one gives way is the holder's decision, and until then the namespace maps one uid and one gid.
- **With a read-only root** there is nothing to layer: the directory is bound read-only, as in place.
- **chroot is not in Phase 1's flip:** it refuses the flip's port, since it shares the host's network.
- **Measured** in `test/cli_test.sh`: `/`'s mode and owner as in place, a write and a removal kept from the directory, the EOVERFLOW of an unmapped group, a directory emptied, removed and made again coming back empty, a read-only overlay refusing a write, and an exec seeing the overlay. In a scratch copy, breaking each of these failed a named case: reading the mutability, the top's mode, keeping a read-only root off the overlay, chrooting into `merged/`, and `userxattr`. `userxattr` first passed, failing only an unrelated `ps` case of the load intermittent (§4.10); the directory case was added for it.

**Volumes are bound in with the new mount API, so the guest path is resolved inside the machine.** Each host side is cloned with `open_tree()` before the chroot, made read-only with `mount_setattr()` for `(mode ro)`, and attached with `move_mount()` after it.

- **Why not a plain bind mount:** one made before the chroot, at the rootfs path plus the guest path, would follow a symlink in the rootfs on the host. A rootfs holding `/var/data` → `/home/you` would get the volume on the host's path, not its own.
- **Following a symlink is safe here:** the chroot confines where it can lead, so `MOVE_MOUNT_T_SYMLINKS` follows one at the guest path inside the machine.
- **The guest path must exist:** nothing creates it, since creating it would write into the user's dir, and the declaration says so.
- **A host path is relative to the file**, like every other.

**The first process is a small init, and the workload is PID 2.**

- **Why not the workload as PID 1:** PID 1 ignores any signal it has no handler for, so a workload there would not die on `stop`'s SIGTERM. And it would have to reap orphans, which most programs never do.
- **What the init does:** it does all the setup, forks the workload, forwards it SIGTERM, SIGINT, SIGHUP, SIGQUIT and the two user signals, and reaps everything orphaned to it. It reports the workload's wait status to the engine through a pipe, so a signal is still reported as a signal.
- **A SIGKILL to the init ends the whole namespace,** so nothing the workload started outlives the machine. This is §5.2's guest agent in miniature.
- **A signal that lands just before the init's wait blocks is forwarded one wakeup late.** `stop`'s SIGKILL after its timeout is the backstop.

**Output goes to the core through `on_data`**, as `hull_engine.h`'s rule 3 requires: one thread per machine reads the workload's stdout and stderr, then waits for it and reports `on_exit`. `stop` sends SIGTERM, then SIGKILL after its timeout.

**The workload leads a session of its own.** Without it, a terminal's Ctrl-C, or any signal sent to hull's process group, reaches the workload directly, behind the engine. Only `stop` should end a machine. The test found this: `timeout` signals its whole process group, so the probe was being killed by `timeout`'s signal, not by hull's `stop`, and a broken `stop` passed.

**`hull run [-f FILE] [--backend B] [--accept TOKEN]... [NAME]`** plans one machine: the file's, or the one NAME names in a `(machines …)` file. It then creates, starts and waits for it, relaying output, and turns SIGINT or SIGTERM into the engine's `stop`.

- **Paths in a file are the file's:** the plan carries the file's directory as `(plan (base "/dir") …)`, and engines resolve a relative path against it through `engine/access.c`. `(source "dir:./rootfs")` works from anywhere, and nothing reading a plan depends on where it was started, which a long-lived core serving many files needs. Ignoring the base fails 25 of the CLI test's cases, which run from `/`.
- **Plan reports go to stderr**, so the workload's stdout stays its own.
- **Since §4.10, `run` goes through the core**, and the machine's output and exit come back over the frontend protocol.
- **Exit status follows docker's convention:** the workload's status, 128 plus the signal that ended it, or 125 when hull could not plan or start it. A rejected plan therefore cannot be mistaken for a workload exiting 1.
- **An engine whose operations are not written** says so and exits 125. Every built-in engine has them now, so no case reaches it.

Open:

- **`command` is a path.** There is no PATH search, and no manifest supplies a default, so a chroot machine must write `(command …)`.

**Measured:** `test/cli_test.sh` runs 67 cases. 32 of them are `hull run` against a rootfs holding one static program, `test/probe.c`, which reports from inside what the machine looks like: env, cwd, hostname, uid, `no_new_privs`, whether it can write, and whether it leads its session. The probe's `wait` step ends itself after 20 seconds, so a broken `stop` cannot leave one behind.

In a scratch copy, breaking each of these failed the test:

- the read-only remount;
- the hostname;
- the workdir;
- `no_new_privs`;
- the environment;
- keeping stderr apart from stdout;
- reporting a signal as a signal;
- reporting an exec failure;
- stop's signals;
- the uid map;
- the workload's own session;
- volumes read from the plan;
- a volume's mode;
- its read-only attribute;
- attaching volumes before the chroot, on the host, which the case with a symlink to `/mnt` exists for;
- the PID namespace;
- mounting `/proc`;
- the init reaping orphans: the probe leaves a grandchild to it and counts `/proc`, where a zombie would show;
- forwarding signals;
- the status pipe.

Three of these had first passed: stderr, stop and the session. Each was caught only after the test was changed to see it.

### 4.9 The docker adapter

**`engine/docker.c` drives the Docker daemon through its API**, as §5.0 says: HTTP/1.1 over the daemon's unix socket (`$DOCKER_HOST` when it names one), API `v1.41`, one connection per request.

- **A machine is one container.** It is created from the plan, then attached to before it starts, so nothing it writes is lost, then started.
- **Output** arrives on the attach stream in docker's multiplexed frames and goes to the core through `on_data`. When the stream ends, the reader asks `/wait` for the exit status.
- **`stop`** is the daemon's stop: SIGTERM, then SIGKILL after the timeout. `destroy` removes the container by force.
- **Every container is named `hull-<hostname>-<pid>-<n>` and labelled `hull.machine`,** so one left behind by a hull that was killed can be found and removed by name. That is `running-code.md`'s point about a container: it belongs to the daemon, and a killed client cannot clean it up.

**The workload runs as the invoking user's uid and gid**, by §7's rule. Under a rootful daemon the container's root is host root, so the adapter does not use it.

- **Volumes:** what a workload writes through one is the user's own file, and the test checks it.
- **Capabilities:** docker gives them only to a root workload, so `(allow …)` is declared rejected on docker with that reason. Granting them by running as root would be the end of the line, reached by default.

**What the create request carries:**

- **The basics:** the image ref, `Cmd`, `WorkingDir`, `Hostname` and `Env`.
- **Volumes:** `Binds`, with host paths made absolute, since the daemon resolves nothing relative. `hull run` stands in the file's directory.
- **Ports:** `PortBindings` on the machine's address (§4.10), tcp or udp as the plan says.
- **A udp port on docker is not quite qemu's,** measured 2026-10-20 on docker 26.1.5, where the daemon runs no userland proxy and the kernel's NAT carries the datagrams. It drops an empty datagram, which hull's qemu forwarder delivers. And a datagram refused in the container, sent while nothing listens, moves the client to a new port there: the probe saw 59316, then 58599 once it listened again, where qemu's flow keeps the client's port (§4.11). Both are docker's, not hull's: hull only asks for the binding. A server that keys state on the client's address loses it on docker, and not on qemu.
- **Limits:** `NanoCpus` and `Memory`.
- **Security:** `ReadonlyRootfs`, `no-new-privileges` and `CapDrop ALL`.
- **`Init`, docker's own tini, as PID 1,** for the same reason §4.8's chroot engine has an init: a workload at PID 1 ignores `stop`'s SIGTERM, and nothing reaps. A command that is not there is therefore the init failing to exec it, status 127, as a shell would report it, not a failed start.

**A dir source runs as an overlay, from §5.0's "cached docker import of the tree".** In place it stays rejected: docker cannot run a directory as it stands.
- **The image is named for the tree:** `hull-dir:<SHA-256 of its tar>`. The daemon is asked for it first, so an unchanged tree is imported once. A second run of the test's tree took 0.6 s.
- **The tar is the same bytes for the same tree** (`engine/tree.c`): ustar, with a pax header for what does not fit, entries in bytewise order, and only the mode, owner and mtime on disk. Every entry is opened from its directory's descriptor, so no symlink is followed. A hardlink is written as a file of its own, a socket is left out, and a device node is refused, since the root daemon would make the node.
- **Read twice, tagged once.** The first reading gives the digest. The second streams into an untagged import while it is hashed again, and the image is tagged only if the two digests agree, so a tree that changed between the readings never wears a name it does not hold. Otherwise the image is deleted and the run refused.
- **Its cost:** the files keep the user's owner, so the user writes where they own, and writes land in the container, as an overlay's do. `/` itself is root's, as docker makes every image's, and the declaration says so. A `./` entry for the root was tried, and docker ignores it.
- **`engine/sha256.c`** is FIPS 180-4's SHA-256, which the store will want as well (§3.5). `test/sha256_test.c` holds it to FIPS's examples, the three lengths where the padding does or does not take a block of its own, and one input fed in pieces of every size.
- **`test/tree_test.sh`** reads the tar back with Python's `tarfile` and GNU tar, neither sharing its code: every entry's kind, mode, owner, mtime, link and content, long names and a long link through pax, bytewise order, the same bytes twice and for the same tree made in the other order, a changed byte changing them, and an unreadable file named.

**The declaration shrank to what the adapter does.** `ssh` and `secrets` left it, because nothing provisioned sshd or materialised a secret. `secrets` came back on 2026-10-21, once they were materialised (§4.10). One consequence: §4's example, which has `(ssh true)`, is refused on ns for that one field. Declaring `ssh` would run the machine without sshd and without a word, which §6's rule 1 forbids.

Open:

- **An image that needs root inside**, an `apt-get` in a build step say, fails on docker as the user. It wants an engine that maps root to the user, as chroot and the native ns engine do, not docker's root.
- **Images must already be local.** Nothing pulls; the OCI border (§3.5) is Phase 3.
- **Sabotage,** baseline first, in scratch copies. Breaking each of these failed a named case: the tar's sorting, its pax names, its checksum, its mode and its mtime; a SHA-256 round constant and its padding boundary; the daemon's cache being asked, and the tag's name. Breaking the tag left images under the wrong names, removed afterwards by the ids docker's event log gave for imports in that window.
- **Phase 1's exit is met, and `make check-flip` holds it (§12).** The file has to say `(mutable overlay)`, since docker refuses the in-place default. chroot runs an overlay too (§4.8), but it is not one of the three: it refuses the file's port.
- **One import per tree is kept, not one per state of it.** Each import is labelled `hull.tree`, the SHA-256 of the tree's real path, so two spellings of a path or a link to it are one tree, and no path can break the label instruction or the query. A fresh import removes the tree's others, unforced, so the daemon keeps one a container uses or one with another name too. Each is labelled `hull.path` as well, the real path in hex, so that gc can ask whether the tree is still there.
- **`hull gc` removes an import nothing wants, unforced,** added 2026-10-20: one whose tree is no longer a directory, and one with no `hull.tree` or `hull.path` label, being older than the labels. The daemon keeps one a container uses, and gc reports it kept with the daemon's reason, then removes it once the container has gone. A live tree's import is left alone. It is the engine's `gc` op, appended to `hull_engine.h`'s ops under rule 4. The core asks each engine that has one, on a thread of its own, since a daemon's answers may take a while, and the answers reach only the frontend that asked: each client's place carries a generation, as a machine's slot does. §3.4's store has a `hull gc` of its own to come, by reachability, and that will join this one.
  - **Sabotage,** baseline first: breaking each of these failed a named case: taking an unlabelled import as wanted, never finding a tree gone, not writing `hull.path`, and the core never saying `done`, which leaves `hull gc` waiting until its `timeout`. Not tested: forcing the removal, which the daemon refuses for a running container anyway (above), and the client's generation, which no case can make reach a second client.
  - **Sabotage:** breaking each of these failed a named case: retiring at all, the label, keeping the fresh import, and the real path. Removing with `force=true` first passed. The daemon refuses even a forced removal of an image a running container uses, so that case could not tell. An image with a second name is what force would take, and the case for it fails.
- **Untested:** a tree that changes between the two readings, which a test cannot time on purpose.

**`make check-docker`**, not part of `make check`, runs `test/docker_test.sh` against the daemon the account can reach. It uses `alpine:latest` and does not pull it. There are 68 cases:

- output on its own stream, the exit status, env, workdir, hostname and the user's uid;
- volumes, a write through one that lands as the user's file, and a read-only root;
- no capabilities, and a grant refused;
- a signal from the workload, and SIGTERM to `hull run` stopping the machine inside docker's ten seconds;
- a port reached from the host on the machine's address, and not on 127.0.0.1; and a udp port, to the probe's echo, each of two clients answered on its own and datagrams of 1, 1400 and 65,000 bytes whole;
- `hull ssh`, alpine's login shell on a terminal with the frontend's TERM;
- a dir source as an overlay: imported under the digest the test works out with `sha256sum`, run as the user, writing into the container and not the directory, imported once for two runs and again once changed, labelled with its tree's path, an older import kept while a machine uses it and retired once nothing does, and one with a second name kept; in place, refused;
- `hull stats`: the probe's CPU, memory and synced writes, an ended exec's CPU and writes added, and an ended container refused;
- `hull proxy-ssh` reaching the container's undeclared port 22 both ways, and refused while nothing listens;
- `hull gc`: an import whose tree was deleted removed, one made by docker with no label removed, one a running machine uses kept with the daemon's reason and removed once it has stopped, and a live tree's left alone, each checked against what the daemon then holds;
- a kept machine under a core killed outright: its file written, taken back by the core `hull ps` starts, its log from before and after, an exec, its exit through `attach`, and its file and container gone after; one whose container went meanwhile forgotten, one that is not hull's not taken, and one stopped no longer kept;
- a missing image and a missing command;
- no `hull-dtest-` container left at the end.

In a scratch copy, breaking each of these failed it:

- the init;
- dropping capabilities;
- running as the user, which also made the write through a volume fail;
- keeping stderr apart;
- the read-only root;
- the read-only volume;
- the port bindings, and, on 2026-10-20, binding every port as tcp;
- the environment;
- the workdir;
- the exit status;
- removing the container, which left 15 behind, and the harness removed them by name.

The read-only volume had first passed: while the container ran as root, a write into a read-write volume was refused as well, for want of a capability. A test asking only whether the write was refused could not tell the two apart, so both read-only cases now require "Read-only file system" by name.

`make check` itself needs no daemon. Its case for an engine with no operations moved from docker to qemu, and went once qemu ran machines (§4.11).

### 4.10 The core: one per user, local first

**Decided 2026-10-15, by the holder: build the per-user core now, on a local socket, with a protocol of hull's own.** §2's core is what exec, logs, machines that outlive a terminal and `(on_disconnect keep)` all need. The shared encrypted protocol §2 names for frontends is still unsettled (Handoff notes, item 2), so the core starts local. What it does is fixed now; moving it onto the shared protocol later changes how frontends reach it, not what it does.

**The socket is `$XDG_RUNTIME_DIR/hull/core.sock`**, mode 0600 in a 0700 directory: the user's own, and nobody else's, which is §7's "user-owned socket". A socket address holds 108 bytes, and a runtime directory can be deeper than that leaves room for. When it is, the core and the frontend both name the socket through a descriptor on its directory, `/proc/self/fd/N/core.sock`, which is short whatever the directory's path. That was found because the first sabotage run's scratch directory was that deep: its core never started, and every sabotage failed the same 39 cases for that reason alone.

**The protocol is described in situ, `core/frontend.situ`**, beside Appendix B's. It uses the same framing (`agent/message.situ`: a big-endian u32 length, then one canonical s-expression of at most 1 MiB) and the same spelling.

- **A message** is `(type (ch n) …)` with its entries in canonical order. The type is chosen by the four bytes after the `(`.
- **Channels:** channel 0 is control. The frontend allocates every other channel, one per request, and the core answers on the request's channel.
- **`make test` holds the schema to vectors:** `core/frontend.vectors`, every type in the direction that sends it, and `core/frontend.refused.vectors`.

| Frontend → core | Core → frontend | Purpose |
|---|---|---|
| `(hello (ch 0) (proto n))` | `(hello (ch 0) (core_version v) (proto n))` | version |
| `(run (ch n) (machine id) (plan …))` | `(started (ch n) (machine id))`, then `data` and `exited` | run a machine from a plan the frontend made (§4.7) |
| `(attach (ch n) (machine id))` | `data`, `exited` | a running machine's output from now on |
| `(logs (ch n) (follow true\|false) (machine id))` | `data` …, then `eof` unless following | what it has written, kept by the core |
| `(stop (ch n) (machine id) (timeout_ms n))` | `(done (ch n))` | SIGTERM, then SIGKILL |
| `(kill (ch n) (machine id) (signal HUP\|INT\|QUIT\|TERM\|USR1\|USR2\|KILL))` | `(done (ch n))` | a signal for its workload |
| `(stats (ch n) (machine id))` | `(stats (ch n) (cpu n) (io n) (memory n))` | what the host spends on a running machine |
| `(list (ch n))` | `(machines (ch n) (machine (address) (class) (engine) (id) (state))…)` | what the core holds |
| `(dial (ch n) (machine id) (port n))` | `(started (ch n) (machine id))`, then `data`, `credit` and `exited`, as for `exec` | a connection to one of the machine's own ports, declared or not (§8.1) |
| `(gc (ch n))` | `data` …, then `(done (ch n))` | what the engines keep that nothing wants, reclaimed: each engine's report on fd 1, an engine that could not look on fd 2 |
| `(exec (argv w…) (ch n) (machine id) (tty false\|((cols n) (rows n))))` | `(started (ch n) (machine id))`, then `data` and `exited` | a command in a running machine |
| `(data (buf b) (ch n) (fd 0))`, `(eof (ch n))` | `(credit (bytes n) (ch n))` | an exec's stdin, and how much more the core will take |
| `(credit (bytes n) (ch n))` | | how much more of an exec's output the frontend will take |
| `(winch (ch n) (cols n) (rows n))` | | its terminal resized |
| `(signal (ch n) (signal HUP\|INT\|QUIT\|TERM\|USR1\|USR2\|KILL))` | | a signal for it |
| | `(lost (bytes n) (ch n))` | output a follower fell too far behind to read |
| | `(data (buf b) (ch n) (fd 1\|2))`, `(exited (ch n) (code n) (signal n))`, `(eof (ch n))`, `(error (ch n) (reason r))` | as B.3's |

**The schema leaves these to the core:** that a channel is not 0 and not in use, that a machine id is a DNS label, that a terminal is 1 to 65535 in each direction, that a signal is one it forwards, and the elements of a run. situ frames `machines`' entries and the plan inside `run` without reading them. The refused vector `machine_no_state` is refused because the run's framing falls short, not because an entry was checked, and its comment says so.

**Every message read is validated by C situ generates from the schema**, for its direction, before anything reads it (`core/wire.c`). The generated C does not compile as situ writes it: it uses a validator of the imported `canonical.situ` before declaring it. That is situ's finding 18, and reproduces with a three-line schema at the pin and at situ's newest. `core/frontend_view.h` declares that one function ahead of the generated header, and is put in front of the generated `.c` with `-include`. Nothing situ wrote is edited, and the file and its make rule go when situ fixes it.

**`hull core` is the core: `core/core.c`.**

- **Structure:** one thread owns the machine table and polls the socket, the clients and a wake pipe. Engines report from their own threads into a queue, so the table has one writer.
- **Each machine gets its own `hull_core` to hand its engine,** because `on_data` names a core and not a machine. Its generation is carried on every event, so a last state an engine reports after an exit cannot land on a later machine in the same slot.
- **A stop runs on a thread of its own,** since an engine's `stop` blocks for up to its timeout.
- **What it keeps:** the newest 1 MiB of each machine's output, for `logs` and its followers, and every machine it has run, `exited` once it has, until a new run takes the slot or the core goes.
- **Nothing it does waits on a frontend.** Each frontend's socket is non-blocking, and what it is sent waits in a queue of its own that `poll()` drains as the socket takes it. It used to be a blocking send that gave up after five seconds, so one frontend that stopped reading stopped the core for five seconds, every client and machine with it, and was then dropped. A frontend that leaves 8 MiB of answers unread is dropped. Nothing else can fill a queue that far, because an exec's output is bounded by its credit and a machine's output by its log (below).
- **A machine's output is read from its log, by each follower at its own pace.** `run`, `attach` and `logs` are followers, each holding its place in the log, counted in bytes the machine has written. A follower is sent more only while less than 64 KiB of its frontend's queue waits. One that falls behind holds back neither the core nor the machine. It loses only what leaves the log before it reads it, and is told how much in a `lost`, between the data before and after. `hull` prints `hull: N bytes of output lost: this reader fell behind` on stderr. A follower hears the machine's `exited` once it has read to the end of the log.
  - **Why the machine is not held back:** it has many followers, and the log is one of them. Holding the machine for the slowest would let one `attach` left in a stopped terminal stop the machine for everyone, and the log already drops output past 1 MiB. An exec's output is held instead (below), because it has one reader, which asked for it.
  - **What it costs:** `hull run m | slow` loses output once `slow` is more than 1 MiB behind, where the same command on the host would be held by its pipe. Holding the machine back for the frontend that ran it, and for nobody else, is the alternative. It would use the exec's mechanism, the engine's reader waiting in `on_data`. Whether a foreground run is to behave like a pipe is the holder's decision. This is the first answer, not a settled one.
- **One core per user:** an `flock` on `core.lock`, which also holds its pid. A second core gives way to the first and exits 0.
- **It exits once it has had no client and no running machine for `HULL_CORE_IDLE` seconds,** 30 by default. On SIGINT or SIGTERM it stops its machines and waits for them. Nothing else is left to stop it, which is why it must stop itself.
  - **The idle time is counted from the end of the last busy wait, in milliseconds of the monotonic clock, since 2026-10-21.** Two faults made it shorter than it said. It ran from the last pass of the loop that had a client, so a client quiet for 0.7 s before it left was charged those 0.7 s: measured, a core of `HULL_CORE_IDLE=1` left 314 ms after such a client, five times of five. And it was counted in whole seconds of `time()`, so an idle second begun at x.999 ended at the next tick: 1 ms, four times of five. With both fixed it is 1015 to 1023 ms. In a scratch copy, putting back the start at the last busy pass failed the case of the idle second, and an `attach` that found no core. Putting back whole seconds failed nothing of the idle second's, since the idle second now begins as the busy wait ends, and its first wait sleeps the whole second before looking; the millisecond clock stays because nothing else then holds the idle second to its length, not because a case shows it.
  - **The frontend asks once whether a core runs.** `ps`, `attach`, `logs`, `stop`, `kill`, `exec` and `stats` dialled the core to see whether one ran, hung up, and dialled again to talk; a core whose idle time ended between the two was gone for the second, and with no kept file to start another the command failed with 2 rather than saying no core runs. `connect_core()` now answers both from one connection.
- **A frontend that comes while a core is going reaches the next core.** A going core first unlinks its socket and closes its listener, then stops its machines, flushes its clients, and releases the lock last.
  - **The new core:** a frontend that finds no socket starts one. It finds the lock held and no socket, so it waits for the lock, a minute at most.
  - **A live core still wins.** A core that finds the lock held and the socket there gives way at once, as before.
  - **The idle clock starts at the loop,** not at startup, or a core that waited for the lock would leave before its frontend arrived.
  - **Why:** the going core used to keep listening until the end. A frontend arriving in that window was accepted by the kernel and never answered. A frontend arriving after the unlink started a core that found the lock held and returned 0, and gave up after five seconds. Seen in `make check-qemu`, where an emu run said the core did not start.
  - **The frontend still waits only five seconds for a core.** A going core that takes longer than that, to stop slow machines, still leaves a frontend saying so. The new core then starts anyway, once it has the lock.

**The frontend starts the core when none answers**, double-forked so that it is never the frontend's zombie, in a session of its own, with its output in `core.log` beside the socket, truncated each time. `hull run` now plans in the frontend and runs through the core. `hull ps`, `hull stop [--timeout S] ID`, `hull logs [-f] ID` and `hull attach ID` reach machines the core holds, and none of them starts a core.

- **A machine's id** is its name in `(machines …)`, else its `hostname`, else the file's directory's name, lowercased into a DNS label. Two running machines cannot share one.
- **SIGINT or SIGTERM to `hull run` stops the machine.** A hangup, or the frontend killed outright, is a disconnect, and the machine then does what `(on_disconnect …)` says. `stop`, the default, has the core stop it. `keep` has the core keep it, for `attach`, `logs` and `stop` from another terminal.
- **A kept machine outlives a core that dies, and the next core takes it back: docker's, written 2026-10-20, and qemu's and chroot's, 2026-10-21.** Every built-in engine now does. That is §5.0's `adopt()` and `persist()`, which the ABI already had.
  - **What is written:** once a kept machine has started, the core writes `hull/machines/ID` beside its socket, by a rename: `(4:kept (7:machine ID) (6:engine …) (5:class …) (5:state S))`, canonical, where S is the engine's `persist()`. The file is removed when the machine exits, so a core that stops its machines as it goes leaves none.
  - **What is read:** a core that starts reads them all, holding the lock, before it serves anyone, and calls the engine's `adopt()` for each. A file no core wrote, one whose name is not its machine's, and one whose engine cannot adopt are forgotten. So is one the engine says is gone or is not its own. One the engine could not reach for now, such as a daemon that is down, is left for the next core. `core.log` says which, a line a machine.
  - **Which core reads them:** `hull ps`, `exec`, `logs`, `attach`, `stop`, `kill` and `stats` start no core of their own, except when `hull/machines/` holds a file. The kept machines are a core's to hold, and after a crash no `hull run` may ever come.
  - **docker** persists the container's id. Adopting it means checking that the name is `hull-…`, then following the container's logs from its start, over HTTP/1.0 so that they are not chunked, in place of the attach. So `hull logs` loses nothing the old core had, and the stream ends when the container does: at once for one that ended while no core held it, which then reports its exit like any other. An attach to an ended container waits for good, measured.
  - **qemu** is in §4.11.
  - **chroot's init relays the workload now**, kept or not: it holds the workload's pipes itself and sends what they carry, and the workload's wait status, over a socket pair to the engine, a frame each, `kind fd length` and the bytes. The pipes used to be the core's, so the workload's output went nowhere once the core had gone, and its status with it.
  - **Under keep the init has no parent-death signal.** It binds `init.sock` in a machine directory of its own, `hull/chroot-PID-N`, itself, so that the pid a connection's peer names is the init's; the directory is the user's, 0700, so no one else can connect, and no token is needed, where qemu's vsock port needs one. While no core reads, it holds the workload's last 256 KiB; a workload that ends meanwhile waits for a core to be told. A machine not kept still dies with its core.
  - **`persist()` gives the init's pid, the directory and the whole plan**, from which adopt() reads the machine again as create() does, so an exec after it has the machine's env, workdir and `no_new_privs`; an exposed secret is read again from the secrets' directory, which a machine taken back keeps. Adopting opens a pidfd for the pid, connects to `init.sock`, and requires the peer's pid to be that pid. A pid the init no longer has is forgotten; the peer check is a second witness, since a dead init's socket has no one listening.
  - **The sweep** of dead cores' directories passes over those of machines taken back, as qemu's does.
  - **Measured** in `test/cli_test.sh`: a machine not kept dying with its core; a kept one's file, its probe alive after the core's SIGKILL, taken back by the core `hull ps` starts, an exec, another kept machine started meanwhile without its directory swept, what was written while no core held it, the exit 6 through `attach`, and the file and directory gone after; a workload that ended while no core held it, its 7 heard by the next core; and a kept file whose pid is another process's, forgotten, that process unharmed. The case of the init's descriptors now asks that it hold none of the core's, as its own sockets are by design.
  - **Sabotage,** baseline first. Breaking each of these failed a named case: the parent-death signal kept under keep, the output not held, the end not kept for a core, the directory swept, and the end not said, which every run's status then shows. The peer check is not reached: no case can give a dead init's directory a listener.
  - **A core told to go still stops its machines, kept ones included.** Adoption covers a core killed or crashed, not a core asked to leave.
  - **Sabotage,** in scratch copies, each suite's unbroken baseline first. Breaking each of these failed a named case. In the docker suite: no file written, the file not removed at the exit, no adoption at the start, the frontend starting no core for kept machines, the logs followed from now rather than from the start, and any container taken whatever its name. In `make test`: a daemon that cannot be reached taken as a container gone, a file whose name is not its machine's believed, and the frontend starting no core.
- **Secrets, J.3's, written 2026-10-21 for docker and chroot, then qemu.**
  - **The frontend resolves them**, as the user and on the user's terminal, before it reaches the core, so a command that prompts can. `file:PATH` is the file. `exec:COMMAND` is what COMMAND writes on stdout, run by `/bin/sh -c` in the spec's directory with the frontend's stdin and stderr. `age:PATH` is `age --decrypt`, with `-i` and the file `$HULL_AGE_IDENTITY` names when it names one, and otherwise age asks for the passphrase. A value is at most 64 KiB. A command that fails, a file that is missing and a value too long each refuse the run, 125, before anything runs.
  - **Decided here, not by J.3:** that `exec:` is a shell command line rather than an argv split some other way, and the `$HULL_AGE_IDENTITY` name. age is not installed on this host, so `age:` has been tried only as far as its absence.
  - **Whose decision:** `exec:` runs on the host, as the user, whatever a `hull.conf` names. A cloned project's file can therefore run a host command at `hull run`, as a Makefile can at `make`, where everything else in it runs inside a machine. J.3 has `exec:`, so it is built, and whether `exec:` wants a confirmation is the holder's call.
  - **On the wire:** the run carries `(secrets (NAME VALUE) …)` after its plan, in `core/frontend.situ`. The core takes exactly the names the plan's machine has, each once.
  - **The core writes them** 0400 into `hull/secrets/ID`, a directory of its own beside the socket, and gives the engine its path as the plan's `(secrets DIR)`. A plan that brings its own is refused, as one that brings an address is. They are kept in memory only, so a runtime directory that is not a tmpfs or a ramfs refuses the run. The directory goes at the machine's exit and on a failed create or start. A core that starts removes every one of a machine it does not hold, so a crash leaves them only for as long as no core runs; a machine taken back keeps its own.
  - **docker** binds the directory read-only at `/run/hull/secrets`; the container runs as the user, who owns the files. **chroot** mounts a tmpfs on the dir's `/run`, which must exist, and the directory read-only, nosuid, nodev and noexec under it. Nothing is made in the user's directory, and a dir's `/run` is what FHS empties at every boot. Inside, a file is root's, mode 0400, being the user's.
  - **qemu** reads the files at create and sends them after the machine in the agent's `spec`, as `(secrets (NAME VALUE) …)`, added to `agent/protocol.situ`; the copy it held is zeroed once it is in that message, and the message once it is sent, though the wire's frame and the allocator's earlier copies are not. The agent mounts a tmpfs on the machine's `/run`, which must exist, and a second, nosuid, nodev and noexec, at `/run/hull/secrets`, writes them 0400, and remounts that one read-only. The workload is root in its VM, which could remount it; inside a VM that is the machine's own business.
  - **`(expose env)`** puts `NAME=VALUE` in the workload's environment, and chroot's execs get it too. The frontend warns that every process of the machine can read it. docker keeps it in the container's configuration, where `docker inspect` shows it, which the declaration says.
  - **The tests need a tmpfs** that `$BUILD_DIR` cannot be, so the secrets cases make a runtime directory with `mktemp -d` under `/dev/shm` and remove it at the end. In cli: the files, their contents, their owner and mode, the env, a write refused, the command's stderr and the warning; no directory left after; a stale one gone when a core starts; a failing command, a missing file, a value over 64 KiB and an `age:` refused, and nothing run; a dir with no `/run` refused, with no directory left; and the run refused in the suite's own runtime directory, which is not a tmpfs. In docker: the file read as the container's user, mode 400, a write refused, the env, and nothing left. In qemu: the files, a file's content, its owner and mode, the env, a write refused, a dir with no `/run` refused, and nothing left, the machine's directory included.
  - **Sabotage,** in scratch copies, each suite's baseline first. Breaking each of these failed a named case. In `make test`: the tmpfs check, the files' mode, the read-only mount, removal at the exit, removal of stale ones at the start, chroot's env, a command's status, the 64 KiB bound, the warning, and removal after a failed start. In docker: the bind's place, and the env. In qemu: the values not sent, the read-only remount, the agent's env, the files' mode, and the pid file put back, which fails the qemu cases as it failed every machine under `/run/user`.
  - **Raw-protocol cases** reach what our frontend never sends, framed by hand to a core of the tmpfs's own: a value given twice, one the plan does not name, one missing, values for a plan with no secrets, and a plan that brings its own `(secrets …)`. Breaking each of the core's three checks failed them.
- **The tests never touch the account's own core:** each sets `XDG_RUNTIME_DIR` to a directory under `BUILD_DIR`, with `HULL_CORE_IDLE=1`, and requires the core to have left by the end. The cli suite sets `XDG_DATA_HOME` too, since its `hull gc` looks in the store.
  - **The cli suite holds one client open** from its first run until it kills that core, so that what the cases between read of that core, the exited `box` and `kept`, is not lost to an idle second spent between two commands on a loaded host. Its case of the idle second has a client leave 0.7 s after its hello, five times, and requires the core to live at least 900 ms after.

**`hull exec [-t] ID [--] CMD [ARG…]` runs a command in a running machine**, on a channel of its own.

- **The frontend sends** `(exec (argv …) (ch n) (machine id) (tty …))`, then its stdin as `(data … (fd 0))` and `(eof …)`. The core answers `started`, then `data` on fds 1 and 2 and `exited`, and `hull exec` exits with the command's status.
- **`started` is what makes a later `error` harmless.** Before it, an error is the exec refused, and `hull exec` exits 125. After it, an error refuses a resize or a signal, and is printed while the command runs on.
- **The core passes the request itself as the engine's `exec_spec`,** being canonical already, and routes the stream's events by the stream pointer the engine reports them with.
- **Pointer reuse:** that pointer is freed by `stream_close()`, and a new stream could be given the same address. So the queue is drained to its end, including what handling it queues, before any client is served again: a closed stream's last events are always seen before another can exist.
- **What ends a command:** a frontend that goes takes its commands with it. A machine that exits takes all of its commands, and their frontends are told how each ended, 137 for the SIGKILL. The core first told them nothing: closing an exec silenced its exit, which is right when the frontend has gone and was wrong here, so a frontend whose machine was stopped under it waited for good. No case had run an exec across a stop; the sabotage runs below left such frontends behind, 46 minutes old when found.
- **stdin flows on credit, B.2's.** The core answers an exec's `started` with `(credit (bytes 65536) …)`, and the frontend sends no more stdin than it has been given; the core gives credit back as bytes reach the command, and refuses more than a frontend holds. So a command that does not read holds its frontend back, as a pipe would, and the core holds at most 64 KiB of each exec's input.
- **Each exec's stdin is written by a thread of its own**, from that 64 KiB ring. The core's one thread used to write it, through an engine `stream_write` that waited up to five seconds for the command to read: one command not reading stopped the core for five seconds a message, every client and machine with it, and its input was refused after. A command that sleeps for six seconds now gets all of it, and another exec beside one that is not reading starts at once.
- **Ending a write.** `stream_write` now blocks for as long as the command does not read, so closing an exec has to be able to cut a write short: the engines' writes do not block, their waits are polls of a tenth of a second, and `stream_eof` from another thread ends one. `hull_engine.h` says so. chroot closes the descriptor only once no write holds it, so no write is made to a number reused meanwhile. Making the stdin descriptor non-blocking makes a terminal's master so too, being one description, and the reader now passes over EAGAIN.
- **Once the command's input has closed,** the frontend is told once, and what it sends after is dropped with its credit given straight back, so it never waits on input nobody will read.
- **Output flows on credit the other way.** An exec's output starts with 64 KiB of credit, unasked, so a frontend that never gives any still sees the start of it. `hull exec` gives credit back for what it has written out, after each batch it reads from the socket. Credit for more than the core has sent is refused.
  - **Why:** the core used to read a command's output as fast as the command wrote it, and write it to a frontend with a socket send that waited up to five seconds. A frontend whose reader fell behind, as `hull exec m cmd | (sleep 7; wc -c)` does, stopped the core for five seconds, every client and machine with it, and was then dropped, and its command killed.
  - **How it holds the command back:** the engine's reader waits inside `on_data` while 64 KiB of the stream's output is uncredited. The command's pipe then fills and the command blocks writing, as it would writing to a slow pipe on the host. Nothing queues in the core beyond the window. `hull_engine.h` says `on_data` may wait, so an engine must call it holding nothing `stream_eof` or `stream_close` need. Closing an exec lets a waiting reader go first, and what it reads after is dropped.
  - **A stream's first output can arrive before `exec()` has returned the stream,** so the core cannot yet know whose it is. The loop makes one `exec()` at a time, so an unknown stream is the one under way, and its output is counted against that exec. The reader does not wait until `exec()` has returned and the exec is set up, so a failed exec cannot leave a reader stuck that the engine is joining.

In chroot:

- **A forked helper joins the init's user, mount, UTS and PID namespaces** through `/proc/<init>/ns/*`, which a single-threaded child can do and the threaded core cannot. It then chroots through `/proc/<init>/root` and forks the command, which is then a process of the machine, with its pid counted from 3.
- **The command gets the machine's environment, workdir and `no_new_privs`,** a session of its own, and `PR_SET_PDEATHSIG`, so `stream_close()` ends it by killing the helper.
- **The helper and the init close every descriptor they inherited and do not need.** Both are forked from the core and never exec, so they held its sockets, its lock and the far end of the command's stdin pipe, which then never reached EOF. `exec … < /dev/null` hung until that was found. A machine outliving its core would also have held `core.lock`, and no new core could have started. `close_range()` over the gaps between the kept descriptors allocates nothing, which matters in a child forked from a threaded process.
- **Reaping and signalling are ordered.** Both engines' readers wait with `WNOWAIT`, then reap under a lock and mark the process gone. `stop` and `stream_close` signal a pid only while it is unreaped, so a freed pid given to another process is never signalled.

**A terminal, with `-t`.** The command runs on a pseudo-terminal of its own: its stdin, stdout and stderr, and its controlling terminal, so all its output is fd 1. The frontend's own terminal is raw meanwhile, so Ctrl-C and Ctrl-D are bytes the command's terminal acts on, and it is put back however `hull exec` ends. `-t` refuses a stdin that is not a terminal.

- **The size is in the request:** `(tty (cols n) (rows n))`, or `(tty false)`. B.3 says `(tty true)` and sends the size after in a `winch`. Built that way first, a command that read its size at once read 80 by 24, before the `winch` arrived: the probe did, every time. A SIGWINCH sends `winch` afterwards.
- **A signal to `hull exec` is the command's:** HUP, INT, QUIT, TERM, USR1 and USR2 are each sent as `signal`, and the command decides. So a command that catches TERM carries on, and `hull exec` exits with whatever status it ends with. Before, they ended `hull exec` itself and, through the disconnect, killed the command. They are blocked except inside `ppoll()`, so none can land between the look at what has arrived and the wait. SIGKILL cannot be forwarded and is still a disconnect.
- **The core** maps the names onto the engine ABI's `resize` and `signal`, which were declared for this and had not been used.
- **An `eof` on a terminal ends nothing.** It closes a second copy of the terminal's master, so the terminal is not hung up, and the command reads on. Only a frontend whose own stdin ends sends one, and with `-t` that stdin is a terminal.

In chroot, **the terminal is the host's**, opened by the core and passed into the machine already open. Nothing is mounted for it, and the command makes it its controlling terminal with `TIOCSCTTY` after its `setsid()`. A signal goes to the helper, which forwards it to the command as the init forwards to the workload, so the only process that ever signals the command's pid is the one that reaps it. The helper holds its signals from its start, so one that arrives before the command exists is forwarded once it does.

In docker, `exec` is the daemon's exec API:

- **The command is created as the user,** with stdin, stdout and stderr attached, and started on a hijacked connection. stdin is written to that socket, and ended by shutting down its writing half. The exit code comes from `/exec/{id}/json`.
- **Ending it, and signalling it:** docker has no call for either. Since it runs as the user, `stream_close()` and `signal` signal its host pid, read from that inspection while it is still running, at the moment of signalling.
- **With a terminal** the exec is created and started with `Tty`, and its output is the bytes themselves rather than docker's frames. API 1.41 cannot size an exec when creating it, so it is resized once started, as docker's own client does. A command that reads its size at once may see the daemon's default first and the SIGWINCH after.

**`hull kill [-s SIGNAL] ID` signals a machine's workload**, as `docker kill` does, KILL unless `-s` names another. `-s` takes `TERM`, `SIGTERM` or `term`; which names it may be is the core's to say, the same seven an exec's `signal` takes.

- **It is `kill`, not `signal` on the machine's channel,** because a frontend's channels are its requests, and a machine is not one: B.3's channel 0 means the workload, here it means control.
- **`done` means sent, not acted on.** A TERM the workload catches leaves it running, and an exit reaches whoever follows the machine, as any exit does.
- **Each engine sends it to the machine's init, which forwards it.** chroot's init forwards the six it handles; docker's kill call signals the container's PID 1, which is docker's init, and that forwards it as well. A KILL ends the init and, with it, the machine, 137, on both.
- **A machine that has exited is refused**, by the core before the engine, which would otherwise say `done` for a signal it could not send: chroot's init is reaped and not signalled, and docker answers 409.

Open:

- **Whether a foreground `hull run` holds its machine back** for a reader that falls behind, as a pipe would, rather than losing what leaves the log. See *A machine's output is read from its log* above.
- **Moving onto the shared encrypted protocol**, when that is settled.
- **The case of a core told to go failed once, intermittently,** with the frontend's 125, "the core went away", where 143 was wanted. It passed eight times of eight alone and in every other suite run, about a dozen, and reading the shutdown path found no race. The case now keeps the frontend's output and `core.log`, so a failure says more. Unexplained. Not seen again in about 45 further runs of the cli suite on 2026-10-20 and 21, in the tree and in sabotage copies, each of which prints every FAIL; that is not evidence it has gone.
- **The qemu case of a TERM to `hull exec` failing with 143 was the stale-file race below, found 2026-10-20.** It failed once, in one of five suite runs on 2026-10-19, with 143 where 0 was wanted, and the output was `ready`, `signal=15`, `after`. That was taken to mean the command had caught the TERM, but the lines were the run before's: `xtrap.out` was not removed first, so the wait read the old `ready` at once and the TERM reached the background subshell before its redirect had truncated the file. The shell died with 143, and the file was left as the last run wrote it. The wait alone, with `sleep` standing in for hull, shows it with no machine at all. A stale file, then a TERM once it says `ready`, gave 0 of 50 at rest. With 30 busy loops on the 24 cores it gave 5 of 50, each a 143 with the old `after` in place. Removing the file first, as the suite now does, gave 0 of 50 under the same load.
- **The qemu case of a TERM through the agent to `hull run` failing with 137 was the agent powering off too soon, found 2026-10-20 and fixed.** It failed twice in suite runs at high load: the probe caught the TERM, and the output stopped at `signal=15`, once with `after` and once without. The agent sent `exited` and `bye` and then called `reboot` at once. A send returning only means the bytes are queued for the vsock device, and a power-off drops what vhost has not carried yet. The host then heard the connection close with no exit, and reported a machine that "died without its agent's word", as SIGKILL. That case alone passed 30 of 30 at a load of 5, and failed 1 of 30 with 30 busy loops on the 24 cores, with `core.log` saying exactly that, the console's last line being `reboot: Power down`. Pinning qemu's `vhost-*` thread to a CPU shared with two busy loops, at nice 19, made it 6 of 10, some runs losing `signal=15` and `after` too. Now the agent says `bye`, shuts its writing, and waits up to 3 seconds for the host's close before powering off; the host closes when it reads `bye`, so everything sent before it has been read. The same pinning gives 0 of 10, and a short machine still takes about 2.2 seconds either way. The suite cannot hold the race: without pinning a run fails about once in thirty under load, and a host that never closes only makes the agent wait the 3 seconds. The pinning loop is the evidence, and it is the experiment to rerun if this comes back.
  - **The timeout is ruled out:** the same case, run alone at the same load, took 2.5 s of its 60, three times of three.
  - **qemu's parent-death signal is ruled out by reading:** it fires when the thread that forked qemu exits, and `handle_run` runs on the core's main thread.
  - **The core's log was gone by then,** since each core truncates it on starting and the suite's cores come and go. The case now puts `core.log` in its failure. Unexplained.
- **The chroot case of a TERM through `hull kill` failed once, intermittently,** on 2026-10-20, with 124 where 0 was wanted, in a sabotage run of `make test` whose change could not reach it. So the TERM never ended the probe, which waits 20 seconds, the run's own `timeout`, while `hull kill` was answered. Its output was lost to how the harness printed it. **It came back once on 2026-10-21,** again 124 and again in a sabotage run whose change, the idle clock, cannot reach a core with a run attached, and its output was lost again, the script printing only the FAIL lines; the sabotage scripts now keep each run's whole output. The case alone did not fail in 130 runs, 50 at rest and 80 with 30 busy loops on the 24 cores, but alone it has a fresh core, where in the suite it shares one with every case before it. Its failure now prints the last of `core.log` too. Unexplained. Not seen again in the same 45 or so runs.
- **Nine waits for a background machine could read a stale file, found 2026-10-20 and fixed.** Each started a run writing `X.out` in the background, then polled `X.out` for the probe's `ready`. A file left by the run before still said `ready` until the background shell truncated it, so the wait could pass before the machine existed. The docker case of an import kept for a running machine failed this way, its keeper not yet started, and its rerun failed where the first run had passed. The docker suite's second `killd` wait read the first's file, which fits a sabotage run's "kill refused" and 124 in a case the break could not reach. Each file is now removed before its run starts. It also explains the `hull exec` 143 above, whose `signal=15` was the old file's; the `hull run` 137 was the power-off.
- **Chroot cases of `make check` failed under load averages of 130 to 190,** from other work on the machine. Two runs failed. In one, `hull kill` found no machine yet and a run hit its `timeout`; that one is unexplained. In the other, `ps` had lost `box`, which an earlier core had kept, and `logs box` found no such machine.
  - **The lost `box` was the core's idle time, shown 2026-10-21 and fixed.** The same shape came back twice in ten runs of `make test` at a load near 70, in the store's sabotage: `ps` without the exited `kept`, at the case after `stop kept` and at the case after a second core, and an `attach` that exited 2. Each fits a core leaving between two commands. The idle time was shorter than `HULL_CORE_IDLE` said, as measured above, and a frontend that dialled twice could find the core gone the second time. Five runs of the cli suite at rest had all passed. The idle clock and the frontend are fixed, and the suite holds a client across the cases that read an exited machine.

**Measured:**

- `test/cli_test.sh`'s exec cases: the machine's hostname, workdir and environment; a pid of the machine's own namespace, small and counted from 3; stdin passed, and ended by `/dev/null`; an exit status; a missing command; an unknown machine; a command killed with its frontend; a command ended with its stopped machine, its frontend told at once; and exec refused once the machine has exited. `test/docker_test.sh` has five: output on both streams, the status and the user's uid, stdin, a missing command, and an exec ended with its frontend.
- **With `-t`,** in cli_test.sh, where `script` gives the frontend a terminal: the terminal's size, its being the controlling terminal, stderr on it, a resize reaching the command as SIGWINCH, Ctrl-C and Ctrl-D as bytes the command's terminal acts on, and `-t` refused without a terminal. Signals: a TERM and a HUP caught by the command, which then exits 0, and a TERM it does not catch, 143. Two cases speak the protocol by hand, as a frontend not ours might: a terminal of 0 columns refused, and `started` followed by a refused resize that leaves the command running. docker_test.sh has three more: a terminal of the frontend's size, a resize, and a caught TERM.
- **Sabotage of the terminal and signals,** baseline first, each failing named cases:
  - the controlling terminal;
  - the size it is opened with;
  - resizing;
  - the helper forwarding a signal;
  - the frontend's raw mode;
  - the frontend forwarding a signal;
  - the core's size check;
  - its signal names (TERM sent as HUP);
  - `started` for an exec;
  - stderr on the terminal;
  - telling a frontend its command ended with the machine.

  Four needed a second try. The first stderr sabotage, `st->err = e[0]`, changed nothing, since `e[0]` is already -1 on a terminal; the child's end is what merges stderr. The TERM case first required only that the command went on, which a TERM delivered as HUP also does; it now requires `signal=15`. The two forwarding sabotages first hung the suite rather than failing it: a frontend that keeps a TERM to itself outlives the `timeout` around it. Each is now `timeout -k 2 8`. And the stopped-machine case first passed with the fault in: the hung frontend was SIGKILLed by that same `timeout`, and a SIGKILL is 137, the status the case expects. It now also requires the frontend to end within five seconds of the stop.
  - **In docker,** breaking the exec's `Tty`, the resize once it starts and the signal by pid each failed docker_test.sh, baseline first.
  - **One docker case was flaky**, and the race above is why: the resize docker makes once an exec starts is a SIGWINCH, and it once landed after the command's trap was set. The case now waits for the size it sets.
- **Sabotage of exec,** baseline first. Breaking each of these failed the test:
  - the helper closing what it inherits;
  - the init closing what it inherits;
  - the command dying with the helper;
  - joining the PID namespace;
  - writing stdin;
  - commands ending with their frontend;
  - an exec's exit status.

  The init's closing first passed. With the parent-death signal its stray descriptors die with the core, so nothing visible broke, and a case now reads the running init's `/proc/<pid>/fd` and requires no socket and no `core.lock` there. The first try at breaking the PID namespace was no sabotage at all: `setns` with a type of 0 joins whatever namespace the descriptor holds, so the namespace was still joined. And the pid check, first written `[3-9][0-9]*`, would have passed a host pid too; it now requires a small pid counted from 3.
- **`hull kill`,** in cli_test.sh: a TERM the probe catches, after which it says so and exits 0; the default KILL, 137, the probe having caught nothing; and refusals of an exited machine, an unknown one, STOP, and a name too long to be one. docker_test.sh has the first two, with a shell that traps TERM.
- **Sabotage of kill,** baseline first. Breaking each of these failed a named case: the core sending it to the engine, its check of the name, the frontend's upper-casing and its dropping of `SIG`, the default of KILL, and in docker the signal given to the kill call (sent as KILL, the caught TERM became 137).
  - **Removing the core's `exited` test passed, and cannot fail:** `machine_exited()` sets `exited` and clears the engine's handle together, on the core's one thread, so the `handle == NULL` beside it refuses the same machines. It is kept for reading alike with `exec`'s.
  - **The STOP case is caught by its message, not its status:** without the name check, an exited machine is refused all the same, as not running, and exits 1.
- **stdin's flow,** in cli_test.sh, on a machine of its own: 1.1 MB through `/probe count`, its length and FNV-1a hash against the host's; 300 kB to a command that sleeps six seconds first; an exec beside one that is not reading, answered inside two seconds; a frontend killed with input pending, its command gone and the core answering a second later; speaking the protocol by hand, the first credit and 65537 bytes refused, and 300 kB in messages of 10000 bytes, so that some straddle the end of the ring; and a frontend held back three seconds using well under one second of CPU. docker_test.sh has the first two, by `md5sum` and `wc -c`.
- **Sabotage of the flow,** baseline first. Breaking each of these failed a named case: credit given back, the core's credit check, `stream_eof` ending a write, a write refused after five seconds as before, the descriptor not blocking, and the ring's head wrapping. Three needed a second try:
  - **The frontend's wait for credit** first passed: without it the frontend spins on a stdin it cannot read, which changes no output. The CPU case catches it.
  - **The ring's tail wrapping** first passed. The frontend reads 16 KiB at a time, which divides the ring, so no message straddled its end; the 10000-byte messages do. Even then it passed once, because the test's bytes repeated every 256, so the stale bytes read past the wrap were the right ones. They are random now, and the case says `mismatch`. With the wrap broken the core writes past its ring, and a later case also failed when the corrupted core lost a machine.
  - **docker's write refused after five seconds** first passed: the daemon's buffers took the 300 kB without the core's write ever waiting. It is 3 MB now.
- **Output's flow,** in cli_test.sh on the same machine:
  - 3 MB from `/probe spew`, counted by the same probe on the host;
  - 20 MB to a reader seven seconds behind, arriving whole;
  - meanwhile the command still running two seconds in, so held rather than read into the core, and another exec answered inside two seconds;
  - speaking the protocol by hand, at most 64 KiB with no credit given, as much again for crediting it, and credit for more than was sent refused.

  docker_test.sh sends `seq 1 3000000` to a reader seven seconds behind, compares its `md5sum`, and requires the `seq` still running three seconds in.
- **Before and after, measured against docker:** `hull exec m seq 1 3000000 | (sleep 7; wc -c)`, with a `hull ps` two seconds in.

  | Core | `ps` took | Bytes arrived (of 22,888,896) | The frontend said |
  |---|---|---|---|
  | Before | 3.4 s | 147,707 | "the core went away" |
  | After | 3 ms | 22,888,896 | nothing |
- **Sabotage of output's flow,** baseline first. Breaking each of these failed named cases: the reader's wait, counting what it read, taking credit off, waking the reader on credit, refusing over-credit, the frontend giving credit, and the schema's case for a frontend's `credit`. Not letting the reader go on close hung the core at the first frontend killed mid-output, and the suite ran out its time.
  - **Adopting a stream's output before `exec()` returns passed, and nothing here reaches it.** chroot starts its reader as exec's last step. docker with `-t` resizes after starting, but fifteen `-t` execs of 300 kB, against a core with adoption removed, never once saw output before the return. It is kept because the engine ABI does not promise that order. Without it, such output is credited by the frontend but was never counted, so credit for it can be refused, and the window then shrinks for good.
- **A machine's output to followers,** in cli_test.sh: a machine writes 20 MB while `hull logs -f` feeds a reader seven seconds behind. The core answers `ps` within two seconds meanwhile. The follower is told it lost some, and what it read and what it lost are 20 MB exactly. What it read is under 2 MiB: the log's last MiB, and what the sockets held when it fell behind. Speaking the protocol by hand, twelve replays of the full log, all asked for before any is read, all arrive. A frontend that sends `list` and never reads an answer is dropped, and the core answers `ps` after. And a core sent TERM stops a running machine first, so its frontend exits 143 rather than 125.
- **Before and after, a follower behind:** the same 20 MB machine, `hull logs -f` into `(sleep 7; wc -c)`, a `hull ps` three seconds in.

  | Core | `ps` took | The follower read | It was told |
  |---|---|---|---|
  | Before | 4.7 s | 249,856 bytes | "the core went away" |
  | After | 7 ms | 1,383,680 bytes | 18,616,320 bytes lost |
- **Sabotage of the frontend queue and the followers,** baseline first. Breaking each of these failed named cases:
  - the socket being non-blocking;
  - telling a lapped follower;
  - the log's start advancing;
  - the queue's limit;
  - asking `poll()` for POLLOUT;
  - the frontend printing `lost`;
  - the schema's `lost`.

  Breaking the exit sent once the log is read hung its frontend, and the suite ran out its time. Three needed more:
  - **Feeding only while less than 64 KiB waits** first passed. A follower that keeps up is only ever fed what arrived since the last turn, so the threshold matters only to one that starts with the log behind it, as `logs` does. The twelve replays catch it: queued whole they come to 12 MiB, and the frontend is dropped.
  - **The two guards on a failed start,** forgetting the follower and telling only a slot in use, each passed alone, since either one fixes the bug. With both removed, the cases that found the bug fail.
  - **The two flushes when the core goes,** each turn while machines stop and once at the end, each passed alone too, since either one delivers the exit. With both removed, the TERM case fails with 125.
  - **The follower case first parsed one `lost` line.** One run, in a sabotage copy, was lapped twice, which can happen to the baseline, so it now sums them.
- **A run whose start fails left its follower in the freed slot.** That was harmless while a run cleared the slot first. Once a slot taken over tells the old machine's followers it has gone, the stale follower was told instead. It pointed at a frontend since gone, so the next frontend in that client slot heard an `exited` before its own `started`, and exited 0 with no output. `pid.conf`'s case caught it, run after `volhost.conf`'s, whose start fails. A failed start now forgets its follower, and only a slot in use has followers to tell.
- `test/wire_test.c` runs 19 cases: each direction's messages let through and the other's refused, non-canonical bodies refused, and the numbers read minimal decimal. Disabling the validator fails four of them.
- `test/cli_test.sh` runs 216 cases, the secrets' among them (above). Among them: `hull proxy-ssh` refused for a chroot machine, which has no ports of its own, and for a name that is only `.hull`; `hull gc` with no docker daemon to reach, which its `DOCKER_HOST` sees to, saying so and exiting 0; `hull ssh`'s login shell, its terminal and the frontend's TERM, and its refusal without a terminal; a machine's address in `ps`, worked out by the test itself, an id that comes to the same address refused, and a plan that gives one refused; `stats` on a chroot machine, its own CPU, its reaped child's, its memory and its writes, and an ended exec's CPU and writes; `ps`, `logs` after an exit, a duplicate id refused, a kept machine surviving its frontend's SIGKILL where an unkept one is stopped, `attach` ending with a stopped machine's 143, a second core giving way and the first still answering, a machine dying with a SIGKILLed core and none kept for the next, the core leaving when idle, and the next core's reading of what a core kept: junk, a misnamed file, an engine that cannot adopt and a half-written file gone, and a docker machine with no daemon to reach left, each said in `core.log`.
- **Sabotage,** in a scratch copy, with an unmodified baseline first that had to pass. Breaking each of these failed the test: honouring `keep`, stopping on disconnect, replaying logs, `attach` following, one id at a time, the idle exit, the lock, and the parent-death signal.
  - The last two first passed. The lock case only checked that a second core exited 0. Without the lock, the second core takes the socket over, then leaves and removes it, stranding the first. The case now requires the first to still answer.
  - The parent-death case's wait could give up silently, so it now fails when the kept machine never ran. Without the signal, the probe was measured alive a second after its core died, and gone once the shell reaped the client, for no cause found then. So the case looks for the probe before the client is reaped, where the two behaviours differ.
  - **That later death went with the descriptors the init inherited**, found while writing `exec` (below). With them closed, the same experiment leaves the probe alive after the client is reaped as well. The exact path from the init's stray descriptors to the probe's death was not traced.

**`hull stats ID` says what the host spends on a running machine**, as its engine measures it, from the host and never from inside:
- **The figures:** CPU time in microseconds, user and system; bytes read from and written to storage, `/proc`'s `read_bytes` and `write_bytes`; and memory resident by Pss, so that pages two processes share are counted once. The engine's `stats()` gives them as `(stats (cpu n) (io n) (memory n))`, which the core checks and answers with on the request's channel. `hull stats` prints them, one to a line.
- **What is summed:** the processes alive now, each counted with the children it has reaped, since the kernel adds a reaped child's CPU and io to its parent's. That counts every process that ever ran once, so long as whatever reaped it is counted too. `engine/usage.c` reads them.
- **chroot:** every process in the machine's pid namespace, found by `/proc/<pid>/ns/pid`. An exec's helper is outside it, so what a helper and its command spent is added from `wait4()`'s rusage once the helper is reaped, its blocks as 512 bytes, which is how `/proc` counts io.
- **qemu:** the qemu process, which holds the guest's memory and runs its CPUs, and its passt, found as the process holding the far end of the engine's socket pair as its fd 3 (§4.11). passt is not the engine's child, so its pid is used only while `/proc/<pid>/comm` still names a passt. So a vm's figures are its whole cost to the host, the guest kernel's included, rather than what its processes use inside. B.3's `(sub … (what stats))` would give the guest's own view, and is not written.
- **docker,** since 2026-10-20: CPU and storage io from docker's cgroup figures, the one-shot `stats` call's `cpu_stats` and `io_service_bytes_recursive`, which count every process the container ran. An exec is reaped by containerd's shim, outside the container, so `/proc` would lose what an ended one spent, as chroot's helper would without its `wait4()`. Memory is still Pss summed from `/proc` over the container's pid namespace, from its `State.Pid`, since a cgroup's usage counts the page cache. The container's processes are the user's (§4.9), so `/proc` is theirs to read.
  - **A cgroup counts io when it reaches storage,** not when it is written, as `/proc`'s `write_bytes` does, and that can be half a minute later. So the probe's `fill` now syncs what it writes, and the figure is the same either way: written to storage.
  - **Measured** in `test/docker_test.sh`, as in cli's: the probe's 0.6 s, 32 MiB and 4 MiB, then 0.4 s and 4 MiB more from an ended exec, and a container that has ended refused.
  - **Sabotage,** baseline first: breaking each of these failed a named case: CPU from `/proc`, io from `/proc`, which both lose the ended exec, memory left at nothing, and the probe not syncing, which left docker's io short of the 4 MiB and so bore out the line above. Not caught: the io read running on into the next array, of operations rather than bytes, which adds a few dozen to a figure the case bounds loosely.
- **A rate, `hull stats --over S ID`,** since 2026-10-20: two readings S seconds apart, 1 to 3600, timed on the monotonic clock. CPU is a percentage of one CPU, so a machine using four whole CPUs is 400 %, as `top` counts; io is bytes a second; memory is the second reading, being what is resident and not a flow. It is the frontend's arithmetic over two `stats`, on every engine that measures.
  - **Measured** in `test/cli_test.sh`: the probe waiting, under 5 %; an exec spinning, 30 to 110 %, the lower bound loose so that a loaded host passes; and an S of 0 or `2s` refused. Sabotage, baseline first: not dividing by the time, and taking the second reading whole rather than the difference, each fail a named case.
- **Measured** in `test/cli_test.sh` on chroot and `test/qemu_test.sh` on vm (§4.11): CPU a probe uses by its own clock, so a loaded host makes it slower and not smaller, memory it holds, and bytes it writes.
- **Sabotage,** baseline first, in scratch copies. On chroot, breaking each of these failed a named case: a reaped child's CPU, `write_bytes`, Pss in bytes, the whole namespace rather than the init, an ended exec's CPU and its io, and the core's keys. On vm: qemu counted at all, `write_bytes` and Pss. A reaped child's CPU passes on vm and cannot fail there: the guest's children are reaped inside qemu, and qemu reaps none of its own.
- **Untested:** passt's share, which is too small beside qemu's to tell apart, and the guards against a freed pid, which a test cannot make happen on purpose.

**Each machine has §8.1's stable address, and its ports are bound there** rather than on 127.0.0.1.
- **The address** is 127.107.x.y, from the machine's id alone. FNV-1a of the id's bytes, modulo 256 × 254, is x × 254 + y − 1, so y runs from 1 to 254. An id has the same address on every start and every engine, and changing the formula moves every machine.
- **The core gives it.** It adds `(address "127.107.x.y")` to the plan before `create()`, first, since it sorts before every other entry. A plan that names one already is refused: "a plan's address is the core's to give". `hull ps` shows it.
- **Two ids can come to one address,** one pair in 65,024. The second is refused while the first runs, as in "its address, 127.107.178.190, is machine kept's", rather than given another, which would not be stable.
- **The engines bind there:** qemu its listeners, docker its `HostIp`. A plan with ports and no address is refused by both. chroot shares the host's network and declares `ports` rejected, so it binds nothing.
- **No root:** Linux routes all of 127.0.0.0/8 to `lo`, so a user can bind any address in it with nothing configured.
- **What it buys:** two machines can publish the same host port. Measured on qemu: a port held on 127.0.0.1 leaves a machine publishing it free to start, and the same port held on the machine's own address refuses the start.
- **Why the plan carries it, and not a forwarder of the core's.** `hull_engine.h`'s draft had each engine hand the core a host endpoint, through `port_endpoint()`, and the core forward from the address to it. That copies every byte once more, through the core, to reach an address the engine can bind itself. The core still owns the address; it hands it over in the plan. `port_endpoint` stays unwritten.
- **Sabotage,** baseline first, in scratch copies. Breaking each of these failed a named case: the formula's `+ 1`, the collision check, refusing a given address, `ps` reading the address, qemu's listeners bound on 127.0.0.1 again, and docker's `HostIp` set back to 127.0.0.1.
- **`web.hull`, §8.1's name for the address, is an NSS module** (§8.1).

**`hull ssh ID` is §8.1's fallback: a login shell in the machine, on a terminal,** as `hull exec -t ID /bin/sh -l` would give, on every engine.
- **TERM:** an exec carries the machine's environment and none of the frontend's, so a shell would start without one, and `less` or `vi` would not know the terminal. The frontend's TERM goes in as the shell's argument, `/bin/sh -c 'TERM="$1" exec /bin/sh -l' sh "$TERM"`, so it needs nothing in the machine but `/bin/sh` and is never read as shell text. With no TERM, it is `/bin/sh -l` alone.
- **It needs a terminal,** and says so without one.
- **Measured:** in `test/cli_test.sh` and `test/qemu_test.sh`, the probe stands in as the machine's `/bin/sh` and says what it was given. In `test/docker_test.sh`, alpine's own shell is typed at through `script` and reports its TERM and its terminal.
- **Sabotage,** baseline first, in scratch copies. Breaking each of these failed a named case: passing TERM, the terminal, the login, and the refusal without a terminal. Passing TERM failed on docker's real shell as well.
- **Not yet, and why:** §8.1's first half, proxying to the machine's sshd when it runs one. What that buys is the machine's own SSH identity, its host key, and nothing provisions or pins host keys until Phase 2 (§12). Until then, a machine that publishes port 22 is reached with `ssh -p PORT` on its address.

### 4.11 The qemu engine, and the guest agent

**The qemu engine runs vm and emu machines as the user, on the host's own kernel: `engine/qemu.c`.** Nothing it does asks for root.

- **What a user needs:** `/dev/kvm` for vm and `/dev/vhost-vsock` for both, which Debian opens to every user, a readable `/boot/vmlinuz` of the running kernel, and `passt` for the network.
- **The machine:** a q35 machine with the host's kernel and an initramfs the engine writes for it into `$XDG_RUNTIME_DIR/hull/qemu-<pid>-<n>/`. The initramfs holds the guest agent as `/init` and the modules the agent loads to reach its directory and the host.
- **The modules:** the kernel's 9p, vsock, virtio_net and overlay modules, with what `modules.dep` says they need, loaded in modprobe's order. They are xz-compressed, which the kernel decompresses itself, so the agent hands them over as they are.
- **The directory, and each volume,** is a share of qemu's own 9p server, written in place as the user's.
- **Under `(mutable overlay)`** the directory's share is read-only, to qemu and to the guest, beneath an overlayfs the agent mounts. Its upper layer is a tmpfs in the guest.
- **Classes:** vm is `-accel kvm -cpu host`. emu is `-accel tcg -cpu max`, on the host's arch only, since a foreign arch needs a foreign kernel.

**9p, not virtiofs, which §3.2, §5.2 and §6 name.** virtiofs needs virtiofsd, a daemon of its own, which this host does not have. qemu's 9p runs inside qemu as the user and needs nothing else. Its fidelity is weaker: no host-side inotify, which is declared rejected as virtiofs's was, and slower metadata. This is a deviation from the design, not a change to it. Moving to virtiofs where virtiofsd is installed, and whether to require it, is the holder's decision.

**The kernel is the host's, standing in for Appendix F's bundled one.** So `vm.kernel.bundled` and `vm.profile.micro`, the defaults every plan writes in, are declared emulated, and every run says so on stderr. The cost is measured: a machine boots and runs `/probe` in about 2.2 s, where §5.2's target is 100 to 300 ms. emu under TCG takes about 17 s.

**A dir source as an overlay is an overlayfs in the guest, its upper layer a tmpfs there.** §3.7 has the directory stay pristine and the writes go to an upper layer. Here the agent mounts the 9p share read-only at `/hull/lower`, a tmpfs at `/hull/upper`, and the overlay of the two as the machine's root.
- **The directory is read-only twice:** qemu's share is `readonly=on`, and the agent mounts it read-only. Each alone holds, as for a read-only root.
- **`/` is what it is in place.** The tmpfs's top directory takes the directory's owner, group and mode, setgid included, so a file the machine's root writes in `/` takes the directory's group when `/` has the bit. Docker cannot do this: its `/` is root's (§4.9).
- **A removal is a whiteout in the upper layer.** The file is gone inside and still on the host.
- **With a read-only root** there is nothing for an overlay to hold, so the root is the share itself, read-only.
- **The cost: writes are held in the machine's memory, and go with it,** as a docker container's writes go with the container. The tmpfs is the kernel's default, half the guest's memory. A machine of 256M, whose kernel reports 217,588 kB, took 106 MiB of a 200 MiB write, and the rest failed as a full disk. So the declaration is emulated, and says so.
- **Why not §5.2's upper layer over a second share, on the host.** Tried with the upper layer on a 9p volume: the mount fails with EACCES. overlayfs makes its `work/work` directory with mode 000, and qemu's 9p server runs as the user, so the host enforces that mode against it. A whiteout would meet the same wall: it is a 0:0 character device, which the user cannot make. `security_model=mapped-xattr` keeps modes and devices in xattrs and might pass both, at the price of an upper layer whose files on the host are not what they are in the machine. Not tried. Writes kept on the host, past the machine's end, are what `hull commit` will want (§3.7).

**A machine's network out is passt's, as the user; its ports in are the engine's.** §8 names user-mode networking per VM. The engine starts a passt for each machine on a socket pair, and the other end is qemu's virtio-net device.
- **The addresses are fixed,** in both the engine and the agent: the guest is 10.0.2.15/24, its gateway 10.0.2.2, and its DNS 10.0.2.3, which passt forwards to the host's own resolver. These are slirp's well-known addresses. The agent brings up `lo` and `eth0` and adds the route itself, by ioctl, since there is no DHCP client in the guest.
- **resolv.conf:** the machine's `/etc/resolv.conf` has the agent's file, which names 10.0.2.3, bound over it, as docker writes its own. It is read-only when the root is. The bind is only over a regular file: a link would be followed in the agent's root, outside the machine.
- **The host's loopback is out of reach.** By default passt makes the gateway address the host's 127.0.0.1, so a guest could reach whatever listens there for the user alone. `--no-map-gw` turns that off. A vm-class machine is the boundary §8 and Appendix D put the risky machine behind, and an open path to the host's loopback would undo it.
- **Why not qemu's own `-netdev user`:** slirp has the same mapping and no way to turn it off.
- **Why passt does not forward the ports.** Tried by hand with `--no-map-gw`, a connection to a port passt forwarded from 127.0.0.1 reached passt and never the guest. passt has no guest-side address to give a connection from the host's loopback. The address that would, `--map-host-loopback`, works both ways: passt(1) says packets from the guest to it go to the host's loopback, and it has no inbound-only form.
- **No pid file, fixed 2026-10-21.** The engine passed `--pid` with a file in the machine's directory, under `$XDG_RUNTIME_DIR`. Debian's AppArmor profile for passt lets it write only under `/tmp` and the home directory, so with the runtime directory where it always is, `/run/user/UID`, passt refused the pid file, and no qemu machine started: `Couldn't open PID file …: Permission denied`. The suites never saw it, because their runtime directories are under `$BUILD_DIR`, in the home directory. The pid was only for `hull stats`, so the engine now finds the daemon as the process whose fd 3 is the far end of its socket pair, by inode in `/proc`. Found while giving qemu secrets, whose case needs a tmpfs runtime directory and so failed the same way under `/dev/shm`; that case now covers it.
- **A kept machine outlives its core, and the next core takes it back** (§4.10's adoption), written 2026-10-21.
  - **qemu** is spawned without the parent-death signal under `(on_disconnect keep)`, so it outlives a core killed or crashed; a machine not kept still dies with it. `persist()` gives qemu's pid, the vsock context id, the machine's directory, the agent's token, passt's pid, the address and the ports, as text, a line each.
  - **The agent** under keep leaves the workload running when its host goes, ends the execs and forwarded connections whose frontends went with it, and listens again. What the workload writes meanwhile is held, its last 256 KiB, for the host that resumes; a workload that ends meanwhile is reported to that host, and the machine powers off once it has been. So a kept VM whose workload has ended waits, holding its memory, until a core comes, as a stopped docker container waits.
  - **A token, since any local user can reach a guest's vsock port.** The engine gives the agent 32 random bytes with `start`; a host after the first is answered `hello` and nothing else until it says `resume` with them, compared in constant time, and is dropped otherwise, the agent listening on. A kept VM can wait for its next core for as long as the user stays away, which is a much longer window than the first connection's at boot. That window, before the engine's first connect, is as it was: B.2's claim that vsock needs no authentication holds for it only as far as the race does.
  - **Taking it back:** the pid must still be a qemu whose command line names the machine's directory, checked through a pidfd opened first, which then stands for it, as it is init's child now. The engine dials the agent, says `resume` with the token, hears `started` and the held output, binds the ports again on the address, and runs its reader and forwarder as at a start. The old core's log is gone with it, so `hull logs` has only what was held. The sweep of dead cores' directories passes over the directories of machines taken back, whose names would otherwise fall to it.
  - **Failure modes:** a pid that is not that qemu is gone, and forgotten; an agent that does not answer or will not resume is left, for a later core, not stopped, as it is not this core's.
  - **An exit is told only to a believed host, found by its case.** The agent's loop settles an ended workload on every turn, so the first version told `exited` to whichever host connected next, before its hello was answered and whether or not it had the token, then powered off. The core taking the machine back heard `exited` where it expected `hello`, and the exit was lost; an unbelieved process would have read it. It now waits for `resume`.
  - **Measured** in `test/qemu_test.sh`: the file written, qemu alive after its core's SIGKILL, a host without the token dropped although it asked for an exec and told nothing but hello, the machine taken back by the core `hull ps` starts, the port forwarded again, an exec, another machine booting meanwhile without sweeping its directory, what was written while no core held it, the exit through `attach`, and the file and qemu gone after; a workload that ended while no core held it, its exit 7 heard by the next core and the VM then gone; and a kept file whose pid is another process's, forgotten, that process unharmed. A machine not kept still dies with its core.
  - **Sabotage,** baseline first. Breaking each of these failed a named case: the parent-death signal kept under keep, any host believed, the output not held, the ports not bound again, the exit not kept for the next host, the exit told to a host not believed, the directory swept, and the pid not checked. The held output and the kept exit first survived: the workload wrote `during` only after the new core had it, and no case had a workload end with no core. The test now waits four seconds after the kill, and the second case was added.
- **Every suite run once with its runtime directory at `/run/user/UID`, 2026-10-21,** in a scratch copy, to look for more of the pid file's kind: a fault that hides because the suites' runtime directories are under `$BUILD_DIR`. `make test`, `check-qemu`, `check-docker` and `check-flip` all passed, but for the cli case that wants the secrets refused in a runtime directory not in memory, which `/run/user` is. That case now says so and is not counted when the suite's runtime directory is a tmpfs, as it would be with `$BUILD_DIR` under a tmpfs `/tmp`, rather than failing there.
- **Its life:** passt's parent exits once passt is set up, so the engine knows whether it started and, if not, why, from `passt.log`. The daemon it leaves ends when qemu's end of the pair closes (`--one-off`), so it goes with qemu, and qemu goes with the core.
- **The cost:** a LAN host at 10.0.2.15 is out of the machine's reach, since that is the machine's own address. This host's LAN is 10.0.0.0/16, which holds it. A host without passt can run no qemu machine. That is a hard requirement, as `/dev/vhost-vsock` is, rather than a machine silently started with no network.
- **Measured:** boot time is unchanged, about 2.2 s.

**The guest agent is `agent/agent.c`,** built static as `hull-agent`, beside `hull`, where the engine looks for it. It is PID 1 and follows B.1:
- It mounts `/dev`, `/proc` and `/sys`, loads the modules `/hull/modules` lists, and listens on vsock port 1 for the one host connection.
- On `spec` it applies the machine: the hostname, the directory's share mounted at `/machine` and read-only if asked, each volume's share at its guest path, and `/proc`, `/dev` and `/sys` inside wherever the directory has a place for them. Then it moves `/machine` onto `/` and chroots, as switch_root does.
- On `start` it forks the workload, in a session of its own, with the spec's environment, workdir and `no_new_privs`. A command that cannot be run exits 127, as on chroot.
- The workload's stdout and stderr come back as `data` on channel 0, which B.3 already uses for the workload in `signal`.
- **Its end:** once the workload is reaped and its pipes read, the agent says `exited`, exactly as the workload ended, kills whatever is left, says `bye`, and powers off once the host has closed the connection, which the host does on reading `bye`, or after 3 seconds (§4.10). A pipe an orphan still holds is read for what it has, and not waited on.
- **`stop`** is SIGTERM, then SIGKILL at its timeout. A host that goes away is `(on_disconnect stop)` with nobody to tell. `signal` forwards the seven names the frontend protocol does, spelled with or without `SIG`.

**The host's side,** in the engine:
- `start` picks a random vsock context id and boots qemu. A context id is the host's, shared by every VM, so if qemu refuses it as taken another is tried, four times in all.
- It connects to the agent while qemu lives, for up to 60 s, then runs hello, `spec` with the plan's own `(machine …)`, `ready`, `start`, `started`.
- A thread then reads what the agent sends until the connection closes, waits for qemu, and reports the exit as the agent gave it. A machine whose agent never said goes as SIGKILL, with the last line of its console, which is kept in a file, logged as why.
- `stop` asks the agent and kills qemu if it has not powered off within the timeout and five seconds more. `hull kill` is the agent's `signal`.
- **qemu has `PR_SET_PDEATHSIG`,** so it dies with the core, as a chroot machine does, and inherits none of the core's descriptors.
- **A core killed outright leaves its machines' directories.** The next core's first machine removes them: one core runs per user, holding `core.lock`, so a directory named for a pid that is not running was a dead core's. Its files are removed by name.

**What it declares** is what that does: a dir source in place or as an overlay, `command`, `workdir`, `env`, `hostname`, `volumes`, `cpus`, `memory`, `readonly_root`, `no_new_privs`, both `on_disconnect` values and `emu.cores_scale`, with the two emulated defaults above. `ports` is declared emulated, as §6 has it: tcp and udp, forwarded from the machine's address (§4.10).
- **What it no longer declares:** image refs, ssh, media, device passthrough, the full profile and the other kernels. The draft declared all of these from §5 and §6 before any of it was written, the way docker's did.
- **The cost:** §4's example, which has an image source, a port and `(ssh true)`, is refused on vm and emu for the image and ssh. A foreign arch is refused on emu.
- **Not written yet:** the guest's own view of its stats (§4.10). A machine's output reaches the core, and so `hull logs`, as a chroot machine's does.

**`hull exec` on a qemu machine is a channel of the agent's,** which the engine allocates per exec.
- **The command** gets the machine's environment, workdir and `no_new_privs`, on pipes or on a pseudo-terminal of the machine's own. The agent mounts `devpts` for that. A terminal is the command's controlling terminal, of the size the request gives.
- **B.3's `exec` carries the size now:** `(tty (cols n) (rows n))` or `(tty false)`. B.3 had `(tty true)` with the size sent after in a `winch`, which the frontend protocol had already found reads 80 by 24 first (§4.10). The schema was changed to the frontend's spelling, and `(tty true)` is a refused vector.
- **Output flows on B.2's credit,** 64 KiB to start. The engine's reader queues what arrives and never waits. Each stream's own thread hands it to `on_data`, which may wait while the frontend is behind, and gives the agent credit back once it has. So a frontend that falls behind holds back its own command and nothing else: the agent does not read a channel out of credit, and the command blocks writing, as on a pipe. A stream queues at most the agent's 64 KiB.
- **stdin flows on the agent's credit the other way.** The agent gives the host 64 KiB, writes from a ring as the command's pipe takes it, and gives credit back as it does. It says `eof` once the command's input has closed. `stream_write` waits for credit, and `stream_eof` ends the wait. An `eof` on a terminal ends nothing, as in chroot.
- **The exit is said once the output has been read,** as for the workload, so nothing the command wrote is lost behind it.
- **`stream_close`** has the agent SIGKILL the command and waits for its exit. Every exec the agent has not reported when the connection ends is reported as SIGKILL, 137: a machine that stops takes its commands, and their frontends hear it at once.
- **Signals and resizes** are the agent's `signal` and `winch` on the exec's channel.

**A machine's ports are forwarded by the engine, over vsock, as connections of the agent's.** passt cannot forward in without opening the host's loopback to the guest (above), and vsock needs no address on either side.
- **The host's side:** the engine binds each port on the machine's address (§4.10) before the machine boots, so a port that is taken refuses the start with its reason: `port 8080 on 127.107.178.190: Address already in use`. A thread per machine accepts. Each connection gets a thread and a channel of the agent's, opened with `(connect (ch n) (port n))`, which B.3 gains for it. The listeners close when the machine ends.
- **The agent's side:** it connects to the machine's own address, 10.0.2.15, on the guest port, as docker's proxy connects to a container's address. So a server listening only on the guest's loopback is not reached, as on docker. The answer is `started` or `error`, and a refused connect reaches the client as a close.
- **Bytes flow as an exec's do,** on B.2's credit both ways, with the socket in place of `on_data` and stdin. A client that does not read holds back the machine's writer, and nothing else.
- **Its ends:** the client shutting its writing is the agent's `eof`, and the agent shuts the machine's side for writing, so a client may half-close. The machine's end, read by the agent, is `exited` with 0, and the host then shuts the client's socket. A client that goes, or resets, has the host send `signal` on the channel, and the agent then resets the machine's side. A machine that ends shuts every client's socket, so a client that does not read cannot hold `destroy`.
- **The reset was found by testing.** The agent first ended an abandoned connection with `shutdown(SHUT_RDWR)`, and a server in the machine writing to it blocked for good. Its socket sat in CLOSE_WAIT with 2.6 MB queued, and the agent's end in FIN_WAIT2. After SHUT_RD, Linux goes on taking what arrives, and the close is then a FIN, so the writer is never told. A linger of zero then a close is a reset, and the writer gets EPIPE.
- **A udp port is a flow per client,** added 2026-10-20. The engine binds a udp socket on the machine's address, and the first datagram from a client's address and port opens a channel with `(connect (ch n) (port n) (proto udp))`. The agent answers it with a udp socket connected to 10.0.2.15 on the guest port, so the server in the machine sees each client as a sender of its own. Each `data` is one datagram, either way, an empty one included, up to 65,536 bytes, in a frame that holds a MiB.
- **Neither way waits for credit:** a datagram the far end cannot take now is dropped, as udp's are. The reader sends the machine's reply to the client with one `sendto` that does not block, so a flood from the machine queues nothing in the host. The agent's socket hears an earlier datagram's ICMP refusal as ECONNREFUSED, which ends nothing, since the client may send again once the server listens.
- **A flow ends after 90 seconds unused,** as docker's proxy keeps one, swept each second while any is open. It also ends if the agent refuses it, and when the machine ends. The host then sends `signal` on its channel, and the agent closes the socket. Flows share the machine's 64 channels with execs and connections, and a datagram that finds none left is dropped and logged.

**Measured,** in `test/qemu_test.sh`, which `make check-qemu` runs, since `make check` needs no KVM. Its 110 cases:
- **One boot,** and every line it prints checked: output on both streams kept apart, the exit status, env, workdir, hostname, uid 0, `no_new_privs`, a session of its own, two CPUs, 256 MiB, and the KVM clock.
- **How a machine ends:** death by a signal, 143, and a missing command, 127 with the reason.
- **Writes:** a write landing in the directory as the user's file, a read-only root, and volumes read-write and read-only.
- **An overlay:** `/` with the directory's owner, group and mode; a write and a removal inside, neither reaching the directory; a 200 MiB write stopping between 90 and 127 MiB; and a read-only overlay refusing a write.
- **A running machine:** listed by `ps`, a TERM through `hull kill` caught by the workload, SIGTERM to `hull run` stopping the machine with 143, and qemu dying with a SIGKILLed core.
- **exec,** in a machine of its own: the machine's environment and hostname, a status, a missing command 127, stdin ended by `/dev/null`, 588,895 bytes of stdin by length and FNV-1a hash, 300 kB to a command that sleeps six seconds first, and 3 MB to one that sleeps three.
- **exec's output:** 3 MB counted against the same probe on the host, and 20 MB to a reader seven seconds behind arriving whole. Meanwhile the command is held, the machine counting one process more two seconds in, and another exec answers within three seconds.
- **stats:** an exec's 300 ms of CPU, 300 more in a child it reaps, and 4 MiB written through 9p, each showing in qemu's figures; at least 32 MiB resident; and a stopped machine refused.
- **exec's terminal and its ends:** a terminal of 100 by 30, spoken by hand, its controlling terminal, resized to 120 by 40; `hull ssh` giving the machine's `/bin/sh` a terminal and the frontend's TERM; a TERM to `hull exec` caught by the command; a frontend SIGKILLed taking its command with it, the machine counting as many processes after; and an exec in a machine that stops ending 137 at once.
- **The network:** the agent's resolv.conf, read-only under a read-only root. `lo` is up: a dial to a port nothing holds is refused rather than unreachable. The host's resolver answers through 10.0.2.3, and a listener on the host's own address is reached. A listener on the host's 127.0.0.1 is not reached through the gateway, checked from both sides. Two host listeners on ports of their own say who came.
- **Ports:** six, each from a free port to the probe. These cases run in order:
  - a greeting and a reply, then the machine's close reaching the client;
  - a port nothing in the machine holds, closed;
  - 3 MB out to a client that reads a second late, matching the probe's own spew on the host by length and FNV-1a;
  - 3 MB in, half-closed and counted by the machine, matching;
  - a client leaving after 100 kB of 20 MB, which the machine's writer hears as a broken pipe;
  - a client holding its connection unread, through a 4 KiB window, while the machine is stopped: the listeners close within four seconds, before that client reads again, and its connection then ends.
- **Ports, around those:** the listeners closed once the machine has gone, a port held on 127.0.0.1 leaving a machine publishing it free to start, the same port taken on the machine's address refusing it.
- **`hull proxy-ssh`:** the machine's undeclared port 22 reached both ways by its name with `.hull.`, and a refusal, 125 with its reason, dialled before the probe listens.
- **A udp port,** to a probe echoing every datagram to its sender: two clients each answered on their own flow, the probe counting two senders; datagrams of 0, 1, 1400 and 65,000 bytes each echoed whole; and a datagram sent while the probe is not listening dropped, while the client's later ones are answered on the same flow: the probe, listening again, sees the client come from the port it came from before. Sabotage, baseline first: breaking each of these failed it. Breaks in the agent: forwarding a refusal as a flow's end, putting datagrams through the byte ring, a 16 KiB datagram buffer, and dropping the empty datagram. Breaks in the engine: one flow for every client, a flow per datagram, and a 16 KiB buffer. The refusal first passed, since a new flow answers as well: the client is simply another sender to the machine. So the case now runs two echoes and compares the ports.
- **The emu class** under TCG, without the KVM clock, as an overlay whose write stays in the machine.
- **Nothing left:** no qemu of the suite's, no passt, and no machine directory, the dead core's included. passt is also checked gone with a SIGKILLed core, and gone with its machine while the core runs on: a machine boots and ends under the core another keeps running, and one passt is left.

**Sabotage,** baseline first, in a scratch copy running `make check-qemu`. Breaking each of these failed a named case:
- **In the agent:** the hostname, the environment, the workdir, `no_new_privs`, the session, mounting volumes, reporting a signal, 127 for a command not there, stderr kept apart, a stop's SIGTERM, and forwarding a signal.
- **In the engine:** memory, CPUs, TCG for emu, sweeping a dead core's directories, and reporting a signal's exit.
- **Read-only, root and volume,** each broken on both sides together: qemu's `readonly=on` and the agent's read-only mount. Either alone holds.
- **qemu's death signal** first passed. A core that dies closes its vsock connection, and the agent then stops the machine and powers off within the same second, which is `(on_disconnect stop)` doing its job. With both removed, the machine outlived its core and the case failed. Each alone holds.
- **Untested:** retrying a vsock context id that is taken, which a test cannot make happen on purpose.

**Sabotage of the overlay,** baseline first, the same way, after one baseline lost to an intermittent (§4.10's open items). Breaking each of these failed a named case: reading the mutability in the agent, the upper root's owner, its mode, the overlay module, and keeping a read-only root off the overlay. Two needed more:
- **qemu's read-only share alone** passes: the agent mounts the share read-only as well. With both broken and the share bound in place of the overlay, the writes reached the directory and the case failed. Each alone holds.
- **The tmpfs removed** passes and cannot fail here. The upper layer then lands on the initramfs's root, which the kernel also makes a tmpfs of half the memory when no `root=` is given. The mount is kept so the bound does not hang on how the kernel makes its root.

**Sabotage of exec,** baseline first, the same way. Breaking each of these failed a named case:
- the agent holding output to its credit, at both of its checks;
- the host giving output credit back;
- the agent giving stdin credit back;
- `eof` ending the input;
- the exit waiting for the output;
- the terminal's first size, and the resize on either side;
- a signal going to its own channel;
- an exec getting the machine's environment;
- `stream_close` killing the command.

Breaking the SIGKILL reported for streams whose agent has gone hung the stopped-machine case, and the suite ran out its time. Two needed more:
- **The host waiting for the agent's stdin credit** first passed. Its largest input to a command not reading was 300 kB, which arrived whole anyway. Tried by hand, 3 MB to a command sleeping three seconds arrived as 131,072 bytes, the rest refused as more input than the credit. That is now a case, and it fails. The same broken engine also delivered 1,174,207 bytes for `seq 1 200000`'s 588,895, about twice. No cause was found, and no path for intact code to overrun the agent's ring was found either. Unexplained, and seen only with the engine broken.
- **`TIOCSCTTY` removed** passed and cannot fail. On Linux a session leader that opens a terminal without `O_NOCTTY` gets it as its controlling terminal, so the open already does what the ioctl says. It is kept for saying so.

**Sabotage of the network,** baseline first, the same way. Breaking each of these failed a named case: `--no-map-gw` (both sides of the loopback case), the default route, eth0's address, the virtio_net module, qemu's netdev, passt's DNS forward, binding resolv.conf, and making it read-only. Three needed more:
- **`lo` left down** first passed. The case wanted any dial refused, and the dial to the gateway is refused too. With `lo` down the dial to 127.0.0.1 times out, and it does not reach the host's loopback, checked by hand. Each dial now names its address in its line.
- **Leaking qemu's end of passt's socket** first passed. passt then lived as long as the core, and every check for one left ran once the core had gone. A machine now boots and ends while another keeps the same core running, and exactly one passt must be left.
- **`--one-off` removed** passed and cannot fail. passt given a connected socket with `--fd` exits when its peer closes, with the option or without it, tried by hand. It is kept for saying so.
- **Untested:** passt failing to start, which a test cannot make happen on purpose.

**Sabotage of the ports,** baseline first, the same way. Breaking each of these failed a named case:
- in the agent: settling a connection, and its half-close;
- in the engine: shutting the client's socket at the end, the credit a connection gives back, the client's end becoming `eof`, and ending the connection once its client has gone;
- checking the listener's bind, and refusing a udp port.

Three needed more:
- **Closing the listeners with the machine** first passed. Once the ports machine was gone, the suite's core idled out, and its exit closed every socket anyway. A second machine now keeps the core running across those cases, and it fails.
- **Shutting each client's socket when the machine ends still passes, and why is not known.** Its job is to free a `deliver()` blocked writing to a client that does not read, so that `destroy()` can end. The case now holds a client with a 4 KiB window, unread, for four seconds before the stop, and requires the listeners closed within four seconds after. Before the stop, the engine's socket had 1.79 MB unsent and two of the core's threads waited in `wait_woken`, one blocked in `send()` and one in `recv()`.
  - **Under strace, the sabotage did what it should:** the main thread ended in a futex wait inside `destroy()`, with the `send()` and `recv()` still blocked. The intact build, traced the same way, shut the socket and returned to its poll.
  - **Untraced,** the sabotaged core had both threads gone within half a second of the stop, and the listeners closed. Nothing found closes or shuts that socket on that path.
  - So the shutdown is shown needed, under a tracer only. What frees the untraced core is open.
- **The count:** of the eleven sabotages, nine fail a named case. `3ba0438`'s message says ten. It was written before the second machine's case was rerun, and that rerun caught only the listeners.
- **The agent's zero linger removed** passed and cannot fail here. Closing a socket with unread data already resets it on Linux, and a writer that is still writing always leaves some. The linger covers the moment a writer has none queued, which a test cannot choose.

---

## 5. Engines

### 5.0 Engines are plugins — the backend set is open

The driver interface is not internal plumbing; it is Hull's **stable plugin API**: `plan(spec) → Plan`, `create/start/stop/destroy`, `exec`, `logs`, `port_map`, `stats`, plus a **capability declaration** — the engine's column of the §6 matrix, published by the engine itself rather than hardcoded in Hull. Everything the CLI exposes goes through this interface, which is what makes backends honestly interchangeable, and (per the project guidelines) it is a natural fit for a C ABI: an engine is a shared object or external helper binary implementing the interface.

**Classes and engines.** ns, vm, and emu are engine *classes* — they define the rows of the capability matrix, the ingress contract, and the migration analysis. Concrete **engines** implement classes: the docker adapter implements ns; the qemu engine implements both vm (KVM acceleration) and emu (TCG); the virtualbox and vmware adapters implement vm. `(backend …)` accepts either a class (host's default engine of that class) or an engine by name. Class-level guarantees are what the conformance suite tests; engine-specific deltas overlay the class column in the matrix.

**First wave — adapter engines** (wrap what users already have installed; maximum reach, minimum novelty):

- **docker** — drives the Docker daemon via its API: spec → container config, `ports`/`volumes`/`env` map directly, `hull exec/logs` ride Docker's own exec/log streams. Honest impedance, declared in its matrix column: dir sources with `(mutable direct)` are *rejected* (Docker can't run an arbitrary rootfs directory in place; `overlay` mode works via a cached `docker import` of the tree), and the adapter **inherits dockerd's root daemon and security posture** — the rootless goal (§7) is delivered by the native ns engine in wave two, not by this adapter. A podman variant is nearly free (same API surface, plus `--rootfs` makes `direct` dir sources native and restores rootless).
- **qemu** — the reference engine, no daemon to adapt to: implements vm (accel=kvm) and emu (accel=tcg) with everything §5.2–5.3 describe (virtiofs, vsock agent, micro/full profiles).
- **virtualbox** — full-profile vm-class via `VBoxManage`: block projection (§3.9) for the rootfs, vboxsf for volumes, NAT rules for ports, agent via seed ISO.
- **vmware** — full-profile vm-class for Workstation/Fusion via `vmrun`/VIX (ESXi/vCenter is enterprise-tier territory, Appendix I): generated `.vmx` + VMDK via block projection, HGFS shared folders for volumes, agent via seed ISO. Live migration on desktop VMware: none (vMotion is vCenter's, and that's fine — it maps to the paid tier anyway).

**Second wave — native engines:** the crun-based rootless ns engine and the microVM profile with bundled kernel (§5.2, Appendix F) — these deliver the security-model promises the adapters can't, and slot in without any spec change precisely because classes, not engines, define the contract.

**Reserved pigeonholes:** the abstraction target is deliberately broader than "a Linux machine" — it is *an isolated execution environment over a directory of files*. Declared engine-name slots are reserved (name, class, expected capability shape) even before any implementation exists, so specs and docs can reference them stably:

- **hyperv** — vm-class, the **Windows-host slot**: driven via PowerShell/WMI, rootfs via VHDX block projection (§3.9/§3.13), volumes via SMB/Plan-9 shares, and the agent control plane over **Hyper-V sockets** (AF_HYPERV — Hyper-V's native vsock equivalent, so Appendix B ports with a transport swap, not a redesign). Reserved now so Windows host support lands as an engine, not a rewrite. (Distinct from the "Windows containers" non-goal: this is Windows *hosting* Hull machines.)

That admits further engines like:

- **chroot** — near-trivial, useful as the reference plugin implementation;
- **DOSBox** — `(source "dir:./c")` mounts as `C:`, `command` becomes the autoexec line, extra `volumes` become additional DOS mounts, `(media …)` cdrom entries appear as disc drives reconstructed from file collections (§3.10, copy protection included), and nearly everything else (`ports`, `memory` semantics, `ssh`) is declared `rejected` in its capability table — which is fine, because the matrix makes partial engines *safe* rather than broken;
- **Wine**, **user-mode Linux**, **wasm runtimes** — same story with different capability subsets.

An engine declares what it can do; the plan step enforces it; the conformance suite is tagged per capability, so any third-party engine can run exactly the subset of the shared tests its declaration claims — and gets held to it. Spec fields an engine doesn't know are `rejected` by default, never ignored. The result: the same `hull.conf` + files-in-a-directory workflow drives any execution substrate anyone cares to plug in, and "switch backend" stays a one-line change all the way from crun to DOSBox.

### 5.1 The `ns` class — namespaces (default)

Wave one implements this class with the **docker adapter** (§5.0). The wave-two **native engine**: crun under user namespaces; idmapped mounts for volume ownership sanity; cgroups v2 limits via systemd user-session delegation; pasta for networking. Startup in tens of milliseconds. Shared host kernel — the fast path for trusted dev work, and the engine that delivers the rootless promise natively.

### 5.2 The `vm` class — hardware-virtualized (reference engine: qemu)

The default `micro` profile:

- QEMU `-M microvm` (or cloud-hypervisor) with a **bundled minimal guest kernel** (virtio, virtiofs, vsock, overlayfs; built from a pinned LTS config, updated with Hull releases). Boot target: 100–300 ms.
- Rootfs: the composefs image — or the dir source — over virtiofs (plus `upper/` over a second share when `(mutable overlay)`), assembled by a ~200 KB static **guest agent** that is PID 1, mounts the root, applies the spec (env, workdir, uid), and exec's the workload.
- The agent speaks a small protocol over **vsock**: exec, logs/stdio streaming, stats, graceful stop. This is what makes `hull exec`/`hull logs` byte-for-byte identical UX across backends.
- Ports: host-side forwarders mapped from the same `ports` spec onto the machine's stable ingress address (§8.1).
- `(kernel image)` lets an image ship its own kernel at a well-known path for kernel-dependent workloads.

The `full` profile boots a standard QEMU machine type instead — full distro init, custom kernels/modules, PCI/USB passthrough, optional GUI. Startup in seconds; the matrix makes the cost visible. The **virtualbox** and **vmware** engines (§5.0) implement this class in full-profile form only, via the block-projection path (§3.9) — same class contract, their own matrix deltas. If VirtualBox's API drag ever exceeds its value it is the first engine to cut — the spec doesn't change either way.

### 5.3 The `emu` class — software emulation (qemu, accel=tcg)

The same driver as `vm` with hardware acceleration removed — same profiles, same kernel strategy, same virtiofs shares, same guest agent, same vsock control plane (with a virtio-serial fallback where vhost-vsock isn't available). Two reasons it exists:

- **No KVM:** locked-down hosts, nested environments, CI runners without /dev/kvm. The spec runs unchanged, just slower.
- **Foreign architecture:** set `(arch arm64)` (or riscv64, …) and the machine boots a foreign-arch kernel and rootfs on any host. Combined with dir sources this means: put an ARM rootfs in a directory, run it on your x86 laptop, switch it to `(backend vm)` unchanged when you're on ARM hardware.

Cost is honesty-first: TCG runs roughly 5–20× slower than native and the matrix says so. A middle path for foreign-arch *processes* (not kernels) is `ns` with `(binfmt true)` — qemu-user under binfmt_misc, faster than full-system emulation, flagged `emulated` in the matrix because syscall emulation has edge cases.

---

## 6. Capability matrix (deep dive)

The matrix is data, not documentation: assembled at load time from each engine's capability declaration (§5.0 — core engines ship theirs built in, plugins publish their own), consulted at plan time, and printable via `hull plan --explain`. Every spec field maps, per backend, to one of:

- **native** — honored exactly;
- **emulated(cost)** — honored via a documented mechanism with a stated cost (e.g. vboxsf volume = slow I/O);
- **rejected(reason)** — plan fails with the reason and, where one exists, the suggested alternative.

| Spec field / feature | ns | vm | emu |
|---|---|---|---|
| `command`, `env`, `workdir` | native | native (agent) | native (agent) |
| `(source "dir:…")` (+ `(mutable direct)`) | native (idmapped bind) | native under passthrough; block projection rejects unless `writeback` (§3.9) | native under passthrough; same block rule |
| `ports` → stable ingress address (§8.1) | native (pasta) | emulated: host forwarder | emulated: host forwarder |
| `(ssh true)` / `hull ssh` | native | native | native |
| `volumes rw/ro` | native (idmapped bind) | native (virtiofs; vboxsf/HGFS on the virtualbox/vmware engines is slow) | native (virtiofs) |
| `cpus`, `memory` | native (cgroups; memory = ceiling) | native (hard allocation) | native (hard allocation; cpus = threads) |
| `readonly_root` | native | native | native |
| `arch` ≠ host | rejected → `emulated` with `(binfmt true)` (qemu-user) | rejected: KVM is same-arch → use emu | native (that's the point) |
| host networking | native, **opt-in flag** | rejected: no shared netns → use `ports` | rejected: same |
| device passthrough | rejected in rootless (no nodes) | emulated: virtio/VFIO/USB where permitted | emulated: QEMU-emulated devices only |
| custom kernel / modules | rejected: shared host kernel | native (`kernel` key; `full` profile for distro kernels) | native |
| inotify on volumes | native | rejected-by-default: virtiofs event gaps → poll option offered | rejected-by-default: same |
| startup latency (informational) | ~10 ms | ~100–300 ms (micro), seconds (full) | seconds; workload 5–20× slower |
| kernel isolation boundary (informational) | none (namespaces) | hardware VM | full guest stack, software boundary |

Two rules keep this honest:

1. **No silent degradation.** If a field would behave *differently* (not just slower) on the selected backend, that's `rejected` unless the user opts in (`hull run --accept vm:inotify-poll`). Slower-but-identical is `emulated` and merely warned about once.
2. **The matrix is versioned with the binary** and covered by a conformance test suite: every `native`/`emulated` cell has an integration test that runs the same spec on that backend and asserts identical observable behavior (exit codes, file contents, port reachability, env, uid mapping). The matrix isn't a promise — it's a test manifest.

`hull check --backend vm` runs plan-only validation, so switchability can be enforced in CI: a project that must stay backend-portable adds one CI line per backend.

---

## 7. Security model

**Decided 2026-10-15, by the holder: hull does everything it can as the user, and asks for root only at the very end of the line.** In the holder's words, "our basic ideology is that docker is broken precisely because it needs root for everything". So root is never a convenience or a default. Where something can be done as the user, through a user namespace, a user-owned socket or a file the user owns, it is. A design that reaches for root has not yet found the user's way, and the engines that inherit root from what they wrap say so in their declarations. How a spec asks for root at the end of the line is not designed, because no spec has reached it.

- No root daemon, no setuid helpers, no privileged socket. The resident component is the per-user core (§2), an unprivileged service owned by the invoking user; the attack surface of "Hull installed" is user-owned processes and a user-owned socket. Adapter engines inherit the wrapped runtime's posture — the docker adapter rides dockerd's root daemon, and its matrix column declares it — while native engines carry the full rootless model.
- ns backend: user namespaces map root-in-container to an unprivileged subordinate uid; seccomp default profile (crun's) + `no_new_privs` on by default; capabilities are empty-by-default and additive via `(security (allow …))` — there is deliberately **no `--privileged`**. Workloads that need it are told to use the vm backend, which is the correct tool.
- vm/emu backends: the kernel boundary is the headline feature. The guest agent treats the vsock channel as the only control plane; no host filesystem is visible except declared volumes.
- Image integrity: fs-verity on objects + signed manifests (§3.5); `hull pull` can require signatures per-registry via policy in `~/.config/hull/policy.conf`.

## 8. Networking

Default is user-mode networking everywhere: pasta (ns) and per-VM user-net/vhost (vm/emu), with the `ports` spec as the single source of truth. Machine-to-machine networking within a multi-machine file gets a shared user-net segment with DNS names matching machine names (compose semantics), implemented per backend but specified once. Anything fancier (macvlan, bridges) requires host privileges and is explicitly out of the rootless core — documented escape hatch, not a default.

### 8.1 Ingress identity — reaching machines identically

The contract: **how you get *into* a machine never depends on its backend.** Three layered mechanisms:

1. **Stable per-machine address.** Every machine gets a deterministic loopback IP (`127.107.x.y`, derived from the machine name — stable across restarts, rebuilds, *and backend switches*) plus a name in Hull's per-user stub resolver: `web.hull → 127.107.x.y`. Every declared port is forwarded from that IP to the machine by whichever plumbing the backend uses (pasta, user-net forwarder) — invisible at this layer. `psql -h db.hull` and `curl web.hull:8080` are byte-identical commands on ns, vm, and emu, and they keep working mid-project when a machine changes backend.
2. **SSH without thinking.** `hull ssh <machine>` always works: if the machine runs sshd it proxies to its port 22; if not, it falls back to an agent exec with a real PTY (ns: namespace enter; vm/emu: vsock exec) — same keystrokes either way. For native tooling — plain `ssh`, `scp`, `rsync`, IDE remote-development — `hull ssh-config` maintains a block in `~/.ssh/config`: `Host *.hull → ProxyCommand hull proxy-ssh %h`. After that, `ssh web.hull` works from anything that speaks SSH, on every backend. Critically, **host keys are machine state, not boot state**: Hull generates the host key at machine creation and injects it on every start, so switching ns → vm → emu never trips a known_hosts warning. The machine's SSH identity belongs to the machine, not to the backend.
3. **sshd provisioning.** `(ssh true)` makes Hull inject and supervise a minimal sshd (with your authorized_keys) through the same start path on all backends — so even a bare `dir:` rootfs with nothing installed is reachable over real SSH with one line of config.

**Built so far:** item 1's address, on docker and qemu; item 2's fallback, `hull ssh` as a login shell on every engine (§4.10); item 2's `hull proxy-ssh HOST`, the ProxyCommand, and `hull ssh-config`, which writes the block naming it; and item 1's name, through an NSS module, all since 2026-10-20.
- **`hull proxy-ssh web.hull` is a byte pipe to the machine's own port 22,** declared or not, with stdin and stdout the connection, as ssh expects of a ProxyCommand. The name may carry `.hull` and a final dot. It is the frontend protocol's `dial`, answered as an exec is, and the engine ABI's `dial` op, appended under rule 4: qemu's is the agent's `connect`, as a forwarded port's is; docker's connects from the host to the container's address, as docker's own proxy would. A port nothing listens on is refused, 125, `port 22: Connection refused` on both. chroot has none: its machines share the host's network, where port 22 is the host's own, and the core says so.
- **`hull ssh-config` keeps the block in `~/.ssh/config`:** `Host *.hull` with `ProxyCommand "/full/path/to/hull" proxy-ssh %h`, this hull's own path, between two marker comments. ssh applies what comes before the first `Host` or `Match` to every host, so the block goes just before that line. There, a user's `Host *` cannot take the ProxyCommand first, and the user's global lines are not taken into the block's Host. With neither line, it goes at the end. Running it again replaces the block, and `--remove` takes out exactly it. The file is written whole and renamed into place, keeping its mode; a missing one is made 0600, in a `~/.ssh` made 0700. A hull whose path holds `"`, `%` or `\` is refused, being nothing ssh could be given.
  - **Measured:** in `test/cli_test.sh`, ssh itself, `ssh -G` on the file written: `web.hull` gets the ProxyCommand over a later `Host *` with one of its own, and another host keeps both that and the global line. The block is once after two runs, in its place, and the file keeps its mode; `--remove` gives back the file to the byte. In `test/qemu_test.sh`, real ssh through the block to a vm whose port 22 is the probe. The probe speaks no SSH, so the login fails, but the line it read was ssh's own identification, `SSH-2.0-OpenSSH_10.0p2`, which came by ssh, the block, `hull proxy-ssh` and the agent.
  - **Sabotage,** baseline first: breaking each of these failed a named case: the block at the end, the block at the very top, the old block not dropped, and the mode not kept. The case asks another host for the global line too, added before the run: at the top the global line falls into `Host *.hull`, so `web.hull` alone would still have it. That it would otherwise have passed was reasoned, not run.
- **`web.hull` is answered by `libnss_hull.so.2`,** glibc's hosts database for names ending in `.hull`, built from `nss/nss_hull.c` by `make`. Decided 2026-10-20 by the holder. A name another program looks up can only reach the host's resolver, and adding to that needs root whichever way it is done: an NSS module, a resolved domain, or `/etc/hosts`. `HOSTALIASES` and `LD_PRELOAD` need no root, but reach only programs started with them. The module needs root once and never after, which is §7's root at the very end of the line: `make install-nss` as root, then `hull` on the hosts line of `/etc/nsswitch.conf`, before `dns`.
  - **What it reads:** the core keeps `hull/hosts` in its runtime directory, a line per machine it holds, `ID ADDRESS`, as `ps` lists them. The file is replaced whole by a rename whenever the listing changes, so a reader never sees half of one. Only the core holding the lock writes it. It writes a fresh one before its first wait, since what a core that died left is not true, and removes it as it goes, before the lock is free. The module reads the caller's own: `$XDG_RUNTIME_DIR` through `secure_getenv`, so a setuid program is not told where to read, and only if absolute, or else `/run/user/UID`. A user's names are theirs, and another's are not answered.
  - **What it answers:** a name ending in `.hull`, in any case and with or without a final dot, forward through `gethostbyname4_r` (what `getaddrinfo` uses) and the older entry points, and in reverse an address in 127.107/16 that a machine has. Any other name is not found, and the next service is asked. IPv4 only: glibc maps the address for a caller asking for IPv6 with mapped addresses allowed.
  - **Tested without installing:** `test/nss_test.sh`, in `make check`, asks glibc through `getent` in a user and mount namespace of its own, where an `nsswitch.conf` saying `hosts: hull` is bound over `/etc`'s and the library is found on `LD_LIBRARY_PATH`. So the module is loaded and called as an installed one is. Its 19 cases: a missing file, forward lookups in two spellings, `ahostsv4` and `ahostsv6`, the reverse, five names and two addresses it must not answer, a relative runtime directory refused, and the core's own file. That file is fresh at start over a stale one, names a running chroot machine at the address `ps` gives, both ways, and is gone with the core.
  - **Sabotage,** baseline first: breaking each of these failed a named case: a name's case kept, its final dot kept, the file never written, and the file left when the core goes. The core first made a fresh file once as it started, and again in the loop after each turn: removing the first passed, since the loop's first turn wrote it anyway. It is now one call, before each wait, which the first wait covers.
- **Sabotage,** baseline first: breaking each of these failed a named case: qemu dialling the next port, `.hull` left on the name, docker dialling the host rather than the container, and docker reporting a dial's end as an exec's. Dialling the host answered where the container refused, from the host's own sshd: what chroot would do, and why it has no `dial`.
- **Not yet:** `hull ssh` preferring a machine's sshd, which needs the user's key authorized in the machine, item 3, before it would be better than the shell it falls back to; and the generated host key.

## 9. Integration

- **systemd:** `hull systemd <machine>` emits a quadlet-style user unit (`Restart=` from the spec's `restart`, journald-native logging, socket activation optional). Boot-start is `systemctl --user enable` + linger — all standard.
- **Migration:** `hull import Dockerfile` converts to a `(build …)` form (best-effort, annotated where semantics differ); `hull import docker-compose.yml` converts to a multi-machine file. Both are one-way generators that print what they couldn't translate.
- **Spec dialects — a second pigeonhole axis.** Engines plug in on the execution side (§5.0); **dialects** plug in on the input side: alternative spec formats that compile to the same MachineSpecs + segments the native hull.conf produces. The **compose dialect** slot is reserved now: `hull up` in a directory containing a `docker-compose.yml`/`compose.yaml` runs it *directly* — no conversion step, the compose file stays the live source of truth (the podman-compose posture, for teams whose compose files are entrenched). Mapping shape: services → machines, networks → segments (Appendix D), `depends_on` → start ordering, healthchecks → agent probes, profiles → machine subsets; compose keys outside the supported subset get the same honesty treatment as everything else — **rejected or warned at plan time, never silently ignored** — and `hull plan --explain -f compose.yaml` shows exactly how each service compiled. The payoff of dialect-over-import: a compose team gets backend switching, ingress identity, and dir sources *without changing their file*, and converts to hull.conf only when they want Hull-native features compose can't express. Dialects are pluggable like engines; the native format remains hull.conf, and dialects never grow features the native format lacks.
- **Vagrant dialect — reserved slot.** `hull up` beside a `Vagrantfile`: `config.vm.box` → an attached/imported box (§3.13 handles the .box format), provider blocks → backend hints (`virtualbox`/`vmware` map to the first-wave engines directly), `synced_folder` → volumes, `forwarded_port` → ports, shell provisioners → build steps or start hooks. One honest complication, declared up front: a Vagrantfile is executable Ruby, not data — the dialect supports the declarative subset that covers the overwhelming majority of real Vagrantfiles and **rejects arbitrary Ruby logic at plan time** rather than embedding a Ruby interpreter. Strategic timing is favorable: Vagrant under HashiCorp's BSL is stagnant and its community is receptive to an exit ramp — this dialect plus the virtualbox/vmware engines *is* that exit ramp.
- **Registries:** any OCI registry works today (§3.5).
- **Kubernetes (later):** the driver interface is deliberately shaped so a CRI shim can sit on top, with the vm driver playing the Kata role. Out of scope for v1.
- **macOS/Windows (later):** on macOS, the qemu engine's "everything through virtiofs + vsock + agent" design is exactly what a Lima-style host VM needs; on Windows, the reserved **hyperv** engine slot (§5.0) is the path. The ns class simply doesn't exist on either, and the matrix says so.

## 10. CLI sketch

```
hull run [-f hull.conf] [--backend vm] [name]
hull build / pull / push / tag / images / diff A B
hull exec <machine> -- cmd… · hull logs -f <machine> · hull stop/rm
hull ssh <machine> · hull ssh-config            # ingress identity (§8.1)
hull commit <machine> <ref> · hull materialize <ref> <dir> · hull dir init <dir>
hull pack <dir> <file.hull> · hull unpack <file.hull> <dir>   # canonical single-file form (§3.13)
hull import <foreign image|Dockerfile|compose.yml>            # decompose to a collection (§3.11/§3.13)
hull plan --explain [--backend …]     # capability-matrix dry run
hull check --backend ns,vm,emu        # CI portability gate
hull systemd <machine> · hull gc
hull engines                          # list installed engines + their capability tables
```

## 11. Risks & open questions

1. **Guest kernel maintenance** (vm): bundling a kernel means owning CVE response for it. Mitigation: pinned LTS, tiny config, auto-rebuild pipeline; `(kernel image)` as the escape hatch. This is the single largest ongoing cost of the design.
2. **virtiofs fidelity:** inotify gaps, mmap coherence, and performance vs. bind mounts are the main sources of ns↔vm behavioral difference. The matrix marks them honestly; upstream virtiofsd keeps improving.
3. **Desktop-hypervisor API drag:** `VBoxManage` and `vmrun` scripting are workable but brittle across versions — the cost of the adapter strategy. Contained by the class contract (an adapter that breaks is dropped or pinned without touching the spec); decision point at end of Phase 2 for each: keep, pin to known-good versions, or cut.
4. **composefs availability:** requires a reasonably recent kernel (EROFS + overlayfs data-only layers). Fallback path for older kernels: plain overlayfs over an extracted tree (loses verity, keeps the CAS).
5. **Ecosystem gravity:** the honest competition is podman + Kata + Incus + vmspawn. Hull's defensible core is the *unified spec + tested capability matrix + plain-file store*; everything else should reuse existing components aggressively rather than fork the ecosystem.
6. Open: chunked object transfer protocol for pull (casync-style seeding?), secrets handling beyond `env_file`, multi-arch manifest strategy.

## 12. Roadmap

- **Phase 1 — Core + docker & qemu engines:** spec parser, plan/matrix engine, the plugin driver API (chroot as reference plugin), `dir:` sources with direct/overlay mutability, the **docker adapter** (ns-class) and the **qemu engine** (vm-class via KVM + emu-class via TCG, guest agent + vsock, virtiofs), `run/exec/logs/stop`, stable ingress addresses + `hull ssh`. No store code at all. *Exit: one hull.conf flips between docker and qemu — container to VM to emulation — and deploys as a plain rsync'd directory.*
  - **Met, 2026-10-19, by `test/flip_test.sh`, which `make check-flip` runs** against the local daemon and KVM. A project directory is copied with `rsync -a` and run from the copy unchanged, by `--backend docker`, `vm` and `emu`. Its file names no backend; it has a dir source as an overlay, a workdir, an env, a volume, a port and a command. For each: the exit status, the port reached by a client on the host at the machine's §8.1 address, which is the same on all three, and the volume's write landing as the user's. The machine's own lines must be the same on all three, and the deployed tree the same bytes afterwards, by its tar's SHA-256. 18 cases.
  - **What differs, and is not checked:** the uid. docker runs the workload as the user (§4.9); qemu runs it as the guest's root, which writes as the user through 9p. So does `/`'s owner (§4.9, §4.11), and what each engine declares emulated, which each run prints.
  - **Sabotage,** baseline first, in scratch copies. Breaking each of these failed a named case: the vm's workdir; docker's env; the port on the machine's address, on qemu and on docker each; overlay left off in the agent, behind qemu's read-only share; and both together, where the writes changed the deployed tree.
- **Phase 2 — virtualbox & vmware adapters:** block projection + deterministic disk materializer (§3.9), seed-ISO agent path, `VBoxManage` and `vmrun` drivers, full ingress identity (`hull ssh-config`, host-key pinning, `(ssh true)` provisioning), `hull check` CI gate. *Exit: all four first-wave engines pass their declared class conformance; an ARM directory rootfs runs on x86 via qemu-tcg.*
- **Phase 3 — Optional store & OCI border:** CAS + manifests, commit/diff/gc, composefs mounting, fs-verity, OCI pull/push (any Docker Hub image becomes runnable or `materialize`-able to a directory), `hull build` with content-hash caching. *Exit: pull, unpack-to-dir, build, and publish images with correct caching — all still optional.*
- **Phase 4 — Native engines & polish:** the rootless crun-based ns engine (delivering §7's security model natively; pasta networking) and the microVM profile with bundled kernel pipeline (Appendix F), systemd generator, Dockerfile/compose importers. *Exit: the rootless promise holds without Docker installed; the multi-engine story is conformance-tested across adapters and native engines alike.*

---

*Prior art gratefully studied: podman/crun (rootless model), Kata Containers (VM-as-OCI-runtime), Incus (unified container/VM UX), systemd-nspawn/vmspawn, OSTree/Nix (CAS), composefs/EROFS (mounting), Firecracker/cloud-hypervisor (microVMs), Lima (host-VM bridging), Vagrant (VM ergonomics).*

---

# Appendix A — Manifest schema (v1)

A manifest is the canonical description of a filesystem tree. Its identity is `sha256(canonical encoding)`; that hash is what refs point at, what signatures cover, and what build caching keys on.

## A.1 Two representations, one identity

- **Canonical form:** a canonical s-expression (§4.1), Rivest's SPKI encoding — every atom written as its decimal byte length, a colon and the bytes (`4:root`), lists in parentheses, and no whitespace or other syntax at all. This is what gets hashed, stored, and signed. Hashing a text format usually means litigating whitespace and key order forever; this one has no whitespace, and the rules below fix everything else, so a value has exactly one encoding:
  - the entries of a record are sorted bytewise by key, and a key appears at most once; the entries of a list (`(ports (port …) …)`, the manifest's `entries`) keep their order, which the schema fixes where it matters — A.2 sorts entries by path;
  - an integer is decimal ASCII with no leading zeros and no `+`, so a mode is `3:493`, never `#o755`;
  - no floats, no display hints (Rivest's `[type]` prefix), and no timestamps except where specified;
  - atoms carry no type: the symbol `ns` and the string `"ns"` encode identically as `2:ns`, and so do `true` and `"true"`. The readable form parses to the canonical one with no help, but rendering canonical back to readable needs the schema, which says which fields are strings.
- **Human form:** `hull manifest show <ref>` renders a lossless s-expression form for eyeballs and diffs. That form is a *view*; editing it and running `hull manifest compile` produces a new canonical manifest with a new identity. Nothing ever mutates in place. An atom that is not printable UTF-8 — an xattr value, say — is shown as hexadecimal between `#` signs (`#7b22…#`), Rivest's own notation for it, so the view stays lossless.

## A.2 Structure

**The manifest is described in situ, decided 2026-10-09: `manifest/manifest.situ` is the definition, and this section is its summary.** It closes the open item in §3.12 for the manifest. The schema states the canonical bytes exactly, and `make test` holds it to `manifest/manifest.vectors`, which it must accept, and `manifest/manifest.refused.vectors`, which it must refuse. Nothing is generated from it yet; hull's reader walks a manifest through `sexpr/` until a consumer needs accessors.

A manifest in its readable view (A.1), entries in canonical order:

```
(manifest
  (arch "amd64")                        ; one arch; multi-arch is a ref-level index
  (entries                              ; sorted by path, bytewise
    (entry (kind dir) (mode 493) (owner root) (path "usr"))
    (entry (kind reg) (mode 493) (object #e3b0…#) (owner user)
           (path "usr/bin/python3") (size 12345)
           (xattrs ("security.capability" #0100…#))))
  (provenance …)                        ; optional, outside the build cache key (A.5)
  (runtime (cmd "python3") (workdir "/srv"))    ; optional (A.4)
  (schema 1)
  (stats (file_count 2) (object_count 1) (total_size 12345)))  ; advisory, verified on import
```

An `entry` holds, in canonical order:

| Key | When | Holds |
|---|---|---|
| `dev` | `kind dev` | `(major n) (minor n) (type b\|c)`: recorded always, materialized only on VM backends (the matrix rejects it on ns) |
| `group` | `kind hardlink` | a group id; entries sharing one are one inode |
| `kind` | always | `reg`, `dir`, `symlink`, `hardlink`, `fifo` or `dev` |
| `mode` | always | permission bits only, in decimal: 0755 is `493`; the file type is `kind` |
| `object` | `kind reg` | the content's sha256 in the object store, as its 32 bytes |
| `owner` | always | `root`, `user`, or `((gid n) (uid n))`: an ownership *class* (A.3) |
| `path` | always | relative, no leading `/`, NFC-normalized UTF-8, `/` separated, no `.` or `..` |
| `size` | `kind reg` | bytes, so a planner need not open the object |
| `target` | `kind symlink` | the link's target, as written |
| `verity` | `kind reg`, where enabled | the object's fs-verity digest, as its 32 bytes (A.6) |
| `xattrs` | optional | `(name value)` pairs sorted by name; `user.*` and `security.*` only |

**What situ states and what hull checks.** Canonical form fixes each record's order, so the schema spells it directly: a required entry is its fixed opening bytes, `preamble u8[7] = "(4:arch"`, and an optional one is chosen by peeking at the four bytes after it, `(` with the key's length and first letter. Keys out of order, a missing required entry, a kind or owner not in the list, a digest not 32 bytes, `schema` other than 1 and any whitespace all fail where the bytes stop matching. What the schema cannot say, hull checks after it: that entries are sorted by path with no path twice and pairs sorted by name, the second column of the table above, and that a number is minimal decimal digits and a path is well formed. Atoms are untyped (A.1), and situ cannot yet read a decimal number whose width a field gives (situ finding 13), so numbers are framed as atoms.

Deliberate omissions: **no mtimes** (normalized to epoch 0 on commit — timestamps are the classic reproducibility leak; software that needs real mtimes gets them in the writable layer), **no whiteouts** (no layer stack to punch holes in), **no uid/gid numbers in the common case** (see A.3).

## A.3 Ownership classes

Raw numeric uids are what make Docker images awkward under rootless and across backends. Manifests therefore store an ownership *class*, resolved at mount/materialize time per backend:

- `(owner root)` → root-in-machine (subordinate uid 0-mapping on ns; real uid 0 in VMs),
- `(owner user)` → the machine's default unprivileged user,
- explicit `(owner (gid n) (uid n))` → preserved verbatim for images that need real multi-user layouts (recorded as-is; ns backend maps through the subordinate range).

OCI imports map `0 → root`, the image's configured user → `user`, everything else → explicit. Round-trip to OCI is lossless.

## A.4 RuntimeDefaults

The OCI config-blob equivalents, overridable by `hull.conf`, every one optional: `(cmd word…)`, `(entrypoint word…)`, `(env (NAME value)…)`, declared `(ports "80/tcp"…)` and `(volumes "/data"…)` (advisory, in OCI's own spellings so a round trip is lossless), `(stop_signal name)`, `(user name)` and `(workdir path)`. Kept in the manifest so `hull run <ref>` with no spec file behaves like `docker run`.

## A.5 Provenance

Audit trail, excluded from the identity hash's *cache-key view*: `(oci (digests …) (registry …))` for pulled images, in OCI's digest spelling; `(build (inputs hash…) (parent hash) (step …))` for built ones, the step being its own s-expression (Appendix E); and `(builder version)` always. Excluded from cache keys so that re-pulling the same content from a mirror doesn't invalidate builds — two manifests with identical entries have identical *content identity* even if provenance differs. (Concretely: the manifest hash covers everything; the build cache keys on `sha256(entries + runtime)` only.)

## A.6 Verity binding

When fs-verity is enabled, each `reg` entry additionally carries the object's verity digest as `(verity …)`, and the composefs image generated from the manifest pins those digests — the kernel then refuses tampered objects at read time. A signed manifest therefore transitively signs every byte of the tree, per-file, which per-layer OCI signing cannot express.

---

# Appendix B — Guest agent & vsock protocol (v1)

The agent is what makes `exec`, `logs`, and `stop` identical across backends. It is a single static binary (~200 KB, no libc dependency — musl or pure syscalls), shipped inside the Hull release, injected read-only into the guest (virtiofs share on vm/emu micro profile; seed ISO on full-profile/VirtualBox boots), and run as PID 1.

## B.1 Boot sequence (vm)

1. Kernel boots with `init=/hull/agent`, cmdline carries: vsock CID, share tags, machine id.
2. Agent mounts `proc`, `sys`, `dev` (devtmpfs), the rootfs virtiofs share (ro) and the `upper` share (rw); assembles overlayfs; `pivot_root`.
3. Applies MachineSpec (received in step 5 — mounts that depend on it are deferred): env, workdir, hostname, uid/gid, rlimits.
4. Listens on **vsock port 1** (guest-side listener; host connects — the host is always the initiator, the guest never dials out).
5. Host connects, `hello` exchange, host sends `spec`, agent finishes setup, replies `ready`.
6. Host sends `start`; agent forks the workload, becomes its reaper, and streams stdio.

Target: steps 1–6 inside the 100–300 ms boot budget. On full-profile and virtualbox/vmware-engine boots the same agent runs as a systemd unit or init drop-in from the seed ISO; everything from step 4 on is identical. Under emu the sequence is unchanged (virtio-serial replaces vsock where vhost-vsock is unavailable).

## B.2 Transport & framing

One vsock connection is the control plane; there is exactly one, and the agent drops any second connection attempt (single-controller invariant — vsock is host↔guest only, so no further authentication is needed; the host side of the socket is the Hull process that owns the VM).

**A second controller after the first has gone**, under `(on_disconnect keep)`, is believed only with the token the first gave with `start` (§4.11): the invariant is one controller at a time, and the token is what makes the next one the user's.

Framing: `u32 length ‖ canonical s-expression` (A.1). A canonical s-expression delimits itself, so the length is not needed to parse; it is there so the receiver can refuse an oversized message before reading it. **The length is big-endian, and a body holds at most 1 MiB, decided 2026-10-09:** under the emu class the host and guest can be different architectures, so native order would be a different number at each end, and refusing an oversized message needs a number to refuse above. What a body holds is `agent/protocol.situ`'s (B.3). A message that breaks either rule, or whose body is not one canonical s-expression, ends the connection: a reader that has lost a frame boundary cannot find the next one, and the protocol does not try. Every message: `(<type> (ch <n>) …)`. Channel 0 is control; each exec/log stream gets its own channel id allocated by the host. Data frames carry raw bytes, which a length-prefixed atom holds as they are with no escaping: `(data (buf <bytes>) (ch <n>) (fd 1|2))`, entries in canonical key order. Flow control: per-channel credit windows (`(credit (bytes <n>) (ch <n>))`) so one chatty exec can't starve the control channel.

The framing is generated by situ from `agent/message.situ`: its frame layer turns the byte stream into whole messages and its build layer writes one. `agent/framing.c` adds what the schema cannot say. A length above the maximum is refused the moment its four bytes arrive, because situ's reader does not apply the schema's `[max]` while waiting: given a buffer larger than the largest message it waits for the body, and once that arrives it hands out a message its own schema rejects. That went to situ as its finding 12. Every message situ hands out is validated, and its body opened with sexpr/'s canonical reader before a caller sees it; the writer refuses a body that is not one canonical s-expression, where the fault is still the sender's to see. `test/framing_test.c` runs 58 cases: arrival byte by byte, two messages in one push, the maximum and one byte over (in a buffer exactly that size and in a larger one), a length that would wrap, five malformed bodies, and writing then reading back. Sabotage in a scratch copy fails the cases aimed at the early length check, the body parse, and each of the writer's two checks. Removing the `validate` of each message leaves the suite green, since the early check always fires first; it stays as a second witness to situ's `[max]`.

## B.3 Message set (v1, complete)

**The messages are described in situ, decided 2026-10-09: `agent/protocol.situ` is the definition, and this table is its summary.** It closes the open item in §3.12 for the protocol. A message is a record like any other (A.1), `(type (ch n) …)` with its entries in canonical order, and the schema spells each type the way A.2 spells a manifest: required entries are fixed opening bytes, optional ones are chosen by a peek, and the type itself by the four bytes after the `(`. The two directions are two vocabularies, `host_message` and `agent_message`, since `hello` and `data` carry different entries each way. `make test` holds it to `agent/protocol.vectors`, every type in the direction that sends it, and `agent/protocol.refused.vectors`. The agent and the qemu engine (§4.11) speak it, each validating what it reads with the C situ generates from it, for its direction.

| Host → agent | Agent → host | Purpose |
|---|---|---|
| `(hello (ch 0) (features name…) (proto n))` | `(hello (agent_version v) (ch 0) (features name…) (proto n))` | version/feature negotiation; lowest common `proto` wins |
| `(spec (ch 0) (machine …))` | `(ready (ch 0))` / `(error (ch n) (reason r))` | apply the resolved `hull.conf` before start |
| `(start (ch 0) (token t))` | `(started (ch n) (pid n))` … later `(exited (ch n) (code n) (signal n))` | run the workload; the token is what a later host resumes with |
| `(resume (ch 0) (token t))` | `started`, the workload's output since the last host went, and `exited` if it has ended | a host after the one that went, under keep (§4.11) |
| `(exec (argv word…) (ch n) (cwd p)? (env (NAME v)…)? (tty false\|((cols n) (rows n))) (user u)?)` | `started` / `exited` on that channel | `hull exec` |
| `(data (buf bytes) (ch n) (fd 0))` / `(eof (ch n))` | `(data (buf bytes) (ch n) (fd 1\|2))` / `(eof (ch n))` | stdio streaming |
| `(winch (ch n) (cols n) (rows n))` | — | tty resize |
| `(signal (ch n) (signal name))` | — | forward a signal: channel 0 to the workload, any other to its exec |
| `(sub (ch n) (since t)? (what logs\|stats))` | `data` / `(stats (ch n) (cpu n) (io n) (memory n))` | the guest's own view; not written, since `hull logs` is the core's and `hull stats` is measured from the host (§4.10) |
| `(stop (ch 0) (timeout_ms n))` | `exited`, then `(bye (ch 0))` | graceful stop: signal → wait → report; host kills the VM after `bye` or timeout |
| `(ping (ch 0))` | `(pong (ch 0) (uptime n))` | liveness; host declares the VM wedged after N missed pongs and falls back to hypervisor kill |
| `(credit (bytes n) (ch n))` | `(credit (bytes n) (ch n))` | per-channel flow control (B.2) |
| `(connect (ch n) (port n) (proto udp)?)` | `started` / `error`, `data` and `eof` as for `exec`, then `exited` | a forwarded port's connection to the machine's own address (§4.11); with `proto udp`, one client's datagrams, a `data` each, on no credit |

`?` marks an optional entry. Control messages are on channel 0 and the schema says so: `hello`, `spec`, `start`, `stop`, `ping`, `ready`, `bye` and `pong` carry the fixed bytes `(2:ch1:0)`. `exited` always carries both numbers, the signal 0 when none ended the process, so that `hull run; echo $?` can be exact. What the schema leaves to the agent and the host: that a channel was allocated, that credit is not exceeded, and that numbers are minimal decimal (situ finding 13, as in A.2).

Unknown message types are answered with `(error (ch n) (reason unsupported))` rather than dropped — that plus `hello`'s features is the whole forward-compatibility story. The schema frames an unknown type whole, as an atom and a run of forms, so it can be answered rather than lost with the connection. No dynamic protocol negotiation beyond this; v2 bumps `proto`.

## B.4 Failure semantics

- Agent crash ⇒ PID 1 dies ⇒ guest kernel panics ⇒ hypervisor reports exit ⇒ host surfaces "machine died: agent" with the last log tail it had streamed. Nothing hangs.
- Host process crash ⇒ vsock closes ⇒ agent's policy is spec-driven: `(on_disconnect stop)` (default: workload is torn down, mirroring ns-backend child-process semantics) or `keep` (for systemd-managed machines that should survive a CLI restart and be re-adopted via `hull attach`).
- Exit codes are reported exactly (code + signal), so `hull run; echo $?` is backend-identical — this is a conformance-suite assertion, not a hope.

---

# Appendix C — Two open questions, resolved into proposals

**Chunked pull (was §11.6a).** Adopt content-defined chunking (casync/FastCDC-style) as a *transport* option: `hull push --chunked` uploads a chunk index alongside the OCI export; `hull pull` with a warm store fetches only missing chunks and reassembles objects locally. Falls back transparently to plain OCI layer pull against dumb registries. The CAS is unchanged either way — chunking never leaks into the store format.

**Secrets (was §11.6b).** Never in env by default. `(secrets …)` maps names to sources (`file:`, `age:` encrypted file, `exec:` command output); Hull materializes them at start into a machine-private tmpfs (`/run/hull/secrets/<name>`, mode 0400) on every backend — tmpfs via bind on ns, via the agent on VMs — and they are excluded from `hull commit` by construction since tmpfs never touches the writable layer. Env injection remains available per-secret via `(expose env)` with a warning, because pragmatism.

---

# Appendix D — Multi-machine networking

A multi-machine file defines a **segment**: a private L2/L3 scope where machines reach each other by name. The spec is backend-neutral; each backend implements it differently but observably identically.

```scheme
(machines
  (web (source "…") (ports (port (host 8080) (guest 80))))
  (db  (source "…")))            ; no host ports: reachable only inside the segment
```

Semantics, tested by the conformance suite: every machine resolves every sibling by name (`db` → its segment IP); DNS is served by Hull itself (a stub resolver injected via `/etc/resolv.conf`), not by patching `/etc/hosts`; only `ports`-declared endpoints are reachable from the host; nothing in the segment is reachable from the LAN.

Per-backend mapping: **ns** — one shared user network namespace per segment (pasta attached once), machines join it, DNS stub listens inside. **vm** — one vhost-user/user-net virtual switch process per segment, VMs attach as NICs; the same DNS stub listens on the switch. **emu** — the same virtual-switch mechanism as vm (the switch doesn't care whether its NICs' guests are accelerated); the virtualbox engine maps segments to VirtualBox internal networks. Mixed-backend segments (web on ns, db in vm) are explicitly supported — this is the payoff of the whole design: put only the scary machine behind a hardware boundary — implemented by bridging pasta's segment socket to the VM switch.

Cross-file/segment communication is out of scope: publish a port. That rule is what keeps the model auditable.

Multi-machine files can also arrive via the **compose dialect** (§9): a `docker-compose.yml` compiles to exactly these segment semantics — services become machines, compose networks become segments — so entrenched compose setups get mixed-backend segments and ingress identity without rewriting their file.

# Appendix E — Build-step sandbox contract

Sound caching (§3.6) requires that a step *cannot* read anything outside its declared inputs. The contract:

- The step's mount namespace contains exactly: the parent manifest's tree (ro, composefs), the step's scratch overlay (rw), declared `copy` inputs (ro, bind-mounted from a content-hashed staging snapshot — the build hashes inputs *before* mounting, so mid-build edits of `./src` can't tear a step), `/proc`, `/dev` minimal, tmpfs `/tmp`.
- **Network is off by default.** A step that fetches (`apt-get`, `pip`) declares `(network true)`, which does two things: allows pasta attachment, and *demotes the step's cache entry to time-bucketed* (`(refresh "7d")` default, configurable) since network steps are inherently unreproducible. The doc-level advice: fetch in few `(network true)` steps, build in pure ones.
- Environment is scrubbed to a fixed allowlist plus declared `env`; no host uid leakage (steps always run as manifest-`root` in a userns); clock is real but `SOURCE_DATE_EPOCH` is set for toolchains that honor it.
- Output normalization on commit: mtimes → epoch (A.2), stable file ordering, `/tmp` and declared `discard` paths dropped. Result: pure steps are bit-reproducible in practice, and `hull build --check` (build twice, compare manifest hashes) is the reproducibility test.

Builds run on the ns backend by default but honor `(backend vm)` per step — needed when a build step must load kernel modules or exercise /dev, and otherwise identical thanks to the same contract applied via the agent.

# Appendix F — Guest kernel pipeline (vm backend)

The bundled kernel is the vm backend's biggest liability (§11.1); this is the containment plan.

- **One kernel, tiny config:** latest LTS, `microvm` defconfig-derived: virtio-{fs,net,vsock,blk,rng}, overlayfs, EROFS, cgroups, seccomp, no modules (everything built in, `CONFIG_MODULES=n` — no module loading means a whole attack class gone), no graphics/sound/USB/legacy. Target < 5 MB compressed, boot-to-init < 50 ms.
- **Reproducible build:** kernel + config + toolchain pinned in a lockfile; CI rebuilds bit-identically and signs the artifact; the kernel ships inside the Hull release archive, versioned with it (no separate download, no TOFU).
- **CVE response:** subscribe to the LTS stable queue; a kernel-only patch release channel (`hull self-update --kernel-only`) decouples security updates from feature releases. Because the config is tiny, the honest expectation is that most kernel CVEs don't apply — each advisory gets a public applies/doesn't-apply note.
- **Escape hatches, in order of preference:** `(kernel "path:./bzImage")` (bring your own), `(kernel image)` (the image ships `/hull/kernel`, for kernel-testing workloads — this is also how you run a *different kernel version per machine*, something Docker structurally cannot do), and the vm `full` profile for full-distro kernels.
- Non-goal: general module support in the microVM kernel. A workload needing arbitrary modules is a full-VM workload; the matrix says so.

---

# Appendix G — Competitive landscape & build-vs-adopt (surveyed August 2026)

A snapshot of the projects nearest to Hull's territory, and what each one means for the plan.

## G.1 Incus — the closest competitor

Incus is actively developed and healthy: monthly feature releases, **7.3 shipped July 2026**, **7.0.1 LTS supported to June 2031**, containers and VMs as first-class instance types with increasingly shared capabilities (SELinux confinement, nested virt, GPU sharing via virtio-gpu native-context), and it has grown **OCI application-container support** with static network config. Its `incus port-forward` and unified instance CLI overlap directly with Hull's pitch.

Where Hull still differentiates: Incus is a **system-daemon architecture** (incusd, typically root, group membership ≈ admin) rather than Hull's no-root-daemon, rootless-first model; its native image format is its own tarball/volume formats with OCI grafted on for app containers, not a plain-file CAS; its container/VM duality is *managed convergence* of two instance types rather than a single spec compiled through a tested capability matrix — behavior differences between the two are documented prose, not machine-checked contracts; and it has no microVM tier (its VMs are full QEMU boots). Verdict: Incus is the project to beat on breadth, and the strongest argument for keeping Hull's scope narrow — dev-machine UX, not cluster management. Anyone needing clustering/HA today should just use Incus, and the plan should say so.

## G.2 libkrun / crun-krun — validation of the vm backend

Podman can already run a container in a microVM by swapping the OCI runtime to `krun` (libkrun) — same image, same invocation, hardware isolation — which is precisely Hull's `(backend vm)` concept expressed at the OCI-runtime layer. Red Hat is pushing it for AI-workload isolation (RamaLama). Current state: early-stage, Linux-only, GPU enablement still in progress, and the isolation choice is a runtime *flag* rather than a first-class, capability-checked spec field — `podman exec` support in krun was, notably, a tracked issue rather than a given, which is exactly the class of drift Hull's conformance suite exists to prevent. Verdict: **adopt, don't fork** — evaluate libkrun as the vm driver's hypervisor library (it would replace bespoke QEMU-microvm orchestration and possibly the bundled-kernel burden, since libkrun ships its own), with cloud-hypervisor as fallback. This could materially shrink Phase 4's native-microVM work and de-risk §11.1.

## G.3 composefs / podman store — the tide is with us

The ecosystem is converging on Hull's storage direction: podman has an open proposal to make **composefs a firm dependency**, and bootc is building a **composefs-native backend** for bootable containers; Red Hat has moved podman/bootc/composefs toward CNCF stewardship. Verdict: strong validation of §3, plus a warning — if podman's store becomes composefs-native CAS too, Hull's store is less of a differentiator on its own. The durable differentiators remain the manifest-as-image model (no layers at rest), ownership classes, and the spec/matrix layer above the store.

## G.4 systemd-vmspawn / nspawn — watch, integrate, don't compete

vmspawn (boot the same directory tree/image as a VM that nspawn boots as a container) keeps maturing but is still catching up to nspawn feature-wise — e.g. ephemeral modes were still an open feature request in 2026. It's plumbing, not product: no image distribution, no build story, no capability contract. Verdict: no threat to the plan; the systemd generator (§9) should target compatibility with `machinectl` conventions so Hull machines appear alongside nspawn/vmspawn ones.

## G.5 Revised build-vs-adopt table

| Component | Decision (updated) |
|---|---|
| Low-level container runtime | adopt: crun |
| microVM hypervisor | **evaluate libkrun first**, then cloud-hypervisor, then QEMU-microvm; bundled-kernel pipeline (App. F) only if libkrun's kernel path is insufficient |
| Rootless networking | adopt: pasta |
| Image mounting | adopt: composefs/EROFS (ecosystem-aligned) |
| CAS + manifest model | **build** (core differentiator) |
| Spec + capability matrix + conformance suite | **build** (the product) |
| Guest agent + vsock protocol | build (small; no reusable equivalent with these exact semantics) |
| Clustering / HA / multi-host | **don't build** — defer to Incus, say so in docs |

## G.6 Positioning vs. podman — on paper

Podman is the strongest sibling, so the differentiation must be stated precisely. Where the ground is shared: rootless, no root daemon, OCI interop, systemd generation (quadlets), even microVM isolation via krun. Any pitch that ignores this loses.

Where Hull is a different *kind* of thing:

1. **Files-first vs. image-first.** Podman is Docker-shaped: the image/registry pipeline is the primary object and `--rootfs` is an edge case. Hull inverts this: the directory is the primary object, the store optional (§3.7–3.8). Podman cannot delete the registry from a CI pipeline; Hull's main workflow never had one.
2. **Machines vs. containers.** Podman runs Linux containers; `podman machine` exists to *host* containers, and krun is an isolation flag. Hull's unit is *the machine*, with containers as one engine class among several — VMware, VirtualBox, Hyper-V, QEMU-full, TCG cross-arch machines, and the retro/media tier are not on podman's map at all, nor is running *your workload itself* as a VM with identical UX.
3. **The contract vs. the flag.** Podman's behavior varies quietly across rootless/rootful, driver, and runtime; discovering the differences is the user's job. Hull's capability matrix + conformance suite make behavioral equivalence a *tested product feature* with plan-time refusals (§6). This is the deepest difference: Hull sells a guarantee, podman sells a toolset.
4. **Identity.** No podman equivalent of ingress identity (§8.1): stable per-machine address, DNS name, and SSH host key that survive backend *and host* switches.
5. **The decomposition law.** Nothing in podman's universe corresponds to §3.11/§3.13: foreign VM images, media, canonical packs, quirk patches, provable lossless import. `podman save` emits a non-canonical tarball; `hull pack` emits a content-addressed identity.
6. **Described formats.** Hull's formats and protocols are situ-described artifacts with generated accessors (§3.12); podman's are implementation-defined Go structs.

Where podman is honestly better today: maturity, Docker-CLI drop-in compatibility, Kubernetes YAML support (`play kube`), Red Hat backing, and a decade of production hardening. The strategic conclusion follows: **don't fight podman for "Docker replacement" — podman won that.** Hull competes one layer up, where podman doesn't play; podman is a *candidate adapter engine* inside Hull (§5.0 notes the variant), not the rival. The rivalry, if it ever comes, arrives when podman grows a spec layer — and Hull's defense is the conformance contract and the decomposition law, neither of which bolts onto an image-first architecture after the fact.

---

*Appendix G sources: Incus news (linuxcontainers.org), Incus 7.2 coverage (linuxiac.com), Red Hat Developer on libkrun/RamaLama, containers/podman#24829 (composefs), bootc-dev/bootc#1190 (composefs-native backend), crun#2090 (krun exec), systemd#39050 (vmspawn ephemeral), systemd-vmspawn(1).*

---

# Appendix H — Live migration across engines

**Scope:** migration here means **host-to-host** — moving a *running* machine from one physical machine to another, same backend on both ends. (Cross-backend conversion appears only as a closing aside in H.5; it is a separate and lesser concern.) Note the emu cross-arch property is a host-to-host statement: the two *hosts* may differ in architecture, because TCG guest state never references the host.

## H.1 The dividing principle

Migratability is determined by **where the machine's state lives**:

- **Self-contained engines** (vm, emu, DOSBox, wasm): the entire machine state — CPU, RAM, device models — lives inside the engine process. The engine can serialize everything it owns; migration is theoretically clean and limited only by engineering.
- **Kernel-entangled engines** (ns, chroot, Wine): the "machine" is a set of live host-kernel objects — task structs, sockets, futexes, fd tables, namespaces. The kernel does not export its internal state by design; CRIU reconstructs it from the outside, which is inherently incomplete and permanently best-effort.

## H.2 Migration classes

Three classes, declared per engine in its capability table and — crucially — **recomputed per machine** at plan time, because spec features can demote a machine's class (H.4):

- **live** — iterative pre-copy/post-copy; brownout of milliseconds, connections survive.
- **warm** — stop-copy: pause, serialize, transfer, resume. Downtime proportional to state size.
- **pinned** — cannot leave its host (or can never migrate at all).

## H.3 Verdict per engine

| Engine | Class | Mechanism | Honest caveats |
|---|---|---|---|
| **vm** | **live** (the flagship) | QEMU/cloud-hypervisor pre-copy + dirty-page tracking; mature, decades of production use | virtiofs is the historical weak point — virtiofsd migration support is recent and partial, and is the main engineering risk for Hull's architecture; vsock control connections break (agent reconnects by design); needs compatible machine type + CPU model on both ends |
| **emu** | **live**, plus a unique trick | same QEMU migration machinery — it is largely accelerator-independent | slower dirty-page convergence (TCG overhead). The trick: TCG guest state is **host-arch-independent** — an emu machine can theoretically live-migrate from an x86 host to an ARM host mid-execution |
| **ns** | **warm**, best-effort | CRIU (+ userfaultfd lazy pages for live-ish post-copy); pasta/passt has grown TCP connection migration (driven by KubeVirt) | the caveat list is long and permanent: io_uring, open devices, GPU, some socket states, kernel version/feature parity between hosts; rootless CRIU is further restricted — checkpoint may need a privileged helper, in tension with Hull's rootless-first stance |
| **chroot** | worse than ns | CRIU with *more* host entanglement (no namespace boundary to cut along) | practically: don't |
| **DOSBox** | **warm that feels live** | save-states (DOSBox-X): total machine state is megabytes, so stop-copy completes in well under a second | vanilla DOSBox lacks save-states (engine choice matters); save-state formats are version-fragile — pin the engine version like a projector version |
| **Wine** | **pinned, effectively never** | — | a Wine app is host processes entangled with the host kernel *and* the display server and GPU state; no serialization boundary exists to cut along |
| **wasm** | **live/warm, cleanest of all** | linear memory + stack snapshot; designed-in host independence | cross-arch migration is trivial by construction — wasm shares emu's host-arch-independence without the speed penalty |

Summary of "who can never": nothing is *categorically* never at the whole-engine level except Wine-style engines whose state has no boundary — but any machine on any engine becomes **pinned** the moment it entangles itself with unserializable host state (H.4).

## H.4 Migratability is computed, not assumed

The matrix machinery (§6) applies directly: migration class is a derived property of `engine ∩ features used`. Examples: **device passthrough pins any machine on any hypervisor** — VFIO state cannot be serialized (this is true in every virtualization stack, not a Hull limitation); host networking pins an ns machine; `(fidelity odirect)` with a local block projection requires storage migration or shared storage. `hull plan --explain` therefore answers, per machine, before it ever runs: *live / warm / pinned, and why* — the same honesty contract as everything else, extended to mobility.

## H.5 What Hull's existing design contributes

- **Ingress identity (§8.1) makes migration invisible.** The machine's name, address, and SSH host key belong to the machine, not the host or backend. Cross-host migration re-points the stub resolver; clients reconnect to the same identity. The mechanism built for backend switching covers host switching unchanged.
- **Dir sources migrate by rsync.** Pre-sync the tree while the machine runs, pause, final delta (small, by rsync's nature), resume on the target — the same stop-copy shape as RAM, at file granularity. The CAS store helps when present: content-addressed pre-seeding means the target likely already has most objects.
- **The agent expects disconnection.** The vsock control plane already defines `on_disconnect` semantics (B.4); migration adds a `MIGRATING` state so the agent holds workload state through the reconnect rather than tearing down.
- **Cross-backend migration** gets a defined meaning: vm↔emu with the same guest arch is theoretically supported by QEMU's accel-independent migration stream, *if* the machine uses a constrained "migratable" CPU profile (no host-specific CPU features) — declared in the spec, checked in the plan. This makes **emu the universal migration destination**: any vm machine on a dying x86 host can theoretically land on whatever hardware exists, at emulation speed, while a replacement is found. ns→vm via CRIU-checkpoint-into-an-identical-guest-kernel is a theoretical curiosity worth an experiment, not a promise.

## H.6 Design consequences (v1 scope)

Migration is **not** in the v1 roadmap phases — but three cheap provisions keep the door open: the capability declaration includes a migration-class field from day one; the agent protocol reserves the `MIGRATING` state; and the constrained migratable CPU profile is defined in the spec schema even before anything consumes it. Retrofitting these later would touch every engine; declaring them now costs a page of schema.

---

# Appendix I — Dual licensing & the Enterprise perimeter

*(Business/legal planning, not legal advice — have a lawyer review the final scheme.)*

## I.1 The perimeter rule

Feature lists drift; rules hold. Two rules define the paywall, permanently:

1. **One host, free.** Anything that runs on a single host — every engine, every workflow — is free. Paid features are those whose *value is coordination*: many hosts, or many people.
2. **Bytes in, bytes out — never the product.** Import, export, materialize, verify, and every projector are free forever. Hull's ethos is that your files are the truth; charging to access your own data would convert the decomposition law into hostage-taking and destroy the project's trust foundation.

These two rules protect the named constituencies by construction, not by curation: **development and testing** live entirely on one host (all engines, dir sources, builds, `hull check`, conformance suite, SSH/ingress, systemd integration — free); **nostalgia and preservation** needs the media projectors, quirk machinery, DOSBox-class engines, and import/export (free under rule 2, and strategically so — the retro/preservation community runs on goodwill and is the project's most vocal evangelist base).

## I.2 Free forever (Community)

All engines and the plugin API (third parties write engines without paying — the ecosystem must grow unmetered); dir/tar/media sources and all projectors; the store, builds, OCI pull/push; ingress identity, SSH, multi-machine segments on one host; the CLI, TUI, and GUI managing your own hosts (small N, single user); **warm migration** (`hull move`: stop → rsync → start — it's just files; gating it is both wrong and unenforceable); import/export including bootable export; the conformance suite.

## I.3 Enterprise (paid subscription)

Picked so each is many-hosts/many-people by nature — the vSphere playbook, minus its mistakes:

1. **Live migration orchestration** (the vMotion analog) — pre-copy zero-downtime moves, H.3's flagship. Devs stop-and-restart; fleets can't.
2. **Fleet control plane** — multi-host inventory, placement/scheduling, HA restart on another host, rolling updates across a fleet (the DRS/HA analog).
3. **Team management** — RBAC, SSO/OIDC/LDAP, audit trails, the GUI's "500 hosts, 40 operators" mode. (Solo remote management of your own machines stays free — the shared encrypted protocol already does remote; gating *basic* remote would hit individuals.)
4. **Policy & compliance at scale** — org-wide signing-policy distribution, attestation reporting, fleet-wide CVE exposure reports computed from manifests.
5. **Fleet storage services** — coordinated CAS mirroring / peer-to-peer seeding, scheduled snapshot & backup orchestration with retention policies.
6. **Certified channel & support** — SLA support, certified builds, extended-life guest-kernel stream with backports (the Red Hat axis: same bits, paid assurance).

## I.4 Mechanics

- **The plugin ABI is the license boundary.** Enterprise features ship as separate plugins against the same stable C plugin API that engines use — with **source published** under the scheme in I.5, not as closed binaries. The open core contains **no license checks, no feature flags, no crippleware** — community builds are complete programs, not demos. This keeps the boundary technically honest and the community build auditable.
- **The perimeter is a public covenant.** Publish rules I.1 as a documented promise: features inside the free perimeter never move behind the paywall. Every open-core scandal of the last decade (Docker Desktop's retroactive licensing, HashiCorp's BSL flip → the OpenTofu fork, Broadcom killing VMware's free tier → the mass homelab exodus) was a *perimeter-moved-after-adoption* event. The covenant is the anti-Broadcom clause, and it is also marketing.
- **Homelab allowance.** Enterprise features free up to ~3 hosts, non-commercial. VMware's homelab users were its evangelist pipeline for twenty years; Broadcom's severing of it is the cautionary tale. The overlap between homelabbers and the retro community makes this doubly cheap goodwill.
- **License choice.** Core: **Apache-2.0** recommended — maximal adoption for the spec, formats, and situ descriptions, which benefit from becoming de-facto standards; patent grant included. Alternatives considered: AGPL + commercial dual-license (Qt's model — stronger sales lever, weaker adoption; plausible given the project's Qt lineage) and MPL-2.0 (file-level middle ground). If cloud-vendor freeriding ever materializes, the answer is competing on the enterprise layer, not relicensing the core — see covenant.
- **CLA required** for core contributions (dual licensing requires copyright aggregation); DCO alone is insufficient. Trademark ("Hull") held by the company — the Docker Inc. lesson: the mark, not the code, is the durable commercial asset.
- **Qt note:** the GUI links Qt under LGPLv3 (dynamic linking; fine for both the open GUI and source-available enterprise plugins) unless a commercial Qt license is preferred for static linking or App Store distribution.

## I.5 The enterprise layer is source-available, not binary-only

Binary-only components are a bane for exactly the users Hull courts: they can't be audited, patched, rebuilt for odd platforms, or trusted to outlive the vendor. The enterprise layer therefore publishes its source under a **noncompete source-available license** — semi-free by design:

- **Model: BUSL-style with an FSL-simplified grant.** Each enterprise release's source is public from day one. The license grants: use, modification, and redistribution for any purpose **except** (a) production use of the fleet features beyond the homelab allowance without a subscription, and (b) offering the code, modified or not, as a competing fleet-management product or managed service. Everything else — reading, auditing, patching your own deployment, rebuilding from source, porting to a weird platform — is expressly permitted.
- **The change date is the anti-stranding clause.** Every enterprise release automatically converts to Apache-2.0 N years after its release (2–4; pick once, publish, never extend). The closed surface is thus *permanently bounded*: today's enterprise code is tomorrow's free code on a fixed schedule, users can always see what they will eventually own, and vendor death strands no one. This is the property that answers the binary-blob objection outright.
- **Enforcement is legal, not technical.** Because the restriction lives in the license text, there is still no DRM, no phone-home, no license-key checks in code — builds stay reproducible and the honest-boundary principle of I.4 survives intact. Subscriptions are enforced the way BUSL vendors and JetBrains-style honor models enforce them: contract and audit for organizations, goodwill for individuals.
- **The honest limit, stated plainly:** a noncompete license protects the enterprise *code*, not the feature *space*. Nothing stops a third party who never reads that code from clean-room-building their own fleet manager on top of the Apache core's plugin API — and fencing the feature space would require BUSL-ing the core itself, which is the HashiCorp move and invites the OpenTofu fork. The moat for the feature space is execution speed, the covenant's goodwill, and the change-date bargain — not law. Accept this openly; it is also what keeps the core's Apache promise credible.
- Candidate texts to draft from: **FSL** (cleanest, shortest), **BUSL 1.1** (most precedented, parameterized), **Elastic License 2.0** (no conversion — rejected here: the change date is the trust mechanism), **PolyForm Shield** (noncompete only, no production gate). Final drafting with counsel. Precedent status (checked Aug 2026): no noncompete source-available grant has been squarely tested in court; the nearest data point, *Neo4j v. PureThink/Suhy*, favors licensors but concerns stripping a bolted-on restriction — a risk a standalone license text avoids entirely.

---

# Appendix J — `hull.conf` schema reference (DRAFT v0.1)

The complete key inventory for §4, as designed, in the syntax of §4.1. The authoritative
machine-readable schema will be a situ description generated from this draft. Canonical
spec-field ids (used by capability tables and `--accept` tokens) are the dotted paths shown
in the left columns.

Single-machine files use `(machine …)`; multi-machine files use `(machines (<name> …) …)`
with the same schema per machine (machines in one file form a segment, Appendix D). A
`(build …)` form may live in the same file or in `hull.build.conf`.

## J.1 `(machine …)` — core keys

| Key | Type | Default | Notes |
|---|---|---|---|
| `source` | string | *required* | `"dir:<path>"` (§3.7, primary) · `"tar:<path>"` · `"pack:<path>"` (§3.13) · `"image:<path>"` foreign attach (§3.13) · bare string = image ref in store/registry (§3.5) |
| `mutable` | `direct` \| `overlay` | `direct` for `dir:`, else `overlay` | `direct` writes to the source tree (bind/passthrough only — block projection rejects without `writeback`, §3.9) |
| `writeback` | `none` \| `sync-on-stop` | `none` | Only meaningful with block projection + `direct`; documents a crash window (§3.9 rule 2) |
| `projection` | `auto` \| `bind` \| `passthrough` \| `block` | `auto` | Planner chooses per engine unless forced (§3.9) |
| `fidelity` | symbols | empty | Declared semantic needs: `inotify`, `flock`, `odirect`, … — planner must satisfy every entry or reject (§3.9 rule 3) |
| `arch` | symbol | `host` | `amd64`, `arm64`, `riscv64`, … Foreign arch ⇒ emu class, or ns with `(binfmt true)` (§5.3) |
| `command` | strings | manifest `RuntimeDefaults.cmd` | Argv; no shell. `entrypoint` semantics come from the manifest (A.4) |
| `workdir` | string | manifest default or `/` | |
| `env` | `(NAME "value")` entries | empty | Merged over manifest env |
| `env_file` | string | — | dotenv-format file; gitignored secrets belong in `(secrets …)` instead |
| `ports` | `(port …)` entries | empty | `(port (host N) (guest N) (proto tcp\|udp))`; bound on the machine's stable ingress IP (§8.1) |
| `volumes` | `(volume …)` entries | empty | `(volume (host "<path>") (guest "<path>") (mode rw\|ro))` |
| `cpus` | integer | engine default | ns: cgroup weight/quota · vm/emu: vCPUs |
| `memory` | string | engine default | `"2G"`; ns: cgroup ceiling · vm/emu: hard allocation (§6) |
| `backend` | symbol | `ns` | Class (`ns`/`vm`/`emu` → host default engine of class) or engine (`docker`, `qemu`, `virtualbox`, `vmware`, reserved: `hyperv`, `dosbox`, …) (§5.0) |
| `ssh` | boolean | `false` | Provision + supervise minimal sshd with user's authorized_keys, all backends (§8.1.3) |
| `restart` | `no` \| `on-failure` \| `always` | `no` | Consumed by the systemd generator (§9), not by a daemon |
| `on_disconnect` | `stop` \| `keep` | `stop` | Machine behavior if the core/CLI dies (B.4); `keep` requires later `hull attach` |
| `hostname` | string | machine name | Also the ingress DNS label `<name>.hull` (§8.1) |

## J.2 `(security …)` (§7)

| Key | Type | Default | Notes |
|---|---|---|---|
| `readonly_root` | boolean | `false` | |
| `no_new_privs` | boolean | `true` | ns class |
| `allow` | symbols | empty | Additive: capability grants (ns) / device passthrough ids (vm). There is deliberately no `privileged` key — the vm class is the escape hatch |

## J.3 `(secrets …)` (Appendix C)

Entries `(<name> (source "…") (expose env))`, `expose` optional. `source`: `"file:<path>"` ·
`"age:<path>"` · `"exec:<argv>"`. Materialized to machine-private tmpfs
`/run/hull/secrets/<name>` (mode 0400) on every backend; excluded from `hull commit` by
construction. `(expose env)` opts one secret into env injection (warned).

## J.4 `(media …)` (§3.10, §3.13)

Entries are `(medium …)`:

| Key | Type | Notes |
|---|---|---|
| `type` | symbol | `cdrom`, `floppy`, `disk`, … — engine must list it in `media_types` |
| `source` | string | `dir:` collection (files + `media.conf` + `quirks/`), `tar:`, `pack:`, or foreign `image:` |

## J.5 `(tuning …)` — class- and engine-level tuning (§4)

Entries may be named for a **class** (applies to any engine of that class) or a **concrete
engine** (overrides the class entry). Non-applicable entries warn.

| Entry.Key | Type | Default | Notes |
|---|---|---|---|
| `vm.profile` | `micro` \| `full` | `micro` | micro: qemu & native engines only; virtualbox/vmware/hyperv are full-only (§5.2) |
| `vm.kernel` | `bundled` \| `image` \| `"path:<file>"` | `bundled` | Appendix F |
| `vm.cpu_profile` | `host` \| `migratable` | `host` | `migratable` = constrained model, enables vm↔emu/live migration (H.5/H.6) |
| `qemu.accel` | `auto` \| `kvm` \| `tcg` | `auto` | `tcg` ⇒ emu class semantics |
| `emu.cores_scale` | number | `1` | vCPU over-provision to offset TCG cost (§5.3) |
| `ns.binfmt` | boolean | `false` | qemu-user for foreign-arch sources; matrix: emulated (§5.3) |
| `virtualbox.gui` | boolean | `false` | |
| `docker.*` / `vmware.*` / `hyperv.*` | — | — | Reserved; defined with each adapter |

## J.6 `(build …)` and its steps (§3.6, Appendix E)

`(build (base …) (steps (step …) …))`; the `step.` keys below are each step's entries.

| Key | Type | Default | Notes |
|---|---|---|---|
| `build.base` | string | *required* | Image ref or `dir:` |
| `step.name` | string | *required* | |
| `step.run` | string | — | Shell line, run in the step sandbox |
| `step.copy` | `(path (from …) (to …))` entries | empty | Declared inputs, content-hashed before mount |
| `step.inputs` | strings | empty | Extra cache-key inputs (paths, content-hashed) |
| `step.network` | boolean | `false` | `true` demotes cache to time-bucketed (`refresh`) |
| `step.refresh` | duration string | `"7d"` | Only with `(network true)` |
| `step.backend` | symbol | `ns` | Per-step engine (module-building steps: vm) |
| `step.discard` | strings | empty | Paths dropped at commit |

## J.7 Multi-machine files (Appendix D)

`(machines (<name> …) …)`; all machines in one file share a segment: sibling DNS by name,
Hull stub resolver, only `ports`-declared endpoints reachable from the host. Mixed
`backend` per machine is supported and is the point. Compose and Vagrant dialects (§9)
compile onto exactly this schema; dialects never add keys this schema lacks.

## J.8 Validation model

Every key above has a canonical field id; engines publish support per id
(native/emulated/rejected — §6). Unknown keys are **errors**, not warnings. Unknown
*values* of known keys are rejections with the engine's declared reason. `hull plan
--explain` prints the resolution of every key for the selected backend; `hull check
--backend a,b,c` is the CI gate.

---

## Handoff notes (for the implementation session)

This document was drafted in planning-only mode on mobile. When this project moves into Claude Code with repo access, the pending work, in order:

1. **Read the situ repo.** Judge actual capabilities and syntax; then rewrite Appendix A (manifest canonical encoding) and Appendix B (agent protocol) as situ descriptions if expressible — checking specifically whether deterministic canonical encoding (sorted keys, definite lengths) is expressible in situ as-is or needs an extension. Validate §3.12's assumptions. The `.hull` pack format and the in-fs collection format (§3.13) are the other two situ-first specs.
2. **Ingest the project guidelines** (not yet received here): Qt/Widgets GUI, TUI, C core server, shared encrypted protocol. §2 and §7 already sketch the frontend/core split (per-user, unprivileged, socket-activated core; CLI/TUI/GUI as peer frontends); finalize it against the real guidelines and reconcile the shared protocol with Appendix B — likely: shared protocol frontend↔core, minimal vsock protocol core↔guest-agent.
3. **Review the two Phase 1 artifact drafts** `hull_engine.h` and Appendix J: the C plugin ABI header (engine interface + capability declaration, §5.0) and the complete hull.conf schema reference (§4, folded in as Appendix J). Both are marked DRAFT — regenerate their encodings/ids from situ once step 1 lands.
4. Then the Phase 1 build order (§12) applies as written: core + docker & qemu engines first.
