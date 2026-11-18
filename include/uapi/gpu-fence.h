/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Reference-bearing GPU fence descriptors and explicit submission dependencies.
 */

#ifndef KERN_UAPI_GPU_FENCE_H
#define KERN_UAPI_GPU_FENCE_H

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#define GPU_CAP_FENCE			2048U

#define GPU_FENCE_PENDING		0U
#define GPU_FENCE_SIGNALED		1U
#define GPU_FENCE_ERROR			2U
#define GPU_FENCE_BIND_RELEASE		1U

#define GPU_FENCE_CREATE		_IOWR('G', 16, struct gpu_fence_create)
#define GPU_FENCE_QUERY			_IOWR('G', 17, struct gpu_fence_state)
#define GPU_FENCE_WAIT			_IOWR('G', 18, struct gpu_fence_state)
#define GPU_FENCE_RESET			_IOWR('G', 19, struct gpu_fence_state)
#define GPU_FENCE_SIGNAL		_IOWR('G', 20, struct gpu_fence_state)
#define GPU_FENCE_BIND			_IOW('G', 21, struct gpu_fence_bind)
#define GPU_COMMAND_SUBMIT_SYNC		_IOWR('G', 22, struct gpu_command_submit_sync)
#define GPU_DISPLAY_PRESENT_SYNC	_IOWR('G', 23, struct gpu_display_present_sync)

/*
 * One reference-bearing fence starts at generation one, optionally signaled.
 * Flags use GPU_HANDLE_CLOEXEC/CLOFORK; input fd is -1 and generation is zero.
 */
struct gpu_fence_create {
	uint32_t version;
	uint32_t size;
	uint32_t flags;
	int32_t fd;
	uint32_t signaled;
	uint32_t reserved;
	uint64_t generation;
};

/*
 * One exact payload generation is queried, waited, reset or signaled.
 * QUERY accepts generation zero and returns the current generation and state.
 * Other operations require an exact generation; reset returns its successor.
 * SIGNAL uses error zero only after authoritative work-success observation.
 */
struct gpu_fence_state {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t generation;
	uint64_t timeout_ns;
	uint32_t state;
	int32_t error;
};

/*
 * One GPU open owns pending work until verified signal, rollback, or final close.
 * Sequence zero reserves ownership before native submission. A nonzero sequence
 * additionally associates terminal transport failure with the pending payload.
 * RELEASE undoes a reservation without changing the unsignaled payload state.
 */
struct gpu_fence_bind {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t generation;
	uint64_t sequence;
};

/*
 * One command can wait before queue publication and retain a later signal target.
 * Each absent dependency uses fd -1 and generation zero. The nested command size
 * remains sizeof(gpu_command_submit); its sequence reports accepted work.
 * Queue notification never substitutes for verified GPU_FENCE_SIGNAL success.
 */
struct gpu_command_submit_sync {
	struct gpu_command_submit command;
	int32_t wait_fd;
	int32_t signal_fd;
	uint64_t wait_generation;
	uint64_t signal_generation;
};

/*
 * One synchronous scanout waits before hardware access and signals after selection.
 * Its nested present retains the ordinary ABI and completed-sequence semantics.
 * An absent wait or signal uses fd -1 with generation zero.
 */
struct gpu_display_present_sync {
	struct gpu_display_present present;
	int32_t wait_fd;
	int32_t signal_fd;
	uint64_t wait_generation;
	uint64_t signal_generation;
};

#endif
