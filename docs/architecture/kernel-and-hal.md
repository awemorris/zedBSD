# Kernel, HAL and driver boundaries

Status: current; source inventory reconciled in q150 (2026-09-09)

zedBSD has its own kernel, drivers and user ABI. Familiar Unix names describe
interfaces, not a claim that a FreeBSD, NetBSD or Linux kernel binary or disk
image can be substituted. The [compatibility profile](../reference/compatibility-profile.md)
separates implemented declarations from conformance claims.

## Ownership and execution

| Layer | Responsibility and current owner |
| --- | --- |
| Loader | Select a configured kernel and retain boot information; [UEFI loader](../../bootloader/uefi/bootx64.c) and [boot parameters](../reference/kernel-boot-parameters.md) |
| HAL | Architecture-dependent address spaces, interrupts, CPU and display primitives; [HAL interface](../../include/hal/hal.h), [amd64 address spaces](../../src/hal/amd64/space.c) |
| Kernel | Process/thread lifetime, VM, descriptors, VFS and I/O; [process](../../src/kern/process.c), [thread](../../src/kern/thread.c), [VM](../../src/kern/vm.c), [file descriptors](../../src/kern/filedesc.c) |
| Drivers | Device discovery and device/filesystem operations; [generic devices](../../src/drivers/generic), [PCI](../../src/drivers/pci), [USB](../../src/drivers/usb), [filesystems](../../src/drivers/fs) |
| Userland | Administration policy and applications; [init](../../userland/base/init), [networkd](../../userland/base/networkd), [libc implementations](../../userland/base/libc) |

A descriptor names a reference to an open file description, not a device's
global lifetime. `dup` and inherited descriptors can keep that description
alive after one descriptor closes. Device teardown must separately stop new
work, retire outstanding transfers and preserve referenced objects until their
owners release them. The [control-device reference](../reference/control-devices.md)
gives the graphics example: exclusive ownership belongs to the open file
description, and final close restores the console.

HAL interfaces hide machine mechanisms, not every device difference. Common
code still handles errors, references and cancellation. On amd64, page mapping
and physical-memory accounting belong to the HAL; VM object/page ownership
belongs to the common kernel. A mapped address, a pinned backing page and
exclusive access to its contents are different guarantees. Callers must not
infer one from another.

## Storage path

The ordinary path crosses [file I/O](../../src/kern/file.c), the filesystem
([UFS](../../src/drivers/fs/ufs.c) or [FAT](../../src/drivers/fs/fat.c)),
[cache](../../src/kern/cache.c), [buffers](../../src/kern/buf.c) and
[disk I/O](../../src/kern/disk.c). [Writeback](../../src/kern/writeback.c)
and [backing claims](../../src/kern/backing-claim.c) distinguish dirty-data
completion and exclusive storage use from merely holding a pathname.
`fsync`/directory synchronization and publication are separate steps;
see [atomic publication](../reference/atomic-publication.md).

The supported filesystem name is `ufs`, using the consolidated 64-bit format.
The former UFS1 alternative is not a second selectable implementation.
Userland [mkfs](../../userland/base/mkfs) owns its formatter implementation
independently of the kernel driver so the tool can be ported separately.
[Image verification](../reference/image-formatters.md) checks the documented
zedBSD image profile; it does not certify other systems' UFS variants.

Current NVMe supports bounded concurrent commands (default depth four).
A syscall, a BIO and a hardware command are different transfer units: reducing
syscall splitting does not imply one hardware command or one durability flush.
[WS025](../../plan/ws025-io-memory-cache/ws.md) retains direct user-page I/O,
the UAS transport and performance measurement as unfinished work. A UAS
descriptor decoder exists; it is not an operational storage transport.

## Interfaces and policy

Public C headers live in [libc/include](../../libc/include); zedBSD-specific
records live in [include/uapi/zedbsd](../../include/uapi/zedbsd). Kernel-private
headers and HAL structures are not application contracts. Build applications
against the selected target sysroot rather than a host OS's ioctl layouts.

Text terminals, event input and graphical drawing have separate interfaces:
[console/graphics](../reference/control-devices.md) and
[evdev](../reference/evdev.md). Graphical ownership does not imply exclusive
ownership of every input source. Native GPU acceleration remains a held
proposal in [WS014](../../plan/ws014-gpu/ws.md), not an installed `/dev/gpu`
contract.

Service and network policy belongs to [init](../reference/init-services.md)
and [networkd](../reference/managed-wlan.md). The kernel supplies device and
socket primitives; it does not choose saved WLAN credentials or implement a
daemon's retry policy. Shutdown likewise coordinates userland, filesystems
and DMA retirement before architecture-specific halt.

## Evidence boundary

Recent producer gates include [TLS q128](../../plan/queue-q128.md),
[NVMe q139](../../plan/queue-q139.md), [USB lifecycle q141](../../plan/queue-q141.md),
[input and real graphical terminal q147](../../plan/queue-q147.md), and
[formatter q148](../../plan/queue-q148.md). They cover their stated host/QEMU
configurations. This source overview adds no new physical-machine or
performance acceptance claim.
