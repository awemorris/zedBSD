# WS013: CPAR container partitioning

Last updated: 2026-08-27

WSID: `ws013`

Status: Proposed; q015 boot-parameter foundation implemented, CPAR work unqueued

Parent: [master plan](../master.md)

Last verified Phase: none

Resume point: complete the bounded UEFI FAT32/VFAT LFN, `boot.cfg`
parser, and menu-to-parameter translation details in `ws013-p001`, whose
Boot CPAR v1 grammar is now fixed. The common four-x86 boot-parameter
foundation was implemented by q015. Runtime CPAR namespace, CLI/build, and
service-package topics are on the manual blocking register.

Shared reviews: [WS013 review index](tests/README.md)

## Goals

- Make kernels, immutable roots, applications, and writable environments
  ordinary files that can be copied, inspected, selected, and rolled back
  without repartitioning or requiring a ZFS administration model.
- Provide Boot CPAR environments selected from the FAT boot filesystem through
  `boot.cfg` defaults or direct keyboard selection.
- Provide Runtime CPAR containers composed from a read-only base image, a
  read-only application image, and explicitly writable data directories.
- Allow selected `/etc/service.d/` services and service-container packages to
  use Runtime CPAR while preserving traditional base and package services.
- Add a later `cpar build` path for producing application images.

## Objective

CPAR applies the UNIX principle that system objects should be manageable as
files. Disk partitions sit below filesystems and are consequently awkward for
a running UNIX system to copy, name, inspect, and reorganize. Boot CPAR moves
the useful boot-environment choice above that boundary: kernels, immutable root
images, and writable data environments are files on the boot filesystem, so a
user can create, select, copy, and roll back environments with ordinary file
operations instead of a complex partition or ZFS command model.

CPAR means container partitioning. Boot CPAR selects a complete boot
environment, while Runtime CPAR constructs a namespace-isolated process
environment under the currently running kernel. Both use the same simple
image-and-data vocabulary, but boot management remains direct `/boot` file and
`boot.cfg` administration rather than a `cpar boot` command family.

This WS designs the bootloader, isolation, image, mount, package, lifecycle,
and service-integration contracts. q015 has already supplied the common
kernel parser, x86 handoffs, boot slots, root selection, swap sources, and
init selection that Boot CPAR will consume. The only current WS013 Phase is
architecture discussion; it does not authorize the remaining Boot CPAR or
Runtime CPAR implementation.

## Scope

- UEFI FAT16/FAT32 and VFAT long-filename boot discovery, the fixed `boot.cfg`
  v1 environment sections, timed default, and complete-section keyboard
  selection;
- Boot CPAR selection of either a native `rootpart` or a `rootfs-*.img` plus
  writable `datafs` image, and a shared swap source, while retaining the
  initial fixed `VMUNIX.X64` kernel name;
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

- forcing containers on traditional services;
- providing a `cpar boot` administration command instead of direct `/boot`
  editing;
- adding CPAR menus or long-name image selection to legacy i386 PC/AT or PC-98
  boot paths;
- boot-environment ABI registries or ABI-based combination enforcement;
- Docker image, OCI, Linux namespace, or cgroup compatibility;
- calling chroot alone a security boundary;
- importing an external base-system container implementation;
- requiring sshd to run inside CPAR before home-directory and login-session
  mounts have a satisfactory design;
- deciding implementation Phases before p001 resolves the architecture.

## Dependencies

- VFS, overlayfs, loop/file-backed storage, process credentials/lifecycle,
  devfs, sockets, and resource accounting in the kernel.
- q015 supplies the implemented four-x86 textual boot-parameter, private
  boot-slot, root-mode, swap-source, and init-selection foundation.
- [WS002](../ws002-services/ws.md) supplies init supervision and service
  definitions; [WS012](../ws012-service-console/ws.md) supplies administration.
- WS004 supplies durable storage drivers; WS005 supplies optional network
  integration; WS009 owns public container documentation.

## Phase registry

| Combined ID | Phase | Status | Required result |
| --- | --- | --- | --- |
| `ws013-p001` | [CPAR architecture discussion](phase001-architecture-discussion/phase.md) | Proposed | Freeze Boot CPAR behavior, Runtime CPAR isolation/composition, ownership, package artifacts, and the later Phase map |

