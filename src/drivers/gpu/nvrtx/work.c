/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The work channel (work-channel, nvrtx.work=1).
 *
 * After the golden context (golden-context) and the VRAM allocator
 * (vram-window) the driver makes the graphics channel the GPU device keeps
 * for the work it is given, the way the first compute launch (compute.c)
 * makes its channel, with both classes on the one channel:
 *
 *   1. an arena of the VRAM allocator for the space's page tables, the
 *      channel's instance block and USERD, and its own context buffers;
 *   2. an address space the driver owns over the 3.5 GiB of
 *      NVRTX_WORK_VA_BASE: the context buffers privileged at the golden
 *      context's addresses, the driver's pages (ring, semaphore, the
 *      fences' push buffers), and later the runs mapped for users;
 *   3. the channel on the graphics engine, the promote of its context
 *      buffers, the 3D and compute objects, the bind and the schedule;
 *   4. a first submission that binds both classes to their subchannels,
 *      and its fence.
 *
 * Afterwards nvrtx_work_channel_submit() hands a push buffer segment to the
 * channel followed by a fence of the driver's, nvrtx_work_channel_passed()
 * tells whether a fence has passed and nvrtx_work_channel_wait() polls for
 * one, and the map calls write the space's page entries for a run and drop the
 * stale translations.  The stop undoes 3 and 2 before the graphics
 * context's stop.
 *
 * A session of the GPU device gets a channel of its own the same way
 * (nvrtx_work_channel_open()): an arena of its own for its instance block,
 * USERD and own context buffers, and 2 MiB of the space (its region) where
 * those buffers and its ring, semaphore and fences' push buffers are
 * mapped; the global context buffers, the page tables and the users' runs
 * stay the space's.  So work that never ends holds up its own channel only,
 * and the channel can be freed without the others.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/* The arena: its size and alignment in the VRAM allocator. */
#define WORK_ARENA_BYTES		0x2000000ULL
#define WORK_ARENA_ALIGN		0x200000ULL

/*
 * Where the runs leave their stamps in the semaphore's page, so a run's
 * start (every run) and its time (timed runs, nvrtx.vk.perf=1) can be read
 * after later runs have passed too: a record of 32 bytes for each of the
 * last WORK_RUN_RECORDS runs (by the fence's low bits) after the fence's
 * word, the start stamp's 16 bytes (value, then the timer at byte 8), then
 * the end stamp's.
 *
 * Value referenced: NVIDIA open-gpu-doc
 * manuals/turing/tu104/dev_pbdma.ref.txt
 * (NV_UDMA_SEM_EXECUTE_RELEASE_TIMESTAMP, the 16-byte release with a
 * timestamp).
 */
#define WORK_RUNS_AT			0x100U
#define WORK_RUN_RECORDS		NVRTX_WORK_RUN_RECORDS
#define WORK_RUN_RECORD_BYTES		32U
#define WORK_RUN_END_AT			16U
#define WORK_STAMP_TIMER_AT		8U

/*
 * The arena's parts: the tables (about 7 MiB for the 3.5 GiB space), the
 * instance block and USERD, then the channel's own context buffers.
 */
#define WORK_TABLES_AT			0x000000ULL
#define WORK_INSTANCE_AT		0x800000ULL
#define WORK_USERD_AT			0x801000ULL
#define WORK_BUFFERS_AT			0xa00000ULL

/* The driver's pages in the space, in order: the ring, the semaphore, the fences' push buffers. */
#define WORK_RING_VA			(NVRTX_WORK_VA_DRIVER + 0x0000ULL)
#define WORK_SEMAPHORE_VA		(NVRTX_WORK_VA_DRIVER + 0x1000ULL)
#define WORK_FENCES_VA			(NVRTX_WORK_VA_DRIVER + 0x2000ULL)
#define WORK_FENCES_BYTES		(NVRTX_WORK_RING_ENTRIES * NVRTX_WORK_FENCE_BYTES)
#define WORK_DRIVER_PAGES		(2U + WORK_FENCES_BYTES / 0x1000U)

/* The address-space limit written into the instance block (47-bit virtual addresses). */
#define WORK_VA_LIMIT			(1ULL << 47)

/*
 * The graphics engine's current context: the instance block (its address
 * shifted right by 12, in the low 28 bits) of the channel the engine holds.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/gr/gf100.c
 * (gf100_gr_intr, gf100_gr_ctxsw_inst: 0x409b00).
 */
#define WORK_REG_GR_CURRENT_CONTEXT	0x409b00U
#define WORK_GR_CONTEXT_INSTANCE_MASK	0x0fffffffU

/* How many times a full space for the users' runs is told in the log. */
#define WORK_VA_FULL_MARKS		8U

/*
 * A clear of the VRAM by the copy engine: rows of 32768 words (128 KiB, a
 * fill's rows are kept below 2^17 words), the rest of the words in one row
 * after them, and how long the driver waits for it.
 */
#define WORK_CLEAR_LINE_WORDS		0x8000U
#define WORK_CLEAR_TIMEOUT_US		2000000U

/*
 * Where a clear's source pitch lies in its push buffer (the seventh word,
 * after the constant's, the components' and the destination's): a clear
 * broken on purpose has it 0, which the engine refuses in a multi-line
 * launch.
 */
#define WORK_CLEAR_PITCH_IN_AT		28U

/* The channel number on the graphics runlist (the golden context 1, the scrubber 2, the compute launch 3). */
#define WORK_CHID			4U

/*
 * A session's channel: the handles of the channel and its objects (each the
 * channel's place among the sessions' numbers after the base), its arena
 * (the instance block, USERD, then its own context buffers) and its region
 * of the space (its own context buffers from the start, then the ring, the
 * semaphore and the fences' push buffers from the next 64 KiB boundary).
 */
#define WORK_SESSION_HANDLE_CHANNEL	0xf1f00010U
#define WORK_SESSION_HANDLE_THREED	0x97000010U
#define WORK_SESSION_HANDLE_COMPUTE	0xc5c00010U
#define WORK_SESSION_HANDLE_COPY	0xc5b50010U
#define WORK_SESSION_ARENA_ALIGN	0x200000ULL
#define WORK_SESSION_INSTANCE_AT	0x0000ULL
#define WORK_SESSION_USERD_AT		0x1000ULL
#define WORK_SESSION_BUFFERS_AT		0x10000ULL
#define WORK_REGION_BYTES		0x200000ULL
#define WORK_REGION_PAGES_SHIFT		16U

/*
 * USERD's get and put of the ring, and the doorbell: runlist in bits 22:16,
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
#define WORK_USERD_RING_GET		0x88U
#define WORK_USERD_RING_PUT		0x8cU
#define WORK_REG_DOORBELL		0xbb0090U

/*
 * The promote's number of the unrestricted access map, which a channel
 * sharing the global buffers leaves out.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gr.h
 * (NV2080_CTRL_GPU_PROMOTE_CTX_BUFFER_ID_UNRESTRICTED_PRIV_ACCESS_MAP).
 */
#define WORK_BUFFER_UNRESTRICTED_MAP	11U

/* How long the first fence and a free ring entry are waited for, and how often the CPU looks. */
#define WORK_FIRST_FENCE_US		2000000U
#define WORK_ENTRY_WAIT_US		2000000U
#define WORK_POLL_US			50U

/*
 * How long a wait looks at the semaphore without sleeping before it sleeps
 * between looks: a sleep lasts at least a scheduler tick (1 ms), many
 * times the few tens of microseconds the GPU takes for most work, so the
 * work that ends soon is seen as it ends.
 */
#define WORK_SPIN_US			2000U

static int work_arena(struct nvrtx_gpu *gpu);
static int work_buffers(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static void work_buffers_free(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_tables(struct nvrtx_gpu *gpu);
static int work_space(struct nvrtx_gpu *gpu);
static int work_session_arena(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_session_region(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_channel(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_objects(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static void work_copy_object(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_first(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, bool wait);
static int work_channel_free(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel);
static int work_map_entries(struct nvrtx_gpu *gpu, uint64_t va, const uint8_t *entries, uint64_t pages);
static int work_entries_write(struct nvrtx_gpu *gpu, uint64_t va, const uint8_t *entries, uint64_t pages);
static int work_ring_wait(struct nvrtx_work_channel *channel, uint32_t entry);
static void work_ring_put(struct nvrtx_work_channel *channel, uint32_t entry, uint64_t va, size_t length);
static int work_ring_publish(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, uint32_t next);
static int work_doorbell(struct nvrtx_gpu *gpu, const struct nvrtx_work_channel *channel);
static uint32_t work_semaphore(const struct nvrtx_work_channel *channel);
static uint32_t work_semaphore_word(const struct nvrtx_work_channel *channel, size_t at);
static uint64_t work_semaphore_timer(const struct nvrtx_work_channel *channel, size_t at);
static size_t work_run_record(uint32_t fence);
static uint64_t work_align(uint64_t value, uint32_t shift);

/*
 * Runs work-channel: the arena, the space, the channel with both classes and
 * its first fence.
 *
 * Needs the golden context of golden-context and the VRAM allocator of
 * vram-window.  A failure is returned; the finish's stop frees whatever
 * objects were made.
 */
int
nvrtx_work_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_channel *driver;
	bool timed;
	int error;

	nvrtx_stage_mark("work-channel begin");

	/* The driver's channel is the one the work space holds. */
	driver = &gpu->work.driver;

	/* Each run is timed on the GPU when the boot asks for the counts (nvrtx.vk.perf=1). */
	timed = nvrtx_stage_vk_perf_wanted();
	gpu->work.timed = 0;
	if (timed)
		gpu->work.timed = 1;

	/* The driver's channel: its number, its handles and where its pages lie (the instance block and USERD follow from the arena). */
	driver->chid = WORK_CHID;
	driver->handle_channel = NVRTX_HANDLE_WORK_CHANNEL;
	driver->handle_threed = NVRTX_HANDLE_WORK_THREED;
	driver->handle_compute = NVRTX_HANDLE_WORK_COMPUTE;
	driver->handle_copy = NVRTX_HANDLE_WORK_COPY;
	driver->ring_va = WORK_RING_VA;
	driver->semaphore_va = WORK_SEMAPHORE_VA;
	driver->fences_va = WORK_FENCES_VA;
	driver->timed = gpu->work.timed;

	/* The channel's context comes from the golden context. */
	if (gpu->gr.buffer_count == 0U) {
		nvrtx_stage_mark("work-channel fail: no golden context");
		return EINVAL;
	}

	/* The arena comes from the VRAM allocator. */
	if (gpu->memory_state.ready == 0) {
		nvrtx_stage_mark("work-channel fail: no VRAM allocator (vram-window)");
		return ENODEV;
	}

	/* The arena, the system-memory buffers and the tables. */
	error = work_arena(gpu);
	if (error != 0)
		return error;

	error = work_buffers(gpu, driver);
	if (error != 0)
		return error;

	error = work_tables(gpu);
	if (error != 0)
		return error;

	/* The space and the channel with its two objects. */
	error = work_space(gpu);
	if (error != 0)
		return error;

	error = work_channel(gpu, driver);
	if (error != 0)
		return error;

	error = work_objects(gpu, driver);
	if (error != 0)
		return error;

	nvrtx_stage_mark("work-channel ok: work channel %u on runlist %u", (unsigned)WORK_CHID, (unsigned)gpu->engine.pick.graphics_runlist);

	/* The classes on their subchannels, and the first fence. */
	error = work_first(gpu, driver, true);
	if (error != 0)
		return error;

	/* The space takes mappings and channels, and the driver's channel takes work. */
	gpu->work.ready = 1;
	driver->state = NVRTX_WORK_CHANNEL_READY;

	/* Whether it has the copy engine (it clears the VRAM), and whether its first clear is broken on purpose. */
	gpu->work.copy_ready = driver->copy_made;
	gpu->work.clear_off = 0;
	if (nvrtx_stage_clear_cpu_wanted())
		gpu->work.clear_off = 1;
	gpu->work.clear_fault = 0;
	if (nvrtx_stage_clear_fault_wanted())
		gpu->work.clear_fault = 1;

	/* Reports the GPU's performance level, and raises it when the boot asks (nvrtx.boost=1). */
	nvrtx_perf_start(gpu);

	/* Succeeded: the channel takes work. */
	return 0;
}

/*
 * Undoes work-channel's objects before the graphics context's
 * stop: the channel off the scheduler, the 3D and compute objects, the
 * channel, the root, the space.  Failures are counted and shown; the
 * unload removes the rest.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_stop)
 * and Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/fifo/chan.c
 * (nvkm_chan_del).
 */
void
nvrtx_work_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_state *work;
	uint8_t params[NVRTX_UNPDIR_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int failures;
	int error;

	work = &gpu->work;

	/* Nothing is mapped or submitted from now on: the channel off the scheduler, its objects and the channel freed. */
	work->ready = 0;
	failures = work_channel_free(gpu, &work->driver);

	/* Takes the root back from the space, then frees it. */
	if (work->root_set != 0) {
		kern_memset(params, 0, sizeof(params));
		nvrtx_put32(params, 0U, NVRTX_HANDLE_WORK_SPACE);
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_DEVICE, NVRTX_CTL_UNSET_PAGE_DIRECTORY, params, sizeof(params), &reply, &reply_length);
		if (error != 0)
			failures++;
		work->root_set = 0;
	}

	if (work->space_made != 0) {
		error = nvrtx_rm_free(gpu, NVRTX_HANDLE_WORK_SPACE);
		if (error != 0)
			failures++;
		work->space_made = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d work frees failed; the unload removes the rest", failures);
}

/*
 * Gives back the work channel's system-memory buffers and its arena.
 *
 * Only once the GPU no longer reads them: after the GSP is stopped.
 */
void
nvrtx_work_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_state *work;

	work = &gpu->work;

	/* Frees each buffer that was allocated. */
	work_buffers_free(gpu, &work->driver);

	/* Returns the arena to the VRAM allocator. */
	if (work->arena_made != 0) {
		nvrtx_bo_free(gpu, &work->arena);
		work->arena_made = 0;
	}
}

/*
 * Reports the VRAM address of the work space's page tables' root, which
 * another channel in the same space names in its instance block.
 */
uint64_t
nvrtx_work_root(
	const struct nvrtx_gpu *gpu)
{
	/* Succeeded: the tables' first byte in the arena. */
	return gpu->work.arena.vram + WORK_TABLES_AT;
}

/*
 * Makes a session's own channel in the work space, numbered chid on the
 * graphics runlist (NVRTX_WORK_SESSION_CHID_FIRST onward): its arena and
 * system-memory buffers, its region of the space with its own context
 * buffers and pages mapped there, the channel with both classes, its
 * promote and its first fence.  channel is all zero before the call.
 *
 * A failure gives back whatever was made (or keeps it, when the resource
 * manager would not free it) and is returned: EINVAL for a number outside
 * the sessions' ones, ENODEV before the work channel runs, ENOSPC for a
 * full space or own buffers too large for a region, or the step's error.
 */
int
nvrtx_work_channel_open(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel,
	uint32_t chid)
{
	uint32_t number;
	int error;

	/* A number of the sessions', once the space runs. */
	if (chid < NVRTX_WORK_SESSION_CHID_FIRST || chid - NVRTX_WORK_SESSION_CHID_FIRST >= NVRTX_WORK_SESSION_CHANNELS)
		return EINVAL;
	if (gpu->work.ready == 0)
		return ENODEV;

	/* The channel's number, its handles, its lock and whether it times its runs. */
	number = chid - NVRTX_WORK_SESSION_CHID_FIRST;
	(void)mutex_init(&channel->lock, LOCK_RANK_DEVICE, "nvrtx session channel");
	channel->chid = chid;
	channel->handle_channel = WORK_SESSION_HANDLE_CHANNEL + number;
	channel->handle_threed = WORK_SESSION_HANDLE_THREED + number;
	channel->handle_compute = WORK_SESSION_HANDLE_COMPUTE + number;
	channel->handle_copy = WORK_SESSION_HANDLE_COPY + number;
	channel->timed = gpu->work.timed;

	/* Its arena, its buffers and its region of the space. */
	error = work_session_arena(gpu, channel);
	if (error == 0)
		error = work_buffers(gpu, channel);
	if (error == 0)
		error = work_session_region(gpu, channel);

	/* The channel with its context and both classes, and its first fence. */
	if (error == 0)
		error = work_channel(gpu, channel);
	if (error == 0)
		error = work_objects(gpu, channel);
	if (error == 0)
		error = work_first(gpu, channel, false);
	if (error != 0) {
		nvrtx_stage_mark("channel %u: no session channel (%d)", (unsigned)chid, error);
		(void)nvrtx_work_channel_close(gpu, channel);
		return error;
	}

	/* Succeeded: the channel takes the session's work. */
	nvrtx_stage_mark("channel %u: session channel on runlist %u", (unsigned)chid, (unsigned)gpu->engine.pick.graphics_runlist);
	return 0;
}

/*
 * Closes a session's channel: off the scheduler, its objects and the
 * channel freed where the resource manager still holds them, then its
 * region, buffers and arena given back.  The caller has made sure its
 * work has ended, or that the GPU no longer runs it.
 *
 * Returns EBUSY when a free or the region's unmap failed: the GPU may still
 * reach the channel's memory, so what is left stays the device's for good.
 */
int
nvrtx_work_channel_close(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	int failures;
	int error;

	/* The resource manager's objects of the channel. */
	failures = work_channel_free(gpu, channel);
	if (failures != 0) {
		nvrtx_stage_mark("channel %u: %d frees failed; its memory stays", (unsigned)channel->chid, failures);
		return EBUSY;
	}

	/* Its region of the space: the entries cleared, the addresses given back. */
	if (channel->region_made != 0) {
		error = nvrtx_work_unmap(gpu, channel->region, WORK_REGION_BYTES);
		if (error != 0) {
			nvrtx_stage_mark("channel %u: its region stayed mapped (%d); its memory stays", (unsigned)channel->chid, error);
			return EBUSY;
		}

		/* The region is given back. */
		channel->region_made = 0;
	}

	/* Its system-memory buffers, USERD's place in the window and its arena. */
	work_buffers_free(gpu, channel);
	nvrtx_bo_unmap(gpu, &channel->userd_window);
	if (channel->arena_made != 0) {
		nvrtx_bo_free(gpu, &channel->arena);
		channel->arena_made = 0;
	}

	/* Succeeded: nothing of the channel is left. */
	return 0;
}

/*
 * Looks at a channel for the recovery, without its lock (the GPU writes the
 * words it reads): whether it has an unfinished run, when its oldest
 * unfinished run was submitted, and whether that run began and at what time
 * of the GPU's.  The view's other fields are the caller's.
 */
void
nvrtx_work_channel_look(
	struct nvrtx_work_channel *channel,
	struct nvrtx_recover_view *view)
{
	uint32_t seen;
	uint32_t newest;
	uint32_t oldest;
	uint32_t started;
	size_t record;
	bool passed;

	/* Nothing is known of the channel until it shows an unfinished run. */
	view->busy = 0;
	view->oldest_submit_us = 0U;
	view->started = 0;
	view->start_ns = 0U;

	/* A channel with no run, or whose newest run has passed, waits for nothing. */
	newest = __atomic_load_n(&channel->fence, __ATOMIC_ACQUIRE);
	if (channel->ready == 0 || newest == 0U)
		return;
	seen = work_semaphore(channel);
	passed = nvrtx_work_fence_passed(seen, newest);
	if (passed)
		return;

	/* The oldest unfinished run follows the last one the semaphore released (fences skip 0). */
	oldest = seen + 1U;
	if (oldest == 0U)
		oldest = 1U;
	view->busy = 1;
	view->oldest_submit_us = __atomic_load_n(&channel->submitted_us[oldest & (NVRTX_WORK_RUN_RECORDS - 1U)], __ATOMIC_RELAXED);

	/* Whether its start stamp came, and the GPU's time then (read after the value it follows). */
	record = work_run_record(oldest);
	started = work_semaphore_word(channel, record);
	if (started != oldest)
		return;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	view->started = 1;
	view->start_ns = work_semaphore_timer(channel, record + WORK_STAMP_TIMER_AT);
}

/*
 * Reports whether the graphics engine holds the channel now: its current
 * context is the channel's instance block.  An engine that names no context
 * (0, or every bit set when it does not answer) holds none.
 */
bool
nvrtx_work_channel_resident(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_work_channel *channel)
{
	uint32_t current;
	uint32_t instance;

	/* The engine's current context. */
	current = nvrtx_rd32(gpu, WORK_REG_GR_CURRENT_CONTEXT);
	if (current == 0U || current == 0xffffffffU)
		return false;

	/* The channel's instance block in the same form. */
	instance = (uint32_t)(channel->instance >> 12) & WORK_GR_CONTEXT_INSTANCE_MASK;
	if ((current & WORK_GR_CONTEXT_INSTANCE_MASK) != instance)
		return false;

	/* Succeeded: the engine holds the channel. */
	return true;
}

/*
 * Stops a channel on the GPU at once: off its runlist, preempted without
 * waiting for it to idle (the resource manager raises an error report on
 * it when the preempt does not finish).  Returns 0 when the stop answered,
 * else the call's error.
 *
 * Procedure referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/src/kernel/gpu/fifo/kernel_channel.c
 * (kchannelCtrlCmdStopChannel_IMPL, sent on to the GSP).
 */
int
nvrtx_work_channel_stop(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	uint8_t params[4];
	const uint8_t *reply;
	size_t reply_length;
	size_t length;
	int error;

	/* Nothing is submitted to it from now on; what any run left in memory is not to be relied on. */
	channel->ready = 0;
	(void)nvrtx_gpu_submitted(gpu);

	/* The stop, at once. */
	kern_memset(params, 0, sizeof(params));
	length = nvrtx_recover_stop_params(params);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, channel->handle_channel, NVRTX_CTL_STOP_CHANNEL, params, length, &reply, &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the GPU no longer runs the channel. */
	return 0;
}

/*
 * Resets a channel and its graphics engine, saying whether an error report
 * on it came.  Returns 0 when the reset answered, else the call's error.
 *
 * Procedure referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/src/kernel/gpu/fifo/kernel_channel.c
 * (kchannelCtrlCmdResetChannel_IMPL, sent on to the GSP).
 */
int
nvrtx_work_channel_reset(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel,
	int reported)
{
	uint8_t params[16];
	const uint8_t *reply;
	size_t reply_length;
	size_t length;
	int error;

	/* The reset of the graphics engine, the default reason (counted as a submission: what runs left is not relied on). */
	(void)nvrtx_gpu_submitted(gpu);
	length = nvrtx_recover_reset_params(params, NVRTX_ENGINE_GRAPHICS, reported);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, channel->handle_channel, NVRTX_CTL_RESET_CHANNEL, params, length, &reply, &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the channel and its engine are reset. */
	return 0;
}

/*
 * Frees a stopped channel's objects and the channel itself in the resource
 * manager (as the close does), leaving its memory for the close.  Returns
 * how many of the calls failed.
 */
int
nvrtx_work_channel_free(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	int failures;

	/* Off the scheduler, the objects, the channel. */
	failures = work_channel_free(gpu, channel);

	/* Succeeded: the failures, counted. */
	return failures;
}

/*
 * Maps a run of VRAM into the work channel's space for its users.
 *
 * The run gets the next free addresses and VRAM entries that are neither
 * privileged nor read-only; stale translations are dropped before the
 * call returns.  Returns the address in *va, ENOSPC when the space is
 * full, EINVAL for a run not on page boundaries, or ENODEV before the
 * channel runs.
 */
int
nvrtx_work_map_vram(
	struct nvrtx_gpu *gpu,
	uint64_t physical,
	uint64_t size,
	uint64_t *va)
{
	struct nvrtx_work_state *work;
	uint8_t *entries;
	uint64_t pages;
	uint64_t address;
	uint64_t free_bytes;
	uint32_t runs;
	int full;
	int error;

	work = &gpu->work;

	/* Whole pages only. */
	if (size == 0U || (size & 0xfffU) != 0U || (physical & 0xfffU) != 0U)
		return EINVAL;

	/* Builds the run's entries. */
	pages = size / NVRTX_GSP_PAGE_SIZE;
	entries = kern_malloc((size_t)pages * 8U);
	if (entries == NULL)
		return ENOMEM;

	nvrtx_mmu_gr_entries_vram(entries, physical, pages);

	/* Takes addresses and writes the entries there. */
	mutex_lock(&work->lock);

	error = ENODEV;
	if (work->ready != 0)
		error = nvrtx_heap_alloc(&work->va, size, NVRTX_GSP_PAGE_SIZE, &address);
	if (error == 0) {
		error = work_entries_write(gpu, address, entries, pages);
		if (error != 0)
			(void)nvrtx_heap_free(&work->va, address, size);
	}

	/* A full space is told, the first few times, with what is left of it. */
	free_bytes = work->va.free_bytes;
	runs = work->va.count;
	full = 0;
	if (error == ENOSPC && work->va_full_marks < WORK_VA_FULL_MARKS) {
		work->va_full_marks++;
		full = 1;
	}

	mutex_unlock(&work->lock);

	kern_free(entries);

	if (full != 0)
		nvrtx_stage_mark("work space full: %llu KiB asked, %llu KiB free in %u runs",
		    (unsigned long long)(size >> 10), (unsigned long long)(free_bytes >> 10), (unsigned)runs);

	/* Reports a full space or entries that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the run is reachable at the address. */
	*va = address;
	return 0;
}

/*
 * Maps system-memory pages into the work channel's space for its users.
 *
 * The pages get the next free addresses in order, with coherent, volatile
 * entries that are not privileged.  Returns as nvrtx_work_map_vram() does.
 */
int
nvrtx_work_map_system(
	struct nvrtx_gpu *gpu,
	const uint64_t *pages,
	size_t count,
	uint64_t *va)
{
	struct nvrtx_work_state *work;
	uint8_t *entries;
	uint64_t size;
	uint64_t address;
	int error;

	work = &gpu->work;

	/* At least one page. */
	if (count == 0U)
		return EINVAL;

	/* Builds the pages' entries. */
	size = (uint64_t)count * NVRTX_GSP_PAGE_SIZE;
	entries = kern_malloc(count * 8U);
	if (entries == NULL)
		return ENOMEM;

	nvrtx_mmu_gr_entries_system(entries, pages, count);

	/* Takes addresses and writes the entries there. */
	mutex_lock(&work->lock);

	error = ENODEV;
	if (work->ready != 0)
		error = nvrtx_heap_alloc(&work->va, size, NVRTX_GSP_PAGE_SIZE, &address);
	if (error == 0) {
		error = work_entries_write(gpu, address, entries, count);
		if (error != 0)
			(void)nvrtx_heap_free(&work->va, address, size);
	}

	mutex_unlock(&work->lock);

	kern_free(entries);

	/* Reports a full space or entries that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the pages are reachable at the address. */
	*va = address;
	return 0;
}

/*
 * Unmaps a run mapped by one of the map calls and gives its addresses back.
 *
 * The caller has made sure no submitted work still reads the run (its
 * fence has passed).  The entries are cleared and stale translations
 * dropped before the addresses can be handed out again.  Returns EIO when
 * the entries could not be cleared; the addresses then stay taken.
 */
int
nvrtx_work_unmap(
	struct nvrtx_gpu *gpu,
	uint64_t va,
	uint64_t size)
{
	struct nvrtx_work_state *work;
	uint8_t *entries;
	uint64_t pages;
	int error;

	work = &gpu->work;

	/* Whole pages only. */
	if (size == 0U || (size & 0xfffU) != 0U || (va & 0xfffU) != 0U)
		return EINVAL;

	/* Invalid entries for the run. */
	pages = size / NVRTX_GSP_PAGE_SIZE;
	entries = kern_calloc((size_t)pages, 8U);
	if (entries == NULL)
		return ENOMEM;

	/* Clears the entries, then gives the addresses back. */
	mutex_lock(&work->lock);

	error = work_entries_write(gpu, va, entries, pages);
	if (error == 0)
		(void)nvrtx_heap_free(&work->va, va, size);

	mutex_unlock(&work->lock);

	kern_free(entries);

	/* Reports entries that could not be cleared. */
	if (error != 0)
		return EIO;

	/* Succeeded: the addresses are free again. */
	return 0;
}

/*
 * Writes the entries of a run already mapped for the users again, of a
 * memory kind (NVRTX_MMU_KIND_*): the same VRAM pages at the same
 * addresses, the kind in each entry; stale translations are dropped before
 * the call returns.  The caller makes sure no work uses the run meanwhile.
 * Returns EINVAL for a run not on page boundaries, ENOMEM, or the entries'
 * write's error.
 */
int
nvrtx_work_remap_vram(
	struct nvrtx_gpu *gpu,
	uint64_t va,
	uint64_t physical,
	uint64_t size,
	uint32_t kind)
{
	struct nvrtx_work_state *work;
	uint8_t *entries;
	uint64_t pages;
	int error;

	work = &gpu->work;

	/* Whole pages only. */
	if (size == 0U || (size & 0xfffU) != 0U || (physical & 0xfffU) != 0U || (va & 0xfffU) != 0U)
		return EINVAL;

	/* The run's entries of the kind. */
	pages = size / NVRTX_GSP_PAGE_SIZE;
	entries = kern_malloc((size_t)pages * 8U);
	if (entries == NULL)
		return ENOMEM;
	nvrtx_mmu_gr_entries_vram_kind(entries, physical, pages, kind);

	/* Written over the old ones under the space's lock. */
	mutex_lock(&work->lock);

	error = work_entries_write(gpu, va, entries, pages);

	mutex_unlock(&work->lock);

	kern_free(entries);

	/* Reports entries that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the run is of the kind. */
	return 0;
}

/*
 * Clears a run of the VRAM to zero with the copy engine on the driver's
 * channel and waits for it: a page of system memory holds the fill's push
 * buffer, the run and the page are mapped into the space for the fill and
 * unmapped after it.  Work submitted to the channel earlier ends first.
 *
 * Returns 0 once the run reads as zero.  ENODEV when the driver's channel
 * does not run or has no copy object, or the clears are off, EINVAL for a
 * run not on page boundaries, or the error of a step before the fill was
 * submitted: the run was not written, and may be cleared another way.
 * EBUSY when the fill was submitted but its fence did not come: the GPU
 * may still write the run, which must then stay taken for good.  A
 * submission or a fill that failed turns the clears off for good, so the
 * CPU clears from then on; the recovery stops the channel when the GPU
 * reports the fill's error.
 */
int
nvrtx_work_clear(
	struct nvrtx_gpu *gpu,
	uint64_t physical,
	uint64_t size)
{
	struct nvrtx_work_channel *channel;
	struct nvrtx_buffer push;
	uint64_t page;
	uint64_t push_va;
	uint64_t target;
	uint64_t words;
	uint64_t lines;
	uint32_t rest;
	uint32_t fence;
	uint8_t *bytes;
	size_t length;
	int unmapped;
	int error;

	channel = &gpu->work.driver;

	/* Whole pages, rows a fill can count, and a channel with a copy engine. */
	if (size == 0U || (size & 0xfffU) != 0U || (physical & 0xfffU) != 0U)
		return EINVAL;
	words = size / 4U;
	lines = words / WORK_CLEAR_LINE_WORDS;
	rest = (uint32_t)(words % WORK_CLEAR_LINE_WORDS);
	if (lines > 0xffffffffULL)
		return EINVAL;
	if (gpu->work.ready == 0 || channel->copy_made == 0 || __atomic_load_n(&gpu->work.clear_off, __ATOMIC_ACQUIRE) != 0)
		return ENODEV;
	if (__atomic_load_n(&channel->state, __ATOMIC_ACQUIRE) != NVRTX_WORK_CHANNEL_READY)
		return ENODEV;

	/* The push buffer's page, reachable from the space. */
	kern_memset(&push, 0, sizeof(push));
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &push);
	if (error != 0)
		return error;
	page = push.dma.device_address;
	error = nvrtx_work_map_system(gpu, &page, 1U, &push_va);
	if (error != 0) {
		nvrtx_buffer_free(gpu, &push);
		return error;
	}

	/* The run, at addresses of its own. */
	error = nvrtx_work_map_vram(gpu, physical, size, &target);
	if (error != 0) {
		unmapped = nvrtx_work_unmap(gpu, push_va, NVRTX_GSP_PAGE_SIZE);
		if (unmapped == 0)
			nvrtx_buffer_free(gpu, &push);
		return error;
	}

	/* The full rows, then the rest of the words in one row. */
	bytes = push.dma.address;
	length = 0U;
	if (lines != 0U)
		length = nvrtx_push_copy_clear(bytes, NVRTX_WORK_SUBCHANNEL_COPY, target, WORK_CLEAR_LINE_WORDS, (uint32_t)lines, 0U);
	if (rest != 0U) {
		length += nvrtx_push_copy_clear(bytes + length,
		    NVRTX_WORK_SUBCHANNEL_COPY,
		    target + lines * WORK_CLEAR_LINE_WORDS * 4U,
		    rest,
		    1U,
		    0U);
	}

	/* The first clear broken on purpose when the boot asks (its source pitch 0). */
	if (lines != 0U && __atomic_exchange_n(&gpu->work.clear_fault, 0, __ATOMIC_ACQ_REL) != 0) {
		nvrtx_put32(bytes, WORK_CLEAR_PITCH_IN_AT, 0U);
		nvrtx_stage_mark("vram clear: this clear is broken on purpose (nvrtx.clear.fault=1)");
	}

	/* Runs it after the channel's earlier work, and waits for its fence. */
	fence = 0U;
	error = nvrtx_work_channel_submit(gpu, channel, push_va, length, &fence);
	if (error == 0) {
		error = nvrtx_work_channel_wait(channel, fence, WORK_CLEAR_TIMEOUT_US);
		if (error != 0) {
			/* The GPU may still read the page and write the run: both stay as they are, and the CPU clears from now on. */
			__atomic_store_n(&gpu->work.clear_off, 1, __ATOMIC_RELEASE);
			nvrtx_stage_mark("vram clear: fence %u did not come (%d); %llu KiB stay taken, the CPU clears from now on",
			    (unsigned)fence,
			    error,
			    (unsigned long long)(size >> 10));
			return EBUSY;
		}
	}

	/* A submission that failed: the CPU clears from now on. */
	if (error != 0) {
		__atomic_store_n(&gpu->work.clear_off, 1, __ATOMIC_RELEASE);
		nvrtx_stage_mark("vram clear: not submitted (%d); the CPU clears from now on", error);
	}

	/* The run's and the page's addresses back; the page goes once nothing maps it. */
	(void)nvrtx_work_unmap(gpu, target, size);
	unmapped = nvrtx_work_unmap(gpu, push_va, NVRTX_GSP_PAGE_SIZE);
	if (unmapped == 0)
		nvrtx_buffer_free(gpu, &push);

	/* A submission that failed wrote nothing. */
	if (error != 0)
		return error;

	/* Succeeded: the run reads as zero. */
	return 0;
}

/*
 * Submits a push buffer segment to a channel of the work space, followed by
 * a fence of the driver's.
 *
 * The segment is length bytes at push_va in the work space; the GPU
 * reads it after the call returns, so it stays intact until the fence
 * returned in *fence has passed.  A start stamp goes before the segment
 * in the run's record (nvrtx_work_channel_look()), and a timed channel
 * (nvrtx.vk.perf=1) puts an end stamp after it, for
 * nvrtx_work_channel_run_time().  Waits up to 2 s for free ring
 * entries.  Returns ENODEV before the channel runs, ETIMEDOUT when the
 * ring stays full, EINVAL for an empty or misaligned segment.
 */
int
nvrtx_work_channel_submit(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel,
	uint64_t push_va,
	size_t length,
	uint32_t *fence)
{
	uint8_t *slot;
	uint64_t record_va;
	uint64_t submitted_us;
	uint32_t entries[4];
	uint32_t count;
	uint32_t index;
	uint32_t value;
	size_t slot_length;
	int error;

	/* Whole words, within what one ring entry carries. */
	if (length == 0U || (length & 3U) != 0U || length / 4U > NVRTX_WORK_SEGMENT_WORDS_MAX)
		return EINVAL;

	/* Puts the segment and the fence on the ring under the lock. */
	mutex_lock(&channel->lock);

	/* The channel must be running. */
	if (channel->ready == 0) {
		mutex_unlock(&channel->lock);
		return ENODEV;
	}

	/* The entries in order: the start stamp, the segment, a timed run's end stamp, the fence. */
	count = 3U;
	if (channel->timed != 0)
		count = 4U;
	for (index = 0U; index < count; index++)
		entries[index] = (channel->put + index) & (NVRTX_WORK_RING_ENTRIES - 1U);

	/* Waits until the entries' earlier work is done. */
	error = 0;
	for (index = 0U; index < count && error == 0; index++)
		error = work_ring_wait(channel, entries[index]);
	if (error != 0) {
		mutex_unlock(&channel->lock);
		return error;
	}

	/* The fence's value: the next one, never 0 (an entry of fence 0 is one never used). */
	value = channel->fence + 1U;
	if (value == 0U)
		value = 1U;

	/* The start stamp in its entry's slot, then the segment; a timed run's end stamp after it. */
	record_va = channel->semaphore_va + work_run_record(value);
	slot = (uint8_t *)channel->fences.dma.address + (size_t)entries[0] * NVRTX_WORK_FENCE_BYTES;
	slot_length = nvrtx_push_stamp(slot, record_va, value);
	work_ring_put(channel, entries[0], channel->fences_va + (uint64_t)entries[0] * NVRTX_WORK_FENCE_BYTES, slot_length);
	work_ring_put(channel, entries[1], push_va, length);
	if (channel->timed != 0) {
		slot = (uint8_t *)channel->fences.dma.address + (size_t)entries[2] * NVRTX_WORK_FENCE_BYTES;
		slot_length = nvrtx_push_fence_timed(slot, record_va + WORK_RUN_END_AT, value);
		work_ring_put(channel, entries[2], channel->fences_va + (uint64_t)entries[2] * NVRTX_WORK_FENCE_BYTES, slot_length);
	}

	/* The fence; then the put moved past them all at once. */
	slot = (uint8_t *)channel->fences.dma.address + (size_t)entries[count - 1U] * NVRTX_WORK_FENCE_BYTES;
	slot_length = nvrtx_push_fence(slot, channel->semaphore_va, value);
	work_ring_put(channel, entries[count - 1U], channel->fences_va + (uint64_t)entries[count - 1U] * NVRTX_WORK_FENCE_BYTES, slot_length);
	error = work_ring_publish(gpu, channel, (entries[count - 1U] + 1U) & (NVRTX_WORK_RING_ENTRIES - 1U));

	/* The doorbell, once every entry is on the ring; it counts as a submission. */
	if (error == 0)
		error = work_doorbell(gpu, channel);
	if (error == 0)
		(void)nvrtx_gpu_submitted(gpu);

	/* Gives up the submission, with the lock released. */
	if (error != 0) {
		mutex_unlock(&channel->lock);
		return error;
	}

	/*
	 * Every entry now belongs to this fence: they are written again only
	 * once it has passed.  fence is the newest value the semaphore will
	 * reach (read without the lock by nvrtx_work_channel_passed()).
	 */
	for (index = 0U; index < count; index++)
		channel->entry_fence[entries[index]] = value;

	/* The CPU's time of this submission, which the recovery reads to tell how long the run has waited. */
	submitted_us = nvrtx_now_us();
	__atomic_store_n(&channel->submitted_us[value & (NVRTX_WORK_RUN_RECORDS - 1U)], submitted_us, __ATOMIC_RELAXED);

	/* The newest fence is published after its time. */
	__atomic_store_n(&channel->fence, value, __ATOMIC_RELEASE);

	mutex_unlock(&channel->lock);

	/* Succeeded: the work runs and will release the fence. */
	*fence = value;
	return 0;
}

/*
 * Reports how long the GPU took for a timed run whose fence has passed:
 * the nanoseconds from its start stamp to its end stamp, by the GPU's
 * timer.
 *
 * Returns ENOENT when the channel is not timed or the run's record is
 * another run's (one WORK_RUN_RECORDS runs later wrote it), else 0.
 */
int
nvrtx_work_channel_run_time(
	struct nvrtx_work_channel *channel,
	uint32_t fence,
	uint64_t *gpu_ns)
{
	size_t record;
	uint32_t started;
	uint32_t ended;
	uint64_t start;
	uint64_t end;

	/* Only a timed channel leaves stamps. */
	if (channel->timed == 0)
		return ENOENT;

	/* Both values must be this run's; each was written after its timer. */
	record = work_run_record(fence);
	started = work_semaphore_word(channel, record);
	ended = work_semaphore_word(channel, record + WORK_RUN_END_AT);
	if (started != fence || ended != fence)
		return ENOENT;

	/* The timers are read only after the values that follow them. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	/* The timers at the start and at the end. */
	start = work_semaphore_timer(channel, record + WORK_STAMP_TIMER_AT);
	end = work_semaphore_timer(channel, record + WORK_RUN_END_AT + WORK_STAMP_TIMER_AT);

	/* A later run may have taken the record meanwhile. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	started = work_semaphore_word(channel, record);
	ended = work_semaphore_word(channel, record + WORK_RUN_END_AT);
	if (started != fence || ended != fence)
		return ENOENT;

	/* A run ended before its own start is not a time. */
	if (end < start)
		return ENOENT;

	/* Succeeded: the GPU's time for the run. */
	*gpu_ns = end - start;
	return 0;
}

/*
 * Reports whether the work before a fence is done: the semaphore has
 * reached it, or it is no fence of the submissions since the values last
 * wrapped (0, or ahead of the newest one submitted).  Any context, no lock.
 */
bool
nvrtx_work_channel_passed(
	struct nvrtx_work_channel *channel,
	uint32_t fence)
{
	uint32_t seen;
	uint32_t newest;
	bool passed;

	/* No fence, or one the semaphore has reached. */
	if (fence == 0U)
		return true;
	seen = work_semaphore(channel);
	passed = nvrtx_work_fence_passed(seen, fence);
	if (passed)
		return true;

	/* A fence ahead of the newest submission is from before the values wrapped: its work is long done. */
	newest = __atomic_load_n(&channel->fence, __ATOMIC_ACQUIRE);
	passed = nvrtx_work_fence_passed(newest, fence);
	if (!passed)
		return true;

	/* Its work is still running. */
	return false;
}

/*
 * Waits until a fence has passed, up to a time measured in microseconds.
 *
 * The semaphore lies in coherent system memory, so looking at it costs
 * the CPU and the bus nothing: the first 2 ms it is looked at without a
 * pause longer than the processor's spin hint, after that between sleeps.
 * Returns 0, or ETIMEDOUT when the semaphore did not reach it in time.
 */
int
nvrtx_work_channel_wait(
	struct nvrtx_work_channel *channel,
	uint32_t fence,
	uint32_t timeout_us)
{
	int error;

	/* Spins from the start. */
	error = nvrtx_work_channel_wait_late(channel, fence, timeout_us, 0U);

	/* Reports the wait's result. */
	return error;
}

/*
 * Waits as nvrtx_work_channel_wait() does for work the caller expects to take
 * about quiet_us more: until then the semaphore is looked at only between
 * sleeps, then without pause for 2 ms, then between sleeps again.  A
 * sleep lasts a scheduler tick, so work that ends while the wait spins is
 * seen as it ends without the processor spinning all along.
 */
int
nvrtx_work_channel_wait_late(
	struct nvrtx_work_channel *channel,
	uint32_t fence,
	uint32_t timeout_us,
	uint32_t quiet_us)
{
	uint64_t start;
	uint64_t spent;
	bool passed;

	/* Looks until the fence has passed or the time, measured, is spent. */
	start = nvrtx_now_us();
	for (;;) {
		passed = nvrtx_work_channel_passed(channel, fence);
		if (passed)
			break;

		spent = nvrtx_now_us() - start;
		if (spent > timeout_us)
			return ETIMEDOUT;

		/* Spins while the end is near; else it looks again after a sleep. */
		if (spent >= quiet_us && spent < (uint64_t)quiet_us + WORK_SPIN_US) {
			atomic_spin_hint();
		} else {
			kern_usleep_range(WORK_POLL_US, WORK_POLL_US * 2U);
		}
	}

	/* Succeeded: the work before the fence is done. */
	return 0;
}

/*
 * Takes the arena from the VRAM allocator and lays the channel's own
 * context buffers out in it; the global ones are the golden context's.
 */
static int
work_arena(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_state *work;
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t at;
	size_t index;
	int error;

	work = &gpu->work;
	gr = &gpu->gr;

	/* Takes the arena. */
	error = nvrtx_bo_alloc(gpu, WORK_ARENA_BYTES, WORK_ARENA_ALIGN, &work->arena);
	if (error != 0) {
		nvrtx_stage_mark("work-channel fail: no VRAM for the arena (%d)", error);
		return error;
	}

	work->arena_made = 1;
	work->driver.instance = work->arena.vram + WORK_INSTANCE_AT;
	work->driver.userd = work->arena.vram + WORK_USERD_AT;

	/* Each buffer: the golden context's when global, else one of the channel's own, at the golden context's address. */
	at = WORK_BUFFERS_AT;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		work->driver.virtual_address[index] = gr->virtual_address[index];
		if (buffer->global) {
			work->driver.physical[index] = gr->golden_physical[index];
			continue;
		}

		at = work_align(at, buffer->page_shift);
		work->driver.physical[index] = work->arena.vram + at;
		at += work_align(buffer->size, buffer->page_shift);
	}

	/* The channel's own buffers must fit the arena. */
	if (at > WORK_ARENA_BYTES) {
		nvrtx_stage_mark("work-channel fail: the context buffers need %llx of VRAM", (unsigned long long)at);
		return ENOSPC;
	}

	/* Succeeded: the arena is laid out. */
	nvrtx_stage_mark("work-channel arena at %llx, own buffers %llx bytes", (unsigned long long)work->arena.vram, (unsigned long long)(at - WORK_BUFFERS_AT));
	return 0;
}

/* Allocates a channel's ring, semaphore, fences' push buffers and method buffer. */
static int
work_buffers(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	int error;

	/* One page each for the ring and the semaphore. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_WORK_RING_ENTRIES * 8U, &channel->ring);
	if (error != 0)
		return error;

	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &channel->semaphore);
	if (error != 0)
		return error;

	/* One fence slot per ring entry. */
	error = nvrtx_buffer_alloc(gpu, WORK_FENCES_BYTES, &channel->fences);
	if (error != 0)
		return error;

	/*
	 * The method buffer, as large as the resource manager asked for
	 * copy-channel's.
	 */
	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &channel->methods);
	if (error != 0)
		return error;

	/* Succeeded: the buffers are clear. */
	return 0;
}

/* Gives a channel's system-memory buffers back (each one that was allocated). */
static void
work_buffers_free(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	/* The ring, the semaphore, the fences' push buffers and the method buffer. */
	nvrtx_buffer_free(gpu, &channel->ring);
	nvrtx_buffer_free(gpu, &channel->semaphore);
	nvrtx_buffer_free(gpu, &channel->fences);
	nvrtx_buffer_free(gpu, &channel->methods);
}

/*
 * Builds the space's tables and writes them into the arena: the context
 * buffers privileged at the golden context's addresses (the access map
 * read-only, the unrestricted one left out), the driver's pages after
 * them.  The rest of the range is free for the runs mapped for users.
 */
static int
work_tables(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_state *work;
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t pages[WORK_DRIVER_PAGES];
	uint64_t size;
	uint8_t *image;
	size_t length;
	size_t index;
	unsigned page;
	int error;

	work = &gpu->work;
	gr = &gpu->gr;

	/* The context buffers must lie below the driver's pages. */
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		size = work_align(buffer->size, 12U);
		if (gr->virtual_address[index] < NVRTX_WORK_VA_BASE || gr->virtual_address[index] + size > NVRTX_WORK_VA_DRIVER) {
			nvrtx_stage_mark("work-channel fail: context buffer %u at %llx is outside the space", (unsigned)index, (unsigned long long)gr->virtual_address[index]);
			return ERANGE;
		}
	}

	/* The driver's pages in order. */
	pages[0] = work->driver.ring.dma.device_address;
	pages[1] = work->driver.semaphore.dma.device_address;
	for (page = 0U; page < WORK_FENCES_BYTES / NVRTX_GSP_PAGE_SIZE; page++)
		pages[2U + page] = work->driver.fences.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;

	/* The directories in memory. */
	length = nvrtx_mmu_gr_tables_bytes(NVRTX_WORK_VA_BYTES);
	image = kern_malloc(length);
	if (image == NULL)
		return ENOMEM;

	error = nvrtx_mmu_gr_tables(image, work->arena.vram + WORK_TABLES_AT, NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES);

	/* Each context buffer the promote names by address. */
	for (index = 0U; error == 0 && index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (buffer->buffer_id == WORK_BUFFER_UNRESTRICTED_MAP)
			continue;
		error = nvrtx_mmu_gr_map(image,
		    NVRTX_WORK_VA_BASE,
		    NVRTX_WORK_VA_BYTES,
		    gr->virtual_address[index],
		    work->driver.physical[index],
		    work_align(buffer->size, 12U),
		    buffer->read_only);
	}

	/* The driver's pages. */
	if (error == 0)
		error = nvrtx_mmu_gr_map_system(image, NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_DRIVER, pages, WORK_DRIVER_PAGES);

	/* Copies the tables into the arena, and clears the instance block and USERD. */
	if (error == 0)
		error = nvrtx_vram_write(gpu, work->arena.vram + WORK_TABLES_AT, image, length);
	kern_free(image);
	if (error == 0)
		error = nvrtx_vram_zero(gpu, work->arena.vram + WORK_INSTANCE_AT, 2U * NVRTX_INSTANCE_BYTES);
	if (error != 0) {
		nvrtx_stage_mark("work-channel fail: the tables could not be written (%d)", error);
		return error;
	}

	/* Hands out the rest of the range to the runs mapped for users. */
	nvrtx_heap_init(&work->va, NVRTX_WORK_VA_USER, NVRTX_WORK_VA_BASE + NVRTX_WORK_VA_BYTES);

	/* Succeeded: the tables are in the VRAM. */
	nvrtx_stage_mark("work-channel page tables at %llx", (unsigned long long)(work->arena.vram + WORK_TABLES_AT));
	return 0;
}

/* Makes the address space the driver owns over the tables, gives it the root and drops stale translations. */
static int
work_space(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_work_state *work;
	uint8_t params[NVRTX_SPACE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint64_t root;
	int error;

	work = &gpu->work;
	root = work->arena.vram + WORK_TABLES_AT;

	/* The space, owned by the driver (as address-space's). */
	nvrtx_space_params(params);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVRTX_HANDLE_WORK_SPACE, NVRTX_CLASS_SPACE, params, sizeof(params));
	if (error != 0)
		return error;

	work->space_made = 1;

	/* The root. */
	nvrtx_pdir_params(params, root, NVRTX_HANDLE_WORK_SPACE);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_DEVICE, NVRTX_CTL_SET_PAGE_DIRECTORY, params, NVRTX_PDIR_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	work->root_set = 1;

	/* Drops any stale translation under the root. */
	error = nvrtx_flush_tlb(gpu, root);
	if (error != 0)
		return error;

	/* Succeeded: the space maps the range. */
	nvrtx_stage_mark("work-channel ok: vaspace");
	return 0;
}

/*
 * Takes a session's channel's arena from the VRAM allocator and lays out
 * its instance block, USERD and own context buffers in it; the global
 * buffers are the golden context's, reached where the space maps them.
 */
static int
work_session_arena(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t offset[NVRTX_GR_BUFFER_MAX];
	uint64_t at;
	size_t index;
	int error;

	gr = &gpu->gr;

	/* Each own buffer's place after the instance block and USERD, on its page size. */
	at = WORK_SESSION_BUFFERS_AT;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		offset[index] = 0U;
		if (buffer->global)
			continue;

		at = work_align(at, buffer->page_shift);
		offset[index] = at;
		at += work_align(buffer->size, buffer->page_shift);
	}

	/* The own buffers must fit the region they are mapped in, with the channel's pages after them. */
	if (at - WORK_SESSION_BUFFERS_AT + (uint64_t)WORK_DRIVER_PAGES * NVRTX_GSP_PAGE_SIZE + (1ULL << WORK_REGION_PAGES_SHIFT) > WORK_REGION_BYTES) {
		nvrtx_stage_mark("channel %u: the context buffers need %llx of a region", (unsigned)channel->chid, (unsigned long long)at);
		return ENOSPC;
	}

	/* Takes the arena. */
	error = nvrtx_bo_alloc(gpu, at, WORK_SESSION_ARENA_ALIGN, &channel->arena);
	if (error != 0)
		return error;

	channel->arena_made = 1;
	channel->instance = channel->arena.vram + WORK_SESSION_INSTANCE_AT;
	channel->userd = channel->arena.vram + WORK_SESSION_USERD_AT;

	/* Each buffer: the golden context's when global, else the channel's own in the arena. */
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (buffer->global) {
			channel->physical[index] = gr->golden_physical[index];
			channel->virtual_address[index] = gr->virtual_address[index];
			continue;
		}

		channel->physical[index] = channel->arena.vram + offset[index];
	}

	/* The instance block and USERD start clear. */
	error = nvrtx_vram_zero(gpu, channel->instance, 2U * NVRTX_INSTANCE_BYTES);
	if (error != 0)
		return error;

	/* USERD's page in the CPU's window, when it has room (else the ring's put goes through the register window). */
	kern_memset(&channel->userd_window, 0, sizeof(channel->userd_window));
	channel->userd_window.vram = channel->userd & ~((uint64_t)NVRTX_GSP_PAGE_SIZE - 1U);
	channel->userd_window.size = NVRTX_GSP_PAGE_SIZE;
	(void)nvrtx_bo_map(gpu, &channel->userd_window);

	/* Succeeded: the arena is laid out. */
	return 0;
}

