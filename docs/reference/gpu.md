# GPU device interface

Status: experimental reference for GPU ABI version 1.

<!-- spec: Introduction, 2 to 4 paragraphs. Say what /dev/gpuN is: the
user API of the kernel GPU framework through which libvulkan, Keiland's
compositor and the test programs reach a GPU's rendering and display. Say
that this file holds the rules shared by every request (nodes,
permissions, calling conventions, concurrency), that gpu-requests.md
indexes every request and the shared error meanings, and that the other
gpu-*.md files describe the requests by area. Say that the ABI is experimental: requests may still be added in
version 1, and how a program detects what a node offers (GPU_GET_INFO
capability bits; see gpu-capabilities.md). Supported architectures: the
ones that have GPU drivers (amd64, aarch64, riscv64; read config/drivers/
and the drivers' build rules to confirm, and name the drivers' source
directories only in gpu-drivers.md). Do not describe driver internals
(include/drivers/gpu/ ops); those are not user API. -->

## Documents

- [Request index and errors](gpu-requests.md)
- [Capabilities and capability sets](gpu-capabilities.md)
- [Memory: resources, blobs, transfers and mappings](gpu-memory.md)
- [Sharing between opens and devices](gpu-sharing.md)
- [Command submission and completion](gpu-submission.md)
- [Fences](gpu-fences.md)
- [Jobs](gpu-jobs.md)
- [Display: outputs, leases and presentation](gpu-display.md)
- [Display control: power, refresh, cursor and planes](gpu-display-control.md)
- [Drivers and node kinds](gpu-drivers.md)
- [Command stream](gpu-command-stream.md)
- [GPU sysctl variables](gpu-sysctl.md)
- [Examples](gpu-examples.md)

## Headers

<!-- spec: A table | Header | Contents | with one row per public header,
each linked relatively from this file (../../include/uapi/...):
gpu.h, gpu-allocation.h, gpu-fence.h, gpu-display.h, gpu-scanout.h,
gpu-job.h, gpu-op.h, and sysctl.h (only its hw.gpu.* and hw.gputelemetry
part). Contents names the request range ('G' numbers) or what the header
holds. Then state the include rule a program follows (the headers include
<uapi/ioctl.h>; a zedBSD target compiler defines __ZEDBSD__, so the
request numbers are zedBSD's own; read include/uapi/ioctl.h and
include/uapi/hosted.h). -->

