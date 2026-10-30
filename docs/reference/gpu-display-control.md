# GPU display control: power, refresh, cursor and planes

Status: experimental reference for GPU ABI version 1.

This document describes the requests that control an output a program has leased: its power (`GPU_DISPLAY_POWER`), its refresh boundaries (`GPU_DISPLAY_REFRESH`), its cursor (`GPU_DISPLAY_CURSOR_SET`, `GPU_DISPLAY_CURSOR_MOVE` and `GPU_DISPLAY_CURSOR_HIDE`) and its planes (`GPU_DISPLAY_COMMIT` and `GPU_DISPLAY_OVERLAY_INFO`). Only `GPU_DISPLAY_POWER` and `GPU_DISPLAY_REFRESH` need `GPU_CAP_DISPLAY_CONTROL`, and the kernel registers a node with that bit only if its driver has both operations. The other five need `GPU_CAP_DISPLAY` and a driver that has the operation (the cursor operations come as a set of three, and a commit comes with the overlay limits); a driver without it answers `EOPNOTSUPP`.

The kernel reads no per-output flag. The flags of `struct gpu_display_info` tell a program what to expect before it calls: `GPU_DISPLAY_POWER_CONTROL` for `GPU_DISPLAY_POWER`, `GPU_DISPLAY_REFRESH_COUNTER` for `GPU_DISPLAY_REFRESH`, `GPU_DISPLAY_HAS_CURSOR` for the three cursor requests and `GPU_DISPLAY_HAS_OVERLAYS` for `GPU_DISPLAY_COMMIT` and `GPU_DISPLAY_OVERLAY_INFO`. The first two are set only on a node with `GPU_CAP_DISPLAY_CONTROL`. An output whose flag is clear is answered by its driver, usually with `EOPNOTSUPP`; which drivers set which flags is in [gpu-drivers.md](gpu-drivers.md).

The cursor requests and `GPU_DISPLAY_COMMIT` carry the `lease` they act on, and `GPU_DISPLAY_POWER` names the output and needs this open's lease on it. `GPU_DISPLAY_REFRESH` and `GPU_DISPLAY_OVERLAY_INFO` name an output and need no lease. Leases, generations and sequence numbers are described in [gpu-display.md](gpu-display.md), and the rules shared by all requests (framing, open modes, concurrency) are in [gpu.md](gpu.md).

## Refresh boundaries

A refresh boundary is the instant at which an output starts a new refresh. `GPU_DISPLAY_REFRESH` reports the boundaries as a count in `sequence` and the time of the latest one in `time_ns`. The count never goes back. It counts boundaries and not calls, so it can advance by more than one between two calls, and a reply to a wait from a nonzero `cursor` always holds a count above `cursor`.

