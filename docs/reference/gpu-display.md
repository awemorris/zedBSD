# GPU display: outputs, leases and presentation

Status: experimental reference for GPU ABI version 1.

A node whose capabilities include `GPU_CAP_DISPLAY` exposes outputs: connectors with a monitor, and virtual outputs that have none. A program enumerates the outputs with `GPU_DISPLAY_QUERY`, takes an exclusive lease on a plane of one output with `GPU_DISPLAY_CLAIM`, and presents whole frames to it with `GPU_DISPLAY_PRESENT`. `GPU_DISPLAY_WAIT` tells when a presented frame has been adopted by the display, and `GPU_DISPLAY_EVENTS` with `poll()` tells when the set of outputs has changed.

This document describes the requests that every node with `GPU_CAP_DISPLAY` answers. Power, refresh counters, the cursor and multi-plane commits are in [gpu-display-control.md](gpu-display-control.md). The rules shared by all requests (structure framing, open modes, concurrency) are in [gpu.md](gpu.md), and the request index is [gpu-requests.md](gpu-requests.md).

## Outputs, identities and generations

An output is first selected by its ordinal. `GPU_DISPLAY_QUERY` with `index` from 0 to `count` - 1 returns a snapshot of one output, and `index` set to `GPU_DISPLAY_COUNT_ONLY` returns only `count`. The ordinal exists for enumeration only. The snapshot carries the output's `display_id`, a nonzero identifier that every other request uses to name the output; no request takes an ordinal except `GPU_DISPLAY_QUERY`.

An output also has a `generation`, a nonzero number that identifies one state of its topology. The driver advances it when the output's connection or topology changes (for example a monitor is connected, disconnected or replaced, so that the set of modes is not the one that was queried). An output whose topology never changes keeps one value. A request that names a `display_id` together with a `generation` that is no longer current fails with `ESTALE`; the kernel core passes the generation to the driver, and the driver answers. A program that gets `ESTALE` queries the outputs again, takes the new `generation` and tries again. `GPU_DISPLAY_EVENTS` tells when such a change has happened (see Display events).

The flags of `struct gpu_display_info` describe what the output offers; each flag is described in the constants table. A program decides from them before it claims: `GPU_DISPLAY_CONNECTED` for a monitor, `GPU_DISPLAY_FIFO` and `GPU_DISPLAY_BLOB` for the kinds of presentation, `GPU_DISPLAY_LIMITED` for an output that cannot be lit now (its claim fails with `ENOSPC`), and `GPU_DISPLAY_VIRTUAL_CLOCK` and `GPU_DISPLAY_ATOMIC_MODE_PRESENT` for timing and mode behaviour (see Presentation and completion).

Two name spaces describe pixel formats and must not be mixed. The `formats` field of `struct gpu_display_info` and `struct gpu_scanout_constraints` is a bit mask of `GPU_DISPLAY_FORMAT_BGRA8888` and `GPU_DISPLAY_FORMAT_RGBA8888`. The `format` field of `struct gpu_display_present` is a single value, `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888` (defined in `include/uapi/gpu.h`). The numbers are the same, but the first is a set of bits and the second is one value; the core accepts only these two values in a present, and the driver refuses a format that the output's `formats` mask does not contain.

A request that names an output that is not connected fails with `ENXIO` in the requests that depend on the connection (claim, present and wait). The driver decides; an unconnected output normally remains in the inventory with `GPU_DISPLAY_CONNECTED` clear, and its `generation` has changed.

## Leases

A lease is an exclusive reservation of one plane of one output. `GPU_DISPLAY_CLAIM` creates it and returns a `lease` value; the value is nonzero. The lease belongs to the open file description that claimed it. The description can be shared by `dup()` and by passing the descriptor over a socket, and every holder of the description then uses the same lease. A lease is never transferred to another open of the node, and another open cannot use the value: a request on another open that names it fails in the driver. The kernel core counts the claims of an open, so one open may hold more than one lease where the driver allows it.

Plane 0 is the output's primary plane and is the only plane a claim names in this ABI; overlay planes and the cursor plane of an output are used through the primary lease (see [gpu-display-control.md](gpu-display-control.md)). While a plane is leased, a claim of the same plane by any open, including the open that holds it, fails with `EBUSY`. A claim on an output that is `GPU_DISPLAY_LIMITED` fails with `ENOSPC`.

The requests that need the lease are the ones that carry a `lease` field: `GPU_DISPLAY_RELEASE`, `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_WAIT`, and the requests of [gpu-display-control.md](gpu-display-control.md) that act on the leased output. `GPU_DISPLAY_QUERY`, `GPU_DISPLAY_MODE`, `GPU_DISPLAY_CONSTRAINTS`, `GPU_DISPLAY_EVENTS` and `GPU_DISPLAY_REFRESH` need none.

