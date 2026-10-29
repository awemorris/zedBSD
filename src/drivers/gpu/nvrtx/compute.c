/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first compute launch (the compute-test stage, nvrtx.compute=1).
 *
 * After the golden context (golden-context) the driver makes a graphics
 * channel of its own and runs one compute program on it:
 *
 *   1. an address space the driver owns, over page tables in an arena of
 *      VRAM below the graphics arena; the tables map the channel's context
 *      buffers (its own main and patch buffers, the golden context's global
 *      ones) privileged, and the launch's system-memory buffers;
 *   2. the channel on the graphics engine (its instance block names the
 *      tables), the promote of its context buffers (the scrubber's form),
 *      the compute object (TURING_COMPUTE_A), the bind and the schedule;
 *   3. the program compiled by the nvrtx shader compiler from the IR of
 *      "every invocation writes 42 at its index", its QMD (version 2.2)
 *      with constant buffer 0 holding the storage buffer's address, the
 *      launch and a semaphore fence after it;
 *   4. the result: 32 words of 42, and the word after them untouched;
 *   5. when the boot asks (nvrtx.draw=1), the first draw on the same
 *      channel (draw.c, the draw-test stage), whose pages the tables map too.
 *
 * Nothing of the display or of the copy channel is touched.  The stop
 * undoes 2 and 1 before the graphics context's stop.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-gpu.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

/* The arena: its size (its reserve below the top, under the graphics arena, is NVRTX_COMPUTE_ARENA_RESERVE). */
#define COMPUTE_ARENA_BYTES		0x2000000ULL
#define COMPUTE_ARENA_ALIGN_SHIFT	21U

/* The arena's parts: the tables, the instance block and USERD, then the channel's own context buffers. */
#define COMPUTE_TABLES_AT		0x000000ULL
#define COMPUTE_INSTANCE_AT		0x080000ULL
#define COMPUTE_USERD_AT		0x081000ULL
#define COMPUTE_BUFFERS_AT		0x100000ULL

/* The arena's last MiB, the first draw's VRAM colour target without the VRAM allocator (the context buffers stay below it). */
#define COMPUTE_DRAW_AT			0x1f00000ULL

/* The range: the context buffers where the golden context had them, then the launch's pages. */
#define COMPUTE_VA_BASE			0x20000000ULL
#define COMPUTE_VA_BYTES		0x8200000ULL
#define COMPUTE_VA_PAGES		0x28000000ULL

/* The launch's pages in the range, in order. */
#define COMPUTE_RING_PAGE		0U
#define COMPUTE_PUSH_PAGE		1U
#define COMPUTE_PUSH_PAGES		4U
#define COMPUTE_SEMAPHORE_PAGE		5U
#define COMPUTE_PROGRAM_PAGE		6U
#define COMPUTE_QMD_PAGE		7U
#define COMPUTE_CONSTANTS_PAGE		8U
#define COMPUTE_RESULT_PAGE		9U
#define COMPUTE_PAGES			10U
#define COMPUTE_RING_ENTRIES		512U

/* The first draw's fences name the semaphore page by its address (nvrtx-gpu.h). */
_Static_assert(COMPUTE_VA_PAGES + COMPUTE_SEMAPHORE_PAGE * 0x1000ULL == NVRTX_COMPUTE_SEMAPHORE_VA, "the semaphore page the draw names");

/* The address-space limit written into the instance block (47-bit virtual addresses). */
#define COMPUTE_VA_LIMIT		(1ULL << 47)

/* The channel number on the graphics runlist (the golden context took 1, the scrubber 2). */
#define COMPUTE_CHID			3U

/*
 * USERD's put of the ring, and the doorbell: runlist in bits 22:16,
 * channel in bits 11:0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (Nvc46fControl_struct), NVIDIA open-gpu-kernel-modules 570.144
 * src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_DOORBELL) and Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/vfn/tu102.c (tu102_vfn_new) and
 * drivers/gpu/drm/nouveau/nvkm/engine/fifo/tu102.c
 * (tu102_chan_doorbell_handle).
 */
#define COMPUTE_USERD_RING_PUT		0x8cU
#define COMPUTE_REG_DOORBELL		0xbb0090U

/*
 * The promote's number of the unrestricted access map, which a channel
 * sharing the global buffers leaves out.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gr.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_UNRESTRICTED_PRIV_ACCESS_MAP).
 */
#define COMPUTE_BUFFER_UNRESTRICTED_MAP	11U

/* The fence value, how long the CPU waits for it, and how often it looks. */
#define COMPUTE_FENCE_VALUE		0x4242U
#define COMPUTE_FENCE_TIMEOUT_US	2000000U
#define COMPUTE_FENCE_STEP_US		100U

/* What the result page holds before the launch, so a word the program did not write shows. */
#define COMPUTE_RESULT_FILL		0xdeadbeefU

/* Constant buffer 0's size in the QMD: the storage table's first entry and more, in whole 256-byte units. */
#define COMPUTE_CONSTANTS_BYTES		0x300U

static int compute_arena(struct nvrtx_gpu *gpu);
static int compute_buffers(struct nvrtx_gpu *gpu);
static int compute_tables(struct nvrtx_gpu *gpu);
static int compute_space(struct nvrtx_gpu *gpu);
static int compute_channel(struct nvrtx_gpu *gpu);
static int compute_program(struct nvrtx_gpu *gpu, uint32_t *gpr_count);
static int compute_launch(struct nvrtx_gpu *gpu, uint32_t gpr_count);
static int compute_check(struct nvrtx_gpu *gpu);
static int compute_map_draw(struct nvrtx_gpu *gpu, uint8_t *image);
static uint64_t compute_page_va(uint32_t page);
static uint64_t compute_align(uint64_t value, uint32_t shift);

/*
 * Runs the compute-test stage: the space, the channel, the program, the
 * launch and the check of its result.
 *
 * Needs the golden context of golden-context.  A failure is returned; the
 * finish's stop frees whatever objects were made.
 */
int
nvrtx_compute_start(
	struct nvrtx_gpu *gpu)
{
	uint32_t gpr_count;
	bool wanted;
	bool allowed;
	int error;

	nvrtx_stage_mark("compute-test begin");

	/* The channel's context comes from the golden context. */
	if (gpu->gr.buffer_count == 0U) {
		nvrtx_stage_mark("compute-test fail: no golden context (nvrtx.gr=1 is needed)");
		return EINVAL;
	}

	/* The arena, the system-memory buffers and the tables. */
	error = compute_arena(gpu);
	if (error != 0)
		return error;

	error = compute_buffers(gpu);
	if (error != 0)
		return error;

	error = compute_tables(gpu);
	if (error != 0)
		return error;

	/* The space and the channel with its compute object. */
	error = compute_space(gpu);
	if (error != 0)
		return error;

	error = compute_channel(gpu);
	if (error != 0)
		return error;

	nvrtx_stage_mark("compute-test ok: compute channel %u on runlist %u", (unsigned)COMPUTE_CHID, (unsigned)gpu->engine.pick.graphics_runlist);

	/* The program, then the launch and its fence. */
	error = compute_program(gpu, &gpr_count);
	if (error != 0)
		return error;

	error = compute_launch(gpu, gpr_count);
	if (error != 0)
		return error;

	/* The result. */
	error = compute_check(gpu);
	if (error != 0)
		return error;

	/* Draws on the same channel when the boot asks (nvrtx.draw=1) and does not stop before it. */
	wanted = nvrtx_stage_draw_wanted();
	allowed = nvrtx_stage_allowed("draw-test");
	if (wanted && !allowed)
		nvrtx_stage_mark("draw-test skipped (nvrtx.stop=draw-test)");
	if (wanted && allowed) {
		error = nvrtx_draw_start(gpu);
		if (error != 0)
			return error;
	}

	/* Succeeded: the GPU ran the program the driver compiled. */
	return 0;
}

/*
 * Undoes compute-test's objects before the graphics context's
 * stop: the channel off the scheduler, the compute object, the channel, the
 * root, the space.  Failures are counted and shown; the unload removes the
 * rest.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_stop)
 * and Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/fifo/chan.c
 * (nvkm_chan_del).
 */
void
nvrtx_compute_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	uint8_t params[NVRTX_UNPDIR_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int failures;
	int error;

	compute = &gpu->compute;
	failures = 0;

	/* Takes the channel off the scheduler. */
	if (compute->scheduled != 0) {
		kern_memset(params, 0, sizeof(params));
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_COMPUTE_CHANNEL, NVRTX_CTL_SCHEDULE, params, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
		if (error != 0)
			failures++;
		compute->scheduled = 0;
	}

	/* Frees the first draw's 3D object. */
	failures += nvrtx_draw_stop(gpu);

	/* Frees the compute object, then the channel. */
	if (compute->compute_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_COMPUTE);
		if (error != 0)
			failures++;
		compute->compute_made = 0;
	}

	if (compute->channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_COMPUTE_CHANNEL);
		if (error != 0)
			failures++;
		compute->channel_made = 0;
	}

	/* Takes the root back from the space, then frees it. */
	if (compute->root_set != 0) {
		kern_memset(params, 0, sizeof(params));
		nvrtx_put32(params, 0U, NVRTX_HANDLE_COMPUTE_SPACE);
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_DEVICE, NVRTX_CTL_UNSET_PAGE_DIRECTORY, params, sizeof(params), &reply, &reply_length);
		if (error != 0)
			failures++;
		compute->root_set = 0;
	}

	if (compute->space_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_COMPUTE_SPACE);
		if (error != 0)
			failures++;
		compute->space_made = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d compute frees failed; the unload removes the rest", failures);
}

