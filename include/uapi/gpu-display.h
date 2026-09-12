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
#include <sys/ioctl.h>

#define GPU_CAP_DISPLAY			128U
#define GPU_DISPLAY_CONNECTED		1U
#define GPU_DISPLAY_VIRTUAL_CLOCK	2U
#define GPU_DISPLAY_FIFO		4U
#define GPU_DISPLAY_ATOMIC_MODE_PRESENT	8U
#define GPU_DISPLAY_ACTIVE		16U
#define GPU_DISPLAY_FORMAT_BGRA8888	1U
#define GPU_DISPLAY_FORMAT_RGBA8888	2U
#define GPU_DISPLAY_MODE_ENUMERATE	0U
#define GPU_DISPLAY_MODE_VALIDATE	1U
#define GPU_DISPLAY_PRESENT_FIFO	1U
#define GPU_DISPLAY_COUNT_ONLY		UINT32_MAX

#define GPU_DISPLAY_QUERY		_IOWR('G', 24, struct gpu_display_info)
#define GPU_DISPLAY_MODE		_IOWR('G', 25, struct gpu_display_mode)
#define GPU_DISPLAY_CLAIM		_IOWR('G', 26, struct gpu_display_claim)
#define GPU_DISPLAY_RELEASE		_IOW('G', 27, struct gpu_display_release)
#define GPU_DISPLAY_PRESENT		_IOWR('G', 28, struct gpu_display_present)
#define GPU_DISPLAY_WAIT		_IOWR('G', 29, struct gpu_display_wait)

/*
 * One output snapshot selected by ordinal, with a stable nonzero display ID.
 * Only version, size and index are inputs; count-only queries use UINT32_MAX.
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
 * The backend finishes reading the source storage before this call returns.
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

#endif