`GPU_DISPLAY_RELEASE` ends a lease explicitly. On success the driver has retired the scanout and the private buffers the lease used, and the lease value is no longer valid. Closing the last reference to the open file description ends every lease of the open in the same way. The kernel core first gives back the keyboard holds of the open's claims (see User-visible side effects) and then calls the driver's close operation, which ends the leases. After a lease ends the output belongs to nobody; what it shows is decided by the driver and is not specified here (see User-visible side effects). The next claim of the output succeeds.

## Presentation and completion

##### Whole frames, FIFO and BLOB

`GPU_DISPLAY_PRESENT` presents one whole frame. Its `flags` field is exactly `GPU_DISPLAY_PRESENT_FIFO` or `GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB`; every other value fails with `EINVAL`. The frame is `height` rows of `stride` bytes starting at `offset` in the resource named by `handle`; each row begins with `width` packed pixels of 4 bytes. The whole extent (`stride` times `height`, from `offset`) must lie inside the resource. A partial update or damage rectangle does not exist.

Without `GPU_DISPLAY_PRESENT_BLOB`, `handle` names ordinary storage created by `GPU_RESOURCE_CREATE` (see [gpu-memory.md](gpu-memory.md)). The driver copies the rows before the call returns, so the storage is no longer borrowed when `GPU_DISPLAY_PRESENT` returns and the program may write it again at once. With `GPU_DISPLAY_PRESENT_BLOB`, `handle` names a blob or a scanout allocation imported from another device (see [gpu-memory.md](gpu-memory.md) and [gpu-sharing.md](gpu-sharing.md)); any other kind of resource fails with `EINVAL`. The display then reads the allocation itself, without a copy, and the allocation stays borrowed until a later frame replaces it or the lease ends; the program must not destroy or overwrite it in the meantime.

The kernel does not wait for rendering that writes the frame. The program finishes the rendering before it presents, or it presents with `GPU_DISPLAY_PRESENT_SYNC`, which waits for a fence first (see [gpu-fences.md](gpu-fences.md)).

The legacy `GPU_PRESENT` request (see [gpu-requests.md](gpu-requests.md)) also flips an image to a display but does not use a lease.

##### Sequence numbers, adoption and waiting

Each present on a lease, and each `GPU_DISPLAY_COMMIT` on it (see [gpu-display-control.md](gpu-display-control.md)), is given the next number of a count that increases and is nonzero. `GPU_DISPLAY_PRESENT` returns the number of its frame in `sequence`, which must be zero on input. A number has meaning only with the lease that returned it. Most drivers start the count again with a new lease; the bcm2711 display keeps counting across leases, so a program does not compare numbers of different leases.

A frame is adopted when the display has taken it to be shown, or when a later frame that replaces it has been adopted. `completed_sequence` of `GPU_DISPLAY_WAIT` is the highest number that is adopted together with every lower number. `GPU_DISPLAY_WAIT` with a `sequence` waits until that number is adopted, for at most `timeout_ns` on a driver that waits: one call waits at most one second, and zero answers at once. A driver whose presents return only once the frame is adopted answers at once. On success it reports `completed_sequence`, the time of that adoption in `present_time_ns` and the output's `generation`. With `GPU_DISPLAY_VIRTUAL_CLOCK` the time is the output's virtual clock, not a monitor's refresh.

##### When GPU_DISPLAY_PRESENT returns

The call does not always return at the same point, and the output decides. On an output whose presents return only once the frame is adopted, `GPU_DISPLAY_WAIT` for that number answers at once, whatever `timeout_ns` is. On an output that returns once the frame is queued for the next refresh boundary, `GPU_DISPLAY_WAIT` waits for the adoption. A request on an output with a separate admission for presentation (see [gpu.md](gpu.md)) lets the open's other requests run while it waits for the adoption.

No output flag tells these two behaviours apart. `GPU_DISPLAY_FIFO` says that the output accepts whole-frame presents that are shown in the order presented, one number each; a program that needs presentation to follow the refresh (for example the Vulkan FIFO present mode) requires this flag. `GPU_DISPLAY_VIRTUAL_CLOCK` says that the output has no scanout of its own and counts refresh boundaries with a virtual clock. `GPU_DISPLAY_ATOMIC_MODE_PRESENT` says that the `width`, `height` and `refresh_millihz` of a present select the mode together with the frame in one operation. A program that must know that its frame is on the display calls `GPU_DISPLAY_WAIT` with the returned number and a nonzero `timeout_ns`, and takes success as adoption. The per-driver behaviour is in [gpu-drivers.md](gpu-drivers.md).

`GPU_DISPLAY_MODE` never selects a mode. The `width`, `height` and `refresh_millihz` of a present describe the frame and the mode it is shown in; `refresh_millihz` must be nonzero. `GPU_DISPLAY_MODE` with `GPU_DISPLAY_MODE_VALIDATE` and the same values tells beforehand whether the output accepts them. An output without `GPU_DISPLAY_ATOMIC_MODE_PRESENT` does not promise that a frame of another size changes the mode: the driver may show it at its fixed mode, or refuse it.

##### Scanout routes

