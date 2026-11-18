# Kernel, HAL, Driver Boundaries, and the Two-Tier OS Architecture

Status: current

zedBSD features an independent micro-modular architecture designed from scratch, with its own kernel, Hardware Abstraction Layer (HAL), device drivers, and userland ABI. Familiar Unix naming conventions are used to describe standard system concepts, rather than implying binary interchangeability with FreeBSD, NetBSD, or Linux. The [compatibility profile](../reference/compatibility-profile.md) separates actual implemented interfaces from standard Unix conformance profiles.

Beyond traditional Unix kernel design, zedBSD establishes a modern **Two-Tier OS Architecture**:
1. **Tier 1 (Core Kernel & POSIX ABI):** The HAL, kernel, device drivers, and standard C library ([libc](../../src/libc)), providing deterministic memory management, process execution, virtual file systems, networking primitives, and low-level descriptor lifetimes.
2. **Tier 2 (libkeiland as the Second OS API):** An application and system framework analogous to the role **OPENSTEP** pioneered. Rather than forcing application developers to write directly against raw POSIX sockets, evdev ioctls, or low-level kernel handles, `libkeiland` provides an integrated, cross-platform programming environment covering UI widgets, touch/pen/keyboard input, network status, wireless communications (WiFi, Bluetooth, cellular telephony, SMS/MMS), media assets (photos and video), and multi-display management.

---

## 1. Ownership and System Execution Hierarchy

The system enforces strict boundaries of responsibility across its layers.

| Layer | Responsibility and Implementation | Boundary Guarantee |
| --- | --- | --- |
| **Loader** | Boot protocol negotiation, hardware discovery passing, and initial kernel staging; [UEFI loader](../../bootloader/uefi/bootx64.c) and [boot parameters](../reference/kernel-boot-parameters.md) | Retains boot handoff parameters (ACPI tables, GOP framebuffer) and transfers execution to HAL |
| **HAL** | Architecture-dependent page tables, context switching, interrupt controllers, CPU topology, and firmware display handover; [HAL interface](../../include/hal/hal.h), [x86_64 address spaces](../../src/hal/x86_64/space.c) | Hides CPU/MMU quirks; does not manage process lifetime, reference counts, or VFS policy |
| **Kernel** | Process/thread scheduling, VM objects, file descriptors, VFS, socket layers, and synchronization; [process](../../src/kern/process.c), [thread](../../src/kern/thread.c), [VM](../../src/kern/vm/), [file descriptors](../../src/kern/filedesc.c) | Provides POSIX primitives and zedBSD syscall contracts (`include/uapi`) |
| **Drivers** | Bus discovery (PCI, USB, I2C), block storage, network MAC/PHY, GPU submission, and audio codecs; [generic](../../src/drivers/generic), [PCI](../../src/drivers/pci), [USB](../../src/drivers/usb), [filesystems](../../src/drivers/fs) | Device teardown must retire DMA and inflight requests before resource reclamation |
| **Core Userland** | System initialization, device policy daemons, and C library; [init](../../userland/base/init), [networkd](../../userland/base/networkd), [audiod](../../userland/base/audiod), [sessiond](../../userland/base/sessiond), [libc](../../src/libc) | Sits on Tier 1 POSIX syscalls; manages system-wide state and policy |
| **Application Framework** | **libkeiland** (Tier 2 "Second OS API"); [libkeiland](../../userland/desktop/include/keiland/keiland.h) | Unified application API spanning UI, peripherals, communications, and media; abstracted from display backends and underlying OS |
| **Compositor & Protocols** | Keiland compositor, Wayland protocol engine, X11 compatibility server; [keiland](../../userland/desktop/keiland) | Multiplexes graphics, touch/input routing, and surface presentation; serves as the native display backend on zedBSD |

### Object Lifetimes and Descriptors
A file descriptor in zedBSD refers to an open file description object, not the hardware device's physical existence. `dup` calls and process inheritance can hold a description open beyond the lifetime of any single descriptor. Device drivers must decouple operational work from reference cleanup: physical teardown cancels pending hardware transactions, while the backing data structures persist until the last file description is closed.

Similarly, within the memory management subsystem:
- A virtual address mapping managed by the HAL,
- A pinned physical backing page, and
- Exclusive access to page contents
are three independent guarantees. Kernel subsystems must never infer one guarantee from the existence of another.

