# GPU fences

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: a fence is a reference-bearing file descriptor
with a generation and a state (PENDING, SIGNALED, ERROR) that orders
work between submissions, displays and processes. Link back to gpu.md;
GPU_CAP_FENCE in gpu-capabilities.md. -->

PENDING

## Fence model

<!-- spec: From src/drivers/gpu/gpu-fence.c and gpu.c (gpu_fence_ioctl,
gpu_fence_create_ioctl, gpu_fence_get, gpu_fence_state_ioctl,
gpu_fence_bind_ioctl, gpu_fence_binding_add, gpu_fence_binding_end,
gpu_fence_retire, gpu_dependency_wait, gpu_dependency_ioctl): (1) The
payload: generation (starts at one), state, error. Which operations need
an exact generation and what a stale generation returns. (2) Who signals
a fence: SIGNAL with error zero only after an authoritative observation
of success; a bound open's failure turns it to ERROR; what close of the
binding open does. (3) Binding (GPU_FENCE_BIND): sequence zero reserves
ownership before native submission, a nonzero sequence associates a
transport failure with the payload, GPU_FENCE_BIND_RELEASE undoes a
reservation. (4) The fence descriptor itself: it is a file descriptor
independent of the GPU open; it can pass between processes; poll on it
reports POLLIN when not PENDING and POLLERR on ERROR (read gpu-fence.c's
poll); what read, write and close do. (5) Whether the fence requests need
GPU_CAP_FENCE (read gpu_fence_ioctl; state the result per request in the
Requires: lines). (6) Dependencies in the *_SYNC requests: wait before
queue publication, signal after selection; an absent dependency uses fd
-1 and generation zero; a queue notification never substitutes for a
verified GPU_FENCE_SIGNAL. -->

PENDING

## Requests

### `GPU_FENCE_CREATE`

<!-- spec: Argument struct gpu_fence_create (Structures). Inputs flags (GPU_HANDLE_CLOEXEC, GPU_HANDLE_CLOFORK), fd = -1,
signaled, generation zero; outputs fd and generation. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_FENCE_QUERY`

<!-- spec: Argument struct gpu_fence_state (Structures). Generation zero accepted; returns the current generation and state. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_FENCE_WAIT`

<!-- spec: Argument struct gpu_fence_state (Structures). Exact generation; timeout_ns semantics (zero, finite, UINT64_MAX), EINTR,
the result when the fence ends in ERROR (state and error outputs). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_FENCE_RESET`

<!-- spec: Argument struct gpu_fence_state (Structures). Exact generation; returns its successor; when a reset is refused (still
bound, still pending?). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_FENCE_SIGNAL`

<!-- spec: Argument struct gpu_fence_state (Structures). Exact generation; error field (zero for success, else a negative or
positive errno? read the code) and who may signal. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_FENCE_BIND`

<!-- spec: Argument struct gpu_fence_bind (Structures). See Fence model (3). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_COMMAND_SUBMIT_SYNC`

<!-- spec: Argument struct gpu_command_submit_sync (Structures). Nested struct gpu_command_submit keeps version GPU_ABI_VERSION and its
own size (sizeof(struct gpu_command_submit)), not the outer size; the
outer structure has no header. wait_fd/wait_generation and
signal_fd/signal_generation. The nested sequence reports accepted work.
Read the handler that serves it (gpu_dependency_ioctl). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_PRESENT_SYNC`

<!-- spec: Argument struct gpu_display_present_sync (Structures). Nested struct gpu_display_present keeps its own ABI and completed
sequence semantics (gpu-display.md GPU_DISPLAY_PRESENT); wait before
hardware access, signal after selection. Requires a display lease like
GPU_DISPLAY_PRESENT. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_FENCE_PENDING` | 0 | PENDING |
| `GPU_FENCE_SIGNALED` | 1 | PENDING |
| `GPU_FENCE_ERROR` | 2 | PENDING |
| `GPU_FENCE_BIND_RELEASE` | 1 | PENDING |

## Structures

#### `struct gpu_fence_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 12 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `signaled` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `generation` | PENDING | PENDING |

#### `struct gpu_fence_state`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `timeout_ns` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `state` | PENDING | PENDING |
| 36 | 4 | `int32_t` | `error` | PENDING | PENDING |

#### `struct gpu_fence_bind`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |

#### `struct gpu_command_submit_sync`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 40 | `struct gpu_command_submit` | `command` | PENDING | PENDING |
| 40 | 4 | `int32_t` | `wait_fd` | PENDING | PENDING |
| 44 | 4 | `int32_t` | `signal_fd` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `wait_generation` | PENDING | PENDING |
| 56 | 8 | `uint64_t` | `signal_generation` | PENDING | PENDING |

#### `struct gpu_display_present_sync`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 80 | `struct gpu_display_present` | `present` | PENDING | PENDING |
| 80 | 4 | `int32_t` | `wait_fd` | PENDING | PENDING |
| 84 | 4 | `int32_t` | `signal_fd` | PENDING | PENDING |
| 88 | 8 | `uint64_t` | `wait_generation` | PENDING | PENDING |
| 96 | 8 | `uint64_t` | `signal_generation` | PENDING | PENDING |
