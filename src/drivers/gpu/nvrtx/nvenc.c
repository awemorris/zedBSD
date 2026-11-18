/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video encoder's channel (nvenc-channel, nvrtx.video=1).
 *
 * After the work channel (and the video decoder's channel, which it does
 * not need) the driver makes a channel on the first video encoder's
 * runlist in the work channel's address space, so the memory the executor
 * maps for its users is at the same address for the encoder:
 *
 *   1. the encoder's runlist and engine descriptor from the device
 *      information table, and its engine context's size from the
 *      resource manager's list of the falcons it made;
 *   2. a VRAM block for the instance block (naming the work space's
 *      tables) and USERD, and one for the engine context, mapped into the
 *      space; a system-memory page for the ring, the semaphores and the
 *      driver's push buffers, mapped too, and the method buffer;
 *   3. the channel, its bind to the encoder and its schedule, the promote
 *      of the engine context, and the encoder's object;
 *   4. a first submission that binds the class to its subchannel and
 *      gives the encoder a no-operation, and its fence; then a second one
 *      in which the encoder releases a semaphore itself, and its fence:
 *      the encoder's firmware (the GSP's; the driver loads none) has then
 *      taken methods and written memory.
 *
 * The channel errors the GSP reports during 4 are read before the
 * bring-up ends and counted in the stage log.  Afterwards
 * nvrtx_nvenc_run() puts a push buffer segment on the channel followed by
 * a fence and waits for it.  The stop undoes 3 before the work channel's
 * stop, the release gives the memory back after the GSP's.
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
#include "drivers/gpu/nvrtx/nvrtx-nvenc.h"

/*
 * The channel's handles: the channel in the channel family with its
 * number, the encoder's object in the encoder's family.
 */
#define NVENC_HANDLE_CHANNEL		0xf1f00031U
#define NVENC_HANDLE_OBJECT		0xc4b70031U

/*
 * The channel's number, after the driver's channels (the graphics ones 1
 * to 4, the copy channel 8), the sessions' (NVRTX_WORK_SESSION_CHID_FIRST
 * onward) and the video decoder's (the one after the sessions').
 */
#define NVENC_CHID			(NVRTX_WORK_SESSION_CHID_FIRST + NVRTX_WORK_SESSION_CHANNELS + 1U)

/* The VRAM block: the instance block, then USERD; its size and alignment. */
#define NVENC_BLOCK_BYTES		0x10000ULL
#define NVENC_INSTANCE_AT		0x0000ULL
#define NVENC_USERD_AT			0x1000ULL

/* The engine context's alignment, and its size rounded to whole pages. */
#define NVENC_CONTEXT_ALIGN		0x1000ULL

/*
 * The system-memory page: the ring's entries, the fences' semaphore, the
 * semaphore the encoder releases itself, the fence's push buffer, the
 * first submission's push buffer and the second's.
 */
#define NVENC_RING_AT			0x000U
#define NVENC_RING_ENTRIES		64U
#define NVENC_SEMAPHORE_AT		0x800U
#define NVENC_RELEASE_SEMAPHORE_AT	0x810U
#define NVENC_FENCE_AT			0xc00U
#define NVENC_OBJECT_AT			0xe00U
#define NVENC_RELEASE_AT		0xe80U

/* The value the encoder releases in the bring-up's second submission. */
#define NVENC_RELEASE_VALUE		0x4e56454eU

/* The address-space limit written into the instance block (47-bit virtual addresses). */
#define NVENC_VA_LIMIT			(1ULL << 47)

/*
 * USERD's put of the ring, and the doorbell: runlist in bits 22:16,
 * channel in bits 11:0.
 *
 * Values referenced: NVIDIA open-gpu-doc classes/host/clc46f.h
 * (Nvc46fControl_struct's GPPut) and NVIDIA open-gpu-kernel-modules
 * 570.144 src/common/inc/swref/published/turing/tu102/dev_vm.h
 * (NV_VIRTUAL_FUNCTION_DOORBELL).
 */
#define NVENC_USERD_RING_PUT		0x8cU
#define NVENC_REG_DOORBELL		0xbb0090U

/* How long each fence of the bring-up is waited for, and how often the CPU looks. */
#define NVENC_FIRST_FENCE_US		2000000U
#define NVENC_POLL_US			50U

/* How long a wait looks at the semaphore without sleeping before it sleeps between looks. */
#define NVENC_SPIN_US			2000U

/* How long the bring-up reads the GSP's messages for the channel errors it may report. */
#define NVENC_NOTICE_US			200000U

/* The function number no message has, so a read of the messages takes them all until the time is spent. */
#define NVENC_NO_FUNCTION		0xffffffffU

