# GPU device interface

Status: experimental reference for GPU ABI version 1.

<!-- spec: Introduction, 2 to 4 paragraphs. Say what /dev/gpuN is: the
user API of the kernel GPU framework through which libvulkan, Keiland's
compositor and the test programs reach a GPU's rendering and display. Say
that this file holds the rules shared by every request (nodes,
permissions, calling conventions, errors, concurrency) and the index of
all requests, and that the other gpu-*.md files describe the requests by
area. Say that the ABI is experimental: requests may still be added in
version 1, and how a program detects what a node offers (GPU_GET_INFO
capability bits; see gpu-capabilities.md). Supported architectures: the
ones that have GPU drivers (amd64, aarch64, riscv64; read config/drivers/
and the drivers' build rules to confirm, and name the drivers' source
directories only in gpu-drivers.md). Do not describe driver internals
(include/drivers/gpu/ ops); those are not user API. -->

## Documents

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
(4) Read and write permission: the open's access mode (O_RDONLY, O_WRONLY,
O_RDWR) decides whether the open is readable and writable; requests check
these (column Open mode of the index). Say what an open with neither may
do. mmap checks the protection against them (refer to gpu-memory.md).
(5) What a failed device does to open (gpu_open, gpu_recover_open): a
device that failed can recover on an open after every old reference is
closed; say which errno an open returns meanwhile.
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
output fields are written on failure (read the copyout paths: in general
nothing is copied back on failure; name the exceptions you find).
(5) Handles, identifiers and sequences: 64-bit handles are local to the
open file description; say what a stale or foreign handle returns.
(6) Pixel formats GPU_PIXEL_* and GPU_DISPLAY_FORMAT_* are distinct name
spaces (the first a value, the second a bit mask); say where each is
used. -->

PENDING

## Errors

<!-- spec: A table | Error | Meaning across the interface | of the errno
values requests return with a common meaning: EINVAL (malformed request,
wrong version or size, nonzero reserved field), EOPNOTSUPP/ENOTSUP (the
node lacks the capability; say which name the source returns and that
ENOTSUP equals EOPNOTSUPP in include/uapi/errno.h), EBADF/EPERM/EACCES
(open mode or ownership; read which one gpu.c returns for a missing
readable or writable permission), ENOENT (unknown handle), EBUSY, EAGAIN,
ETIMEDOUT, EINTR, ENODEV and EIO (device or session failure; how they
differ), ENOMEM, ENOSPC, ESTALE, ENXIO, EFAULT. Only list values the
source returns. Each request's own Errors: list in its section is the
exact list; this table gives the shared meaning. -->

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
source shows). Values from the header comments and the code (for example
the one-second bound of display waits).
(4) Cancellation: there is no cancel request for a wait; a signal ends it
with EINTR; say whether a wait restarts (SA_RESTART) and what state a
interrupted request leaves. Closing the last reference of an open: what
happens to pending work, leases and borrowed images (point to the area
documents).
(5) poll(): which events an open reports and when (completion POLLIN,
display events POLLPRI, device loss POLLERR|POLLHUP, session failure
POLLERR; read gpu_poll), and that a fence descriptor polls separately
(gpu-fences.md). -->

PENDING

## Request index

<!-- spec: Fill the Open mode and Capability columns of every row from the
source (gpu_ioctl_body and each handler): Open mode is R when the request
needs a readable open, W when it needs a writable open, RW when it needs
both, none when it checks neither. Capability is the GPU_CAP_* bit the
kernel requires before calling the driver (none when it requires none;
join several with "and", alternatives with "or"). Each request's section
states the same values in its Requires: line; the two are compared
mechanically, so read the code rather than copying the section. Do not
change the other columns. -->

