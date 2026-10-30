# GPU device interface

Status: experimental reference for GPU ABI version 1.

`/dev/gpuN` is the user API of the kernel GPU framework. A process opens a
node and talks to it with `ioctl`, `poll` and `mmap`. The node exposes a
GPU's rendering and display work to libvulkan, to the Keiland compositor and
to the test programs that drive the kernel directly. Every node is backed by
one driver; the framework in the kernel validates each request, keeps the
per-open state and passes the request to the driver.

This file holds the rules shared by every request: the nodes and who may open
them, the calling conventions, concurrency and waiting, and compatibility.
[Request index and errors](gpu-requests.md) lists every request with its open
mode and capability requirement and the error meanings shared across the
interface. The other `gpu-*.md` files describe the requests by area and are
listed below.

The ABI is experimental. Requests may still be added within ABI version 1, so
a program does not assume that a request exists because its number is in a
header. A program learns what a node offers from the capability bits that
`GPU_GET_INFO` returns ([capabilities](gpu-capabilities.md)). A request that
the node does not implement fails with `EOPNOTSUPP`, and so does a request
number the kernel does not know. GPU drivers are built for amd64, aarch64 and
riscv64; [Drivers and node kinds](gpu-drivers.md) names the drivers and what
their nodes offer. On a build without a GPU driver no `/dev/gpuN` node exists.
The operations a driver implements internally are not user API and are not
described here.

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

| Header | Contents |
| --- | --- |
| [`gpu.h`](../../include/uapi/gpu.h) | `GPU_ABI_VERSION`; the base capability bits; the size limits; `GPU_PIXEL_BGRA8888` and `GPU_PIXEL_RGBA8888`; requests 0 to 13 (info, resources, capsets, blobs, transfers, commands, presents, mappings, resource export and import, command submission and completion wait). |
| [`gpu-allocation.h`](../../include/uapi/gpu-allocation.h) | `GPU_CAP_ALLOCATION_SHARE`; requests 14 and 15 (allocation export and import with an application-defined description). |
| [`gpu-fence.h`](../../include/uapi/gpu-fence.h) | `GPU_CAP_FENCE`; requests 16 to 23 (fence create, query, wait, reset, signal and bind, and the two requests that wait for a fence before submitting or presenting). |
| [`gpu-display.h`](../../include/uapi/gpu-display.h) | The display capability bits, the output flags and the `GPU_DISPLAY_FORMAT_*` bits; requests 24 to 29, 32 and 39 to 45 (outputs, modes, leases, presentation, events, power, refresh, cursor, commit and overlay limits). |
| [`gpu-scanout.h`](../../include/uapi/gpu-scanout.h) | Node roles, scanout routes and placement conditions; requests 30, 31 and 33 (device query, display constraints, placed blobs). |
| [`gpu-job.h`](../../include/uapi/gpu-job.h) | `GPU_CAP_JOB` and `GPU_CAP_JOB_CAPACITY`; requests 34 to 38 (job reserve, commit, cancel, capacity and policy). |
| [`gpu-op.h`](../../include/uapi/gpu-op.h) | The command numbers of the stream that `GPU_COMMAND` and `GPU_COMMAND_SUBMIT` carry. It defines no ioctl request. |
| [`sysctl.h`](../../include/uapi/sysctl.h) | Only the `hw.gpu.*` variables and `hw.gputelemetry`: `HW_GPU_ATTACHING`, `HW_GPU_START`, `HW_GPUTELEMETRY` and `HW_GPU_STAGES`. |

All request numbers use ioctl group `G`. The headers include
`<uapi/ioctl.h>`, which builds a request number from the direction, the
size of the argument structure, the group and the request number. A compiler
that targets zedBSD defines `__ZEDBSD__`, and with it the request macros are
zedBSD's own. A build that defines neither `__ZEDBSD__` nor `KERN_UAPI_NATIVE`
takes the `_IO` macros of the host's `<sys/ioctl.h>` instead, and the numbers
it gets are the host's, not the kernel's. A program that talks to a node is
therefore built with a target compiler. The kernel compares the whole request
value, size included, so a number built from a different argument size is an
unknown request.