/*
 * The encoder's channel: what was made (so the stop and the release free
 * only that), where its pieces are, and the ring's put and the newest
 * fence.  lock serializes the submissions; ready is set once the
 * bring-up's submissions have passed and cleared by the stop or a
 * submission that did not finish.
 */
struct nvrtx_nvenc_state {
	struct mutex lock;
	uint32_t nvenc_class;
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
	int privileged;
};

static int nvenc_start(struct nvrtx_gpu *gpu, int privileged);
static int nvenc_find(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_memory(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_channel(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_context(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_object(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_first(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc);
static int nvenc_once(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc, uint32_t push_at, size_t length, const char *what);
static int nvenc_submit(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc, uint64_t push_va, size_t length, uint32_t *fence);
static int nvenc_ring_put(struct nvrtx_gpu *gpu, struct nvrtx_nvenc_state *nvenc, uint64_t va, size_t length);
static int nvenc_wait(struct nvrtx_nvenc_state *nvenc, uint32_t fence, uint32_t timeout_us, const volatile uint32_t *watch, uint32_t sentinel, uint64_t *changed_us);
static uint32_t nvenc_word(const struct nvrtx_nvenc_state *nvenc, uint32_t at);
static uint64_t nvenc_round(uint64_t value, uint64_t boundary);

/*
 * Runs nvenc-channel: finds the encoder, makes its memory, its channel,
 * its context and its object, runs the two first submissions and counts
 * the channel errors the GSP reported meanwhile.
 *
 * Needs the work channel.  A failure is returned and leaves the GPU
 * without an encoder; the stop and the release free whatever was made.
 */
int
nvrtx_nvenc_start(
	struct nvrtx_gpu *gpu)
{
	int error;

	/* The encoder's channel with the user's privilege. */
	error = nvenc_start(gpu, 0);
	if (error != 0)
		return error;

	/* Succeeded: the encoder takes work. */
	return 0;
}

/*
 * Runs nvenc-channel with the channel's privilege (nonzero: an admin
 * channel, as the resource manager's own are).
 */
static int
nvenc_start(
	struct nvrtx_gpu *gpu,
	int privileged)
{
	struct nvrtx_nvenc_state *nvenc;
	uint32_t before;
	int error;

	/* Marks the start of the bring-up in the stage log. */
	nvrtx_stage_mark("nvenc-channel begin");

	/* The encoder's channel lives in the work channel's space. */
	if (gpu->work.ready == 0) {
		nvrtx_stage_mark("nvenc-channel fail: no work channel");
		return ENODEV;
	}

	/* The state, kept until the release. */
	nvenc = kern_calloc(1U, sizeof(*nvenc));
	if (nvenc == NULL)
		return ENOMEM;

	/* Starts the state's lock and publishes the state, so a failed bring-up can still be released. */
	(void)mutex_init(&nvenc->lock, LOCK_RANK_DEVICE, "nvrtx nvenc");
	nvenc->privileged = privileged;
	gpu->nvenc = nvenc;

	/* The channel errors reported before the bring-up are not its own. */
	before = nvrtx_nvenc_notices(gpu);

	/* The encoder's class, runlist and context size. */
	error = nvenc_find(gpu, nvenc);

	/* Makes the memory the channel needs. */
	if (error == 0)
		error = nvenc_memory(gpu, nvenc);

	/* Makes the channel on the encoder's runlist. */
	if (error == 0)
		error = nvenc_channel(gpu, nvenc);

	/* Promotes the engine context to the channel. */
	if (error == 0)
		error = nvenc_context(gpu, nvenc);

	/* Allocates the encoder object on the channel. */
	if (error == 0)
		error = nvenc_object(gpu, nvenc);

	/* The class on its subchannel, the encoder's own release, and their fences. */
	if (error == 0)
		error = nvenc_first(gpu, nvenc);

	/* Reads what the GSP sent meanwhile and counts the channel errors, whether the bring-up passed or not. */
	nvrtx_nvenc_report_notices(gpu, before, "nvenc-channel");

	/* Propagates the failure of a step. */
	if (error != 0)
		return error;

	/* Succeeded: the encoder takes work. */
	nvrtx_stage_mark("nvenc-channel ok: class %x, channel %u on runlist %u%s", (unsigned)nvenc->nvenc_class, (unsigned)NVENC_CHID,
	    (unsigned)nvenc->runlist, privileged != 0 ? ", admin" : "");
	return 0;
}

/*
 * Undoes nvenc-channel's objects before the work channel's stop: the
 * channel off the scheduler, the encoder's object, the channel, and the
 * page's and the context's mappings in the work space.  Failures are
 * counted and shown; the unload removes the rest.
 */
void
nvrtx_nvenc_stop(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_nvenc_state *nvenc;
	uint8_t params[NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int failures;
	int error;

	/* Starts the teardown with no failure counted. */
	nvenc = gpu->nvenc;
	failures = 0;

	/* Nothing was made without the state. */
	if (nvenc == NULL)
		return;

	/* Nothing is submitted from now on. */
	mutex_lock(&nvenc->lock);

	nvenc->ready = 0;

	mutex_unlock(&nvenc->lock);

	/* Takes the channel off the scheduler. */
	if (nvenc->scheduled != 0) {
		kern_memset(params, 0, sizeof(params));
		error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVENC_HANDLE_CHANNEL, NVRTX_CTL_SCHEDULE, params, sizeof(params), &reply, &reply_length);
		if (error != 0)
			failures++;
		nvenc->scheduled = 0;
	}

	/* Frees the encoder's object, then the channel. */
	if (nvenc->object_made != 0) {
		error = nvrtx_rm_free(gpu, NVENC_HANDLE_OBJECT);
		if (error != 0)
			failures++;
		nvenc->object_made = 0;
	}

	/* Frees the channel from the resource manager. */
	if (nvenc->channel_made != 0) {
		error = nvrtx_rm_free(gpu, NVENC_HANDLE_CHANNEL);
		if (error != 0)
			failures++;
		nvenc->channel_made = 0;
	}

	/* Gives the page's address in the work space back. */
	if (nvenc->page_mapped != 0) {
		error = nvrtx_work_unmap(gpu, nvenc->page_va, NVRTX_GSP_PAGE_SIZE);
		if (error != 0)
			failures++;
		nvenc->page_mapped = 0;
	}

	/* Unmaps the engine context from the work space. */
	if (nvenc->context_mapped != 0) {
		error = nvrtx_work_unmap(gpu, nvenc->context_va, nvenc->context.size);
		if (error != 0)
			failures++;
		nvenc->context_mapped = 0;
	}

	/* Says how the stop went, when anything failed. */
	if (failures != 0)
		nvrtx_stage_mark("channel-stop: %d encoder frees failed; the unload removes the rest", failures);
}

/*
 * Gives back the encoder's memory and its state.
 *
 * Only once the GPU no longer reads it: after the GSP is stopped (the
 * work space's mappings go with the work channel's arena).
 */
void
nvrtx_nvenc_release(
	struct nvrtx_gpu *gpu)
{
	struct nvrtx_nvenc_state *nvenc;

	/* Takes the encoder's state, which may not exist. */
	nvenc = gpu->nvenc;

	/* Nothing was made without the state. */
	if (nvenc == NULL)
		return;

	/* The system memory. */
	nvrtx_buffer_free(gpu, &nvenc->page);
	nvrtx_buffer_free(gpu, &nvenc->methods);

	/* The VRAM blocks. */
	if (nvenc->context_made != 0) {
		nvrtx_bo_free(gpu, &nvenc->context);
		nvenc->context_made = 0;
	}

	/* Frees the instance block. */
	if (nvenc->block_made != 0) {
		nvrtx_bo_free(gpu, &nvenc->block);
		nvenc->block_made = 0;
	}

	/* The state. */
	gpu->nvenc = NULL;
	kern_free(nvenc);
}

/*
 * Remakes the encoder's channel after one that stopped on an error: the
 * old channel's objects are freed as by the stop, its memory is left (the
 * engine may still name it; it is not given back before the unload), and
 * nvenc-channel runs again.  For the encoder's self-test, which tries
 * values the engine may refuse; privileged makes the new channel an admin
 * one.
 */
int
nvrtx_nvenc_restart(
	struct nvrtx_gpu *gpu,
	int privileged)
{
	int error;

	/* Frees the stopped channel's objects and forgets its state, whose memory stays. */
	nvrtx_nvenc_stop(gpu);
	gpu->nvenc = NULL;
	nvrtx_stage_mark("nvenc-channel: the stopped channel is freed, its memory left; the channel is made again");

	/* Makes the channel again, with the privilege asked for. */
	error = nvenc_start(gpu, privileged);
	if (error != 0)
		return error;

	/* Succeeded: the new channel takes work. */
	return 0;
}

/* Reports whether the encoder's channel is an admin one (0 without an encoder). */
int
nvrtx_nvenc_privileged(
	const struct nvrtx_gpu *gpu)
{
	/* No state, no channel. */
	if (gpu->nvenc == NULL)
		return 0;

	/* Succeeded: the channel's privilege. */
	return gpu->nvenc->privileged;
}

/*
 * Reports whether the GPU has an encoder that takes work.  Any context,
 * no lock (a stop may follow at once; nvrtx_nvenc_run() refuses then).
 */
bool
nvrtx_nvenc_ready(
	const struct nvrtx_gpu *gpu)
{
	const struct nvrtx_nvenc_state *nvenc;
	int ready;

	/* Takes the encoder's state, which may not exist. */
	nvenc = gpu->nvenc;

	/* No state, no encoder. */
	if (nvenc == NULL)
		return false;

	/* The channel's readiness. */
	ready = __atomic_load_n(&nvenc->ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return false;

	/* Succeeded: the encoder runs. */
	return true;
}

/* Reports the encoder's class (0 without an encoder). */
uint32_t
nvrtx_nvenc_class_of(
	const struct nvrtx_gpu *gpu)
{
	/* No state, no class. */
	if (gpu->nvenc == NULL)
		return 0U;

	/* Succeeded: the class the channel's object has. */
	return gpu->nvenc->nvenc_class;
}

/*
 * Runs one push buffer segment on the encoder and waits for it.
 *
 * The segment is length bytes at push_va in the work channel's space.
 * Returns ENODEV without a running encoder, EINVAL for an empty or
 * misaligned segment, ETIMEDOUT when the encoder did not finish within
 * timeout_us (the channel is then taken out of use: later work would
 * queue behind the work that hangs).
 */
int
nvrtx_nvenc_run(
	struct nvrtx_gpu *gpu,
	uint64_t push_va,
	size_t length,
	uint32_t timeout_us)
{
	int error;

	/* The run with no word watched. */
	error = nvrtx_nvenc_run_watched(gpu, push_va, length, timeout_us, NULL, 0U, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the encoder has finished the segment. */
	return 0;
}

/*
 * Runs one push buffer segment as nvrtx_nvenc_run() does, and while it
 * waits watches a word the encoder writes: changed_us gets the time from
 * the doorbell to the first look that saw the word differ from sentinel
 * (UINT64_MAX when it never did).  watch may be NULL (then nothing is
 * watched).  For the encoder's self-test.
 */
int
nvrtx_nvenc_run_watched(
	struct nvrtx_gpu *gpu,
	uint64_t push_va,
	size_t length,
	uint32_t timeout_us,
	const volatile uint32_t *watch,
	uint32_t sentinel,
	uint64_t *changed_us)
{
	struct nvrtx_nvenc_state *nvenc;
	uint32_t fence;
	uint32_t seen;
	int error;

	/* Takes the encoder's state, which may not exist. */
	nvenc = gpu->nvenc;

	/* No state, no encoder. */
	if (nvenc == NULL)
		return ENODEV;

	/* Whole words of one ring entry. */
	if (length == 0U ||
	    (length & 3U) != 0U ||
	    length / 4U > NVRTX_WORK_SEGMENT_WORDS_MAX)
		return EINVAL;

	/* Submits the segment and its fence, then waits for the fence, one segment at a time. */
	mutex_lock(&nvenc->lock);

	error = ENODEV;
	fence = 0U;

	/* Submits only to an encoder that has not been taken out of use. */
	if (nvenc->ready != 0)
		error = nvenc_submit(gpu, nvenc, push_va, length, &fence);

	/* Waits for the segment's fence. */
	if (error == 0)
		error = nvenc_wait(nvenc, fence, timeout_us, watch, sentinel, changed_us);

	/* An encoder that did not finish is taken out of use, so no later work queues behind it. */
	if (error == ETIMEDOUT)
		__atomic_store_n(&nvenc->ready, 0, __ATOMIC_RELEASE);

	mutex_unlock(&nvenc->lock);

	/* Reports an encoder that did not finish. */
	if (error == ETIMEDOUT) {
		seen = nvenc_word(nvenc, NVENC_SEMAPHORE_AT);
		nvrtx_stage_mark("nvenc: work did not finish (fence %u, semaphore %08x); the encoder is off", (unsigned)fence, (unsigned)seen);
	}

	/* Propagates the failure of the segment. */
	if (error != 0)
		return error;

	/* Succeeded: the encoder has finished the segment. */
	return 0;
}

/* Reports how many channel errors the GSP has reported so far. */
uint32_t
nvrtx_nvenc_notices(
	struct nvrtx_gpu *gpu)
{
	uint32_t count;
	int owned;

	/* The record is written under the GSP's lock (a caller may hold it already). */
	owned = mutex_owned(&gpu->rpc_lock);
	if (owned == 0)
		nvrtx_rpc_lock(gpu);

	count = gpu->rc_notices;

	if (owned == 0)
		nvrtx_rpc_unlock(gpu);

	/* Succeeded: the count. */
	return count;
}

/*
 * Reads the messages the GSP sent since the last call to it for a short
 * time (a channel error is recorded and shown by the read), then marks
 * under the stage's name how many channel errors came since before, and
 * the newest one's channel and kind.
 */
void
nvrtx_nvenc_report_notices(
	struct nvrtx_gpu *gpu,
	uint32_t before,
	const char *stage)
{
	uint32_t count;
	uint32_t chid;
	uint32_t type;
	size_t length;
	int owned;

	/* Reads the messages under the GSP's lock (a caller may hold it already); the read ends when its time is spent. */
	owned = mutex_owned(&gpu->rpc_lock);
	if (owned == 0)
		nvrtx_rpc_lock(gpu);

	if (gpu->reply != NULL)
		(void)nvrtx_rpc_wait(gpu, NVENC_NO_FUNCTION, 0U, NVENC_NOTICE_US, &length);

	count = gpu->rc_notices - before;
	chid = gpu->rc_last_chid;
	type = gpu->rc_last_type;

	if (owned == 0)
		nvrtx_rpc_unlock(gpu);

	/* No channel error came. */
	if (count == 0U) {
		nvrtx_stage_mark("%s: no channel error was reported", stage);
		return;
	}

	/* Counts them, with the newest. */
	nvrtx_stage_mark("%s: %u channel errors, newest chid %u type %u", stage, (unsigned)count, (unsigned)chid, (unsigned)type);
}

/*
 * Finds the encoder: its class for the GPU's family, its runlist and
 * engine descriptor in the device information table the first channel
 * read, and its engine context's size.
 */
static int
nvenc_find(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* The family's class. */
	nvenc->nvenc_class = nvrtx_nvenc_class(gpu->identity.chip.family);
	if (nvenc->nvenc_class == 0U) {
		nvrtx_stage_mark("nvenc-channel fail: no encoder class for this family");
		return ENODEV;
	}

	/* The encoder's runlist and descriptor. */
	if (gpu->engine.pick.nvenc_found == 0U) {
		nvrtx_stage_mark("nvenc-channel fail: no encoder in the device information table");
		return ENODEV;
	}

	/* Keeps the runlist and the descriptor the table gave. */
	nvenc->runlist = gpu->engine.pick.nvenc_runlist;
	nvenc->engine_descriptor = gpu->engine.pick.nvenc_descriptor;

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
		nvrtx_stage_mark("nvenc-channel fail: the falcons' list was refused (%d)", error);
		return error;
	}

	/* The encoder's context size, by its descriptor in the falcons' list. */
	error = nvrtx_nvdec_context_bytes(reply, reply_length, nvenc->engine_descriptor, &nvenc->context_bytes);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: engine %x is not among the falcons (%d)", (unsigned)nvenc->engine_descriptor, error);
		return error;
	}

	/* The encoder found, by the table's entry and the falcons' list. */
	nvrtx_stage_mark("nvenc-channel: engine %x on runlist %u, context %u bytes", (unsigned)nvenc->engine_descriptor,
	    (unsigned)nvenc->runlist, (unsigned)nvenc->context_bytes);

	/* Succeeded: the encoder is known. */
	return 0;
}

/*
 * Makes the channel's memory: the VRAM block of the instance block and
 * USERD, the engine context (cleared and mapped into the space) when the
 * encoder has one, the system-memory page (mapped into the space) and the
 * method buffer.
 */
static int
nvenc_memory(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	uint8_t *instance;
	uint64_t context_size;
	uint64_t page;
	uint64_t root;
	int error;

	/* The VRAM block of the instance block and USERD, cleared. */
	error = nvrtx_bo_alloc(gpu, NVENC_BLOCK_BYTES, NVENC_BLOCK_BYTES, &nvenc->block);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: no VRAM for the instance block (%d)", error);
		return error;
	}

	/* Marks the block made, so the release frees it, then zeroes it. */
	nvenc->block_made = 1;
	error = nvrtx_vram_zero(gpu, nvenc->block.vram, (size_t)NVENC_BLOCK_BYTES);
	if (error != 0)
		return error;

	/* The instance block names the work channel's tables. */
	instance = kern_calloc(1U, NVRTX_INSTANCE_BYTES);
	if (instance == NULL)
		return ENOMEM;

	/* Builds the instance block from the work channel's root and writes it. */
	root = nvrtx_work_root(gpu);
	nvrtx_mmu_instance(instance, root, NVENC_VA_LIMIT);
	error = nvrtx_vram_write(gpu, nvenc->block.vram + NVENC_INSTANCE_AT, instance, NVRTX_INSTANCE_BYTES);
	kern_free(instance);
	if (error != 0)
		return error;

	/* The engine context, cleared, in the space. */
	if (nvenc->context_bytes != 0U) {
		context_size = nvenc_round(nvenc->context_bytes, NVENC_CONTEXT_ALIGN);
		error = nvrtx_bo_alloc(gpu, context_size, NVENC_CONTEXT_ALIGN, &nvenc->context);
		if (error != 0) {
			nvrtx_stage_mark("nvenc-channel fail: no VRAM for the engine context (%d)", error);
			return error;
		}

		/* Marks the context made, so the release frees it, then zeroes it. */
		nvenc->context_made = 1;
		error = nvrtx_vram_zero(gpu, nvenc->context.vram, (size_t)context_size);
		if (error != 0)
			return error;

		/* Maps the context into the work space. */
		error = nvrtx_work_map_vram(gpu, nvenc->context.vram, context_size, &nvenc->context_va);
		if (error != 0)
			return error;

		/* Marks the context mapped, so the release unmaps it. */
		nvenc->context_mapped = 1;
	}

	/* The page of the ring, the semaphores and the push buffers, in the space. */
	error = nvrtx_buffer_alloc(gpu, NVRTX_GSP_PAGE_SIZE, &nvenc->page);
	if (error != 0)
		return error;

	/* Maps the page into the work space. */
	page = nvenc->page.dma.device_address;
	error = nvrtx_work_map_system(gpu, &page, 1U, &nvenc->page_va);
	if (error != 0)
		return error;

	/* Marks the page mapped, so the release unmaps it. */
	nvenc->page_mapped = 1;

	/* The method buffer, as large as the resource manager asked for the first channel's. */
	error = nvrtx_buffer_alloc(gpu, gpu->engine.methods_size, &nvenc->methods);
	if (error != 0)
		return error;

	/* Succeeded: the memory is in place. */
	return 0;
}

/*
 * Makes the channel on the encoder's engine in the work channel's space,
 * binds it to the encoder and lets the scheduler run it.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/fifo.c (r535_chan_alloc,
 * then the bind and the schedule before the engine's context).
 */
static int
nvenc_channel(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	struct nvrtx_channel_setup setup;
	uint8_t *params;
	uint8_t control[NVRTX_BIND_PARAMS_BYTES + NVRTX_SCHEDULE_PARAMS_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* The channel on the encoder's engine, its ring at the start of the page. */
	setup.ring_va = nvenc->page_va + NVENC_RING_AT;
	setup.ring_entries = NVENC_RING_ENTRIES;
	setup.chid = NVENC_CHID;
	setup.space = NVRTX_HANDLE_WORK_SPACE;
	setup.engine = NVRTX_ENGINE_NVENC0;
	setup.privileged = (uint32_t)nvenc->privileged;
	setup.instance = nvenc->block.vram + NVENC_INSTANCE_AT;
	setup.userd = nvenc->block.vram + NVENC_USERD_AT;
	setup.methods = nvenc->methods.dma.device_address;
	setup.methods_size = gpu->engine.methods_size;

	/* Allocates the channel's parameters. */
	params = kern_malloc(NVRTX_CHANNEL_PARAMS_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Describes the channel to the resource manager and allocates it. */
	nvrtx_channel_params(params, &setup);
	error = nvrtx_rm_allocate(gpu, NVRTX_HANDLE_DEVICE, NVENC_HANDLE_CHANNEL, gpu->identity.chip.profile->class_channel, params, NVRTX_CHANNEL_PARAMS_BYTES);
	kern_free(params);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: the channel was refused (%d)", error);
		return error;
	}

	/* Marks the channel made, so the release frees it. */
	nvenc->channel_made = 1;

	/* Binds it to the encoder. */
	kern_memset(control, 0, sizeof(control));
	nvrtx_put32(control, 0U, NVRTX_ENGINE_NVENC0);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVENC_HANDLE_CHANNEL, NVRTX_CTL_BIND, control, NVRTX_BIND_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: the bind was refused (%d)", error);
		return error;
	}

	/* Lets the scheduler run it. */
	kern_memset(control, 0, sizeof(control));
	control[0] = 1U;
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVENC_HANDLE_CHANNEL, NVRTX_CTL_SCHEDULE, control, NVRTX_SCHEDULE_PARAMS_BYTES, &reply, &reply_length);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: the schedule was refused (%d)", error);
		return error;
	}

	/* Succeeded: the stop takes the channel off the scheduler before it frees it. */
	nvenc->scheduled = 1;
	return 0;
}

/* Hands the channel its engine context by its address in the space (an encoder without one is left as it is). */
static int
nvenc_context(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	uint8_t *params;
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* No context, nothing to promote. */
	if (nvenc->context_made == 0) {
		nvrtx_stage_mark("nvenc-channel: the encoder has no engine context to promote");
		return 0;
	}

	/* The promote of the context to the subdevice. */
	params = kern_malloc(NVRTX_NVENC_PROMOTE_BYTES);
	if (params == NULL)
		return ENOMEM;

	/* Fills the promote's parameters and sends them. */
	nvrtx_nvenc_promote_params(params, NVRTX_HANDLE_CLIENT, NVENC_CHID, NVENC_HANDLE_CHANNEL, nvenc->context_va, nvenc->context.size);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_SUBDEVICE, NVRTX_CTL_PROMOTE_CONTEXT, params, NVRTX_NVENC_PROMOTE_BYTES, &reply, &reply_length);
	kern_free(params);
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: the context's promote was refused (%d)", error);
		return error;
	}

	/* Succeeded: the channel has its context. */
	return 0;
}