/*
 * Gives back compute-test's system-memory buffers.
 *
 * Only once the GPU no longer reads them: after the GSP is stopped.
 */
void
nvrtx_compute_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;

	compute = &gpu->compute;

	/* Frees each buffer that was allocated. */
	nvrtx_buffer_free(gpu, &compute->ring);
	nvrtx_buffer_free(gpu, &compute->push);
	nvrtx_buffer_free(gpu, &compute->semaphore);
	nvrtx_buffer_free(gpu, &compute->program);
	nvrtx_buffer_free(gpu, &compute->qmd);
	nvrtx_buffer_free(gpu, &compute->constants);
	nvrtx_buffer_free(gpu, &compute->result);
	nvrtx_buffer_free(gpu, &compute->methods);

	/* The first draw's buffers. */
	nvrtx_draw_release(gpu);
}

/*
 * Submits a push buffer segment of the channel: the ring entry, the ring's
 * put in USERD past it, and the doorbell.
 *
 * Entries are used in order from 0 and the ring never wraps, so the put is
 * one past the entry.  Returns 0, or the error of the write into USERD.
 */
int
nvrtx_compute_submit(
	struct nvrtx_gpu *gpu,
	uint32_t entry,
	uint64_t push_va,
	size_t length)
{
	struct nvrtx_compute_state *compute;
	uint8_t put[4];
	int error;

	compute = &gpu->compute;

	/* The entry, written before the put that hands it to the GPU. */
	nvrtx_ring_entry((uint8_t *)compute->ring.dma.address + (size_t)entry * 8U, push_va, (uint32_t)(length / 4U));
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The ring's put in USERD. */
	nvrtx_put32(put, 0U, entry + 1U);
	error = nvrtx_vram_write(gpu, compute->arena + COMPUTE_USERD_AT + COMPUTE_USERD_RING_PUT, put, sizeof(put));
	if (error != 0)
		return error;

	/* The doorbell: the graphics runlist and the channel. */
	nvrtx_wr32(gpu, COMPUTE_REG_DOORBELL, (gpu->engine.pick.graphics_runlist << 16) | COMPUTE_CHID);
	(void)nvrtx_gpu_submitted(gpu);

	/* Succeeded: the GPU fetches the entry. */
	return 0;
}

