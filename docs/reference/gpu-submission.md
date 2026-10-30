# GPU command submission and completion

Status: experimental reference for GPU ABI version 1.

A program drives a GPU node by sending it command streams (the bytes described in [the command stream](gpu-command-stream.md)) and by observing when the node has finished with them. This document covers the plain submission requests `GPU_COMMAND` and `GPU_COMMAND_SUBMIT`; the completion request `GPU_COMMAND_WAIT` and the per-open completion records it observes; whole-frame presentation with `GPU_PRESENT`; and what happens to an open when its context or the whole device fails, and how a program recovers. Fences are in [fences](gpu-fences.md), producer-owned jobs in [jobs](gpu-jobs.md), and the rules shared by every request (nodes, permissions, calling conventions, concurrency) in [the device interface](gpu.md).

## Acceptance and completion

Success of a submission means only that the kernel accepted the stream. It never means that the work finished, and never means that it succeeded. The Vulkan result of a command travels back in the stream's own reply, which the program reads from the reply blob it set up ([the command stream](gpu-command-stream.md)); the kernel does not interpret it. Three different outcomes therefore exist for one command: the request returned (the kernel accepted or refused the stream), the completion record became terminal (the transport or driver finished with the stream), and the reply holds a Vulkan result.

`GPU_COMMAND` has no completion record. It hands the stream to the driver and returns. Whether the driver has already run the stream when the call returns, or has only queued it, is the driver's choice, and a program that needs to know when the stream was decoded reads the reply. A driver that runs the stream at once on the calling thread can return `EAGAIN` while earlier work of the same open is still queued; the caller then sends the stream with `GPU_COMMAND_SUBMIT`. The driver is called while the open's admission is held, so a driver that runs the stream synchronously keeps the open's other requests waiting for the duration.

`GPU_COMMAND_SUBMIT` accepts a stream and returns a `sequence`. The sequence names one completion record of the calling open, and `GPU_COMMAND_WAIT` observes that record. A sequence is a non-zero 64-bit number that the kernel draws from one counter shared with its other identities (resource handles among them). It is unique across all opens and nodes for the life of the system and is not consecutive. It is meaningful only on the open that received it: on any other open, and after the record was consumed, it names nothing, and a wait fails with `ENOENT`.

A submission can select a timeline. With `GPU_COMMAND_CONTEXT_FENCE` set in `flags`, the `timeline` field is a small number that selects a completion domain defined by the node, and the driver ties the record's completion to a context-fence marker in that domain. Without the flag, `timeline` must be zero. The kernel does not interpret the number: it passes it to the driver, which refuses numbers it does not serve (some in-tree renderers refuse values from 64 up with `EINVAL`). Zero is the open's default domain; a program uses it when it only needs to know that the node has processed the stream. A node declares its timelines in the `timelines` word of its capability set ([capabilities](gpu-capabilities.md)), and the Vulkan driver refuses a node that declares none. Records are independent. The kernel keeps no order among them, within a timeline or across timelines, and does not assume that records become terminal in submission order. Ordering between the work behind two records is whatever the node defines for its timelines; a program that needs an order waits for the earlier sequence before it submits the later stream. A submission with `bytes` zero and `GPU_COMMAND_CONTEXT_FENCE` set carries no stream: it only inserts the marker on the selected timeline, and `address` must then be zero.

A completion record is pending from the moment the kernel accepts the submission until the driver reports a terminal result or the open fails. It is terminal after that. For a terminal record, `status` of `GPU_COMMAND_WAIT` is zero, meaning the node finished with the stream without a transport error, or a positive `errno` value that the driver or the kernel recorded for the record: for example `ETIMEDOUT` when a deadline expired, `EIO`, or `ENODEV`. A reservation that is cancelled with `GPU_JOB_CANCEL` flags zero, or that the driver refused to accept, leaves no record: a later wait fails with `ENOENT`, and only a wait that was already asleep on it sees `status` `ECANCELED`. The first terminal result of a record is final; a later driver callback does not change it. `status` is a transport outcome and is a different thing from a Vulkan result. A terminal record stays observable, to any number of waits, until a wait with `GPU_WAIT_CONSUME` retires it. Retiring happens only after the whole output of that wait was copied to the caller; if the copy fails, the record stays terminal and the wait can be repeated. After it is retired, the sequence is unknown.

