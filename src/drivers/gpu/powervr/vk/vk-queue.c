/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's submissions, fences and semaphores, and the work it
 * hands the core.
 *
 * vkQueueSubmit builds one compute control stream for all its command
 * buffers: for each workgroup of each dispatch a kernel whose shading
 * cluster program the driver builds from the pipeline's shader
 * (compute-program.c), and after each dispatch a fence kernel, so the
 * dispatches run in order.  One compute command on the session's
 * firmware context runs the stream; the submission waits, bounded, for
 * the session's timeline before it answers, so its fence is signaled
 * then, and the marker libvulkan reserved for it completes after it.  A
 * semaphore orders nothing more than the one queue already does.
 *
 * The executor's own memory (the programs and the stream) is made at the
 * first submission and reused by each: the earlier one has finished.
 * When a submission does not finish in time, the core may still read and
 * write that memory, so it stays with the core (abandoned).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/render-sr-program.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/compute-program.h"
#include "drivers/gpu/powervr/render-job.h"
#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/vk/vk.h"

/* The most submits of one vkQueueSubmit, and the most command buffers and semaphores of one submit. */
#define QUEUE_SUBMITS_MAX		8U
#define QUEUE_ITEMS_MAX			32U

/*
 * The most kernels (workgroups) one submission runs: vkcompute's 128
 * (userland/tests/vkcompute/main.c), the most the tests dispatch.
 */
#define QUEUE_KERNELS_MAX		64U

/*
 * The executor's memory: the shading cluster programs (one 512-byte room
 * per kernel) and the data sequencer programs (one 64-byte slot per
 * program, data then code; slot 0 the fence's), each at its place in its
 * heap past the session's fixed area (the heap's first page,
 * fixed-area.h), and the stream buffer (the resume area first, then the
 * stream).
 */
#define QUEUE_USC_OFFSET		0x10000U
#define QUEUE_USC_ROOM			0x800U
#define QUEUE_USC_BYTES			(QUEUE_USC_ROOM * QUEUE_KERNELS_MAX)
#define QUEUE_PDS_OFFSET		0x10000U
#define QUEUE_PDS_SLOT			0x80U
#define QUEUE_PDS_CODE			0x10U
#define QUEUE_PDS_BYTES			(QUEUE_PDS_SLOT * (QUEUE_KERNELS_MAX + 1U))
#define QUEUE_STREAM_BYTES		0x10000U
#define QUEUE_STREAM_FIRST		0x1000U

_Static_assert(QUEUE_USC_OFFSET == POWERVR_PROGRAM_HEAP_START && QUEUE_USC_BYTES <= POWERVR_EXECUTOR_USC_BYTES, "the executor's programs are in the place the session holds for them");
_Static_assert(QUEUE_PDS_OFFSET == POWERVR_PROGRAM_HEAP_START && QUEUE_PDS_BYTES <= POWERVR_EXECUTOR_PDS_BYTES, "the executor's programs are in the place the session holds for them");

/*
 * Whether a dispatch runs as one kernel of all its workgroups (1), the
 * data sequencer writing each workgroup's id into the coefficient
 * registers the program reads it from, or as one kernel per workgroup
 * with the id compiled into each (0).  The first needs the compiler to
 * read the workgroup id from coefficient registers 0 to 2.
 */
#ifndef POWERVR_EXECUTOR_ONE_KERNEL
#define POWERVR_EXECUTOR_ONE_KERNEL	1
#endif
#define QUEUE_GROUP_COEFFICIENTS	3U

/*
 * Whether a dispatch ends with a fence kernel (1).  On the board a kernel
 * with the fence bit locked up after its stores even when it was the same
 * bits as Mesa's, and a stream of kernels whose programs wait for their
 * own stores before the end ran (2026-10-21); the one-kernel executor
 * leaves the fence out, as Mesa's one dispatch is one kernel.
 */
#ifndef POWERVR_EXECUTOR_FENCE_KERNEL
#define POWERVR_EXECUTOR_FENCE_KERNEL	(!POWERVR_EXECUTOR_ONE_KERNEL || POWERVR_EXECUTOR_MESA_STREAM)
#endif
#define QUEUE_PDS_GROUP_INPUTS		4U

/*
 * Whether a dispatch is Mesa's three kernels (1): a kernel that only
 * allocates the shared registers (64 bytes of common store on every
 * cluster, the registers kept for the kernels after it, no shared data,
 * eight instances a task), the dispatch with the shared data the
 * shading cluster's (sd_type USC), then the fence that waits for it and
 * frees the shared registers.  On the board a fence after a dispatch
 * without the first kernel locked up (2026-10-21), which may be the
 * fence freeing registers no kernel allocated.  With it the fence kernel
 * is on (POWERVR_EXECUTOR_FENCE_KERNEL), and on the board four dispatches
 * of the three kernels ran without a reset (2026-10-21), so it is the
 * default and a submission's dispatches keep their order.
 *
 * Values referenced:
 * Mesa 26.2.5's vkcompute stream on BXE-4-32, as its driver wrote it
 * on the host with the drm shim (kernel 0: usc_common_size
 * 64 bytes, usc_target ALL, sd_type NONE, usc_common_shared, pds data 32
 * bytes, max_instances 8, one instance; kernel 1 sd_type USC; kernel 2
 * fence, sd_type PDS, usc_common_shared, max_instances 8; terminate).
 */
#ifndef POWERVR_EXECUTOR_MESA_STREAM
#define POWERVR_EXECUTOR_MESA_STREAM	1
#endif

/*
 * A dispatch kernel's program and sizes: the one kernel's data sequencer
 * program writes the workgroup ids into the common store; a kernel per
 * workgroup has them compiled in.
 */
#if POWERVR_EXECUTOR_ONE_KERNEL
#define QUEUE_KERNEL_PDS_CODE		POWERVR_COMPUTE_PDS_GROUP_CODE
#define QUEUE_KERNEL_DATA_WORDS		POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS
#define QUEUE_KERNEL_USC_INPUTS		POWERVR_COMPUTE_GROUP_USC_INPUTS
#define QUEUE_KERNEL_PDS_INPUTS		QUEUE_PDS_GROUP_INPUTS
#define QUEUE_KERNEL_COMMON		QUEUE_GROUP_COEFFICIENTS
#else
#define QUEUE_KERNEL_PDS_CODE		QUEUE_PDS_CODE
#define QUEUE_KERNEL_DATA_WORDS		POWERVR_COMPUTE_PDS_DATA_WORDS
#define QUEUE_KERNEL_USC_INPUTS		0U
#define QUEUE_KERNEL_PDS_INPUTS		QUEUE_PDS_INPUTS
#define QUEUE_KERNEL_COMMON		0U
#endif
#define QUEUE_SHARED_DATA		0x40U
#define QUEUE_SHARED_CODE		0x50U