/*
 * Polls the channel's semaphore until it holds a fence's value or the
 * time, measured, is spent (2 s).
 *
 * Returns 0 with the microseconds waited, or ETIMEDOUT.
 */
int
nvrtx_compute_wait(
	struct nvrtx_gpu *gpu,
	uint32_t value,
	uint32_t *waited)
{
	const volatile uint32_t *semaphore;
	uint64_t start;
	uint64_t spent;
	uint32_t seen;

	/* The semaphore the fences release. */
	semaphore = (const volatile uint32_t *)gpu->compute.semaphore.dma.address;

	/* Looks until the value is there or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		seen = *semaphore;
		spent = nvrtx_now_us() - start;
		if (seen == value)
			break;
		if (spent > COMPUTE_FENCE_TIMEOUT_US)
			return ETIMEDOUT;
		kern_usleep_range(COMPUTE_FENCE_STEP_US, COMPUTE_FENCE_STEP_US * 2U);
	}

	/* Succeeded: the work before the fence is done. */
	*waited = (uint32_t)spent;
	return 0;
}

/*
 * Places the arena below the graphics arena and the channel's own context
 * buffers in it; the global ones are the golden context's.
 */
static int
compute_arena(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t candidate;
	uint64_t physical;
	uint32_t region;
	size_t index;
	int error;

	compute = &gpu->compute;
	gr = &gpu->gr;

	/* Takes the arena below the graphics arena, in the last usable region. */
	error = nvrtx_mmu_vram_block(gpu->static_info, NVRTX_COMPUTE_ARENA_RESERVE, &candidate, &region);
	if (error != 0) {
		nvrtx_stage_mark("compute-test fail: no VRAM for the arena (%d)", error);
		return error;
	}

	compute->arena = compute_align(candidate, COMPUTE_ARENA_ALIGN_SHIFT);
	if (compute->arena + COMPUTE_ARENA_BYTES > gr->arena) {
		nvrtx_stage_mark("compute-test fail: the arena %llx meets the graphics arena", (unsigned long long)compute->arena);
		return ENOSPC;
	}

	/* Each buffer: the golden context's when global, else one of the channel's own. */
	physical = COMPUTE_BUFFERS_AT;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (buffer->global) {
			compute->physical[index] = gr->golden_physical[index];
			continue;
		}

		physical = compute_align(physical, buffer->page_shift);
		compute->physical[index] = compute->arena + physical;
		physical += compute_align(buffer->size, buffer->page_shift);
	}

	/* The channel's own buffers must fit the arena below the draw's target. */
	if (physical > COMPUTE_DRAW_AT) {
		nvrtx_stage_mark("compute-test fail: the context buffers need %llx of VRAM", (unsigned long long)physical);
		return ENOSPC;
	}

	/* Succeeded: the arena is laid out. */
	nvrtx_stage_mark("compute-test arena at %llx, own buffers %llx bytes", (unsigned long long)compute->arena, (unsigned long long)(physical - COMPUTE_BUFFERS_AT));
	return 0;
}