There are two kinds of boundary. On an output that scans out to a monitor, a boundary is a frame of the display controller (amdrdna, i915 and nvrtx read the frame counter of the controller's timing generator). An output with `GPU_DISPLAY_VIRTUAL_CLOCK` (venus) has no scanout of its own: its boundaries are the ticks of a clock the driver keeps at the output's refresh rate, and the reply says so with `GPU_DISPLAY_VIRTUAL_CLOCK` in `flags`. The reply's `flags` is zero for the other kind. In both kinds `time_ns` is on the monotonic scheduler clock of the kernel, which advances in steps of 1 ms, and the drivers read the count at least once a tick.

An output that does not scan out makes no boundary. A wait for a boundary after a nonzero `cursor` on such an output ends with `ETIMEDOUT` when its time runs out, and so does a wait with a timeout of zero when no boundary has come after `cursor`. Which outputs do not scan out depends on the driver: an output the lease holder has powered off with `GPU_DISPLAY_POWER` makes none on amdrdna and venus, and i915 lights only one output, so its other connectors make none. A call with a `cursor` of zero never waits; it reports the count at that moment, which can be zero when nothing has been counted yet.

Other changes are tied to a boundary. A change of the cursor (`GPU_DISPLAY_CURSOR_SET`, `GPU_DISPLAY_CURSOR_MOVE`, `GPU_DISPLAY_CURSOR_HIDE`) shows from the next boundary, and the call does not wait for it. The planes of a `GPU_DISPLAY_COMMIT` show together, from one boundary. `GPU_DISPLAY_WAIT`, and a commit with `GPU_DISPLAY_COMMIT_WAIT`, report when a record has been taken by a boundary (see [gpu-display.md](gpu-display.md)).

## Planes and commits

`GPU_DISPLAY_COMMIT` changes the state of several planes of one leased output at once. This section gives the rules of the commit; the request and its fields are described under Requests.

**Planes.** Plane 0 (`GPU_DISPLAY_PLANE_PRIMARY`) is the primary plane. It shows the whole output, unscaled. Planes 1 to `plane_count` - 1 of `struct gpu_display_info` are overlay planes, at most `GPU_DISPLAY_OVERLAY_MAX` of them, so a commit names at most five planes. An output has overlays only if it sets `GPU_DISPLAY_HAS_OVERLAYS`; in the drivers read, only the bcm2711 display node does, with two overlays. The cursor plane is not one of these planes (see `GPU_DISPLAY_CURSOR_SET`).

**What a commit names.** A commit holds `count` states, from 1 to 5, each for one plane. Planes it does not name keep their state, and a plane named twice fails with `EINVAL`. The entries of `planes` from `count` on are not part of the request; the kernel requires `plane`, `flags`, `handle` and `result` zero and `wait_fd` -1 in them and reads nothing else. A plane number above `GPU_DISPLAY_OVERLAY_MAX` fails with `EINVAL`, and a plane the output does not have is refused by the driver.

**Shown planes and images.** `GPU_DISPLAY_PLANE_SHOWN` puts the plane in the list the display uses; clear, the plane is hidden and no image is read or borrowed for it. `GPU_DISPLAY_PLANE_IMAGE` says that `handle`, `offset`, `width`, `height`, `stride` and `format` name a new image for the plane; clear, the plane keeps the image it has, `handle` must be zero and `wait_fd` -1, and those image fields are not read. A shown plane without `GPU_DISPLAY_PLANE_IMAGE` needs an image from an earlier state of the same lease, or it fails with `EINVAL` in the driver (bcm2711). A new image is for a shown plane: bcm2711 refuses `GPU_DISPLAY_PLANE_IMAGE` on a hidden plane with `EINVAL`. The image is a native shared allocation of this open, a blob or a scanout allocation (see [gpu-memory.md](gpu-memory.md) and [gpu-sharing.md](gpu-sharing.md)); ordinary storage is refused with `EINVAL`. Its `format` is `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, `stride` is at least `width` times 4 and a multiple of 4, and `stride` times `height` from `offset` lies inside the allocation. The pixels of an overlay carry premultiplied alpha. The primary is opaque.

**Rectangles.** For a shown plane the source rectangle (`src_x`, `src_y`, `src_width`, `src_height`) lies inside the image, and the destination rectangle (`dst_x`, `dst_y`, `dst_width`, `dst_height`) lies inside the output. Both sizes are nonzero. The kernel checks that the source lies inside a new image and that `dst_x` plus `dst_width` and `dst_y` plus `dst_height` do not overflow; the driver checks the rest. The primary shows the whole image over the whole output without scaling; bcm2711 refuses any other rectangle of the primary with `EINVAL`. An overlay may be smaller than the output and may be scaled, within the limits `GPU_DISPLAY_OVERLAY_INFO` reports: `GPU_DISPLAY_OVERLAY_SCALE_UP` says the destination may be larger than the source, and `GPU_DISPLAY_OVERLAY_SCALE_DOWN_X` and `GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y` say it may be narrower or lower. An overlay shows only over a primary that is shown: bcm2711 refuses a commit that leaves an overlay shown over a hidden primary with `EINVAL`.

**Serials and DROPPED.** Each state carries a `serial`, nonzero, that increases for each state of that plane. The driver keeps the serial of the last state it recorded for the plane. A state whose serial is at or below it is not recorded and reports `GPU_DISPLAY_PLANE_DROPPED` in `result`; a recorded state reports `GPU_DISPLAY_PLANE_APPLIED`. A dropped state does not fail the call and does not stop the other states of the same call from being recorded. This is the one exception to the rule that a commit changes all its planes or none: when the record fails (for example with `ENOSPC` or `EINVAL`), no state is recorded, but when it succeeds some of its states may have been dropped. A commit whose states are all dropped changes nothing and records no new number. On bcm2711 a lease starts with every plane hidden and its serials counted afresh. `result` is zero on input and is written only when the call succeeds.

**Fences.** A plane may wait for the writes to its new image: `wait_fd` (-1 for none) with `wait_generation` names a GPU fence that the image's producer signals. The commit waits for every `wait_fd` of its states before it records anything, holding no admission, and it waits for a fence of another GPU node as well as of this one (see [gpu-fences.md](gpu-fences.md)). A fence whose work the driver has not yet taken fails at once with `EAGAIN`, and a fence that ended in error fails the commit with that error. With `GPU_DISPLAY_COMMIT_WAIT`, `signal_fd` (-1 for none) with `signal_generation` names a fence of this node that the commit signals when its record is adopted. If the record or the adoption fails, the fence is signaled with the error instead; a failure before the record (a `wait_fd`, or the binding of the fence) leaves it untouched. A commit with a fence needs `GPU_CAP_FENCE`; without it the call fails with `EOPNOTSUPP`.

**Borrowing.** A recorded image is borrowed: the display reads it until a list that does not name it has been adopted, which happens when a later state replaces it or hides the plane, or when the lease ends. The kernel holds the image only while the driver records the planes; the driver keeps its own reference to every image it records. A program does not write an image while it is borrowed. When a commit names storage that the CPU caches as a new image, the driver cleans the cache lines of the image once, at that commit (bcm2711), so a program finishes writing an image before it commits it and does not write it while it is borrowed.

**Numbers.** A commit that records at least one state is given the next number of the count that `GPU_DISPLAY_PRESENT` shares (see [gpu-display.md](gpu-display.md)), returned in `sequence`; `GPU_DISPLAY_WAIT` waits for it. A driver may also advance the count for other changes, so consecutive numbers can skip (bcm2711 does when a shown cursor changes). `adopted` is the highest number that is adopted together with every lower one when the call returns: a number is adopted when a list that shows it has been taken by the display, or a later state it was replaced by has been. When nothing was recorded, `sequence` is the output's last number. Without `GPU_DISPLAY_COMMIT_WAIT` the call records and returns, and `adopted` can be below `sequence`. With it, the call returns after a list made from this record or a later one has been adopted, so `adopted` is at least `sequence`; `GPU_DISPLAY_COMMIT_WAIT` needs the primary plane among the named planes.

**Limits.** A commit that would exceed the output's bandwidth or the limits of the overlays fails with `ENOSPC`, and nothing changes (bcm2711 checks a bandwidth budget, the clock the planes need, the largest positions and sizes of an overlay, the reduction ratio and the length of the line buffer of a vertically scaled overlay).

**When the adoption does not come.** With `GPU_DISPLAY_COMMIT_WAIT`, the driver bounds the wait for the adoption (bcm2711: about 100 ms) and fails with `ETIMEDOUT` when it does not come. The record was made before the wait began, so the failure does not undo it, and the call writes nothing back, so `sequence` and `result` are not returned. The output then keeps every image it borrowed, and the later commits and presents of the lease fail with `EBUSY`. Releasing the lease or closing the open lets the images go. What the output shows after that is decided by the driver (see User-visible side effects in [gpu-display.md](gpu-display.md#user-visible-side-effects)); a program does not count on the text console coming back.

**First flip.** The first `GPU_DISPLAY_COMMIT` that succeeds, like any first accepted flip of the boot, ends the text console's use of the display (see [gpu-display.md](gpu-display.md#user-visible-side-effects)).

## Requests

### `GPU_DISPLAY_POWER`

Requires: open mode W; capability `GPU_CAP_DISPLAY` and `GPU_CAP_DISPLAY_CONTROL`.

Switches the output the open leases on or off. `display_id` and `generation` name the output and must be nonzero, `state` is `GPU_DISPLAY_POWER_ON`, `GPU_DISPLAY_POWER_OFF` or `GPU_DISPLAY_POWER_SUSPEND`, and `reserved` is zero. Every field is an input, and the request writes nothing back. The open must hold the lease of the output; an output that this open has not leased, or that another open has, fails in the driver with `EBUSY`. `GPU_DISPLAY_POWER_SUSPEND` may do the same as `GPU_DISPLAY_POWER_OFF`: it does in amdrdna, i915 and venus. The request takes the open's admission like the other requests that change a lease (see [gpu.md](gpu.md)), so it waits for another request of the same open and a signal ends that wait with `EINTR`.

What off does depends on the driver. amdrdna blanks the plane, so the output shows black, venus disables the output's scanout and keeps its images, and i915 switches off the light of an eDP panel. On amdrdna and venus an output that is off makes no refresh boundary (see Refresh boundaries). The output reports it with `GPU_DISPLAY_POWERED_OFF` in the `flags` of `GPU_DISPLAY_QUERY`. Asking for the state the output already has succeeds. The end of the lease powers the output on again. A power change is not a topology change: the output's `generation` stays the same and no display event is raised.

The request needs the node-wide `GPU_CAP_DISPLAY_CONTROL`, but it works only on an output with `GPU_DISPLAY_POWER_CONTROL`. For an output without it the driver answers `EOPNOTSUPP`: nvrtx answers so for its output after it has checked the display, the generation and the lease, and i915 answers so for its resident output unless that is an eDP panel, before it checks the lease.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY` or `GPU_CAP_DISPLAY_CONTROL`.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; `reserved` is not zero; `state` is not one of the three values.
- `ENODEV`: The device is offline (withdrawn).
- `EINTR`: A signal arrived while the request waited for the open's admission.
- Other values: Driver answers pass through. `EBUSY` for an output that this open has not leased (i915 also answers it while no frame of the lease is shown yet), `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EOPNOTSUPP` for an output without power control (see above), `ENOENT` for a `display_id` the output does not have, `EIO` for a display that is broken or a command that failed, and `ENODEV` for a failed transport (venus). The device's or the open's recorded error is returned if it has failed.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`), [present.c](../../src/drivers/gpu/amdrdna/present.c) (`present_power`), [display.c](../../src/drivers/gpu/i915/display/display.c) (`i915_display_power`), [display.c](../../src/drivers/gpu/venus/display.c) (`display_power`), [display-control](../../userland/tests/display-control/main.c).

### `GPU_DISPLAY_REFRESH`

Requires: open mode R; capability `GPU_CAP_DISPLAY_CONTROL`.

Waits for the next refresh boundary of an output after `cursor`, or reports the current count (see Refresh boundaries). `display_id` and `generation` name the output and must be nonzero. `flags`, `sequence` and `time_ns` are zero on input. `cursor` is a count the program has seen: zero reports the current count at once, and a nonzero value waits for a boundary with a count above it. The usual loop passes the `sequence` of the last reply as the next `cursor`. `timeout_ns` bounds the wait: the kernel cuts a value above one second to one second, so one call waits at most one second and a program that needs longer calls again; a zero with a nonzero `cursor` makes one check and fails with `ETIMEDOUT` if no boundary after `cursor` has come.

On success `sequence` is the count of the boundary, above a nonzero `cursor`, `time_ns` is the time of the boundary on the 1 ms scheduler clock, and `flags` is `GPU_DISPLAY_VIRTUAL_CLOCK` when the boundary is a virtual clock's and zero otherwise. A reply with a nonzero `cursor` whose `sequence` is not above it is rejected with `EIO`. The request needs no lease, so any open that can read may call it, and the output's flag `GPU_DISPLAY_REFRESH_COUNTER` says that the output has boundaries. It runs outside the open's admission, so it does not hold up the open's other requests while it sleeps. The drivers read (amdrdna, i915, nvrtx and venus) sleep without checking for signals, so a signal does not end the wait early; the wait ends at the timeout. On failure the argument is not written.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY_CONTROL`.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; `flags`, `sequence` or `time_ns` is not zero.
- `EIO`: The driver's reply has a `sequence` that is not above a nonzero `cursor`.
- `ENODEV`: The device is offline (withdrawn).
- Other values: Driver answers pass through. `ETIMEDOUT` when no boundary after `cursor` came within `timeout_ns` (also when the output makes no boundary, and for a zero `timeout_ns` with a nonzero `cursor`), `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `ENOENT` (amdrdna, i915, nvrtx) or `EINVAL` (venus) for a `display_id` the node does not have, `ENODEV` for a failed transport (venus) and `EOVERFLOW` when the virtual clock cannot be computed (venus). The device's or the open's recorded error is returned if it has failed.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_refresh_ioctl`), [control.c](../../src/drivers/gpu/i915/display/control.c) (`drv_i915_display_refresh_wait`), [display.c](../../src/drivers/gpu/venus/display.c) (`display_refresh_boundary`), [display-control](../../userland/tests/display-control/main.c).

