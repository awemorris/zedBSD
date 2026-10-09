/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Recorded dispatches on the GPU (see compute.h, ws101-p004).
 *
 * A dispatch is laid out as the compute test of tests/execution/eu-test.c,
 * which ran on the Latitude 5330, and as anv programs Gen12.0
 * (genX_cmd_compute.c, genX_pipeline.c): in 3D the context setup programs
 * the state bases at the dispatch's slot and the kernel's instruction
 * window (Wa_1607854226 wants them programmed in 3D); a stalling flush of
 * the render target, depth and HDC caches switches to GPGPU; MEDIA_VFE_STATE
 * sets the thread limit, the URB and the CURBE room and the scratch space;
 * the CURBE and the interface descriptor are loaded from the slot's dynamic
 * heap; GPGPU_WALKER runs the groups; a stalling flush of the HDC and data
 * caches returns to 3D, so the next operation finds the pipeline as a draw
 * leaves it.
 *
 * The slot's dynamic heap holds, at the offsets of heap.h, the interface
 * descriptor, the three group counts gl_NumWorkGroups reads, and the CURBE:
 * the kernel's cross-thread push data (the push constants, the uniform
 * blocks' ranges and the storage buffers' addresses, from the compute bind
 * point's sets), then the pipeline's table of each thread's IDs.
 *
 * An indirect dispatch (ws101-p007) loads the walker's three counts from the
 * application's buffer with MI_LOAD_REGISTER_MEM when it runs (as anv's
 * compute_load_indirect_params), and gl_NumWorkGroups reads that buffer's
 * three words in place of the slot's.  Counts of zero need nothing of their
 * own on Gen8 and later (Mesa's i965 predicated the walker off on Gen7
 * only).
 */

#include "compute.h"
#include "batch.h"
#include "draw.h"
#include "gfx.h"
#include "heap.h"
#include "internal.h"
#include "state.h"
#include <kern/kcrt.h>

#include "../i915.h"

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "../intel/commands.h"
#include "../intel/genxml.h"

/*
 * PIPE_CONTROL's Stall At Pixel Scoreboard (gen120.xml, bit 33: bit 1 of
 * the flags dword): with the command streamer stall, the stalling flush
 * MEDIA_VFE_STATE wants before it, as the compute test issues it.
 */
#define I915_COMPUTE_STALL_AT_SCOREBOARD	(1U << 1)

/* The bytes of a register of push data. */
#define I915_COMPUTE_REGISTER_BYTES		32U

/* The bytes of a storage buffer's address and range in its push register: low, high, range. */
#define I915_COMPUTE_ADDRESS_WORDS		3U

/* The bytes of the three group counts. */
#define I915_COMPUTE_GROUP_COUNT_BYTES		12U

/* Shared local memory: the smallest a group takes (1 KiB, the encoding 1) and the largest (16 KiB, 5). */
#define I915_COMPUTE_SLM_MIN_BYTES		1024U
#define I915_COMPUTE_SLM_MAX_SIZE		5U

/* Scratch space: the smallest per-thread space and the largest encoding (1 KiB << 11 = 2 MiB). */
#define I915_COMPUTE_SCRATCH_MIN_BYTES		1024U
#define I915_COMPUTE_SCRATCH_MAX_SPACE		11U

static int i915_compute_reads_uniforms(const struct i915_shader_binary *binary);
static int i915_compute_names_storage(const struct i915_shader_binary *binary);
static uint32_t i915_compute_dss_count(const struct i915_render_session *session);
static uint64_t i915_compute_scratch(uint32_t per_thread_bytes, uint64_t offset);
static uint32_t i915_compute_slm_size(uint32_t bytes);

/*
 * Records one dispatch of the bound compute pipeline over the grid's
 * workgroups into the submission's batch, or runs it to its end outside a
 * submission.
 *
 * The caller has checked that a prepared compute pipeline is bound, and
 * that direct counts are nonzero and within the device's limit or that
 * the indirect counts lie in a bound buffer.  Returns
 * ENOMEM when the session's objects cannot be made, EINVAL when the bound
 * sets do not give the kernel its buffers, ENOSPC when the dispatch does
 * not fit a batch, or the error of a GPU run.
 */