PENDING

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_ABI_VERSION` | 1 | PENDING |

## Nodes and permissions

<!-- spec: Cover, from the source (src/drivers/gpu/gpu.c: gpu_publish_node,
gpu_open, the node-number allocation in drv_gpu_register; and
userland/desktop/sessiond/seat.c):
(1) Node names /dev/gpuN: how N is chosen (lowest free number, reused
after a device goes away; assigned in probe order, so not stable across
boots or hardware changes). A program that needs to know two nodes are the
same device, or which display node pairs with which render node, uses
GPU_DEVICE_QUERY (device_id, companion_id; gpu-memory.md).
(2) One physical GPU may publish several nodes (render and display); refer
to gpu-drivers.md for which.
(3) Who may open: the exact rule gpu_open applies (superuser, or the
effective uid owns the node), and what the node mode is by default and
after the seat manager hands nodes to the seat user (which nodes, which
mode). Consequence for nodes the seat manager does not hand over.
(4) Read and write permission: the open's access mode decides whether the
open is readable and writable (read gpu_open and the O_ACCMODE values in
include/uapi/fcntl.h; give the result for each access mode); requests
check these (column Open mode of the index in gpu-requests.md). mmap
checks the protection against them (refer to gpu-memory.md).
(5) What a failed device does to open (gpu_open, gpu_recover_open): a
device that failed can recover on an open after every old reference is
closed; an open that finds another open recovering waits interruptibly
(read which wait queue and what ends the wait); until recovery an open
fails with the error the driver reported for the device (device->error),
so name the value only as "the device's recorded error".
(6) Comments elsewhere in the tree may describe node access differently
(for example the seat manager's comments); write what gpu_open does.
Each claim cites the function it comes from in a Sources: line at the end
of the section. -->

PENDING

## Calling conventions

<!-- spec: Cover:
(1) Every request is ioctl(fd, REQUEST, &argument) with one fixed-width
structure; the structures have the same layout on ILP32 and LP64 (the
tables in these documents are checked on both).
(2) version and size: a top-level argument begins with version =
GPU_ABI_VERSION and size = the size the request expects (column Bytes of
the index); say what the kernel returns when they differ (read the check
in gpu_ioctl_body or the handlers). List the exceptions exactly, from the
source: GPU_COMMAND_SUBMIT_SYNC and GPU_DISPLAY_PRESENT_SYNC carry no
header of their own; their nested command or present carries version and
the nested structure's size. struct gpu_blob_create_placed keeps the
nested gpu_blob_create header with size set to the outer size (64).
struct gpu_display_plane_state and struct gpu_placement have no header.
Nested descriptors (struct gpu_image_descriptor, struct
gpu_allocation_descriptor) carry their own version and size; say how they
are checked.
(3) Input, output and zero fields: reserved fields and output fields must
be zero on input where the kernel checks that (say where it does; the
Direction column of each structure table says zero for reserved).
(4) Return: 0 on success; -1 with errno on failure. Whether an argument's
output fields are written on failure: read gpu_ioctl and gpu_ioctl_body
(where the argument is copied in and out) and state the general rule; do
not survey every handler (each area document notes its own exceptions).
An unknown request number fails with the errno gpu_ioctl_body returns for
it (read it; it is not ENOTTY).
(5) Handles, identifiers and sequences: 64-bit handles are local to the
open file description; say what a stale or foreign handle returns.
(6) Pixel formats GPU_PIXEL_* and GPU_DISPLAY_FORMAT_* are distinct name
spaces (the first a value, the second a bit mask); say where each is
used. -->

PENDING

## Concurrency, blocking and cancellation

<!-- spec: From gpu_ioctl, gpu_session_enter and gpu_session_leave in
src/drivers/gpu/gpu.c:
(1) Which requests on one open file description run one at a time (under
the session's admission) and which run outside it and may run in
parallel with others on the same open (list them by name; read the
source, do not guess: candidates are the GPU_JOB_* requests,
GPU_COMMAND_WAIT, GPU_DISPLAY_EVENTS, GPU_DISPLAY_REFRESH, the fence
requests, the *_SYNC requests, GPU_DISPLAY_COMMIT and the requests that
gpu_display_ungated lets through). What a caller sees while another
thread holds the admission (does it wait, interruptibly, or fail?).
(2) Requests on different opens of the same node, and on different nodes
of the same device: what is shared (device state, display leases).
(3) Blocking requests and their timeouts: a table | Request | Timeout field
| Zero | Finite | UINT64_MAX | Upper bound per call | Interrupted by a
signal |, one row per waiting request (GPU_COMMAND_WAIT, GPU_FENCE_WAIT,
GPU_JOB_CAPACITY, GPU_DISPLAY_WAIT, GPU_DISPLAY_REFRESH,
GPU_DISPLAY_COMMIT with COMMIT_WAIT, the *_SYNC waits, any other the
source shows). Read the code, not only the header comments: the kernel
bounds some waits itself (GPU_DISPLAY_REFRESH), while for others the
driver's wait operation decides (GPU_DISPLAY_WAIT: some drivers return
at once, some bound the wait themselves); where the bound depends on the
driver, say so instead of giving one number.
(4) Cancellation: there is no cancel request for a wait; a signal ends it
with EINTR; say whether a wait restarts (SA_RESTART) and what state an
interrupted request leaves. Closing the last reference of an open: what
happens to pending work, leases and borrowed images (point to the area
documents).
(5) poll(): which events an open reports and when (completion POLLIN,
display events POLLPRI, device loss POLLERR|POLLHUP, session failure
POLLERR; read gpu_poll), and that a fence descriptor polls separately
(gpu-fences.md). -->

PENDING

## Compatibility

<!-- spec: State that this is zedBSD's own ABI: not Linux DRM, not
FreeBSD drm-kmod, not compatible with their ioctls or headers (do not
call any part "DRM compatible"). The command stream reuses the Venus
protocol's command numbers (gpu-command-stream.md names the version).
The Vulkan driver (libvulkan) is the intended consumer; say which Vulkan
features map onto which requests only where a header comment says so
(for example VK_EXT_display_control for GPU_DISPLAY_POWER,
vkGetDisplayPlaneCapabilitiesKHR for GPU_DISPLAY_OVERLAY_INFO). -->

PENDING

## Related documents

<!-- spec: Links to control-devices.md (the /dev/graphics and /dev/console
devices and how GPU display takeover relates), visionfive2-gpu.md,
kernel-boot-parameters.md (GPU driver parameters), and
../architecture/kernel-and-hal.md. -->

PENDING
