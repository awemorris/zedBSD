/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Optional display discovery, ownership and completed-frame presentation requests.
 */

#ifndef KERN_UAPI_GPU_DISPLAY_H
#define KERN_UAPI_GPU_DISPLAY_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define GPU_CAP_DISPLAY			128U
#define GPU_CAP_DISPLAY_EVENTS		4096U
#define GPU_CAP_DISPLAY_CONTROL		32768U
#define GPU_DISPLAY_EVENT_CHANGE	1U
#define GPU_DISPLAY_EVENT_ACK		1U
#define GPU_DISPLAY_CONNECTED		1U
#define GPU_DISPLAY_VIRTUAL_CLOCK	2U
#define GPU_DISPLAY_FIFO		4U
#define GPU_DISPLAY_ATOMIC_MODE_PRESENT	8U
#define GPU_DISPLAY_ACTIVE		16U
#define GPU_DISPLAY_BLOB		32U
#define GPU_DISPLAY_POWER_CONTROL	64U
#define GPU_DISPLAY_POWERED_OFF		128U
#define GPU_DISPLAY_REFRESH_COUNTER	256U
#define GPU_DISPLAY_LIMITED		512U
#define GPU_DISPLAY_HAS_CURSOR		1024U

/* gpu_display_info.flags: planes 1 to plane_count - 1 are overlays (GPU_DISPLAY_COMMIT, GPU_DISPLAY_OVERLAY_INFO). */
#define GPU_DISPLAY_HAS_OVERLAYS	2048U
#define GPU_DISPLAY_FORMAT_BGRA8888	1U
#define GPU_DISPLAY_FORMAT_RGBA8888	2U
#define GPU_DISPLAY_MODE_ENUMERATE	0U
#define GPU_DISPLAY_MODE_VALIDATE	1U
#define GPU_DISPLAY_PRESENT_FIFO	1U
#define GPU_DISPLAY_PRESENT_BLOB	2U
#define GPU_DISPLAY_COUNT_ONLY		UINT32_MAX
#define GPU_DISPLAY_POWER_ON		0U
#define GPU_DISPLAY_POWER_OFF		1U
#define GPU_DISPLAY_POWER_SUSPEND	2U
#define GPU_DISPLAY_CURSOR_MAX		64U

/* The planes of one commit: the primary (0) and at most four overlays. */
#define GPU_DISPLAY_PLANE_PRIMARY	0U
#define GPU_DISPLAY_OVERLAY_MAX		4U

/* gpu_display_plane_state.flags (inputs). */
#define GPU_DISPLAY_PLANE_SHOWN		1U	/* in the list; clear: hidden, no image read or borrowed */
#define GPU_DISPLAY_PLANE_IMAGE		2U	/* handle to format name a new image; clear: keep the image */

/* gpu_display_plane_state.result (output). */
#define GPU_DISPLAY_PLANE_APPLIED	1U
#define GPU_DISPLAY_PLANE_DROPPED	2U	/* a later serial of the plane was applied before */

/* gpu_display_commit.flags (inputs). */
#define GPU_DISPLAY_COMMIT_WAIT		1U	/* return after adoption; needs the primary named */

/* gpu_display_overlay_info.flags (outputs). */
#define GPU_DISPLAY_OVERLAY_SCALE_UP	1U	/* dst may be larger than src */
#define GPU_DISPLAY_OVERLAY_SCALE_DOWN_X 2U	/* dst may be narrower than src */
#define GPU_DISPLAY_OVERLAY_SCALE_DOWN_Y 4U	/* dst may be lower than src */