/* Makes the encoder's object on the channel. */
static int
nvenc_object(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	uint8_t params[NVRTX_NVENC_ALLOC_PARAMS_BYTES];
	int error;

	/* The object of the family's class on the first encoder. */
	nvrtx_nvenc_alloc_params(params);
	error = nvrtx_rm_allocate(gpu, NVENC_HANDLE_CHANNEL, NVENC_HANDLE_OBJECT, nvenc->nvenc_class, params, sizeof(params));
	if (error != 0) {
		nvrtx_stage_mark("nvenc-channel fail: the encoder's object was refused (%d)", error);
		return error;
	}

	/* Succeeded: the stop frees the object before its channel. */
	nvenc->object_made = 1;
	return 0;
}

/*
 * Runs the channel's first submissions: the class on its subchannel and a
 * no-operation, then a semaphore the encoder releases itself, each
 * followed by its fence.  The channel takes work once both have passed
 * and the encoder's semaphore holds the value it was given.
 */
static int
nvenc_first(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc)
{
	uint8_t *push;
	uint32_t released;
	size_t length;
	int error;

	/* Opens the channel; both semaphores start at 0. */
	kern_memset((uint8_t *)nvenc->page.dma.address + NVENC_SEMAPHORE_AT, 0, 32U);
	nvenc->put = 0U;
	nvenc->fence = 0U;

	/* The class and the no-operation, then its fence. */
	push = (uint8_t *)nvenc->page.dma.address + NVENC_OBJECT_AT;
	length = nvrtx_nvenc_push_object(push, nvenc->nvenc_class);
	error = nvenc_once(gpu, nvenc, NVENC_OBJECT_AT, length, "class");
	if (error != 0)
		return error;

	/* The encoder's own release, then its fence. */
	push = (uint8_t *)nvenc->page.dma.address + NVENC_RELEASE_AT;
	length = nvrtx_nvenc_push_release(push, nvenc->page_va + NVENC_RELEASE_SEMAPHORE_AT, NVENC_RELEASE_VALUE);
	error = nvenc_once(gpu, nvenc, NVENC_RELEASE_AT, length, "release");
	if (error != 0)
		return error;

	/* The fence has passed; the encoder's semaphore must hold its value. */
	released = nvenc_word(nvenc, NVENC_RELEASE_SEMAPHORE_AT);
	if (released != NVENC_RELEASE_VALUE) {
		nvrtx_stage_mark("nvenc-channel fail: the encoder released %08x, not %08x", (unsigned)released, (unsigned)NVENC_RELEASE_VALUE);
		return EIO;
	}

	/* Opens the channel to work. */
	__atomic_store_n(&nvenc->ready, 1, __ATOMIC_RELEASE);
	nvrtx_stage_mark("nvenc-channel: the encoder released %08x itself", (unsigned)released);

	/* Succeeded: the encoder took methods and wrote memory. */
	return 0;
}