---

## 2. The Two-Tier OS Architecture: POSIX and libkeiland (The OPENSTEP Analogy)

In traditional Unix-like systems, applications either target low-level C libraries and POSIX APIs directly, or depend on fragmented, desktop-specific toolkits (GTK, Qt) that rely on external userland daemon networks (D-Bus, NetworkManager, BlueZ, PulseAudio/PipeWire, ModemManager).

zedBSD unifies this application model through a **Two-Tier OS Architecture**, drawing direct philosophical inspiration from **OPENSTEP** (where the AppKit/Foundation frameworks served as an integrated, multi-system OS API above the Unix kernel):

```
+-----------------------------------------------------------------------------------+
|                           Native Keiland Applications                             |
|          (Browser, Files, Settings, Camera, Terminal, Mail, Phone, Notes)         |
+-----------------------------------------------------------------------------------+
                                          |
                                          v
+-----------------------------------------------------------------------------------+
|                        libkeiland ("Second OS API")                               |
|  - Modern Touch-First UI Toolkit (Gestures, Pens, Glass, Transitions)             |
|  - Device Control (LAN, WiFi, Bluetooth, Display Configuration)                   |
|  - Media Subsystem (Photo Library, Video Playback / Vulkan Video)                 |
|  - Communications (Cellular Voice Calls, SMS/MMS Messaging)                       |
+-----------------------------------------------------------------------------------+
       |                                |                           |
       v                                v                           v
+-----------------------+     +-------------------+     +-----------------------+
|  Keiland Wayland      |     |  Windows Backend  |     |  Linux / FreeBSD      |
|  Backend (zedBSD)     |     |  (Win32 / D3D /   |     |  Backends             |
|  (Wayland + GPU WSI)  |     |   DirectWrite)    |     |  (Wayland / X11)      |
+-----------------------+     +-------------------+     +-----------------------+
       |                                |                           |
       v                                v                           v
+-----------------------+     +-------------------+     +-----------------------+
|  zedBSD Kernel & HAL  |     |  Windows NT / OS  |     |  Linux / FreeBSD      |
|  (Tier 1 POSIX ABI)   |     |                   |     |  Kernels              |
+-----------------------+     +-------------------+     +-----------------------+
```

### Full-Spectrum Device and Service Control in `libkeiland`
Applications linked against `libkeiland` do not need to query separate, platform-specific subsystem daemons or issue ad-hoc ioctls. `libkeiland` serves as a comprehensive, first-class operating environment API covering:
- **Vulkan as the Native Rendering Engine:** All graphical rendering in `libkeiland`—from fluid touch gestures and frosted glass blurs to 2D vector primitives and 3D scenes—targets **Vulkan as the primary, native OS graphics API**.
- **Comprehensive Network Management:** Querying interface status, joining and monitoring IEEE 802.11 WiFi networks, configuring cellular mobile data, and controlling Bluetooth pairing and peripherals.
- **Display and Environment Control:** Multi-monitor topology, panel rotation, brightness curves, color profiles, and docking states.
- **Media and Asset Management:** Photo and video album indexing, camera capture, hardware-accelerated video decoding (via Vulkan Video), and audio routing.
- **Telephony and Messaging:** Managing incoming/outgoing cellular phone calls, contact address books, and SMS/MMS transmission and notification dispatch.

### OS-Transcendental Portability
`libkeiland` is strictly decoupled from the underlying operating system kernel. Applications written using `libkeiland` are immediately portable across platforms:
- **zedBSD:** Runs natively as the primary system framework, interfacing with `networkd`, `audiod`, and the Keiland Wayland compositor.
- **Windows Port:** `libkeiland` has been ported to Microsoft Windows, utilizing native Windows APIs (Win32 windowing, Direct3D/DirectWrite, Windows networking stacks). The official **Keiland Web Browser** already runs natively on Windows using this backend without altering application code.
- **Linux and FreeBSD Ports:** `libkeiland` and the Keiland desktop environment have also been fully ported to Linux and FreeBSD, running on top of native kernel subsystems and standard GPU DRM/KMS stacks.

---

## 3. Decoupling the Client Toolkit from the Desktop Display Protocol

A foundational architectural achievement of the Keiland desktop ecosystem is the complete separation between:
1. **The Touch UI/UX Toolkit (`libkeiland`),** and
2. **The Desktop Display Protocol and Compositor Backend.**