An open can hold at most `GPU_SUBMIT_MAX` records at a time, counting pending ones and terminal ones that have not been consumed. The same pool is used by `GPU_JOB_RESERVE` and by the nested submission of `GPU_COMMAND_SUBMIT_SYNC`. When every record is in use, `GPU_COMMAND_SUBMIT` fails with `EAGAIN` before the driver sees the stream. The program retries after it consumed terminal records (a wait with `GPU_WAIT_CONSUME`) or after pending ones became terminal. A program that never consumes its records runs out after `GPU_SUBMIT_MAX` submissions.

The kernel reads the command buffer from user memory during the call and keeps its own copy. `GPU_COMMAND` frees its copy before it returns; `GPU_COMMAND_SUBMIT` keeps the copy until the driver has finished with it. In both cases the caller may reuse or free its buffer as soon as the request returns, whether it succeeded or not.

## Session failure and recovery

Two kinds of failure end an open's ability to submit work.

A session failure loses one open's context and no other. The open records the first errno that caused it, and the record is permanent for that open: nothing clears it. The errno comes from a driver that reports the loss of one context, or from the kernel itself: `ETIMEDOUT` when a supervised job's deadline expires, `EIO` when the kernel can no longer prove what the context's native work did, and `ENODEV` when the open is being closed. Afterwards:

- Every request that runs under the open's admission (all except the ones that run outside it, listed in [the device interface](gpu.md)) fails with the recorded errno without reaching the driver. The display requests that skip the admission check the same state first and fail the same way. The other requests that run outside the admission apply their own rules, which their area documents give.
- Completion records that were pending become terminal at once with the recorded errno as `status`, even though the driver may still be stopping its native work. `GPU_COMMAND_WAIT` on them succeeds and returns that `status`. Records that were already terminal keep their `status`. A producer's graceful close is the exception: jobs it had already committed keep running and complete with their own result ([jobs](gpu-jobs.md)).
- Fences bound to the open with `GPU_FENCE_BIND` are signaled with the recorded errno ([fences](gpu-fences.md)). The failure does not itself end a display lease; the lease ends when the program releases it or closes the open ([display](gpu-display.md)).
- `poll` reports `POLLERR` whatever events were asked for, and `POLLIN` (when asked for) while a terminal record has not been consumed.
- Other opens of the same node are not affected.

After a session failure the kernel asks the driver to stop the context's native work and waits for the driver to confirm it. If the driver cannot confirm in time, the kernel isolates the context when the driver supports isolation, and otherwise turns the failure into a device failure.

A device failure loses the whole device. A driver reports it with a non-zero errno, or the kernel does when a stop could not be confirmed. The device records the errno, and every open of the node is failed with it as a session failure of its own, with all the effects above. New requests that run under admission, and the display requests that skip it, fail with the device's errno on every open, including opens made later; when both the device and the open have recorded an errno, the device's is returned. `poll` reports `POLLERR`. When the device is withdrawn (removed), the errno is `ENODEV` and `poll` reports `POLLERR` and `POLLHUP`; a failed device that is still registered does not report `POLLHUP`.

To recover, a program closes the failed open and opens the node again. A new open starts a new context; nothing of the old open (resources, handles, fences, sequences) is revived. After a session failure alone, the new open succeeds at once if the device is healthy. After a device failure, the device recovers in place on an open that finds every old reference closed: no other open of the node remains, and no descriptor exported from an old open is still alive ([sharing](gpu-sharing.md)). When that holds and the driver supplies a reset operation, the open itself runs the reset before it returns; the same applies to an open that finds a context the kernel had isolated. An open that arrives while another open is running the reset waits for its result, interruptibly (`EINTR`). When the reset succeeds, the device's errno is cleared and the open proceeds with a fresh context. Otherwise the open fails. It fails with the device's recorded errno while an old open or exported descriptor remains or the driver offers no reset; with the reset's own errno if the reset failed; with the device's errno if a new fault was recorded while the reset ran, or `EIO` when the kernel cannot certify that none occurred; and with `ENODEV` after the device was withdrawn. The old opens still hold the failed state, which is why they must close first.

In libvulkan, this is how `VK_ERROR_DEVICE_LOST` arises: a failed submission or wait, a completion `status` that is not zero, a missing or malformed reply, or an expired transport deadline marks the Vulkan context lost, and later calls on it return `VK_ERROR_DEVICE_LOST` (the application destroys the device and creates a new one).

## Requests

### `GPU_COMMAND`

Requires: open mode W; capability `GPU_CAP_COMMAND`.

