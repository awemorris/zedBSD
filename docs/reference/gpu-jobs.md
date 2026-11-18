# GPU jobs

Status: experimental reference for GPU ABI version 1.

A job is GPU work that a producer owns from before it submits the work to the hardware until the work is finished. The producer is the program that drives the node natively; in the Vulkan driver it is the queue submission path. It first reserves completion storage and backend capacity with `GPU_JOB_RESERVE`, then submits the work to the hardware itself, and then publishes the job with `GPU_JOB_COMMIT` or withdraws the reservation with `GPU_JOB_CANCEL`. From the reservation on, the kernel supervises the job with deadlines that the administrator selected when the kernel was built, so that a producer that stops, hangs or dies cannot leave a fence unsignaled or a completion record pending for ever. The result of a committed job is observed through the open's completion records with `GPU_COMMAND_WAIT` ([submission](gpu-submission.md)), and an optional fence ([fences](gpu-fences.md)) can be tied to the job.

A node offers `GPU_CAP_JOB` for `GPU_JOB_RESERVE`, `GPU_JOB_COMMIT`, `GPU_JOB_CANCEL` and `GPU_JOB_POLICY`, and `GPU_CAP_JOB_CAPACITY` for `GPU_JOB_CAPACITY` ([capabilities](gpu-capabilities.md)). `GPU_CAP_JOB` is only set together with `GPU_CAP_NOTIFICATION`. The rules shared by every request are in [the device interface](gpu.md).

## Job life cycle

A job is one completion record of the calling open with a life cycle on top of it. It lives in the same pool of `GPU_SUBMIT_MAX` records as the records of `GPU_COMMAND_SUBMIT` ([submission](gpu-submission.md)), is named by the `sequence` that `GPU_JOB_RESERVE` returns, and is meaningful only on the open that reserved it. The job requests find only job records: an ordinary submission's sequence, a sequence of another open and a sequence that was discarded or consumed all fail with `ENOENT`.

##### States

A job is in one of three states. A fourth case, no record, is what remains when a reservation was cancelled with flags zero or the finished record was consumed.

| State | Meaning |
| --- | --- |
| reserved | `GPU_JOB_RESERVE` succeeded. The kernel holds completion storage and the driver holds capacity. The producer may submit its native work, but the kernel does not yet treat the work as accepted. A reservation deadline runs. |
| committed | `GPU_JOB_COMMIT` succeeded. The work is accepted and the driver owes a completion. An execution deadline runs. |
| finished | The record is terminal. Its `status` is zero when the committed work completed without a transport error, and otherwise a positive `errno` value. `GPU_COMMAND_WAIT` observes it and `GPU_WAIT_CONSUME` retires it. The record keeps counting against `GPU_SUBMIT_MAX` until then. |

##### Transitions