`GPU_DISPLAY_CONSTRAINTS` tells which route a program may use to get a frame to the output, as a mask of `GPU_SCANOUT_SHARED`, `GPU_SCANOUT_COPY` and `GPU_SCANOUT_FOREIGN`. `GPU_SCANOUT_COPY` says that ordinary storage and `GPU_DISPLAY_PRESENT` without `GPU_DISPLAY_PRESENT_BLOB` work. `GPU_SCANOUT_SHARED` says that an allocation may be tried for import and presented with `GPU_DISPLAY_PRESENT_BLOB`; `GPU_SCANOUT_FOREIGN` says in addition that the backing may come from another device. Neither bit promises that an arbitrary allocation imports. The constraints also give the alignments and placement a shared allocation must meet. A program chooses the route once for a swapchain, from the constraints of the generation it queried, and does not change it per presentation.

## Display events

A node with `GPU_CAP_DISPLAY_EVENTS` reports changes of its set of outputs through `GPU_DISPLAY_EVENTS` and through `poll()`. A change is a connection, disconnection or other topology change, the kind that advances an output's `generation`.

The driver keeps a sequence number for the node. It starts at one and increases for every change, so a new open has not acknowledged it and must first do a complete query of the outputs. `GPU_DISPLAY_EVENTS` without `GPU_DISPLAY_EVENT_ACK` is a query: it returns the current `sequence`, and `events` is `GPU_DISPLAY_EVENT_CHANGE` when that sequence is newer than the one this open has acknowledged. A query changes nothing about readiness.

The protocol is: query the events and keep the `sequence`; enumerate the outputs again with `GPU_DISPLAY_QUERY` (new `display_id` and `generation` values); then acknowledge that sequence with `GPU_DISPLAY_EVENTS` with `GPU_DISPLAY_EVENT_ACK` in `flags` and the kept value in `ack_sequence`. The acknowledgement covers only that sequence. Changes after it leave the open ready for `POLLPRI` and `events` set, so a change that happens during the re-enumeration is not lost. `ack_sequence` must be a sequence that this open has already received successfully from this request, and not newer than the current one; any other value fails with `EINVAL`, and nothing is changed. Acknowledging a sequence older than one already acknowledged is accepted and does not move the acknowledgement back.

`poll()` with `POLLPRI` reports that the open has an unacknowledged change. It does so only for an open that can read and only on a node with `GPU_CAP_DISPLAY_EVENTS`; if the driver's snapshot fails, `poll()` reports `POLLERR` instead.

The acknowledgement belongs to the open file description, so descriptors that share it (by `dup()` or by passing a descriptor over a socket) share it too; another open of the same node has its own. The kernel commits an observation or an acknowledgement only after it has copied the reply to the program: a failed copy acknowledges nothing and records nothing. Notifications may coalesce several changes into one or repeat; a program does not count them. The sequence is not an output's generation: it says that something changed, and the generations of the re-queried outputs say what.

## User-visible side effects

These are effects on the console and the keyboard that a program does not ask for by name.

The first flip ends the text console on the display. After the first request that flips an image to a display and succeeds (`GPU_PRESENT`, `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_PRESENT_SYNC` or `GPU_DISPLAY_COMMIT`), the kernel asks the text console to end its use of the display. This happens once for the whole system, on the first successful flip of any GPU node, and it is not undone: the console no longer draws on the display for the rest of the boot, whether or not the lease ends, and a later lease on any output does not bring it back. The console's characters still reach its serial line and the log. A failed flip does not end the console. A board that has no display console, or whose console cannot stop, has nothing to stop; the request is not affected. Any other failure is logged, and the request still succeeds.

A claim stops the boot splash and holds the keyboard. The first claim also ends the boot's progress display for good, if the board shows one. Each successful claim holds the console's keyboard input: while any claim exists, anywhere in the system, the keyboard events do not reach the text console. `GPU_DISPLAY_RELEASE` gives back the hold of that claim, and closing the open gives back the holds of all its claims that were not released. The keyboard reaches the console again when the last hold ends.

What the output shows when a lease ends depends on the driver: some leave the last picture, some blank the output, and some ask the console to draw the screen again. The last has an effect only if no flip has ended the console; after the first flip the text console draws nothing, and the output shows what the driver leaves. Do not rely on the text console coming back after a lease. The per-driver behaviour is in [gpu-drivers.md](gpu-drivers.md).

## Requests

### `GPU_DISPLAY_QUERY`

Requires: open mode R; capability `GPU_CAP_DISPLAY`.

Returns a snapshot of one output. The only inputs are `version`, `size` and `index`; the kernel clears every other field before it asks the driver, so a value left in an output field is not echoed back, and `reserved` must be zero. `index` is an ordinal from 0 to `count` - 1, or `GPU_DISPLAY_COUNT_ONLY`, which returns `count` and leaves every other output field zero. `count` is the number of outputs of the node in both forms.