Sends one command stream to the node's driver, which receives a private kernel copy of the bytes. No completion record exists: the call returns once the driver has taken the stream. Use it when the program reads the result from the reply and needs no notification, or when the node does not offer queued completion; use `GPU_COMMAND_SUBMIT` when the program needs to know when the node has finished with the stream. `flags` must be zero, `bytes` must be a non-zero multiple of four, and `bytes` is at most `GPU_COMMAND_MAX`. `address` is the user address of the first byte; it must be non-zero, must fit the address width of the system, and the whole range must be readable. Success means the driver received the stream, not that the stream was executed.

The request runs under the open's admission, so a second thread's request on the same open waits (interruptibly) until it returns.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; `EIO` and `ETIMEDOUT` are values the kernel itself records, and a driver can record others (see Session failure and recovery).
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_COMMAND`.
- `EFAULT`: the argument cannot be read, or `address` is zero, does not fit the system's address width, or the range wraps or cannot be read.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `flags` is not zero; `bytes` is zero, larger than `GPU_COMMAND_MAX` or not a multiple of four.
- `ENOMEM`: the kernel could not allocate the copy.
- Other values: the driver's command operation returns its own errno unchanged. In-tree drivers return `EAGAIN` when earlier queued work of the open must run first, `EIO` when the stream failed or the open is stopping, and `EINVAL` for a stream they cannot accept.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_command_ioctl`, `gpu_session_enter`), [client.c](../../userland/tests/venus-frame/client.c).

### `GPU_COMMAND_SUBMIT`

Requires: open mode W; capability `GPU_CAP_NOTIFICATION`.

Copies one command stream, or inserts an empty marker, and hands it to the driver together with a new completion record, then returns the record's `sequence`. The caller observes the record with `GPU_COMMAND_WAIT`. Input validation: `reserved` and `sequence` are zero; `flags` is zero or `GPU_COMMAND_CONTEXT_FENCE`; `timeline` is zero unless the flag is set; `bytes` is at most `GPU_COMMAND_MAX`; with `bytes` zero the flag must be set and `address` must be zero; with `bytes` non-zero `address` must name a readable range. The kernel does not require `bytes` to be a multiple of four; drivers do, and refuse other lengths. `sequence` is written to the caller's structure before the driver sees the stream. If the driver then refuses the stream, the request fails but the structure already holds a `sequence`; it names nothing, and a wait on it fails with `ENOENT`. Acceptance does not mean execution (see Acceptance and completion).

The request runs under the open's admission only while it validates and hands the stream to the driver; the work itself continues after the call returns.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records; a driver can record others). The check is repeated just before the stream is handed over, so a failure that occurs while the request is running also ends it with the recorded errno, and no work is submitted.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_NOTIFICATION`.
- `EFAULT`: the argument cannot be read or written back, or a non-empty stream's `address` is zero, does not fit the system's address width, or the range cannot be read.
- `EINVAL`: wrong `version` or `size`; `reserved` or `sequence` not zero; unknown bit in `flags`; `timeline` non-zero without `GPU_COMMAND_CONTEXT_FENCE`; `bytes` zero without the flag; `bytes` zero with a non-zero `address`.
- `E2BIG`: `bytes` is larger than `GPU_COMMAND_MAX`.
- `ENOMEM`: the kernel could not allocate the copy.
- `EOVERFLOW`: the counter that issues sequences is exhausted.
- `EAGAIN`: all `GPU_SUBMIT_MAX` completion records of the open are in use.
- Other values: the driver's submit operation returns its own errno unchanged (for example `EINVAL` for a length that is not a multiple of four or for a timeline it does not serve, or `ENOMEM`). On any failure no record remains for the request.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_submit_ioctl`, `gpu_completion_reserve`, `gpu_completion_discard`), [main.c](../../userland/tests/gpu-i915/main.c).

### `GPU_COMMAND_WAIT`

Requires: open mode R; capability `GPU_CAP_NOTIFICATION`.

Observes the completion record named by `sequence`, optionally waiting for it to become terminal, and returns its `status`. It does not run under the open's admission: other threads may submit and wait on the same open while one thread is blocked in it. A `timeout_ns` of zero only observes: the request returns at once, with `EAGAIN` if the record is not yet terminal. A finite non-zero `timeout_ns` waits at most that long, rounded up to the kernel's clock tick, and returns `ETIMEDOUT` when it expires. `UINT64_MAX` waits without a limit until the record is terminal, a signal arrives, or the device is withdrawn. A signal ends any wait with `EINTR`; the kernel does not restart the request. After `EAGAIN`, `ETIMEDOUT` or `EINTR` the record is unchanged and the program can wait again. With `GPU_WAIT_CONSUME` set, the first wait that finds the record terminal retires it after `status` was copied out. A second consuming wait that arrives while that consumption is in progress fails with `EAGAIN`, and once it is done the sequence is unknown (`ENOENT`). Without the flag, a terminal record can be observed repeatedly. On failure `status` is not written.

