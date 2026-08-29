# WS013 Phase 001: CPAR architecture discussion

Last updated: 2026-08-27

WSID: `ws013`

Phase ID: `p001`

Combined ID: `ws013-p001`

Status: Proposed

Parent: [WS013](../ws.md)

Reviews: [WS013 review index](../tests/README.md)

## Objective

Turn Boot CPAR, Runtime CPAR, and service-container ideas into a simple
file-oriented boot, security, and image architecture that can be decomposed
into bounded implementation Phases.

## Confirmed baseline

- zedBSD boots from immutable `rootfs.img` plus a writable data image and has a
  native overlay filesystem.
- q015 implements the common bounded boot-parameter parser, kernel-owned x86
  handoffs on all four production loader paths, four private boot slots,
  native/overlay root selection, up to four file or raw swap sources,
  `ZEDSWAP2`, and `init=`.
- The current mount tree is global; process cwd/root objects do not yet amount
  to a private mount namespace or public chroot/container contract.
- File-backed loop attachment is kernel-internal, limited to eight devices,
  and rejects unsafe recursive backing including files visible through the
  overlay filesystem.
- Current overlay composition is one lower plus one upper and rejects nested
  overlays; the fixed Runtime CPAR contract needs two read-only image roles and
  writable directory mounts, which the current mechanism cannot yet express.

## Fixed product decisions

- CPAR means container partitioning. Boot CPAR and Runtime CPAR are separate
  modes with a common base/app/data vocabulary.
- Boot CPAR exists to make boot environments ordinary files. It deliberately
  avoids requiring partition manipulation or a ZFS-style administration model.
- UEFI FAT boot support expands from the current layout to FAT32 and VFAT long
  filenames. Versioned root images, named data images, and the shared swap file
  remain directly visible and editable.
- Legacy i386 PC/AT and PC-98 retain FAT16, the fixed legacy `boot.cfg`, and
  fixed image names. They do not implement CPAR Boot Menu.
- `/boot/boot.cfg` contains a timed default plus named menu sections. Each
  section supplies either native `rootpart` or overlay `rootfs` plus `datafs`,
  and may supply `swap`. No separate CPAR manifest is introduced.
- The initial menu selects one complete named section. It does not perform
  semantic version comparison or independently compose kernel/root/data at
  the keyboard. The explicit `default` key is authoritative.
- The UEFI kernel filename remains the existing fixed `VMUNIX.X64` in the
  initial Boot CPAR grammar. Versioned-kernel selection is outside v1 rather
  than inferred from section titles.
- There is no `cpar boot` command family. A running system administers Boot
  CPAR by ordinary file operations and direct `boot.cfg` editing under `/boot`.
- File-backed image mounting remains a VFS abstraction; no public loop-device
  UAPI is introduced for CPAR.
- Runtime CPAR composes one read-only base image and one read-only app image.
  Only explicitly mounted application-data directories are writable; there is
  no general writable root layer in the initial model.
- `cpar` owns Runtime CPAR execution. `cpar build` is a later image-production
  feature and keeps the logical model at two image layers rather than Docker's
  arbitrary-depth layer history.
- Containers are recommended for suitable non-base services but are not
  mandatory. sshd is explicitly deferred until home-directory and session
  mount semantics are designed.
- Runtime namespace/security design, Runtime CLI/build grammar, and
  service-container packaging are manually blocked until explicitly resumed.

## Boot CPAR `boot.cfg` v1

The accepted initial syntax is:

```ini
timeout=5
default=zedBSD 1.0.0

[zedBSD 1.0.0]
rootfs=boot0:rootfs-1.0.0.img
datafs=boot0:datafs-1.0.0.img
swap=boot0:swapfile

[zedBSD 0.8.0]
rootpart=/dev/nvme0n1p2
swap=boot0:swapfile
```

The global keys are only `timeout` in seconds and `default`, whose value must
exactly name one section. A section title is the displayed menu title and must
be unique. Each section has exactly one root mode: either one `rootpart`, or one
`rootfs` plus one `datafs`; mixing or omitting a required root key fails.
`swap` is optional and may occur once. Unknown or duplicate keys fail visibly.
`bootN:NAME` names a long filename on the corresponding implemented
boot-filesystem slot. Native roots reuse the kernel's `rootpart=` contract
rather than being overloaded into `datafs`.

The UEFI menu highlights `default`, counts down `timeout`, and boots it unless
the user selects another complete section. It does not sort versions, infer a
matching root, or expose an independent file-picker in v1. Missing `boot.cfg`
keeps the current fixed `rootfs.img`/`data.img`/`swapfile` behavior. A present
but invalid file stops with a visible configuration error rather than silently
booting a different environment.

Selection uses the implemented common textual boot-parameter model. The loader
translates an overlay section into `overlay-root=`, `overlay-data=`, and an
optional `swap0=`; it translates a native section into `rootpart=` and an
optional `swap0=`. `boot0` denotes the loader-origin FAT filesystem by default,
and `boot1`--`boot3` may name additional FAT filesystems. No CPAR-specific
handoff structure or ABI registry is introduced. The authoritative parameter
contract is
[kernel-boot-parameters.md](../../../docs/reference/kernel-boot-parameters.md).

q015 closed the earlier common-kernel and transport gap: the parser, all four
x86 transports, boot/root selection, configurable root/data paths,
four-source file/raw swap, and init selection are implemented. Boot CPAR
reuses that textual channel rather than creating a CPAR handoff format. Its
remaining bootloader work is UEFI FAT32/VFAT long-filename access, a bounded
`boot.cfg` parser and menu/default/timeout implementation, and translation
of the selected complete section into the common parameter string.