For an ordinal the reply names the output (`display_id`, a nonzero identifier that other requests use, and `generation`, see Outputs, identities and generations), its `flags`, the number of planes, the pixel formats, the largest frame the output accepts in one present (`max_frame_bytes`), the current, preferred and largest sizes, the physical size and the refresh rate of the current mode in millihertz (60000 is 60 Hz). `plane_count` counts the primary plane and the overlay planes; a cursor plane (`GPU_DISPLAY_HAS_CURSOR`) is not counted. `name` is a text for people; the kernel makes its last byte a NUL, so it is always terminated.

If the driver returns an ordinal reply with a zero `display_id` or `generation`, or an `index` that is not below `count`, the kernel fails the request with `EIO`. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; or `reserved` is not zero.
- `EIO`: The driver returned a reply without a nonzero `display_id` and `generation` and an `index` below `count`.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: A driver may refuse an ordinal at or above `count` with `EINVAL`. Other values are returned as they are from the driver's query operation, or the device's or the open's recorded error. A request on a failed device returns the device's recorded error.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`), [display-inventory](../../userland/tests/display-inventory/main.c).

### `GPU_DISPLAY_MODE`

Requires: open mode R; capability `GPU_CAP_DISPLAY`.

Enumerates the modes of an output or validates one. It has no side effect and never selects the active mode: a mode is used by presenting a frame of that size (see Presentation and completion). `display_id` and `generation` must be nonzero and must be those of a query.

With `GPU_DISPLAY_MODE_ENUMERATE`, `index` selects a mode from 0 to `count` - 1 (the order is the driver's), and the reply gives `width`, `height` and `refresh_millihz` of that mode. `index` set to `GPU_DISPLAY_COUNT_ONLY` returns only `count`. The `width`, `height` and `refresh_millihz` inputs are ignored; the kernel clears them.

With `GPU_DISPLAY_MODE_VALIDATE`, `index` must be zero and `width` and `height` must be nonzero. `refresh_millihz` set to zero asks the driver to choose its preferred supported frequency for those dimensions; the reply gives the frequency it chose. A mode that the output cannot show fails with the driver's error. If the driver reports success but leaves a zero `width`, `height` or `refresh_millihz`, the kernel fails the request with `EIO`.

`count` and `flags` must be zero on input. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; `count` or `flags` is not zero; `operation` is neither value; with validation, `index` is not zero or `width` or `height` is zero.
- `EIO`: A validation succeeded in the driver but left `width`, `height` or `refresh_millihz` zero.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through: `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EINVAL` for a mode the output cannot show or an `index` that is out of range, and `ENOENT` where an output does not exist.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`), [ghci-display](../../userland/tests/ghci-display/main.c).

### `GPU_DISPLAY_CLAIM`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Takes an exclusive lease on one plane of one output. `display_id` and `generation` must be nonzero and come from a query, `plane_index` is 0 (the primary plane), and `lease` must be zero on input. On success `lease` is a nonzero value that the later requests use. The lease belongs to this open file description (see Leases).

A plane that is leased already, by any open, cannot be claimed: the driver answers `EBUSY`. An output that is `GPU_DISPLAY_LIMITED` answers `ENOSPC`, an old `generation` `ESTALE`, and an output that is not connected `ENXIO`.

A successful claim stops the boot splash and holds the console's keyboard input until the lease is released or the open closes (see User-visible side effects). If the kernel cannot copy the reply to the program, it releases the lease again and returns the copy error, so no lease exists that the program cannot name. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read or written (the claim is undone if the reply cannot be written).
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; `lease` is not zero.
- `EIO`: The driver reported success with a zero `lease`.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through: `EBUSY` for a plane that is leased, `ENOSPC` for an output with `GPU_DISPLAY_LIMITED`, `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EINVAL` for a `plane_index` other than 0 (one driver answers `EBUSY`), and `EOVERFLOW` when lease numbers are exhausted.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_close`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_RELEASE`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Ends a lease of this open. `lease` must be nonzero and must be a value that this open's claim returned. On success the driver has retired the scanout and the private buffers of the lease before the call returns, the lease value is invalid, and the open's keyboard hold for that claim is given back. A failure leaves the lease with the open. The argument is not written back.

What the output shows afterwards is decided by the driver; see User-visible side effects. Closing the open ends the leases in the same way.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `lease` is zero.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through: a lease that this open does not hold is refused (`EBUSY`, `EPERM` or `EINVAL`, depending on the driver), and a failure to retire the scanout is returned as the driver reports it.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_close`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_PRESENT`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Presents one whole frame to a leased plane. The frame is described by `handle`, `offset`, `stride`, `height`, `width` and `format`, and the kind of presentation by `flags` (see Presentation and completion). `lease` and `generation` must be nonzero, `lease` must be this open's, `sequence` must be zero on input, `width`, `height` and `refresh_millihz` must be nonzero, `width` times 4 must fit in 32 bits, `format` must be `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, and `stride` must be a multiple of 4 and at least `width` times 4. `handle` must name a resource of this open of the right kind: ordinary storage without `GPU_DISPLAY_PRESENT_BLOB`, a blob or a scanout allocation with it. `offset` and `stride` times `height` must lie inside the resource. A zero or unknown `handle` fails with `EINVAL`.