### The Wayland Compositor as a Backend Implementation
The Keiland Wayland compositor is **one backend implementation** among several for `libkeiland`. `libkeiland` does not require a Wayland compositor to function; when running on Windows, it outputs directly through the Windows desktop subsystem.

On zedBSD, the Keiland compositor operates as the native display server:
- **Wayland as the Native Protocol:** Currently, Wayland is chosen as the native protocol because its buffer sharing model (`wl_shm`, DRM/GPU buffer handles, explicit synchronization) is clean, robust, and highly extensible.
- **Protocol Agnosticism and Evolution:** The native communication channel between `libkeiland` and the desktop compositor can be augmented or migrated to a custom protocol in the future simply by adding APIs to the compositor and switching the `libkeiland` backend plugin. Applications built on `libkeiland` remain completely unaffected by protocol revisions.
- **X11 and Traditional Wayland Compatibility:** Concurrently, the Keiland compositor fully implements standard Wayland protocols (`xdg-shell`, subsurfaces, tablet v2, text-input v3) and X11 support (via Xwayland). Existing legacy applications (GTK, Qt, SDL, X11 programs) run alongside native `libkeiland` touch-aware applications with full server-side decoration and window management.

### The Touch-First Desktop Paradigm
Keiland simultaneously achieves four major UI/UX and architectural goals:
1. **Unified Touch-First UI/UX:** Elevates touch, digital pens, mice, and keyboards to first-class, co-equal input mechanisms, eliminating the false dichotomy between "mobile/tablet mode" and "desktop mode".
2. **High-Portability Touch Toolkit:** A rich widget set supporting fluid physics, frosted glass blurs, gestures, and docking titlebars.
3. **Multi-OS Deployment:** The identical toolkit and application binaries or source code run across zedBSD, Windows, Linux, and FreeBSD.
4. **Clean Protocol Separation:** The application code is insulated from the low-level rendering and windowing protocol details.

### Vulkan Rendering Pipeline, Game Mode, and Direct Scanout
In addition to standard desktop compositing, zedBSD and Keiland feature high-performance graphical execution pathways designed around Vulkan:

- **Universal Vulkan Rendering:** Vulkan serves as the unified rendering API across the desktop ecosystem. The Keiland compositor, `libkeiland` GUI components, games, and 3D applications all render through Vulkan queues and pipelines.
- **Game Mode Activation:** When a gaming title, 3D simulation, or low-latency graphics application enters fullscreen display (or requests Game Mode), the desktop compositor automatically shifts from composited scene rendering to **direct scanout**.
- **Direct Hardware Scanout:** Under direct scanout, the application's swapchain buffers—allocated in GPU device memory and shared via kernel capability file descriptors—are handed directly to the hardware display controller pipe via `GPU_DISPLAY_PRESENT` / `GPU_DISPLAY_COMMIT`.
- **Zero-Latency and Full Hardware Throughput:**
  - **Zero Intermediate Blits:** Bypasses compositor scene rendering and intermediate compositing buffers entirely.
  - **Sub-Millisecond Input Latency:** Directly pairs GPU presentation flips with the display refresh cycle (VBLANK), eliminating frame queues and composition lag.
  - **Variable Refresh Rate (VRR):** Enables direct hardware pacing and adaptive synchronization (FreeSync / G-Sync compatible VRR) without compositor frame-rate caps.

---

## 4. Storage and Virtual File System (VFS) Boundaries

The zedBSD storage stack cleanly isolates high-level filesystem operations from block-device DMA transports:

```
Applications / Daemons (Tier 1 & Tier 2)
       | (open, read, write, fsync)
       v
Virtual File System (VFS) [src/kern/file.c]
       |
       +---> Cache & Buffer Subsystem [src/kern/cache.c, src/kern/buf.c]
       |        | (Writeback & Backing Claims)
       |        v
       +---> Filesystem Drivers (UFS [src/drivers/fs/ufs.c], FAT [src/drivers/fs/fat.c])
                | (Block I/O / BIO Requests)
                v
       Disk I/O Subsystem [src/kern/disk.c]
                |
                v
       Device Drivers (NVMe, AHCI, SDHCI, USB Storage)
```

