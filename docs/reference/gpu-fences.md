# GPU fences

Status: experimental reference for GPU ABI version 1.

A fence records whether one piece of GPU work has finished, so that submissions, displays and processes can order their work against it. A program holds a fence through a file descriptor. The fence carries a generation number and a state, `GPU_FENCE_PENDING` (not finished), `GPU_FENCE_SIGNALED` (finished successfully) or `GPU_FENCE_ERROR` (ended in failure, with an `errno` value). A producer, the program or kernel code that does the work, signals the fence. Consumers wait on it, poll it, or name it as a prerequisite of a later request with `GPU_COMMAND_SUBMIT_SYNC` or `GPU_DISPLAY_PRESENT_SYNC`. A node offers the fence requests when `GPU_GET_INFO` reports `GPU_CAP_FENCE` ([capabilities](gpu-capabilities.md)). The rules shared by every request (nodes, permissions, calling conventions, concurrency) are in [the device interface](gpu.md).

## Fence model

##### Payload and generations

A fence holds three values: `generation`, `state` and `error`. A new fence has generation one and the state `GPU_FENCE_PENDING`, or `GPU_FENCE_SIGNALED` when it was created with `signaled` set. `error` is zero except in `GPU_FENCE_ERROR`, where it is the positive `errno` value that ended the generation. These values belong to the fence, not to the GPU open that created it and not to one descriptor: every descriptor of the same fence (duplicates, descriptors sent to other processes) shows the same values, and the fence lives until its last descriptor is closed and no open holds a binding to it. A fence belongs to the GPU device whose node created it. Every request that names a fence by descriptor refuses a fence of another device with `EXDEV`, with one exception: the `wait_fd` of `GPU_DISPLAY_PRESENT_SYNC` is not compared with the device, so a fence made through the node of a companion renderer can be waited on. A descriptor that is not an open kernel handle descriptor of the calling process fails with `EBADF`; a descriptor of another kind that the GPU framework hands out (for example an exported resource) fails with `EINVAL`.

A generation is one use of the fence. `GPU_FENCE_RESET` starts the next generation, with the state `GPU_FENCE_PENDING` and `error` zero, so that a fence can be used again. The requests that wait, reset or signal name the generation they mean, and a request that names a generation other than the current one fails with `ESTALE`; a wait that is asleep when the fence is reset wakes with `ESTALE`. This keeps a late wait or signal of one use from touching the next. `GPU_FENCE_QUERY` is the only request that may pass generation zero, which means the current generation; every other request fails with `EINVAL` on generation zero. Generations count up by one and never wrap: a reset of generation `UINT64_MAX` fails with `EOVERFLOW`.

##### Who signals a fence

Only a producer that bound a generation signals it, and a descriptor alone gives no right to signal. `GPU_FENCE_SIGNAL` with `error` zero declares the work successful, so a producer issues it only after it has observed the success authoritatively, for example after reading the reply of the command stream. The completion of a submission is not that proof: a completion record that becomes terminal with `status` zero ([submission](gpu-submission.md)) says only that the node received and finished the stream without a transport error, and the kernel never turns it into `GPU_FENCE_SIGNALED` for a fence bound with `GPU_FENCE_BIND`.

The kernel moves a bound generation to `GPU_FENCE_ERROR` by itself in these cases, and the first terminal state of a generation is final:

- The completion record whose `sequence` the generation was bound with becomes terminal with a non-zero `status`. The fence takes that `status` as its `error`. If the record had already ended in failure when the binding was made, the fence is in `GPU_FENCE_ERROR` as soon as `GPU_FENCE_BIND` returns.
- The open that holds the binding suffers a session failure, or the device fails. The fence takes the recorded errno ([submission](gpu-submission.md)).
- The open that holds the binding is closed for the last time while the generation is still bound. The fence takes `ENODEV`.

A generation that a job reservation bound ([jobs](gpu-jobs.md)) is signaled by the kernel with the job's own result, success included; the program cannot signal or release it (`EPERM`).

##### Binding a generation

`GPU_FENCE_BIND` gives the calling open (the open file description, shared by duplicated and inherited descriptors) the exclusive right to end one generation, and it is made before the work starts. A generation has at most one binding: binding one that already has an owner, or that is no longer pending, fails with `EBUSY`. While a generation is bound, `GPU_FENCE_RESET` fails with `EBUSY`. An open can hold at most `GPU_SUBMIT_MAX` bindings at a time; one more fails with `EAGAIN`. A binding that the program ends with `GPU_FENCE_SIGNAL` or `GPU_FENCE_BIND_RELEASE` frees its slot at once. A binding that the kernel ended because of a failure (the cases above) keeps its slot until the open is closed.