/* Allocates the launch's system-memory buffers and the channel's method buffer. */
static int
compute_buffers(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	bool wanted;
	int error;

	compute = &gpu->compute;

	/* One page each, the push buffer four. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->ring);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, COMPUTE_PUSH_PAGES * NVRTX_GSP_PAGE_SIZE, &compute->push);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->semaphore);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->program);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->qmd);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->constants);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &compute->result);
	if (error != 0)
		return error;

	/*
	 * The method buffer, as large as the resource manager asked for
	 * copy-channel's.
	 */
	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &compute->methods);
	if (error != 0)
		return error;

	/* The first draw's buffers, when the boot asks for it. */
	wanted = nvrtx_stage_draw_wanted();
	if (wanted) {
		error = nvrtx_draw_buffers(gpu, compute->arena + COMPUTE_DRAW_AT);
		if (error != 0)
			return error;
	}

	/* Succeeded: the buffers are clear. */
	return 0;
}

/*
 * Builds the space's tables and writes them into the arena: the context
 * buffers privileged at the golden context's addresses (the access map
 * read-only, the unrestricted one left out), the launch's pages after them.
 */
static int
compute_tables(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t pages[COMPUTE_PAGES];
	uint8_t *image;
	size_t length;
	size_t index;
	unsigned page;
	int error;

	compute = &gpu->compute;
	gr = &gpu->gr;

	/* The launch's pages in order. */
	pages[COMPUTE_RING_PAGE] = compute->ring.dma.device_address;
	for (page = 0U; page < COMPUTE_PUSH_PAGES; page++)
		pages[COMPUTE_PUSH_PAGE + page] = compute->push.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;
	pages[COMPUTE_SEMAPHORE_PAGE] = compute->semaphore.dma.device_address;
	pages[COMPUTE_PROGRAM_PAGE] = compute->program.dma.device_address;
	pages[COMPUTE_QMD_PAGE] = compute->qmd.dma.device_address;
	pages[COMPUTE_CONSTANTS_PAGE] = compute->constants.dma.device_address;
	pages[COMPUTE_RESULT_PAGE] = compute->result.dma.device_address;

	/* The directories in memory. */
	length = nvrtx_mmu_gr_tables_bytes(COMPUTE_VA_BYTES);
	image = kern_malloc(length);
	if (image == NULL)
		return ENOMEM;

	error = nvrtx_mmu_gr_tables(image, compute->arena + COMPUTE_TABLES_AT, COMPUTE_VA_BASE, COMPUTE_VA_BYTES);

	/* Each context buffer the promote names by address. */
	for (index = 0U; error == 0 && index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (buffer->buffer_id == COMPUTE_BUFFER_UNRESTRICTED_MAP)
			continue;
		error = nvrtx_mmu_gr_map(image,
		    COMPUTE_VA_BASE,
		    COMPUTE_VA_BYTES,
		    gr->virtual_address[index],
		    compute->physical[index],
		    compute_align(buffer->size, 12U),
		    buffer->read_only);
	}

	/* The launch's pages. */
	if (error == 0)
		error = nvrtx_mmu_gr_map_system(image, COMPUTE_VA_BASE, COMPUTE_VA_BYTES, COMPUTE_VA_PAGES, pages, COMPUTE_PAGES);

	/* The first draw's pages and VRAM target, when the boot asks for it. */
	if (error == 0)
		error = compute_map_draw(gpu, image);

	/* Copies the tables into the arena, and clears the instance block and USERD. */
	if (error == 0)
		error = nvrtx_vram_write(gpu, compute->arena + COMPUTE_TABLES_AT, image, length);
	kern_free(image);
	if (error == 0)
		error = nvrtx_vram_zero(gpu, compute->arena + COMPUTE_INSTANCE_AT, 2U * NVRTX_INSTANCE_BYTES);
	if (error != 0) {
		nvrtx_stage_mark("compute-test fail: the tables could not be written (%d)", error);
		return error;
	}

	/* Succeeded: the tables are in the VRAM. */
	nvrtx_stage_mark("compute-test page tables at %llx", (unsigned long long)(compute->arena + COMPUTE_TABLES_AT));
	return 0;
}