/*
 * Submits one of the bring-up's push buffers (length bytes at push_at in
 * the page) and waits for its fence; what names it in the stage log.
 */
static int
nvenc_once(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc,
	uint32_t push_at,
	size_t length,
	const char *what)
{
	uint32_t fence;
	uint32_t seen;
	int error;

	/* Submits it and waits for its fence. */
	mutex_lock(&nvenc->lock);

	fence = 0U;
	error = nvenc_submit(gpu, nvenc, nvenc->page_va + push_at, length, &fence);

	/* Waits for the fence once the submission took. */
	if (error == 0)
		error = nvenc_wait(nvenc, fence, NVENC_FIRST_FENCE_US, NULL, 0U, NULL);

	mutex_unlock(&nvenc->lock);

	/* Reports a fence that did not come. */
	if (error != 0) {
		seen = nvenc_word(nvenc, NVENC_SEMAPHORE_AT);
		nvrtx_stage_mark("nvenc-channel fail: %s fence %u did not come (%d, %08x)", what, (unsigned)fence, error, (unsigned)seen);
		return error;
	}

	/* Succeeded: the push buffer ran. */
	return 0;
}

/*
 * Puts a segment and a fence after it on the ring and rings the doorbell.
 * The caller holds the lock and waits for the fence before the next
 * submission, so the entries and the fence's push buffer are free.
 */
