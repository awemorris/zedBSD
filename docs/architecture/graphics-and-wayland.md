# Graphics and Wayland Architecture

Status: current

This document describes zedBSD's graphical execution model, the custom GPU
buffer sharing mechanism that replaces Linux `dma-buf`, the Vulkan Window System
Integration (WSI), and the operation of the `/bin/wayland` compositor.

---

## 1. The Design Rationale: Why Not Linux `dma-buf` and `DRI`?

Modern Unix graphics stacks have accumulated decades of architectural baggage.
zedBSD consciously departs from Linux's `dma-buf` and DRI/DRM/KMS infrastructure
in favor of a streamlined, capability-oriented model built around native Vulkan.

### 1.1 The Shift to Unified Memory and the Critique of `dma-buf`

The computing industry has decisively shifted toward Unified Memory Architecture
(UMA), prevalent in modern ARM64 SoCs, Apple Silicon, and contemporary x86 APUs
where the CPU and GPU share physical RAM. In this architectural reality, Linux's
`dma-buf` framework is excessively heavy and over-abstracted:

- **Over-Engineering**:
  The elaborate machinery of dynamic buffer attachments, exporter/importer
  treaties, and complex scatter-gather synchronization in `dma-buf` adds
  substantial kernel complexity with negligible practical benefit.
- **Narrow Real-World Utility**:
  Even on discrete GPU hardware, cross-subsystem buffer sharing in practice
  is almost exclusively used for video hardware encode/decode on VRAM and handing
  frames to the display compositor.
- **Explicit Driver Copy Control**:
  When transfers between discrete GPU VRAM pools are genuinely required, the GPU
  driver framework can explicitly orchestrate the copy, rather than requiring a
  convoluted global kernel subsystem.
- **Simplicity via Standard POSIX Primitives**:
  zedBSD replaces `dma-buf` with a lightweight kernel capability handle model
  transmitted across standard POSIX UNIX domain sockets via `SCM_RIGHTS`.
- **Operating System Portability**:
  Removing the deep dependency on Linux-specific `dma-buf` semantics ensures
  that GPU drivers and graphics components remain decoupled, modular, and far
  easier to port across operating systems.

### 1.2 Legacy Bloat, Licensing Hurdles, and Moving Past DRI/DRM/KMS

The Direct Rendering Infrastructure (DRI), Direct Rendering Manager (DRM), and
Kernel Mode Setting (KMS) subsystems on Linux suffer from acute architectural debt:

- **Historical Accretion**:
  DRI/DRM/KMS has become bloated in its effort to preserve backwards compatibility
  and abstract every historical graphics card design pattern—most of which have
  no relevance to modern computer architecture.
- **GPL Licensing Friction**:
  Linux DRM code is predominantly licensed under the GNU General Public License
  (GPL). Historically, other operating systems (such as the BSD family) have
  struggled with immense engineering overhead and legal friction trying to maintain
  out-of-tree compatibility shims for Linux DRM.
- **Direct Vulkan as the OS Graphics API**:
  Rather than perpetuating the DRM/KMS model, zedBSD replaces KMS with the
  modern paradigms of **Vulkan Display** (`VK_KHR_display` and direct scanout)
  and adopts **Vulkan directly as the native OS graphics API**. The kernel provides
  a concise, bounded device interface through `/dev/gpu0`, delegating hardware
  scheduling, command execution, and display management directly to Vulkan-oriented
  primitives.
- **Vulkan Video for Hardware Video Acceleration**:
  Similarly, instead of adopting legacy, fragmented video acceleration APIs
  such as VA-API (libva) or VDPAU, zedBSD adopts **Vulkan Video as the native
  OS API for GPU-accelerated video decoding and playback** (Khronos extensions
  `VK_KHR_video_queue`, `VK_KHR_video_decode_queue`, and `VK_KHR_video_decode_h264`).
  This unifies 3D rendering, display presentation, and video acceleration under
  a single, coherent API. Decoded video frames reside directly in GPU memory
  as native Vulkan images, ready to be sampled or composited without intermediate
  driver backends, format shims, or unnecessary memory copies.

---

## 2. Kernel Capability Handle Architecture

GPU memory sharing between processes (e.g., from an application client to the
compositor, or from the compositor to a display controller) is implemented
through kernel capability file descriptors.

```
+--------------------------------------------------------------------------+
| Client Application (Vulkan WSI)                                          |
|   1. Allocates GPU VkDeviceMemory and VkImage                            |
|   2. Calls ioctl(gpu_fd, GPU_RESOURCE_EXPORT, &export_args)              |
|      -> Kernel returns capability file descriptor: handle_fd             |
+--------------------------------------------------------------------------+
                                    |
                                    | SCM_RIGHTS (UNIX Domain Socket)
                                    | kl_gpu_buffer_v1.create_buffer(handle_fd)
                                    v
+--------------------------------------------------------------------------+
| Compositor (/bin/wayland)                                                |
|   1. Receives handle_fd from client message                              |
|   2. Calls ioctl(compositor_gpu_fd, GPU_RESOURCE_IMPORT, &import_args)   |
|      -> Reconstructs local GPU resource / VkImage without data copying   |
|   3. Renders into composite scene or programs scanout plane              |
+--------------------------------------------------------------------------+
```

