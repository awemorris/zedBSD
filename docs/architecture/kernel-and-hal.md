# Kernel, HAL and driver boundaries

Status: current

zedBSD has its own kernel, drivers and user ABI. Familiar Unix names describe
interfaces, not a claim that a FreeBSD, NetBSD or Linux kernel binary or disk
image can be substituted. The [compatibility profile](../reference/compatibility-profile.md)
separates implemented declarations from conformance claims.

## Ownership and execution

| Layer | Responsibility and current owner |
| --- | --- |
| Loader | Select a configured kernel and retain boot information; [UEFI loader](../../bootloader/uefi/bootx64.c) and [boot parameters](../reference/kernel-boot-parameters.md) |
| HAL | Architecture-dependent address spaces, interrupts, CPU and display primitives; [HAL interface](../../include/hal/hal.h), [amd64 address spaces](../../src/hal/amd64/space.c) |
| Kernel | Process/thread lifetime, VM, descriptors, VFS and I/O; [process](../../src/kern/process.c), [thread](../../src/kern/thread.c), [VM](../../src/kern/vm/), [file descriptors](../../src/kern/filedesc.c) |
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

Current NVMe supports bounded concurrent commands (default depth four). A
syscall, a BIO and a hardware command are different transfer units: reducing
syscall splitting does not imply one hardware command or one durability flush.
A UAS descriptor decoder exists; it is not an operational storage transport.

## Display path

Three parts can draw on the screen. Each has its own owner, and none of them
hands the screen to another by itself.

| Part | What it draws on | Who sets it up |
| --- | --- | --- |
| Text console (`/dev/console`) | The board's display, through the [text-display table](../../include/kern/text-display.h) | The board's display driver, once, during its bring-up. On PC/AT that is the [text driver](../../src/drivers/platform/pcat/graphics/text.c), which draws into the firmware's framebuffer (the UEFI GOP framebuffer, or VGA text memory on a BIOS boot) |
| `/dev/graphics` | The same board display, in a drawing mode | The board's [graphics driver](../../src/drivers/platform/pcat/graphics/pcat-graphics.c). Entering a mode suspends the text console, and the final close resumes it |
| GPU node (`/dev/gpu0`) | The panel and outputs the GPU drives | The GPU driver ([i915](../../src/drivers/gpu/i915), [Venus](../../src/drivers/gpu/venus) and the others in [GPU drivers and node kinds](../reference/gpu-drivers.md)). Keiland's compositor is its user. The interface is described in [GPU device interface](../reference/gpu.md) |

The text console and `/dev/graphics` are one board-level pair. The text
console calls only the text-display table, so it stays a platform-independent
multiplexer. The board driver registers that table once. Until it does, every
console write is a no-op, which is the right behaviour on a board with no
display.

A GPU driver is not part of that pair:

- Loading a GPU driver and publishing `/dev/gpu0` sends no notice to
  `/dev/console` or `/dev/graphics`. The console's back end does not move to
  the GPU, and a GPU driver never registers the text-display table.
- `/dev/graphics` has no native GPU takeover. It always draws on the board
  display.
- At load, a GPU driver leaves the screen as the firmware lit it. The firmware's
  picture (the GOP output with the console on it) stays on screen.
- The GPU driver takes the display only when a client claims it through the GPU
  display interface: a display lease, which Keiland's compositor takes when it
  starts. Then the driver reads out the pipe the firmware left lit, stops it,
  sets its own mode and shows the client's buffers.
- Until the first image is flipped to a display, the text console keeps
  drawing on the screen, also while a lease is held. The first flip that any
  GPU node accepts (`GPU_PRESENT`, `GPU_DISPLAY_PRESENT`,
  `GPU_DISPLAY_PRESENT_SYNC` or `GPU_DISPLAY_COMMIT`) makes the kernel ask the
  text console to end its use of the display, once for the whole system. After
  that the console's characters reach only the serial line and the log for the
  rest of the boot, and they do not come back on the screen when the lease
  ends. What the screen then shows depends on the driver
  ([GPU device interface](../reference/gpu.md),
  [Display](../reference/gpu-display.md)).
- Keiland does not show the text console between the greeter and the session
  either (see [Keiland](keiland.md)).

## Interfaces and policy

Public C headers live in [include/libc](../../include/libc); zedBSD-specific
records live in [include/uapi](../../include/uapi). Kernel-private
headers and HAL structures are not application contracts. Build applications
against the selected target sysroot rather than a host OS's ioctl layouts.

Text terminals, event input and graphical drawing have separate interfaces:
[console/graphics](../reference/control-devices.md) and
[evdev](../reference/evdev.md). Graphical ownership does not imply exclusive
ownership of every input source.

Service and network policy belongs to [init](../reference/init-services.md)
and [networkd](../reference/managed-wlan.md). The kernel supplies device and
socket primitives; it does not choose saved WLAN credentials or implement a
daemon's retry policy. Shutdown likewise coordinates userland, filesystems
and DMA retirement before architecture-specific halt.

## Evidence boundary

They cover their stated host/QEMU configurations. This source overview adds no
new physical-machine or performance acceptance claim. 