| Request | Nr | Direction | Argument | Bytes | Open mode | Capability | Described in |
| --- | ---: | --- | --- | ---: | --- | --- | --- |
| `GPU_GET_INFO` | 0 | `_IOWR` | `struct gpu_info` | 56 | PENDING | PENDING | [gpu-capabilities](gpu-capabilities.md#gpu_get_info) |
| `GPU_RESOURCE_CREATE` | 1 | `_IOWR` | `struct gpu_resource_create` | 32 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_resource_create) |
| `GPU_RESOURCE_DESTROY` | 2 | `_IOW` | `struct gpu_resource_destroy` | 16 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_resource_destroy) |
| `GPU_GET_CAPSET` | 3 | `_IOWR` | `struct gpu_capset` | 280 | PENDING | PENDING | [gpu-capabilities](gpu-capabilities.md#gpu_get_capset) |
| `GPU_BLOB_CREATE` | 4 | `_IOWR` | `struct gpu_blob_create` | 40 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_blob_create) |
| `GPU_RESOURCE_READ` | 5 | `_IOW` | `struct gpu_transfer` | 40 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_resource_read) |
| `GPU_RESOURCE_WRITE` | 6 | `_IOW` | `struct gpu_transfer` | 40 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_resource_write) |
| `GPU_COMMAND` | 7 | `_IOW` | `struct gpu_command` | 24 | PENDING | PENDING | [gpu-submission](gpu-submission.md#gpu_command) |
| `GPU_PRESENT` | 8 | `_IOW` | `struct gpu_present` | 48 | PENDING | PENDING | [gpu-submission](gpu-submission.md#gpu_present) |
| `GPU_RESOURCE_MAP` | 9 | `_IOWR` | `struct gpu_resource_map` | 32 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_resource_map) |
| `GPU_RESOURCE_EXPORT` | 10 | `_IOWR` | `struct gpu_resource_export` | 88 | PENDING | PENDING | [gpu-sharing](gpu-sharing.md#gpu_resource_export) |
| `GPU_RESOURCE_IMPORT` | 11 | `_IOWR` | `struct gpu_resource_import` | 96 | PENDING | PENDING | [gpu-sharing](gpu-sharing.md#gpu_resource_import) |
| `GPU_COMMAND_SUBMIT` | 12 | `_IOWR` | `struct gpu_command_submit` | 40 | PENDING | PENDING | [gpu-submission](gpu-submission.md#gpu_command_submit) |
| `GPU_COMMAND_WAIT` | 13 | `_IOWR` | `struct gpu_command_wait` | 32 | PENDING | PENDING | [gpu-submission](gpu-submission.md#gpu_command_wait) |
| `GPU_ALLOCATION_EXPORT` | 14 | `_IOWR` | `struct gpu_allocation_export` | 184 | PENDING | PENDING | [gpu-sharing](gpu-sharing.md#gpu_allocation_export) |
| `GPU_ALLOCATION_IMPORT` | 15 | `_IOWR` | `struct gpu_allocation_import` | 192 | PENDING | PENDING | [gpu-sharing](gpu-sharing.md#gpu_allocation_import) |
| `GPU_FENCE_CREATE` | 16 | `_IOWR` | `struct gpu_fence_create` | 32 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_create) |
| `GPU_FENCE_QUERY` | 17 | `_IOWR` | `struct gpu_fence_state` | 40 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_query) |
| `GPU_FENCE_WAIT` | 18 | `_IOWR` | `struct gpu_fence_state` | 40 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_wait) |
| `GPU_FENCE_RESET` | 19 | `_IOWR` | `struct gpu_fence_state` | 40 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_reset) |
| `GPU_FENCE_SIGNAL` | 20 | `_IOWR` | `struct gpu_fence_state` | 40 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_signal) |
| `GPU_FENCE_BIND` | 21 | `_IOW` | `struct gpu_fence_bind` | 32 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_fence_bind) |
| `GPU_COMMAND_SUBMIT_SYNC` | 22 | `_IOWR` | `struct gpu_command_submit_sync` | 64 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_command_submit_sync) |
| `GPU_DISPLAY_PRESENT_SYNC` | 23 | `_IOWR` | `struct gpu_display_present_sync` | 104 | PENDING | PENDING | [gpu-fences](gpu-fences.md#gpu_display_present_sync) |
| `GPU_DISPLAY_QUERY` | 24 | `_IOWR` | `struct gpu_display_info` | 152 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_query) |
| `GPU_DISPLAY_MODE` | 25 | `_IOWR` | `struct gpu_display_mode` | 48 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_mode) |
| `GPU_DISPLAY_CLAIM` | 26 | `_IOWR` | `struct gpu_display_claim` | 32 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_claim) |
| `GPU_DISPLAY_RELEASE` | 27 | `_IOW` | `struct gpu_display_release` | 16 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_release) |
| `GPU_DISPLAY_PRESENT` | 28 | `_IOWR` | `struct gpu_display_present` | 80 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_present) |
| `GPU_DISPLAY_WAIT` | 29 | `_IOWR` | `struct gpu_display_wait` | 56 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_wait) |
| `GPU_DEVICE_QUERY` | 30 | `_IOWR` | `struct gpu_device_info` | 40 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_device_query) |
| `GPU_DISPLAY_CONSTRAINTS` | 31 | `_IOWR` | `struct gpu_scanout_constraints` | 56 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_constraints) |
| `GPU_DISPLAY_EVENTS` | 32 | `_IOWR` | `struct gpu_display_events` | 40 | PENDING | PENDING | [gpu-display](gpu-display.md#gpu_display_events) |
| `GPU_BLOB_CREATE_PLACED` | 33 | `_IOWR` | `struct gpu_blob_create_placed` | 64 | PENDING | PENDING | [gpu-memory](gpu-memory.md#gpu_blob_create_placed) |
| `GPU_JOB_RESERVE` | 34 | `_IOWR` | `struct gpu_job_reserve` | 40 | PENDING | PENDING | [gpu-jobs](gpu-jobs.md#gpu_job_reserve) |
| `GPU_JOB_COMMIT` | 35 | `_IOW` | `struct gpu_job_action` | 24 | PENDING | PENDING | [gpu-jobs](gpu-jobs.md#gpu_job_commit) |
| `GPU_JOB_CANCEL` | 36 | `_IOW` | `struct gpu_job_action` | 24 | PENDING | PENDING | [gpu-jobs](gpu-jobs.md#gpu_job_cancel) |
| `GPU_JOB_CAPACITY` | 37 | `_IOWR` | `struct gpu_job_capacity` | 48 | PENDING | PENDING | [gpu-jobs](gpu-jobs.md#gpu_job_capacity) |
| `GPU_JOB_POLICY` | 38 | `_IOWR` | `struct gpu_job_policy` | 40 | PENDING | PENDING | [gpu-jobs](gpu-jobs.md#gpu_job_policy) |
| `GPU_DISPLAY_POWER` | 39 | `_IOW` | `struct gpu_display_power` | 32 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_power) |
| `GPU_DISPLAY_REFRESH` | 40 | `_IOWR` | `struct gpu_display_refresh` | 56 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_refresh) |
| `GPU_DISPLAY_CURSOR_SET` | 41 | `_IOW` | `struct gpu_display_cursor_image` | 64 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_set) |
| `GPU_DISPLAY_CURSOR_MOVE` | 42 | `_IOW` | `struct gpu_display_cursor_move` | 40 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_move) |
| `GPU_DISPLAY_CURSOR_HIDE` | 43 | `_IOW` | `struct gpu_display_cursor_hide` | 32 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_hide) |
| `GPU_DISPLAY_COMMIT` | 44 | `_IOWR` | `struct gpu_display_commit` | 552 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_commit) |
| `GPU_DISPLAY_OVERLAY_INFO` | 45 | `_IOWR` | `struct gpu_display_overlay_info` | 56 | PENDING | PENDING | [gpu-display-control](gpu-display-control.md#gpu_display_overlay_info) |

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