Errors:

- `EACCES`: the open is not readable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_NOTIFICATION`.
- `EFAULT`: the argument cannot be read, or the result cannot be written back (the record then stays unconsumed).
- `EINVAL`: wrong `version` or `size`; `sequence` is zero; `status` is not zero; an unknown bit in `flags`.
- `ENOENT`: the sequence names no record of this open: it was never issued, belongs to another open, was refused, or was consumed.
- `EAGAIN`: `timeout_ns` is zero and the record is not terminal; or `GPU_WAIT_CONSUME` was set and another wait is consuming the same record.
- `ETIMEDOUT`: a finite non-zero `timeout_ns` expired before the record became terminal.
- `EINTR`: a signal ended the wait.
- `ENODEV`: the device was withdrawn while a wait with a non-zero timeout found the record not terminal.
- `EOVERFLOW`: `timeout_ns` is so large that the deadline cannot be represented, or the record already has the largest possible number of observers.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_wait_ioctl`, `gpu_completion_observe`, `gpu_completion_observer_leave`, `gpu_completion_deadline`, `drv_gpu_complete`), [main.c](../../userland/tests/gpu-recovery/main.c).

### `GPU_PRESENT`

Requires: open mode W; capability `GPU_CAP_PRESENT`.

Shows one complete frame on the node's display. The frame is a packed image of four-byte pixels in an owned storage resource (created with `GPU_RESOURCE_CREATE`, see [memory](gpu-memory.md)), not a blob. `handle` names the resource and `offset` the byte at which the image starts. The image has `width` by `height` pixels, each row begins `stride` bytes after the previous one, and the image occupies `stride` times `height` bytes from `offset`, all of which must lie inside the resource. `format` is `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, and the four bytes of a pixel hold the channels in the order of the name, lowest address first. `width` and `height` are each from 1 to 4096, and `stride` is at least four times `width`. `frame` is a number chosen by the caller; the kernel does not interpret it, and the in-tree driver ignores it. The call returns once the driver has accepted the image. The resource is not borrowed afterwards, so the caller may rewrite the storage and present again.

Only a node that offers `GPU_CAP_PRESENT` accepts the request; every other node answers `EOPNOTSUPP`. Among the in-tree drivers this is the node of a virtual machine's paravirtual renderer ([drivers and node kinds](gpu-drivers.md)), which copies the image into the host's image for its first output. The request uses no display lease and is separate from the lease interface of [display](gpu-display.md): it presents to that first output as a whole and cannot address another output. It excludes a lease on that output, and a lease on that output excludes it (`EBUSY` from the driver). The presenting open keeps the display reserved, and a different open's `GPU_PRESENT` fails with `EBUSY` while it does; the reservation ends at the latest when that open closes.