On success `sequence` is the number given to the frame (see Presentation and completion). Ordinary storage is no longer borrowed when the call returns; a blob stays borrowed until a later frame replaces it or the lease ends. `frame` is the program's own frame number; the kernel does not check it and no driver reads it, and it is returned unchanged. The first successful flip of the system ends the text console (see User-visible side effects).

Where the node lets presentation run outside the open's admission, the request records the frame, then waits for its adoption; otherwise it runs under the admission. If the kernel cannot copy the reply, the frame has already been presented. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `lease`, `generation`, `width`, `height` or `refresh_millihz` is zero; `sequence` is not zero; `flags` is neither `GPU_DISPLAY_PRESENT_FIFO` nor `GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB`; `width` times 4 does not fit in 32 bits; `format` is not a defined value; `stride` is smaller than `width` times 4 or not a multiple of 4; `handle` is zero, unknown, or of the wrong kind for `flags`; `offset` or the extent of the frame lies outside the resource.
- `EIO`: The driver reported success with a zero `sequence`.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through: `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EBUSY` for a lease that is not this open's or that has ended (some drivers answer `EPERM` or `EINVAL` for a foreign lease), `EINVAL` for a frame size, format or stride the output does not accept, `ETIMEDOUT` for an adoption that did not come, and `EIO` for a lost GPU. After `ETIMEDOUT` the output may keep the frame's buffers and refuse the lease's later presents with `EBUSY`.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_present_resource`, `gpu_display_ungated`, `gpu_display_admit`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_WAIT`

Requires: open mode R; capability `GPU_CAP_DISPLAY`.

Waits until a presented frame has been adopted. `lease` must be this open's and `sequence` the number that `GPU_DISPLAY_PRESENT` (or `GPU_DISPLAY_COMMIT`) returned. `timeout_ns` bounds the wait on the drivers that wait (ghci and bcm2711): zero answers at once, and one call waits at most one second whatever the value. The other drivers do not use `timeout_ns` (see below). On success `completed_sequence` is the highest number adopted together with every lower number, and is at least `sequence`; `present_time_ns` is the time of that adoption in nanoseconds and `generation` is the output's generation. The three output fields must be zero on input.

On an output whose presents return only once adopted, the request answers at once and ignores `timeout_ns`. A request that is not ready answers `EAGAIN` when `timeout_ns` is zero, and `ETIMEDOUT` when the time ran out. A signal ends the wait with `EINTR` on the bcm2711 display only; the wait of the ghci display does not end on a signal. There is no cancellation request, and a new call waits again. The lease ending during the wait is an error (`EBUSY`).

Whether other requests of the open run during the wait depends on the node (see [gpu.md](gpu.md)). On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `lease` is zero; `completed_sequence`, `present_time_ns` or `generation` is not zero.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through, and the header lists them: `EAGAIN` when `timeout_ns` is zero and the sequence is not adopted (also when nothing has been presented yet), `ETIMEDOUT` when the time ran out, `EINTR` on a signal (bcm2711 only), `EBUSY` for a lease that is not this open's or that ended during the wait (some drivers answer `EPERM` or `EINVAL` for a foreign lease), `ENXIO` for an output that is not connected, and `EINVAL` for a `sequence` the lease has not been given.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_ungated`), [hvs-load](../../userland/tests/hvs-load/main.c).

### `GPU_DISPLAY_EVENTS`

Requires: open mode R; capability `GPU_CAP_DISPLAY_EVENTS`.

Queries the display-change sequence of the node, or acknowledges one, for this open (see Display events). The request does not change the display and does not wait: it runs outside the open's admission, so it works while another thread of the same open presents or waits.