static int
nvenc_submit(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc,
	uint64_t push_va,
	size_t length,
	uint32_t *fence)
{
	uint8_t *fence_push;
	size_t fence_length;
	uint32_t value;
	int error;

	/* The fence's value: the next one, never 0. */
	value = nvenc->fence + 1U;
	if (value == 0U)
		value = 1U;

	/* The fence's push buffer: a release of the value once the encoder is idle. */
	fence_push = (uint8_t *)nvenc->page.dma.address + NVENC_FENCE_AT;
	fence_length = nvrtx_push_fence(fence_push, nvenc->page_va + NVENC_SEMAPHORE_AT, value);

	/* The segment, then the fence, then the doorbell: the encoder's runlist and the channel. */
	error = nvenc_ring_put(gpu, nvenc, push_va, length);

	/* The fence follows the segment on the ring. */
	if (error == 0)
		error = nvenc_ring_put(gpu, nvenc, nvenc->page_va + NVENC_FENCE_AT, fence_length);

	/* Refuses to ring the doorbell for a ring that did not take its entries. */
	if (error != 0)
		return error;

	/* Rings the doorbell, so the engine reads the ring. */
	nvrtx_wr32(gpu, NVENC_REG_DOORBELL, (nvenc->runlist << 16) | NVENC_CHID);
	(void)nvrtx_gpu_submitted(gpu);

	/* Succeeded: the semaphore reaches the value once the segment is done. */
	nvenc->fence = value;
	*fence = value;
	return 0;
}