#define GPU_DISPLAY_QUERY		_IOWR('G', 24, struct gpu_display_info)
#define GPU_DISPLAY_MODE		_IOWR('G', 25, struct gpu_display_mode)
#define GPU_DISPLAY_CLAIM		_IOWR('G', 26, struct gpu_display_claim)
#define GPU_DISPLAY_RELEASE		_IOW('G', 27, struct gpu_display_release)
#define GPU_DISPLAY_PRESENT		_IOWR('G', 28, struct gpu_display_present)
#define GPU_DISPLAY_WAIT		_IOWR('G', 29, struct gpu_display_wait)
#define GPU_DISPLAY_EVENTS		_IOWR('G', 32, struct gpu_display_events)
#define GPU_DISPLAY_POWER		_IOW('G', 39, struct gpu_display_power)
#define GPU_DISPLAY_REFRESH		_IOWR('G', 40, struct gpu_display_refresh)
#define GPU_DISPLAY_CURSOR_SET		_IOW('G', 41, struct gpu_display_cursor_image)
#define GPU_DISPLAY_CURSOR_MOVE		_IOW('G', 42, struct gpu_display_cursor_move)
#define GPU_DISPLAY_CURSOR_HIDE		_IOW('G', 43, struct gpu_display_cursor_hide)
#define GPU_DISPLAY_COMMIT		_IOWR('G', 44, struct gpu_display_commit)
#define GPU_DISPLAY_OVERLAY_INFO	_IOWR('G', 45, struct gpu_display_overlay_info)

/*
 * One non-destructive display-change snapshot, with an optional exact acknowledgement.
 * Query inputs are version and size. ACK additionally sets flags and ack_sequence
 * to an earlier successfully returned sequence; all output/reserved inputs are zero.
 * Sequence starts at one, so a new open initially requires a complete display query.
 * Query, re-enumerate outputs/generations, then ACK that snapshot. Events after it
 * remain POLLPRI-ready. Copyout failure never acknowledges or records observation.
 * Acknowledgement belongs to the open description shared by dup and SCM_RIGHTS.
 * Notifications may coalesce or repeat; sequence is not an output generation.
 */
struct gpu_display_events {
	uint32_t version;
	uint32_t size;
	uint32_t flags;
	uint32_t events;
	uint64_t sequence;
	uint64_t ack_sequence;
	uint64_t reserved;
};

/*
 * One output snapshot selected by ordinal, with a stable nonzero display ID.
 * Only version, size and index are inputs; count-only queries use UINT32_MAX.
 * With GPU_CAP_DISPLAY_CONTROL, POWER_CONTROL says GPU_DISPLAY_POWER works on
 * the output, POWERED_OFF that its lease holder powered it off, and
 * REFRESH_COUNTER that GPU_DISPLAY_REFRESH reports its refresh boundaries.
 * LIMITED marks a connected output that cannot be lit now because of the
 * limit of outputs shown at once (its claim answers ENOSPC).  HAS_CURSOR
 * says the output has a cursor plane: GPU_DISPLAY_CURSOR_SET, MOVE and HIDE
 * work on its primary lease.
 */
struct gpu_display_info {
	uint32_t version;
	uint32_t size;
	uint32_t index;
	uint32_t count;
	uint32_t display_id;
	uint32_t flags;
	uint32_t plane_count;
	uint32_t formats;
	uint64_t generation;
	uint64_t max_frame_bytes;
	uint32_t current_width;
	uint32_t current_height;
	uint32_t preferred_width;
	uint32_t preferred_height;
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t refresh_millihz;
	uint32_t reserved;
	char name[64];
};

/*
 * One native mode query or side-effect-free custom mode validation.
 * Enumeration takes index; validation takes width, height and refresh_millihz.
 * Validation refresh zero asks the driver to return its preferred supported
 * frequency for those dimensions without selecting an active scanout mode.
 */
struct gpu_display_mode {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t operation;
	uint64_t generation;
	uint32_t index;
	uint32_t count;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t flags;
};

/*
 * One exclusive plane reservation owned by this open file description.
 * The returned lease is nonzero, cannot transfer between opens and is input0.
 */
struct gpu_display_claim {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t plane_index;
	uint64_t generation;
	uint64_t lease;
};

/*
 * One explicit reservation release, independent of unrelated GPU resources.
 * Success consumes the lease after scanout and private buffers are retired.
 */
struct gpu_display_release {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
};

/*
 * One whole-frame FIFO presentation from session-owned packed pixel storage.
 * Sequence is input0 and reports completed virtual presentation on success.
 * Ordinary storage is no longer borrowed when this call returns. BLOB mode
 * instead retains the shared allocation until a later frame replaces it or
 * the lease is released; the caller must finish rendering before presenting.
 */
struct gpu_display_present {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	uint64_t handle;
	uint64_t offset;
	uint64_t frame;
	uint64_t sequence;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint32_t refresh_millihz;
	uint32_t flags;
	uint64_t generation;
};