A query has `flags` zero and `ack_sequence` zero. An acknowledgement has `flags` set to `GPU_DISPLAY_EVENT_ACK` and `ack_sequence` set to a nonzero sequence this open has already received. `events`, `sequence` and `reserved` must be zero on input, and `flags` has no other bit. The reply gives the node's current `sequence` and, in `events`, `GPU_DISPLAY_EVENT_CHANGE` if a change newer than the acknowledged one is pending after this request. The reply describes the state after the acknowledgement of this request and not later events. The kernel records the observation and the acknowledgement only after the reply has been copied out.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY_EVENTS`.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written (nothing is recorded or acknowledged).
- `EINVAL`: `version` or `size` is not the structure's; `flags` has a bit other than `GPU_DISPLAY_EVENT_ACK`; `events`, `sequence` or `reserved` is not zero; `flags` is zero and `ack_sequence` is not, or `flags` is `GPU_DISPLAY_EVENT_ACK` and `ack_sequence` is zero; `ack_sequence` is newer than any sequence this open has received or newer than the current one.
- `EIO`: The driver reported a zero sequence.
- `ENODEV`: The device is offline.
- Other values: The device's or the open's recorded error is returned if the device or the open has failed, and a driver error from its events operation is returned as it is.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_events_ioctl`, `gpu_events_snapshot`, `gpu_poll`), [gpu-admission](../../userland/tests/gpu-admission/main.c).

### `GPU_DISPLAY_CONSTRAINTS`

Requires: open mode none; capability `GPU_CAP_DISPLAY`.

Returns the requirements for getting frames to one generation of an output, before any allocation or import. `display_id` and `generation` must be nonzero and must be those of a query; every other input field must be zero. The reply gives `flags` (the routes `GPU_SCANOUT_SHARED`, `GPU_SCANOUT_COPY` and `GPU_SCANOUT_FOREIGN`), `formats` (a mask of `GPU_DISPLAY_FORMAT_BGRA8888` and `GPU_DISPLAY_FORMAT_RGBA8888`), the stride and offset alignments in bytes (zero means byte alignment, otherwise a power of two), `placement` (a mask of `GPU_PLACEMENT_DMA32`, `GPU_PLACEMENT_CONTIGUOUS` and `GPU_PLACEMENT_COHERENT` that the allocation must meet, see [gpu-memory.md](gpu-memory.md)) and `max_dma_address` (the highest address the display can read; zero means the backend exposes no limit). The requirements describe the output; they do not promise that a particular allocation can be shown.

The request needs neither a read nor a write open. The kernel checks the reply: unknown route, placement or format bits, a route `GPU_SCANOUT_COPY` on a node without `GPU_CAP_RESOURCE` and `GPU_CAP_TRANSFER`, alignments that are not powers of two, a route `GPU_SCANOUT_FOREIGN` without `GPU_SCANOUT_SHARED` or without an import operation, or a changed `display_id` or `generation` all fail with `EIO`. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`, or its driver gives no constraints.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; any other input field is not zero.
- `EIO`: The driver's reply breaks the rules above.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission (requests that run under it).
- Other values: Driver answers pass through: `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, and `ENOENT` or `EINVAL` for an output that does not exist.

Sources: [gpu-scanout.h](../../include/uapi/gpu-scanout.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_constraints_ioctl`), [display-scanout](../../userland/tests/display-scanout/main.c).

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_CONNECTED` | 1 | A monitor is connected to the output, or the output is virtual and present. Clear when the monitor is absent; requests that depend on the connection then fail with `ENXIO`. |
| `GPU_DISPLAY_VIRTUAL_CLOCK` | 2 | The output has no scanout of its own: its refresh boundaries and presentation times come from a virtual clock. Used with `GPU_DISPLAY_REFRESH` (see gpu-display-control.md). |
| `GPU_DISPLAY_FIFO` | 4 | The output takes whole-frame presents shown in the order presented, one number each, so a program can offer a FIFO present mode. Set on every output of the drivers in the tree. |
| `GPU_DISPLAY_ATOMIC_MODE_PRESENT` | 8 | The `width`, `height` and `refresh_millihz` of a present select the output's mode together with the frame in one operation. Without it a different frame size may be refused or shown at the output's fixed mode. |
| `GPU_DISPLAY_ACTIVE` | 16 | The plane is showing an image now; clear for an output that is dark or that no lease has presented to. |
| `GPU_DISPLAY_BLOB` | 32 | The output accepts `GPU_DISPLAY_PRESENT_BLOB`: a shared allocation is shown without a copy. |
| `GPU_DISPLAY_POWER_CONTROL` | 64 | `GPU_DISPLAY_POWER` works on the output; set only on a node with `GPU_CAP_DISPLAY_CONTROL`. |
| `GPU_DISPLAY_POWERED_OFF` | 128 | The lease holder has powered the output off with `GPU_DISPLAY_POWER`. |
| `GPU_DISPLAY_REFRESH_COUNTER` | 256 | `GPU_DISPLAY_REFRESH` reports the output's refresh boundaries; set only on a node with `GPU_CAP_DISPLAY_CONTROL`. |
| `GPU_DISPLAY_LIMITED` | 512 | The output is connected but cannot be lit now, because the number of outputs shown at once has reached its limit; its claim fails with `ENOSPC`. |
| `GPU_DISPLAY_HAS_CURSOR` | 1024 | The output has a cursor plane: `GPU_DISPLAY_CURSOR_SET`, `GPU_DISPLAY_CURSOR_MOVE` and `GPU_DISPLAY_CURSOR_HIDE` work on its primary lease. The plane is not counted in `plane_count`. |
| `GPU_DISPLAY_HAS_OVERLAYS` | 2048 | Planes 1 to `plane_count` - 1 of the output are overlay planes, used with `GPU_DISPLAY_COMMIT` and `GPU_DISPLAY_OVERLAY_INFO`. |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_FORMAT_BGRA8888` | 1 | Bit in a `formats` mask: the output accepts packed 32-bit pixels with blue first (the value `GPU_PIXEL_BGRA8888` in a present). |
| `GPU_DISPLAY_FORMAT_RGBA8888` | 2 | Bit in a `formats` mask: the output accepts packed 32-bit pixels with red first (the value `GPU_PIXEL_RGBA8888` in a present). |
| `GPU_DISPLAY_MODE_ENUMERATE` | 0 | `operation` of `GPU_DISPLAY_MODE`: return the mode at `index`, or `count` for `GPU_DISPLAY_COUNT_ONLY`. |
| `GPU_DISPLAY_MODE_VALIDATE` | 1 | `operation` of `GPU_DISPLAY_MODE`: check `width`, `height` and `refresh_millihz` without selecting a mode; a zero refresh asks for the driver's preferred frequency. |
| `GPU_DISPLAY_PRESENT_FIFO` | 1 | Bit of the `flags` of a present: a whole-frame FIFO presentation. Every present sets it. |
| `GPU_DISPLAY_PRESENT_BLOB` | 2 | Bit of the `flags` of a present, together with `GPU_DISPLAY_PRESENT_FIFO`: `handle` names a shared allocation that the display reads without a copy and that stays borrowed until a later frame replaces it or the lease ends. |
| `GPU_DISPLAY_COUNT_ONLY` | UINT32_MAX | Value of `index` in `GPU_DISPLAY_QUERY` and in a mode enumeration that returns only `count`. |
| `GPU_DISPLAY_EVENT_CHANGE` | 1 | Bit of `events` in a reply: a display change newer than the acknowledged sequence is pending. |
| `GPU_DISPLAY_EVENT_ACK` | 1 | Bit of `flags` in a request: acknowledge `ack_sequence`. |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_SCANOUT_SHARED` | 1 | Route: an allocation may be tried for import and presented with `GPU_DISPLAY_PRESENT_BLOB`; this does not promise that an arbitrary allocation imports. |
| `GPU_SCANOUT_COPY` | 2 | Route: ordinary storage and `GPU_DISPLAY_PRESENT` without `GPU_DISPLAY_PRESENT_BLOB` work. |
| `GPU_SCANOUT_FOREIGN` | 4 | Route: in addition to `GPU_SCANOUT_SHARED`, backing from another device may be imported; this does not promise that an arbitrary allocation imports. |

## Structures

#### `struct gpu_display_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_info)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `index` | in | Ordinal of the output, from 0 to `count` - 1, or `GPU_DISPLAY_COUNT_ONLY` for a query that returns only `count`. |
| 12 | 4 | `uint32_t` | `count` | out | Number of outputs of the node. |
| 16 | 4 | `uint32_t` | `display_id` | out | Nonzero identifier of the output; the name every other request uses. Zero in a count-only reply. |
| 20 | 4 | `uint32_t` | `flags` | out | Mask of the `GPU_DISPLAY_*` output flags in the constants table. |
| 24 | 4 | `uint32_t` | `plane_count` | out | Number of planes the output has, counting the primary plane (0) and the overlay planes; a cursor plane is not counted. |
| 28 | 4 | `uint32_t` | `formats` | out | Mask of `GPU_DISPLAY_FORMAT_*` bits: the pixel formats a present to the output accepts. |
| 32 | 8 | `uint64_t` | `generation` | out | Nonzero generation of the output's topology; other requests that name the output carry it. |
| 40 | 8 | `uint64_t` | `max_frame_bytes` | out | Largest frame, in bytes, that the output accepts in one present. |
| 48 | 4 | `uint32_t` | `current_width` | out | Width in pixels of the mode in use now. |
| 52 | 4 | `uint32_t` | `current_height` | out | Height in pixels of the mode in use now. |
| 56 | 4 | `uint32_t` | `preferred_width` | out | Width in pixels of the output's preferred mode. |
| 60 | 4 | `uint32_t` | `preferred_height` | out | Height in pixels of the output's preferred mode. |
| 64 | 4 | `uint32_t` | `physical_width_mm` | out | Physical width of the display in millimetres; zero when the driver does not know it. |
| 68 | 4 | `uint32_t` | `physical_height_mm` | out | Physical height of the display in millimetres; zero when the driver does not know it. |
| 72 | 4 | `uint32_t` | `max_width` | out | Largest frame width in pixels the output accepts. |
| 76 | 4 | `uint32_t` | `max_height` | out | Largest frame height in pixels the output accepts. |
| 80 | 4 | `uint32_t` | `refresh_millihz` | out | Refresh rate of the current mode in millihertz (60000 is 60 Hz). |
| 84 | 4 | `uint32_t` | `reserved` | zero | Reserved; must be zero. |
| 88 | 64 | `char[64]` | `name` | out | Name of the output for people, as text; the kernel makes the last byte a NUL. |