### `GPU_DISPLAY_CURSOR_SET`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Sets the image of the cursor of the output the open leases. `lease` is the lease of the output's primary plane and must be this open's, `generation` is the output's, and `handle` names storage of this open that holds the image: ordinary storage, a blob or a scanout allocation. `offset` is the byte offset of the first row. `width` and `height` are from 1 to `GPU_DISPLAY_CURSOR_MAX` pixels, `stride` is at least `width` times 4 and a multiple of 4, and `format` is `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, a value and not a `GPU_DISPLAY_FORMAT_*` bit. The pixels carry premultiplied alpha. Every row, that is `stride` times `height` - 1 plus `width` times 4 bytes from `offset`, lies inside the storage. `reserved` is zero. The request writes nothing back.

The driver copies the image before the call returns, so the storage is not borrowed afterwards and the program may write it at once. A driver may skip the copy when it holds the same rows from storage that has not changed since, and it always copies storage that a process maps. A cursor that is shown shows the new image from the next refresh boundary; a cursor that is hidden stays hidden until `GPU_DISPLAY_CURSOR_MOVE`. The image is kept until the lease ends or a new one is set.

The request needs an output with a cursor plane (`GPU_DISPLAY_HAS_CURSOR`). On a node whose driver has no cursor operations it fails with `EOPNOTSUPP`. A driver may have the operations and still answer `EOPNOTSUPP` for an output without the plane (nvrtx does when its cursor channel was not set up). The three cursor requests run outside the open's admission on a node with ungated display and under it on the others (see [gpu.md](gpu.md)).