int
drv_i915_gfx_dispatch(
	struct i915_render_session *session,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_grid *grid)
{
	struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	struct i915_gfx_session *work;
	struct i915_gfx_op_space space;
	struct i915_gfx_kernels kernels;
	uint32_t mocs;
	uint32_t dss_count;
	int uniforms;
	int storage;
	int error;

	/* The kernel the dispatch runs. */
	pipeline = state->compute_pipeline;
	binary = pipeline->cs_binary;

	/* Takes the session's objects, making them on the first operation. */
	work = drv_i915_gfx_session_get(session);
	if (work == NULL)
		return ENOMEM;

	/* Every buffer and state of a dispatch is uncached, as a draw's. */
	mocs = GEN12_MOCS(I915_MOCS_UNCACHED_INDEX);

	/*
	 * Uniform data is copied into the CURBE by the CPU now, so an operation
	 * recorded before the dispatch, which may have written the buffer, runs
	 * first.
	 */
	uniforms = i915_compute_reads_uniforms(binary);
	if (work->transfer_pending != 0 && uniforms != 0) {
		error = drv_i915_gfx_flush(session, work);
		if (error != 0)
			return error;
	}

	/* Gives a kernel that spills its part of the scratch buffer. */
	kern_memset(&kernels, 0, sizeof(kernels));
	kernels.cs_scratch_bytes = binary->scratch_bytes;
	error = drv_i915_gfx_scratch(session, work, &kernels);
	if (error != 0)
		return error;

	/* Finds the pipeline's instruction window, placing its kernel the first time: the whole window is the kernel's. */
	error = drv_i915_gfx_window(session,
				    work,
				    &pipeline->kernel_owner,
				    &pipeline->kernel_window,
				    &pipeline->kernel_generation,
				    binary->code,
				    binary->code_bytes,
				    NULL,
				    0U,
				    NULL,
				    0U,
				    &space);
	if (error != 0)
		return error;

	/* Takes the dispatch's slot and its room in the batch. */
	error = drv_i915_gfx_op_begin(session, work, &space);
	if (error != 0)
		return error;

	/* Writes the interface descriptor, the group counts and the CURBE, then the dispatch's commands. */
	dss_count = i915_compute_dss_count(session);
	error = drv_i915_gfx_dispatch_write(space.slot, space.slot_va, state, grid);
	if (error == 0)
		drv_i915_gfx_dispatch_build(space.batch, &space, pipeline, &kernels, grid, dss_count, mocs);

	/* Keeps the dispatch in the batch, or takes it back; outside a submission it runs now. */
	error = drv_i915_gfx_op_end(session, work, error);
	if (error != 0) {
		kern_logf("i915: vk: dispatch of %u x %u x %u groups (indirect at 0x%llx) failed: error %d\n",
			  grid->groups[0],
			  grid->groups[1],
			  grid->groups[2],
			  (unsigned long long)grid->indirect_va,
			  error);
		return error;
	}

	/*
	 * The kernel may write its storage buffers: a later operation that
	 * copies uniform data on the CPU must wait for the dispatch to run.
	 */
	storage = i915_compute_names_storage(binary);
	if (storage != 0)
		work->transfer_pending = 1;

	/* Succeeded: the dispatch is recorded, or has run outside a submission. */
	return 0;
}

/*
 * Writes a dispatch's slot: the interface descriptor, the three group
 * counts and the CURBE -- the cross-thread push data, then each thread's
 * IDs.
 *
 * The push data comes from the compute bind point's sets.  The system
 * storage buffer (DRV_GPU_IR_SYSTEM_SET) points at the group counts: the
 * slot's, or an indirect dispatch's in its buffer.  Returns EINVAL when the
 * sets do not give the kernel its buffers or the CURBE does not fit the
 * slot.
 */