There are three forms of the request:

- `sequence` zero and `flags` zero: reserve ownership before the work is submitted natively. The fence stays pending. The program then ends the generation with `GPU_FENCE_SIGNAL`, or gives the reservation back.
- `sequence` non-zero and `flags` zero: reserve ownership and also associate the generation with the completion record that `GPU_COMMAND_SUBMIT` returned on this open. The record must still be listed on the open: a `sequence` the open never received, or one already consumed with `GPU_WAIT_CONSUME`, fails with `ENOENT`. A transport failure of that record then turns the fence into `GPU_FENCE_ERROR`, as described above. Success of the record does not signal the fence. A reservation made this way can no longer be released.
- `GPU_FENCE_BIND_RELEASE` in `flags`, with `sequence` zero: undo a reservation. The fence is left pending with no owner, so another producer can bind it. It is for submissions the kernel or the node refused.

Binding a generation needs the device to be online and healthy: if the device was withdrawn or has a recorded failure, `GPU_FENCE_BIND` fails with `ENODEV`. A failure of the calling open alone does not stop it.

##### The fence descriptor

The descriptor returned by `GPU_FENCE_CREATE` is independent of the GPU open that made it. Closing that open does not close the descriptor, and a fence descriptor works with any open of the same device. `flags` of the creation selects `GPU_HANDLE_CLOEXEC` (the descriptor closes on `exec`) and `GPU_HANDLE_CLOFORK` (the descriptor is not inherited by `fork`). Like any descriptor it can be duplicated and sent to another process, for example with `SCM_RIGHTS` over a UNIX domain socket; the other process sees the same fence.

`poll` on a fence descriptor reports readiness by level. When the state is not `GPU_FENCE_PENDING`, it reports `POLLIN` and `POLLRDNORM` for the bits that `events` asked for; when the state is `GPU_FENCE_ERROR`, it also reports `POLLERR` whatever `events` asked for. The readiness lasts until a reset starts the next generation, which makes the descriptor not ready again. `read` and `write` on a fence descriptor fail with `EBADF`, and `ioctl` on it fails with `ENOTTY`. Closing a descriptor drops one reference and never changes the state of the fence.

##### Capability and open mode

The six fence requests and the two `_SYNC` requests need `GPU_CAP_FENCE`; without it they fail with `EOPNOTSUPP`. `GPU_FENCE_QUERY` and `GPU_FENCE_WAIT` need a readable open; the other fence requests and both `_SYNC` requests need a writable open. The fence requests do not run under the open's admission, so they proceed while another thread of the same open is inside a request that holds it, and they do not fail because the open itself has failed. The `_SYNC` requests wait for their prerequisite outside the admission too, and take it only for the nested submission or present.

##### Dependencies in the `_SYNC` requests

`GPU_COMMAND_SUBMIT_SYNC` and `GPU_DISPLAY_PRESENT_SYNC` add a prerequisite and a completion target to a submission or a present. The outer structure has no header; the nested `command` or `present` keeps `version` `GPU_ABI_VERSION` and its own `size` (the size of `struct gpu_command_submit` or `struct gpu_display_present`, not the size of the outer structure). Each of the two dependencies is optional and is absent when its descriptor is `-1` and its generation is zero; any other combination of an absent descriptor with a non-zero generation, or of a real descriptor with generation zero, fails with `EINVAL`.

The request proceeds in this order:

