# GPU display: outputs, leases and presentation

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: a node with GPU_CAP_DISPLAY exposes outputs
(connectors with a monitor or a virtual output). A program enumerates
them, takes an exclusive lease on a plane of an output, and presents
frames to it. Power, refresh counters, the cursor and multi-plane commits
are in gpu-display-control.md. Link back to gpu.md. -->

PENDING

## Outputs, identities and generations

<!-- spec: From gpu_display_ioctl and include/uapi/gpu-display.h: (1) An
output is selected by ordinal index in GPU_DISPLAY_QUERY and then named
by its stable nonzero display_id. (2) generation: what changes it
(connection, mode, topology), and that every request naming a display
with an old generation fails with ESTALE; a program re-queries.
(3) Output flags: fill the Meaning of every GPU_DISPLAY_* flag of
gpu_display_info.flags in the constants table, including which flags tell
when a present returns (GPU_DISPLAY_FIFO, GPU_DISPLAY_VIRTUAL_CLOCK,
GPU_DISPLAY_ATOMIC_MODE_PRESENT) and LIMITED (claim answers ENOSPC).
(4) Formats (GPU_DISPLAY_FORMAT_* bit mask) versus GPU_PIXEL_* values.
(5) Disconnection: ENXIO. -->

PENDING

## Leases

<!-- spec: From the claim and release paths of gpu_display_ioctl and
gpu_close: a lease is an exclusive reservation of one plane of one output
by one open file description (shared by dup and SCM_RIGHTS, never
transferred to another open); lease values are nonzero. What another
open's claim returns while the plane is leased (EBUSY), what release does
(retires scanout and private buffers first), what close of the last
reference does, and that ending the lease returns the output to the text
console (see User-visible side effects). Which requests need the lease
and which do not (GPU_DISPLAY_REFRESH needs none). -->

PENDING

## Presentation and completion

<!-- spec: From gpu_display_present_resource, gpu_display_admit,
gpu_commit_record and the wait path: (1) FIFO presentation of a whole
frame from session storage (GPU_DISPLAY_PRESENT_FIFO) and BLOB mode
(GPU_DISPLAY_PRESENT_BLOB): which storage each accepts, when the storage
is no longer borrowed (ordinary storage: at return; BLOB: until a later
frame replaces it or the lease ends), and that the caller finishes
rendering before presenting (or uses GPU_DISPLAY_PRESENT_SYNC,
gpu-fences.md). (2) The per-output sequence count shared with
GPU_DISPLAY_COMMIT (gpu-display-control.md); adopted versus completed;
GPU_DISPLAY_WAIT. (3) When GPU_DISPLAY_PRESENT returns: differs by output
(an output whose presents return only once adopted answers
GPU_DISPLAY_WAIT at once); say which output flag tells a program which
behaviour it gets and read the drivers only to confirm, naming none here
(gpu-drivers.md has the per-driver table). (4) Scanout routes from
GPU_DISPLAY_CONSTRAINTS: SHARED (import trial), FOREIGN (backing from
another device), COPY (ordinary storage without BLOB); the route is
chosen once per swapchain, never per presentation. -->

PENDING

## Display events

<!-- spec: From gpu_events_snapshot and gpu_events_ioctl and gpu_poll:
POLLPRI readiness, the sequence that starts at one so that a new open
first does a complete query, the ACK protocol (query, re-enumerate,
acknowledge that snapshot), coalescing and repetition, that the
acknowledgement belongs to the open file description, that a failed
copyout acknowledges nothing, and that sequence is not an output
generation. -->

PENDING

## User-visible side effects

<!-- spec: From gpu_console_end and the claim/release paths (grep
kern_text_end, tty_console_input_hold, tty_console_input_unhold in
src/drivers/gpu/gpu.c): (1) The first successful flip
(GPU_PRESENT, GPU_DISPLAY_PRESENT, GPU_DISPLAY_PRESENT_SYNC or
GPU_DISPLAY_COMMIT) ends the text console's drawing into the framebuffer
(once per boot? read the code). (2) A display claim holds the console
keyboard input; release and close give it back. (3) What a program sees
on the screen when its lease ends. Keep to what the code does now. -->

PENDING

## Requests

### `GPU_DISPLAY_QUERY`

<!-- spec: Argument struct gpu_display_info (Structures). Inputs version, size, index (or GPU_DISPLAY_COUNT_ONLY for a count-only
query); every output field; plane_count and overlays; max_frame_bytes;
refresh_millihz; name (NUL termination). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_MODE`

<!-- spec: Argument struct gpu_display_mode (Structures). operation GPU_DISPLAY_MODE_ENUMERATE (index, count) or
GPU_DISPLAY_MODE_VALIDATE (width, height, refresh_millihz; refresh zero
asks for the preferred frequency); side-effect free: it never selects the
active mode. How a mode is then selected (by the present's
width/height/refresh_millihz? read the code). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_CLAIM`

<!-- spec: Argument struct gpu_display_claim (Structures). Inputs display_id, plane_index, generation; output lease. Errors:
EBUSY (leased), ENOSPC (LIMITED), ESTALE, ENXIO. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_RELEASE`

<!-- spec: Argument struct gpu_display_release (Structures). Consumes the lease after scanout and private buffers are retired; what
the output shows afterwards. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_PRESENT`