int
drv_i915_gfx_dispatch_write(
	uint8_t *slot,
	uint64_t slot_va,
	const struct i915_gfx_draw_state *state,
	const struct i915_gfx_grid *grid)
{
	const struct i915_gfx_pipeline *pipeline;
	const struct i915_shader_binary *binary;
	const struct i915_shader_block *block;
	struct i915_gfx_draw_state view;
	struct i915_gfx_push_layout layout;
	uint32_t address[I915_COMPUTE_ADDRESS_WORDS];
	uint8_t *dynamic;
	uint8_t *curbe;
	uint32_t *descriptor;
	uint32_t curbe_regs;
	uint32_t thread_bytes;
	uint32_t index;
	uint64_t counts_va;
	int error;

	/* The kernel the dispatch runs. */
	pipeline = state->compute_pipeline;
	binary = pipeline->cs_binary;

	/* Refuses a CURBE larger than the slot's room for it. */
	curbe_regs = binary->cross_thread_regs + pipeline->threads * I915_SHADER_PER_THREAD_REGS;
	if (curbe_regs * I915_COMPUTE_REGISTER_BYTES > I915_GFX_CURBE_BYTES)
		return EINVAL;

	/* Clears the whole slot and locates its dynamic heap. */
	kern_memset(slot, 0, I915_GFX_SLOT_BYTES);
	dynamic = slot + I915_GFX_DYNAMIC_HEAP;

	/*
	 * The interface descriptor: the kernel at the start of its window, no
	 * mid-thread preemption (anv), the per-thread registers, the threads of
	 * a group with their shared local memory and barrier (ws101-p006), and
	 * the cross-thread registers.
	 */
	descriptor = (uint32_t *)(void *)(dynamic + I915_GFX_DYN_INTERFACE);
	descriptor[2] = GEN12_IDD_PREEMPTION_DISABLE;
	descriptor[5] = I915_SHADER_PER_THREAD_REGS << GEN12_IDD_PER_THREAD_LENGTH_SHIFT;
	descriptor[6] = pipeline->threads | (i915_compute_slm_size(binary->shared_bytes) << GEN12_IDD_SLM_SIZE_SHIFT);
	if (binary->uses_barrier != 0U)
		descriptor[6] |= GEN12_IDD_BARRIER_ENABLE;
	descriptor[7] = binary->cross_thread_regs;

	/* The group counts gl_NumWorkGroups reads: in the slot, or an indirect dispatch's where the walker reads them. */
	counts_va = slot_va + I915_GFX_DYNAMIC_HEAP + I915_GFX_DYN_GROUP_COUNTS;
	if (grid->indirect_va != 0U) {
		counts_va = grid->indirect_va;
	} else {
		kern_memcpy(dynamic + I915_GFX_DYN_GROUP_COUNTS, grid->groups, I915_COMPUTE_GROUP_COUNT_BYTES);
	}

	/* Sees the compute bind point's sets where the push data looks for the graphics ones. */
	view = *state;
	kern_memcpy(view.dset, state->compute_dset, sizeof(view.dset));
	kern_memcpy(view.dynamic_count, state->compute_dynamic_count, sizeof(view.dynamic_count));
	kern_memcpy(view.dynamic_bindings, state->compute_dynamic_bindings, sizeof(view.dynamic_bindings));
	kern_memcpy(view.dynamic_offsets, state->compute_dynamic_offsets, sizeof(view.dynamic_offsets));

	/* Fills the cross-thread push data. */
	curbe = dynamic + I915_GFX_DYN_CURBE;
	layout.regs = binary->cross_thread_regs;
	layout.constant_bytes = binary->push_constant_bytes;
	layout.block_count = binary->block_count;
	layout.blocks = binary->blocks;
	error = drv_i915_gfx_write_push(curbe, &view, &layout);
	if (error != 0)
		return error;

	/* Points the system storage buffer at the group counts: their address and their bytes. */
	address[0] = (uint32_t)counts_va;
	address[1] = (uint32_t)(counts_va >> 32);
	address[2] = I915_COMPUTE_GROUP_COUNT_BYTES;
	for (index = 0U; index < binary->block_count; index++) {
		block = &binary->blocks[index];
		if (block->set == DRV_GPU_IR_SYSTEM_SET && block->address != 0U) {
			kern_memcpy(curbe + block->push_offset, address, sizeof(address));
		}
	}

	/* Each thread's IDs follow the cross-thread data. */
	thread_bytes = pipeline->threads * I915_SHADER_PER_THREAD_REGS * I915_COMPUTE_REGISTER_BYTES;
	kern_memcpy(curbe + binary->cross_thread_regs * I915_COMPUTE_REGISTER_BYTES, pipeline->thread_ids, thread_bytes);

	/* Succeeded: the slot holds everything the dispatch loads. */
	return 0;
}