/*
 * One completed-presentation observation scoped to an owning display lease.
 * Input lease, sequence and timeout_ns select the observation; other outputs0.
 *
 * Waits until the commit or present numbered sequence (the output's count,
 * GPU_DISPLAY_COMMIT) has been adopted, at most timeout_ns (one call waits
 * at most a second; zero answers at once).  completed_sequence is the
 * highest number adopted with every lower one; images that only lower
 * records named and the adopted list no longer names are no longer borrowed
 * when it returns.  EAGAIN when timeout_ns is zero and the sequence is not
 * yet adopted, ETIMEDOUT when the time ran out, EINTR on a signal, EBUSY
 * when the lease is not this open's (also when it ends during the wait),
 * ENXIO when disconnected.  An output whose presents return only once
 * adopted answers at once.  These are the framework's terms; the drivers
 * still differ (the reference, gpu-display and gpu-drivers, says which):
 * only bcm2711 and ghci cap a call at a second, only bcm2711 ends the wait
 * on a signal, and venus refuses another open's lease with EPERM, i915
 * with EINVAL.
 */
struct gpu_display_wait {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	uint64_t sequence;
	uint64_t timeout_ns;
	uint64_t completed_sequence;
	uint64_t present_time_ns;
	uint64_t generation;
};

/*
 * Powers the display this open leases on or off (VK_EXT_display_control).
 * Every field is an input; reserved is zero.  It needs a writable open and
 * this open's lease on the display: EBUSY otherwise (another open's lease, or
 * none).  ESTALE for an old generation, ENXIO when disconnected, EOPNOTSUPP
 * when the output has no power control.  SUSPEND may be the same as OFF.
 * The lease's end powers the output on again.  A power change is not a
 * topology change.
 */
struct gpu_display_power {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t state;
	uint64_t generation;
	uint64_t reserved;
};

/*
 * Waits for the output's next refresh boundary after cursor; no lease is
 * needed.  Cursor zero reports the current count at once.  Inputs are
 * display_id, generation, cursor and timeout_ns (one call waits at most a
 * second); the outputs are zero on input.  sequence is a count that never
 * goes back and is above cursor, time_ns the boundary's time on the 1 ms
 * monotonic scheduler clock, and flags has GPU_DISPLAY_VIRTUAL_CLOCK when the
 * boundary is a virtual clock's rather than a scanout's.  An output that does
 * not scan out makes no boundary: ETIMEDOUT.  ESTALE for an old generation,
 * ENODEV when the GPU is offline.
 */
struct gpu_display_refresh {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t flags;
	uint64_t generation;
	uint64_t cursor;
	uint64_t timeout_ns;
	uint64_t sequence;
	uint64_t time_ns;
};

/*
 * Sets the cursor image of the output this open leases (its primary plane's
 * lease), copied at once from session storage: width and height 1 to
 * GPU_DISPLAY_CURSOR_MAX, stride at least width * 4 and a multiple of 4,
 * format GPU_PIXEL_BGRA8888 or GPU_PIXEL_RGBA8888 with premultiplied alpha.
 * The storage is not borrowed after the call; rows set before from storage
 * that has not changed since may be shown again without a copy, and storage
 * a process maps is always copied.  A shown cursor shows the new
 * image from the next refresh boundary; a hidden one stays hidden until
 * GPU_DISPLAY_CURSOR_MOVE.  Every field is an input; reserved fields are zero.
 * It needs a writable open: EBUSY when the lease is not this open's, ESTALE
 * for an old generation, ENXIO when disconnected, EOPNOTSUPP when the
 * driver has no cursor operation (the request is not checked against the
 * output's GPU_DISPLAY_HAS_CURSOR).  EBUSY also comes, for a moment, when every
 * cursor image is named by a display list the hardware has not yet left and
 * none holds the same rows; nothing changed, and the call may succeed a
 * refresh later.
 */
struct gpu_display_cursor_image {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	uint64_t handle;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t generation;
	uint64_t reserved;
};

/*
 * Shows the cursor image with its top-left corner at (x, y) in output pixels
 * (the caller subtracts the image's hotspot); parts outside the output are
 * clipped.  The change takes effect at the next refresh boundary and the call
 * does not wait for it; a later move before that boundary replaces it.  The
 * lease must have an image (EINVAL otherwise).  Errors as for CURSOR_SET.
 */