## Scope

- Boot CPAR file naming, discovery, the fixed v1 `boot.cfg` grammar, menu,
  timeout, textual boot-parameter selection, and legacy-layout fallback;
- future minimum Runtime CPAR isolation required before the word “container”
  is used;
- initial host-network profile versus later network namespace;
- process/prison identity and visibility, signals, credentials, and devices;
- private mount namespace and automatic mount/image teardown;
- two read-only image roles and declared writable data-directory mounts;
- persistent image store, per-instance config/data, manifests, hashes, updates,
  rollback, and garbage collection;
- init execution path, FD 3 readiness, stop/restart, and crash cleanup;
- `DESTDIR` staging, UFS payload image, configuration tarball, dependency
  closure, licenses, and menuconfig integration;
- Runtime CPAR command surface and later `cpar build` input format.

## Non-goals

- implementing kernel or userland code in this Phase;
- defining a `cpar boot` command or a second boot manifest alongside
  `boot.cfg`;
- adding boot-environment ABI management;
- promising cgroups, user namespaces, or private networking in the first
  runtime without a separately accepted design;
- treating a globally mounted chroot as equivalent to a jail/container;
- choosing OCI compatibility.

## Future Runtime starting point (manual hold)

- Private mount namespace, restricted root, process/signal boundary, and
  restricted devfs are candidates for the initial mandatory isolation set.
- Shared host networking can be an explicit initial profile; cgroup-style
  resource control can be later.
- The base/app composite remains read-only. `/run` and `/tmp` use tmpfs,
  config uses read-only binds, and only declared data paths use writable binds.
- PID 1 should supervise the eventual service process through a small native
  runtime rather than learning image/mount mechanics itself.

## Open Boot implementation details

- FAT32/VFAT long-filename lookup, including bounded name/path lengths,
  normalization, case matching, malformed-chain handling, and media-read
  failure behavior.
- A bounded `boot.cfg` parser, including file/line/token limits, exact
  duplicate/unknown/malformed-key errors, and missing-versus-invalid
  configuration behavior.
- The smallest keyboard menu controls, default/timeout behavior, and visible
  parse/error presentation.
- Exact translation of one selected complete native or overlay section to the
  corresponding `rootpart=` or `overlay-root=`/`overlay-data=` contract plus
  optional `swap0=`, without merging hidden defaults into a present section.

## Manually blocked design topics

- Runtime CPAR mandatory namespaces, security claim, process/device/network
  visibility, and the VFS mechanism for the fixed two-image model.
- Runtime image store, recursion boundary, instance/config/data lifecycle, and
  failure cleanup.
- `cpar run`, `cpar sh`, and `cpar build` grammar, writable-path mapping,
  reproducibility, and update behavior.
- Service-container package boundaries, dependency closure/deduplication,
  configuration upgrade/merge, persistent data ownership, service-definition
  keys, readiness forwarding, and secret handling.

These topics correspond to `MB-002`--`MB-004` in the master plan and are not
part of the active discussion agenda until explicitly resumed.

## Work packages

- [ ] Manual hold `MB-002`: resolve the minimum isolation/security claim and
      threat model.
- [x] Resolve the Boot CPAR v1 grammar, complete-section menu behavior,
      legacy fallback, and textual parameter model.
- [x] Record the q015 common parser, four-x86 handoff, boot/root, swap, and
      init foundation that Boot CPAR will reuse.
- [ ] Resolve the bounded Boot CPAR implementation details above.
- [ ] Manual hold `MB-002`: resolve the two-image root mechanism, writable data
      paths, and image lifetimes.
- [ ] Manual hold `MB-002`: resolve service/runtime/PID 1 ownership and failure
      cleanup.
- [ ] Manual hold `MB-004`: resolve package artifacts, dependency closure,
      updates, and rollback.
- [ ] Manual hold `MB-004`: resolve configuration/data binding and
      secret-handling rules.
- [ ] Define host, focused-kernel, QEMU, and later hardware acceptance classes.
- [ ] Split UEFI Boot CPAR implementation into ordered loader/configuration and
      acceptance Phases; split Runtime/package Phases only after their holds.
- [ ] Synchronize WS013, master dependencies, and WS009 documentation handoffs.

## Acceptance

All review cases in the [WS013 review index](../tests/README.md) have explicit
answers, including failure and cleanup behavior. No implementation or security
claim is accepted merely from a successful chroot demonstration.

## Actual results and evidence

q015 establishes the common boot-parameter, four-x86 handoff, private
boot-slot, root-mode, multi-source swap, and init-selection foundation that
Boot CPAR needs. It does not implement Boot CPAR's FAT32/VFAT LFN lookup,
`boot.cfg` parser, menu, or selected-section translation. The initial source
audit also proves that private mount namespaces, public dynamic image
attachment, and the requested Runtime CPAR multi-layer composition are not
current features. Discussion remains open.

## Interruption / resumption

Resume the bounded UEFI FAT32/VFAT LFN, `boot.cfg` parser, menu, and
selected-section translation details, then split Boot CPAR implementation
Phases. Runtime namespace, Runtime CLI/build, and service-container package
discussion remain on manual hold and must not enter a Queue until explicitly
resumed.

## Remaining debt and handoff

Every CPAR-specific implementation and executable fixture, plus every package
conversion, service integration, and user-facing command, remains a later
extracted Phase. The q015 common boot foundation is already implemented.