/*
 * Appends the commands of one dispatch to the batch: the context setup in
 * 3D at the dispatch's slot and window, the switch to GPGPU, the VFE, the
 * CURBE and the interface descriptor, an indirect dispatch's loads of its
 * counts, the walker over the groups, and the switch back to 3D behind the
 * flushes.
 */
void
drv_i915_gfx_dispatch_build(
	struct i915_gfx_batch *batch,
	const struct i915_gfx_op_space *space,
	const struct i915_gfx_pipeline *pipeline,
	const struct i915_gfx_kernels *kernels,
	const struct i915_gfx_grid *grid,
	uint32_t dss_count,
	uint32_t mocs)
{
	static const uint32_t dimensions[3] = {
		GEN12_GPGPU_DISPATCHDIMX,
		GEN12_GPGPU_DISPATCHDIMY,
		GEN12_GPGPU_DISPATCHDIMZ,
	};
	const struct i915_shader_binary *binary;
	uint64_t address;
	uint32_t walker;
	uint32_t index;
	uint64_t scratch;
	uint32_t curbe_regs;
	uint32_t curbe_allocation;
	uint32_t max_threads;

	/* The kernel, and its CURBE in whole pairs of registers (64-byte units). */
	binary = pipeline->cs_binary;
	curbe_regs = binary->cross_thread_regs + pipeline->threads * I915_SHADER_PER_THREAD_REGS;
	curbe_allocation = (curbe_regs + 1U) & ~1U;

	/* In 3D: the flush, the state bases at the slot and the window, and the invalidation (Wa_1607854226). */
	drv_i915_gfx_emit_context_setup(batch, space->slot_va, space->window_va, kernels->scratch_base, mocs);

	/* Switches 3D to GPGPU behind a stalling flush of the render target, depth and HDC caches (TGL PRM). */
	drv_i915_batch_pipe_control(batch,
				    PIPE_CONTROL_CS_STALL |
				    PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH |
				    PIPE_CONTROL_DEPTH_CACHE_FLUSH);
	drv_i915_batch_emit(batch, GEN12_PIPELINE_SELECT_DWORD(GEN12_PIPELINE_SELECT_GPGPU));

	/* The stalling flush MEDIA_VFE_STATE wants before it. */
	drv_i915_batch_pipe_control(batch, PIPE_CONTROL_CS_STALL | I915_COMPUTE_STALL_AT_SCOREBOARD);

	/*
	 * MEDIA_VFE_STATE: the kernel's scratch, the thread limit (112 threads
	 * a dual-subslice, less one), two URB entries of two, and the CURBE's
	 * registers.
	 */
	scratch = i915_compute_scratch(kernels->cs_scratch_bytes, kernels->cs_scratch_offset);
	max_threads = GEN12_VFE_THREADS_PER_DSS * dss_count - 1U;
	drv_i915_batch_emit(batch, GEN12_MEDIA_VFE_STATE_HEADER);
	drv_i915_batch_emit(batch, (uint32_t)scratch);
	drv_i915_batch_emit(batch, (uint32_t)(scratch >> 32));
	drv_i915_batch_emit(batch, (max_threads << GEN12_VFE_MAX_THREADS_SHIFT) | (GEN12_VFE_URB_ENTRIES << GEN12_VFE_URB_ENTRIES_SHIFT));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (GEN12_VFE_URB_ALLOCATION << GEN12_VFE_URB_ALLOCATION_SHIFT) | curbe_allocation);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);

	/* MEDIA_STATE_FLUSH before the loads. */
	drv_i915_batch_emit(batch, GEN12_MEDIA_STATE_FLUSH_HEADER);
	drv_i915_batch_emit(batch, 0U);

	/* MEDIA_CURBE_LOAD: the CURBE's bytes at its offset in the dynamic heap. */
	drv_i915_batch_emit(batch, GEN12_MEDIA_CURBE_LOAD_HEADER);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, curbe_allocation * I915_COMPUTE_REGISTER_BYTES);
	drv_i915_batch_emit(batch, I915_GFX_DYN_CURBE);

	/* MEDIA_INTERFACE_DESCRIPTOR_LOAD: the one descriptor at its offset. */
	drv_i915_batch_emit(batch, GEN12_MEDIA_INTERFACE_DESCRIPTOR_LOAD_HEADER);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, GEN12_INTERFACE_DESCRIPTOR_DWORDS * 4U);
	drv_i915_batch_emit(batch, I915_GFX_DYN_INTERFACE);

	/*
	 * An indirect dispatch: the walker's three counts loaded from the
	 * buffer (a PPGTT address, as the rest of the batch's) when the
	 * commands run, after the operations before them have written it.
	 */
	walker = GEN12_GPGPU_WALKER_HEADER;
	if (grid->indirect_va != 0U) {
		for (index = 0U; index < 3U; index++) {
			address = grid->indirect_va + (uint64_t)index * 4U;
			drv_i915_batch_emit(batch, MI_LOAD_REGISTER_MEM_GEN8);
			drv_i915_batch_emit(batch, dimensions[index]);
			drv_i915_batch_emit(batch, (uint32_t)address);
			drv_i915_batch_emit(batch, (uint32_t)(address >> 32));
		}

		/* The walker takes its counts from the registers. */
		walker |= GEN12_WALKER_INDIRECT;
	}

	/*
	 * GPGPU_WALKER: descriptor 0, SIMD8, the group's threads, the groups
	 * from the origin along x, y and z (an indirect walker takes them from
	 * the registers and these are zero), the channels of the group's last
	 * thread, every row.
	 */
	drv_i915_batch_emit(batch, walker);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, (GEN12_WALKER_SIMD8 << GEN12_WALKER_SIMD_SHIFT) | (pipeline->threads - 1U));
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, grid->groups[0]);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, grid->groups[1]);
	drv_i915_batch_emit(batch, 0U);
	drv_i915_batch_emit(batch, grid->groups[2]);
	drv_i915_batch_emit(batch, pipeline->right_mask);
	drv_i915_batch_emit(batch, 0xffffffffU);

	/* MEDIA_STATE_FLUSH after the walker. */
	drv_i915_batch_emit(batch, GEN12_MEDIA_STATE_FLUSH_HEADER);
	drv_i915_batch_emit(batch, 0U);

	/*
	 * Switches GPGPU back to 3D behind a stalling flush of the HDC and the
	 * data cache, which makes the kernel's writes visible to the CPU and to
	 * the next operation (TGL PRM; the Generic Media State Clear the PRM
	 * also lists is left out, as anv leaves it out for the hangs it causes).
	 */
	drv_i915_batch_pipe_control_hdc(batch,
					PIPE_CONTROL_CS_STALL |
					PIPE_CONTROL_DC_FLUSH_ENABLE |
					PIPE_CONTROL_FLUSH_ENABLE);
	drv_i915_batch_emit(batch, GEN12_PIPELINE_SELECT_DWORD(GEN12_PIPELINE_SELECT_3D));
}