/*
 * Takes a session's channel's region of the space and maps it: its own
 * context buffers privileged from the region's start (the access map
 * read-only, the unrestricted one left out, as the space's tables map the
 * golden context's), then its ring, semaphore and fences' push buffers.
 */
static int
work_session_region(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	struct nvrtx_work_state *work;
	struct nvrtx_gr_state *gr;
	struct nvrtx_gr_buffer *buffer;
	uint64_t pages[WORK_DRIVER_PAGES];
	uint64_t region;
	uint64_t offset;
	uint64_t size;
	uint64_t end;
	uint8_t *entries;
	size_t index;
	unsigned page;
	int error;

	work = &gpu->work;
	gr = &gpu->gr;

	/* Takes 2 MiB of the space on its own boundary. */
	mutex_lock(&work->lock);

	error = nvrtx_heap_alloc(&work->va, WORK_REGION_BYTES, WORK_REGION_BYTES, &region);

	mutex_unlock(&work->lock);

	/* Reports a full space. */
	if (error != 0)
		return error;

	/* The region is the channel's, and its close gives it back. */
	channel->region = region;
	channel->region_made = 1;

	/* Each own buffer at the place it has in the arena, counted from the region's start. */
	end = 0U;
	for (index = 0U; index < gr->buffer_count; index++) {
		buffer = &gr->buffers[index];
		if (buffer->global)
			continue;

		offset = channel->physical[index] - channel->arena.vram - WORK_SESSION_BUFFERS_AT;
		size = work_align(buffer->size, 12U);
		channel->virtual_address[index] = region + offset;
		if (offset + size > end)
			end = offset + size;
		if (buffer->buffer_id == WORK_BUFFER_UNRESTRICTED_MAP)
			continue;

		/* Its privileged entries. */
		entries = kern_malloc((size_t)(size / NVRTX_GSP_PAGE_SIZE) * 8U);
		if (entries == NULL)
			return ENOMEM;

		/* Writes them into the space's tables. */
		nvrtx_mmu_gr_entries_privileged(entries, channel->physical[index], size / NVRTX_GSP_PAGE_SIZE, buffer->read_only);
		error = work_map_entries(gpu, channel->virtual_address[index], entries, size / NVRTX_GSP_PAGE_SIZE);
		kern_free(entries);
		if (error != 0)
			return error;
	}

	/* The channel's pages after them, in order: the ring, the semaphore, the fences' push buffers. */
	offset = work_align(end, WORK_REGION_PAGES_SHIFT);
	channel->ring_va = region + offset;
	channel->semaphore_va = channel->ring_va + NVRTX_GSP_PAGE_SIZE;
	channel->fences_va = channel->ring_va + 2U * NVRTX_GSP_PAGE_SIZE;
	pages[0] = channel->ring.dma.device_address;
	pages[1] = channel->semaphore.dma.device_address;
	for (page = 0U; page < WORK_FENCES_BYTES / NVRTX_GSP_PAGE_SIZE; page++)
		pages[2U + page] = channel->fences.dma.device_address + (uint64_t)page * NVRTX_GSP_PAGE_SIZE;

	/* Their coherent entries. */
	entries = kern_malloc((size_t)WORK_DRIVER_PAGES * 8U);
	if (entries == NULL)
		return ENOMEM;

	/* Writes them into the space's tables. */
	nvrtx_mmu_gr_entries_system(entries, pages, WORK_DRIVER_PAGES);
	error = work_map_entries(gpu, channel->ring_va, entries, WORK_DRIVER_PAGES);
	kern_free(entries);
	if (error != 0)
		return error;

	/* Succeeded: the channel reaches its buffers and pages. */
	return 0;
}