1. The nested header and the shape of the signal pair are checked, so a malformed pair fails before any wait; then the signal descriptor is resolved and pinned.
2. The prerequisite (`wait_fd` and `wait_generation`; a malformed pair fails with `EINVAL`) is waited for, before the open's admission is taken and before anything is queued or shown. A prerequisite that is terminal does not wait: `GPU_FENCE_SIGNALED` lets the request go on, and `GPU_FENCE_ERROR` ends it at once with the fence's `error` as the errno, so that failed work is never followed by work that depends on it; nothing is submitted. A prerequisite that is still pending is waited for only when a driver has accepted responsibility for completing it (a generation that a committed job owns, see [jobs](gpu-jobs.md)); the wait is interruptible (`EINTR`) and has no timeout. A pending generation that no job owns, in particular one bound with `GPU_FENCE_BIND`, fails with `EAGAIN` at once, because the kernel does not wait for a program to signal future work. If the fence is reset while the request waits, it fails with `ESTALE`.
3. The completion target (`signal_fd` and `signal_generation`) is bound to the calling open before the nested request runs, so that closing the open always ends the target.
4. The nested submission or present runs under the open's admission, with the validation and errors of `GPU_COMMAND_SUBMIT` ([submission](gpu-submission.md)) or `GPU_DISPLAY_PRESENT` ([display](gpu-display.md)).
5. If the nested request fails, the outcome of the target depends on the request. A failed command releases the binding, and the target stays pending and unbound. A failed present ends the target in `GPU_FENCE_ERROR` with the errno that the present returned. If the open's admission fails before the nested request runs, the binding is released in both cases and the target stays pending and unbound.

What the target means after a success differs. For `GPU_COMMAND_SUBMIT_SYNC` the target is associated with the accepted completion record (`command.sequence`), and only a failure of that record signals it, as described under binding. Success is not signaled by the kernel: the program signals `GPU_FENCE_SIGNAL` with `error` zero after it has observed the work succeed. For `GPU_DISPLAY_PRESENT_SYNC` the target is signaled by the request itself when it returns: `GPU_FENCE_SIGNALED` after the present was selected for display, which is the same boundary as a plain `GPU_DISPLAY_PRESENT` and not the physical refresh. A queue notification never replaces a verified `GPU_FENCE_SIGNAL`.

## Requests

### `GPU_FENCE_CREATE`

Requires: open mode W; capability `GPU_CAP_FENCE`.

Creates a fence with generation one and installs a descriptor for it in the calling process. `flags` selects `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK` for the descriptor (zero for neither); `fd` must be `-1`, `generation` zero and `reserved` zero on input. With `signaled` zero the fence starts in `GPU_FENCE_PENDING` and has no owner: bind it with `GPU_FENCE_BIND` to become its producer. With `signaled` one it starts in `GPU_FENCE_SIGNALED`, has nobody to signal it and can only be reset. On success `fd` holds the new descriptor and `generation` is one. The fence belongs to the device of the node the request was made on. The fields are written to the argument before the descriptor is committed, so a request that fails at that point returns an error and the `fd` it wrote names nothing.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not writable.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `fd` is not `-1`; `signaled` is greater than one; `reserved` or `generation` is not zero; `flags` has a bit other than `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK`.
- `ENOMEM`: the kernel could not allocate the fence, its handle, or room in the descriptor table.
- `EMFILE`: the calling process has no free descriptor.
- `ESRCH`: the calling thread has no process descriptor table.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_create_ioctl`, `gpu_export_install`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_create`), [main.c](../../userland/tests/acquire-fence/main.c).

### `GPU_FENCE_QUERY`

Requires: open mode R; capability `GPU_CAP_FENCE`.

Reads the fence without waiting. `generation` is zero for the current generation, or the generation the caller means. On success `generation`, `state` and `error` describe one generation as it was at one instant: the fields are never taken from two generations. `flags`, `timeout_ns`, `state` and `error` must be zero on input.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not readable.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `version` or `size` is not the structure's; `flags`, `timeout_ns` or `state` is not zero; `error` is not zero; `fd` is a descriptor of another kind that the GPU framework hands out rather than a fence.
- `EBADF`: `fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the fence belongs to another device.
- `ESTALE`: `generation` is non-zero and not the current generation.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_state_ioctl`, `gpu_fence_get`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_query`), no source-tree test.

### `GPU_FENCE_WAIT`

Requires: open mode R; capability `GPU_CAP_FENCE`.

Waits until the generation named by `generation` leaves `GPU_FENCE_PENDING`. `generation` must be an exact, non-zero generation. `timeout_ns` selects how long the wait lasts: zero does not wait and fails with `EAGAIN` when the fence is pending; a finite value waits at most that long, rounded up to the kernel's clock tick, and fails with `ETIMEDOUT` when it expires; `UINT64_MAX` waits without a limit. A signal ends the wait with `EINTR`; the kernel does not restart it, and the fence is unchanged, so the caller can wait again. The wait does not need the work to have been submitted: it also waits on a fence that nobody will ever signal, until the timeout. A reset of the fence during the wait ends it with `ESTALE`.

A fence that ended in `GPU_FENCE_ERROR` does not make the request fail. The request succeeds, and `state` is `GPU_FENCE_ERROR` with `error` set to the errno the generation ended with, so a caller reads `state` before it trusts the work. On success `state` is `GPU_FENCE_SIGNALED` or `GPU_FENCE_ERROR`, never `GPU_FENCE_PENDING`. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not readable.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `version` or `size` is not the structure's; `generation` is zero; `flags` or `state` is not zero; `error` is not zero; `fd` is a descriptor of another kind that the GPU framework hands out rather than a fence.
- `EBADF`: `fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the fence belongs to another device.
- `EOVERFLOW`: `timeout_ns` is so large that the deadline cannot be represented.
- `EAGAIN`: `timeout_ns` is zero and the generation is pending.
- `ETIMEDOUT`: a finite `timeout_ns` expired while the generation was pending.
- `EINTR`: a signal arrived during the wait.
- `ESTALE`: `generation` is not the current generation, or the fence was reset during the wait.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_state_ioctl`, `gpu_completion_deadline`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_wait`), no source-tree test.