/* Makes the address space the driver owns over the tables, gives it the root and drops stale translations. */
static int
compute_space(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	uint8_t params[NVRTX_SPACE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint64_t root;
	int error;

	compute = &gpu->compute;
	root = compute->arena + COMPUTE_TABLES_AT;

	/* The space, owned by the driver (as address-space's). */
	nvrtx_space_params(params);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_COMPUTE_SPACE, NVRTX_CLASS_SPACE, params, sizeof(params));
	if (error != 0)
		return error;

	compute->space_made = 1;

	/* The root. */
	nvrtx_pdir_params(params, root, NVRTX_HANDLE_COMPUTE_SPACE);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_DEVICE, NVRTX_CTL_SET_PAGE_DIRECTORY, params, NVRTX_PDIR_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	compute->root_set = 1;

	/* Drops any stale translation under the root. */
	error = nvrtx_flush_tlb(gpu, root);
	if (error != 0)
		return error;

	/* Succeeded: the space maps the range. */
	nvrtx_stage_mark("compute-test ok: vaspace");
	return 0;
}

/*
 * Makes the graphics channel: its instance block naming the tables, the
 * channel on the graphics engine, the promote, the compute object, the bind
 * and the schedule.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_alloc).
 */
static int
compute_channel(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_compute_state *compute;
	struct nvrtx_channel_setup setup;
	uint8_t *params;
	uint8_t *instance;
	const uint8_t *reply;
	size_t reply_length;
	int error;

	compute = &gpu->compute;

	/* The instance block names the tables. */
	instance = kern_calloc(1U, NVRTX_INSTANCE_BYTES);
	if (instance == NULL)
		return ENOMEM;

	nvrtx_mmu_instance(instance, compute->arena + COMPUTE_TABLES_AT, COMPUTE_VA_LIMIT);
	error = nvrtx_vram_write(gpu, compute->arena + COMPUTE_INSTANCE_AT, instance, NVRTX_INSTANCE_BYTES);
	kern_free(instance);
	if (error != 0)
		return error;

	/* The channel on the graphics engine, its ring at the first launch page. */
	setup.ring_va = compute_page_va(COMPUTE_RING_PAGE);
	setup.ring_entries = COMPUTE_RING_ENTRIES;
	setup.chid = COMPUTE_CHID;
	setup.space = NVRTX_HANDLE_COMPUTE_SPACE;
	setup.engine = NVRTX_ENGINE_GRAPHICS;
	setup.privileged = 0U;
	setup.instance = compute->arena + COMPUTE_INSTANCE_AT;
	setup.userd = compute->arena + COMPUTE_USERD_AT;
	setup.methods = compute->methods.dma.device_address;
	setup.methods_size = gpu->engine.methods_size;

	params = kern_malloc(NVRTX_CHANNEL_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_COMPUTE_CHANNEL, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	if (error != 0) {
		kern_free(params);
		return error;
	}

	compute->channel_made = 1;

	/* Hands it its context buffers: its own main and patch, the golden context's global ones. */
	error = nvrtx_gr_promote(gpu, NVRTX_HANDLE_COMPUTE_CHANNEL, compute->physical, NULL, false);
	if (error != 0) {
		kern_free(params);
		return error;
	}

	/* The compute object. */
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_COMPUTE_CHANNEL, NVRTX_HANDLE_COMPUTE, gpu->identity.chip.profile->class_compute, NULL, 0U);
	if (error != 0) {
		kern_free(params);
		return error;
	}

	compute->compute_made = 1;

	/* Binds it to the graphics engine and lets the scheduler run it. */
	kern_memset(params, 0, NVRTX_BIND_PARAMS_BYTES);
	nvrtx_put32(params, 0U, NVRTX_ENGINE_GRAPHICS);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_COMPUTE_CHANNEL, NVRTX_CTL_BIND, params, NVRTX_BIND_PARAMS_BYTES, &reply, &reply_length);
	if (error == 0) {
		kern_memset(params, 0, NVRTX_SCHEDULE_PARAMS_BYTES);
		params[0] = 1U;
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_COMPUTE_CHANNEL, NVRTX_CTL_SCHEDULE, params, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
	}
	kern_free(params);
	if (error != 0)
		return error;

	/* Succeeded: the stop takes the channel off the scheduler before it frees it. */
	compute->scheduled = 1;
	return 0;
}