/* Writes one ring entry and moves the put in USERD past it. */
static int
nvenc_ring_put(
	struct nvrtx_gpu *gpu,
	struct nvrtx_nvenc_state *nvenc,
	uint64_t va,
	size_t length)
{
	uint8_t put[4];
	uint8_t *entry;
	uint32_t next;
	int error;

	/* The entry, written before the put that hands it to the GPU. */
	entry = (uint8_t *)nvenc->page.dma.address + NVENC_RING_AT + (size_t)nvenc->put * 8U;
	nvrtx_ring_entry(entry, va, (uint32_t)(length / 4U));
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The ring's put in USERD, wrapping at the ring's end. */
	next = (nvenc->put + 1U) % NVENC_RING_ENTRIES;
	nvrtx_put32(put, 0U, next);
	error = nvrtx_vram_write(gpu, nvenc->block.vram + NVENC_USERD_AT + NVENC_USERD_RING_PUT, put, sizeof(put));
	if (error != 0)
		return error;

	/* Succeeded: the next entry is the one after it. */
	nvenc->put = next;
	return 0;
}

/*
 * Waits until the semaphore has reached a fence, up to a time measured in
 * microseconds: the first 2 ms without a pause, then between sleeps.  A
 * watched word's first change is timed into changed_us (see
 * nvrtx_nvenc_run_watched()).
 */
static int
nvenc_wait(
	struct nvrtx_nvenc_state *nvenc,
	uint32_t fence,
	uint32_t timeout_us,
	const volatile uint32_t *watch,
	uint32_t sentinel,
	uint64_t *changed_us)
{
	uint64_t start;
	uint64_t spent;
	uint32_t seen;
	bool passed;

	/* The watched word has not changed yet. */
	if (changed_us != NULL)
		*changed_us = UINT64_MAX;

	/* Looks until the fence has passed or the time, measured, is spent. */
	start = nvrtx_now_us();
	for (;;) {
		/* Notes when the watched word first differs from its sentinel. */
		if (watch != NULL &&
		    changed_us != NULL &&
		    *changed_us == UINT64_MAX &&
		    *watch != sentinel)
			*changed_us = nvrtx_now_us() - start;

		seen = nvenc_word(nvenc, NVENC_SEMAPHORE_AT);
		passed = nvrtx_work_fence_passed(seen, fence);
		if (passed)
			break;

		/* Gives up on a segment that has taken longer than the caller allows. */
		spent = nvrtx_now_us() - start;
		if (spent > timeout_us)
			return ETIMEDOUT;

		/* Spins at first; else it looks again after a sleep. */
		if (spent < NVENC_SPIN_US) {
			atomic_spin_hint();
		} else {
			kern_usleep_range(NVENC_POLL_US, NVENC_POLL_US * 2U);
		}
	}

	/* Succeeded: the work before the fence is done. */
	return 0;
}

/* Reads a word of the coherent page the GPU writes (a semaphore). */
static uint32_t
nvenc_word(
	const struct nvrtx_nvenc_state *nvenc,
	uint32_t at)
{
	const volatile uint32_t *word;

	/* The word in the coherent page. */
	word = (const volatile uint32_t *)((const uint8_t *)nvenc->page.dma.address + at);

	/* Succeeded: what the GPU last wrote there. */
	return *word;
}

/* Rounds a value up to a boundary that is a power of two. */
static uint64_t
nvenc_round(
	uint64_t value,
	uint64_t boundary)
{
	/* Succeeded: the rounded value. */
	return (value + boundary - 1U) & ~(boundary - 1U);
}
