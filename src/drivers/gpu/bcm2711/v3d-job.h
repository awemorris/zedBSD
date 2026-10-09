/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Trusted native jobs are built by the kernel executor, never supplied as UAPI. */
#ifndef KERN_DRIVERS_GPU_BCM2711_V3D_JOB_H
#define KERN_DRIVERS_GPU_BCM2711_V3D_JOB_H

#include <stdbool.h>
#include <stdint.h>

struct bcm2711_v3d;

/* Only one serialized worker can occupy any of the native queues at a time. */
enum bcm2711_v3d_job_kind {
	BCM2711_V3D_JOB_CL,
	BCM2711_V3D_JOB_TFU,
	BCM2711_V3D_JOB_COMPUTE
};

/* Preallocated, already mapped overflow storage is retained through rendering. */
struct bcm2711_v3d_overflow {
	uint32_t address;
	uint32_t bytes;
};

/* Bin must finish before render launches; both lists borrow their caller's mappings. */
struct bcm2711_v3d_cl_job {
	uint32_t bin_start;
	uint32_t bin_end;
	uint32_t render_start;
	uint32_t render_end;
	uint32_t pool_address;
	uint32_t pool_bytes;
	uint32_t state_address;
	uint32_t state_bytes;
	struct bcm2711_v3d_overflow overflow[4];
	uint32_t overflow_count;
	bool clean_output;
};

/* TFU input/output spans and register fields come from the private image lowering. */
struct bcm2711_v3d_tfu_job {
	uint32_t input;
	uint32_t input_bytes;
	uint32_t output;
	uint32_t output_bytes;
	uint32_t output_format;
	uint32_t stride;
	uint32_t size;
	uint32_t configuration;
	uint32_t coefficients[4];
};

/* Direct CSD configuration borrows kernel-compiled QPU code and uniform storage. */
struct bcm2711_v3d_compute_job {
	uint32_t configuration[7];
	uint32_t shader_bytes;
	uint32_t uniform_bytes;
};

/*
 * Job fields stay immutable while the sole worker executes them.
 * The caller cleans input bytes, retains every referenced allocation and VA,
 * and invalidates GPU outputs before CPU reads.  An unsuccessful launched
 * job keeps all those owners quarantined until a proved global native reset.
 */
struct bcm2711_v3d_job {
	enum bcm2711_v3d_job_kind kind;
	union {
		struct bcm2711_v3d_cl_job cl;
		struct bcm2711_v3d_tfu_job tfu;
		struct bcm2711_v3d_compute_job compute;
	} command;
};

/* Retirement is true only for no launch or confirmed completion plus required clean. */
struct bcm2711_v3d_job_result {
	bool retired;
	uint32_t core_events;
	uint32_t hub_events;
	uint32_t overflow_used;
	uint32_t bin_counter;
	uint32_t render_counter;
	uint32_t fault_client;
	uint32_t fault_address;
};

int bcm2711_v3d_job_run(struct bcm2711_v3d *engine, const struct bcm2711_v3d_job *job, struct bcm2711_v3d_job_result *result);

#endif