The request can fail with `EBUSY` for a moment even for the open's own lease: when every cursor image the hardware has is still named by a display list it has not yet left, and none holds the same rows, nothing changes and the call may succeed a refresh later (ghci, bcm2711 and nvrtx).

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`, or its driver has no cursor operations.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `lease` or `generation` is zero; `reserved` is not zero; `width` or `height` is zero or above `GPU_DISPLAY_CURSOR_MAX`; `format` is not a defined value; `stride` is smaller than `width` times 4 or not a multiple of 4; `handle` is zero, unknown or of a kind that cannot hold a frame; `offset` or the rows lie outside the storage.
- `ENODEV`: The device is offline (withdrawn), or the device or the open has failed.
- `EINTR`: A signal arrived while the request waited for the open's admission (on a node without ungated display).
- Other values: Driver answers pass through. `EBUSY` for a lease that is not this open's or that has ended, or for the moment described above, `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EOPNOTSUPP` for an output without a cursor plane, `EINVAL` for an image the plane cannot take, `ENOMEM` when the driver cannot allocate the cursor surfaces and `EIO` for a display that is broken.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_cursor_resource`, `gpu_display_ungated`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`output_cursor_set`), [display.c](../../src/drivers/gpu/ghci/display.c) (`display_cursor_set`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_CURSOR_MOVE`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Shows the cursor of the output the open leases with the top-left corner of its image at (`x`, `y`) in output pixels. The position is of the image's corner, so a program subtracts the image's hotspot itself. The coordinates may be negative or beyond the output; the part of the image outside the output is clipped, and an image wholly outside is not visible. `lease` is this open's lease of the output's primary plane and `generation` is the output's; `reserved` is zero. The request writes nothing back.

The move takes effect at the next refresh boundary, and the call does not wait for it. A later move before that boundary replaces this one. A move also shows a cursor that was hidden. The lease must have an image set with `GPU_DISPLAY_CURSOR_SET`; without one the request fails with `EINVAL`. Like the other cursor requests it needs an output with a cursor plane (`GPU_DISPLAY_HAS_CURSOR`), and a node whose driver has no cursor operations answers `EOPNOTSUPP`.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`, or its driver has no cursor operations.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `lease` or `generation` is zero; `reserved` is not zero.
- `ENODEV`: The device is offline (withdrawn), or the device or the open has failed.
- `EINTR`: A signal arrived while the request waited for the open's admission (on a node without ungated display).
- Other values: Driver answers pass through. `EINVAL` when the lease has set no image, `EBUSY` for a lease that is not this open's or that has ended, `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EOPNOTSUPP` for an output without a cursor plane and `EIO` for a display that is broken.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_ungated`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`output_cursor_move`), [present.c](../../src/drivers/gpu/amdrdna/present.c) (`present_cursor_move`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_CURSOR_HIDE`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Hides the cursor of the output the open leases, from the next refresh boundary. The image is kept, so a later `GPU_DISPLAY_CURSOR_MOVE` shows it again. Hiding a cursor that is already hidden succeeds. `lease` is this open's lease of the output's primary plane and `generation` is the output's; `reserved` is zero. The request writes nothing back. The end of the lease and a device fault hide the cursor too. It needs an output with a cursor plane (`GPU_DISPLAY_HAS_CURSOR`); the errors are those of `GPU_DISPLAY_CURSOR_SET`, except that the request has no image to check.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`, or its driver has no cursor operations.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read.
- `EINVAL`: `version` or `size` is not the structure's; `lease` or `generation` is zero; `reserved` is not zero.
- `ENODEV`: The device is offline (withdrawn), or the device or the open has failed.
- `EINTR`: A signal arrived while the request waited for the open's admission (on a node without ungated display).
- Other values: Driver answers pass through. `EBUSY` for a lease that is not this open's or that has ended, `ESTALE` for an old `generation`, `ENXIO` for an output that is not connected, `EOPNOTSUPP` for an output without a cursor plane and `EIO` for a display that is broken.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_ungated`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`output_cursor_hide`), [present.c](../../src/drivers/gpu/amdrdna/present.c) (`present_cursor_hide`), [display-scanout](../../userland/tests/display-scanout/main.c).

