# GPU request index and errors

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: every request of the /dev/gpuN interface in one
table, with the open mode and capability it needs and the document that
describes it, followed by the errno values with a meaning shared across
the interface. Link back to gpu.md for the calling conventions. -->

PENDING

## Request index

<!-- spec: Fill the Open mode and Capability columns of every row. Take
them from the Requires: line of each request's section (the documents in
the Described in column), then confirm each against the handler in
src/drivers/gpu/gpu.c (gpu_ioctl_body and the handler): Open mode is R
when the request needs a readable open, W a writable open, RW both, none
neither. Capability follows the rule of the Requires: line (the bits the
kernel always checks before the driver, nested handlers included; bits
that only some flags need and conditions that are not bits are in the
section's text, not here). Where a section and the code disagree, the
code wins: correct the section too. Do not change the other columns. -->

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

## Errors

<!-- spec: A table | Error | Meaning across the interface | of the errno
values the kernel's own checks return with a common meaning. Build it
from the Errors: lists of every request section in the gpu-*.md files
and confirm each meaning in src/drivers/gpu/gpu.c. Candidates: EINVAL
(malformed request, wrong version or size, nonzero reserved field),
EOPNOTSUPP/ENOTSUP (the node lacks the capability or the driver lacks the
operation; ENOTSUP equals EOPNOTSUPP in include/uapi/errno.h; an unknown
request number), EBADF/EPERM/EACCES (open mode or ownership; read which
one gpu.c returns for a missing readable or writable permission), ENOENT
(unknown handle), EBUSY, EAGAIN, ETIMEDOUT, EINTR, ENODEV and EIO (device
or session failure; how they differ), ENOMEM, ENOSPC, ESTALE, ENXIO,
EFAULT, EXDEV, ECANCELED, EALREADY, EOVERFLOW, ESRCH. Only list values
the source returns. Say that each request section lists the kernel's own
checks and that a driver may return other values (the "Other values"
item). -->

PENDING
