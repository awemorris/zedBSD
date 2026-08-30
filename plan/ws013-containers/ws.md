# WS013: CPAR container partitioning

Last updated: 2026-08-30

WSID: `ws013`

Status: Active; q031 UEFI path complete, BIOS p005-p006 planned, Runtime
topics manually blocked

Parent: [master plan](../master.md)

Last verified Phase: `ws013-p004`; p001 remains the architecture discussion
ledger

Resume point: place the separately bounded PC/AT p005 and PC-98 p006
configuration-unification work in a later Queue. The common four-x86
boot-parameter foundation was implemented by q015, and q031 completed p002,
p003, and p004.
Runtime CPAR namespace, CLI/build, and service-package topics remain on the
manual blocking register.

Shared reviews: [WS013 review index](tests/README.md)

## Goals

- Make kernels, immutable roots, applications, and writable environments
  ordinary files that can be copied, inspected, selected, and rolled back
  without repartitioning or requiring a ZFS administration model.
- Give `BOOTX64.EFI` one simple, required `/zedbsd.cfg`: discover the
  configuration-bearing FAT on the boot disk, load its configured kernel, and
  pass its remaining lines through the common kernel-parameter contract.
- Make the i386 PC/AT PBR/`BOOTZBSD.EXE` path read `/zedbsd.cfg` and implement
  exactly the UEFI p003 configuration format and parameter result.
- Make amd64 BIOS boot through the payload PBR/`BOOTZBSD.EXE`, read that same
  `/zedbsd.cfg`, and retire its separate fixed-name/direct-kernel policy.
- Make PC-98 `BOOTZBSD.EXE` read `/BOOTZBSD.CFG` and implement the same p003
  format and parameter result; only the legacy filename differs.
- Provide Runtime CPAR containers composed from a read-only base image, a
  read-only application image, and explicitly writable data directories.
- Allow selected `/etc/service.d/` services and service-container packages to
  use Runtime CPAR while preserving traditional base and package services.
- Add a later `cpar build` path for producing application images.

## Objective

CPAR applies the UNIX principle that system objects should be manageable as
files. Disk partitions sit below filesystems and are consequently awkward for
a running UNIX system to copy, name, inspect, and reorganize. The initial UEFI
work makes the kernel, immutable root image, writable data image, and swap file
ordinary files selected by one directly editable configuration. A menu for
choosing among several Boot CPAR environments is deliberately deferred.

CPAR means container partitioning. Boot CPAR will eventually select complete
boot environments, while Runtime CPAR constructs a namespace-isolated process
environment under the currently running kernel. Both use the same simple
image-and-data vocabulary, but boot management remains direct `/boot` file
administration rather than a `cpar boot` command family.

This WS designs the bootloader, isolation, image, mount, package, lifecycle,
and service-integration contracts. q015 has already supplied the common
kernel parser, x86 handoffs, boot slots, root selection, swap sources, and init
selection that the UEFI work consumes. p002 and p003 now bound the initial
single-configuration UEFI implementation, p004 audits obsolete boot-path
source after that path is established, and p001 retains the architecture
record and manually blocked Runtime discussion. The older p001 Boot menu notes
are historical; this W book and p002-p004 are authoritative for the revised
UEFI contract. p005 and p006 then extend the frozen language to PC/AT BIOS and
PC-98 `BOOTZBSD.EXE` without expanding q031.

## Scope

- UEFI discovery of FAT16/FAT32 filesystems on the same physical disk as the
  loaded image and deterministic selection by a readable root
  `/zedbsd.cfg`;
- one bounded `/zedbsd.cfg` containing a required loader-only `kernel=` line
  plus ordinary textual kernel parameters;
- loading the named kernel from the selected configuration filesystem,
  synthesizing selected-FAT `boot0` identity when omitted, and expanding
  convenient relative overlay/data/swap file values;
- both the existing overlay mode and native-UFS `rootpart=` mode;
- an audit and removal Phase for confirmed dead boot-path source, beginning
  with the unused `src/kern/startup.c` path;
- i386 PC/AT BIOS `/zedbsd.cfg` loading through its active payload FAT PBR and
  `BOOTZBSD.EXE`;
- amd64 BIOS convergence from its reserved-area direct loader to the payload
  FAT PBR, `BOOTZBSD.EXE`, and `/zedbsd.cfg`, including one hybrid GPT/MBR
  image whose UEFI and BIOS paths select the same payload files;
- PC-98 `BOOTZBSD.CFG` loading with the same grammar and handoff result;
- process-root, mount, process-visibility, signal, device, IPC, and network
  isolation profiles for Runtime CPAR;
- a two-layer read-only Runtime CPAR root composed from base and application
  images, with writes allowed only through declared data-directory mounts;
- file-backed UFS image attachment, mount lifetime, teardown, and limits;
- service definition and `/sbin/init`/runtime ownership boundaries;
- staged package installation, manifests, dependency closure, menuconfig, and
  update/rollback behavior;
- interactive `cpar` execution and later `cpar build` behavior.

## Non-goals