### `GPU_DISPLAY_COMMIT`

Requires: open mode W; capability `GPU_CAP_DISPLAY`.

Changes some planes of the output the open leases, as described in Planes and commits. `lease` is this open's lease of the output and `generation` the output's; both are nonzero. `count` is the number of states in `planes` (1 to 5), and each state has the fields of `struct gpu_display_plane_state`. `flags` is zero or `GPU_DISPLAY_COMMIT_WAIT`, and `signal_fd` with `signal_generation` is a fence to signal at the adoption, allowed only with `GPU_DISPLAY_COMMIT_WAIT`. `reserved0` and `reserved` are zero, and `sequence` and `adopted` are outputs and zero on input, as is `result` of each state.

The call first waits for every `wait_fd`, then records the named states at once, and then, with `GPU_DISPLAY_COMMIT_WAIT`, waits for the adoption. It holds no admission of the open while it waits for the fences or for the adoption, so the open's other requests run meanwhile. On success it returns each state's `result` and the numbers `sequence` and `adopted`. A state that reports `GPU_DISPLAY_PLANE_DROPPED` was not recorded, and the others of the call were (see Serials and DROPPED in Planes and commits). The request needs a driver with a commit operation, and only a driver with ungated display has one: in the drivers read, only the bcm2711 display node. On any other node it fails with `EOPNOTSUPP`. If the kernel cannot copy the reply, the planes have already been recorded.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`; its driver has no commit operation; or the commit names a fence (a `wait_fd` or the `signal_fd`) and the node does not have `GPU_CAP_FENCE`.
- `EACCES`: The open cannot write.
- `EFAULT`: The argument cannot be read or written (the planes are already recorded if the reply cannot be written).
- `EINVAL`: `version` or `size` is not the structure's; `lease` or `generation` is zero; `reserved0`, `reserved`, `sequence` or `adopted` is not zero; `flags` has a bit other than `GPU_DISPLAY_COMMIT_WAIT`; `count` is zero or above 5; `signal_fd` is below -1, or is -1 with a nonzero `signal_generation`, or is a descriptor with a zero `signal_generation` or without `GPU_DISPLAY_COMMIT_WAIT`; `GPU_DISPLAY_COMMIT_WAIT` without the primary plane among the states; a plane named twice; a plane above `GPU_DISPLAY_OVERLAY_MAX`; `flags` of a state with a bit other than `GPU_DISPLAY_PLANE_SHOWN` and `GPU_DISPLAY_PLANE_IMAGE`; `result` nonzero or `serial` zero; `wait_fd` below -1, or -1 with a nonzero `wait_generation`, or a descriptor with a zero `wait_generation`; a shown plane with a zero source or destination size, or a destination that overflows; a state without `GPU_DISPLAY_PLANE_IMAGE` with a nonzero `handle` or a `wait_fd` other than -1; a new image with a zero `handle`, `width` or `height`, a `format` that is not a defined value, a `stride` below `width` times 4 or not a multiple of 4, or a source rectangle outside the image; an entry from `count` on that is not zero (or whose `wait_fd` is not -1); a `handle` that is unknown, or of ordinary storage; `offset` or `stride` times `height` outside the allocation; a `wait_fd` or the `signal_fd` that is a driver object but not a GPU fence.
- `EBADF`: A `wait_fd` or the `signal_fd` is not an open descriptor of a driver object.
- `EXDEV`: The `signal_fd` is a fence of another device.
- `EAGAIN`: A `wait_fd` names a generation whose work the driver has not yet taken, or the open has no free place to bind the `signal_fd`.
- `EBUSY`: The generation named by `signal_fd` is not pending or already has a producer.
- `ESTALE`: A fence generation is not the current one.
- `EINTR`: A signal arrived while the call waited for a `wait_fd`.
- `ENODEV`: The device is offline (withdrawn), or the device or the open has failed.
- Other values: Driver answers pass through, and so do the errors of the fences. `EBUSY` (from the driver) for a lease that is not this open's or that has ended (also while the call waits), after a `ETIMEDOUT` of the lease, and for a moment when the driver has no room for the images a commit replaces; `ESTALE` for an old `generation`; `ENOSPC` when the planes would exceed the output's bandwidth or the overlays' limits, with nothing changed; `ETIMEDOUT` when the adoption did not come; `EINVAL` for a state the output cannot show, such as an image of another layout than the allocation was imported with or a plane the output does not have; `EIO` for a display that is broken; a fence that ended in error fails the call with that error.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_commit_ioctl`, `gpu_commit_check`, `gpu_commit_plane_check`, `gpu_commit_record`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`output_commit`, `commit_change`, `output_adoption_wait`), [display-flip.c](../../src/drivers/gpu/bcm2711/display/display-flip.c) (`bcm2711_display_flip_commit`, `bcm2711_display_flip_wait`), [hvs-load](../../userland/tests/hvs-load/main.c).

