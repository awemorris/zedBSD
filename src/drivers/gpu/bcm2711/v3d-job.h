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

/*
 * TFU input/output spans and register fields come from the private image lowering.
 *
 * chroma and u_plane are the second and third input planes of a YUV input;
 * zero leaves the register zero, as a packed or single-plane input has it.
 *
 * On a V3D 7.1 (the Raspberry Pi 5) the unit's fields moved: configuration
 * carries the 7.1 input configuration word (texture type at bit 16, input
 * format at bit 23) and output_format the whole 7.1 output control word
 * (output format at bit 12, mipmap count at bit 4, output stride at bit 16),
 * which 4.2 does not have; the 7.1 job runner writes them as given.
 */
struct bcm2711_v3d_tfu_job {
	uint32_t input;
	uint32_t input_bytes;
	uint32_t chroma;
	uint32_t chroma_bytes;
	uint32_t u_plane;
	uint32_t u_plane_bytes;
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
/*
 * The performance counters each render run reports, in this order: cycles,
 * QPU idle, fragment-shading, valid-instruction, TMU-wait, scoreboard-wait,
 * varyings-wait and fragment-stall cycles, TMU accesses and misses, TMU
 * stall cycles, render-thread cycles, valid quads, quads written, and TLB
 * words read and written.
 */
#define BCM2711_V3D_COUNTERS 16U

struct bcm2711_v3d_job_result {
	bool retired;
	uint32_t core_events;
	uint32_t hub_events;
	uint32_t overflow_used;
	uint32_t bin_counter;
	uint32_t render_counter;
	uint32_t fault_client;
	uint32_t fault_address;
	/* Diagnostic wall times of the bin and render queues and the output cache clean. */
	uint64_t bin_ticks;
	uint64_t render_ticks;
	uint64_t clean_ticks;
	/* Diagnostic wall time of the texture formatting jobs that preceded a CL job, their number and the longest one. */
	uint64_t tfu_ticks;
	uint32_t tfu_jobs;
	uint64_t tfu_max_ticks;
	/* The performance counters' increase over the render queue's run (BCM2711_V3D_COUNTER_* order). */
	uint32_t counters[BCM2711_V3D_COUNTERS];
	/* The same counters' increase over the bin queue's run, when there was one. */
	uint32_t bin_counters[BCM2711_V3D_COUNTERS];
};

int bcm2711_v3d_job_run(struct bcm2711_v3d *engine, const struct bcm2711_v3d_job *job, struct bcm2711_v3d_job_result *result);

#endif