#### `struct gpu_display_mode`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_mode)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `operation` | in | `GPU_DISPLAY_MODE_ENUMERATE` or `GPU_DISPLAY_MODE_VALIDATE`. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. |
| 24 | 4 | `uint32_t` | `index` | in | Enumeration: ordinal of the mode, or `GPU_DISPLAY_COUNT_ONLY` for a count only. Validation: must be zero. |
| 28 | 4 | `uint32_t` | `count` | out | Number of modes of the output. Must be zero on input. |
| 32 | 4 | `uint32_t` | `width` | in/out | Validation: width in pixels to check, nonzero. Enumeration: output, the mode's width; the input is ignored. |
| 36 | 4 | `uint32_t` | `height` | in/out | Validation: height in pixels to check, nonzero. Enumeration: output, the mode's height; the input is ignored. |
| 40 | 4 | `uint32_t` | `refresh_millihz` | in/out | Validation: refresh in millihertz to check; zero asks for the driver's preferred frequency, and the reply gives the frequency chosen. Enumeration: output, the mode's refresh; the input is ignored. |
| 44 | 4 | `uint32_t` | `flags` | zero | Reserved; must be zero. |

#### `struct gpu_display_claim`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_claim)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output to lease, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `plane_index` | in | Plane to lease; 0, the primary plane. A plane other than 0 is refused by the driver. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 8 | `uint64_t` | `lease` | out | The new lease; nonzero. Must be zero on input. |