/* Compiles the first program and copies its code into the program page. */
static int
compute_program(
	struct nvrtx_gpu *gpu,
	uint32_t *gpr_count)
{
	struct nvrtx_compute_first *first;
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_sm75_diagnostic diagnostic;
	size_t bytes;
	int error;

	/* The IR of "every invocation writes 42 at its index". */
	first = kern_malloc(sizeof(*first));
	if (first == NULL)
		return ENOMEM;

	nvrtx_compute_first_ir(first);

	/* Compiled for the generation's shader model. */
	binary = NULL;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_ir_sm(&first->ir, gpu->identity.chip.profile->sm, &binary, &diagnostic);
	kern_free(first);
	if (error != 0) {
		nvrtx_stage_mark("compute-test fail: the program did not compile (%d: %s)", error, (diagnostic.reason != NULL) ? diagnostic.reason : "?");
		return error;
	}

	/* The code must fit the page. */
	bytes = (size_t)binary->code_words * 4U;
	if (bytes > NVRTX_GSP_PAGE_SIZE || binary->storage_count != 1U) {
		nvrtx_stage_mark("compute-test fail: the program takes %u bytes and %u storage buffers", (unsigned)bytes, (unsigned)binary->storage_count);
		nvrtx_sm75_binary_free(binary);
		return EINVAL;
	}

	/* Copies the code; the page is coherent memory, so the GPU sees it as written. */
	kern_memcpy(gpu->compute.program.dma.address, binary->code, bytes);
	*gpr_count = binary->gpr_count;
	nvrtx_stage_mark("compute-test program: %u instructions, %u registers", (unsigned)(binary->code_words / 4U), (unsigned)binary->gpr_count);
	nvrtx_sm75_binary_free(binary);

	/* Succeeded: the program is in place. */
	return 0;
}