### `GPU_DISPLAY_OVERLAY_INFO`

Requires: open mode R; capability `GPU_CAP_DISPLAY`.

Reports the limits of one overlay plane of an output on the output's current mode. `display_id` and `generation` name the output and must be nonzero, and `plane` is the number of an overlay plane, from 1 to `GPU_DISPLAY_OVERLAY_MAX`; 0, the primary, fails with `EINVAL`. The outputs are zero on input. The reply gives the largest source (`max_src_width`, `max_src_height`) and destination (`max_dst_width`, `max_dst_height`) sizes in pixels, in `flags` the scaling the plane does (`GPU_DISPLAY_OVERLAY_SCALE_UP`, `GPU_DISPLAY_OVERLAY_SCALE_DOWN_X`, `GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y`) and in `formats` the pixel formats it takes as a mask of `GPU_DISPLAY_FORMAT_*` bits. The limits are those of the mode in use when the request is made, and they change with it. A commit that stays within them can still fail, for example with `ENOSPC`. The request needs no lease. On failure the argument is not written.

The request works only on a node whose driver has an overlay operation and declares ungated display, because the kernel reaches the request only through the path of ungated display requests. On any other node, even one with `GPU_CAP_DISPLAY`, it fails with `EOPNOTSUPP`. In the drivers read, only the bcm2711 display node answers it, with two overlay planes. A request for an overlay plane the output does not have fails in the driver with `EINVAL`.

Errors:

- `EOPNOTSUPP`: The node does not have `GPU_CAP_DISPLAY`, its driver has no overlay operation or does not declare ungated display.
- `EACCES`: The open cannot read.
- `EFAULT`: The argument cannot be read or written.
- `EINVAL`: `version` or `size` is not the structure's; `display_id` or `generation` is zero; `plane` is 0 or above `GPU_DISPLAY_OVERLAY_MAX`; an output field or `reserved` is not zero.
- `ENODEV`: The device is offline (withdrawn), or the device or the open has failed.
- `EINTR`: A signal arrived while the request waited for the open's admission (on a node without ungated display, before it answers `EOPNOTSUPP`).
- Other values: Driver answers pass through. `ESTALE` for an old `generation`, `ENXIO` for an output the node does not have (bcm2711 answers it for an unknown `display_id`) or that is not connected, and `EINVAL` for an overlay plane the output does not have.