/*
 * Makes the graphics channel: its instance block naming the tables, the
 * channel on the graphics engine and the promote of its context buffers.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_alloc).
 */
static int
work_channel(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	struct nvrtx_work_state *work;
	struct nvrtx_channel_setup setup;
	uint8_t *params;
	uint8_t *instance;
	int error;

	work = &gpu->work;

	/* The instance block names the space's tables. */
	instance = kern_calloc(1U, NVRTX_INSTANCE_BYTES);
	if (instance == NULL)
		return ENOMEM;

	nvrtx_mmu_instance(instance, work->arena.vram + WORK_TABLES_AT, WORK_VA_LIMIT);
	error = nvrtx_vram_write(gpu, channel->instance, instance, NVRTX_INSTANCE_BYTES);
	kern_free(instance);
	if (error != 0)
		return error;

	/* The channel on the graphics engine, its ring at the first of its pages. */
	setup.ring_va = channel->ring_va;
	setup.ring_entries = NVRTX_WORK_RING_ENTRIES;
	setup.chid = channel->chid;
	setup.space = NVRTX_HANDLE_WORK_SPACE;
	setup.engine = NVRTX_ENGINE_GRAPHICS;
	setup.privileged = 0U;
	setup.instance = channel->instance;
	setup.userd = channel->userd;
	setup.methods = channel->methods.dma.device_address;
	setup.methods_size = gpu->engine.methods_size;

	params = kern_malloc(NVRTX_CHANNEL_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, channel->handle_channel, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	kern_free(params);
	if (error != 0)
		return error;

	channel->channel_made = 1;

	/* Hands it its context buffers where it reaches them: its own main and patch, the golden context's global ones. */
	error = nvrtx_gr_promote(gpu, channel->handle_channel, channel->physical, channel->virtual_address, false);
	if (error != 0)
		return error;

	/* Succeeded: the channel exists with its context. */
	return 0;
}

/* Makes the 3D and compute objects on the channel, binds it to the graphics engine and lets the scheduler run it. */
static int
work_objects(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	uint8_t params[NVRTX_BIND_PARAMS_BYTES + NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* The 3D object. */
	error = nvrtx_rm_allocate(gpu, channel->handle_channel, channel->handle_threed, gpu->identity.chip.profile->class_threed, NULL, 0U);
	if (error != 0)
		return error;

	channel->threed_made = 1;

	/* The compute object. */
	error = nvrtx_rm_allocate(gpu, channel->handle_channel, channel->handle_compute, gpu->identity.chip.profile->class_compute, NULL, 0U);
	if (error != 0)
		return error;

	channel->compute_made = 1;

	/* The copy object on the copy engine of the graphics runlist, with the video decoder only. */
	work_copy_object(gpu, channel);

	/* Binds the channel to the graphics engine. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, 0U, NVRTX_ENGINE_GRAPHICS);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, channel->handle_channel, NVRTX_CTL_BIND, params, NVRTX_BIND_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	/* Lets the scheduler run it. */
	kern_memset(params, 0, sizeof(params));
	params[0] = 1U;
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, channel->handle_channel, NVRTX_CTL_SCHEDULE, params, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the stop takes the channel off the scheduler before it frees it. */
	channel->scheduled = 1;
	return 0;
}

/*
 * Makes the copy object on a channel: on the driver's channel always, on
 * which the VRAM a blob takes is cleared (nvrtx_work_clear()), and on a
 * session's channel when the boot asks for the video decoder
 * (nvrtx.video=1).  It is of the class of the copy engine that shares the
 * graphics runlist, on which the executor's copies out of block-linear
 * video images run.  A copy object the resource manager refuses leaves the
 * channel without one (the VRAM is then cleared through the register
 * window, and a session has no way out of a video image to a buffer).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/class/clb0b5sw.h
 * (NVB0B5_ALLOCATION_PARAMETERS: version, engineType).
 */
static void
work_copy_object(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	uint8_t params[NVRTX_COPY_PARAMS_BYTES];
	uint32_t engine;
	bool wanted;
	int error;

	/* On the driver's channel always (it clears the VRAM), on a session's only with the video decoder. */
	wanted = nvrtx_stage_video_wanted();
	if (!wanted && channel != &gpu->work.driver)
		return;

	/* A copy engine must share the graphics runlist. */
	engine = gpu->engine.pick.graphics_copy;
	if (engine == 0xffffffffU) {
		nvrtx_stage_mark("channel %u: no copy engine on the graphics runlist; no copy object", (unsigned)channel->chid);
		return;
	}

	/* The object, version 1, on that copy engine. */
	kern_memset(params, 0, sizeof(params));
	nvrtx_put32(params, 0U, 1U);
	nvrtx_put32(params, 4U, engine);
	error = nvrtx_rm_allocate(gpu, channel->handle_channel, channel->handle_copy, gpu->identity.chip.profile->class_copy, params, sizeof(params));
	if (error != 0) {
		nvrtx_stage_mark("channel %u: the copy object on engine %x was refused (%d)", (unsigned)channel->chid, (unsigned)engine, error);
		return;
	}

	/* The free takes it before the compute object. */
	channel->copy_made = 1;
	nvrtx_stage_mark("channel %u: copy object on engine %x", (unsigned)channel->chid, (unsigned)engine);
}

/*
 * Runs the channel's first submission: both classes (and the copy class,
 * when the channel has its object) on their subchannels, then its fence,
 * waited for when wait is set (the driver's channel at the
 * start; a session's channel is made while other channels may hold the
 * graphics engine, and its runs follow the submission on the ring).
 */
static int
work_first(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel,
	bool wait)
{
	uint8_t *push;
	uint64_t push_va;
	uint64_t start;
	uint64_t waited;
	size_t length;
	uint32_t fence;
	uint32_t semaphore;
	int error;

	/*
	 * The objects' push buffer goes in the last fence slot, which the
	 * ring reaches only after NVRTX_WORK_RING_ENTRIES - 1 more entries,
	 * long after this submission's fence.
	 */
	push = (uint8_t *)channel->fences.dma.address + (size_t)(NVRTX_WORK_RING_ENTRIES - 1U) * NVRTX_WORK_FENCE_BYTES;
	push_va = channel->fences_va + (uint64_t)(NVRTX_WORK_RING_ENTRIES - 1U) * NVRTX_WORK_FENCE_BYTES;
	length = nvrtx_work_push_objects(push, gpu->identity.chip.profile->class_threed, gpu->identity.chip.profile->class_compute);

	/* The copy engine's object follows when the channel has a copy engine. */
	if (channel->copy_made != 0)
		length += nvrtx_work_push_copy_object(push + length, gpu->identity.chip.profile->class_copy);

	/* Opens the channel to submissions; the semaphore starts at 0. */
	kern_memset(channel->semaphore.dma.address, 0, NVRTX_GSP_PAGE_SIZE);
	channel->put = 0U;
	channel->fence = 0U;
	kern_memset(channel->entry_fence, 0, sizeof(channel->entry_fence));
	channel->ready = 1;

	/* Submits it; the last ring entry, whose fence slot holds it, is written again only after its fence. */
	start = nvrtx_now_us();
	fence = 0U;
	error = nvrtx_work_channel_submit(gpu, channel, push_va, length, &fence);
	if (error == 0)
		channel->entry_fence[NVRTX_WORK_RING_ENTRIES - 1U] = fence;

	/* Waits for its fence when asked (a session's channel's later runs follow it on the ring instead). */
	if (error == 0 && wait)
		error = nvrtx_work_channel_wait(channel, fence, WORK_FIRST_FENCE_US);
	if (error != 0) {
		channel->ready = 0;

		/* Says how far the semaphore got. */
		semaphore = work_semaphore(channel);
		nvrtx_stage_mark("work-channel fail: channel %u's first fence %u did not come (%d, semaphore %08x)",
		    (unsigned)channel->chid,
		    (unsigned)fence,
		    error,
		    (unsigned)semaphore);
		return error;
	}

	/* A first fence not waited for is not told. */
	if (!wait)
		return 0;

	/* Succeeded: the channel ran its first work. */
	waited = nvrtx_now_us() - start;
	nvrtx_stage_mark("work-channel ok: channel %u fence %u after %u us", (unsigned)channel->chid, (unsigned)fence, (unsigned)waited);
	return 0;
}

/*
 * Takes a channel off the scheduler and frees its 3D, copy and compute
 * objects and the channel itself, each that exists; nothing is submitted to it
 * afterwards.  Returns how many of the resource manager's calls failed
 * (an object whose free failed is taken as gone: the unload removes it).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_stop)
 * and Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/fifo/chan.c
 * (nvkm_chan_del).
 */
static int
work_channel_free(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel)
{
	uint8_t params[NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int failures;
	int error;

	/* Counts the resource manager's calls that fail. */
	failures = 0;

	/* Nothing is submitted from now on. */
	channel->ready = 0;

	/* Takes the channel off the scheduler. */
	if (channel->scheduled != 0) {
		kern_memset(params, 0, sizeof(params));
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, channel->handle_channel, NVRTX_CTL_SCHEDULE, params, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
		if (error != 0)
			failures++;
		channel->scheduled = 0;
	}

	/* Frees the 3D object, then the copy object, then the compute object. */
	if (channel->threed_made != 0) {
		error = nvrtx_rm_free(gpu, channel->handle_threed);
		if (error != 0)
			failures++;
		channel->threed_made = 0;
	}

	/* The copy object. */
	if (channel->copy_made != 0) {
		error = nvrtx_rm_free(gpu, channel->handle_copy);
		if (error != 0)
			failures++;
		channel->copy_made = 0;
	}

	/* The compute object. */
	if (channel->compute_made != 0) {
		error = nvrtx_rm_free(gpu, channel->handle_compute);
		if (error != 0)
			failures++;
		channel->compute_made = 0;
	}

	/* Frees the channel. */
	if (channel->channel_made != 0) {
		error = nvrtx_rm_free(gpu, channel->handle_channel);
		if (error != 0)
			failures++;
		channel->channel_made = 0;
	}

	/* Succeeded: the failures, counted. */
	return failures;
}

/* Writes a run of page entries into the space's tables under the space's lock and drops the stale translations. */
static int
work_map_entries(
	struct nvrtx_gpu *gpu,
	uint64_t va,
	const uint8_t *entries,
	uint64_t pages)
{
	int error;

	/* The entries, under the lock of the space's mappings. */
	mutex_lock(&gpu->work.lock);

	error = work_entries_write(gpu, va, entries, pages);

	mutex_unlock(&gpu->work.lock);

	/* Reports entries that could not be written. */
	if (error != 0)
		return error;

	/* Succeeded: the run maps as written. */
	return 0;
}

/* Writes a run of page entries into the tables and drops the stale translations. */
static int
work_entries_write(
	struct nvrtx_gpu *gpu,
	uint64_t va,
	const uint8_t *entries,
	uint64_t pages)
{
	struct nvrtx_work_state *work;
	size_t offset;
	int error;

	work = &gpu->work;

	/* The run's first entry; the rest follow it (the tables lie in page order). */
	offset = nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, va);
	if (offset == 0U)
		return EINVAL;

	/* Writes the entries. */
	error = nvrtx_vram_write(gpu, work->arena.vram + WORK_TABLES_AT + offset, entries, (size_t)pages * 8U);
	if (error != 0)
		return error;

	/* Drops what the GPU may have kept of the old entries. */
	error = nvrtx_flush_tlb(gpu, work->arena.vram + WORK_TABLES_AT);
	if (error != 0)
		return error;

	/* Succeeded: the run maps as written. */
	return 0;
}

/* Waits until a ring entry's earlier submission is done, so the entry and its fence slot may be written again. */
static int
work_ring_wait(
	struct nvrtx_work_channel *channel,
	uint32_t entry)
{
	uint32_t fence;
	int error;

	/* An entry not used yet is free. */
	fence = channel->entry_fence[entry];
	if (fence == 0U)
		return 0;

	/* Waits for the fence that ended its submission. */
	error = nvrtx_work_channel_wait(channel, fence, WORK_ENTRY_WAIT_US);
	if (error != 0)
		return error;

	/* Succeeded: the entry is free. */
	return 0;
}

/* Writes one ring entry; the GPU takes it once the put in USERD moves past it. */
static void
work_ring_put(
	struct nvrtx_work_channel *channel,
	uint32_t entry,
	uint64_t va,
	size_t length)
{
	/* The entry. */
	nvrtx_ring_entry((uint8_t *)channel->ring.dma.address + (size_t)entry * 8U, va, (uint32_t)(length / 4U));
}

/*
 * Moves the ring's put in USERD to next, after the entries before it are
 * written; one write for all of a submission's entries: a store through
 * the CPU's window when USERD's page is there, else through the register
 * window (two register reads and three writes).
 *
 * Procedure referenced: NVIDIA open-gpu-kernel-modules 570.144
 * kernel-open/nvidia-uvm/uvm_channel.c (uvm_channel_ring_doorbell: the put
 * written to the mapped USERD, a write barrier, then the doorbell).
 */
static int
work_ring_publish(
	struct nvrtx_gpu *gpu,
	struct nvrtx_work_channel *channel,
	uint32_t next)
{
	volatile uint8_t *userd;
	uint8_t put[4];
	int error;

	/* The entries, written before the put that hands them to the GPU. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The ring's put in USERD, by the window when it is mapped. */
	if (channel->userd_window.bar1_mapped != 0) {
		userd = nvrtx_bo_cpu(gpu, &channel->userd_window) + (channel->userd - channel->userd_window.vram);
		*(volatile uint32_t *)(volatile void *)(userd + WORK_USERD_RING_PUT) = next;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		channel->put = next;
		return 0;
	}
	nvrtx_put32(put, 0U, next);
	error = nvrtx_vram_write(gpu, channel->userd + WORK_USERD_RING_PUT, put, sizeof(put));
	if (error != 0)
		return error;

	/* The next submission starts after these entries. */
	channel->put = next;

	/* Succeeded: the entries are the GPU's once the doorbell rings. */
	return 0;
}

/* Rings the channel's doorbell: the graphics runlist and the channel. */
static int
work_doorbell(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_work_channel *channel)
{
	/* The GPU fetches the entries up to the put. */
	nvrtx_wr32(gpu, WORK_REG_DOORBELL, (gpu->engine.pick.graphics_runlist << 16) | channel->chid);

	/* Succeeded: the doorbell was written. */
	return 0;
}

/* Reads the value the fences released last. */
static uint32_t
work_semaphore(
	const struct nvrtx_work_channel *channel)
{
	const volatile uint32_t *semaphore;

	/* The semaphore's first word in the coherent page. */
	semaphore = (const volatile uint32_t *)channel->semaphore.dma.address;

	/* Succeeded: what the GPU last released. */
	return *semaphore;
}

/* Reads a word a release wrote into the semaphore's page at a byte offset. */
static uint32_t
work_semaphore_word(
	const struct nvrtx_work_channel *channel,
	size_t at)
{
	const volatile uint32_t *word;

	/* The word in the coherent page. */
	word = (const volatile uint32_t *)((const uint8_t *)channel->semaphore.dma.address + at);

	/* Succeeded: what the GPU last wrote there. */
	return *word;
}

/* Reports where a timed run's record lies in the semaphore's page: by its fence's low bits. */
static size_t
work_run_record(
	uint32_t fence)
{
	/* The record the fence's value picks among the last runs'. */
	return WORK_RUNS_AT + (size_t)(fence & (WORK_RUN_RECORDS - 1U)) * WORK_RUN_RECORD_BYTES;
}

/* Reads a 64-bit GPU timer a timed release wrote into the semaphore's page at a byte offset. */
static uint64_t
work_semaphore_timer(
	const struct nvrtx_work_channel *channel,
	size_t at)
{
	const volatile uint32_t *words;
	uint64_t low;
	uint64_t high;

	/* The timer's two words in the coherent page, low first. */
	words = (const volatile uint32_t *)((const uint8_t *)channel->semaphore.dma.address + at);
	low = words[0];
	high = words[1];

	/* Succeeded: the timer in nanoseconds. */
	return (high << 32) | low;
}

/* Rounds a value up to a power-of-two boundary given as its log 2. */
static uint64_t
work_align(
	uint64_t value,
	uint32_t shift)
{
	uint64_t mask;

	/* The boundary's mask. */
	mask = (1ULL << shift) - 1U;

	/* Succeeded: the rounded value. */
	return (value + mask) & ~mask;
}