/*
 * Fills constant buffer 0 and the QMD, writes the launch and a fence after
 * it, submits them, and waits for the fence.
 *
 * Procedure referenced: Mesa 25.3.6 src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_CmdDispatchBase, nvk_cmd_upload_qmd).
 */
static int
compute_launch(
	struct nvrtx_gpu *gpu,
	uint32_t gpr_count)
{
	struct nvrtx_compute_state *compute;
	struct nvrtx_compute_kind kind;
	struct nvrtx_qmd_info info;
	uint32_t *words;
	uint8_t *push;
	uint64_t result_va;
	uint64_t push_va;
	size_t length;
	uint32_t index;
	uint32_t waited;
	int error;

	compute = &gpu->compute;
	result_va = compute_page_va(COMPUTE_RESULT_PAGE);

	/* The generation's QMD version and compute class. */
	kind.qmd_major = gpu->identity.chip.profile->qmd_major;
	kind.qmd_minor = gpu->identity.chip.profile->qmd_minor;
	kind.compute_class = gpu->identity.chip.profile->class_compute;

	/* The result page starts as a pattern the program does not write. */
	words = (uint32_t *)compute->result.dma.address;
	for (index = 0U; index < NVRTX_GSP_PAGE_SIZE / 4U; index++)
		words[index] = COMPUTE_RESULT_FILL;

	/* Constant buffer 0: storage buffer 0's address and size at the table's first entry (sm75.h). */
	kern_memset(compute->constants.dma.address, 0, NVRTX_GSP_PAGE_SIZE);
	words = (uint32_t *)compute->constants.dma.address;
	words[NVRTX_SM75_CB_STORAGE / 4U] = (uint32_t)(result_va & 0xffffffffU);
	words[NVRTX_SM75_CB_STORAGE / 4U + 1U] = (uint32_t)(result_va >> 32);
	words[(NVRTX_SM75_CB_STORAGE + NVRTX_SM75_CB_STORAGE_SIZE) / 4U] = NVRTX_COMPUTE_FIRST_WIDTH * 4U;

	/* The QMD: one workgroup of 32, the program, constant buffer 0. */
	kern_memset(&info, 0, sizeof(info));
	info.program = compute_page_va(COMPUTE_PROGRAM_PAGE);
	info.groups[0] = 1U;
	info.groups[1] = 1U;
	info.groups[2] = 1U;
	info.local_size[0] = NVRTX_COMPUTE_FIRST_WIDTH;
	info.local_size[1] = 1U;
	info.local_size[2] = 1U;
	info.gpr_count = gpr_count;
	info.cbuf_address[0] = compute_page_va(COMPUTE_CONSTANTS_PAGE);
	info.cbuf_size[0] = COMPUTE_CONSTANTS_BYTES;
	error = nvrtx_qmd_build_for((uint32_t *)compute->qmd.dma.address, &info, &kind);
	if (error != 0) {
		nvrtx_stage_mark("compute-test fail: the QMD could not be built (%d)", error);
		return error;
	}

	/* The launch, then the fence. */
	kern_memset(compute->semaphore.dma.address, 0, NVRTX_GSP_PAGE_SIZE);
	push = (uint8_t *)compute->push.dma.address;
	length = nvrtx_push_compute_launch_for(push, compute_page_va(COMPUTE_QMD_PAGE), &kind);
	length += nvrtx_push_fence(push + length, compute_page_va(COMPUTE_SEMAPHORE_PAGE), COMPUTE_FENCE_VALUE);

	/* Ring entry 0. */
	push_va = compute_page_va(COMPUTE_PUSH_PAGE);
	error = nvrtx_compute_submit(gpu, 0U, push_va, length);
	if (error != 0)
		return error;

	/* Waits for the fence. */
	error = nvrtx_compute_wait(gpu, COMPUTE_FENCE_VALUE, &waited);
	if (error != 0) {
		nvrtx_stage_mark("compute-test fail: fence %08x after 2 s, result word 0 %08x", (unsigned)*(const volatile uint32_t *)compute->semaphore.dma.address, (unsigned)((const volatile uint32_t *)compute->result.dma.address)[0]);
		return error;
	}

	/* Succeeded: the launch ran to its fence. */
	nvrtx_stage_mark("compute-test ok: fence 0x%x reached after %u us", (unsigned)COMPUTE_FENCE_VALUE, (unsigned)waited);
	return 0;
}