Sources: [`uapi/ioctl.h`](../../include/uapi/ioctl.h),
[`uapi/hosted.h`](../../include/uapi/hosted.h),
`gpu_ioctl_body` in [`gpu.c`](../../src/drivers/gpu/gpu.c).

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_ABI_VERSION` | 1 | The ABI version. A caller puts it in the `version` field of every top-level argument, and the kernel writes it back in the replies it fills. |

## Nodes and permissions

**Names.** A node is `/dev/gpuN`. The framework gives a newly registered
device the lowest number that no registered device holds. Numbers are
assigned in probe order, so the same GPU can get a different number after a
reboot or a hardware change. A number is free again only after the device
has been removed, every open of it has been closed and every resource or
allocation descriptor exported from it has been closed; until then the
driver's removal does not complete and a new device does not reuse the
number. A node is published as soon as its driver
registers it, whether or not devfs is mounted. The sysctl variable
`hw.gpu.attaching` counts the devices that have attached but not yet
published a node ([GPU sysctl variables](gpu-sysctl.md)).

A program that needs to tell nodes apart, or to pair a display node with a
render node, does not compare numbers. It issues `GPU_DEVICE_QUERY` on each
node: `device_id` is the kernel's identity of the node, never reused while the
kernel runs, and it is different for every node, even for two nodes of one
physical GPU. `roles` says what the node does, and `companion_id` names the
driver's preferred companion node when the driver has one ([Memory](gpu-memory.md)).

**Several nodes for one GPU.** A physical GPU may publish more than one node,
for example a render node and a display node. Each node is registered
separately and gets its own number and its own `device_id`. Which drivers
split a GPU this way, and what each kind of node offers, is in
[Drivers and node kinds](gpu-drivers.md).

**Who may open.** `gpu_open` admits a caller whose credential is the
superuser, or whose effective user ID equals the owner of the node. It refuses
anyone else with `EACCES`, and also refuses a caller that has no credential.
This rule is applied in addition to the node's file mode: the file mode must
let the open through first, and then `gpu_open` applies the rule above. The
group and other permission bits therefore do not admit a user who is not the
owner.

devfs creates the node with owner root and mode 0666, so without a graphical
login only root opens a GPU node. When the graphical login runs, the seat
manager (sessiond) changes the owner of `/dev/gpu0` to `/dev/gpu3` to the seat
user and the mode to 0600, both for the login screen's account and for a
session's user. It returns them to root and mode 0666 when it stops. devfs
remembers the owner and mode given to a node by its device number, so a node
that is registered again under the same number comes back with them. The seat
manager handles only the nodes from `/dev/gpu0` to `/dev/gpu3`. A node with a
higher number, or any node while no seat manager has given it away, keeps its
owner root, and a user other than root gets `EACCES` from it whatever the
mode. Giving a node away does not take it from a process that already has it
open.

**Read and write permission.** The open's access mode decides what the open
may do. The kernel records it at open time and requests use the record, not
later credential changes.

| Access mode | Readable open | Writable open |
| --- | --- | --- |
| `O_RDONLY` | yes | no |
| `O_WRONLY` | no | yes |
| `O_RDWR` | yes | yes |

Requests that observe state need a readable open; requests that change state,
create objects or export capabilities need a writable open; a few need neither.
No request needs both. The `Open mode` column of the [index](gpu-requests.md) gives the
requirement of each request, and a request that is refused for this reason
fails with `EACCES`. `mmap` needs a readable open, and it refuses a
protection bit beyond the open's rights, so `PROT_WRITE` needs a writable
open ([Memory](gpu-memory.md)). The limit also applies to later `mprotect`
calls on the mapping.

**A failed device.** After a device fault, requests fail with the error the
driver reported, which this document calls the device's recorded error: every
request that takes the admission, the display requests that run outside it,
`GPU_JOB_CAPACITY`, and every new submission or job reservation. Requests that
only look at what the open already has still answer: `GPU_COMMAND_WAIT`
reports the records the open holds, `GPU_JOB_POLICY` reports the policy, and
the fence requests other than `GPU_FENCE_BIND` do not look at the device's
state. A request that binds a fence to new work (`GPU_FENCE_BIND`, and a
`GPU_COMMAND_SUBMIT_SYNC`, `GPU_DISPLAY_PRESENT_SYNC` or `GPU_DISPLAY_COMMIT`
that names a signal fence, once its wait fence has completed) fails with
`ENODEV` on a failed device, whatever error the device recorded. An open on the failed
device fails with the recorded error unless it recovers the device. A device can recover
only when `gpu_recover_open` finds that the new open is the only one: every
older open of the device has been closed, no capability exported from the
device is still held, and the driver offers a reset. The open then resets the
hardware, clears the recorded error on success and succeeds. A reset failure
fails the open with the error the reset reported. Old objects are never
revived; the fresh open starts with new state. If the device faults again
while the reset runs, the open fails with the new recorded error, or with
`EIO` when that fault can no longer be told apart from the reset.

An open that arrives while another open is resetting the same device waits
for the result. The wait is interruptible, so a signal ends it with `EINTR`,
and it ends when the reset finishes or the device is withdrawn (`ENODEV`).
Once it ends, the open continues as above: it succeeds if the reset cleared
the error and fails with the recorded error otherwise. An open can also fail
with `ENODEV` when the device has been withdrawn, with `EOVERFLOW` when the
device already has the largest possible number of opens, with `ENOMEM`, and
with the driver's own error when the driver's open fails.

Comments elsewhere in the tree describe node access differently. A comment in
the seat manager's source says that a node given back to root is one that
anyone may open again. `gpu_open` is what decides, and it admits only root and
the owner.

Sources: `gpu_publish_node`, `drv_gpu_register`, `drv_gpu_unregister`,
`gpu_open`, `gpu_recover_open`, `gpu_device_query_ioctl` and `gpu_mmap` in
[`gpu.c`](../../src/drivers/gpu/gpu.c); the node owner and mode in
[`devfs.c`](../../src/kern/devfs.c); `sessiond_seat_give` and
`sessiond_seat_restore` in
[`seat.c`](../../userland/desktop/sessiond/seat.c);
[`fcntl.h`](../../include/uapi/fcntl.h); tests
[`gpu-admission`](../../userland/tests/gpu-admission/main.c) and
[`gpu-recovery`](../../userland/tests/gpu-recovery/main.c).

## Calling conventions

**Requests.** Every request is `ioctl(fd, REQUEST, &argument)` with one
fixed-width structure and no embedded user pointers except where a field is
documented as an address. The structures have the same layout on ILP32 and
LP64; the tables in these documents are checked on both.

**Version and size.** A top-level argument begins with two fields: `version`
set to `GPU_ABI_VERSION` and `size` set to the size the request expects, which
is the `Bytes` column of the [index](gpu-requests.md). When either differs the
kernel returns `EINVAL` before any driver sees the request. The exceptions
are:

- `GPU_COMMAND_SUBMIT_SYNC` and `GPU_DISPLAY_PRESENT_SYNC` carry no header of
  their own. The nested `struct gpu_command_submit` or `struct
  gpu_display_present` at the start carries `version`, and `size` is the size
  of that nested structure, not of the whole argument.
- `struct gpu_blob_create_placed` begins with a `struct gpu_blob_create`
  whose `size` is the size of the outer structure (64). `GPU_BLOB_CREATE`
  itself keeps its own size, 40.
- `struct gpu_display_plane_state` and `struct gpu_placement` have no header.
  They are parts of `struct gpu_display_commit` and of `struct
  gpu_blob_create_placed`, and the enclosing structure carries the header.
  The kernel checks their fields (flags, reserved fields, ranges) and returns
  `EINVAL` for an invalid one.
- `struct gpu_image_descriptor` and `struct gpu_allocation_descriptor` carry
  their own `version` and `size`. On export the kernel checks them like a
  top-level header: both must match, and the description must pass the
  request's checks, otherwise `EINVAL` (or, for an image descriptor,
  `EOPNOTSUPP` for an unsupported tiling, memory type or pixel format). On import the descriptor is an output: it must be
  all zero on input, and the kernel returns the description stored with the
  exported capability.

**Input, output and zero fields.** A field the tables mark `in` or `in/out`
is read by the kernel. Reserved fields, the fields marked `zero`, and fields
that the kernel fills (handles, identities, sequences, results) must be zero
on input where the kernel checks them, and a nonzero value fails with
`EINVAL`. Where a request returns a file descriptor, the descriptor field
must be -1 on input, and a dependency that is absent is written as descriptor
-1 with generation 0. Each request section states its own checks.

**Return and output on failure.** A request returns 0 on success. On failure
`ioctl` returns -1 and sets `errno`. The kernel copies an argument in at the
start, validates it, performs the operation and copies the reply out only
after the operation has succeeded. A failed request therefore normally leaves
the caller's structure as it was, and a caller must not read its output
fields after a failure. A few requests copy a result out before a later step
that can still fail (for example `GPU_JOB_RESERVE` returns the reserved
identity before the driver accepts the reservation and withdraws it on
failure); the area documents note such exceptions. A request whose copy-out
fails returns the copy's error.

**Unknown requests.** A request number that no case of `gpu_ioctl_body`
matches fails with `EOPNOTSUPP`, not `ENOTTY`. It first takes the admission,
so on a failed or withdrawn device it fails with the admission's error
instead, and a signal during the admission wait ends it with `EINTR`. A request whose capability the
node lacks fails with `EOPNOTSUPP` as well.

**Handles, identifiers and sequences.** The 64-bit handles in these requests
are local to the open file description: a resource handle names an object in
the table of the open that created it. A handle that is zero, that was
destroyed, or that another open created identifies no object, and the request
fails with `EINVAL`. Handles are never reused while the kernel runs, so a
stale handle cannot name a new object. A command sequence that is unknown or
already consumed fails with `ENOENT`. A fence is named by a descriptor and a
generation: a descriptor that is not an open kernel handle descriptor of the caller fails
with `EBADF`, a kernel handle descriptor that is not a fence (for example an
exported resource) fails with `EINVAL`, a fence made by another device fails with `EXDEV`, and an old
generation fails with `ESTALE`. Display leases and outputs have their own
identities and errors ([Display](gpu-display.md)).

**Pixel formats.** `GPU_PIXEL_BGRA8888` and `GPU_PIXEL_RGBA8888` are values
(1 and 2). They appear in a field of one structure: the format of a present,
of a display present, of a cursor image, of a plane state and of an image
descriptor. `GPU_DISPLAY_FORMAT_BGRA8888` and `GPU_DISPLAY_FORMAT_RGBA8888`
are bits of a mask (1 and 2) that a display or a scanout route reports as
`formats`: the formats an output, an overlay or a route accepts. A caller
turns a supported bit into the matching value before it presents.

Sources: `gpu_ioctl_body`, `gpu_ioctl`, `gpu_info_ioctl`,
`gpu_dependency_ioctl`, `gpu_blob_ioctl`, `gpu_image_validate`,
`gpu_allocation_validate`, `gpu_resource_lookup`, `gpu_job_reserve_ioctl`,
`gpu_fence_state_ioctl` and `gpu_handle_allocate` in
[`gpu.c`](../../src/drivers/gpu/gpu.c); `cdev_ioctl_file` in
[`cdev.c`](../../src/kern/cdev.c); [`gpu.h`](../../include/uapi/gpu.h);
[`gpu-display.h`](../../include/uapi/gpu-display.h).

## Concurrency, blocking and cancellation

**Requests that run one at a time.** Each open file description has one
admission. The requests below take it, so on one open they run one at a time,
from the check of the request to the copy-out of the reply:

- `GPU_GET_INFO`, `GPU_DEVICE_QUERY`, `GPU_DISPLAY_CONSTRAINTS`;
- `GPU_RESOURCE_CREATE`, `GPU_RESOURCE_DESTROY`, `GPU_GET_CAPSET`,
  `GPU_BLOB_CREATE`, `GPU_BLOB_CREATE_PLACED`, `GPU_RESOURCE_READ`,
  `GPU_RESOURCE_WRITE`, `GPU_RESOURCE_MAP`, and `mmap` of a resource;
- `GPU_COMMAND`, `GPU_COMMAND_SUBMIT`, `GPU_PRESENT`;
- `GPU_RESOURCE_EXPORT`, `GPU_RESOURCE_IMPORT`, `GPU_ALLOCATION_EXPORT`,
  `GPU_ALLOCATION_IMPORT`;
- the display requests `GPU_DISPLAY_QUERY`, `GPU_DISPLAY_MODE`,
  `GPU_DISPLAY_CLAIM`, `GPU_DISPLAY_RELEASE`, `GPU_DISPLAY_PRESENT`,
  `GPU_DISPLAY_WAIT`, `GPU_DISPLAY_POWER`, `GPU_DISPLAY_CURSOR_SET`,
  `GPU_DISPLAY_CURSOR_MOVE` and `GPU_DISPLAY_CURSOR_HIDE`, except where the
  driver declares ungated display (see below);
- the submit or present step of `GPU_COMMAND_SUBMIT_SYNC` and
  `GPU_DISPLAY_PRESENT_SYNC`, after their wait.

These requests run outside the admission, so they may run in parallel with
each other and with a request that holds the admission, on the same open:

- `GPU_JOB_RESERVE`, `GPU_JOB_COMMIT`, `GPU_JOB_CANCEL`, `GPU_JOB_CAPACITY`,
  `GPU_JOB_POLICY`;
- `GPU_COMMAND_WAIT`;
- `GPU_DISPLAY_EVENTS`, `GPU_DISPLAY_REFRESH`, `GPU_DISPLAY_COMMIT`;
- the six fence requests `GPU_FENCE_CREATE`, `GPU_FENCE_QUERY`,
  `GPU_FENCE_WAIT`, `GPU_FENCE_RESET`, `GPU_FENCE_SIGNAL` and
  `GPU_FENCE_BIND`;
- the wait of `GPU_COMMAND_SUBMIT_SYNC` and `GPU_DISPLAY_PRESENT_SYNC` for
  their wait fence, which happens before the request takes admission;
- where the driver declares ungated display (the ghci and bcm2711 drivers do,
  the others do not): `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_WAIT`,
  `GPU_DISPLAY_CURSOR_SET`, `GPU_DISPLAY_CURSOR_MOVE`,
  `GPU_DISPLAY_CURSOR_HIDE` and `GPU_DISPLAY_OVERLAY_INFO`, and the present
  step of `GPU_DISPLAY_PRESENT_SYNC`. These requests still refuse an open
  whose device or context has failed. Where the driver does not declare
  ungated display, `GPU_DISPLAY_OVERLAY_INFO` has no case in the dispatch and
  fails with `EOPNOTSUPP`, and `GPU_DISPLAY_COMMIT` fails with `EOPNOTSUPP`
  because only such a driver has a commit operation.

A request that needs the admission while another thread holds it waits for
it. The wait is interruptible: a signal ends it with `EINTR`. It also ends,
and the request fails, when the device is withdrawn (`ENODEV`), when the
device has a recorded error (that error), or when the open has failed (the
open's error). A request that already has the admission is not affected by
requests outside it. `O_NONBLOCK` has no effect on any GPU request.

**Opens and nodes.** Each open has its own table of resources, its own
completion records and its own fence bindings. Two opens of the same node
share the device: the hardware; a device-wide fault or withdrawal, which fails
every open (a fault that only one open suffers fails that open alone); the
capacity of the driver's job queue, which `GPU_JOB_CAPACITY` reports
together with the open's own free completion records; and the display, whose lease belongs to one open at a time (another
open's request on that output fails with the driver's lease error, usually
`EBUSY`; some requests of the i915 and venus drivers answer `EINVAL` or
`EPERM` instead ([Display](gpu-display.md))). Capabilities that an open exports (descriptors from
`GPU_RESOURCE_EXPORT`, `GPU_ALLOCATION_EXPORT` and `GPU_FENCE_CREATE`) can be
used by another open or process. A resource or allocation descriptor keeps
the device from recovering and from being removed until it is closed. A
fence descriptor does not: a fence names its device only by `device_id`, so
the device can recover or be removed while one is open. Different nodes are different devices to
the framework, even when they come from one physical GPU. They have separate
error state, separate opens and separate `device_id` values, and they share
only what a program passes between them: exported allocations and fences. A
fence from one node is accepted by another only as the wait dependency of a
display present or commit; any other use of a foreign fence fails with
`EXDEV`.

**Blocking requests.** The table lists the requests that can sleep and what
bounds the sleep. A wait whose time comes from a `timeout_ns` field computes
its deadline from the field on each call and rounds it up to a whole scheduler
tick.

| Request | Timeout field | Zero | Finite | UINT64_MAX | Upper bound per call | Interrupted by a signal |
| --- | --- | --- | --- | --- | --- | --- |
| `GPU_COMMAND_WAIT` | `timeout_ns` | Observes: `EAGAIN` if the sequence is not complete | Waits up to the time, then `ETIMEDOUT` | Waits until completion | None | Yes, `EINTR` |
| `GPU_FENCE_WAIT` | `timeout_ns` | Observes: `EAGAIN` while the generation is pending | Waits up to the time, then `ETIMEDOUT` | Waits until the fence leaves pending | None | Yes, `EINTR` |
| `GPU_JOB_CAPACITY` (wait mode; the query flag never waits) | `timeout_ns` | Observes: `EAGAIN` if nothing changed and there is no capacity | Waits up to the time, then `ETIMEDOUT` | Waits | None | Yes, `EINTR` |
| `GPU_DISPLAY_WAIT` | `timeout_ns` | ghci and bcm2711: `EAGAIN` if the sequence is not adopted. Other drivers answer at once (`EAGAIN` before the first frame) | ghci and bcm2711: up to the time, then `ETIMEDOUT`. Others: not used | ghci and bcm2711: capped to one second. Others: not used | One second on ghci and bcm2711, applied by the driver. Others never sleep, except that the i915 node ignores the field and can wait for the flip of its last present | bcm2711: yes, `EINTR`. ghci and the others: no |
| `GPU_DISPLAY_REFRESH` | `timeout_ns` | One check: `ETIMEDOUT` if no boundary after `cursor` has come (`cursor` 0 answers at once) | Waits up to the time, then `ETIMEDOUT` | Capped to one second | One second, applied by the kernel before the driver runs | No in the drivers read (amdrdna, i915, nvrtx and venus sleep without checking for signals) |
| `GPU_DISPLAY_PRESENT`, and `GPU_DISPLAY_COMMIT` with `GPU_DISPLAY_COMMIT_WAIT`, on drivers with ungated display | None | Not applicable | Not applicable | Not applicable | The driver's adoption wait: about 100 ms on bcm2711, about 12 seconds on ghci; then `ETIMEDOUT` | No |
| `GPU_COMMAND_SUBMIT_SYNC`, `GPU_DISPLAY_PRESENT_SYNC`, and the plane fences of `GPU_DISPLAY_COMMIT` | None | Not applicable | Not applicable | Not applicable | None; the supervision of the producing job ends it. A fence whose producer work was never admitted fails at once with `EAGAIN` | Yes, `EINTR` |
| `GPU_RESOURCE_DESTROY` | None | Not applicable | Not applicable | Not applicable | Waits for a display callback that still uses the resource; no limit besides the callback | Yes, `EINTR`; the resource is unchanged |

`GPU_DISPLAY_EVENTS` never waits: it samples the state and returns. A present
on a driver without ungated display returns when the driver has shown the
frame, and `GPU_PRESENT` and `GPU_COMMAND` return when the driver has
accepted the work; how long that takes is the driver's ([Display](gpu-display.md),
[Submission](gpu-submission.md)). The open of a node can also wait, as the
section on nodes describes.

**Cancellation.** There is no request that cancels a wait. A signal ends an
interruptible wait with `EINTR`. `ioctl` is among the system calls that the
kernel restarts: if the signal's handler was installed with `SA_RESTART`, the
call runs again with its original arguments and a wait with a `timeout_ns`
starts over with the full time; without `SA_RESTART`, `ioctl` returns -1 with
`EINTR`. A request that is interrupted or times out leaves the open's state as
it was: a completion record stays readable and a consuming wait does not
consume it, no reservation is made, a fence keeps its state, a submit step
that was waiting for a fence submits nothing, and a lease keeps its owner. A
`close` from another thread does not end a wait: a request in progress holds
the open file description, and the final close runs after it returns.

**Closing.** When the last reference to the open file description goes away,
the kernel closes the open. It stops new work from the open, withdraws job
reservations that were never committed and lets committed jobs finish. Every
fence the open has bound ends with the open's recorded error (`ENODEV` when the
open had not failed before), except a fence that a still running committed job
owns, which that job signals when it finishes; a fence still bound after that
ends with `ENODEV`. It then
destroys every resource of the open and closes the driver's session. A display
lease that the open held ends with it, and a claimed lease keeps the keyboard
from the text console until the lease is released or the open is closed. The
area documents describe the effect on jobs ([Jobs](gpu-jobs.md)), on fences
([Fences](gpu-fences.md)) and on leases and the images a display borrows
([Display](gpu-display.md)).

**poll.** `poll` on a GPU descriptor reports these events. Every one of them
is level-triggered, and failure events are reported whatever events the
caller asked for.

| Event | When |
| --- | --- |
| `POLLIN` and `POLLRDNORM`, each only if requested | The open has a completion record that is complete and not yet consumed (`GPU_COMMAND_WAIT` with `GPU_WAIT_CONSUME` consumes it). The open mode does not matter. |
| `POLLPRI` | The caller asked for it, the open is readable, the node has `GPU_CAP_DISPLAY_EVENTS`, and the driver's topology sequence is above the sequence this open has acknowledged with `GPU_DISPLAY_EVENTS` (a new open has acknowledged none). If the sequence cannot be sampled, `POLLERR` is reported instead. |
| `POLLERR` | The device has a recorded error, or this open has failed, or the topology sequence could not be sampled. |
| `POLLERR` and `POLLHUP` | The device has been withdrawn. |

A fence descriptor polls separately from the GPU descriptor: it reports
`POLLIN` and `POLLRDNORM` when the fence is not pending (signaled or failed)
and `POLLERR` when it failed, and it stays that way until the fence is reset
([Fences](gpu-fences.md)).

Sources: `gpu_ioctl_body`, `gpu_session_enter`, `gpu_session_leave`,
`gpu_display_ungated`, `gpu_display_admit`, `gpu_dependency_ioctl`,
`gpu_dependency_wait`, `gpu_commit_ioctl`, `gpu_completion_observe`,
`gpu_completion_deadline`, `gpu_capacity_ioctl`, `gpu_refresh_ioctl`,
`gpu_destroy_ioctl`, `gpu_close`, `gpu_session_fail_locked` and `gpu_poll` in
[`gpu.c`](../../src/drivers/gpu/gpu.c); `fence_wait` and `fence_poll` in
[`gpu-fence.c`](../../src/drivers/gpu/gpu-fence.c); `waitq_sleep` in
[`waitq.c`](../../src/kern/waitq.c); the restart policy in
[`syscall.c`](../../src/kern/syscall.c); tests
[`gpu-admission`](../../userland/tests/gpu-admission/main.c) and
[`gpu-recovery`](../../userland/tests/gpu-recovery/main.c).

## Compatibility

This is zedBSD's own ABI. It is not the Linux DRM interface and not the
FreeBSD drm-kmod interface: it does not accept their ioctls and does not use
their headers, and no part of it is "DRM compatible". The command stream that
`GPU_COMMAND` and `GPU_COMMAND_SUBMIT` carry uses the command numbers of the
Venus protocol's wire format 1 as shipped in virglrenderer 1.1.0, so that a
Venus renderer on a virtual machine's host accepts the stream. The commands of
zedBSD's own start at a separate number. [Command stream](gpu-command-stream.md)
names the protocol version, which is `GPU_OP_PROTOCOL_VERSION` 2 in the
header, and the commands.

The Vulkan driver, libvulkan, is the consumer this interface is shaped for,
and applications reach a GPU through it. Where a header names a Vulkan
feature, that is the mapping: `GPU_DISPLAY_POWER` is the request behind
`VK_EXT_display_control`, and `GPU_DISPLAY_OVERLAY_INFO` reports the limits
that `vkGetDisplayPlaneCapabilitiesKHR` returns for an overlay plane. The
interface does not otherwise map Vulkan features onto requests; the area
documents describe each request on its own terms.

Sources: [`gpu-op.h`](../../include/uapi/gpu-op.h),
[`gpu-display.h`](../../include/uapi/gpu-display.h).

## Related documents

- [Console, graphics and system controls](control-devices.md) describes
  `/dev/console` and `/dev/graphics`. A GPU node is a different interface:
  display ownership on `/dev/gpuN` is a lease from `GPU_DISPLAY_CLAIM`
  ([Display](gpu-display.md)), and the first image that a driver accepts for
  display ends the text console's use of the display.
- [VisionFive 2 GPU](visionfive2-gpu.md) covers the GPU of that board.
- [Kernel boot parameters](kernel-boot-parameters.md) describes `login=`,
  whose graphical value depends on `/dev/gpu0` being published.
- [Kernel, HAL and driver boundaries](../architecture/kernel-and-hal.md)
  describes how drivers sit between the kernel and the hardware.