The first accepted flip of the boot has a visible side effect. When `GPU_PRESENT`, `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_PRESENT_SYNC` or `GPU_DISPLAY_COMMIT` succeeds for the first time since boot, the kernel ends the text console's use of the framebuffer for the rest of the boot: the console's characters then reach only the serial line and the log. A board without a display console is not affected, and a request that fails does not end the console. What the screen shows when a lease ends depends on the driver ([display](gpu-display.md#user-visible-side-effects)).

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records; a driver can record others).
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_PRESENT`.
- `EFAULT`: the argument cannot be read.
- `EINVAL`: wrong `version` or `size`; `width` or `height` is zero or above 4096; `format` is not one of the two values; `stride` is less than four times `width`; `handle` is zero, unknown, stale or belongs to another open (this request answers `EINVAL` and not `ENOENT`); the resource is not ordinary storage; `offset` is past the end of the resource; the image does not fit between `offset` and the end of the resource.
- Other values: the driver's present operation returns its own errno unchanged. The in-tree driver returns `EBUSY` when another open owns the display or a lease holds the output, `ENODEV` when the machine has no output, and `EINVAL` for a `stride` that is not a multiple of four or is above 16384 bytes; it can also fail with errors of its transport.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_present_ioctl`, `gpu_console_end`), [venus-frame.c](../../userland/tests/venus-frame/venus-frame.c).

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_COMMAND_CONTEXT_FENCE` | 1 | The only flag of `struct gpu_command_submit`. The record's completion is tied to a context-fence marker in the completion domain that `timeline` selects. Without it `timeline` must be zero. |
| `GPU_WAIT_CONSUME` | 1 | The only flag of `struct gpu_command_wait`. Retires the record once it is terminal and the wait's whole output has been copied to the caller. |
| `GPU_SUBMIT_MAX` | 64 | The number of completion records one open can hold, counting pending records and terminal records not yet consumed. The pool is shared with job reservations and with the nested submission of `GPU_COMMAND_SUBMIT_SYNC`. |
| `GPU_COMMAND_MAX` | 0x10000 | The largest command stream in bytes, 65536. `GPU_COMMAND` refuses a larger one with `EINVAL`, and `GPU_COMMAND_SUBMIT` with `E2BIG`. |
| `GPU_PIXEL_BGRA8888` | 1 | Format value of `struct gpu_present`: four bytes per pixel, blue, green, red, alpha in increasing address order. A value of its own name space, distinct from the format bit masks of the display requests. |
| `GPU_PIXEL_RGBA8888` | 2 | Format value of `struct gpu_present`: four bytes per pixel, red, green, blue, alpha in increasing address order. |

## Structures

#### `struct gpu_command`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 24. |
| 8 | 8 | `uint64_t` | `address` | in | User address of the first byte of the command stream. Non-zero, and the range of `bytes` bytes must be readable. |
| 16 | 4 | `uint32_t` | `bytes` | in | Length of the stream in bytes: a non-zero multiple of four, at most `GPU_COMMAND_MAX`. |
| 20 | 4 | `uint32_t` | `flags` | zero | Reserved; must be zero. |

#### `struct gpu_command_submit`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 8 | `uint64_t` | `address` | in | User address of the first byte of the stream. Must be zero when `bytes` is zero. |
| 16 | 4 | `uint32_t` | `bytes` | in | Length of the stream in bytes, at most `GPU_COMMAND_MAX`. Zero is an empty marker and requires `GPU_COMMAND_CONTEXT_FENCE`. |
| 20 | 4 | `uint32_t` | `flags` | in | Zero or `GPU_COMMAND_CONTEXT_FENCE`. No other bit is defined. |
| 24 | 4 | `uint32_t` | `timeline` | in | Completion domain of the node in which the context fence is placed. Zero is the open's default domain. Must be zero when `GPU_COMMAND_CONTEXT_FENCE` is not set. |
| 28 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 32 | 8 | `uint64_t` | `sequence` | out | The identity of the completion record, non-zero and unique across the system. Must be zero on input. It is written before the driver sees the stream, and is meaningless when the request fails. |

#### `struct gpu_command_wait`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 32. |
| 8 | 8 | `uint64_t` | `sequence` | in | The identity returned by `GPU_COMMAND_SUBMIT` on this open. Must not be zero. |
| 16 | 8 | `uint64_t` | `timeout_ns` | in | How long to wait, in nanoseconds. Zero only observes. `UINT64_MAX` waits without a limit. Other values are rounded up to the kernel's clock tick. |
| 24 | 4 | `uint32_t` | `flags` | in | Zero or `GPU_WAIT_CONSUME`. No other bit is defined. |
| 28 | 4 | `uint32_t` | `status` | out | Zero, or a positive `errno` value that is the transport outcome of the record. Must be zero on input. It is written only when the request succeeds. |

#### `struct gpu_present`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 48. |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of an owned storage resource of this open, not a blob. |
| 16 | 8 | `uint64_t` | `offset` | in | Byte offset in the resource at which the image starts. It must not be past the end of the resource. |
| 24 | 4 | `uint32_t` | `width` | in | Width of the image in pixels, 1 to 4096. |
| 28 | 4 | `uint32_t` | `height` | in | Height of the image in pixels, 1 to 4096. |
| 32 | 4 | `uint32_t` | `stride` | in | Bytes from the start of one row to the start of the next. At least four times `width`. `stride` times `height` bytes must fit between `offset` and the end of the resource. |
| 36 | 4 | `uint32_t` | `format` | in | `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`. |
| 40 | 8 | `uint64_t` | `frame` | in | A number chosen by the caller to identify the frame. The kernel does not interpret it. |