### `GPU_FENCE_RESET`

Requires: open mode W; capability `GPU_CAP_FENCE`.

Starts the next generation of a fence, so that it can be bound and signaled again. `generation` must be the current generation; the fence may be in any state, including pending with no owner, but it must have no owner. On success the generation is one higher, `state` is `GPU_FENCE_PENDING` and `error` is zero, and the new generation is returned in `generation`. Descriptors keep working for the new generation. Everyone who waits on the old generation is woken and fails with `ESTALE`, and a descriptor that was ready is no longer ready. `timeout_ns` and `error` must be zero on input. If the reply cannot be copied out, the reset has already happened.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not writable.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `version` or `size` is not the structure's; `generation` is zero; `flags`, `timeout_ns` or `state` is not zero; `error` is not zero; `fd` is a descriptor of another kind that the GPU framework hands out rather than a fence.
- `EBADF`: `fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the fence belongs to another device.
- `ESTALE`: `generation` is not the current generation.
- `EBUSY`: the current generation is bound to a producer (an open or a job) that has not ended it.
- `EOVERFLOW`: the generation is `UINT64_MAX` and cannot be incremented.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_state_ioctl`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_reset`), no source-tree test.

### `GPU_FENCE_SIGNAL`

Requires: open mode W; capability `GPU_CAP_FENCE`.

Ends a generation that the calling open bound, and releases the binding. `generation` must be the bound generation. `error` is the outcome the producer reports: zero ends the generation in `GPU_FENCE_SIGNALED`, to be used only after the work was verified to have succeeded; a positive `errno` value ends it in `GPU_FENCE_ERROR` with that value stored as `error`. Any positive value is stored; a negative value fails with `EINVAL`. On success `generation`, `state` and `error` report the ended generation, and every waiter and poller of the fence sees it. Only the open that bound the generation can signal it; possessing the descriptor, even in the process that created the fence, does not give the right, and a generation that was never bound, was already ended, or was bound by a job cannot be signaled. `timeout_ns` must be zero.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not writable.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `version` or `size` is not the structure's; `generation` is zero; `flags`, `timeout_ns` or `state` is not zero; `error` is negative; `fd` is a descriptor of another kind that the GPU framework hands out rather than a fence.
- `EBADF`: `fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the fence belongs to another device.
- `EPERM`: this open holds no binding for this fence and generation (it was never bound, was bound by another open, or was ended by an earlier `GPU_FENCE_SIGNAL` or release); the binding belongs to a job; or the kernel had already ended the generation with a failure, so that this open no longer owns it.
- `ESTALE`: this open still holds a binding for `generation`, but the kernel had ended that generation with a failure and the fence has since been reset to a later one.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_state_ioctl`, `gpu_fence_binding_end`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_signal_deferred`), [main.c](../../userland/tests/acquire-fence/main.c).

### `GPU_FENCE_BIND`

Requires: open mode W; capability `GPU_CAP_FENCE`.