Sources: [gpu-display.h](../../include/uapi/gpu-display.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_display_ioctl`, `gpu_display_ungated`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`output_overlay_info`); no source-tree test.

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_POWER_ON` | 0 | `state` of `GPU_DISPLAY_POWER`: the output is lit and shows its image. |
| `GPU_DISPLAY_POWER_OFF` | 1 | `state` of `GPU_DISPLAY_POWER`: the output is off; the effect (black, scanout disabled, panel light off) depends on the driver. |
| `GPU_DISPLAY_POWER_SUSPEND` | 2 | `state` of `GPU_DISPLAY_POWER`: a low-power off; it may be the same as `GPU_DISPLAY_POWER_OFF`, and is in amdrdna, i915 and venus. |
| `GPU_DISPLAY_CURSOR_MAX` | 64 | Largest side of a cursor image in pixels; `width` and `height` of `GPU_DISPLAY_CURSOR_SET` are from 1 to this value. |

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_DISPLAY_PLANE_PRIMARY` | 0 | Number of the primary plane in `plane`: it shows the whole output, unscaled. |
| `GPU_DISPLAY_OVERLAY_MAX` | 4 | Largest number of overlay planes of an output, so planes 1 to this value; a commit names at most this many plus the primary. |
| `GPU_DISPLAY_PLANE_SHOWN` | 1 | Bit of the `flags` of a plane state: the plane is in the list the display uses; clear, it is hidden and no image is read or borrowed. |
| `GPU_DISPLAY_PLANE_IMAGE` | 2 | Bit of the `flags` of a plane state: `handle` to `format` name a new image; clear, the plane keeps its image. |
| `GPU_DISPLAY_PLANE_APPLIED` | 1 | `result` of a plane state: the state was recorded. |
| `GPU_DISPLAY_PLANE_DROPPED` | 2 | `result` of a plane state: not recorded, because a later serial of the plane was recorded before; the other states of the call were recorded. |
| `GPU_DISPLAY_COMMIT_WAIT` | 1 | Bit of the `flags` of a commit: return after the record is adopted, and signal `signal_fd` then; the primary must be among the planes named. |
| `GPU_DISPLAY_OVERLAY_SCALE_UP` | 1 | Bit of the `flags` of `struct gpu_display_overlay_info`: the destination may be larger than the source. |
| `GPU_DISPLAY_OVERLAY_SCALE_DOWN_X` | 2 | Bit of the `flags` of `struct gpu_display_overlay_info`: the destination may be narrower than the source. |
| `GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y` | 4 | Bit of the `flags` of `struct gpu_display_overlay_info`: the destination may be lower than the source. |

## Structures

#### `struct gpu_display_power`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_power)`). |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `state` | in | `GPU_DISPLAY_POWER_ON`, `GPU_DISPLAY_POWER_OFF` or `GPU_DISPLAY_POWER_SUSPEND`; any other value fails with `EINVAL`. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_display_refresh`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_refresh)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `flags` | in/out | Must be zero on input. The reply has `GPU_DISPLAY_VIRTUAL_CLOCK` when the boundary is a virtual clock's, otherwise zero. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 8 | `uint64_t` | `cursor` | in | Count the program has seen. Zero reports the current count at once; a nonzero value waits for a boundary with a count above it. |
| 32 | 8 | `uint64_t` | `timeout_ns` | in | Longest wait in nanoseconds. A value above one second is cut to one second; zero checks once and fails with `ETIMEDOUT` if no boundary after `cursor` has come. |
| 40 | 8 | `uint64_t` | `sequence` | out | Count of the boundary reported; above `cursor` when `cursor` is nonzero, and never decreasing. Must be zero on input. |
| 48 | 8 | `uint64_t` | `time_ns` | out | Time of the boundary in nanoseconds on the monotonic scheduler clock, which advances in steps of 1 ms. Must be zero on input. |

#### `struct gpu_display_cursor_image`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_cursor_image)`). |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open on the output's primary plane; nonzero. |
| 16 | 8 | `uint64_t` | `handle` | in | Handle of the storage that holds the image: ordinary storage, a blob or a scanout allocation of this open; nonzero. |
| 24 | 8 | `uint64_t` | `offset` | in | Byte offset of the first row in the storage. |
| 32 | 4 | `uint32_t` | `width` | in | Width of the image in pixels, from 1 to `GPU_DISPLAY_CURSOR_MAX`. |
| 36 | 4 | `uint32_t` | `height` | in | Height of the image in rows, from 1 to `GPU_DISPLAY_CURSOR_MAX`. |
| 40 | 4 | `uint32_t` | `stride` | in | Bytes from the start of one row to the next; at least `width` times 4 and a multiple of 4. |
| 44 | 4 | `uint32_t` | `format` | in | `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, a value and not a `GPU_DISPLAY_FORMAT_*` bit; premultiplied alpha. |
| 48 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 56 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_display_cursor_move`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_cursor_move)`). |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open on the output's primary plane; nonzero. It must have a cursor image. |
| 16 | 4 | `int32_t` | `x` | in | Horizontal position, in output pixels, of the image's top-left corner; may be negative. The caller subtracts the hotspot. |
| 20 | 4 | `int32_t` | `y` | in | Vertical position, in output pixels, of the image's top-left corner; may be negative. The caller subtracts the hotspot. |
| 24 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 32 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_display_cursor_hide`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_cursor_hide)`). |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open on the output's primary plane; nonzero. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_display_plane_state`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `plane` | in | Plane to change: 0 (`GPU_DISPLAY_PLANE_PRIMARY`) or an overlay, up to `GPU_DISPLAY_OVERLAY_MAX`. A plane may be named once in a commit. |
| 4 | 4 | `uint32_t` | `flags` | in | Mask of `GPU_DISPLAY_PLANE_SHOWN` and `GPU_DISPLAY_PLANE_IMAGE`; no other bit. |
| 8 | 8 | `uint64_t` | `serial` | in | Serial of this state of the plane; nonzero and increasing. A serial at or below the plane's last recorded one is dropped. |
| 16 | 8 | `uint64_t` | `handle` | in | Handle of the new image, a blob or a scanout allocation of this open. Nonzero with `GPU_DISPLAY_PLANE_IMAGE`, zero without. |
| 24 | 8 | `uint64_t` | `offset` | in | Byte offset of the first row of the new image in the allocation. Read only with `GPU_DISPLAY_PLANE_IMAGE`. |
| 32 | 4 | `uint32_t` | `width` | in | Width of the new image in pixels; nonzero. Read only with `GPU_DISPLAY_PLANE_IMAGE`. |
| 36 | 4 | `uint32_t` | `height` | in | Height of the new image in rows; nonzero. Read only with `GPU_DISPLAY_PLANE_IMAGE`. |
| 40 | 4 | `uint32_t` | `stride` | in | Bytes from one row of the new image to the next; at least `width` times 4 and a multiple of 4. Read only with `GPU_DISPLAY_PLANE_IMAGE`. |
| 44 | 4 | `uint32_t` | `format` | in | `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`, a value and not a `GPU_DISPLAY_FORMAT_*` bit; overlays use premultiplied alpha. Read only with `GPU_DISPLAY_PLANE_IMAGE`. |
| 48 | 4 | `uint32_t` | `src_x` | in | Left edge, in pixels of the image, of the part that is shown. Read for a shown plane. |
| 52 | 4 | `uint32_t` | `src_y` | in | Top edge, in rows of the image, of the part that is shown. Read for a shown plane. |
| 56 | 4 | `uint32_t` | `src_width` | in | Width of the part shown, in pixels; nonzero for a shown plane, and inside the image. |
| 60 | 4 | `uint32_t` | `src_height` | in | Height of the part shown, in rows; nonzero for a shown plane, and inside the image. |
| 64 | 4 | `uint32_t` | `dst_x` | in | Left edge, in output pixels, where the part is shown. |
| 68 | 4 | `uint32_t` | `dst_y` | in | Top edge, in output rows, where the part is shown. |
| 72 | 4 | `uint32_t` | `dst_width` | in | Width in output pixels of the shown part; nonzero for a shown plane. It differs from `src_width` only within the plane's scaling limits. |
| 76 | 4 | `uint32_t` | `dst_height` | in | Height in output rows of the shown part; nonzero for a shown plane. It differs from `src_height` only within the plane's scaling limits. |
| 80 | 4 | `int32_t` | `wait_fd` | in | Descriptor of a GPU fence that the image's writes signal, or -1 for none. It must be -1 without `GPU_DISPLAY_PLANE_IMAGE`. |
| 84 | 4 | `uint32_t` | `result` | out | `GPU_DISPLAY_PLANE_APPLIED` or `GPU_DISPLAY_PLANE_DROPPED` for the state. Must be zero on input; written only when the call succeeds. |
| 88 | 8 | `uint64_t` | `wait_generation` | in | Generation of the fence in `wait_fd`; nonzero with a descriptor, zero with -1. |