struct gpu_display_cursor_move {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	int32_t x;
	int32_t y;
	uint64_t generation;
	uint64_t reserved;
};

/*
 * Hides the cursor from the next refresh boundary; its image is kept.  The
 * lease's release and a device fault hide it too.  Errors as for CURSOR_SET.
 */
struct gpu_display_cursor_hide {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	uint64_t generation;
	uint64_t reserved;
};

/*
 * One plane's new state in a commit.  plane is 0 (the primary) or an
 * overlay.  The image is a blob or a scanout allocation of this open
 * (ordinary storage: EINVAL), GPU_PIXEL_BGRA8888 or RGBA8888; an
 * overlay's has premultiplied alpha.  Cached storage is
 * cleaned when a commit names it as a new image, so the caller writes no
 * image while it is borrowed.  src (inside the image) is shown at dst (inside the
 * output); an overlay may scale within its GPU_DISPLAY_OVERLAY_INFO limits,
 * the primary is the whole output, unscaled.  SHOWN without IMAGE keeps the
 * plane's image (EINVAL when it has none).  serial increases for each
 * state of the plane; a state with a serial at or below the plane's last
 * recorded one is not recorded and reports DROPPED.  wait_fd (-1 for none)
 * with wait_generation is a fence the image's writes signal; the commit
 * waits for it before the state is recorded.  The image is borrowed until
 * a list without it is adopted.
 */
struct gpu_display_plane_state {
	uint32_t plane;
	uint32_t flags;
	uint64_t serial;
	uint64_t handle;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_width;
	uint32_t src_height;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_width;
	uint32_t dst_height;
	int32_t wait_fd;
	uint32_t result;
	uint64_t wait_generation;
};

/*
 * Changes some planes of the output this open leases; planes not named keep
 * their state, and a plane named twice is EINVAL.  The call first waits for
 * every named wait_fd, then records the named states at once; every display
 * list made afterwards shows them together, from one refresh boundary.  A
 * state that reports DROPPED is not recorded while the others of the same
 * call are: the one exception to all or nothing.  sequence (output) is the
 * number the record got, from one count per output that never goes back and
 * that GPU_DISPLAY_PRESENT shares (the output's last number when nothing
 * was recorded); adopted (output) is the highest number that is adopted with
 * every lower one (shown by the adopted list, or replaced by a later state it
 * shows).  The call does not wait for the boundary unless COMMIT_WAIT is set
 * (allowed only when the primary is named): then it returns after a list
 * made from this record or a later one is adopted, and signals signal_fd
 * (-1 for none; EINVAL without COMMIT_WAIT) at that adoption.  ENOSPC: the
 * planes would exceed the output's bandwidth or the overlays' limits;
 * nothing changed.  ESTALE for an old generation, EBUSY when the lease is not
 * this open's (also when it ends while the call waits), ENXIO when
 * disconnected.  ETIMEDOUT when the adoption did not come: the output keeps
 * every borrowed image, the lease's later commits and presents fail with
 * EBUSY, and releasing the lease or closing the open lets the images go
 * (the output then shows what the driver leaves: the text console, which
 * the first flip of the system ended, does not draw on it again).
 */
struct gpu_display_commit {
	uint32_t version;
	uint32_t size;
	uint64_t lease;
	uint64_t generation;
	uint32_t flags;
	uint32_t count;
	struct gpu_display_plane_state planes[1 + GPU_DISPLAY_OVERLAY_MAX];
	int32_t signal_fd;
	uint32_t reserved0;
	uint64_t signal_generation;
	uint64_t sequence;
	uint64_t adopted;
	uint64_t reserved;
};

/* One overlay plane's limits on the output's current mode (vkGetDisplayPlaneCapabilitiesKHR); it names the output and needs no lease. */
struct gpu_display_overlay_info {
	uint32_t version;
	uint32_t size;
	uint32_t display_id;
	uint32_t plane;
	uint64_t generation;
	uint32_t max_src_width;
	uint32_t max_src_height;
	uint32_t max_dst_width;
	uint32_t max_dst_height;
	uint32_t flags;
	uint32_t formats;	/* GPU_DISPLAY_FORMAT_* */
	uint64_t reserved;
};

#endif