/* The data sequencer temporaries a compute kernel's inputs take. */
#define QUEUE_PDS_INPUTS		3U

/*
 * The most render jobs one command buffer runs: an instance takes one
 * job for each POWERVR_RENDER_DRAWS draws it records (the next job goes
 * on loading what the one before stored), so 64 jobs take 64 times that many draws.
 * The jobs are made for each command buffer, as many as it needs.
 */
#define QUEUE_JOBS_MAX			64U

/*
 * Whether a submission returns once it kicked its work (1, the default),
 * or waits for each render job and compute stream to finish (0).
 */
#ifndef POWERVR_ASYNC_SUBMIT
#define POWERVR_ASYNC_SUBMIT		1
#endif

/* How long a submission waits for the core. */
#define QUEUE_WAIT_MS			2000U

/* Which of the executor's buffers are mapped at their fixed places. */
#define MAPPED_USC			0x1U
#define MAPPED_PDS			0x2U

/*
 * The executor's memory on the core, made at the first submission: the
 * program buffers mapped at their places, and the stream's resource.
 */
struct powervr_vk_executor {
	struct powervr_buffer *usc;
	struct powervr_buffer *pds;
	struct powervr_resource *stream;
	unsigned mapped;
};

/* One submission being built: where the stream is written and how many kernels it has. */
struct submission {
	struct powervr_vk_session *session;
	struct powervr_vk_executor *executor;
	uint32_t stream_at;
	uint32_t kernels;
	uint32_t dispatches;
	uint32_t sent;
	const struct powervr_vk_pipeline *pipeline;
	const struct powervr_vk_descriptor_set *sets[POWERVR_VK_SETS_MAX];
};