/* Reports whether a kernel reads a uniform block, which the CPU copies into the CURBE. */
static int
i915_compute_reads_uniforms(
	const struct i915_shader_binary *binary)
{
	uint32_t index;

	/* A block that is not an address is a uniform block's range. */
	for (index = 0U; index < binary->block_count; index++) {
		if (binary->blocks[index].address == 0U)
			return 1;
	}

	/* Succeeded: no uniform block. */
	return 0;
}

/* Reports whether a kernel names a storage buffer of the application, which it may write. */
static int
i915_compute_names_storage(
	const struct i915_shader_binary *binary)
{
	uint32_t index;

	/* An address block other than the system buffer (the group counts) is an application's storage buffer. */
	for (index = 0U; index < binary->block_count; index++) {
		if (binary->blocks[index].address != 0U && binary->blocks[index].set != DRV_GPU_IR_SYSTEM_SET)
			return 1;
	}

	/* Succeeded: no storage buffer. */
	return 0;
}

/* Counts the dual-subslices the fuses left, at least one. */
static uint32_t
i915_compute_dss_count(
	const struct i915_render_session *session)
{
	uint32_t mask;
	uint32_t count;

	/* Counts the set bits of the subslice mask. */
	count = 0U;
	for (mask = session->vk->i915->gt.info.sseu.subslice_mask;
	     mask != 0U;
	     mask &= mask - 1U)
		count++;

	/* A device that reported none still runs one. */
	if (count == 0U)
		count = 1U;

	/* Succeeded: the dual-subslices. */
	return count;
}