| Event | From | To | What remains |
| --- | --- | --- | --- |
| `GPU_JOB_RESERVE` succeeds | no record | reserved | A new `sequence`, the reservation deadline, and the fence bound to it when `fd` was given. |
| `GPU_JOB_RESERVE` fails | no record | no record | No record and no deadline. The `sequence` field of the caller's structure may already hold a number, which names nothing (see the request). |
| `GPU_JOB_COMMIT` succeeds | reserved | committed | The execution deadline replaces the reservation deadline, and the fence becomes eligible as a dependency of the kernel's `*_SYNC` requests. |
| `GPU_JOB_COMMIT` fails after the reservation check (the job, the open or the device ended meanwhile, the fence refuses the transition, or the driver refuses to publish) | reserved | finished, if the driver then accepts a fault cancel; otherwise reserved | The kernel treats the native work as possibly started. When the driver accepts the fault cancel, the open fails with `EIO` ([session failure](gpu-submission.md#session-failure-and-recovery)) and the record is terminal with the open's errno. |
| `GPU_JOB_CANCEL` with flags zero succeeds | reserved | no record | The record is discarded. The fence, if any, stays pending and is unbound, and its generation is unchanged. The sequence names nothing. |
| `GPU_JOB_CANCEL` with `GPU_JOB_CANCEL_FAULT` succeeds | reserved or committed | finished | The open fails with `EIO` (or keeps an errno it had already recorded). All records of the open that the driver still owns become terminal with the open's errno, and the fences bound to them are signaled with it. |
| The driver reports completion | committed | finished | `status` is zero for success, or the errno the driver reported. A fence bound to the job is signaled with the same result, success included. A completion that reports success while the job is still reserved is recorded as `EIO`. |
| The reservation deadline expires | reserved | finished | The open fails with `ETIMEDOUT`; see Supervision. |
| The execution deadline expires | committed | finished | The open fails with `ETIMEDOUT`; see Supervision. |
| The open or the device fails for another reason | reserved or committed | finished | `status` is the recorded errno. |
| The open is closed | reserved | finished | `status` is `ENODEV`. A committed job is not ended by a close: it keeps its deadline and its real completion. The kernel withdraws an uncommitted reservation through the driver's cancel operation. |
| `GPU_COMMAND_WAIT` with `GPU_WAIT_CONSUME` | finished | no record | The sequence is unknown. |

The first failure of an open decides the `status` of every record it ends. When the open had already failed with another errno, that errno is the `status`, not `ETIMEDOUT` or `EIO`. A finished record that was ended by a failure can still be waited for and consumed. `GPU_JOB_COMMIT` and `GPU_JOB_CANCEL` on a finished record return the record's own errno, or `EALREADY` when it finished with `status` zero.

`GPU_JOB_CANCEL` with flags zero is for work that the producer knows the hardware did not accept. `GPU_JOB_CANCEL_FAULT` is for work that may have reached the hardware, or for a device that was lost: the kernel cannot prove what the context's native work did, so it fails the open and keeps the memory the work may still access until the driver confirms that the work stopped or the device is reset ([session failure](gpu-submission.md#session-failure-and-recovery)).

##### The optional fence

`fd` and `generation` of `GPU_JOB_RESERVE` select a fence generation ([fences](gpu-fences.md)); `-1` and zero mean no fence, and the job is observed only through its completion record. A fence generation can be bound to one job at a time, and the binding gives the job the sole authority to signal it.

- A reserved generation has a producer deadline but is not a GPU dependency. A kernel dependency wait on it (the `*_SYNC` requests) fails at once with `EAGAIN` until `GPU_JOB_COMMIT` succeeds.
- `GPU_JOB_COMMIT` makes the generation eligible as a dependency.
- When the job finishes, the fence is signaled with the job's result: success when `status` is zero, otherwise the errno.
- `GPU_JOB_CANCEL` with flags zero unbinds the fence and leaves it pending; the producer may bind it again. `GPU_JOB_CANCEL_FAULT` signals it with the open's errno together with the rest of the open's work.
- When the reservation itself fails, the fence is not bound.

##### Supervision

The kernel runs one supervisor per device, started by the first reservation on it (or by the close of an open, when the driver can be asked to stop native work), so the deadlines are enforced even when no client is running. Three intervals are fixed when the kernel is built and cannot be extended by a client. `GPU_JOB_POLICY` reports them in nanoseconds.

| Interval | Build setting | Default | Armed when | If it expires |
| --- | --- | --- | --- | --- |
| Reservation | `CONFIG_GPU_JOB_RESERVATION_MS` | 10000 ms | `GPU_JOB_RESERVE` is accepted | The job's open fails with `ETIMEDOUT`. |
| Execution | `CONFIG_GPU_JOB_EXECUTION_MS` | 60000 ms | `GPU_JOB_COMMIT` succeeds (it replaces the reservation deadline) | The job's open fails with `ETIMEDOUT`. |
| Stop | `CONFIG_GPU_JOB_STOP_MS` | 10000 ms | The kernel begins to ask the driver to stop the failed open's native work | The kernel isolates the open's context if the driver can, and otherwise fails the whole device, with the open's errno or with the error that the driver's stop operation returned. |

Each value must be at least one millisecond or the kernel does not build. An interval is converted to the kernel's clock ticks rounded up, so it is never shorter than selected. The reservation and execution deadlines apply to job records only; the records of `GPU_COMMAND_SUBMIT` get no deadline from this supervision.

When a reservation or execution deadline passes, the open fails as a session failure ([submission](gpu-submission.md#session-failure-and-recovery)): every record of the open that the driver still owns becomes terminal with the open's errno, fences bound to them are signaled with it, new requests on the open fail with it, and other opens of the node are not affected. The kernel then asks the driver to stop the open's native work and polls it about every 100 ms until the stop interval ends. A driver that confirms in time lets the open close normally. A driver that cannot be asked to stop, that reports an error, or that does not confirm in time leads to the escalation in the table. A graceful close of the open does not start the stop while a committed job still runs inside its own execution deadline.

##### Capacity

Capacity is what lets a producer reserve without blocking. `GPU_JOB_CAPACITY` has two modes and neither one owns a job:

- QUERY (`GPU_JOB_CAPACITY_QUERY` in `flags`) returns at once with the node's current capacity sequence and whether a reservation would find room now.
- WAIT (`flags` zero; there is no macro for it) takes a sequence that an earlier call returned and waits for something to change. It returns when the capacity sequence differs from the one given, or when there is room now.

The capacity sequence belongs to the node and is shared by all its opens. It starts at one. It advances when a record of any open retires or is consumed, when a reservation is cancelled, when an open or the device fails or recovers, and when the driver reports a change of its own capacity. It never wraps: once a change would advance it past its largest value, every later `GPU_JOB_CAPACITY` on the node fails with `EOVERFLOW`. Room means that the driver reports capacity in the requested domain and that the calling open has a free record in its pool of `GPU_SUBMIT_MAX`. Terminal records that are not consumed do not make a repeated wait return by themselves.

`available` in the answer is only a hint: it is 1 when there was room at the moment of the call and 0 otherwise, and another caller can take the room before the producer reserves. A producer follows this order: observe with QUERY, consume the terminal records it can, try `GPU_JOB_RESERVE`; when that returns `EAGAIN`, wait with the sequence from the observation, and start again. `GPU_JOB_RESERVE` returns `EAGAIN` when the open has no free record, when it has no free slot for another bound fence (at most `GPU_SUBMIT_MAX` bound at a time), or when the driver has no capacity in the domain.

`domain` selects a completion domain of the node, with the same numbering as `timeline` of `GPU_JOB_RESERVE`. The kernel does not interpret it; the driver validates it. A Venus node accepts 1 to 63 and refuses 0 and larger values with `EINVAL`.

##### A node that offers jobs but cannot serve them

A node may report `GPU_CAP_JOB` and `GPU_CAP_JOB_CAPACITY` while `GPU_JOB_RESERVE` and `GPU_JOB_CAPACITY` fail with `ENOTSUP` (the same value as `EOPNOTSUPP`). The Venus driver does this when the host did not promise strict queue completion, that is, when `VENDOR_STRICT_QUEUE` is not set in the node's capability set ([capabilities](gpu-capabilities.md), [drivers](gpu-drivers.md)). Its reservation is checked after the argument checks: a Venus node answers `EINVAL` for an unusable domain even without a strict queue, and `ENOTSUP` otherwise. No job can exist on such a node, so `GPU_JOB_COMMIT` and `GPU_JOB_CANCEL` find nothing to act on (`ENOENT`). A program that needs jobs reads the capability set's flag before it relies on the bits.

## Requests

### `GPU_JOB_RESERVE`

Requires: open mode W; capability `GPU_CAP_JOB`.

Reserves completion storage and driver capacity for one job before the producer submits its native work, and returns the job's `sequence`. The reservation is a record of the calling open in the pool of `GPU_SUBMIT_MAX` records, with a reservation deadline that starts at once ([Job life cycle](#job-life-cycle)). `flags`, `reserved` and `sequence` must be zero on input. `fd` is `-1` for a job observed only through its completion record, or a fence descriptor of the same device, in which case `generation` must name a current, pending generation of that fence that no other job owns; with `fd` of `-1`, `generation` must be zero. `timeline` selects the completion domain in which the driver places the job's marker; the kernel does not interpret it and the driver validates it. The Venus driver accepts 1 to 63 and refuses zero.

The request does not run under the open's admission: it never waits for another request of the same open, and other requests of the open are not held up while it runs. The supervisor of the device is started if it is not running yet, before the reservation can be returned.

The kernel writes `sequence` to the caller's structure before it asks the driver to reserve. If the driver then refuses, the request fails, but the structure already holds a number; it names nothing, and a later request with it fails with `ENOENT`. A program uses `sequence` only after the request succeeds. A reservation that is not committed or cancelled before its deadline fails the open with `ETIMEDOUT`, so the producer submits its native work and commits promptly.

Errors:

- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_JOB`.
- `EFAULT`: the argument cannot be read, or the result cannot be written back (the reservation is then discarded).
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `flags`, `reserved` or `sequence` is not zero; `fd` is below `-1`; `fd` is `-1` and `generation` is not zero, or `fd` is not `-1` and `generation` is zero; `fd` names a descriptor of the kernel that is not a fence.
- `EBADF`: `fd` is not an open descriptor of the kind that fences use.
- `EXDEV`: the fence belongs to another device.
- `ESTALE`: `generation` is not the fence's current generation.
- `EBUSY`: the fence generation already has a producer, or is no longer pending.
- `ENODEV`: the supervisor of the device is shutting down, the device was withdrawn, or the device or this open failed with `ENODEV`. A failure that is already recorded when the request reaches the reservation is reported with the recorded errno instead (for example `EIO`, `ETIMEDOUT`); a failure that occurs while the request runs is reported as `ENODEV`.
- `EAGAIN`: all `GPU_SUBMIT_MAX` records of the open are in use, or all `GPU_SUBMIT_MAX` fence bindings of the open are in use.
- `EOVERFLOW`: the counter that issues sequences is exhausted.
- Other values: the driver's reserve operation returns its own errno unchanged, and creating the supervisor can fail with the errno of thread creation (for example `ENOMEM`). In every such case no record remains. The Venus driver answers `EINVAL` for a `timeline` of zero or of 64 and above, `ENOTSUP` when the host does not complete native work strictly, `ENODEV` when its transport has failed or is shutting down, `EAGAIN` when it has no free marker slot (it keeps a part of its slots for control traffic), and `EOVERFLOW` when its marker numbering is exhausted.

Sources: [gpu-job.h](../../include/uapi/gpu-job.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_job_reserve_ioctl`, `gpu_job_bind`, `gpu_completion_reserve`, `gpu_monitor_start`), [venus transport](../../src/drivers/gpu/venus/transport.c) (`drv_venus_transport_job_reserve`), [main.c](../../userland/tests/gpu-i915/main.c).

### `GPU_JOB_COMMIT`

Requires: open mode W; capability `GPU_CAP_JOB`.

Tells the kernel that the native work was accepted by the hardware and publishes the job using the storage that the reservation already holds, so publishing needs no allocation and cannot fail for lack of memory. `sequence` names a job in the reserved state of this open. `flags` and `reserved` must be zero. On success the job is committed, the execution deadline replaces the reservation deadline, and a fence tied to the job becomes a valid dependency for the kernel's `*_SYNC` requests ([Job life cycle](#job-life-cycle)). The result of the job is observed with `GPU_COMMAND_WAIT` on the same sequence ([submission](gpu-submission.md)).

The request does not run under the open's admission and does not write to the caller's structure.

If the commit fails after the kernel has checked that the job is reserved (the job, the open or the device ended in between, the fence refuses the transition, or the driver refuses to publish), the native work may already exist. The kernel then does not roll the job back: it asks the driver to cancel the job as a fault, and when the driver accepts, the open fails with `EIO` and the job becomes terminal with the open's errno. The request still returns the error that stopped the commit. When the driver refuses the fault cancel too, the job stays reserved and the caller sees the original error.

Errors:

- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_JOB`.
- `EFAULT`: the argument cannot be read.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `sequence` is zero; `reserved` or `flags` is not zero. A fence that is no longer pending also fails the commit with `EINVAL`.
- `ENOENT`: `sequence` names no job record of this open: it was never issued, belongs to another open, is an ordinary submission, was cancelled with flags zero, or was consumed.
- `EBUSY`: another job request of this open is still working on the same record.
- `EOVERFLOW`: the record already has the largest possible number of observers.
- `EALREADY`: the job is already committed, or it is finished with `status` zero.
- `ENODEV`: the job was ended, this open failed, or the device was withdrawn or failed, between the check and the commit.
- `ESTALE` and `EPERM`: the fence's generation changed (it was reset) or the fence is no longer owned by this job.
- `ETIMEDOUT`, `EIO` and the other errno values that a failure can record: the job is already finished with a failure, and the request returns the record's `status` (the open's recorded errno).
- Other values: the driver's commit operation returns its own errno unchanged; the Venus driver answers `ESTALE` for a token that is no longer reserved and `ENODEV` when its transport has failed or is shutting down.

Sources: [gpu-job.h](../../include/uapi/gpu-job.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_job_action_ioctl`, `gpu_job_observe`, `gpu_job_admit`), [venus transport](../../src/drivers/gpu/venus/transport.c) (`drv_venus_transport_job_commit`), [main.c](../../userland/tests/gpu-i915/main.c).

### `GPU_JOB_CANCEL`

Requires: open mode W; capability `GPU_CAP_JOB`.

Ends a job without a normal completion. `sequence` names a job of this open, and `reserved` must be zero. `flags` selects one of two meanings, and the producer chooses by what it knows about its native work.

With `flags` zero, the producer states that the hardware definitely did not accept the work. This is valid only for a job in the reserved state. The driver releases the reservation, the record is discarded (the sequence names nothing afterwards), and a fence tied to the job stays pending and is unbound, so the producer or another job can use that generation. The device's capacity sequence advances. No other open and no other job is affected, and no error is recorded for the open.

With `GPU_JOB_CANCEL_FAULT`, the producer reports that the work may have reached the hardware, or that the device was lost. This is valid for a reserved or a committed job. The driver keeps the job's callback and memory as uncertain, and the kernel fails the open with `EIO` as a session failure ([submission](gpu-submission.md#session-failure-and-recovery)): the job and every other record of the open that the driver still owns become terminal with the open's errno, fences bound to them are signaled with it, and the memory the work may still access is retained until the driver confirms that it stopped, the context is isolated, or the device is reset. The record stays observable with `GPU_COMMAND_WAIT`.

The request does not run under the open's admission and does not write to the caller's structure. If the driver refuses the cancel, nothing changes, the job keeps its state and deadline, and the driver's error is returned.

Errors:

- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_JOB`.
- `EFAULT`: the argument cannot be read.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `sequence` is zero; `reserved` is not zero; `flags` has a bit other than `GPU_JOB_CANCEL_FAULT`.
- `ENOENT`: `sequence` names no job record of this open (never issued, another open's, an ordinary submission, already discarded, or consumed).
- `EBUSY`: another job request of this open is still working on the same record.
- `EOVERFLOW`: the record already has the largest possible number of observers.
- `EALREADY`: with flags zero, the job is committed; or the job is finished with `status` zero.
- `ETIMEDOUT`, `EIO` and the other errno values that a failure can record: the job is already finished with a failure, and the request returns the record's `status` (the open's recorded errno).
- Other values: the driver's cancel operation returns its own errno unchanged; the Venus driver answers `EINVAL` for a missing token and `ESTALE` when the token no longer names a job in a cancellable state.

Sources: [gpu-job.h](../../include/uapi/gpu-job.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_job_action_ioctl`, `gpu_job_observe`, `gpu_job_unbind`, `gpu_completion_discard`, `gpu_session_fail_locked`), [venus transport](../../src/drivers/gpu/venus/transport.c) (`drv_venus_transport_job_cancel`), no source-tree test.

### `GPU_JOB_CAPACITY`

Requires: open mode none; capability `GPU_CAP_JOB_CAPACITY`.

Observes the node's capacity to take reservations in one completion domain, without owning a job and without reserving anything ([Job life cycle](#capacity)). `domain` selects the completion domain, with the numbering of `timeline` in `GPU_JOB_RESERVE`; the driver validates it. `sequence`, `available` and `reserved` must be zero on input.

- QUERY: `flags` is `GPU_JOB_CAPACITY_QUERY`, and `observed_sequence` and `timeout_ns` must be zero. The request returns at once with the node's current capacity sequence in `sequence`.
- WAIT: `flags` is zero (there is no macro for this mode). `observed_sequence` is a non-zero sequence that an earlier QUERY or WAIT returned. The request returns when the node's capacity sequence is different from `observed_sequence`, or when there is room now. `timeout_ns` bounds the wait: zero does not wait and fails with `EAGAIN` when nothing has changed and there is no room; a finite value waits at most that long, rounded up to the kernel's clock tick, and fails with `ETIMEDOUT`; `UINT64_MAX` waits without a limit, interruptibly. After a failure the caller can wait again with the same `observed_sequence`.

On success `sequence` holds the node's current capacity sequence, to pass to the next WAIT, and `available` is 1 when the driver reported capacity in `domain` and the calling open had a free record, and 0 otherwise. A WAIT can therefore return with `available` 0 because the sequence changed. The answer is a hint: another caller can reserve first. A failed open does not wait for room it can no longer use: the request fails with the open's errno, also while waiting.

The request does not run under the open's admission, so other requests of the same open, including `GPU_JOB_RESERVE`, proceed while it waits. It does not read the open's mode.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_JOB_CAPACITY`.
- `EFAULT`: the argument cannot be read, or the result cannot be written back (observation does not consume anything).
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; `flags` has a bit other than `GPU_JOB_CAPACITY_QUERY`; `sequence`, `available` or `reserved` is not zero; QUERY with a non-zero `observed_sequence` or `timeout_ns`; WAIT with a zero `observed_sequence`, or one larger than the node's current sequence.
- `EOVERFLOW`: `timeout_ns` is so large that the deadline cannot be represented, or the capacity sequence could not advance past its largest value.
- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records; a driver can record others).
- `EAGAIN`: WAIT with `timeout_ns` zero and no change.
- `ETIMEDOUT`: a finite non-zero `timeout_ns` expired.
- `EINTR`: a signal ended the wait.
- Other values: the driver's capacity operation returns its own errno unchanged. The Venus driver answers `EINVAL` for a `domain` of zero or of 64 and above, `ENOTSUP` when the host does not complete native work strictly, and `ENODEV` when its transport has failed or is shutting down.

Sources: [gpu-job.h](../../include/uapi/gpu-job.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_capacity_ioctl`, `gpu_capacity_changed_locked`, `gpu_retirement_changed_locked`, `gpu_session_error_locked`), [venus transport](../../src/drivers/gpu/venus/transport.c) (`drv_venus_transport_capacity`), no source-tree test.