Makes the calling open the producer of one pending generation, or gives such a reservation back. See Fence model for the three forms. `generation` must be an exact non-zero generation; `flags` is zero or `GPU_FENCE_BIND_RELEASE`; `sequence` is zero, or the `sequence` of a completion record of this open, and must be zero with `GPU_FENCE_BIND_RELEASE`. Success changes the owner only: the state and `error` of the fence do not change, except that a bound record that had already failed ends the generation in `GPU_FENCE_ERROR`. The argument is not written back.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`.
- `EACCES`: the open is not writable.
- `EFAULT`: the argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `generation` is zero; `flags` has a bit other than `GPU_FENCE_BIND_RELEASE`; `flags` is `GPU_FENCE_BIND_RELEASE` and `sequence` is not zero; `fd` is a descriptor of another kind that the GPU framework hands out rather than a fence.
- `EBADF`: `fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the fence belongs to another device.
- `ENODEV`: the device was withdrawn or has a recorded failure (binding only).
- `ENOENT`: `sequence` is not zero and names no completion record of this open that is still listed (binding only).
- `EAGAIN`: the open already holds `GPU_SUBMIT_MAX` bindings (binding only).
- `ESTALE`: `generation` is not the current generation.
- `EBUSY`: binding: the generation already has an owner or is no longer pending. Release: the binding was made with a non-zero `sequence`.
- `EPERM`: release only: this open holds no binding for this fence and generation, the binding belongs to a job, or the generation had already ended.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_fence_ioctl`, `gpu_fence_bind_ioctl`, `gpu_fence_binding_add`, `gpu_fence_binding_end`), [gpu-fence.c](../../src/drivers/gpu/gpu-fence.c) (`drv_gpu_fence_bind`, `drv_gpu_fence_unbind`), [main.c](../../userland/tests/acquire-fence/main.c).

### `GPU_COMMAND_SUBMIT_SYNC`

Requires: open mode W; capability `GPU_CAP_FENCE` and `GPU_CAP_NOTIFICATION`.

Submits one command like `GPU_COMMAND_SUBMIT`, after waiting for an optional prerequisite fence and with an optional fence as the completion target. The outer structure has no `version` or `size`; the nested `command` carries `version` `GPU_ABI_VERSION` and `size` equal to `sizeof(struct gpu_command_submit)` (40), and all of its other fields are those of `GPU_COMMAND_SUBMIT` ([submission](gpu-submission.md)). `wait_fd` and `wait_generation` name the prerequisite; `signal_fd` and `signal_generation` name the target; an absent one is `-1` with generation zero. The order of the steps, what a prerequisite in each state does, and what happens to the target are in Fence model, Dependencies in the `_SYNC` requests.

On success `command.sequence` names the accepted completion record, as for `GPU_COMMAND_SUBMIT`; a target is then associated with that record, and the program observes the work with `GPU_COMMAND_WAIT` and signals the target itself after it has verified success. On failure the target is left pending and unbound, `command.sequence` may hold a number that names nothing, and nothing was submitted if the failure came from the prerequisite.

Errors:

- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`, or does not offer `GPU_CAP_NOTIFICATION`.
- `EFAULT`: the argument cannot be read or written back, or the stream's range cannot be read.
- `EINVAL`: `command.version` or `command.size` is not the nested structure's; `signal_fd` is `-1` with a non-zero `signal_generation`, is below `-1`, or is a descriptor with `signal_generation` zero; `wait_fd` is `-1` with a non-zero `wait_generation`, is below `-1`, or is a descriptor with `wait_generation` zero; a descriptor is of another kind that the GPU framework hands out rather than a fence; the nested command breaks the input rules of `GPU_COMMAND_SUBMIT`.
- `E2BIG`: `command.bytes` is larger than `GPU_COMMAND_MAX`.
- `EBADF`: `wait_fd` or `signal_fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the wait fence or the signal fence belongs to another device.
- `ESTALE`: `signal_generation` is not the current generation of the signal fence; or `wait_generation` is not the current generation of the wait fence, or the wait fence was reset during the wait.
- `EBUSY`: the signal generation already has an owner or is no longer pending.
- `EAGAIN`: the wait fence is pending and no job owns it; or the open already holds `GPU_SUBMIT_MAX` bindings; or all `GPU_SUBMIT_MAX` completion records of the open are in use.
- `EINTR`: a signal arrived while the request waited for the prerequisite or for the open's admission.
- `ENODEV`: the device was withdrawn or has a recorded failure, or the open failed with `ENODEV`.
- `EIO`: the open or the device failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records; a driver can record others, see [submission](gpu-submission.md)).
- `ENOMEM`: the kernel could not allocate the copy of the stream.
- `EOVERFLOW`: the counter that issues sequences is exhausted.
- Any positive errno value: the wait fence ended in `GPU_FENCE_ERROR`; the request returns that fence's `error`, and nothing is submitted.
- Other values: the driver's submit operation returns its own errno unchanged, as for `GPU_COMMAND_SUBMIT`.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_dependency_ioctl`, `gpu_dependency_wait`, `gpu_submit_ioctl`, `gpu_fence_binding_add`), no source-tree test.