### 2.1 Exporting Resources (`GPU_RESOURCE_EXPORT`)

A process holding an allocation creates a capability file descriptor using
the `GPU_RESOURCE_EXPORT` request on its open GPU node:
- **Immutable Descriptor**:
  The exporter fills in the linear image dimensions: `width`, `height`,
  `format` (`GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`), `stride`, `offset`,
  `allocation_bytes`, Vulkan `memory_type`, and `usage` flags.
- **Kernel Capability Object**:
  The kernel validates the geometry against the underlying memory blob, pins
  the memory allocation, assigns the exporting device's unique `device_id`, and
  returns a new file descriptor (`handle_fd`).
- **Inheritance Flags**:
  The caller can specify `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK`.

### 2.2 IPC Transmission via `SCM_RIGHTS`

Because the capability is an ordinary file descriptor backed by a kernel
object, the client transmits it across the Wayland UNIX domain socket using
standard POSIX `SCM_RIGHTS` ancillary control messages. The receiving
compositor receives a distinct descriptor number referencing the same
underlying capability object.

### 2.3 Importing Resources (`GPU_RESOURCE_IMPORT`)

The importing process calls `GPU_RESOURCE_IMPORT`:
- The kernel validates that the capability object matches the target GPU node
  (cross-device imports are rejected with `EXDEV`, unless `GPU_IMPORT_SCANOUT`
  is explicitly supported by a display node).
- The kernel returns the immutable image description to the importer and creates
  a new, local GPU resource handle.
- The underlying physical memory remains valid and pinned as long as either
  the capability descriptor or an imported resource handle exists.

---

## 3. The `kl_gpu_buffer_v1` Wayland Protocol

Instead of `zwp_linux_dmabuf_v1`, zedBSD defines the native `kl_gpu_buffer_v1`
global interface in the compositor.

### Revisions & Capabilities

| Revision | Features |
| :---: | :--- |
| **v1** | Basic GPU buffer construction: passes the exported `handle_fd`, width, height, stride, and pixel format. Commits are assumed completed upon submission. |
| **v2** | **Acquire Fences**: Introduces asynchronous presentation. The client can attach a synchronization fence (`set_acquire_fence`) so the compositor waits for GPU rendering completion before sampling. |
| **v3** | **Premultiplied Alpha**: Adds `set_alpha` support (`KL_GPU_BUFFER_V1_ALPHA_PREMULTIPLIED` vs opaque) enabling translucent window compositing. |

### Protocol Flow

1. **Registry Binding**:
   The client binds to `kl_gpu_buffer_v1` during display setup.
2. **Buffer Creation**:
   The client invokes `kl_gpu_buffer_v1_create_buffer(factory, handle_fd, &metadata)`.
   The marshaller passes `handle_fd` as ancillary data. The compositor adopts
   the image and instantiates a `struct wl_buffer`.
3. **Commit & Damage**:
   The client attaches the buffer to a `wl_surface`, posts damage regions, and
   calls `wl_surface_commit()`.

---

## 4. Vulkan Window System Integration (WSI)

zedBSD's Vulkan implementation integrates natively with Wayland through
[`userland/desktop/libvulkan/wsi-wayland.c`](../../userland/desktop/libvulkan/wsi-wayland.c):

1. **Swapchain Creation (`vkCreateSwapchainKHR`)**:
   - Queries `kl_gpu_buffer_v1` capabilities from the compositor registry.
   - Allocates `VkImage` instances backed by device-local, shareable blobs
     (`GPU_BLOB_SHAREABLE`).
   - For each swapchain image, calls `GPU_RESOURCE_EXPORT` to generate a
     capability file descriptor.
   - Instantiates a `wl_buffer` for each swapchain image via
     `kl_gpu_buffer_v1_create_buffer()`.
2. **Presentation (`vkQueuePresentKHR`)**:
   - Acquires the rendering completion fence.
   - If `kl_gpu_buffer_v1` revision 2+ is available, passes the acquire fence
     descriptor to the compositor via `kl_gpu_buffer_v1_set_acquire_fence()`.
   - Attaches the swapchain buffer and commits the surface.
3. **Hardware Video Acceleration via Vulkan Video**:
   - Hardware decode (e.g. H.264 playback) submits bitstreams directly to the
     GPU's video decode queue (`VK_QUEUE_VIDEO_DECODE_BIT_KHR`).
   - The decoded output is written directly into a device `VkImage`.
   - Because video output images and 3D rendering surfaces share the same
     unified Vulkan memory model, decoded frames can be sampled by composition
     shaders or presented via swapchains with zero copying and without
     requiring external video driver frameworks such as VA-API.