static int submit(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int read_submit(struct i915_wire_reader *reader, uint64_t *commands, uint32_t *command_count);
static int read_identities(struct i915_wire_reader *reader, bool stages);
static uint32_t submit_result(int error);
static int create_sync(struct powervr_vk_session *session, enum i915_vk_object_kind kind, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int reset_fences(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int fence_status(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int executor_make(struct powervr_vk_session *session);
static int map_program_buffer(struct powervr_session *owner, uint64_t bytes, uint64_t address, struct powervr_buffer **result);
static int build_command_buffer(struct submission *build, const struct powervr_vk_command_buffer *command);
static int build_dispatch(struct submission *build, uint32_t groups);
static int dispatch_kernels(struct submission *build, const struct powervr_vk_pipeline *pipeline, uint32_t groups, uint32_t kernels, struct powervr_shader_workspace *workspace, struct powervr_shader_program *program, uint8_t *code);
static int build_shared(struct submission *build, const struct powervr_shader_program *program, uint32_t groups, uint32_t *values);
static int build_kernel(struct submission *build, const uint8_t *code, size_t length, uint32_t temps, uint32_t instances, uint32_t workgroups);
static int build_fence(struct submission *build);
static int build_shared_kernel(struct submission *build);
static void put_stream_words(struct submission *build, const uint32_t *words, unsigned count);
static void put_word(struct powervr_buffer *buffer, uint32_t offset, uint32_t word);
static int run(struct submission *build);
static int run_flush(struct submission *build);
static int run_job(struct submission *build, const struct powervr_render_job *job);
static int order_after(struct submission *build, unsigned kind);
static int settle(struct submission *build);
static uint32_t instance_jobs(const struct powervr_vk_command_buffer *command, uint32_t begin);

/*
 * Runs a submission, fence or semaphore command; *handled stays clear for
 * another handler's opcode.
 *
 * Procedure referenced:
 * zedBSD src/drivers/gpu/bcm2711/vk/vulkan-queue.c and vulkan-sync.c (the
 * wire of each command).
 */
int
powervr_vk_queue_route(
	struct powervr_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	(void)requested;
	*handled = 1;
	switch (opcode) {
	case GPU_OP_QUEUE_SUBMIT:
		error = submit(session, reader, reply);
		break;
	case GPU_OP_CREATE_FENCE:
		error = create_sync(session, I915_VK_OBJ_FENCE, reader, reply);
		break;
	case GPU_OP_DESTROY_FENCE:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_FENCE);
		break;
	case GPU_OP_RESET_FENCES:
		error = reset_fences(session, reader, reply);
		break;
	case GPU_OP_GET_FENCE_STATUS:
		error = fence_status(session, reader, reply);
		break;
	case GPU_OP_CREATE_SEMAPHORE:
		error = create_sync(session, I915_VK_OBJ_SEMAPHORE, reader, reply);
		break;
	case GPU_OP_DESTROY_SEMAPHORE:
		error = powervr_vk_destroy_plain(session, reader, I915_VK_OBJ_SEMAPHORE);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * Returns the executor's memory: unmapped and released, unless a
 * submission was abandoned or the core may still reach the session, when
 * it stays with the core until the device is reset.
 */
void
powervr_vk_executor_free(
	struct powervr_vk_session *session)
{
	struct powervr_vk_executor *executor;
	struct powervr_render *render;
	struct powervr_session *owner;
	unsigned long enabled;
	unsigned keep;

	/* No executor memory was made. */
	executor = session->executor;
	if (executor == NULL)
		return;
	session->executor = NULL;
	owner = session->owner;
	render = owner->render;

	/* Whether the core may still reach the memory. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	keep = 0;
	if (render->faulted || owner->isolated)
		keep = 1;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (session->abandoned)
		keep = 1;

	/* Left with the core: nothing of it is touched again. */
	if (keep) {
		kern_logf("powervr: vk: the executor's memory stays with the core\n");
		return;
	}

	/* Unmaps the program buffers and has the core drop the entries. */
	mutex_lock(&render->core->lock);

	if ((executor->mapped & MAPPED_USC) != 0)
		powervr_gpu_mmu_unmap(&owner->mmu, POWERVR_USC_HEAP_BASE + QUEUE_USC_OFFSET, executor->usc->run.size);
	if ((executor->mapped & MAPPED_PDS) != 0)
		powervr_gpu_mmu_unmap(&owner->mmu, POWERVR_PDS_HEAP_BASE + QUEUE_PDS_OFFSET, executor->pds->run.size);
	if (executor->mapped != 0)
		(void)powervr_command_mmu_flush(render->core);

	mutex_unlock(&render->core->lock);

	/* The buffers and the stream's resource. */
	if (executor->usc != NULL)
		powervr_buffer_put(render, executor->usc);
	if (executor->pds != NULL)
		powervr_buffer_put(render, executor->pds);
	if (executor->stream != NULL)
		powervr_resource_unmake(owner, executor->stream);
	kern_free(executor);
}

/*
 * vkQueueSubmit: [queue][count][count]{submit}[fence] -> [result].  Every
 * submit is read first; then all command buffers run in one stream, and
 * the fence is signaled once the core finished it.
 */
static int
submit(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	uint64_t commands[QUEUE_SUBMITS_MAX * QUEUE_ITEMS_MAX];
	struct powervr_vk_command_buffer *command;
	struct powervr_vk_fence *fence;
	struct submission build;
	uint32_t command_count;
	uint32_t taken;
	uint64_t submits;
	uint64_t index;
	uint64_t fence_id;
	uint64_t started;
	uint32_t result;
	uint64_t reaped;
	int reached;
	int error;

	/* When the submission started, for its timing. */
	started = powervr_timing_now();

	/* The queue and how many submits follow. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	submits = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || submits > QUEUE_SUBMITS_MAX)
		return EINVAL;

	/* Each submit's command buffers, in order. */
	command_count = 0;
	for (index = 0; index < submits; index++) {
		error = read_submit(reader, commands + command_count, &taken);
		if (error != 0)
			return error;
		command_count += taken;
	}

	/* The fence. */
	fence_id = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The render jobs and transfers of earlier submissions that finished give their memory back. */
	reaped = powervr_timing_now();
	powervr_render_jobs_reap(session->owner->render, session->owner, 0);
	powervr_transfers_reap(session->owner->render, session->owner);
	powervr_timing_add(POWERVR_TIMING_REAP, reaped);

	/* The executor's memory, made at the first submission. */
	error = 0;
	if (session->abandoned) {
		kern_logf("powervr: vk: submission refused (EIO): the session was abandoned\n");
		error = EIO;
	}
	if (error == 0 && session->executor == NULL)
		error = executor_make(session);

	/* The stream of every command buffer's dispatches. */
	kern_memset(&build, 0, sizeof(build));
	build.session = session;
	build.executor = session->executor;
	build.stream_at = QUEUE_STREAM_FIRST;
	for (index = 0; error == 0 && index < command_count; index++) {
		command = powervr_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, commands[index]);
		if (command == NULL || command->state != POWERVR_VK_COMMAND_EXECUTABLE) {
			error = EINVAL;
			break;
		}
		error = build_command_buffer(&build, command);
	}

	/* The core runs it, when it has any kernel. */
	if (error == 0 && build.kernels != 0)
		error = run(&build);
	if (error != 0)
		kern_logf("powervr: vk: a submission did not run (%d)\n", error);

	/*
	 * A failure once work of this submission went to the core (a stream,
	 * a render pass's job or a transfer) is a lost submission, never a
	 * refusal: a caller that took it for one would cancel a job part of
	 * which runs.
	 */
	if (error != 0 && build.sent != 0U)
		error = EIO;

	/* The submission's time (the timing's own thread says the sums). */
	powervr_timing_add(POWERVR_TIMING_SUBMIT, started);

	/* The fence completes at the submission's last point, which may already be reached. */
	fence = powervr_vk_payload(session, I915_VK_OBJ_FENCE, fence_id);
	if (error == 0 && fence != NULL) {
		fence->kind = session->owner->last_kind;
		fence->value = session->owner->last_value;
		reached = powervr_session_point_reached(session->owner, fence->kind, fence->value);
		fence->signaled = reached != 0;

		/* A compute stream already finished is the compute stage's ok. */
		if (reached && fence->kind == POWERVR_POINT_COMPUTE)
			powervr_stage_ok(POWERVR_STAGE_COMPUTE);
	}

	/* The submission's result. */
	result = submit_result(error);
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/*
 * Reads one submit: [sType][pNext][wait count][count]{semaphore}[count]
 * {stage}[command count][count]{command buffer}[signal count][count]
 * {semaphore}; the command buffers' identities are kept.
 */
static int
read_submit(
	struct i915_wire_reader *reader,
	uint64_t *commands,
	uint32_t *command_count)
{
	uint64_t count;
	uint64_t index;
	int error;

	/* sType and pNext. */
	*command_count = 0;
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);

	/* The waits with their stages. */
	error = read_identities(reader, true);
	if (error != 0)
		return error;

	/* The command buffers. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > QUEUE_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		commands[index] = drv_i915_wire_read_u64(reader);
	*command_count = (uint32_t)count;

	/* The signals. */
	error = read_identities(reader, false);
	if (error != 0)
		return error;

	/* Succeeded: the submit is read. */
	return 0;
}

/* Reads a semaphore list [count][count]{identity}, and with stages its [count]{stage}. */
static int
read_identities(
	struct i915_wire_reader *reader,
	bool stages)
{
	uint64_t count;
	uint64_t index;

	/* The semaphores. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > QUEUE_ITEMS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++)
		(void)drv_i915_wire_read_u64(reader);

	/* Their stages. */
	if (stages) {
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > QUEUE_ITEMS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++)
			(void)drv_i915_wire_read_u32(reader);
	}

	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the list is read. */
	return 0;
}

/* Reports a submission's VkResult: work the core did not finish loses the device. */
static uint32_t
submit_result(
	int error)
{
	uint32_t result;

	/* Work that did not finish. */
	if (error == EIO || error == ETIMEDOUT)
		return (uint32_t)VK_ERROR_DEVICE_LOST;

	/* Any other error as the executor maps it. */
	result = powervr_vk_result(error);

	/* Succeeded: the result. */
	return result;
}

/*
 * vkCreateFence and vkCreateSemaphore: [device][present][sType][pNext]
 * [flags] and the create's tail -> the create's reply.  A fence created
 * signaled starts signaled.
 */
static int
create_sync(
	struct powervr_vk_session *session,
	enum i915_vk_object_kind kind,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct powervr_vk_fence *fence;
	uint64_t identity;
	uint32_t flags;

	/* The device, the presence word, sType, pNext and the flags, then the tail. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	identity = powervr_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A fence's state; a semaphore's record is nothing but itself. */
	fence = kern_calloc(1, sizeof(*fence));
	if (fence != NULL && kind == I915_VK_OBJ_FENCE && (flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0)
		fence->signaled = true;

	/* Publishes it and answers. */
	powervr_vk_create_reply(session, reply, kind, identity, fence, powervr_vk_free_payload, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkResetFences: [device][count][count]{fence} -> [result]. */
static int
reset_fences(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct powervr_vk_fence *fence;
	uint64_t count;
	uint64_t index;
	uint64_t identity;

	/* How many fences follow. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > QUEUE_SUBMITS_MAX * QUEUE_ITEMS_MAX)
		return EINVAL;

	/* Each known fence is unsignaled. */
	for (index = 0; index < count; index++) {
		identity = drv_i915_wire_read_u64(reader);
		fence = powervr_vk_payload(session, I915_VK_OBJ_FENCE, identity);
		if (fence != NULL) {
			fence->signaled = false;
			fence->kind = POWERVR_POINT_NONE;
			fence->value = 0;
		}
	}

	if (reader->error != 0)
		return EINVAL;

	/* Success. */
	drv_i915_wire_reply_u32(reply, (uint32_t)VK_SUCCESS);

	/* Succeeded: the reply carries the result. */
	return 0;
}

/* vkGetFenceStatus: [device][fence] -> [VK_SUCCESS or VK_NOT_READY]. */
static int
fence_status(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct powervr_vk_fence *fence;
	uint64_t identity;
	int reached;
	uint32_t result;

	/* The fence behind the device. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Finished render jobs give their memory back. */
	powervr_render_jobs_reap(session->owner->render, session->owner, 0);

	/* Signaled once its point is reached; a lost session's work never completes. */
	result = (uint32_t)VK_NOT_READY;
	fence = powervr_vk_payload(session, I915_VK_OBJ_FENCE, identity);
	if (fence != NULL && !fence->signaled) {
		reached = powervr_session_point_reached(session->owner, fence->kind, fence->value);
		if (reached)
			fence->signaled = true;

		/* A compute stream seen to finish is the compute stage's ok. */
		if (reached && fence->kind == POWERVR_POINT_COMPUTE)
			powervr_stage_ok(POWERVR_STAGE_COMPUTE);
	}
	if (fence != NULL && fence->signaled)
		result = (uint32_t)VK_SUCCESS;
	if (fence != NULL && !fence->signaled && (session->owner->lost || session->abandoned))
		result = (uint32_t)VK_ERROR_DEVICE_LOST;
	drv_i915_wire_reply_u32(reply, result);

	/* Succeeded: the reply carries the status. */
	return 0;
}

/*
 * Makes the executor's memory: the program buffers mapped at their
 * places in the session's address space, the stream's resource, and the
 * fence program in its slot.
 */
static int
executor_make(
	struct powervr_vk_session *session)
{
	struct powervr_vk_executor *executor;
	struct powervr_render *render;
	struct powervr_buffer *stream;
	uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS];
	uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* The record. */
	render = session->owner->render;
	executor = kern_calloc(1, sizeof(*executor));
	if (executor == NULL)
		return ENOMEM;
	session->executor = executor;

	/* The shading cluster and the data sequencer programs, at their places in their heaps. */
	error = map_program_buffer(session->owner, QUEUE_USC_BYTES, POWERVR_USC_HEAP_BASE + QUEUE_USC_OFFSET, &executor->usc);
	if (error != 0)
		return error;
	executor->mapped |= MAPPED_USC;

	error = map_program_buffer(session->owner, QUEUE_PDS_BYTES, POWERVR_PDS_HEAP_BASE + QUEUE_PDS_OFFSET, &executor->pds);
	if (error != 0)
		return error;
	executor->mapped |= MAPPED_PDS;

	/* The stream's buffer, at an address of the session's heap; the resource takes the reference. */
	error = powervr_buffer_create(render, QUEUE_STREAM_BYTES, &stream);
	if (error != 0)
		return error;
	error = powervr_resource_make(session->owner, stream, 1, 0, &executor->stream);
	powervr_buffer_put(render, stream);
	if (error != 0)
		return error;

	/* Has the core drop stale table entries before it walks the program mappings. */
	mutex_lock(&render->core->lock);

	error = powervr_command_mmu_flush(render->core);

	mutex_unlock(&render->core->lock);
	if (error != 0)
		return error;

	/* The fence program, in slot 0. */
	error = powervr_compute_fence_program(data, code, &count);
	if (error != 0)
		return error;
	put_word(executor->pds, 0, data[0]);
	put_word(executor->pds, 4, data[1]);
	for (index = 0; index < count; index++)
		put_word(executor->pds, QUEUE_PDS_CODE + index * 4U, code[index]);

	/* The shared register allocation's program, after the fence's in slot 0. */
	error = powervr_compute_shared_program(data, code, &count);
	if (error != 0)
		return error;
	put_word(executor->pds, QUEUE_SHARED_DATA, data[0]);
	put_word(executor->pds, QUEUE_SHARED_DATA + 4U, data[1]);
	for (index = 0; index < count; index++)
		put_word(executor->pds, QUEUE_SHARED_CODE + index * 4U, code[index]);

	/* Succeeded: submissions can build their streams. */
	return 0;
}

/* Takes a program buffer and maps it at its place, around the system level cache. */
static int
map_program_buffer(
	struct powervr_session *owner,
	uint64_t bytes,
	uint64_t address,
	struct powervr_buffer **result)
{
	struct powervr_buffer *buffer;
	unsigned flags;
	int error;

	/* A zeroed, contiguous buffer with its uncached view. */
	*result = NULL;
	error = powervr_buffer_create(owner->render, bytes, &buffer);
	if (error != 0)
		return error;

	/* The attributes of a buffer the processor writes. */
	flags = powervr_gpu_buffer_flags(1, 0);

	/* Maps it at its place. */
	mutex_lock(&owner->render->core->lock);

	error = powervr_gpu_mmu_map(&owner->mmu, address, buffer->run.paddr, buffer->run.size, flags);

	mutex_unlock(&owner->render->core->lock);

	/* Not mapped: the buffer goes back. */
	if (error != 0) {
		powervr_buffer_put(owner->render, buffer);
		return error;
	}

	/* Succeeded: the buffer is mapped. */
	*result = buffer;
	return 0;
}

/*
 * Adds one command buffer's dispatches to the stream, with the pipeline
 * and sets each one finds bound, and runs its render pass instances in
 * their places: the dispatches before an instance run first.
 */
static int
build_command_buffer(
	struct submission *build,
	const struct powervr_vk_command_buffer *command)
{
	const struct powervr_vk_record *entry;
	struct powervr_render_job *jobs;
	uint32_t job_count;
	uint32_t begins;
	uint32_t expected;
	uint32_t draws;
	uint32_t opcode;
	uint32_t in_instance;
	uint32_t next_job;
	uint32_t index;
	uint32_t set;
	uint64_t started;
	int error;

	/* The jobs each instance takes: one, and one more for each further run of POWERVR_RENDER_DRAWS draws. */
	begins = 0;
	expected = 0;
	draws = 0;
	for (index = 0; index < command->record_count; index++) {
		opcode = command->records[index].opcode;
		if (opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			begins++;
			draws = 0;
		} else if (opcode == GPU_OP_CMD_DRAW || opcode == GPU_OP_CMD_DRAW_INDEXED) {
			draws++;
		} else if (opcode == GPU_OP_CMD_END_RENDER_PASS) {
			expected += draws == 0 ? 1U : (draws + POWERVR_RENDER_DRAWS - 1U) / POWERVR_RENDER_DRAWS;
		}
	}

	/* The instances as jobs, as many as they take, when the command buffer has any. */
	jobs = NULL;
	job_count = 0;
	if (begins != 0) {
		if (expected > QUEUE_JOBS_MAX)
			return E2BIG;
		jobs = kern_calloc(expected + 1U, sizeof(*jobs));
		if (jobs == NULL)
			return ENOMEM;
		started = powervr_timing_now();
		error = powervr_vk_render_jobs(command, jobs, expected + 1U, &job_count);
		powervr_timing_add(POWERVR_TIMING_RENDER_JOBS, started);
		if (error != 0) {
			kern_free(jobs);
			return error;
		}
	}

	/* Nothing is bound at a command buffer's start. */
	build->pipeline = NULL;
	for (set = 0; set < POWERVR_VK_SETS_MAX; set++)
		build->sets[set] = NULL;

	/* Each record in order. */
	next_job = 0;
	error = 0;
	for (index = 0; error == 0 && index < command->record_count; index++) {
		entry = &command->records[index];
		if (entry->opcode == GPU_OP_CMD_BIND_PIPELINE) {
			build->pipeline = entry->objects[0]->payload;
		} else if (entry->opcode == GPU_OP_CMD_BIND_DESCRIPTOR_SETS) {
			/* Object i is set first + i; a null set leaves its slot as it was. */
			for (set = 0; set < entry->object_count; set++) {
				if (entry->objects[set] != NULL)
					build->sets[entry->first + set] = entry->objects[set]->payload;
			}
		} else if (entry->opcode == GPU_OP_CMD_DISPATCH) {
			error = build_dispatch(build, entry->values[0]);
		} else if (entry->opcode == GPU_OP_CMD_COPY_BUFFER || entry->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || entry->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER ||
		    entry->opcode == GPU_OP_CMD_COPY_IMAGE || entry->opcode == GPU_OP_CMD_BLIT_IMAGE || entry->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			/*
			 * A transfer: the dispatches before it run first; the firmware
			 * runs it after the session's earlier work and it is left
			 * running, the session's last work.
			 */
			error = run_flush(build);
			if (error == 0) {
				started = powervr_timing_now();
				build->sent++;
				error = powervr_vk_transfer_run(build->session, entry);
				powervr_timing_add(POWERVR_TIMING_TRANSFER, started);
			}
		} else if (entry->opcode == GPU_OP_CMD_BEGIN_RENDER_PASS) {
			/* The dispatches before it run first, then the instance's jobs in order. */
			error = run_flush(build);
			if (error == 0 && job_count == expected) {
				in_instance = instance_jobs(command, index);
				while (error == 0 && in_instance-- != 0U)
					error = run_job(build, &jobs[next_job++]);
			}
		}
	}

	/* Instances dropped from the jobs: the rest run after the command buffer's dispatches before them. */
	while (error == 0 && job_count != expected && next_job < job_count) {
		error = run_flush(build);
		if (error == 0)
			error = run_job(build, &jobs[next_job++]);
	}

	/* The jobs' bookkeeping goes. */
	kern_free(jobs);
	if (error != 0)
		return error;

	/* Succeeded: the command buffer's dispatches are in the stream and its render passes ran. */
	return 0;
}

/*
 * Adds one dispatch: a kernel per workgroup running the pipeline's
 * shader compiled for that workgroup, then a fence kernel.
 *
 * The executor has no data sequencer program that fills shared
 * registers yet, so each workgroup's program is compiled with its
 * shared register words (the bound buffers' addresses, the workgroup's
 * id and count) as constants.  A shader that reads past its buffers
 * faults on the core's MMU rather than being refused here.
 */
static int
build_dispatch(
	struct submission *build,
	uint32_t groups)
{
	struct powervr_shader_workspace *workspace;
	struct powervr_shader_program *program;
	const struct powervr_vk_pipeline *pipeline;
	uint8_t *code;
	uint32_t kernels;
	int error;

	/* No workgroup, no work. */
	if (groups == 0)
		return 0;

	/*
	 * The executor's programs and stream are rewritten: a stream still
	 * running finishes first, as does a transfer left running (which may
	 * follow a stream still running).
	 */
	if (build->session->owner->last_kind == POWERVR_POINT_COMPUTE || build->session->owner->last_kind == POWERVR_POINT_TRANSFER) {
		error = order_after(build, POWERVR_POINT_NONE);
		if (error != 0)
			return error;
	}

	/* A bound pipeline. */
	pipeline = build->pipeline;
	if (pipeline == NULL)
		return EINVAL;

	/* Room for its kernels: one of all the workgroups, or one a workgroup. */
	kernels = groups;
	if (POWERVR_EXECUTOR_ONE_KERNEL)
		kernels = 1U;
	if (kernels > QUEUE_KERNELS_MAX - build->kernels)
		return E2BIG;

	/* The compiler's working memory. */
	workspace = kern_malloc(powervr_shader_workspace_size());
	if (workspace == NULL)
		return ENOMEM;

	/* The program's description. */
	program = kern_malloc(sizeof(*program));
	if (program == NULL) {
		kern_free(workspace);
		return ENOMEM;
	}

	/* The room for the compiled code. */
	code = kern_malloc(QUEUE_USC_ROOM);
	if (code == NULL) {
		kern_free(program);
		kern_free(workspace);
		return ENOMEM;
	}

	/* Compiles the shader and adds its kernels. */
	error = dispatch_kernels(build, pipeline, groups, kernels, workspace, program, code);

	/* The working memory goes, whatever the outcome. */
	kern_free(code);
	kern_free(program);
	kern_free(workspace);

	/* Reports a shader that did not compile or kernels that did not fit. */
	if (error != 0)
		return error;

	/* The fence: the next dispatch starts after this one's work. */
	if (POWERVR_EXECUTOR_FENCE_KERNEL) {
		error = build_fence(build);
		if (error != 0)
			return error;
	}

	build->dispatches++;

	/* Succeeded: the dispatch is in the stream. */
	return 0;
}

/*
 * Compiles a dispatch's shader and adds its kernels: one in all, the data
 * sequencer giving each workgroup its id, or one a workgroup with the id
 * compiled in.
 */
static int
dispatch_kernels(
	struct submission *build,
	const struct powervr_vk_pipeline *pipeline,
	uint32_t groups,
	uint32_t kernels,
	struct powervr_shader_workspace *workspace,
	struct powervr_shader_program *program,
	uint8_t *code)
{
	uint32_t values[POWERVR_SHADER_REQUESTS * 4U];
	struct powervr_ir_shader *shader;
	uint32_t kernel_groups;
	uint32_t group;
	uint32_t index;
	int error;

	/* What the shader asks for. */
	error = powervr_shader_read(pipeline->words, pipeline->word_count, POWERVR_SHADER_COMPUTE, NULL, workspace, program, &shader);
	if (error != 0)
		return error;

	/* Refuses a workgroup one task cannot hold. */
	if (program->workgroup[0] == 0 || program->workgroup[0] > POWERVR_COMPUTE_INSTANCES)
		return ENOTSUP;

	/* Refuses more shared words than the requests hold. */
	if (program->shareds > POWERVR_SHADER_REQUESTS * 4U)
		return ENOTSUP;

	/* The shared register words: the buffers' addresses and the workgroup count. */
	error = build_shared(build, program, groups, values);
	if (error != 0)
		return error;

	/* The workgroups each kernel runs: all of them in the one kernel, else one. */
	kernel_groups = 1U;
	if (POWERVR_EXECUTOR_ONE_KERNEL)
		kernel_groups = groups;

	/* Compiles and adds each kernel. */
	for (group = 0; group < kernels; group++) {
		/* The workgroup's id, where the program reads it from its shared words. */
		for (index = 0; index < program->request_count; index++) {
			if (program->requests[index].kind == POWERVR_SHADER_REQUEST_WORKGROUP_ID)
				values[program->requests[index].shared] = group;
		}

		/* The program with its shared words as constants. */
		error = powervr_shader_compile_inline(pipeline->words, pipeline->word_count, POWERVR_SHADER_COMPUTE, NULL, values, program->shareds, workspace, program, code, QUEUE_USC_ROOM);
		if (error != 0) {
			kern_logf("powervr: vk: the compute shader did not compile (%d)\n", error);
			return error;
		}

		/* The diagnostics: the first and last kernel's places. */
		if (POWERVR_DIAGNOSTICS && (group == 0 || group + 1U == groups)) {
			kern_logf("powervr: vk: kernel %u (workgroup %u): usc 0x%llx, pds data 0x%llx\n", (unsigned)build->kernels, (unsigned)group,
				  (unsigned long long)(POWERVR_USC_HEAP_BASE + QUEUE_USC_OFFSET + build->kernels * QUEUE_USC_ROOM),
				  (unsigned long long)(POWERVR_PDS_HEAP_BASE + QUEUE_PDS_OFFSET + (build->kernels + 1U) * QUEUE_PDS_SLOT));
		}

		/* The diagnostics: the first kernel's shared words. */
		if (POWERVR_DIAGNOSTICS && group == 0) {
			for (index = 0; index < program->shareds; index++)
				kern_logf("powervr: vk: dispatch: shared word %u = 0x%08x\n", (unsigned)index, (unsigned)values[index]);
		}

		/* Says once what the dispatch compiled to. */
		if (group == 0) {
			kern_logf("powervr: vk: dispatch of %u workgroups: the compute shader compiled to %u bytes, %u temps, %u shared words\n",
				  groups, (unsigned)program->length, program->temps, program->shareds);
		}

		/* Mesa's shared register allocation kernel goes before the first. */
		if (POWERVR_EXECUTOR_MESA_STREAM && group == 0) {
			error = build_shared_kernel(build);
			if (error != 0)
				return error;
		}

		/* The kernel itself. */
		error = build_kernel(build, code, program->length, program->temps, program->workgroup[0], kernel_groups);
		if (error != 0)
			return error;
	}

	/* Succeeded: every kernel of the dispatch is in the stream. */
	return 0;
}

/*
 * Works out the shared register words a program asks for: the bound
 * buffers' addresses, the workgroup count (and zero for the workgroup
 * id, which the caller sets per workgroup).
 */
static int
build_shared(
	struct submission *build,
	const struct powervr_shader_program *program,
	uint32_t groups,
	uint32_t *values)
{
	const struct powervr_shader_request *request;
	const struct powervr_vk_descriptor_set *set;
	const struct powervr_vk_descriptor_slot *slot;
	const struct powervr_vk_buffer *buffer;
	uint64_t address;
	uint32_t index;
	uint32_t word;
	int error;

	/* Each request's words. */
	for (index = 0; index < program->request_count; index++) {
		request = &program->requests[index];
		for (word = 0; word < request->words; word++)
			values[request->shared + word] = 0;

		switch (request->kind) {
		case POWERVR_SHADER_REQUEST_DESCRIPTOR:
			/* Only a buffer's address: the bound set's binding, its first byte. */
			if (request->part != POWERVR_SHADER_PART_BUFFER_ADDRESS)
				return ENOTSUP;
			if (request->set >= POWERVR_VK_SETS_MAX || request->binding >= POWERVR_VK_BINDINGS_MAX)
				return ENOTSUP;
			set = build->sets[request->set];
			if (set == NULL)
				return EINVAL;
			slot = &set->slots[request->binding];
			if (slot->buffer == NULL)
				return EINVAL;
			buffer = slot->buffer->payload;
			if (slot->offset > buffer->size)
				return EINVAL;
			error = powervr_vk_buffer_address(buffer, &address);
			if (error != 0)
				return error;
			address += slot->offset;
			if (POWERVR_DIAGNOSTICS) {
				kern_logf("powervr: vk: dispatch: set %u binding %u buffer 0x%llx-0x%llx (%llu bytes, offset %llu) in shared words %u-%u\n",
				  (unsigned)request->set, (unsigned)request->binding, (unsigned long long)address,
				  (unsigned long long)(address - slot->offset + buffer->size), (unsigned long long)buffer->size,
				  (unsigned long long)slot->offset, (unsigned)request->shared, (unsigned)(request->shared + 1U));
			}
			values[request->shared] = (uint32_t)address;
			values[request->shared + 1U] = (uint32_t)(address >> 32);
			break;
		case POWERVR_SHADER_REQUEST_WORKGROUP_ID:
			/* Set per workgroup; one deep in y and z. */
			if (POWERVR_DIAGNOSTICS)
				kern_logf("powervr: vk: dispatch: workgroup id in shared word %u\n", (unsigned)request->shared);
			break;
		case POWERVR_SHADER_REQUEST_NUM_WORKGROUPS:
			if (POWERVR_DIAGNOSTICS)
				kern_logf("powervr: vk: dispatch: workgroup count %u in shared word %u\n", groups, (unsigned)request->shared);
			values[request->shared] = groups;
			for (word = 1; word < request->words; word++)
				values[request->shared + word] = 1;
			break;
		default:
			/* Push constants and the rest wait for the data sequencer's shared loads. */
			return ENOTSUP;
		}
	}

	/* Succeeded: the words. */
	return 0;
}

/*
 * Adds one kernel: its shading cluster program in its room, the data
 * sequencer program that issues it in its slot, and its control stream
 * words.
 */
static int
build_kernel(
	struct submission *build,
	const uint8_t *code,
	size_t length,
	uint32_t temps,
	uint32_t instances,
	uint32_t workgroups)
{
	struct powervr_compute_kernel kernel;
	uint32_t data[POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS];
	uint32_t pds[POWERVR_COMPUTE_PDS_CODE_MAX];
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	uint32_t room;
	uint32_t slot;
	uint32_t code_at;
	uint32_t data_words;
	unsigned field;
	unsigned count;
	size_t index;
	int error;

	/* The program, written through the uncached view into the kernel's room. */
	if (length > QUEUE_USC_ROOM)
		return E2BIG;
	room = build->kernels * QUEUE_USC_ROOM;
	for (index = 0; index < length; index++)
		build->executor->usc->view[room + index] = code[index];

	/*
	 * The program that issues it, in the slot after the fence's, with
	 * the task's temporaries field by the one rule the compute programs
	 * share (powervr_compute_task_field).
	 */
	error = powervr_compute_task_field(temps, &field);
	if (error != 0)
		return error;
	if (POWERVR_EXECUTOR_ONE_KERNEL)
		error = powervr_compute_workgroup_program(QUEUE_USC_OFFSET + room, field, data, pds, &count);
	else
		error = powervr_compute_kick_program(QUEUE_USC_OFFSET + room, field, data, pds, &count);
	if (error != 0)
		return error;
	slot = (build->kernels + 1U) * QUEUE_PDS_SLOT;
	code_at = QUEUE_KERNEL_PDS_CODE;
	data_words = QUEUE_KERNEL_DATA_WORDS;
	for (index = 0; index < data_words; index++)
		put_word(build->executor->pds, slot + (uint32_t)index * 4U, data[index]);
	for (index = 0; index < count; index++)
		put_word(build->executor->pds, slot + code_at + (uint32_t)index * 4U, pds[index]);

	/* The kernel: one workgroup of the given instances, on any cluster, no shared registers. */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = QUEUE_PDS_OFFSET + slot;
	kernel.code_offset = QUEUE_PDS_OFFSET + slot + code_at;
	kernel.data_words = data_words;
	kernel.usc_inputs = QUEUE_KERNEL_USC_INPUTS;
	kernel.shared_from_usc = 0U;
	kernel.pds_temporaries = QUEUE_KERNEL_PDS_INPUTS;
	kernel.workgroups[0] = workgroups;
	kernel.usc_common = QUEUE_KERNEL_COMMON;
	kernel.usc_temporaries = temps;
	kernel.instances = instances;
	kernel.max_instances = 0;
	kernel.any_cluster = 1;

	/* In Mesa's stream the shading cluster gives the kernel its shared data. */
	if (POWERVR_EXECUTOR_MESA_STREAM)
		kernel.shared_from_usc = 1U;

	/* The kernel's words. */
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;
	put_stream_words(build, words, POWERVR_COMPUTE_KERNEL_WORDS);
	build->kernels++;

	/* Succeeded: the kernel is in the stream. */
	return 0;
}

/* Adds Mesa's shared register allocation kernel before a dispatch (POWERVR_EXECUTOR_MESA_STREAM). */
static int
build_shared_kernel(
	struct submission *build)
{
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	int error;

	/* The allocation kernel's words, its program in slot 0. */
	error = powervr_compute_shared_kernel_words(QUEUE_PDS_OFFSET + QUEUE_SHARED_DATA, QUEUE_PDS_OFFSET + QUEUE_SHARED_CODE, words);
	if (error != 0)
		return error;
	put_stream_words(build, words, POWERVR_COMPUTE_KERNEL_WORDS);

	/* Succeeded: the allocation is in the stream. */
	return 0;
}

/* Adds a fence kernel: it waits for the work before it and frees the shared registers. */
static int
build_fence(
	struct submission *build)
{
	struct powervr_compute_kernel kernel;
	uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS];
	int error;

	/* The fence program of slot 0. */
	kern_memset(&kernel, 0, sizeof(kernel));
	kernel.data_offset = QUEUE_PDS_OFFSET;
	kernel.code_offset = QUEUE_PDS_OFFSET + QUEUE_PDS_CODE;
	kernel.data_words = POWERVR_COMPUTE_PDS_DATA_WORDS;
	kernel.instances = 1;
	kernel.max_instances = POWERVR_COMPUTE_FENCE_INSTANCES;
	kernel.fence = 1;
	kernel.shared_from_pds = 1;
	kernel.common_shared = 1;
	kernel.any_cluster = 1;
	error = powervr_compute_kernel_words(&kernel, words);
	if (error != 0)
		return error;
	put_stream_words(build, words, POWERVR_COMPUTE_KERNEL_WORDS);

	/* Succeeded: the fence is in the stream. */
	return 0;
}

/* Appends words to the stream through the stream buffer's uncached view. */
static void
put_stream_words(
	struct submission *build,
	const uint32_t *words,
	unsigned count)
{
	unsigned index;

	/* Each word at the stream's end. */
	for (index = 0; index < count; index++) {
		put_word(build->executor->stream->buffer, build->stream_at, words[index]);
		build->stream_at += 4U;
	}
}

/* Writes one little-endian word into a buffer through its uncached view. */
static void
put_word(
	struct powervr_buffer *buffer,
	uint32_t offset,
	uint32_t word)
{
	/* One aligned store through the view. */
	*(volatile uint32_t *)(buffer->view + offset) = word;
}

/*
 * Ends the stream and has the core run it: one compute command with the
 * session's next timeline value, then a bounded wait for it.  A wait that
 * runs out abandons the executor's memory to the core.
 *
 * Procedure referenced:
 * zedBSD src/drivers/gpu/powervr/compute-check.c (the places of the
 * programs, the kick program per kernel, the fence kernel and the end of
 * the stream, the compute command and its wait).
 */
static int
run(
	struct submission *build)
{
	struct powervr_session *owner;
	struct powervr_render *render;
	uint32_t command[POWERVR_COMPUTE_COMMAND_WORDS];
	uint32_t terminate;
	uint64_t stream;
	uint32_t value;
	uint64_t started;
	int error;

	/* When the run started, for its timing. */
	started = powervr_timing_now();

	/* Another context's work finishes first (the stream may read what it wrote). */
	error = order_after(build, POWERVR_POINT_COMPUTE);
	if (error != 0)
		return error;

	/* The end of the stream. */
	terminate = powervr_compute_terminate_word();
	put_stream_words(build, &terminate, 1);

	/* The command names the stream and the resume area before it. */
	owner = build->session->owner;
	render = owner->render;
	stream = build->executor->stream->address;
	if (POWERVR_DIAGNOSTICS) {
		kern_logf("powervr: vk: run: stream 0x%llx, words 0x%llx-0x%llx of 0x%x bytes, %u kernels; usc 0x%llx-0x%llx, pds 0x%llx-0x%llx\n",
		  (unsigned long long)stream, (unsigned long long)(stream + QUEUE_STREAM_FIRST), (unsigned long long)(stream + build->stream_at),
		  QUEUE_STREAM_BYTES, (unsigned)build->kernels,
		  (unsigned long long)(POWERVR_USC_HEAP_BASE + QUEUE_USC_OFFSET), (unsigned long long)(POWERVR_USC_HEAP_BASE + QUEUE_USC_OFFSET + build->kernels * QUEUE_USC_ROOM),
		  (unsigned long long)(POWERVR_PDS_HEAP_BASE + QUEUE_PDS_OFFSET), (unsigned long long)(POWERVR_PDS_HEAP_BASE + QUEUE_PDS_OFFSET + (build->kernels + 1U) * QUEUE_PDS_SLOT));
	}
	powervr_compute_command(stream + QUEUE_STREAM_FIRST, stream, command);
	if (POWERVR_COMPUTE_SWITCH_STATE)
		command[POWERVR_COMPUTE_RESUME_PDS1_WORD] = powervr_compute_resume_pds1(0);

	/* Sends it with the next timeline value. */
	mutex_lock(&render->core->lock);

	value = owner->submitted + 1U;
	build->sent++;
	error = powervr_session_kick_compute(render, owner, value, command, POWERVR_COMPUTE_COMMAND_WORDS);
	if (error == 0)
		owner->submitted = value;

	mutex_unlock(&render->core->lock);
	if (error != 0) {
		kern_logf("powervr: vk: compute kick failed (%d)\n", error);
		powervr_stage_fail(POWERVR_STAGE_COMPUTE, powervr_stage_reason_of(error));
		return error;
	}

	/* The stream runs on; later callbacks and fences complete at its point. */
	powervr_session_point_set(owner, POWERVR_POINT_COMPUTE, value);
	powervr_timing_add(POWERVR_TIMING_DISPATCH, started);

	/* A synchronous submission waits for it here. */
	error = settle(build);
	if (error != 0)
		return error;

	/* Succeeded: the stream was kicked. */
	return 0;
}

/*
 * Waits, when submissions are synchronous (POWERVR_ASYNC_SUBMIT 0), for
 * the work just kicked, as the submission did before it became
 * asynchronous: within QUEUE_WAIT_MS, else the session is abandoned.
 * The finished render job's memory is released.
 */
static int
settle(
	struct submission *build)
{
	struct powervr_session *owner;
	int error;

	/* Asynchronous submissions do not wait. */
	if (POWERVR_ASYNC_SUBMIT)
		return 0;

	/* The last work's point. */
	owner = build->session->owner;
	error = powervr_session_point_wait(owner, owner->last_kind, owner->last_value, QUEUE_WAIT_MS);
	if (error != 0) {
		kern_logf("powervr: vk: work did not finish (%d); the session is abandoned\n", error);
		build->session->abandoned = true;
		owner->abandoned = 1;
		if (owner->last_kind == POWERVR_POINT_COMPUTE)
			powervr_stage_fail(POWERVR_STAGE_COMPUTE, powervr_stage_reason_of(error));
		return error;
	}

	/* The compute stage's ok, and the finished job's memory. */
	if (owner->last_kind == POWERVR_POINT_COMPUTE)
		powervr_stage_ok(POWERVR_STAGE_COMPUTE);
	powervr_render_jobs_reap(owner->render, owner, 0);

	/* Succeeded: the work finished. */
	return 0;
}

/*
 * Orders the session's next work of a kind after its last work: work on
 * the same context keeps the firmware's order; anything else (and a
 * transfer, kind POWERVR_POINT_NONE, after anything) waits for the last
 * work's point, bounded by POWERVR_PROGRESS_MS.  A wait that runs out
 * abandons the session, whose work the core may still run.
 */
static int
order_after(
	struct submission *build,
	unsigned kind)
{
	struct powervr_session *owner;
	uint64_t started;
	int error;

	/*
	 * The same context: the firmware runs its kicks in order; a render
	 * after a transfer has the firmware wait for it (render-job.c).
	 */
	owner = build->session->owner;
	if (kind != POWERVR_POINT_NONE && owner->last_kind == kind)
		return 0;
	if (kind == POWERVR_POINT_FRAGMENT && owner->last_kind == POWERVR_POINT_TRANSFER)
		return 0;

	/* The last work's point. */
	started = powervr_timing_now();
	error = powervr_session_point_wait(owner, owner->last_kind, owner->last_value, POWERVR_PROGRESS_MS);
	powervr_timing_add(POWERVR_TIMING_ORDER_WAIT, started);
	if (error != 0) {
		kern_logf("powervr: vk: earlier work did not finish (%d); the session is abandoned\n", error);
		build->session->abandoned = true;
		owner->abandoned = 1;
		if (owner->last_kind == POWERVR_POINT_COMPUTE)
			powervr_stage_fail(POWERVR_STAGE_COMPUTE, powervr_stage_reason_of(error));
		return error;
	}

	/* The compute stage's ok, once a stream was seen to finish. */
	if (owner->last_kind == POWERVR_POINT_COMPUTE)
		powervr_stage_ok(POWERVR_STAGE_COMPUTE);

	/* Succeeded: the earlier work finished. */
	return 0;
}

/* Runs the dispatches the stream holds so far, and starts the stream again. */
static int
run_flush(
	struct submission *build)
{
	int error;

	/* Nothing to run. */
	if (build->kernels == 0)
		return 0;

	/* The stream, then an empty one. */
	error = run(build);
	if (error != 0)
		return error;

	build->stream_at = QUEUE_STREAM_FIRST;
	build->kernels = 0;

	/* Succeeded: the dispatches ran. */
	return 0;
}

/* Runs one render pass instance; one that does not finish leaves the session abandoned. */
static int
run_job(
	struct submission *build,
	const struct powervr_render_job *job)
{
	struct powervr_session *owner;
	int error;

	/* The work of another context finishes first (a render may read what it wrote). */
	owner = build->session->owner;
	error = order_after(build, POWERVR_POINT_FRAGMENT);
	if (error != 0)
		return error;

	/* The job on the session's render context (counted as sent even when it fails); a synchronous submission waits for it. */
	build->sent++;
	error = powervr_render_job_run(owner->render, owner, job);
	if (error == 0)
		error = settle(build);
	if (error == ETIMEDOUT)
		build->session->abandoned = true;
	if (error != 0) {
		kern_logf("powervr: vk: a render pass did not run (%d)\n", error);
		return error;
	}

	/* Succeeded: the instance ran. */
	return 0;
}

/* Counts the jobs the instance beginning at a record takes: one, and one more for each further run of POWERVR_RENDER_DRAWS draws. */
static uint32_t
instance_jobs(
	const struct powervr_vk_command_buffer *command,
	uint32_t begin)
{
	uint32_t index;
	uint32_t draws;
	uint32_t opcode;
	uint32_t jobs;

	/* The draws up to the instance's end. */
	draws = 0;
	for (index = begin + 1U; index < command->record_count; index++) {
		opcode = command->records[index].opcode;
		if (opcode == GPU_OP_CMD_END_RENDER_PASS)
			break;
		if (opcode == GPU_OP_CMD_DRAW || opcode == GPU_OP_CMD_DRAW_INDEXED)
			draws++;
	}

	/* Succeeded: the jobs. */
	jobs = 1;
	if (draws != 0)
		jobs = (draws + POWERVR_RENDER_DRAWS - 1U) / POWERVR_RENDER_DRAWS;
	return jobs;
}