/* Checks the result: every invocation's word is 42, and the word after them kept the pattern. */
static int
compute_check(
	struct nvrtx_gpu *gpu)
{
	const volatile uint32_t *words;
	uint32_t index;

	words = (const volatile uint32_t *)gpu->compute.result.dma.address;

	/* Each invocation's word. */
	for (index = 0U; index < NVRTX_COMPUTE_FIRST_WIDTH; index++) {
		if (words[index] != NVRTX_COMPUTE_FIRST_VALUE) {
			nvrtx_stage_mark("compute-test fail: result word %u is %08x", (unsigned)index, (unsigned)words[index]);
			return EIO;
		}
	}

	/* The word after them. */
	if (words[NVRTX_COMPUTE_FIRST_WIDTH] != COMPUTE_RESULT_FILL) {
		nvrtx_stage_mark("compute-test fail: the program wrote past its words (%08x)", (unsigned)words[NVRTX_COMPUTE_FIRST_WIDTH]);
		return EIO;
	}

	/* Succeeded: the program ran as compiled. */
	nvrtx_stage_mark("compute-test ok: compute wrote %u words of %u", (unsigned)NVRTX_COMPUTE_FIRST_WIDTH, (unsigned)NVRTX_COMPUTE_FIRST_VALUE);
	return 0;
}

/*
 * Maps the first draw's system-memory pages after the launch's, and its
 * VRAM colour target for work, when the boot asks for the draw; maps
 * nothing otherwise.
 */
static int
compute_map_draw(
	struct nvrtx_gpu *gpu,
	uint8_t *image)
{
	uint64_t pages[NVRTX_DRAW_PAGES];
	bool wanted;
	int error;

	/* Only a boot that draws has the pages. */
	wanted = nvrtx_stage_draw_wanted();
	if (!wanted)
		return 0;

	/* The system-memory pages in the draw's order. */
	nvrtx_draw_pages(gpu, pages);
	error = nvrtx_mmu_gr_map_system(image, COMPUTE_VA_BASE, COMPUTE_VA_BYTES, NVRTX_DRAW_VA_PAGES, pages, NVRTX_DRAW_PAGES);
	if (error != 0)
		return error;

	/* The VRAM colour target. */
	error = nvrtx_mmu_gr_map_vram(image, COMPUTE_VA_BASE, COMPUTE_VA_BYTES, NVRTX_DRAW_VA_VRAM, gpu->draw.vram_target, NVRTX_DRAW_VRAM_BYTES);
	if (error != 0)
		return error;

	/* Succeeded: the draw's memory is in the range. */
	return 0;
}

/* Reports the address of one of the launch's pages in the range. */
static uint64_t
compute_page_va(
	uint32_t page)
{
	/* The pages follow each other from the launch's start. */
	return COMPUTE_VA_PAGES + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;
}

/* Rounds a value up to a multiple of 2^shift. */
static uint64_t
compute_align(
	uint64_t value,
	uint32_t shift)
{
	uint64_t unit;

	unit = 1ULL << shift;

	/* Reports the rounded value. */
	return (value + unit - 1U) & ~(unit - 1U);
}
