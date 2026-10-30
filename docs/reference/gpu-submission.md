# GPU command submission and completion

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: a program sends command streams
(gpu-command-stream.md) to a node and observes their completion; this
file covers the plain submission requests, completion records, session
failure and recovery. Fences are in gpu-fences.md, producer-owned jobs in
gpu-jobs.md. Link back to gpu.md. -->

PENDING

## Acceptance and completion

<!-- spec: From gpu_command_ioctl, gpu_submit_ioctl, gpu_wait_ioctl,
gpu_completion_reserve, gpu_completion_observe and drv_gpu_complete in
src/drivers/gpu/gpu.c: (1) Success of a submission means the kernel
accepted the stream; it never means the work finished or succeeded (the
Vulkan result travels in the stream's reply, gpu-command-stream.md).
(2) GPU_COMMAND has no completion record; GPU_COMMAND_SUBMIT returns a
sequence whose completion GPU_COMMAND_WAIT observes. (3) Timelines and
GPU_COMMAND_CONTEXT_FENCE: what a timeline selects (a backend-defined
completion domain; the capset's timelines word bounds it), ordering
within and across timelines. (4) The completion record: status is zero or
a positive errno of the transport; GPU_WAIT_CONSUME retires a terminal
record only after its whole output is copied out; how many records an
open may hold (GPU_SUBMIT_MAX) and what a submission does when they are
all in use. (5) The command buffer is read from user memory during the
call; say whether the caller may reuse it after return (read when the
kernel copies or pins it). -->

PENDING

## Session failure and recovery

<!-- spec: From gpu_session_error_locked, gpu_session_fail_locked,
drv_gpu_report_error, drv_gpu_report_session_error, gpu_poll and
gpu_recover_open: (1) Two failures: a session failure (one open's
context is lost: which requests then fail and with which errno, what
poll reports) and a device failure (the whole device: errno, poll
POLLERR|POLLHUP, what happens to every open). (2) What a program does to
recover: close the failed open and open again; a failed device recovers
on an open after every old reference has closed (gpu_recover_open); say
which errno an open returns until then. (3) What happens to pending
completion records, fences bound to the open, jobs and display leases
on failure (point to the area documents for detail). (4) This is how
Vulkan's VK_ERROR_DEVICE_LOST arises in libvulkan (one sentence). -->

PENDING

## Requests

### `GPU_COMMAND`

<!-- spec: Argument struct gpu_command (Structures). Read gpu_command_ioctl. Inputs address (user address of the stream),
bytes (bounds, GPU_COMMAND_MAX), flags (accepted values). No completion
record; when a caller uses it instead of GPU_COMMAND_SUBMIT. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_COMMAND_SUBMIT`

<!-- spec: Argument struct gpu_command_submit (Structures). Read gpu_submit_ioctl. Inputs address, bytes, flags
(GPU_COMMAND_CONTEXT_FENCE), timeline, reserved; output sequence (start
value, monotonic per what?). Errors when records are exhausted. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_COMMAND_WAIT`

<!-- spec: Argument struct gpu_command_wait (Structures). Read gpu_wait_ioctl and gpu_completion_observe. Inputs sequence,
timeout_ns (zero only observes; UINT64_MAX waits until completion or a
signal), flags (GPU_WAIT_CONSUME); output status. Errors for an unknown
sequence, a consumed one, a timeout (EAGAIN versus ETIMEDOUT: read which
for zero and for a finite timeout), EINTR. Whether it runs outside the
session admission (gpu.md Concurrency). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_PRESENT`

<!-- spec: Argument struct gpu_present (Structures). Read gpu_present_ioctl. Whole-frame presentation of a packed four-byte
pixel image in an owned storage resource (not a blob): handle, offset,
width, height, stride, format (GPU_PIXEL_BGRA8888, GPU_PIXEL_RGBA8888),
frame. Which nodes support it (GPU_CAP_PRESENT; gpu-drivers.md), where
the image goes, how it relates to the display lease interface
(gpu-display.md), and the first-flip console side effect (gpu-display.md
User-visible side effects). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_COMMAND_CONTEXT_FENCE` | 1 | PENDING |
| `GPU_WAIT_CONSUME` | 1 | PENDING |
| `GPU_SUBMIT_MAX` | 64 | PENDING |
| `GPU_COMMAND_MAX` | 0x10000 | PENDING |
| `GPU_PIXEL_BGRA8888` | 1 | PENDING |
| `GPU_PIXEL_RGBA8888` | 2 | PENDING |

## Structures

#### `struct gpu_command`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `address` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `bytes` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `flags` | PENDING | PENDING |

#### `struct gpu_command_submit`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `address` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `bytes` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `timeline` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |

#### `struct gpu_command_wait`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `timeout_ns` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `status` | PENDING | PENDING |

#### `struct gpu_present`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `stride` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `format` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `frame` | PENDING | PENDING |
