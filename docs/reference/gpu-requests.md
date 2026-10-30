# GPU request index and errors

Status: experimental reference for GPU ABI version 1.

Every request of the `/dev/gpuN` interface is listed below in one table, with
the open mode and the capability it needs and the document that describes it.
After the table come the errno values whose meaning is the same across the
interface. The calling conventions that all requests share (version and size
fields, input and output fields, handles, blocking and cancellation) are in
[GPU device interface](gpu.md#calling-conventions); the request sections
themselves are in the documents named in the last column.

## Request index

**Open mode.** The open mode is the access mode of the `open` that created the
file description ([permissions](gpu.md#nodes-and-permissions)). `R` means the
request needs a readable open (`O_RDONLY` or `O_RDWR`), `W` a writable open
(`O_WRONLY` or `O_RDWR`), and `none` that the request looks at neither. No
request needs both. A request refused for its open mode fails with `EACCES`.

**Capability.** The capability column lists the bits of the `capabilities`
field of `GPU_GET_INFO` that the kernel always checks before it calls the
driver, including the bits that a request it runs inside itself checks. When
two bits are listed, joined by `and`, both are required. A request whose node
lacks a required bit fails with `EOPNOTSUPP`. `none` means that no bit is
checked in every case. That does not mean that the request always works:
some requests depend on a bit only for some flags (for example
`GPU_BLOB_SHAREABLE` on `GPU_BLOB_CREATE`), or on a condition that is not a
bit, such as an operation the driver does not implement, a display operation
or the presence of a scanout route. Such conditions are in the text and the
`Errors:` list of the request section and not in this table. In particular,
`GPU_RESOURCE_DESTROY` is available on a node with `GPU_CAP_RESOURCE` or
`GPU_CAP_BLOB` and also on a node whose scanout route can import images, and
`GPU_RESOURCE_IMPORT` needs `GPU_CAP_SHARE` or a scanout route that can import
images ([sharing](gpu-sharing.md)). Both therefore show `none`.
`GPU_DEVICE_QUERY` and `GPU_GET_INFO` need no bit at all.

**Order of the checks.** When a request fails both checks, the errno depends on
which one the kernel makes first. The display requests (including
`GPU_DISPLAY_COMMIT` and excluding `GPU_DISPLAY_PRESENT_SYNC`),
`GPU_DISPLAY_EVENTS`, `GPU_DISPLAY_REFRESH` and the six `GPU_FENCE_*` requests
check the capability first, so a node without it answers `EOPNOTSUPP` even to
an open of the wrong mode. The other requests check the open mode first.

| Request | Nr | Direction | Argument | Bytes | Open mode | Capability | Described in |
| --- | ---: | --- | --- | ---: | --- | --- | --- |
| `GPU_GET_INFO` | 0 | `_IOWR` | `struct gpu_info` | 56 | none | none | [gpu-capabilities](gpu-capabilities.md#gpu_get_info) |
| `GPU_RESOURCE_CREATE` | 1 | `_IOWR` | `struct gpu_resource_create` | 32 | W | `GPU_CAP_RESOURCE` | [gpu-memory](gpu-memory.md#gpu_resource_create) |
| `GPU_RESOURCE_DESTROY` | 2 | `_IOW` | `struct gpu_resource_destroy` | 16 | W | none | [gpu-memory](gpu-memory.md#gpu_resource_destroy) |
| `GPU_GET_CAPSET` | 3 | `_IOWR` | `struct gpu_capset` | 280 | none | `GPU_CAP_CAPSET` | [gpu-capabilities](gpu-capabilities.md#gpu_get_capset) |
| `GPU_BLOB_CREATE` | 4 | `_IOWR` | `struct gpu_blob_create` | 40 | W | `GPU_CAP_BLOB` | [gpu-memory](gpu-memory.md#gpu_blob_create) |
| `GPU_RESOURCE_READ` | 5 | `_IOW` | `struct gpu_transfer` | 40 | R | `GPU_CAP_TRANSFER` | [gpu-memory](gpu-memory.md#gpu_resource_read) |
| `GPU_RESOURCE_WRITE` | 6 | `_IOW` | `struct gpu_transfer` | 40 | W | `GPU_CAP_TRANSFER` | [gpu-memory](gpu-memory.md#gpu_resource_write) |
| `GPU_COMMAND` | 7 | `_IOW` | `struct gpu_command` | 24 | W | `GPU_CAP_COMMAND` | [gpu-submission](gpu-submission.md#gpu_command) |
| `GPU_PRESENT` | 8 | `_IOW` | `struct gpu_present` | 48 | W | `GPU_CAP_PRESENT` | [gpu-submission](gpu-submission.md#gpu_present) |
| `GPU_RESOURCE_MAP` | 9 | `_IOWR` | `struct gpu_resource_map` | 32 | R | `GPU_CAP_MAPPING` | [gpu-memory](gpu-memory.md#gpu_resource_map) |
| `GPU_RESOURCE_EXPORT` | 10 | `_IOWR` | `struct gpu_resource_export` | 88 | W | `GPU_CAP_SHARE` | [gpu-sharing](gpu-sharing.md#gpu_resource_export) |
| `GPU_RESOURCE_IMPORT` | 11 | `_IOWR` | `struct gpu_resource_import` | 96 | W | none | [gpu-sharing](gpu-sharing.md#gpu_resource_import) |
| `GPU_COMMAND_SUBMIT` | 12 | `_IOWR` | `struct gpu_command_submit` | 40 | W | `GPU_CAP_NOTIFICATION` | [gpu-submission](gpu-submission.md#gpu_command_submit) |
| `GPU_COMMAND_WAIT` | 13 | `_IOWR` | `struct gpu_command_wait` | 32 | R | `GPU_CAP_NOTIFICATION` | [gpu-submission](gpu-submission.md#gpu_command_wait) |
| `GPU_ALLOCATION_EXPORT` | 14 | `_IOWR` | `struct gpu_allocation_export` | 184 | W | `GPU_CAP_ALLOCATION_SHARE` | [gpu-sharing](gpu-sharing.md#gpu_allocation_export) |
| `GPU_ALLOCATION_IMPORT` | 15 | `_IOWR` | `struct gpu_allocation_import` | 192 | W | `GPU_CAP_ALLOCATION_SHARE` | [gpu-sharing](gpu-sharing.md#gpu_allocation_import) |
| `GPU_FENCE_CREATE` | 16 | `_IOWR` | `struct gpu_fence_create` | 32 | W | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_create) |
| `GPU_FENCE_QUERY` | 17 | `_IOWR` | `struct gpu_fence_state` | 40 | R | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_query) |
| `GPU_FENCE_WAIT` | 18 | `_IOWR` | `struct gpu_fence_state` | 40 | R | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_wait) |
| `GPU_FENCE_RESET` | 19 | `_IOWR` | `struct gpu_fence_state` | 40 | W | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_reset) |
| `GPU_FENCE_SIGNAL` | 20 | `_IOWR` | `struct gpu_fence_state` | 40 | W | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_signal) |
| `GPU_FENCE_BIND` | 21 | `_IOW` | `struct gpu_fence_bind` | 32 | W | `GPU_CAP_FENCE` | [gpu-fences](gpu-fences.md#gpu_fence_bind) |
| `GPU_COMMAND_SUBMIT_SYNC` | 22 | `_IOWR` | `struct gpu_command_submit_sync` | 64 | W | `GPU_CAP_FENCE` and `GPU_CAP_NOTIFICATION` | [gpu-fences](gpu-fences.md#gpu_command_submit_sync) |
| `GPU_DISPLAY_PRESENT_SYNC` | 23 | `_IOWR` | `struct gpu_display_present_sync` | 104 | W | `GPU_CAP_FENCE` and `GPU_CAP_DISPLAY` | [gpu-fences](gpu-fences.md#gpu_display_present_sync) |
| `GPU_DISPLAY_QUERY` | 24 | `_IOWR` | `struct gpu_display_info` | 152 | R | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_query) |
| `GPU_DISPLAY_MODE` | 25 | `_IOWR` | `struct gpu_display_mode` | 48 | R | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_mode) |
| `GPU_DISPLAY_CLAIM` | 26 | `_IOWR` | `struct gpu_display_claim` | 32 | W | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_claim) |
| `GPU_DISPLAY_RELEASE` | 27 | `_IOW` | `struct gpu_display_release` | 16 | W | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_release) |
| `GPU_DISPLAY_PRESENT` | 28 | `_IOWR` | `struct gpu_display_present` | 80 | W | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_present) |
| `GPU_DISPLAY_WAIT` | 29 | `_IOWR` | `struct gpu_display_wait` | 56 | R | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_wait) |
| `GPU_DEVICE_QUERY` | 30 | `_IOWR` | `struct gpu_device_info` | 40 | none | none | [gpu-memory](gpu-memory.md#gpu_device_query) |
| `GPU_DISPLAY_CONSTRAINTS` | 31 | `_IOWR` | `struct gpu_scanout_constraints` | 56 | none | `GPU_CAP_DISPLAY` | [gpu-display](gpu-display.md#gpu_display_constraints) |
| `GPU_DISPLAY_EVENTS` | 32 | `_IOWR` | `struct gpu_display_events` | 40 | R | `GPU_CAP_DISPLAY_EVENTS` | [gpu-display](gpu-display.md#gpu_display_events) |
| `GPU_BLOB_CREATE_PLACED` | 33 | `_IOWR` | `struct gpu_blob_create_placed` | 64 | W | `GPU_CAP_BLOB` | [gpu-memory](gpu-memory.md#gpu_blob_create_placed) |
| `GPU_JOB_RESERVE` | 34 | `_IOWR` | `struct gpu_job_reserve` | 40 | W | `GPU_CAP_JOB` | [gpu-jobs](gpu-jobs.md#gpu_job_reserve) |
| `GPU_JOB_COMMIT` | 35 | `_IOW` | `struct gpu_job_action` | 24 | W | `GPU_CAP_JOB` | [gpu-jobs](gpu-jobs.md#gpu_job_commit) |
| `GPU_JOB_CANCEL` | 36 | `_IOW` | `struct gpu_job_action` | 24 | W | `GPU_CAP_JOB` | [gpu-jobs](gpu-jobs.md#gpu_job_cancel) |
| `GPU_JOB_CAPACITY` | 37 | `_IOWR` | `struct gpu_job_capacity` | 48 | none | `GPU_CAP_JOB_CAPACITY` | [gpu-jobs](gpu-jobs.md#gpu_job_capacity) |
| `GPU_JOB_POLICY` | 38 | `_IOWR` | `struct gpu_job_policy` | 40 | none | `GPU_CAP_JOB` | [gpu-jobs](gpu-jobs.md#gpu_job_policy) |
| `GPU_DISPLAY_POWER` | 39 | `_IOW` | `struct gpu_display_power` | 32 | W | `GPU_CAP_DISPLAY` and `GPU_CAP_DISPLAY_CONTROL` | [gpu-display-control](gpu-display-control.md#gpu_display_power) |
| `GPU_DISPLAY_REFRESH` | 40 | `_IOWR` | `struct gpu_display_refresh` | 56 | R | `GPU_CAP_DISPLAY_CONTROL` | [gpu-display-control](gpu-display-control.md#gpu_display_refresh) |
| `GPU_DISPLAY_CURSOR_SET` | 41 | `_IOW` | `struct gpu_display_cursor_image` | 64 | W | `GPU_CAP_DISPLAY` | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_set) |
| `GPU_DISPLAY_CURSOR_MOVE` | 42 | `_IOW` | `struct gpu_display_cursor_move` | 40 | W | `GPU_CAP_DISPLAY` | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_move) |
| `GPU_DISPLAY_CURSOR_HIDE` | 43 | `_IOW` | `struct gpu_display_cursor_hide` | 32 | W | `GPU_CAP_DISPLAY` | [gpu-display-control](gpu-display-control.md#gpu_display_cursor_hide) |
| `GPU_DISPLAY_COMMIT` | 44 | `_IOWR` | `struct gpu_display_commit` | 552 | W | `GPU_CAP_DISPLAY` | [gpu-display-control](gpu-display-control.md#gpu_display_commit) |
| `GPU_DISPLAY_OVERLAY_INFO` | 45 | `_IOWR` | `struct gpu_display_overlay_info` | 56 | R | `GPU_CAP_DISPLAY` | [gpu-display-control](gpu-display-control.md#gpu_display_overlay_info) |

## Errors

Each request section in the `gpu-*.md` documents lists, in its `Errors:` list,
the values that the kernel's own checks return for that request: the checks in
the shared ioctl path and in the request's handler, before and after the
driver is called. Those lists are authoritative for a request. The table below
gives the meaning of each value that is used in the same way by many requests.
A driver may return other values from its own operations, and the kernel
passes them through unchanged; the last item of every `Errors:` list, `Other
values`, says which ones are known.

| Error | Meaning across the interface |
| --- | --- |
| `EINVAL` | The request is malformed. The `version` is not `GPU_ABI_VERSION`, or the `size` is not the size of the argument structure; a reserved field, a field marked `zero` or an output field is not zero on input; a flag bit is unknown or a combination of flags is not allowed; a value is outside its range (a zero size, an offset past the end, a count above its limit). A resource handle that is zero, unknown, destroyed or created by another open also fails with `EINVAL` (not `ENOENT`). The kernel returns it before any driver sees the request. |
| `EOPNOTSUPP` | The kernel or the node does not provide the request. The request number is not one the kernel knows (the answer is `EOPNOTSUPP`, not `ENOTTY`); the node lacks a required capability bit; the driver lacks the operation the request calls; or a flag or format asks for something the node cannot do. `ENOTSUP` has the same value as `EOPNOTSUPP` in `include/uapi/errno.h`; the sections use the name that the source uses. |
| `EACCES` | The open does not have the access mode the request needs: a request that needs a readable open on an `O_WRONLY` open, or one that needs a writable open on an `O_RDONLY` open. See the Open mode column. |
| `EPERM` | The open is not the owner of the object it acts on: it holds no binding for the fence generation, or the binding belongs to a job (`GPU_FENCE_SIGNAL`, the release form of `GPU_FENCE_BIND`, `GPU_JOB_COMMIT`). |
| `EBADF` | A file descriptor in the argument is not an open descriptor of the calling process, or is not a descriptor of the kind that the request takes (a fence descriptor). |
| `ENOENT` | A completion sequence names no record of this open: it was never issued, belongs to another open, was consumed or discarded (`GPU_COMMAND_WAIT`, `GPU_JOB_COMMIT`, `GPU_JOB_CANCEL`, `GPU_FENCE_BIND`). Resource handles are not reported with `ENOENT`. Some drivers return it for a display they do not have. |
| `ESTALE` | A generation is not the current one: the fence was reset, or the object was replaced since the caller read the generation. The caller reads the current state and tries again. Drivers use it for an old display `generation` as well. |
| `EXDEV` | The object belongs to another device: a fence made by another node, or a descriptor exported by another node, where the request does not accept one. |
| `EBUSY` | The object is in use and the request cannot proceed: the resource is mapped into a process (`GPU_RESOURCE_DESTROY`); the fence generation already has a producer or owner, or is no longer pending; another job request of the open works on the same record; or the request was made from inside a driver callback of the same open (a program does not see this one). Drivers return it for a display lease that is not the open's. |
| `EALREADY` | The state change was already made: the job is already committed or finished with status zero (`GPU_JOB_COMMIT`, `GPU_JOB_CANCEL`). |
| `EAGAIN` | The request would have to wait and was told not to, or no place is free. A wait with `timeout_ns` zero finds the fence, record or capacity not ready; all `GPU_SUBMIT_MAX` completion records or fence bindings of the open are in use; a fence that a request waits for is pending and no job owns it; or another consuming wait holds the same record. The state is unchanged and the request can be repeated. |
| `ETIMEDOUT` | A finite `timeout_ns` expired before the condition came. The state is unchanged and the wait can be repeated. The kernel also records `ETIMEDOUT` as the error of an open whose supervised job missed its deadline; later requests on that open return it ([failure and recovery](gpu-submission.md#session-failure-and-recovery)). |
| `EINTR` | A signal ended an interruptible wait: the wait for the open's admission (the open's one request at a time), a wait for a fence, a completion record or a capacity change, or the wait for a device reset in `open`. The request did nothing. If the handler was installed with `SA_RESTART`, the kernel runs the call again and the program does not see `EINTR` ([concurrency](gpu.md#concurrency-blocking-and-cancellation)). |
| `ENODEV` | The device has been withdrawn (removed) and the node cannot serve the request any more, or the open is being closed; also the recorded error of a device or open that failed with `ENODEV`. A fence that an open still had bound when it closed is signaled with it. A request that finds the device withdrawn returns it from then on. |
| `EIO` | The device or the open has failed and the request returns the recorded error: `EIO` is what the kernel records when it can no longer prove what the context's native work did. `EIO` is also returned when the driver's reply breaks the contract of the request (a success without an object, a zero sequence, a reply outside its range). It differs from `ENODEV` in that the device is still registered; the open must be closed and made again ([failure and recovery](gpu-submission.md#session-failure-and-recovery)). When the recorded error is another errno, that errno is returned, not `EIO`. |
| `ENOMEM` | The kernel could not allocate the record, the copy or the descriptor that the request needs. |
| `ENOSPC` | The open already holds `max_resources` resources (`GPU_RESOURCE_CREATE`, `GPU_BLOB_CREATE`, `GPU_BLOB_CREATE_PLACED`, `GPU_RESOURCE_IMPORT`, `GPU_ALLOCATION_IMPORT`). |
| `EMFILE` | The calling process has no free descriptor for the fence or exported capability that the request returns. |
| `ESRCH` | The calling thread has no process descriptor table, so no descriptor can be installed (`GPU_FENCE_CREATE`, `GPU_RESOURCE_EXPORT`, `GPU_ALLOCATION_EXPORT`). |
| `E2BIG` | A command stream is larger than `GPU_COMMAND_MAX` (`GPU_COMMAND_SUBMIT`, `GPU_COMMAND_SUBMIT_SYNC`). |
| `EOVERFLOW` | A value cannot be represented or a limit of the kernel is reached: the counter that issues handles or sequences is exhausted, a `timeout_ns` is too large for a deadline, the record has the largest possible number of observers, the device has the largest possible number of live exports, or a view cannot be addressed by this system. |
| `EFAULT` | The kernel cannot read the argument from, or write the reply to, the address that the caller gave, or an address field of the argument does not name memory the caller may use. |

Two kinds of value are not in the table because the kernel does not return
them on its own checks. `ENXIO` (for example an output that is not connected)
and `ECANCELED` (for example from a driver that is closing the namespace of an
open) come from drivers and from recorded completion errors; where a request can show them, the
`Other values` item of its section names them.