- **Consolidated 64-bit UFS:** The standard root and data filesystem is 64-bit UFS. Legacy UFS1 is not maintained as a runtime alternative. Userland tools ([mkfs](../../userland/base/mkfs)) maintain independent formatters to facilitate cross-platform rootfs generation without host kernel dependencies.
- **Storage Concurrency & Flush Guarantees:** Block devices operate under bounded concurrency queues (e.g. NVMe default depth four). Splitting a syscall into smaller BIOs does not imply hardware command flushes; durability is governed explicitly via `fsync` and [atomic publication](../reference/atomic-publication.md).

---

## 5. Low-Level Display Path and Console Handover

Three independent display paths exist within the kernel and driver tiers, ensuring that headless, board-level, and high-performance GPU systems are handled deterministically:

| Display Path | Target Destination | Initialization and Ownership |
| --- | --- | --- |
| **Text Console (`/dev/console`)** | Board firmware display buffer | Initialized once during platform bring-up via the [text-display table](../../include/kern/text-display.h). On PC/AT, this is [drv_pcat_text](../../src/drivers/platform/pcat/graphics/text.c), which renders to the UEFI GOP framebuffer or VGA memory |
| **Board Graphics (`/dev/graphics`)** | Raw framebuffer drawing | The board [graphics driver](../../src/drivers/platform/pcat/graphics/pcat-graphics.c). Acquiring the device suspends the text console; final close resumes text output |
| **GPU Node (`/dev/gpu0`)** | Hardware scanout pipelines and GPU memory | Managed by dedicated GPU drivers ([i915](../../src/drivers/gpu/i915), [Venus](../../src/drivers/gpu/venus), AMD RDNA, etc.; see [GPU drivers](../reference/gpu-drivers.md)). Used exclusively by the Keiland compositor via [GPU interface](../reference/gpu.md) |

### Seamless Console Handover to GPU
The kernel enforces clean, flicker-free handover rules:
1. Loading a GPU driver and publishing `/dev/gpu0` does not disturb `/dev/console` or overwrite the firmware's visual state. The boot splash or console text remains intact on screen.
2. The GPU driver claims scanout display pipes only when a client (the Keiland compositor) issues an explicit display lease.
3. Upon the very first successful page flip or commit (`GPU_PRESENT`, `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_PRESENT_SYNC`, or `GPU_DISPLAY_COMMIT`), the kernel notifies the text console driver to permanently retire its use of the physical display. From that point forward, console log messages are routed exclusively to memory ring buffers and serial debug ports, ensuring the compositor retains exclusive, tearing-free display control.

---

## 6. Interfaces and Policy Guidelines

- **Public C Headers:** Public libc symbols live in [include/libc](../../include/libc); kernel UAPI structures live in [include/uapi](../../include/uapi). Applications must compile against target sysroots rather than relying on host Linux/BSD headers.
- **Input and Control Separation:** Event input ([evdev](../reference/evdev.md)) and graphical display ([control-devices](../reference/control-devices.md)) have independent file descriptors. Holding a graphics lease does not grant unmediated access to raw input devices without appropriate session capability tokens.
- **Separation of Policy and Mechanism:** Kernel drivers provide hardware manipulation primitives; all configuration policy (such as WLAN association, DHCP negotiation, audio stream mixing, session startup) resides strictly in userland daemons ([networkd](../../userland/base/networkd), [audiod](../../userland/base/audiod), [sessiond](../../userland/base/sessiond)).

---

## 7. Related Architecture Documents

- [Keiland Desktop Architecture](keiland.md): Detailed specifications of the Keiland desktop, tablet and phone modes, client lifecycle, and Wayland extensions.
- [HAL and BSP Architecture](hal-and-bsp.md): HAL subsystems, machine primitives, and instructions for porting zedBSD to new architectures and boards.
- [Graphics and Wayland Subsystem](graphics-and-wayland.md): Deep dive into GPU handles, WSI, Vulkan Video, and `/bin/wayland` compositor modes.
- [Session Management Architecture](session-management.md): Multi-seat session orchestration, `/dev/session`, authentication, and seat switching.
- [Networking Architecture](networking.md): Kernel network stack, `networkd`, WiFi management, and `/sbin/net` tooling.
- [Audio Architecture](audio.md): `audiod`, shared memory audio rings, and `libpulse` compatibility layer.