### `GPU_JOB_POLICY`

Requires: open mode none; capability `GPU_CAP_JOB`.

Returns the supervision intervals of this kernel build as one fixed snapshot, in nanoseconds ([Supervision](#supervision)). It is read-only: nothing in the request changes a deadline, and a client cannot extend one. All input fields except `version` and `size` must be zero. The three intervals are the build settings in milliseconds times 1000000, before the kernel rounds a deadline up to its clock tick, and are the same for every open of every node. `flags` and `reserved` are returned as zero. Waiting for capacity ([`GPU_JOB_CAPACITY`](#gpu_job_capacity)) is not bounded by any of these intervals.

The request does not run under the open's admission and does not check the health of the open or the device, so a program can read the policy of a node whose context failed.

Errors:

- `EOPNOTSUPP`: the node does not offer `GPU_CAP_JOB`.
- `EFAULT`: the argument cannot be read, or the result cannot be written back.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the structure size; any of the three timeout fields, `flags` or `reserved` is not zero.
- Other values: none. The request does not call the driver, and every errno comes from the checks above.

Sources: [gpu-job.h](../../include/uapi/gpu-job.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_policy_ioctl`), no source-tree test.

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_JOB_CANCEL_FAULT` | 1 | The only flag of `GPU_JOB_CANCEL` in `struct gpu_job_action`. Reports native work that may have reached the hardware, or a lost device: the open fails with `EIO`, the job becomes terminal, and the memory the work may access is retained. Without it, `GPU_JOB_CANCEL` states that the hardware did not accept the work. `GPU_JOB_COMMIT` refuses it. |
| `GPU_JOB_CAPACITY_QUERY` | 1 | The only flag of `struct gpu_job_capacity`. Selects the immediate query; without it the request is a wait. |

## Structures

#### `struct gpu_job_reserve`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 4 | `int32_t` | `fd` | in | A fence descriptor of this device whose generation the job will signal, or `-1` for a job observed only through its completion record. A value below `-1` is refused. |
| 12 | 4 | `uint32_t` | `flags` | zero | Reserved; must be zero. |
| 16 | 8 | `uint64_t` | `generation` | in | The pending generation of the fence `fd` that the job owns. Non-zero when `fd` is not `-1`, and zero when it is. The generation is not a GPU dependency until the job is committed. |
| 24 | 4 | `uint32_t` | `timeline` | in | Completion domain of the node in which the driver places the job's marker; the driver validates it. The Venus driver accepts 1 to 63. |
| 28 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 32 | 8 | `uint64_t` | `sequence` | out | The identity of the job's completion record, non-zero and unique across the system. Must be zero on input. It is written before the driver is asked to reserve, and is meaningless when the request fails. |

#### `struct gpu_job_action`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 24. |
| 8 | 8 | `uint64_t` | `sequence` | in | The `sequence` that `GPU_JOB_RESERVE` returned on this open. Must not be zero. |
| 16 | 4 | `uint32_t` | `flags` | in | `GPU_JOB_COMMIT`: must be zero. `GPU_JOB_CANCEL`: zero for definite non-acceptance, or `GPU_JOB_CANCEL_FAULT`. No other bit is defined. |
| 20 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |

#### `struct gpu_job_capacity`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 48. |
| 8 | 4 | `uint32_t` | `flags` | in | `GPU_JOB_CAPACITY_QUERY` for a query, or zero for a wait. No other bit is defined. |
| 12 | 4 | `uint32_t` | `domain` | in | Completion domain of the node whose capacity is observed, numbered like `timeline` of `GPU_JOB_RESERVE`. The driver validates it. Used by both modes. |
| 16 | 8 | `uint64_t` | `timeout_ns` | in | Wait only. How long to wait, in nanoseconds: zero does not wait, `UINT64_MAX` waits without a limit, other values are rounded up to the kernel's clock tick. Must be zero for a query. |
| 24 | 8 | `uint64_t` | `observed_sequence` | in | Wait only. A non-zero capacity sequence returned by an earlier call; it must not be larger than the node's current sequence. Must be zero for a query. |
| 32 | 8 | `uint64_t` | `sequence` | out | The node's capacity sequence at the moment of the answer. Must be zero on input. |
| 40 | 4 | `uint32_t` | `available` | out | 1 when the driver reported capacity in `domain` and the open had a free record, otherwise 0. A hint only. Must be zero on input. |
| 44 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |

#### `struct gpu_job_policy`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 8 | `uint64_t` | `reservation_timeout_ns` | out | How long a reservation may wait for its commit or cancel, in nanoseconds. Must be zero on input. |
| 16 | 8 | `uint64_t` | `execution_timeout_ns` | out | How long a committed job may take to complete, in nanoseconds. Must be zero on input. |
| 24 | 8 | `uint64_t` | `stop_timeout_ns` | out | How long the kernel waits for the driver to confirm that a failed open's native work has stopped, in nanoseconds. Must be zero on input. |
| 32 | 4 | `uint32_t` | `flags` | zero | Must be zero; returned as zero. |
| 36 | 4 | `uint32_t` | `reserved` | zero | Must be zero; returned as zero. |