#### `struct gpu_display_release`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_release)`). The kernel also writes it in the reply. |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open to end; nonzero. |

#### `struct gpu_display_present`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_present)`). The kernel also writes it in the reply. |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open that the frame is presented to; nonzero. |
| 16 | 8 | `uint64_t` | `handle` | in | Handle of the resource that holds the frame: ordinary storage, or with `GPU_DISPLAY_PRESENT_BLOB` a blob or scanout allocation. Nonzero. |
| 24 | 8 | `uint64_t` | `offset` | in | Byte offset of the first row in the resource. |
| 32 | 8 | `uint64_t` | `frame` | in | The program's own frame number; not checked or used by the kernel, and not changed. |
| 40 | 8 | `uint64_t` | `sequence` | out | Number given to the frame (see Presentation and completion). Must be zero on input. |
| 48 | 4 | `uint32_t` | `width` | in | Width of the frame in pixels; nonzero. |
| 52 | 4 | `uint32_t` | `height` | in | Height of the frame in rows; nonzero. |
| 56 | 4 | `uint32_t` | `stride` | in | Bytes from the start of one row to the next; a multiple of 4 and at least `width` times 4. |
| 60 | 4 | `uint32_t` | `format` | in | `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, a value and not a `GPU_DISPLAY_FORMAT_*` bit. |
| 64 | 4 | `uint32_t` | `refresh_millihz` | in | Refresh rate of the presentation's mode in millihertz; nonzero. |
| 68 | 4 | `uint32_t` | `flags` | in | `GPU_DISPLAY_PRESENT_FIFO` or `GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB`. |
| 72 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |

#### `struct gpu_display_wait`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_wait)`). The kernel also writes it in the reply. |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open; nonzero. |
| 16 | 8 | `uint64_t` | `sequence` | in | Number of the frame or commit to wait for, as returned by the request that made it. |
| 24 | 8 | `uint64_t` | `timeout_ns` | in | Longest wait in nanoseconds; zero answers at once, and one call waits at most one second. |
| 32 | 8 | `uint64_t` | `completed_sequence` | out | Highest number adopted together with every lower one. Must be zero on input. |
| 40 | 8 | `uint64_t` | `present_time_ns` | out | Time of that adoption in nanoseconds; with `GPU_DISPLAY_VIRTUAL_CLOCK` the output's virtual clock. Must be zero on input. |
| 48 | 8 | `uint64_t` | `generation` | out | Generation of the output at the time of the reply. Must be zero on input. |

#### `struct gpu_display_events`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_events)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `flags` | in | Zero for a query, or `GPU_DISPLAY_EVENT_ACK` to acknowledge `ack_sequence`. |
| 12 | 4 | `uint32_t` | `events` | out | `GPU_DISPLAY_EVENT_CHANGE` if a change newer than the acknowledged one is pending after this request, otherwise zero. Must be zero on input. |
| 16 | 8 | `uint64_t` | `sequence` | out | The node's current display-change sequence; starts at one. Must be zero on input. |
| 24 | 8 | `uint64_t` | `ack_sequence` | in | Sequence to acknowledge: nonzero with `GPU_DISPLAY_EVENT_ACK`, and a value this open has received; zero for a query. |
| 32 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_scanout_constraints`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_scanout_constraints)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `flags` | out | Mask of `GPU_SCANOUT_*` routes. Must be zero on input. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. |
| 24 | 4 | `uint32_t` | `formats` | out | Mask of `GPU_DISPLAY_FORMAT_*` bits that the output accepts. Must be zero on input. |
| 28 | 4 | `uint32_t` | `stride_alignment` | out | Required alignment of a frame's `stride` in bytes; zero means byte alignment, otherwise a power of two. Must be zero on input. |
| 32 | 4 | `uint32_t` | `offset_alignment` | out | Required alignment of a frame's `offset` in bytes; zero means byte alignment, otherwise a power of two. Must be zero on input. |
| 36 | 4 | `uint32_t` | `placement` | out | Mask of `GPU_PLACEMENT_*` requirements that an allocation must meet (see gpu-memory.md); a requirement, not a guarantee. Must be zero on input. |
| 40 | 8 | `uint64_t` | `max_dma_address` | out | Highest address the display can read; zero means no limit is exposed. Must be zero on input. |
| 48 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |
