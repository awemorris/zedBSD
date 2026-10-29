/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video decoder's channel (nvdec-channel, nvrtx.video=1).
 *
 * After the work channel the driver makes a channel on the first video
 * decoder's runlist in the work channel's address space, so the memory the
 * executor maps for its users is at the same address for the decoder:
 *
 *   1. the decoder's runlist and engine descriptor from the device
 *      information table, and its engine context's size from the
 *      resource manager's list of the falcons it made;
 *   2. a VRAM block for the instance block (naming the work space's
 *      tables) and USERD, and one for the engine context, mapped into the
 *      space; a system-memory page for the ring, the semaphore and the
 *      driver's push buffers, mapped too, and the method buffer;
 *   3. the channel, its bind to the decoder and its schedule, the promote
 *      of the engine context, and the decoder's object;
 *   4. a first submission that binds the class to its subchannel, and its
 *      fence.
 *
 * Afterwards nvrtx_nvdec_run() puts a push buffer segment on the channel
 * followed by a fence and waits for it: the executor decodes one picture
 * at a time and waits for it.  The decoder's firmware is the GSP's; the
 * driver loads none.  The stop undoes 3 before the work channel's stop,
 * the release gives the memory back after the GSP's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"
#include "drivers/gpu/nvrtx/nvrtx-nvdec.h"

/*
 * The channel's handles: the channel in the channel family with its
 * number, the decoder's object in the decoder's family.
 */
#define NVDEC_HANDLE_CHANNEL		0xf1f00030U
#define NVDEC_HANDLE_OBJECT		0xc4b00030U

/*
 * The channel's number, after the driver's channels (the graphics ones 1
 * to 4, the copy channel 8) and the sessions' (NVRTX_WORK_SESSION_CHID_FIRST
 * onward).
 */
#define NVDEC_CHID			(NVRTX_WORK_SESSION_CHID_FIRST + NVRTX_WORK_SESSION_CHANNELS)

/* The VRAM block: the instance block, then USERD; its size and alignment. */
#define NVDEC_BLOCK_BYTES		0x10000ULL
#define NVDEC_INSTANCE_AT		0x0000ULL
#define NVDEC_USERD_AT			0x1000ULL

/* The engine context's alignment, and its size rounded to whole pages. */
#define NVDEC_CONTEXT_ALIGN		0x1000ULL

/*
 * The system-memory page: the ring's entries, the semaphore, the fence's
 * push buffer and the first submission's push buffer.
 */
#define NVDEC_RING_AT			0x000U
#define NVDEC_RING_ENTRIES		64U
#define NVDEC_SEMAPHORE_AT		0x800U
#define NVDEC_FENCE_AT			0xc00U
#define NVDEC_OBJECT_AT			0xe00U

/* The address-space limit written into the instance block (47-bit virtual addresses). */
#define NVDEC_VA_LIMIT			(1ULL << 47)

/*
 * USERD's put of the ring, and the doorbell: runlist in bits 22:16,
 * channel in bits 11:0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (Nvc46fControl_struct's GPPut) and NVIDIA open-gpu-kernel-modules
 * 570.144 src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_DOORBELL).
 */
#define NVDEC_USERD_RING_PUT		0x8cU
#define NVDEC_REG_DOORBELL		0xbb0090U

/* How long the first fence is waited for, and how often the CPU looks. */
#define NVDEC_FIRST_FENCE_US		2000000U
#define NVDEC_POLL_US			50U

/* How long a wait looks at the semaphore without sleeping before it sleeps between looks. */
#define NVDEC_SPIN_US			2000U

/*
 * The decoder's channel: what was made (so the stop and the release free
 * only that), where its pieces are, and the ring's put and the newest
 * fence.  lock serializes the submissions; ready is set once the first
 * fence has passed and cleared by the stop.
 */
struct nvrtx_nvdec_state {
	struct mutex lock;
	uint32_t nvdec_class;
	uint32_t runlist;
	uint32_t engine_descriptor;
	uint32_t context_bytes;
	struct nvrtx_bo block;
	struct nvrtx_bo context;
	struct nvrtx_buffer page;
	struct nvrtx_buffer methods;
	uint64_t page_va;
	uint64_t context_va;
	uint32_t put;
	uint32_t fence;
	int block_made;
	int context_made;
	int page_mapped;
	int context_mapped;
	int channel_made;
	int scheduled;
	int object_made;
	int ready;
};

static int nvdec_find(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_memory(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_channel(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_context(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_object(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_first(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec);
static int nvdec_submit(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec, uint64_t push_va, size_t length, uint32_t *fence);
static int nvdec_ring_put(struct nvrtx_gpu *gpu, struct nvrtx_nvdec_state *nvdec, uint64_t va, size_t length);
static int nvdec_wait(struct nvrtx_nvdec_state *nvdec, uint32_t fence, uint32_t timeout_us);
static uint32_t nvdec_semaphore(const struct nvrtx_nvdec_state *nvdec);
static uint64_t nvdec_round(uint64_t value, uint64_t boundary);

/*
 * Runs nvdec-channel: finds the decoder, makes its memory, its channel,
 * its context and its object, and runs the first fence.
 *
 * Needs the work channel.  A failure is returned and leaves the GPU
 * without a decoder; the stop and the release free whatever was made.
 */
int
nvrtx_nvdec_start(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_nvdec_state *nvdec;
	int error;

	/* Marks the start of the bring-up in the stage log. */
	nvrtx_stage_mark("nvdec-channel begin");

	/* The decoder's channel lives in the work channel's space. */
	if (gpu->work.ready == 0) {
		nvrtx_stage_mark("nvdec-channel fail: no work channel");
		return ENODEV;
	}

	/* The state, kept until the release. */
	nvdec = kern_calloc(1U, sizeof(*nvdec));
	if (nvdec == NULL)
		return ENOMEM;

	/* Starts the state's lock and publishes the state, so a failed bring-up can still be released. */
	(void)mutex_init(&nvdec->lock, LOCK_RANK_DEVICE, "nvrtx nvdec");
	gpu->nvdec = nvdec;

	/* The decoder's class, runlist and context size. */
	error = nvdec_find(gpu, nvdec);
	if (error != 0)
		return error;

	/* Makes the memory the channel needs. */
	error = nvdec_memory(gpu, nvdec);
	if (error != 0)
		return error;

	/* Makes the channel on the decoder's runlist. */
	error = nvdec_channel(gpu, nvdec);
	if (error != 0)
		return error;

	/* Promotes the engine context to the channel. */
	error = nvdec_context(gpu, nvdec);
	if (error != 0)
		return error;

	/* Allocates the decoder object on the channel. */
	error = nvdec_object(gpu, nvdec);
	if (error != 0)
		return error;

	/* The class on its subchannel, and the first fence. */
	error = nvdec_first(gpu, nvdec);
	if (error != 0)
		return error;

	/* Succeeded: the decoder takes pictures. */
	nvrtx_stage_mark("nvdec-channel ok: class %x, channel %u on runlist %u, context %u bytes",
	    (unsigned)nvdec->nvdec_class, (unsigned)NVDEC_CHID, (unsigned)nvdec->runlist, (unsigned)nvdec->context_bytes);
	return 0;
}

/*
 * Undoes nvdec-channel's objects before the work channel's stop: the
 * channel off the scheduler, the decoder's object, the channel, and the
 * page's and the context's mappings in the work space.  Failures
 * are counted and shown; the unload removes the rest.
 */
void
nvrtx_nvdec_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_nvdec_state *nvdec;
	uint8_t params[NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int failures;
	int error;

	/* Starts the teardown with no failure counted. */
	nvdec = gpu->nvdec;
	failures = 0;

	/* Nothing was made without the state. */
	if (nvdec == NULL)
		return;

	/* Nothing is submitted from now on. */
	mutex_lock(&nvdec->lock);

	nvdec->ready = 0;

	mutex_unlock(&nvdec->lock);

	/* Takes the channel off the scheduler. */
	if (nvdec->scheduled != 0) {
		kern_memset(params, 0, sizeof(params));
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVDEC_HANDLE_CHANNEL, NVRTX_CTL_SCHEDULE, params, sizeof(params), &reply, &reply_length);
		if (error != 0)
			failures++;
		nvdec->scheduled = 0;
	}

	/* Frees the decoder's object, then the channel. */
	if (nvdec->object_made != 0) {
		error = nvrtx_rm_free(gpu, NVDEC_HANDLE_OBJECT);
		if (error != 0)
			failures++;
		nvdec->object_made = 0;
	}

	/* Frees the channel from the resource manager. */
	if (nvdec->channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVDEC_HANDLE_CHANNEL);
		if (error != 0)
			failures++;
		nvdec->channel_made = 0;
	}

	/* Gives the page's address in the work space back. */
	if (nvdec->page_mapped != 0) {
		error = nvrtx_work_unmap(gpu, nvdec->page_va, NVRTX_GSP_PAGE_SIZE);
		if (error != 0)
			failures++;
		nvdec->page_mapped = 0;
	}

	/* Unmaps the engine context from the work space. */
	if (nvdec->context_mapped != 0) {
		error = nvrtx_work_unmap(gpu, nvdec->context_va, nvdec->context.size);
		if (error != 0)
			failures++;
		nvdec->context_mapped = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d decoder frees failed; the unload removes the rest", failures);
}

/*
 * Gives back the decoder's memory and its state.
 *
 * Only once the GPU no longer reads it: after the GSP is stopped (the
 * work space's mappings go with the work channel's arena).
 */
void
nvrtx_nvdec_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_nvdec_state *nvdec;

	/* Takes the decoder's state, which may not exist. */
	nvdec = gpu->nvdec;

	/* Nothing was made without the state. */
	if (nvdec == NULL)
		return;

	/* The system memory. */
	nvrtx_buffer_free(gpu, &nvdec->page);
	nvrtx_buffer_free(gpu, &nvdec->methods);

	/* The VRAM blocks. */
	if (nvdec->context_made != 0) {
		nvrtx_bo_free(gpu, &nvdec->context);
		nvdec->context_made = 0;
	}

	/* Frees the instance block. */
	if (nvdec->block_made != 0) {
		nvrtx_bo_free(gpu, &nvdec->block);
		nvdec->block_made = 0;
	}

	/* The state. */
	gpu->nvdec = NULL;
	kern_free(nvdec);
}

/*
 * Reports whether the GPU has a decoder that takes pictures.  Any context,
 * no lock (a stop may follow at once; nvrtx_nvdec_run() refuses then).
 */
bool
nvrtx_nvdec_ready(
	const struct nvrtx_gpu *gpu)
{
	const struct nvrtx_nvdec_state *nvdec;
	int ready;

	/* Takes the decoder's state, which may not exist. */
	nvdec = gpu->nvdec;

	/* No state, no decoder. */
	if (nvdec == NULL)
		return false;

	/* The channel's readiness. */
	ready = __atomic_load_n(&nvdec->ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return false;

	/* Succeeded: the decoder runs. */
	return true;
}

/* Reports the decoder's class (0 without a decoder). */
uint32_t
nvrtx_nvdec_class_of(
	const struct nvrtx_gpu *gpu)
{
	/* No state, no class. */
	if (gpu->nvdec == NULL)
		return 0U;

	/* Succeeded: the class the channel's object has. */
	return gpu->nvdec->nvdec_class;
}

/*
 * Runs one push buffer segment on the decoder and waits for it.
 *
 * The segment is length bytes at push_va in the work channel's space.
 * Returns ENODEV without a running decoder, EINVAL for an empty or
 * misaligned segment, ETIMEDOUT when the decoder did not finish within
 * timeout_us (the channel is then taken out of use: a later picture would
 * queue behind the one that hangs).
 */
int
nvrtx_nvdec_run(
	struct nvrtx_gpu *gpu,
	uint64_t push_va,
	size_t length,
	uint32_t timeout_us)
{
	struct nvrtx_nvdec_state *nvdec;
	uint32_t fence;
	uint32_t seen;
	int error;

	/* Takes the decoder's state, which may not exist. */
	nvdec = gpu->nvdec;

	/* No state, no decoder. */
	if (nvdec == NULL)
		return ENODEV;

	/* Whole words of one ring entry. */
	if (length == 0U ||
	    (length & 3U) != 0U ||
	    length / 4U > NVRTX_WORK_SEGMENT_WORDS_MAX)
		return EINVAL;

	/* Submits the segment and its fence, then waits for the fence, one picture at a time. */
	mutex_lock(&nvdec->lock);

	error = ENODEV;

	/* Submits only to a decoder that has not been taken out of use. */
	if (nvdec->ready != 0)
		error = nvdec_submit(gpu, nvdec, push_va, length, &fence);

	/* Waits for the segment's fence. */
	if (error == 0)
		error = nvdec_wait(nvdec, fence, timeout_us);

	/* A decoder that did not finish is taken out of use, so no later picture queues behind it. */
	if (error == ETIMEDOUT)
		__atomic_store_n(&nvdec->ready, 0, __ATOMIC_RELEASE);

	mutex_unlock(&nvdec->lock);

	/* Reports a decoder that is not running or did not finish. */
	if (error == ETIMEDOUT) {
		seen = nvdec_semaphore(nvdec);
		nvrtx_stage_mark("nvdec: a picture did not finish (fence %u, semaphore %08x); the decoder is off", (unsigned)fence,
		    (unsigned)seen);
	}

	/* Propagates the failure of the segment. */
	if (error != 0)
		return error;

	/* Succeeded: the decoder has finished the segment. */
	return 0;
}

/*
 * Finds the decoder: its class for the GPU's family, its runlist and
 * engine descriptor in the device information table the first channel
 * read, and its engine context's size.
 */
static int
nvdec_find(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* The family's class. */
	nvdec->nvdec_class = nvrtx_nvdec_class(gpu->identity.chip.family);
	if (nvdec->nvdec_class == 0U) {
		nvrtx_stage_mark("nvdec-channel fail: no decoder class for this family");
		return ENODEV;
	}

	/* The decoder's runlist and descriptor. */
	if (gpu->engine.pick.nvdec_found == 0U) {
		nvrtx_stage_mark("nvdec-channel fail: no decoder in the device information table");
		return ENODEV;
	}

	/* Keeps the runlist and the descriptor the table gave. */
	nvdec->runlist = gpu->engine.pick.nvdec_runlist;
	nvdec->engine_descriptor = gpu->engine.pick.nvdec_descriptor;

	/* Allocates the parameters of the falcon query. */
	params = kern_calloc(1U, NVRTX_FALCON_INFO_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Asks the resource manager for the falcons it made, and their contexts' sizes. */
	error = nvrtx_rm_control(gpu,
	    gpu->internal_client,
	    gpu->internal_subdevice,
	    NVRTX_CTL_FALCON_INFO,
	    params,
	    NVRTX_FALCON_INFO_PARAMS_BYTES,
	    &reply,
	    &reply_length);
	kern_free(params);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the falcons' list was refused (%d)", error);
		return error;
	}

	/* The decoder's context size; a decoder needs one. */
	error = nvrtx_nvdec_context_bytes(reply, reply_length, nvdec->engine_descriptor, &nvdec->context_bytes);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: engine %x is not among the falcons (%d)", (unsigned)nvdec->engine_descriptor, error);
		return error;
	}

	/* Refuses an engine whose context has no size. */
	if (nvdec->context_bytes == 0U) {
		nvrtx_stage_mark("nvdec-channel fail: engine %x has no context size", (unsigned)nvdec->engine_descriptor);
		return ENODEV;
	}

	/* The decoder found, by the table's entry. */
	nvrtx_stage_mark("nvdec-channel: decoder engine %x on runlist %u", (unsigned)nvdec->engine_descriptor, (unsigned)nvdec->runlist);

	/* Succeeded: the decoder is known. */
	return 0;
}

/*
 * Makes the channel's memory: the VRAM block of the instance block and
 * USERD, the engine context (cleared and mapped into the space), the
 * system-memory page (mapped into the space) and the method buffer.
 */
static int
nvdec_memory(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	uint8_t *instance;
	uint64_t context_size;
	uint64_t page;
	uint64_t root;
	int error;

	/* The VRAM block of the instance block and USERD, cleared. */
	error = nvrtx_bo_alloc(gpu, NVDEC_BLOCK_BYTES, NVDEC_BLOCK_BYTES, &nvdec->block);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: no VRAM for the instance block (%d)", error);
		return error;
	}

	/* Marks the block made, so the release frees it, then zeroes it. */
	nvdec->block_made = 1;
	error = nvrtx_vram_zero(gpu, nvdec->block.vram, (size_t)NVDEC_BLOCK_BYTES);
	if (error != 0)
		return error;

	/* The instance block names the work channel's tables. */
	instance = kern_calloc(1U, NVRTX_INSTANCE_BYTES);
	if (instance == NULL)
		return ENOMEM;

	/* Builds the instance block from the work channel's root and writes it. */
	root = nvrtx_work_root(gpu);
	nvrtx_mmu_instance(instance, root, NVDEC_VA_LIMIT);
	error = nvrtx_vram_write(gpu, nvdec->block.vram + NVDEC_INSTANCE_AT, instance, NVRTX_INSTANCE_BYTES);
	kern_free(instance);
	if (error != 0)
		return error;

	/* The engine context, cleared, in the space. */
	if (nvdec->context_bytes != 0U) {
		context_size = nvdec_round(nvdec->context_bytes, NVDEC_CONTEXT_ALIGN);
		error = nvrtx_bo_alloc(gpu, context_size, NVDEC_CONTEXT_ALIGN, &nvdec->context);
		if (error != 0) {
			nvrtx_stage_mark("nvdec-channel fail: no VRAM for the engine context (%d)", error);
			return error;
		}

		/* Marks the context made, so the release frees it, then zeroes it. */
		nvdec->context_made = 1;
		error = nvrtx_vram_zero(gpu, nvdec->context.vram, (size_t)context_size);
		if (error != 0)
			return error;

		/* Maps the context into the work space. */
		error = nvrtx_work_map_vram(gpu, nvdec->context.vram, context_size, &nvdec->context_va);
		if (error != 0)
			return error;

		/* Marks the context mapped, so the release unmaps it. */
		nvdec->context_mapped = 1;
	}

	/* The page of the ring, the semaphore and the push buffers, in the space. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &nvdec->page);
	if (error != 0)
		return error;

	/* Maps the page into the work space. */
	page = nvdec->page.dma.device_address;
	error = nvrtx_work_map_system(gpu, &page, 1U, &nvdec->page_va);
	if (error != 0)
		return error;

	/* Marks the page mapped, so the release unmaps it. */
	nvdec->page_mapped = 1;

	/* The method buffer, as large as the resource manager asked for the first channel's. */
	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &nvdec->methods);
	if (error != 0)
		return error;

	/* Succeeded: the memory is in place. */
	return 0;
}

/*
 * Makes the channel on the decoder's engine in the work channel's space,
 * binds it to the decoder and lets the scheduler run it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_alloc,
 * then the bind and the schedule before the engine's context).
 */
static int
nvdec_channel(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	struct nvrtx_channel_setup setup;
	uint8_t *params;
	uint8_t control[NVRTX_BIND_PARAMS_BYTES + NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* The channel on the decoder's engine, its ring at the start of the page. */
	setup.ring_va = nvdec->page_va + NVDEC_RING_AT;
	setup.ring_entries = NVDEC_RING_ENTRIES;
	setup.chid = NVDEC_CHID;
	setup.space = NVRTX_HANDLE_WORK_SPACE;
	setup.engine = NVRTX_ENGINE_NVDEC0;
	setup.privileged = 0U;
	setup.instance = nvdec->block.vram + NVDEC_INSTANCE_AT;
	setup.userd = nvdec->block.vram + NVDEC_USERD_AT;
	setup.methods = nvdec->methods.dma.device_address;
	setup.methods_size = gpu->engine.methods_size;

	/* Allocates the channel's parameters. */
	params = kern_malloc(NVRTX_CHANNEL_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Describes the channel to the resource manager and allocates it. */
	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVDEC_HANDLE_CHANNEL, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	kern_free(params);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the channel was refused (%d)", error);
		return error;
	}

	/* Marks the channel made, so the release frees it. */
	nvdec->channel_made = 1;

	/* Binds it to the decoder. */
	kern_memset(control, 0, sizeof(control));
	nvrtx_put32(control, 0U, NVRTX_ENGINE_NVDEC0);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVDEC_HANDLE_CHANNEL, NVRTX_CTL_BIND, control, NVRTX_BIND_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the bind was refused (%d)", error);
		return error;
	}

	/* Lets the scheduler run it. */
	kern_memset(control, 0, sizeof(control));
	control[0] = 1U;
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVDEC_HANDLE_CHANNEL, NVRTX_CTL_SCHEDULE, control, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the schedule was refused (%d)", error);
		return error;
	}

	/* Succeeded: the stop takes the channel off the scheduler before it frees it. */
	nvdec->scheduled = 1;
	return 0;
}

/* Hands the channel its engine context by its address in the space (a decoder without one is left as it is). */
static int
nvdec_context(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* No context, nothing to promote. */
	if (nvdec->context_made == 0)
		return 0;

	/* The promote of the context to the subdevice. */
	params = kern_malloc(NVRTX_NVDEC_PROMOTE_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Fills the promote's parameters and sends them. */
	nvrtx_nvdec_promote_params(params, NVRTX_HANDLE_CLIENT, NVDEC_CHID, NVDEC_HANDLE_CHANNEL, nvdec->context_va, nvdec->context.size);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_SUBDEVICE, NVRTX_CTL_PROMOTE_CONTEXT, params, NVRTX_NVDEC_PROMOTE_BYTES, &reply, &reply_length);
	kern_free(params);
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the context's promote was refused (%d)", error);
		return error;
	}

	/* Succeeded: the channel has its context. */
	return 0;
}

/* Makes the decoder's object on the channel. */
static int
nvdec_object(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	uint8_t params[NVRTX_NVDEC_ALLOC_PARAMS_BYTES];
	int error;

	/* The object of the family's class on the first decoder. */
	nvrtx_nvdec_alloc_params(params);
	error = nvrtx_rm_allocate(gpu, NVDEC_HANDLE_CHANNEL, NVDEC_HANDLE_OBJECT, nvdec->nvdec_class, params, sizeof(params));
	if (error != 0) {
		nvrtx_stage_mark("nvdec-channel fail: the decoder's object was refused (%d)", error);
		return error;
	}

	/* Succeeded: the stop frees the object before its channel. */
	nvdec->object_made = 1;
	return 0;
}

/* Runs the channel's first submission: the class on its subchannel, then its fence. */
static int
nvdec_first(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec)
{
	uint8_t *push;
	uint32_t fence;
	uint32_t seen;
	size_t length;
	int error;

	/* The object's push buffer in its place of the page. */
	push = (uint8_t *)nvdec->page.dma.address + NVDEC_OBJECT_AT;
	length = nvrtx_nvdec_push_object(push, nvdec->nvdec_class);

	/* Opens the channel; the semaphore starts at 0. */
	kern_memset((uint8_t *)nvdec->page.dma.address + NVDEC_SEMAPHORE_AT, 0, 16U);
	nvdec->put = 0U;
	nvdec->fence = 0U;

	/* Submits it and waits for its fence. */
	mutex_lock(&nvdec->lock);

	fence = 0U;
	error = nvdec_submit(gpu, nvdec, nvdec->page_va + NVDEC_OBJECT_AT, length, &fence);

	/* Waits for the fence once the submission took. */
	if (error == 0)
		error = nvdec_wait(nvdec, fence, NVDEC_FIRST_FENCE_US);

	/* Opens the channel to pictures once the first fence has passed. */
	if (error == 0)
		__atomic_store_n(&nvdec->ready, 1, __ATOMIC_RELEASE);

	mutex_unlock(&nvdec->lock);

	/* Reports a first fence that did not come. */
	if (error != 0) {
		seen = nvdec_semaphore(nvdec);
		nvrtx_stage_mark("nvdec-channel fail: the first fence %u did not come (%d, semaphore %08x)", (unsigned)fence, error,
		    (unsigned)seen);
		return error;
	}

	/* Succeeded: the channel ran its first work. */
	return 0;
}

/*
 * Puts a segment and a fence after it on the ring and rings the doorbell.
 * The caller holds the lock and waits for the fence before the next
 * submission, so the entries and the fence's push buffer are free.
 */
static int
nvdec_submit(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec,
	uint64_t push_va,
	size_t length,
	uint32_t *fence)
{
	uint8_t *fence_push;
	size_t fence_length;
	uint32_t value;
	int error;

	/* The fence's value: the next one, never 0. */
	value = nvdec->fence + 1U;
	if (value == 0U)
		value = 1U;

	/* The fence's push buffer: a release of the value once the decoder is idle. */
	fence_push = (uint8_t *)nvdec->page.dma.address + NVDEC_FENCE_AT;
	fence_length = nvrtx_push_fence(fence_push, nvdec->page_va + NVDEC_SEMAPHORE_AT, value);

	/* The segment, then the fence, then the doorbell: the decoder's runlist and the channel. */
	error = nvdec_ring_put(gpu, nvdec, push_va, length);

	/* The fence follows the segment on the ring. */
	if (error == 0)
		error = nvdec_ring_put(gpu, nvdec, nvdec->page_va + NVDEC_FENCE_AT, fence_length);

	/* Refuses to ring the doorbell for a ring that did not take its entries. */
	if (error != 0)
		return error;

	/* Rings the doorbell, so the engine reads the ring. */
	nvrtx_wr32(gpu, NVDEC_REG_DOORBELL, (nvdec->runlist << 16) | NVDEC_CHID);
	(void)nvrtx_gpu_submitted(gpu);

	/* Succeeded: the semaphore reaches the value once the segment is done. */
	nvdec->fence = value;
	*fence = value;
	return 0;
}

/* Writes one ring entry and moves the put in USERD past it. */
static int
nvdec_ring_put(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvdec_state *nvdec,
	uint64_t va,
	size_t length)
{
	uint8_t put[4];
	uint8_t *entry;
	uint32_t next;
	int error;

	/* The entry, written before the put that hands it to the GPU. */
	entry = (uint8_t *)nvdec->page.dma.address + NVDEC_RING_AT + (size_t)nvdec->put * 8U;
	nvrtx_ring_entry(entry, va, (uint32_t)(length / 4U));
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The ring's put in USERD, wrapping at the ring's end. */
	next = (nvdec->put + 1U) % NVDEC_RING_ENTRIES;
	nvrtx_put32(put, 0U, next);
	error = nvrtx_vram_write(gpu, nvdec->block.vram + NVDEC_USERD_AT + NVDEC_USERD_RING_PUT, put, sizeof(put));
	if (error != 0)
		return error;

	/* Succeeded: the next entry is the one after it. */
	nvdec->put = next;
	return 0;
}

/*
 * Waits until the semaphore has reached a fence, up to a time measured in
 * microseconds: the first 2 ms without a pause, then between sleeps.
 */
static int
nvdec_wait(
	struct nvrtx_nvdec_state *nvdec,
	uint32_t fence,
	uint32_t timeout_us)
{
	uint64_t start;
	uint64_t spent;
	uint32_t seen;
	bool passed;

	/* Looks until the fence has passed or the time, measured, is spent. */
	start = nvrtx_now_us();
	for (;;) {
		seen = nvdec_semaphore(nvdec);
		passed = nvrtx_work_fence_passed(seen, fence);
		if (passed)
			break;

		/* Gives up on a segment that has taken longer than the caller allows. */
		spent = nvrtx_now_us() - start;
		if (spent > timeout_us)
			return ETIMEDOUT;

		/* Spins at first; else it looks again after a sleep. */
		if (spent < NVDEC_SPIN_US) {
			atomic_spin_hint();
		} else {
			kern_usleep_range(NVDEC_POLL_US, NVDEC_POLL_US * 2U);
		}
	}

	/* Succeeded: the work before the fence is done. */
	return 0;
}

/* Reads the value the decoder's fences released last. */
static uint32_t
nvdec_semaphore(
	const struct nvrtx_nvdec_state *nvdec)
{
	const volatile uint32_t *semaphore;

	/* The semaphore's word in the coherent page. */
	semaphore = (const volatile uint32_t *)((const uint8_t *)nvdec->page.dma.address + NVDEC_SEMAPHORE_AT);

	/* Succeeded: what the decoder last released. */
	return *semaphore;
}

/* Rounds a value up to a boundary that is a power of two. */
static uint64_t
nvdec_round(
	uint64_t value,
	uint64_t boundary)
{
	/* Succeeded: the rounded value. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}