- a Boot CPAR menu, named sections, timeout, default selection, file picker,
  or keyboard interaction in the initial UEFI configuration;
- using or merging UEFI `LoadOptions` as a kernel-parameter source;
- falling back to embedded parameters or a fixed kernel when `/zedbsd.cfg` is
  absent or invalid;
- implementing the planned p005/p006 BIOS changes as part of the currently
  authorized q031 UEFI work;
- giving PC-98 a second `ZEDBSD.CFG` fallback in addition to its requested
  `BOOTZBSD.CFG`;
- forcing containers on traditional services;
- providing a `cpar boot` administration command instead of direct `/boot`
  editing;
- boot-environment ABI registries or ABI-based combination enforcement;
- Docker image, OCI, Linux namespace, or cgroup compatibility;
- calling chroot alone a security boundary;
- importing an external base-system container implementation;
- requiring sshd to run inside CPAR before home-directory and login-session
  mounts have a satisfactory design;
- treating p002-p004 as authorization to resume manually blocked Runtime work.

## Dependencies

- q015 supplies the implemented four-x86 textual boot-parameter, private
  boot-slot, root-mode, swap-source, and init-selection foundation.
- UEFI Loaded Image, Device Path, Simple File System, and Block I/O protocols
  must expose enough information to prove same-physical-disk membership and
  validate FAT16/FAT32 candidates.
- VFS, overlayfs, loop/file-backed storage, process credentials/lifecycle,
  devfs, sockets, and resource accounting remain Runtime CPAR dependencies.
- [WS002](../ws002-services/ws.md) supplies init supervision and service
  definitions; [WS012](../ws012-service-console/ws.md) supplies administration.