#### `struct gpu_display_commit`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_commit)`). The kernel also writes it in the reply. |
| 8 | 8 | `uint64_t` | `lease` | in | Lease of this open on the output; nonzero. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 4 | `uint32_t` | `flags` | in | Zero or `GPU_DISPLAY_COMMIT_WAIT`; no other bit. |
| 28 | 4 | `uint32_t` | `count` | in | Number of states used in `planes`, from 1 to 5. |
| 32 | 480 | `struct gpu_display_plane_state[5]` | `planes` | in/out | The states, one per plane named. Entries from `count` on are unused: `plane`, `flags`, `handle` and `result` zero and `wait_fd` -1. The `result` of each used state is an output. |
| 512 | 4 | `int32_t` | `signal_fd` | in | Descriptor of a GPU fence of this node to signal at the adoption, or -1 for none. Allowed only with `GPU_DISPLAY_COMMIT_WAIT`. |
| 516 | 4 | `uint32_t` | `reserved0` | zero | Reserved; must be zero. |
| 520 | 8 | `uint64_t` | `signal_generation` | in | Generation of the fence in `signal_fd`; nonzero with a descriptor, zero with -1. |
| 528 | 8 | `uint64_t` | `sequence` | out | Number the record was given; the output's last number when nothing was recorded. Must be zero on input. |
| 536 | 8 | `uint64_t` | `adopted` | out | Highest number adopted together with every lower one when the call returns. Must be zero on input. |
| 544 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |

#### `struct gpu_display_overlay_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. The kernel also writes it in the reply. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_display_overlay_info)`). The kernel also writes it in the reply. |
| 8 | 4 | `uint32_t` | `display_id` | in | Identifier of the output, from `GPU_DISPLAY_QUERY`; nonzero. |
| 12 | 4 | `uint32_t` | `plane` | in | Number of the overlay plane, from 1 to `GPU_DISPLAY_OVERLAY_MAX`; 0, the primary, fails with `EINVAL`. |
| 16 | 8 | `uint64_t` | `generation` | in | Generation of the output from the query; nonzero. An old value fails with `ESTALE`. |
| 24 | 4 | `uint32_t` | `max_src_width` | out | Largest source width in pixels. Must be zero on input. |
| 28 | 4 | `uint32_t` | `max_src_height` | out | Largest source height in rows. Must be zero on input. |
| 32 | 4 | `uint32_t` | `max_dst_width` | out | Largest destination width in output pixels. Must be zero on input. |
| 36 | 4 | `uint32_t` | `max_dst_height` | out | Largest destination height in output rows. Must be zero on input. |
| 40 | 4 | `uint32_t` | `flags` | out | Mask of `GPU_DISPLAY_OVERLAY_SCALE_*` bits: the scaling the plane does. Must be zero on input. |
| 44 | 4 | `uint32_t` | `formats` | out | Mask of `GPU_DISPLAY_FORMAT_*` bits: the pixel formats the plane takes. Must be zero on input. |
| 48 | 8 | `uint64_t` | `reserved` | zero | Reserved; must be zero. |