No CPAR-specific implementation Phase is defined until `ws013-p001` is
complete.

## Confirmed product direction

- Container use is opt-in; ordinary BSD-style services remain supported.
- CPAR is the common product name; Boot CPAR and Runtime CPAR are distinct
  operating modes.
- The boot filesystem remains directly editable and is mounted as `/boot` in
  the running system. `boot.cfg`, not a separate CPAR manifest, records named
  or default kernel/root/data combinations.
- A user may interrupt the boot timeout and select one complete named
  `boot.cfg` section. The initial menu does not independently select files or
  infer compatibility. No ABI registry is required.
- `default=TITLE` selects the timed default exactly. The loader performs no
  version comparison or automatic newest-version inference.
- The UEFI bootloader is extended to FAT32 and VFAT long filenames instead of
  hiding versioned images behind DOS 8.3 aliases.
- CPAR Boot Menu is UEFI-only. Legacy i386 PC/AT and PC-98 keep FAT16, read the
  fixed legacy `boot.cfg`, and boot their fixed kernel/root/data layout.
- The base system remains available from an immutable `rootfs-*.img`; multiple
  versioned roots and named writable data images may coexist on one boot
  filesystem. Versioned-kernel selection is not part of Boot CPAR v1.
- A complete section selects exactly one root mode: `rootpart` for a native
  filesystem, or both `rootfs` and `datafs` for an overlay. The loader maps the
  fields to the already implemented kernel parameters rather than adding a
  new handoff.
- Runtime CPAR uses exactly two read-only image roles, `base` and `app`.
  Application state is written only through explicitly mounted writable data
  directories; the rest of the composed root remains read-only.
- A service-container package produces an immutable service payload including
  its dependency closure plus separately managed configuration and data.
- Configuration is mounted read-only by default; declared data paths are
  writable. `/run` and `/tmp` should be private runtime filesystems.
- `cpar` is a Runtime CPAR command. Boot environments are administered by
  editing files and `boot.cfg` under `/boot`, not by `cpar boot` subcommands.
- A later `cpar build` command may construct an app image in a Docker-build-like
  workflow without adopting Docker's arbitrary-depth filesystem layer model.
- sshd containerization is deferred because host home-directory and login
  session mounting needs a separate design.
- Runtime CPAR namespace/security design, `cpar run`/`cpar sh`/`cpar build`,
  and service-container package design are manually blocked until explicitly
  resumed.
- `menuconfig` can select service-container packages under
  `userland/containers/`.

## Boot CPAR v1 configuration

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

An overlay section maps `rootfs`, `datafs`, and `swap` to the implemented
textual `overlay-root=`, `overlay-data=`, and `swap0=` contract. A native
section maps `rootpart` and `swap` to `rootpart=` and `swap0=` and may not also
contain `rootfs` or `datafs`. `boot0` is the loader-origin FAT filesystem unless
explicitly overridden; `boot1`--`boot3` allow later named filesystems. No
CPAR-specific binary handoff is added. The implemented common kernel contract is
[documented here](../../docs/reference/kernel-boot-parameters.md). Boot CPAR
still must add UEFI FAT32/VFAT long-filename lookup, the bounded `boot.cfg`
parser and menu, and translation of the selected complete section into that
parameter string. Missing configuration makes the loader emit the
fixed-layout defaults; invalid present configuration fails visibly.

## WS completion direction

The current planning-stage WS may pause after p001 produces an architecture
specification and implementation Phase decomposition. The common q015
foundation is not a Boot CPAR completion claim: FAT32/LFN lookup,
`boot.cfg` parsing, menu/default/timeout behavior, and selected-section
translation still require bounded implementation and acceptance Phases.
Runtime completion conditions remain deferred until the security boundary is
honest and testable.

## Reconsideration boundaries

Reconsider if safe teardown requires global mounts, a writable image can be
loop-backed recursively through itself, a host service can escape through
devices/process APIs, dependency licensing cannot be represented, or the
format cannot update and roll back atomically.
