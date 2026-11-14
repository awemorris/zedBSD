/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Optional native display operations, independent of a rendering command ABI.
 */

#ifndef DRIVERS_GPU_DISPLAY_H
#define DRIVERS_GPU_DISPLAY_H

#include <uapi/gpu-display.h>

/*
 * Immutable display operations shared by every instance of one GPU backend.
 * The core passes validated kernel request copies and resolves presentation
 * storage before calling the backend. No callback runs under a core spinlock.
 * The backend owns lease arbitration, frame timing and scanout lifetimes.
 */
struct drv_gpu_display_ops {
	int (*query)(void *, void *, struct gpu_display_info *);
	int (*mode)(void *, void *, struct gpu_display_mode *);
	int (*claim)(void *, void *, struct gpu_display_claim *);
	int (*release)(void *, void *, const struct gpu_display_release *);
	int (*present)(void *, void *, void *, struct gpu_display_present *);
	int (*wait)(void *, void *, struct gpu_display_wait *);

	/*
	 * Optional IRQ-safe sequence snapshot, independent of the controller mutex.
	 * It neither sleeps, allocates, clears events nor issues hardware commands.
	 * The file retains the backend session even when unregister makes it offline.
	 * A driver-owned sequence starts at one and never wraps; overflow is terminal
	 * EOVERFLOW. The sequence belongs to the device, the same for every open.
	 * Event publication calls drv_gpu_display_changed() with the new sequence
	 * outside its short lock, through a registration handle that its teardown
	 * keeps alive until the publication can no longer run.
	 */
	int (*events)(void *, void *, uint64_t *);

	/*
	 * GPU_CAP_DISPLAY_CONTROL: both, or neither.  power gets a checked request
	 * (state ON, OFF or SUSPEND, reserved zero) from a writable open, inside the
	 * ordinary admission; refresh gets one from a readable open outside it, with
	 * timeout_ns bounded by the core, and may sleep that long.  When the driver
	 * has no refresh_arm, refresh is also called by the core's refresh watcher,
	 * a thread of the device, with a NULL session, to wait for the boundaries
	 * that end GPU_DISPLAY_FENCE refresh registrations.
	 */
	int (*power)(void *, void *, const struct gpu_display_power *);
	int (*refresh)(void *, void *, struct gpu_display_refresh *);

	/*
	 * Optional cursor plane (GPU_DISPLAY_HAS_CURSOR on an output): all three,
	 * or none.  Each gets a checked request from a writable open, inside the
	 * ordinary admission, and checks this open's lease before any side effect.
	 * cursor_set also gets the resolved storage object, whose bounds the core
	 * checked, and copies the image before it returns; none of them waits for
	 * the refresh boundary that shows the change.
	 */
	int (*cursor_set)(void *, void *, void *, const struct gpu_display_cursor_image *);
	int (*cursor_move)(void *, void *, const struct gpu_display_cursor_move *);
	int (*cursor_hide)(void *, void *, const struct gpu_display_cursor_hide *);

	/*
	 * Optional ungated presentation (DRV_GPU_DISPLAY_UNGATED in flags):
	 * adoption_wait and the flag come together.  Then present, wait, the
	 * three cursor operations, commit and overlay_info run outside the
	 * open's ordinary admission, at the same time as each other and as
	 * admitted operations on the same open.  A resource the core passes
	 * stays pinned (its destruction waits) only while the callback runs, so
	 * the backend takes its own reference to anything it keeps.  present
	 * records the frame without waiting for the refresh boundary and leaves
	 * the record's number in sequence; the core then calls adoption_wait
	 * with the lease and that number, holding neither a pin nor a lock, and
	 * it waits for the adoption and puts the number the caller sees in
	 * *sequence and, when adopted is not NULL, the highest adopted number in
	 * *adopted.  PRESENT_SYNC and COMMIT wait for their dependencies before
	 * any resource is looked up.
	 */
	int (*adoption_wait)(void *, void *, uint64_t, uint64_t *, uint64_t *);
	uint32_t flags;

	/*
	 * Optional overlay planes (GPU_DISPLAY_HAS_OVERLAYS on an output): both
	 * or neither, and commit needs DRV_GPU_DISPLAY_UNGATED.  commit gets a
	 * checked request from a writable open (defined flags, distinct planes,
	 * the primary named for COMMIT_WAIT, every named image inside its
	 * storage) with objects[i] the pinned storage of planes[i] when it names
	 * an IMAGE (NULL otherwise); it records without waiting, fills each
	 * plane's result, sequence and adopted, and the core waits through
	 * adoption_wait for COMMIT_WAIT.  overlay_info gets a checked request from
	 * a readable open.
	 */
	int (*commit)(void *, void *, void *const *, struct gpu_display_commit *);
	int (*overlay_info)(void *, void *, struct gpu_display_overlay_info *);

	/*
	 * Optional push of refresh boundaries, with GPU_CAP_DISPLAY_CONTROL.  The
	 * core calls refresh_arm(private, display_id, 1) in a thread, without its
	 * locks, after a refresh registration of the output is armed, and
	 * refresh_arm(private, display_id, 0) after a cancel leaves the output
	 * none; calls for one device do not overlap.  While armed, the driver calls
	 * drv_gpu_display_refreshed() at each boundary of the output, with the
	 * count refresh reports; when that returns zero the driver may stop until
	 * the next arm.  A nonzero return from an arm refuses the registration.
	 * Without refresh_arm the core waits for the boundaries with refresh.
	 */
	int (*refresh_arm)(void *, uint32_t, unsigned);
};

/* drv_gpu_display_ops.flags: presentation and the cursor bypass the open's admission. */
#define DRV_GPU_DISPLAY_UNGATED	1U

/*
 * drv_gpu_display_ops.flags: the backend shows a primary turned by 180
 * degrees (GPU_DISPLAY_PRESENT_ROTATE_180, GPU_DISPLAY_PLANE_ROTATE_180) on
 * the outputs whose query reports GPU_DISPLAY_ROTATE_180, and refuses the
 * turn with EOPNOTSUPP on any other.  Without it the core refuses the turn
 * before the backend sees the request.
 */
#define DRV_GPU_DISPLAY_ROTATE_180	2U

#endif