/*
 * Packs dwords 1-2 of MEDIA_VFE_STATE: the Scratch Space Base Pointer of the
 * compute part of the scratch buffer (an offset from the general state
 * base) and the Per Thread Scratch Space (1 KiB << n); zero for a kernel
 * that spills nothing.
 */
static uint64_t
i915_compute_scratch(
	uint32_t per_thread_bytes,
	uint64_t offset)
{
	uint32_t space;
	uint32_t bytes;

	/* A kernel that spills nothing has no scratch space. */
	if (per_thread_bytes == 0U)
		return 0U;

	/* Finds n with 1 KiB << n equal to the kernel's space (the compiler rounds it to a power of two). */
	space = 0U;
	bytes = I915_COMPUTE_SCRATCH_MIN_BYTES;
	while (bytes < per_thread_bytes && space < I915_COMPUTE_SCRATCH_MAX_SPACE) {
		bytes *= 2U;
		space++;
	}

	/* Succeeded: the pointer's bits 47:10 and the space in bits 3:0. */
	return (offset & ~(uint64_t)(I915_COMPUTE_SCRATCH_MIN_BYTES - 1U)) | (uint64_t)space;
}

/*
 * Encodes the Shared Local Memory Size of the interface descriptor
 * (ws101-p006): 0 for none, else n with 1 KiB << (n - 1) the group's bytes
 * rounded up to a power of two of at least 1 KiB (Mesa 25.0.7
 * src/intel/common/intel_compute_slm.c, sha256
 * 2c5ccd8fdef7e070fd8c14ffa7fa172e29d77c65bee81e3ca3fa2e1a4dfc5ebc,
 * intel_compute_slm_calculate_size() and intel_compute_slm_encode_size() on
 * Gen9 to Gen12.0).  The parser refuses more than 16 KiB.
 */
static uint32_t
i915_compute_slm_size(
	uint32_t bytes)
{
	uint32_t size;
	uint32_t encoded;

	/* A kernel without shared memory takes none. */
	if (bytes == 0U)
		return 0U;

	/* Finds the smallest power of two from 1 KiB that holds the bytes. */
	size = I915_COMPUTE_SLM_MIN_BYTES;
	encoded = 1U;
	while (size < bytes && encoded < I915_COMPUTE_SLM_MAX_SIZE) {
		size *= 2U;
		encoded++;
	}

	/* Succeeded: the encoding. */
	return encoded;
}