4. **Game Mode and Direct Scanout**:
   - When a 3D application or game requests fullscreen mode without translucent
     desktop overlays, `/bin/wayland` engages **direct scanout**.
   - Instead of blitting or compositing the client's swapchain image into the
     desktop scene, the compositor assigns the client's imported `VkImage`
     buffer handle directly to the primary hardware display scanout plane.
   - Page flips occur at the display controller level via `GPU_DISPLAY_PRESENT`
     synchronized with hardware VBLANK, achieving bare-metal performance,
     ultra-low latency, and Variable Refresh Rate (VRR) support.

---

## 5. Software Rendering Fallback (`wl_shm`)

For non-GPU clients or software rendering (e.g. cairo-image, traditional software
clients), zedBSD implements the standard Wayland `wl_shm` interface:

- **Anonymous Shared Memory**:
  Because zedBSD does not implement Linux's `memfd_create()`, clients call
  `shm_open()` with a unique name, immediately call `shm_unlink()` to detach
  the name from the VFS namespace, call `ftruncate()`, and pass the resulting
  anonymous file descriptor over the Wayland socket via `SCM_RIGHTS`.
- **Zero-Leaked Inodes**:
  Since `shm_unlink` is called before transmission, abnormal client crashes
  never leave orphaned shared memory files in `/tmp` or `/run`.

---

## 6. The `/bin/wayland` Compositor

`/bin/wayland` is the unified, fullscreen Wayland display server and window
manager for zedBSD. It operates in multiple execution modes governed by its
command-line role resolver.

### 6.1 Execution Roles

The role is resolved once during startup by `userland/desktop/wayland/role.c`:

1. **Normal Desktop Session (`KWL_ROLE_NORMAL`)**:
   - Invoked as default or with `--session`.
   - Manages the full desktop UI: dock bar, window management, application menus,
     virtual keyboard, and status popups.
   - Accepts multiple Wayland client connections on `/run/user/<uid>/wayland-0`.
2. **Greeter Mode (`KWL_ROLE_GREETER`)**:
   - Invoked with `--greeter --auth-fd=<fd>`.
   - Runs as the unprivileged `_greeter` system user under `sessiond`.
   - Displays the graphical login screen (user selection, password prompt).
   - Does not expose an open client socket. Communicates user authentication
     events directly to `sessiond` over `--auth-fd`.
3. **Testing Mode (`KWL_ROLE_TESTING`)**:
   - Invoked with `--testing [--timeout=<sec>] [--max-frames=<n>]`.
   - Used for continuous integration, regression testing, and smoke verification.
   - Automatically shuts down when the frame limit or deadline expires.

### 6.2 Command-Line Options

| Option | Argument | Description |
| --- | --- | --- |
| `--socket=` | `<path>` | Absolute path to the Wayland listening UNIX socket (default: `$XDG_RUNTIME_DIR/wayland-0`). |
| `--width=` | `<pixels>` | Explicit output display width (e.g. for emulator or fixed display mode). |
| `--height=` | `<pixels>` | Explicit output display height. |
| `--rotate=` | `0\|90\|180\|270` | Screen orientation rotation angle in degrees. |
| `--greeter` | *None* | Selects the login greeter role. |
| `--auth-fd=` | `<fd>` | IPC descriptor connecting the greeter to `sessiond`. |
| `--control-fd=` | `<fd>` | Descriptor to notify `sessiond` when the user session is `READY`. |
| `--lock-idle=` | `<seconds>` | Screen lock idle timeout (0 to disable). |
| `--keyboard-blur` | *None* | Enables dynamic scene background blur under the on-screen keyboard. |
| `--no-layer-cache`| *None* | Disables intermediate layer surface caching (forces full redraw). |
| `--log-frames` | *None* | Logs per-frame presentation latency and timing to the console/log. |

### 6.3 Internal Threading Architecture

`/bin/wayland` employs dedicated worker threads to guarantee fluid rendering
regardless of client IPC load:

- **Main Event Loop**:
  Dispatches client Wayland socket protocol messages, surface damage tracking,
  and seat input events (`input-thread.c`).
- **Render Thread (`render-thread.c`)**:
  Compiles draw lists, executes Vulkan GPU rendering pipelines, and manages
  composition layers.
- **Display Thread (`display-thread.c`)**:
  Interacts with `/dev/gpu0` display presentation ioctls, synchronizing scene
  flips with hardware vertical blanking intervals (VSync).
- **Cursor Thread (`cursor-thread.c`)**:
  Independent cursor positioning thread allowing low-latency mouse and stylus
  movement even under heavy GPU composition loads.

---

## 7. Related Documents

- [Kernel, HAL and driver boundaries](kernel-and-hal.md)
- [Session management and graphical login](session-management.md)
- [GPU device interface](../reference/gpu.md)
- [GPU sharing between opens and devices](../reference/gpu-sharing.md)
- [Vulkan Video decode](../reference/vulkan-video.md)
- [Media playback](../reference/media-playback.md)
