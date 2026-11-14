/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virtual display interface, /dev/vgpu.
 *
 * A program plugs a virtual monitor into one connector of the virtual
 * display node (a /dev/gpuN whose only role is display), receives each
 * frame that a compositor presents to it as a GPU image capability, and
 * gives the frame back when it has read it.  Closing the descriptor
 * unplugs the monitor.  The frames stay in the memory of the GPU that
 * rendered them; the program imports the capability into its own open of
 * that GPU and reads the image there.
 *
 * Every top-level structure begins with version and size and has the
 * same layout on ILP32 and LP64.  On failure no output field is written.
 */

#ifndef KERN_UAPI_VGPU_H
#define KERN_UAPI_VGPU_H

#include <stdint.h>
#include <uapi/gpu.h>
#include <uapi/ioctl.h>

#define VGPU_ABI_VERSION		1U

/* Value of connector in VGPU_CREATE: any free connector. */
#define VGPU_CONNECTOR_ANY		UINT32_MAX

/* The sizes and refresh rates a virtual monitor may have. */
#define VGPU_SIZE_MIN			64U
#define VGPU_SIZE_MAX			4096U
#define VGPU_REFRESH_MIN		1000U
#define VGPU_REFRESH_MAX		240000U
#define VGPU_REFRESH_DEFAULT		60000U

/* The longest a held frame keeps the output from taking the next one. */
#define VGPU_HOLD_MAX_NS		8000000U

/* Bits of state: what the compositor does with the monitor now. */
#define VGPU_STATE_CONNECTED		1U	/* this open has a monitor plugged in */
#define VGPU_STATE_LEASED		2U	/* a compositor holds the output's lease */
#define VGPU_STATE_POWERED_OFF		4U	/* the lease holder turned the output off; no frames are delivered */

/* Bits of flags in VGPU_FRAME_ACQUIRE. */
#define VGPU_HANDLE_CLOEXEC		GPU_HANDLE_CLOEXEC
#define VGPU_HANDLE_CLOFORK		GPU_HANDLE_CLOFORK
#define VGPU_FRAME_FD			4U	/* return a new descriptor even for an image already received */

/* Bit of result in VGPU_FRAME_RELEASE. */
#define VGPU_RELEASE_REVOKED		1U	/* the hold ran past its limit: the image may have changed while it was read */

#define VGPU_INFO			_IOWR('V', 0, struct vgpu_info)
#define VGPU_CREATE			_IOWR('V', 1, struct vgpu_create)
#define VGPU_DESTROY			_IOW('V', 2, struct vgpu_destroy)
#define VGPU_RESIZE			_IOWR('V', 3, struct vgpu_resize)
#define VGPU_STATE			_IOWR('V', 4, struct vgpu_state)
#define VGPU_FRAME_ACQUIRE		_IOWR('V', 5, struct vgpu_frame)
#define VGPU_FRAME_RELEASE		_IOWR('V', 6, struct vgpu_frame_release)

/*
 * What the virtual display node offers: its identity, so that a program
 * can tell it apart from other GPU nodes, and its connectors.
 * Only version and size are inputs.
 */
struct vgpu_info {
	uint32_t version;
	uint32_t size;
	uint64_t device_id;		/* GPU_DEVICE_QUERY identity of the display node; 0 before it is registered */
	uint32_t node;			/* N of /dev/gpuN, for people; programs compare device_id */
	uint32_t connectors;		/* connectors of the node */
	uint32_t free;			/* connectors that a create may take now */
	uint32_t size_min;		/* VGPU_SIZE_MIN */
	uint32_t size_max;		/* VGPU_SIZE_MAX */
	uint32_t refresh_min;		/* VGPU_REFRESH_MIN */
	uint32_t refresh_max;		/* VGPU_REFRESH_MAX */
	uint32_t reserved;
	uint64_t hold_max_ns;		/* VGPU_HOLD_MAX_NS */
};

/*
 * One virtual monitor to plug in.  The size and refresh are the monitor's
 * one mode; the physical size (zero when unknown) lets a compositor scale.
 */
struct vgpu_create {
	uint32_t version;
	uint32_t size;
	uint32_t connector;		/* in/out: a connector number, or VGPU_CONNECTOR_ANY */
	uint32_t flags;			/* zero */
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;	/* zero selects VGPU_REFRESH_DEFAULT */
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	uint32_t display_id;		/* out: the output's display_id on the node */
	uint64_t generation;		/* out: the output's generation */
	uint64_t device_id;		/* out: the display node's identity */
};

/*
 * Unplugs this open's monitor, as closing the descriptor does.
 */
struct vgpu_destroy {
	uint32_t version;
	uint32_t size;
	uint64_t reserved;
};

/*
 * Replaces the mode of this open's monitor.  The compositor sees a new
 * monitor on the same connector (a new generation).
 */
struct vgpu_resize {
	uint32_t version;
	uint32_t size;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	uint32_t reserved;
	uint64_t generation;		/* out */
};

/*
 * A snapshot of this open's monitor; it never waits.
 */
struct vgpu_state {
	uint32_t version;
	uint32_t size;
	uint32_t state;			/* out: VGPU_STATE_* */
	uint32_t connector;		/* out */
	uint64_t generation;		/* out */
	uint64_t adopted;		/* out: newest frame the output took */
	uint64_t delivered;		/* out: newest frame this open received */
	uint64_t image_epoch;		/* out: advances when received images are no longer valid */
};

/*
 * One frame received and held.  fd is -1 on input; on output it is a new
 * descriptor of the image's capability when this open has not received
 * image_id since image_epoch last changed (or VGPU_FRAME_FD asked for
 * one), and -1 otherwise.  image is the description the exporter stored.
 */
struct vgpu_frame {
	uint32_t version;
	uint32_t size;
	uint64_t timeout_ns;		/* in: 0 observes; at most one second is waited */
	uint32_t flags;			/* in: VGPU_HANDLE_* and VGPU_FRAME_FD */
	int32_t fd;			/* in: -1; out */
	uint64_t sequence;		/* out: the frame's number on the output */
	uint64_t skipped;		/* out: frames taken since the last receive and never received */
	uint64_t present_time_ns;	/* out: when the output took the frame */
	uint64_t image_id;		/* out: names the image while image_epoch stays */
	uint64_t image_epoch;		/* out */
	uint32_t state;			/* out: VGPU_STATE_* */
	uint32_t reserved;
	struct gpu_image_descriptor image;	/* out; zero on input */
};

/*
 * Gives a held frame back, so that the output may take the next one.
 * After a revoke the release of that sequence is still owed: until it is
 * made, VGPU_FRAME_ACQUIRE fails with EBUSY.  image_id is never reused
 * while the kernel runs; a program that ignores an epoch change keeps the
 * old images' memory alive through its own imports.
 */
struct vgpu_frame_release {
	uint32_t version;
	uint32_t size;
	uint64_t sequence;		/* in: the held frame */
	uint32_t result;		/* out: VGPU_RELEASE_* */
	uint32_t reserved;
};

#endif