- WS004 supplies durable storage drivers; WS005 supplies optional network
  integration; WS009 owns public container documentation.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws013-p001` | [CPAR architecture discussion](phase001-architecture-discussion/phase.md) | Proposed; Boot notes superseded, Runtime holds remain | Retain the architecture ledger and the manually blocked Runtime questions |
| `ws013-p002` | [UEFI `zedbsd.cfg` volume discovery](phase002-uefi-payload-discovery/phase.md) | Completed (`q031`) | Select the first same-disk FAT16/FAT32 containing `/zedbsd.cfg`, error on zero, and warn on later matches |
| `ws013-p003` | [UEFI `zedbsd.cfg` parsing and parameter assembly](phase003-uefi-boot-config-menu/phase.md) | Completed (`q031`) | Load the required configured kernel and emit one bounded common parameter string without a menu or `LoadOptions` |
| `ws013-p004` | [boot-path dead-source audit](phase004-boot-path-dead-source-audit/phase.md) | Completed (`q031`) | Remove `startup.c`, clean its remnants, and evidence-audit other boot-path sources before any further deletion |
| `ws013-p005` | [PC/AT BIOS `zedbsd.cfg` boot unification](phase005-pcat-bios-zedbsd-config/phase.md) | Planned; not in q031 | Independently prove i386 PBR/`BOOTZBSD.EXE` and amd64 PBR/`BOOTZBSD.EXE` consume p003 `/zedbsd.cfg`; make the hybrid UEFI/BIOS image share one payload FAT |
| `ws013-p006` | [PC-98 `BOOTZBSD.CFG` boot unification](phase006-pc98-bootzbsd-config/phase.md) | Planned; not in q031 | Make PC-98 `BOOTZBSD.EXE` use the p003 language under the PC-98-specific configuration filename |

## Confirmed product direction

- Container use is opt-in; ordinary BSD-style services remain supported.
- CPAR is the common product name; Boot CPAR and Runtime CPAR are distinct
  operating modes.
- The payload boot filesystem remains directly editable. Its root
  `/zedbsd.cfg` is the only initial UEFI configuration; no second CPAR
  manifest exists.
- `BOOTX64.EFI` searches only FAT16/FAT32 filesystems on the same physical disk
  as its loaded image. The loaded image filesystem is examined first, then
  other UEFI SimpleFS handles in the order returned by firmware, with the
  loaded handle de-duplicated.
- A candidate is a supported FAT with a readable root `/zedbsd.cfg`. Zero
  candidates is a visible fatal error. More than one produces a visible
  warning and the loader still uses the first candidate in the defined order.
  It never tries a later candidate because the first configuration is invalid.
- `/zedbsd.cfg` contains exactly one required `kernel=PATH` loader directive.
  The path is rooted at the selected FAT and cannot select another volume.
  `kernel=` is consumed by the loader and is not passed to the kernel.
- Every other non-empty line is one ordinary textual kernel parameter. The
  loader preserves its order and leaves semantic root/swap validation to the
  common kernel parser.
- If `boot0=` is absent, the loader prepends
  `boot0=UUID=<selected-FAT-volume-serial>`. An explicit `boot0=` is preserved
  and suppresses that synthesized token; no hidden duplicate is merged.
- A relative `overlay-root=`, `overlay-data=`, or `swapN=` file value may
  omit `boot0:`. The loader adds it. Existing `boot0:` through `boot3:`
  references remain unchanged. Unambiguous raw-swap selectors remain
  unchanged.
- Native UFS boot uses the existing `rootpart=` parameter directly. It is not
  translated through an overlay-specific key.
- UEFI `LoadOptions` is ignored. It neither overrides nor augments
  `/zedbsd.cfg`, and it is not a fallback when configuration fails.
- There are no sections, menu, default, timeout, version ordering, or keyboard
  controls in this revision. Supporting several selectable Boot CPAR
  environments is later work.
- No PC/AT `boot.cfg` reader exists today. The i386 PC/AT
  PBR/`BOOTZBSD.EXE` path uses a fixed kernel name and embedded parameter
  record. A later p005 replaces those inputs with root `/zedbsd.cfg`.
- The amd64 image already contains `BOOTZBSD.EXE`, but its BIOS entry currently
  uses a separate reserved-area Stage 2 which loads the kernel directly. p005
  changes the supported amd64 BIOS path to active payload FAT PBR ->
  `BOOTZBSD.EXE` -> `/zedbsd.cfg` and removes the direct-loader fallback.
  Its hybrid image separates the ESP from the following payload FAT and makes
  UEFI and BIOS select the same payload configuration.
- PC-98 later uses the same language through `BOOTZBSD.EXE`, but its requested
  root filename is `BOOTZBSD.CFG`. This filename difference is not a different
  parameter grammar.
- Runtime CPAR uses exactly two read-only image roles, `base` and `app`.
  Application state is written only through explicitly mounted writable data
  directories; the rest of the composed root remains read-only.
- A service-container package produces an immutable service payload including
  its dependency closure plus separately managed configuration and data.
- Configuration is mounted read-only by default; declared data paths are
  writable. `/run` and `/tmp` should be private runtime filesystems.
- `cpar` is a Runtime CPAR command. Boot environments are administered by
  editing files under `/boot`, not by `cpar boot` subcommands.
- A later `cpar build` command may construct an app image in a Docker-build-like
  workflow without adopting Docker's arbitrary-depth filesystem layer model.
- sshd containerization is deferred because host home-directory and login
  session mounting needs a separate design.
- Runtime CPAR namespace/security design, `cpar run`/`cpar sh`/`cpar build`,
  and service-container package design are manually blocked until explicitly
  resumed.
- `menuconfig` can select service-container packages under
  `userland/containers/`.

## UEFI `zedbsd.cfg` v1

The compact overlay form is:

```ini
kernel=vmunix
overlay-root=rootfs.img
overlay-data=data.img
swap0=swapfile
```

For a selected FAT whose volume serial is `6740-911D`, the loader emits:

```text
boot0=UUID=6740-911D overlay-root=boot0:rootfs.img \
overlay-data=boot0:data.img swap0=boot0:swapfile
```

The fully explicit form remains valid:

```ini
kernel=/kernels/vmunix
boot0=LABEL=ZEDBOOT
overlay-root=boot0:rootfs.img
overlay-data=boot0:data.img
swap0=boot0:swapfile
```

A native UFS configuration writes the common parameter directly:

```ini
kernel=vmunix
rootpart=PARTUUID=01234567-02
swap0=swapfile
```

`kernel=` is the sole loader-only key. Configuration lines otherwise use the
authoritative parameter vocabulary documented in
[kernel-boot-parameters.md](../../docs/reference/kernel-boot-parameters.md).
The loader performs only bounded syntax/path validation and the three stated
normalizations: consume `kernel=`, synthesize missing selected-volume
`boot0=`, and qualify relative overlay/data/swap file paths. It does not invent
a root mode, merge embedded defaults, or reinterpret `rootpart=`.

For `swapN=`, `/dev/NAME`, `UUID=`, `LABEL=`, `PARTUUID=`, and
`PARTLABEL=` are unambiguous raw-device selectors and are preserved. Any
other unqualified value is treated as a selected-FAT file and receives
`boot0:`. Consequently a raw device named only as `sda1` must be written
`/dev/sda1` in `zedbsd.cfg`; this removes the otherwise unavoidable
file-versus-device ambiguity.

## WS completion direction

The common q015 foundation is not a completion claim for this UEFI work. p002
owns same-disk FAT discovery, deterministic first-match behavior, diagnostics,
and selected-volume identity. p003 owns bounded `zedbsd.cfg` parsing, kernel
path loading, parameter assembly, absence/error behavior, and UEFI acceptance.
p004 then removes the confirmed unused startup path and records evidence for
every other boot-path source retained or removed. Later p005/p006 converge the
BIOS loaders on the same configuration language and exact handoff text.
Runtime completion conditions remain deferred until the security boundary is
honest and testable.

## Reconsideration boundaries

Reconsider the Boot plan if firmware cannot prove same-physical-disk
membership, FAT volume identity cannot be handed to the kernel without a new
ABI, or the direct one-configuration format cannot fit the existing 3071-byte
parameter record. Reconsider Runtime CPAR if safe teardown requires global
mounts, a writable image can be loop-backed recursively through itself, a host
service can escape through devices/process APIs, dependency licensing cannot
be represented, or the format cannot update and roll back atomically.