<!-- spec: Argument struct gpu_display_present (Structures). Every field: lease, handle, offset, frame, sequence (input zero, output
the completed virtual presentation), width, height, stride, format,
refresh_millihz (mode selection?), flags (FIFO, BLOB), generation. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_WAIT`

<!-- spec: Argument struct gpu_display_wait (Structures). Input lease, sequence, timeout_ns (at most one second per call; zero
answers at once); outputs completed_sequence, present_time_ns (clock),
generation. Errors EAGAIN, ETIMEDOUT, EINTR, EBUSY, ENXIO as the header
comment lists; confirm in the code. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_EVENTS`

<!-- spec: Argument struct gpu_display_events (Structures). Query (version and size) and ACK (flags GPU_DISPLAY_EVENT_ACK and
ack_sequence); events output (GPU_DISPLAY_EVENT_CHANGE); see Display
events. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_CONSTRAINTS`

<!-- spec: Argument struct gpu_scanout_constraints (Structures). Inputs version, size, display_id, generation, the rest zero; outputs
flags (GPU_SCANOUT_*), formats, stride_alignment, offset_alignment,
placement (GPU_PLACEMENT_* requirement mask, gpu-memory.md),
max_dma_address. Read gpu_constraints_ioctl. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_CONNECTED` | 1 | PENDING |
| `GPU_DISPLAY_VIRTUAL_CLOCK` | 2 | PENDING |
| `GPU_DISPLAY_FIFO` | 4 | PENDING |
| `GPU_DISPLAY_ATOMIC_MODE_PRESENT` | 8 | PENDING |
| `GPU_DISPLAY_ACTIVE` | 16 | PENDING |
| `GPU_DISPLAY_BLOB` | 32 | PENDING |
| `GPU_DISPLAY_POWER_CONTROL` | 64 | PENDING |
| `GPU_DISPLAY_POWERED_OFF` | 128 | PENDING |
| `GPU_DISPLAY_REFRESH_COUNTER` | 256 | PENDING |
| `GPU_DISPLAY_LIMITED` | 512 | PENDING |
| `GPU_DISPLAY_HAS_CURSOR` | 1024 | PENDING |
| `GPU_DISPLAY_HAS_OVERLAYS` | 2048 | PENDING |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_FORMAT_BGRA8888` | 1 | PENDING |
| `GPU_DISPLAY_FORMAT_RGBA8888` | 2 | PENDING |
| `GPU_DISPLAY_MODE_ENUMERATE` | 0 | PENDING |
| `GPU_DISPLAY_MODE_VALIDATE` | 1 | PENDING |
| `GPU_DISPLAY_PRESENT_FIFO` | 1 | PENDING |
| `GPU_DISPLAY_PRESENT_BLOB` | 2 | PENDING |
| `GPU_DISPLAY_COUNT_ONLY` | UINT32_MAX | PENDING |
| `GPU_DISPLAY_EVENT_CHANGE` | 1 | PENDING |
| `GPU_DISPLAY_EVENT_ACK` | 1 | PENDING |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_SCANOUT_SHARED` | 1 | PENDING |
| `GPU_SCANOUT_COPY` | 2 | PENDING |
| `GPU_SCANOUT_FOREIGN` | 4 | PENDING |

## Structures

#### `struct gpu_display_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `index` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `count` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `plane_count` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `formats` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `max_frame_bytes` | PENDING | PENDING |
| 48 | 4 | `uint32_t` | `current_width` | PENDING | PENDING |
| 52 | 4 | `uint32_t` | `current_height` | PENDING | PENDING |
| 56 | 4 | `uint32_t` | `preferred_width` | PENDING | PENDING |
| 60 | 4 | `uint32_t` | `preferred_height` | PENDING | PENDING |
| 64 | 4 | `uint32_t` | `physical_width_mm` | PENDING | PENDING |
| 68 | 4 | `uint32_t` | `physical_height_mm` | PENDING | PENDING |
| 72 | 4 | `uint32_t` | `max_width` | PENDING | PENDING |
| 76 | 4 | `uint32_t` | `max_height` | PENDING | PENDING |
| 80 | 4 | `uint32_t` | `refresh_millihz` | PENDING | PENDING |
| 84 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 88 | 64 | `char[64]` | `name` | PENDING | PENDING |

#### `struct gpu_display_mode`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `operation` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `index` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `count` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `refresh_millihz` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `flags` | PENDING | PENDING |

#### `struct gpu_display_claim`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `plane_index` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `lease` | PENDING | PENDING |

#### `struct gpu_display_release`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |

#### `struct gpu_display_present`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `frame` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 48 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 52 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 56 | 4 | `uint32_t` | `stride` | PENDING | PENDING |
| 60 | 4 | `uint32_t` | `format` | PENDING | PENDING |
| 64 | 4 | `uint32_t` | `refresh_millihz` | PENDING | PENDING |
| 68 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 72 | 8 | `uint64_t` | `generation` | PENDING | PENDING |

#### `struct gpu_display_wait`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `timeout_ns` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `completed_sequence` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `present_time_ns` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `generation` | PENDING | PENDING |

#### `struct gpu_display_events`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `events` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `ack_sequence` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_scanout_constraints`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `formats` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `stride_alignment` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `offset_alignment` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `placement` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `max_dma_address` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |
