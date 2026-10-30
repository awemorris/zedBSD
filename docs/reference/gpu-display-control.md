# GPU display control: power, refresh, cursor and planes

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: the requests that need GPU_CAP_DISPLAY_CONTROL
(confirm per request in the Requires: lines) and the per-output flags
that say which of them an output supports (POWER_CONTROL,
REFRESH_COUNTER, HAS_CURSOR, HAS_OVERLAYS; gpu-display.md). Link back to
gpu.md and gpu-display.md (leases, generations, sequences). -->

PENDING

## Refresh boundaries

<!-- spec: What a refresh boundary is (a scanout's or a virtual clock's,
GPU_DISPLAY_VIRTUAL_CLOCK), the never-decreasing count, the 1 ms
monotonic scheduler clock of time_ns, that an output that does not scan
out has no boundary (ETIMEDOUT), and that cursor and plane changes take
effect from a boundary. -->

PENDING

## Planes and commits

<!-- spec: From gpu_commit_ioctl, gpu_commit_check, gpu_commit_plane_check
and gpu_commit_record: (1) Plane 0 is the primary (the whole output,
unscaled); planes 1 to plane_count - 1 are overlays (at most
GPU_DISPLAY_OVERLAY_MAX). (2) A commit names some planes; planes not named
keep their state; a plane named twice is EINVAL. (3) SHOWN and IMAGE
flags; premultiplied alpha for overlays; src and dst rectangles and
scaling limits (GPU_DISPLAY_OVERLAY_INFO). (4) serial per plane and the
DROPPED result, the only exception to all or nothing. (5) wait_fd per
plane, signal_fd with COMMIT_WAIT. (6) Borrowing: an image is borrowed
until a list without it is adopted; cached storage is cleaned when named
as a new image, so the caller does not write it while borrowed.
(7) sequence and adopted outputs, the count shared with
GPU_DISPLAY_PRESENT. (8) ENOSPC for bandwidth or overlay limits
(nothing changed). (9) ETIMEDOUT: the output keeps every borrowed image,
later commits and presents of the lease fail with EBUSY, and release or
close returns the output to the console. -->

PENDING

## Requests

### `GPU_DISPLAY_POWER`

<!-- spec: Argument struct gpu_display_power (Structures). state GPU_DISPLAY_POWER_ON, OFF, SUSPEND (SUSPEND may equal OFF);
writable open and this open's lease; the lease's end powers the output
on; a power change is not a topology change. Errors from the header
comment, confirmed in the code. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_REFRESH`

<!-- spec: Argument struct gpu_display_refresh (Structures). No lease; inputs display_id, generation, cursor (zero reports the current
count at once), timeout_ns (at most one second per call); outputs
sequence, time_ns, flags (GPU_DISPLAY_VIRTUAL_CLOCK). ENODEV when the GPU
is offline. Whether it runs outside the session admission. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_CURSOR_SET`

<!-- spec: Argument struct gpu_display_cursor_image (Structures). Image copied at once from session storage: width and height 1 to
GPU_DISPLAY_CURSOR_MAX, stride rules, GPU_PIXEL_* format with
premultiplied alpha; storage not borrowed after the call; the transient
EBUSY when every cursor image is still named by a display list. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_CURSOR_MOVE`

<!-- spec: Argument struct gpu_display_cursor_move (Structures). x, y of the image's top-left in output pixels (the caller subtracts the
hotspot), clipping, takes effect at the next boundary, a later move
replaces it; EINVAL without an image. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_CURSOR_HIDE`

<!-- spec: Argument struct gpu_display_cursor_hide (Structures). Hides from the next boundary, keeps the image; lease release and a
device fault hide it too. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_COMMIT`

<!-- spec: Argument struct gpu_display_commit (Structures). Every field of struct gpu_display_commit and struct
gpu_display_plane_state; flags GPU_DISPLAY_COMMIT_WAIT (needs the primary
named); count; the result field of each plane; see Planes and commits. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DISPLAY_OVERLAY_INFO`

<!-- spec: Argument struct gpu_display_overlay_info (Structures). Inputs display_id, plane (an overlay index), generation; outputs the
source and destination size limits, scaling flags
(GPU_DISPLAY_OVERLAY_SCALE_*), formats; on the output's current mode. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_POWER_ON` | 0 | PENDING |
| `GPU_DISPLAY_POWER_OFF` | 1 | PENDING |
| `GPU_DISPLAY_POWER_SUSPEND` | 2 | PENDING |
| `GPU_DISPLAY_CURSOR_MAX` | 64 | PENDING |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_PLANE_PRIMARY` | 0 | PENDING |
| `GPU_DISPLAY_OVERLAY_MAX` | 4 | PENDING |
| `GPU_DISPLAY_PLANE_SHOWN` | 1 | PENDING |
| `GPU_DISPLAY_PLANE_IMAGE` | 2 | PENDING |
| `GPU_DISPLAY_PLANE_APPLIED` | 1 | PENDING |
| `GPU_DISPLAY_PLANE_DROPPED` | 2 | PENDING |
| `GPU_DISPLAY_COMMIT_WAIT` | 1 | PENDING |
| `GPU_DISPLAY_OVERLAY_SCALE_UP` | 1 | PENDING |
| `GPU_DISPLAY_OVERLAY_SCALE_DOWN_X` | 2 | PENDING |
| `GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y` | 4 | PENDING |

## Structures

#### `struct gpu_display_power`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `state` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_display_refresh`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `cursor` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `timeout_ns` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `time_ns` | PENDING | PENDING |

#### `struct gpu_display_cursor_image`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `stride` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `format` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 56 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_display_cursor_move`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 4 | `int32_t` | `x` | PENDING | PENDING |
| 20 | 4 | `int32_t` | `y` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_display_cursor_hide`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_display_plane_state`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `plane` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `serial` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `stride` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `format` | PENDING | PENDING |
| 48 | 4 | `uint32_t` | `src_x` | PENDING | PENDING |
| 52 | 4 | `uint32_t` | `src_y` | PENDING | PENDING |
| 56 | 4 | `uint32_t` | `src_width` | PENDING | PENDING |
| 60 | 4 | `uint32_t` | `src_height` | PENDING | PENDING |
| 64 | 4 | `uint32_t` | `dst_x` | PENDING | PENDING |
| 68 | 4 | `uint32_t` | `dst_y` | PENDING | PENDING |
| 72 | 4 | `uint32_t` | `dst_width` | PENDING | PENDING |
| 76 | 4 | `uint32_t` | `dst_height` | PENDING | PENDING |
| 80 | 4 | `int32_t` | `wait_fd` | PENDING | PENDING |
| 84 | 4 | `uint32_t` | `result` | PENDING | PENDING |
| 88 | 8 | `uint64_t` | `wait_generation` | PENDING | PENDING |

#### `struct gpu_display_commit`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `lease` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `count` | PENDING | PENDING |
| 32 | 480 | `struct gpu_display_plane_state[5]` | `planes` | PENDING | PENDING |
| 512 | 4 | `int32_t` | `signal_fd` | PENDING | PENDING |
| 516 | 4 | `uint32_t` | `reserved0` | PENDING | PENDING |
| 520 | 8 | `uint64_t` | `signal_generation` | PENDING | PENDING |
| 528 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 536 | 8 | `uint64_t` | `adopted` | PENDING | PENDING |
| 544 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_display_overlay_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `display_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `plane` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `max_src_width` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `max_src_height` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `max_dst_width` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `max_dst_height` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `formats` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |
