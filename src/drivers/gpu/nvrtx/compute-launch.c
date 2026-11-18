/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of the first compute launch that touch no hardware (see
 * nvrtx-compute.h).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The bit positions of the QMD fields at the same place in versions 2.2 and
 * 3.0.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc3c0qmd.h (NVC3C0_QMDV02_02_*) and
 * NVIDIA open-gpu-doc classes/compute/clc6c0qmd.h and clc9c0qmd.h
 * (NVC6C0_QMDV03_00_*, NVC9C0_QMDV03_00_*).
 */
#define QMD_SM_GLOBAL_CACHING		134U
#define QMD_API_CALL_LIMIT		378U
#define QMD_API_CALL_LIMIT_NO_CHECK	1U
#define QMD_RASTER_WIDTH_LOW		384U
#define QMD_RASTER_HEIGHT_LOW		416U
#define QMD_RASTER_DEPTH_LOW		448U
#define QMD_SHARED_SIZE_LOW		544U
#define QMD_SHARED_SIZE_BITS		18U
#define QMD_VERSION_LOW			576U
#define QMD_MAJOR_VERSION_LOW		580U
#define QMD_THREADS0_LOW		592U
#define QMD_THREADS1_LOW		608U
#define QMD_THREADS2_LOW		624U
#define QMD_CBUF_VALID_LOW		640U
#define QMD_REGISTER_COUNT_LOW		648U
#define QMD_REGISTER_COUNT_BITS		9U
#define QMD_BARRIER_COUNT_BITS		5U
#define QMD_CBUF_ADDRESS_LOW_LOW	1024U
#define QMD_CBUF_ADDRESS_HIGH_LOW	1056U
#define QMD_CBUF_ADDRESS_HIGH_BITS	17U
#define QMD_CBUF_SIZE_LOW		1075U
#define QMD_CBUF_SIZE_BITS		13U
#define QMD_CBUF_SIZE_SHIFT		4U
#define QMD_CBUF_STRIDE			64U
#define QMD_PROGRAM_LOW_LOW		1536U
#define QMD_PROGRAM_HIGH_LOW		1568U
#define QMD_PROGRAM_HIGH_BITS		17U

/*
 * The fields that moved between QMD version 2.2 (Turing) and 3.0 (Ampere
 * and Ada): the shared-memory configurations, one bit narrower in 3.0, and
 * the barrier count; and the version each names.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc3c0qmd.h
 * (NVC3C0_QMDV02_02_MIN_SM_CONFIG_SHARED_MEM_SIZE,
 * _MAX_SM_CONFIG_SHARED_MEM_SIZE, _TARGET_SM_CONFIG_SHARED_MEM_SIZE,
 * _BARRIER_COUNT) and NVIDIA open-gpu-doc classes/compute/clc6c0qmd.h
 * (the same fields of NVC6C0_QMDV03_00), with src/nouveau/compiler/nak/qmd.rs
 * (the version numbers each QMD writes).
 */
#define QMD22_MINOR			2U
#define QMD22_MAJOR			2U
#define QMD22_MIN_SHARED_CONFIG_LOW	562U
#define QMD22_MAX_SHARED_CONFIG_LOW	569U
#define QMD22_TARGET_SHARED_CONFIG_LOW	657U
#define QMD22_SHARED_CONFIG_BITS	7U
#define QMD22_BARRIER_COUNT_LOW		955U
#define QMD30_MINOR			0U
#define QMD30_MAJOR			3U
#define QMD30_MIN_SHARED_CONFIG_LOW	562U
#define QMD30_MAX_SHARED_CONFIG_LOW	569U
#define QMD30_TARGET_SHARED_CONFIG_LOW	657U
#define QMD30_SHARED_CONFIG_BITS	6U
#define QMD30_BARRIER_COUNT_LOW		763U

/*
 * Turing's shared memory carve-outs in KiB, and the warps an SM holds (the
 * shared configuration's inputs).
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/winsys/nouveau_device.c
 * (init_shared_mem_sizes, max_warps_per_mp_for_sm) and
 * src/nouveau/compiler/nak/qmd.rs (gv100_get_hw_smem_sizes).
 */
#define QMD_SHARED_SMALL_KIB		32U
#define QMD_SHARED_LARGE_KIB		64U
#define QMD_WARPS_PER_SM		32U
#define QMD_THREADS_PER_WARP		32U

/*
 * The compute class's methods (TURING_COMPUTE_A) and the fields of the
 * ones that take flags.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/headers/nvidia/classes/clc5c0.h
 * (NVC5C0_*).
 */
#define COMPUTE_SET_OBJECT		0x0000U
#define COMPUTE_INVALIDATE_SHADER_CACHES	0x021cU
#define COMPUTE_INVALIDATE_INSTRUCTION	(1U << 0)
#define COMPUTE_INVALIDATE_DATA		(1U << 4)
#define COMPUTE_INVALIDATE_CONSTANT	(1U << 12)
#define COMPUTE_SHARED_WINDOW_A		0x02a0U
#define COMPUTE_SEND_PCAS_A		0x02b4U
#define COMPUTE_SEND_SIGNALING_PCAS_B	0x02bcU
#define COMPUTE_PCAS_INVALIDATE		(1U << 0)
#define COMPUTE_PCAS_SCHEDULE		(1U << 1)

/*
 * The launch method of the compute classes after Turing's, and its
 * invalidate-copy-and-schedule action.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/compute/clc6c0.h
 * (NVC6C0_SEND_SIGNALING_PCAS2_B, _PCAS_ACTION_INVALIDATE_COPY_SCHEDULE) and
 * Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_dispatch.c (nvk_CmdDispatchBase:
 * the method for classes above TURING_COMPUTE_A).
 */
#define COMPUTE_SEND_SIGNALING_PCAS2_B	0x02c0U
#define COMPUTE_PCAS2_INVALIDATE_COPY_SCHEDULE	3U
#define COMPUTE_LOCAL_WINDOW_A		0x07b0U

/*
 * The shared and local memory windows, at the top of the low 4 GiB.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/hw_runner.rs
 * (Runner::run_raw) and src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_push_dispatch_state_init).
 */
#define COMPUTE_SHARED_WINDOW		0xfe000000U
#define COMPUTE_LOCAL_WINDOW		0xff000000U

/*
 * An incrementing method header (the channel class's format).
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/headers/nv_push.h
 * (NVC0_FIFO_PKHDR_SQ).
 */
#define COMPUTE_METHOD_INCREMENT	(1U << 29)

/* Where one QMD version keeps the fields that moved, and the version it names. */
struct qmd_layout {
	uint32_t minor;
	uint32_t major;
	unsigned min_shared_low;
	unsigned max_shared_low;
	unsigned target_shared_low;
	unsigned shared_config_bits;
	unsigned barrier_low;
};

/* QMD version 2.2 (Turing). */
static const struct qmd_layout qmd_layout_22 = {
	QMD22_MINOR,
	QMD22_MAJOR,
	QMD22_MIN_SHARED_CONFIG_LOW,
	QMD22_MAX_SHARED_CONFIG_LOW,
	QMD22_TARGET_SHARED_CONFIG_LOW,
	QMD22_SHARED_CONFIG_BITS,
	QMD22_BARRIER_COUNT_LOW
};

/* QMD version 3.0 (Ampere and Ada). */
static const struct qmd_layout qmd_layout_30 = {
	QMD30_MINOR,
	QMD30_MAJOR,
	QMD30_MIN_SHARED_CONFIG_LOW,
	QMD30_MAX_SHARED_CONFIG_LOW,
	QMD30_TARGET_SHARED_CONFIG_LOW,
	QMD30_SHARED_CONFIG_BITS,
	QMD30_BARRIER_COUNT_LOW
};

/* Turing's launches: QMD 2.2 and its compute class, for the callers that name no generation. */
static const struct nvrtx_compute_kind compute_kind_turing = {
	2U,
	2U,
	NVRTX_CLASS_COMPUTE
};

static void qmd_set(uint32_t *qmd, unsigned low, unsigned bits, uint64_t value);
static uint32_t qmd_shared_config(uint32_t kib);
static uint32_t qmd_pick_shared(uint32_t bytes, uint32_t groups);
static size_t compute_method(uint8_t *push, size_t at, uint32_t method, uint32_t count);

/*
 * Builds a launch's QMD for Turing (version 2.2), as nvrtx_qmd_build_for().
 */
int
nvrtx_qmd_build(
	uint32_t *qmd,
	const struct nvrtx_qmd_info *info)
{
	int error;

	/* Turing's version. */
	error = nvrtx_qmd_build_for(qmd, info, &compute_kind_turing);
	if (error != 0)
		return error;

	/* Succeeded: the QMD is complete. */
	return 0;
}

/*
 * Builds a launch's QMD in the generation's version (2.2 for Turing, 3.0
 * for Ampere and Ada): the version, global caching on, no call limit, the
 * grid and the workgroup, the shared memory and the carve-outs it allows,
 * the registers, the barriers, no local memory, the constant buffers and
 * the program.  The carve-outs are Turing's (32 and 64 KiB), which Ampere's
 * and Ada's SMs also offer.
 *
 * Returns 0, or EINVAL for a value a field cannot hold, a misaligned
 * address or size, or a QMD version this driver does not write.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/qmd.rs
 * (fill_qmd for versions 2.2 and 3.0).
 */
int
nvrtx_qmd_build_for(
	uint32_t *qmd,
	const struct nvrtx_qmd_info *info,
	const struct nvrtx_compute_kind *kind)
{
	const struct qmd_layout *layout;
	uint32_t threads;
	uint32_t warps;
	uint32_t shared;
	uint32_t groups_per_sm;
	uint32_t index;

	/* The version's layout. */
	if (kind->qmd_major == QMD22_MAJOR && kind->qmd_minor == QMD22_MINOR) {
		layout = &qmd_layout_22;
	} else if (kind->qmd_major == QMD30_MAJOR && kind->qmd_minor == QMD30_MINOR) {
		layout = &qmd_layout_30;
	} else {
		/* A version this driver does not write. */
		return EINVAL;
	}

	/* Every field must fit: the program and the buffers on their boundaries. */
	if ((info->program & (NVRTX_QMD_ALIGN - 1U)) != 0U)
		return EINVAL;
	if (info->gpr_count == 0U || info->gpr_count > 255U)
		return EINVAL;
	if (info->local_size[0] == 0U || info->local_size[0] > 0xffffU)
		return EINVAL;
	if (info->local_size[1] == 0U || info->local_size[1] > 0xffffU)
		return EINVAL;
	if (info->local_size[2] == 0U || info->local_size[2] > 0xffffU)
		return EINVAL;
	if (info->groups[1] > 0xffffU || info->groups[2] > 0xffffU)
		return EINVAL;
	if (info->shared_bytes > QMD_SHARED_LARGE_KIB * 1024U || info->barrier_count > 31U)
		return EINVAL;

	/* Starts empty. */
	kern_memset(qmd, 0, NVRTX_QMD_WORDS * sizeof(uint32_t));

	/* The version, global caching, and no check of the call limit. */
	qmd_set(qmd, QMD_VERSION_LOW, 4U, layout->minor);
	qmd_set(qmd, QMD_MAJOR_VERSION_LOW, 4U, layout->major);
	qmd_set(qmd, QMD_SM_GLOBAL_CACHING, 1U, 1U);
	qmd_set(qmd, QMD_API_CALL_LIMIT, 1U, QMD_API_CALL_LIMIT_NO_CHECK);

	/* The grid and the workgroup. */
	qmd_set(qmd, QMD_RASTER_WIDTH_LOW, 32U, info->groups[0]);
	qmd_set(qmd, QMD_RASTER_HEIGHT_LOW, 16U, info->groups[1]);
	qmd_set(qmd, QMD_RASTER_DEPTH_LOW, 16U, info->groups[2]);
	qmd_set(qmd, QMD_THREADS0_LOW, 16U, info->local_size[0]);
	qmd_set(qmd, QMD_THREADS1_LOW, 16U, info->local_size[1]);
	qmd_set(qmd, QMD_THREADS2_LOW, 16U, info->local_size[2]);

	/* The registers and the barriers; the local memory and the call stack stay zero. */
	qmd_set(qmd, QMD_REGISTER_COUNT_LOW, QMD_REGISTER_COUNT_BITS, info->gpr_count);
	qmd_set(qmd, layout->barrier_low, QMD_BARRIER_COUNT_BITS, info->barrier_count);

	/*
	 * The shared memory, in 256-byte units, and the carve-outs: the least
	 * for one workgroup, the target for as many as an SM holds, the most
	 * the SM has (gv100_get_hw_smem_sizes).
	 */
	shared = (info->shared_bytes + 0xffU) & ~0xffU;
	threads = info->local_size[0] * info->local_size[1] * info->local_size[2];
	warps = (threads + QMD_THREADS_PER_WARP - 1U) / QMD_THREADS_PER_WARP;
	groups_per_sm = QMD_WARPS_PER_SM / warps;
	if (groups_per_sm == 0U)
		groups_per_sm = 1U;
	qmd_set(qmd, QMD_SHARED_SIZE_LOW, QMD_SHARED_SIZE_BITS, shared);
	qmd_set(qmd, layout->min_shared_low, layout->shared_config_bits, qmd_shared_config(qmd_pick_shared(shared, 1U)));
	qmd_set(qmd, layout->target_shared_low, layout->shared_config_bits, qmd_shared_config(qmd_pick_shared(shared, groups_per_sm)));
	qmd_set(qmd, layout->max_shared_low, layout->shared_config_bits, qmd_shared_config(QMD_SHARED_LARGE_KIB));

	/* Each bound constant buffer: its address, its size in 16-byte units, and its valid bit. */
	for (index = 0U; index < NVRTX_QMD_CBUFS; index++) {
		if (info->cbuf_size[index] == 0U)
			continue;
		if ((info->cbuf_address[index] & (NVRTX_QMD_CBUF_ALIGN - 1U)) != 0U)
			return EINVAL;
		if ((info->cbuf_size[index] & (NVRTX_QMD_CBUF_ALIGN - 1U)) != 0U)
			return EINVAL;
		if ((info->cbuf_size[index] >> QMD_CBUF_SIZE_SHIFT) >= (1U << QMD_CBUF_SIZE_BITS))
			return EINVAL;
		qmd_set(qmd, QMD_CBUF_ADDRESS_LOW_LOW + index * QMD_CBUF_STRIDE, 32U, info->cbuf_address[index] & 0xffffffffU);
		qmd_set(qmd, QMD_CBUF_ADDRESS_HIGH_LOW + index * QMD_CBUF_STRIDE, QMD_CBUF_ADDRESS_HIGH_BITS, info->cbuf_address[index] >> 32);
		qmd_set(qmd, QMD_CBUF_SIZE_LOW + index * QMD_CBUF_STRIDE, QMD_CBUF_SIZE_BITS, info->cbuf_size[index] >> QMD_CBUF_SIZE_SHIFT);
		qmd_set(qmd, QMD_CBUF_VALID_LOW + index, 1U, 1U);
	}

	/* The program's address. */
	qmd_set(qmd, QMD_PROGRAM_LOW_LOW, 32U, info->program & 0xffffffffU);
	qmd_set(qmd, QMD_PROGRAM_HIGH_LOW, QMD_PROGRAM_HIGH_BITS, info->program >> 32);

	/* Succeeded: the QMD is complete. */
	return 0;
}

/*
 * Writes the push buffer of one launch for Turing, as
 * nvrtx_push_compute_launch_for().
 */
size_t
nvrtx_push_compute_launch(
	uint8_t *push,
	uint64_t qmd_va)
{
	size_t length;

	/* Turing's class and launch method. */
	length = nvrtx_push_compute_launch_for(push, qmd_va, &compute_kind_turing);

	/* Succeeded: the push buffer's length. */
	return length;
}

/*
 * Writes the push buffer of one launch on the compute subchannel: the
 * generation's compute class bound there, the shared and local memory
 * windows, the shader caches invalidated, then the QMD sent and scheduled
 * (SEND_SIGNALING_PCAS_B on Turing, SEND_SIGNALING_PCAS2_B on the classes
 * after it).
 *
 * Returns the length in bytes.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_push_dispatch_state_init, nvk_CmdDispatchBase).
 */
size_t
nvrtx_push_compute_launch_for(
	uint8_t *push,
	uint64_t qmd_va,
	const struct nvrtx_compute_kind *kind)
{
	size_t at;

	/* Binds the generation's compute class to its subchannel. */
	at = compute_method(push, 0U, COMPUTE_SET_OBJECT, 1U);
	nvrtx_put32(push, at, kind->compute_class);
	at += 4U;

	/* The shared and the local memory windows, below 4 GiB. */
	at = compute_method(push, at, COMPUTE_SHARED_WINDOW_A, 2U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, COMPUTE_SHARED_WINDOW);
	at += 8U;
	at = compute_method(push, at, COMPUTE_LOCAL_WINDOW_A, 2U);
	nvrtx_put32(push, at, 0U);
	nvrtx_put32(push, at + 4U, COMPUTE_LOCAL_WINDOW);
	at += 8U;

	/* The instruction, data and constant caches start clean for the new program. */
	at = compute_method(push, at, COMPUTE_INVALIDATE_SHADER_CACHES, 1U);
	nvrtx_put32(push, at, COMPUTE_INVALIDATE_INSTRUCTION | COMPUTE_INVALIDATE_DATA | COMPUTE_INVALIDATE_CONSTANT);
	at += 4U;

	/* The QMD's address in 256-byte units, then the launch. */
	at = compute_method(push, at, COMPUTE_SEND_PCAS_A, 1U);
	nvrtx_put32(push, at, (uint32_t)(qmd_va >> 8));
	at += 4U;
	if (kind->compute_class > NVRTX_CLASS_COMPUTE) {
		/* The classes after Turing's schedule the QMD through the second method. */
		at = compute_method(push, at, COMPUTE_SEND_SIGNALING_PCAS2_B, 1U);
		nvrtx_put32(push, at, COMPUTE_PCAS2_INVALIDATE_COPY_SCHEDULE);
	} else {
		/* Turing's class invalidates and schedules with the first. */
		at = compute_method(push, at, COMPUTE_SEND_SIGNALING_PCAS_B, 1U);
		nvrtx_put32(push, at, COMPUTE_PCAS_INVALIDATE | COMPUTE_PCAS_SCHEDULE);
	}
	at += 4U;

	/* Succeeded: the push buffer's length. */
	return at;
}

/*
 * Builds the IR of the first program, as the frontend would parse
 *
 *   layout(local_size_x = 32) in;
 *   layout(set = 0, binding = 0) buffer O { uint o[]; };
 *   void main() { o[gl_GlobalInvocationID.x] = 42u; }
 *
 * The kernel compiles it without a SPIR-V module.
 */
void
nvrtx_compute_first_ir(
	struct nvrtx_compute_first *first)
{
	struct drv_gpu_shader_ir_inst *list;
	struct drv_gpu_shader_ir *ir;

	kern_memset(first, 0, sizeof(*first));
	list = first->instructions;
	ir = &first->ir;

	/* v0 = the local id, v1 = the group id. */
	list[0].op = DRV_GPU_IR_LOAD_SYSTEM;
	list[0].dst = 0U;
	list[0].component = DRV_GPU_IR_SYSTEM_LOCAL_ID_X;
	list[1].op = DRV_GPU_IR_LOAD_SYSTEM;
	list[1].dst = 1U;
	list[1].component = DRV_GPU_IR_SYSTEM_GROUP_ID_X;

	/* v4 = v1 * 32 + v0, the global id. */
	list[2].op = DRV_GPU_IR_ICONST;
	list[2].dst = 2U;
	list[2].immediate = NVRTX_COMPUTE_FIRST_WIDTH;
	list[3].op = DRV_GPU_IR_IMUL;
	list[3].dst = 3U;
	list[3].src[0] = 1U;
	list[3].src[1] = 2U;
	list[4].op = DRV_GPU_IR_IADD;
	list[4].dst = 4U;
	list[4].src[0] = 3U;
	list[4].src[1] = 0U;

	/* v6 = v4 * 4, the byte offset of its word. */
	list[5].op = DRV_GPU_IR_ICONST;
	list[5].dst = 5U;
	list[5].immediate = 4U;
	list[6].op = DRV_GPU_IR_IMUL;
	list[6].dst = 6U;
	list[6].src[0] = 4U;
	list[6].src[1] = 5U;

	/* v7 = 42, stored there. */
	list[7].op = DRV_GPU_IR_ICONST;
	list[7].dst = 7U;
	list[7].immediate = NVRTX_COMPUTE_FIRST_VALUE;
	list[8].op = DRV_GPU_IR_STORE_STORAGE;
	list[8].location = 0U;
	list[8].src[0] = 6U;
	list[8].src[1] = 7U;

	/* The one storage buffer at (0, 0). */
	first->uniform.set = 0U;
	first->uniform.binding = 0U;
	first->uniform.kind = DRV_GPU_IR_UNIFORM_STORAGE;

	/* The program. */
	ir->stage = DRV_GPU_STAGE_COMPUTE;
	ir->uniforms = &first->uniform;
	ir->uniform_count = 1U;
	ir->instructions = list;
	ir->instruction_count = NVRTX_COMPUTE_FIRST_INSTRUCTIONS;
	ir->value_count = NVRTX_COMPUTE_FIRST_VALUES;
	ir->local_size[0] = NVRTX_COMPUTE_FIRST_WIDTH;
	ir->local_size[1] = 1U;
	ir->local_size[2] = 1U;
}

/* Sets a field of up to 64 bits of the QMD at a bit position. */
static void
qmd_set(
	uint32_t *qmd,
	unsigned low,
	unsigned bits,
	uint64_t value)
{
	unsigned bit;
	uint32_t mask;

	/* Each bit of the field into its word. */
	for (bit = 0U; bit < bits; bit++) {
		mask = 1U << ((low + bit) % 32U);

		/* Sets or clears the bit the value has there. */
		if (((value >> bit) & 1U) != 0U) {
			qmd[(low + bit) / 32U] |= mask;
		} else {
			qmd[(low + bit) / 32U] &= ~mask;
		}
	}
}

/*
 * Reports the QMD's code of a shared memory carve-out in KiB.
 *
 * Values referenced: Mesa 25.3.6 src/nouveau/compiler/nak/qmd.rs
 * (gv100_smem_size_to_hw).
 */
static uint32_t
qmd_shared_config(
	uint32_t kib)
{
	/* One more than the quarter. */
	return kib / 4U + 1U;
}

/*
 * Picks the smallest carve-out that holds as many workgroups of the
 * shared memory as asked, as many as the largest one allows (Turing's 32
 * and 64 KiB).
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/compiler/nak/qmd.rs
 * (gv100_pick_smem_size_kb).
 */
static uint32_t
qmd_pick_shared(
	uint32_t bytes,
	uint32_t groups)
{
	uint32_t fitting;
	uint32_t per_group;

	/* How many workgroups the largest carve-out holds. */
	per_group = bytes;
	if (per_group == 0U)
		per_group = 1U;
	fitting = QMD_SHARED_LARGE_KIB * 1024U / per_group;
	if (groups > fitting)
		groups = fitting;

	/* The smaller carve-out when it holds them. */
	if ((uint64_t)QMD_SHARED_SMALL_KIB * 1024U >= (uint64_t)bytes * groups)
		return QMD_SHARED_SMALL_KIB;

	/* Succeeded: the larger one. */
	return QMD_SHARED_LARGE_KIB;
}

/* Writes an incrementing method header on the compute subchannel and returns where its data goes. */
static size_t
compute_method(
	uint8_t *push,
	size_t at,
	uint32_t method,
	uint32_t count)
{
	/* The header: incrementing, the count, the subchannel, the method's word address. */
	nvrtx_put32(push, at, COMPUTE_METHOD_INCREMENT | (count << 16) | (NVRTX_COMPUTE_SUBCHANNEL << 13) | (method >> 2));

	/* Succeeded: the data follows the header. */
	return at + 4U;
}