### `GPU_DISPLAY_PRESENT_SYNC`

Requires: open mode W; capability `GPU_CAP_FENCE` and `GPU_CAP_DISPLAY`.

Presents one frame like `GPU_DISPLAY_PRESENT` ([display](gpu-display.md)), after waiting for an optional prerequisite fence and with an optional fence that the request signals when it returns. The outer structure has no `version` or `size`; the nested `present` carries `version` `GPU_ABI_VERSION` and `size` equal to `sizeof(struct gpu_display_present)` (80), and its fields, including the display lease, the completed-sequence rules and the frame checks, are those of `GPU_DISPLAY_PRESENT`. The wait happens before the display hardware is touched. The target is bound before the present and ends when the present returns: `GPU_FENCE_SIGNALED` when the present succeeded, which is the moment the frame was selected for display and not a physical refresh, or `GPU_FENCE_ERROR` with the errno of a failed present. A prerequisite fence can come from another device (see Fence model); the target must belong to the device of the node. The request is a flip like `GPU_DISPLAY_PRESENT`: the first success ends the text console on the display, as described in [display](gpu-display.md).

Errors:

- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_FENCE`, or does not offer `GPU_CAP_DISPLAY`.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: `present.version` or `present.size` is not the nested structure's; `signal_fd` is `-1` with a non-zero `signal_generation`, is below `-1`, or is a descriptor with `signal_generation` zero; `wait_fd` is `-1` with a non-zero `wait_generation`, is below `-1`, or is a descriptor with `wait_generation` zero; a descriptor is of another kind that the GPU framework hands out rather than a fence; the nested present breaks the input rules of `GPU_DISPLAY_PRESENT`.
- `EBADF`: `wait_fd` or `signal_fd` is not an open descriptor of the calling process, or is not a kernel handle descriptor of the driver kind (an ordinary file, for example).
- `EXDEV`: the signal fence belongs to another device.
- `ESTALE`: `signal_generation` is not the current generation of the signal fence; or `wait_generation` is not the current generation of the wait fence, or the wait fence was reset during the wait.
- `EBUSY`: the signal generation already has an owner or is no longer pending.
- `EAGAIN`: the wait fence is pending and no job owns it; or the open already holds `GPU_SUBMIT_MAX` bindings.
- `EINTR`: a signal arrived while the request waited for the prerequisite or for the open's admission.
- `ENODEV`: the device was withdrawn or has a recorded failure, or the open failed with `ENODEV`.
- `EIO`: the open or the device failed and the recorded errno is returned, or the driver reported success with a zero `sequence`.
- Any positive errno value: the wait fence ended in `GPU_FENCE_ERROR`; the request returns that fence's `error` and the frame is not presented.
- Other values: the driver's present operation answers pass through unchanged, as for `GPU_DISPLAY_PRESENT` (for example `ENXIO` for an output that is not connected, `EBUSY` for a lease that is not this open's, `ETIMEDOUT` for an adoption that did not come). When the present itself fails after the target was bound, the target is signaled `GPU_FENCE_ERROR` with the same errno.

Sources: [gpu-fence.h](../../include/uapi/gpu-fence.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_dependency_ioctl`, `gpu_dependency_wait`, `gpu_display_ioctl`, `gpu_display_ungated`, `gpu_fence_binding_end`), no source-tree test.

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_FENCE_PENDING` | 0 | State: the generation has not ended. A new fence starts here unless it was created signaled, and a reset returns here. |
| `GPU_FENCE_SIGNALED` | 1 | State: the generation ended successfully; `error` is zero. |
| `GPU_FENCE_ERROR` | 2 | State: the generation ended in failure; `error` holds the positive `errno` value. The state is as final as `GPU_FENCE_SIGNALED` until a reset. |
| `GPU_FENCE_BIND_RELEASE` | 1 | Bit of the `flags` of `GPU_FENCE_BIND`: undo a reservation instead of making one. It requires `sequence` zero. |

## Structures

#### `struct gpu_fence_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 32. |
| 8 | 4 | `uint32_t` | `flags` | in | Zero, or `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK` in any combination; they select the inheritance of the new descriptor. No other bit is defined. |
| 12 | 4 | `int32_t` | `fd` | in/out | On input must be `-1`. On success the new fence descriptor. |
| 16 | 4 | `uint32_t` | `signaled` | in | Zero creates the fence pending; one creates it already `GPU_FENCE_SIGNALED`. No other value is accepted. |
| 20 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 24 | 8 | `uint64_t` | `generation` | out | The generation of the new fence, always one. Must be zero on input. |

#### `struct gpu_fence_state`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 4 | `int32_t` | `fd` | in | The fence descriptor, of the device of the node. |
| 12 | 4 | `uint32_t` | `flags` | zero | Reserved; must be zero. |
| 16 | 8 | `uint64_t` | `generation` | in/out | The generation the request means. `GPU_FENCE_QUERY` accepts zero for the current generation; every other request needs a non-zero exact generation. On success the generation the reply describes (the new one after `GPU_FENCE_RESET`). |
| 24 | 8 | `uint64_t` | `timeout_ns` | in | Used by `GPU_FENCE_WAIT` only, and zero for every other request: zero does not wait, `UINT64_MAX` waits without a limit, other values are in nanoseconds and rounded up to the kernel's clock tick. |
| 32 | 4 | `uint32_t` | `state` | out | `GPU_FENCE_PENDING`, `GPU_FENCE_SIGNALED` or `GPU_FENCE_ERROR`. Must be zero on input. Written only when the request succeeds. |
| 36 | 4 | `int32_t` | `error` | in/out | Input: the outcome a producer reports with `GPU_FENCE_SIGNAL`, zero for success or a positive `errno` value; zero for every other request, and negative values are refused. Output: the `errno` value the generation ended with, zero when it has not ended in failure. |

#### `struct gpu_fence_bind`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 32. |
| 8 | 4 | `int32_t` | `fd` | in | The fence descriptor, of the device of the node. |
| 12 | 4 | `uint32_t` | `flags` | in | Zero to reserve ownership, or `GPU_FENCE_BIND_RELEASE` to undo a reservation. No other bit is defined. |
| 16 | 8 | `uint64_t` | `generation` | in | The generation to bind or release. Must not be zero. |
| 24 | 8 | `uint64_t` | `sequence` | in | Zero, or the `sequence` of a completion record of this open (from `GPU_COMMAND_SUBMIT`) whose transport failure should end the generation in `GPU_FENCE_ERROR`. Must be zero with `GPU_FENCE_BIND_RELEASE`. |

#### `struct gpu_command_submit_sync`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 40 | `struct gpu_command_submit` | `command` | in/out | The command to submit, with its own `version` and `size` (40, not the size of the outer structure). `command.sequence` is output, as for `GPU_COMMAND_SUBMIT`. |
| 40 | 4 | `int32_t` | `wait_fd` | in | Descriptor of the fence to wait for before the command is queued, or `-1` for none. |
| 44 | 4 | `int32_t` | `signal_fd` | in | Descriptor of the fence that is the completion target of the command, or `-1` for none. |
| 48 | 8 | `uint64_t` | `wait_generation` | in | Generation of the fence to wait for; zero exactly when `wait_fd` is `-1`. |
| 56 | 8 | `uint64_t` | `signal_generation` | in | Generation of the target fence, which must be pending and unbound; zero exactly when `signal_fd` is `-1`. |

#### `struct gpu_display_present_sync`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 80 | `struct gpu_display_present` | `present` | in/out | The present, with its own `version` and `size` (80, not the size of the outer structure). `present.sequence` is output, as for `GPU_DISPLAY_PRESENT`. |
| 80 | 4 | `int32_t` | `wait_fd` | in | Descriptor of the fence to wait for before the display hardware is touched, or `-1` for none. It may belong to another device. |
| 84 | 4 | `int32_t` | `signal_fd` | in | Descriptor of the fence signaled when the present returns, or `-1` for none. It must belong to the device of the node. |
| 88 | 8 | `uint64_t` | `wait_generation` | in | Generation of the fence to wait for; zero exactly when `wait_fd` is `-1`. |
| 96 | 8 | `uint64_t` | `signal_generation` | in | Generation of the target fence, which must be pending and unbound; zero exactly when `signal_fd` is `-1`. |
