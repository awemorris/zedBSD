/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR render node: sessions with a memory context of their own,
 * storage and blob allocations mapped into it, CPU views of them, and
 * sharing of allocations between opens and with the display node.
 *
 * Every allocation is one physically contiguous run below the core's DMA
 * limit.  The core does not snoop the processor's caches, so the host
 * reaches an allocation only through its uncached alias, in the kernel
 * and in user mappings alike; the core reaches it through the session's
 * page tables, 4 KiB pages only, never asking for a snoop.  Allocations
 * the processor reads also bypass the core's system level cache.
 *
 * The node is published once the firmware runs, after a check of the
 * firmware processor's writes and a short self-test of the queue.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <drivers/gpu/gpu-scanout.h>
#include <drivers/gpu/gpu-share.h>
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <uapi/errno.h>
#include <uapi/gpu.h>
#include <uapi/gpu-scanout.h>
#include <uapi/sysctl.h>

#include "drivers/gpu/powervr/render-command.h"
#include "drivers/gpu/powervr/render-private.h"
#include "drivers/gpu/powervr/fixed-area.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/retry.h"
#include "drivers/gpu/powervr/render-pass.h"
#include "drivers/gpu/powervr/vk/vk.h"

/* How long a timeline poll sleeps before it looks again, in milliseconds (a tick at least). */
#define RENDER_POLL_MS 1U

/*
 * How often, and how far apart, a refused registration is tried again.
 */
#define REGISTER_ATTEMPTS	50U
#define REGISTER_PAUSE_MS	100U

/* How long the self-test waits for an empty job, and for a fence it expects to hold. */
#define SELF_TEST_WAIT_MS	1000U
#define SELF_TEST_HOLD_MS	200U

static int render_open(void *device, void **result);
static void render_close(void *device, void *session);
static void border_table_make(struct powervr_render *render, struct powervr_session *session);
static int render_info(void *device, void *session, struct gpu_info *info);
static int render_resource_create(void *device, void *session, const struct gpu_resource_create *request, void **result);
static void render_resource_destroy(void *device, void *session, void *resource);
static int render_blob_create(void *device, void *session, const struct gpu_blob_create *request, void **result, uint32_t *identifier);
static int render_blob_create_placed(void *device, void *session, const struct gpu_blob_create_placed *request, void **result, uint32_t *identifier);
static int blob_make(struct powervr_render *render, struct powervr_session *session, const struct gpu_blob_create *request, const struct gpu_placement *placement, void **result, uint32_t *identifier);
static int render_read(void *device, void *session, void *resource, uint64_t offset, void *bytes, uint32_t length);
static int render_write(void *device, void *session, void *resource, uint64_t offset, const void *bytes, uint32_t length);
static int render_map(void *device, void *session, void *resource, struct drv_gpu_mapping *mapping);
static int render_export(void *device, void *session, void *resource, const struct gpu_image_descriptor *image, void **result);
static void render_release(void *device, void *shared);
static int render_import(void *device, void *session, void *shared, uint32_t flags, void **result, uint32_t *identifier);
static int render_backing(void *device, void *shared, struct drv_gpu_scanout_backing *backing);
static int render_query(void *device, void *session, struct gpu_device_info *info);
static int resource_make(struct powervr_session *session, struct powervr_buffer *buffer, int mappable, int shareable, struct powervr_resource **result);
/* Not a mapping flag: the resource's address comes from the region header window. */
#define RESOURCE_REGION_HEADERS	0x80000000U

static int resource_make_flags(struct powervr_session *session, struct powervr_buffer *buffer, int mappable, int shareable, unsigned extra, struct powervr_resource **result);
static void resource_unmake(struct powervr_session *session, struct powervr_resource *resource);
static void resource_release(struct powervr_session *session, struct powervr_resource *resource);
static void resources_drain(struct powervr_session *session, int force);
static void resource_log(const char *what, const struct powervr_resource *resource, uint64_t fault);
static void blob_list(struct powervr_session *session, struct powervr_resource *resource, uint32_t *identifier);
static int check_image(const struct powervr_buffer *buffer, const struct gpu_image_descriptor *image);
static int make_pages(struct powervr_buffer *buffer);
static int same_image(const struct gpu_image_descriptor *first, const struct gpu_image_descriptor *second);
static void buffer_free(struct powervr_buffer *buffer);
static void pool_put(struct powervr_render *render, struct powervr_buffer *buffer);
static int buffer_make(struct powervr_render *render, uint64_t bytes, bool clear, struct powervr_buffer **result);
static void copy_out(volatile const uint8_t *view, uint8_t *bytes, uint32_t length);
static void copy_in(volatile uint8_t *view, const uint8_t *bytes, uint32_t length);
static int register_attempt(void *context);
static void register_pause(void *context);
static int self_test(struct powervr_render *render, const char **step);
static int render_telemetry(void *context, struct gpu_telemetry_entry *entry);

/*
 * The board's one render node.  Filled by powervr_render_start() on the
 * later stage's thread before the node is published; its fields are then
 * guarded as struct powervr_render says.
 */
static struct powervr_render board_render;

/*
 * The bytes of every render buffer's run that exists, the pooled ones
 * too: buffer_make adds a run's size and buffer_free takes it off, both
 * atomically, so hw.gputelemetry reads it without a lock.  Zero before
 * the first buffer; it lives as long as the kernel.
 */
static volatile uint64_t buffer_run_bytes;

/* The node's sharing operations. */
static const struct drv_gpu_share_ops share_operations = {
	.export_resource = render_export,
	.release = render_release,
	.import_resource = render_import,
	.get_scanout_backing = render_backing,
};

/* The node's role: rendering, with no display of its own. */
static const struct drv_gpu_scanout_ops scanout_operations = {
	.query_device = render_query,
};

/* The node's queue: native commands retired through each session's timeline. */
static const struct drv_gpu_command_ops command_operations = {
	.submit = powervr_render_submit,
	.drain = powervr_render_drain,
};

/* The node's recovery: one session stopped or isolated, or the device faulted and reset. */
static const struct drv_gpu_recovery_ops recovery_operations = {
	.stop_begin = powervr_render_stop_begin,
	.stop_poll = powervr_render_stop_poll,
	.fault = powervr_render_fault,
	.reset = powervr_render_reset,
	.isolate = powervr_render_isolate,
};

/*
 * Publishes the render node over the running firmware.
 */
int
powervr_render_start(
	struct powervr_core *core)
{
	struct powervr_render *render;
	struct powervr_retry retry;
	const char *step;
	unsigned tries;
	int error;

	/* The lock the firmware's buffers and heap are shared under from here. */
	render = &board_render;
	error = mutex_init(&core->lock, LOCK_RANK_DEVICE, "powervr-core");
	if (error != 0)
		return error;

	/* Checks what the processor sees of the firmware processor's writes; it only reports, so a release image skips it. */
	if (POWERVR_DEBUG)
		powervr_coherency_check(core);

	/* Sets up the node's state and its operation table. */
	kern_memset(render, 0, sizeof(*render));
	render->core = core;
	render->next_identity = 1;
	spin_init(&render->queue_lock, LOCK_RANK_DEVICE, "powervr-queue");
	spin_init(&render->buffer_guard, LOCK_RANK_DEVICE, "powervr-buffers");
	waitq_init(&render->service, "powervr-service");
	waitq_init(&render->retired, "powervr-retired");
	waitq_init(&render->reached, "powervr-reached");
	waitq_init(&render->spare_wanted, "powervr-spare");
	render->ops.version = DRV_GPU_INTERFACE_VERSION;
	render->ops.size = sizeof(render->ops);
	render->ops.capabilities = GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | GPU_CAP_NOTIFICATION;
	render->ops.open = render_open;
	render->ops.close = render_close;
	render->ops.get_info = render_info;
	render->ops.resource_create = render_resource_create;
	render->ops.resource_destroy = render_resource_destroy;
	render->ops.blob_create = render_blob_create;
	render->ops.blob_create_placed = render_blob_create_placed;
	render->ops.resource_read = render_read;
	render->ops.resource_write = render_write;
	render->ops.resource_map = render_map;
	render->ops.share = &share_operations;
	render->ops.scanout = &scanout_operations;
	render->ops.commands = &command_operations;
	render->ops.recovery = &recovery_operations;
	powervr_render_runtime_bind(render);
	render->running = 1;

	/* The shading cluster programs of a render context's switch; without them no render context is made. */
	error = powervr_render_switch_shaders(render);
	if (error != 0)
		kern_logf("powervr: render context switch programs not built (%d)\n", error);

	/* Lets the interrupt handler wake the service from here on. */
	core->render = render;

	/* Starts the service that retires commands and reads the firmware's notices. */
	error = powervr_service_start(render);
	if (error != 0) {
		core->render = NULL;
		return error;
	}

	/* Runs one empty job through a session of the driver's own, with its fence and memory. */
	step = "open";
	error = self_test(render, &step);
	if (error != 0) {
		kern_logf("powervr: render self-test %s failed (%d)\n", step, error);
		powervr_render_quarantine(render, error);
		powervr_service_stop(render);
		core->render = NULL;
		return error;
	}

	/* A test kernel runs the driver's own compute work and reports it; it only reports. */
	if (powervr_compute_check != NULL && !POWERVR_FIRMWARE_BOOT_TRACE)
		powervr_compute_check(render);

	/* A test kernel runs the driver's own render and reports it; it only reports. */
	if (powervr_render_check != NULL && !POWERVR_FIRMWARE_BOOT_TRACE)
		powervr_render_check(render);

	/* Registers the node, trying again a bounded number of times while registration is busy. */
	retry.attempt = register_attempt;
	retry.pause = register_pause;
	retry.context = render;
	error = powervr_retry_busy(&retry, REGISTER_ATTEMPTS, &tries);
	if (error != 0) {
		kern_logf("powervr: render node not registered after %u tries (%d)\n", tries, error);
		powervr_render_quarantine(render, error);
		powervr_service_stop(render);
		core->render = NULL;
		return error;
	}

	/* Lists the GPU in hw.gputelemetry, which only reads what the node keeps; a full list leaves it out. */
	(void)kern_gpu_telemetry_register("powervr", render_telemetry, render);

	/* Succeeded: the node is published. */
	kern_logf("powervr: render node ready (self-test ok: empty job, fence, memory)\n");
	return 0;
}

/*
 * Finds the render buffer exported with this image whose backing is this
 * one, and retains it for the caller.
 */
int
powervr_render_lookup(
	const struct gpu_image_descriptor *image,
	const struct drv_gpu_scanout_backing *backing,
	struct powervr_buffer **result)
{
	struct powervr_render *render;
	struct powervr_shared *shared;
	unsigned long enabled;
	int matches;

	/* Only the render node's own page lists name its buffers. */
	render = &board_render;
	*result = NULL;
	if (backing == NULL || image == NULL)
		return ENOTSUP;
	if (backing->page_bytes != POWERVR_GPU_PAGE_BYTES || backing->flags != DRV_GPU_BACKING_CONTIGUOUS)
		return ENOTSUP;

	/* Walks the live exports for the one whose page list this is. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	for (shared = render->exports; shared != NULL; shared = shared->next) {
		if (!shared->has_image || shared->buffer->pages != backing->pages)
			continue;
		if (backing->page_count != shared->buffer->page_count || backing->bytes != shared->buffer->bytes)
			continue;

		/* The image must be the one exported, whatever the core added. */
		matches = same_image(image, &shared->image);
		if (!matches)
			continue;

		/* Takes a reference that outlives the export. */
		shared->buffer->references++;
		*result = shared->buffer;
		break;
	}

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	/* No live export of the render node matches. */
	if (*result == NULL)
		return ENOTSUP;

	/* Succeeded: the caller holds the buffer. */
	return 0;
}

/*
 * Reports a retained buffer's uncached kernel view.
 */
const volatile uint8_t *
powervr_buffer_view(
	const struct powervr_buffer *buffer)
{
	/* Succeeded: the view the buffer was made with. */
	return buffer->view;
}

/*
 * Reports a retained buffer's size in bytes, as allocated.
 */
uint64_t
powervr_buffer_bytes(
	const struct powervr_buffer *buffer)
{
	/* Succeeded: the size asked for at creation. */
	return buffer->bytes;
}

/*
 * Releases a reference taken by powervr_render_lookup().
 */
void
powervr_buffer_release(
	struct powervr_buffer *buffer)
{
	/* Drops it the way the node's own holders do. */
	powervr_buffer_put(&board_render, buffer);
}

/*
 * The pool of freed buffers: kept with their uncached views, so a buffer
 * of the same size is made again without a new view, whose making and
 * ending each write back and drop the run's cached lines one by one (an
 * image's 8 MB took about 30 ms on the board).  Only buffers of at least
 * POWERVR_BUFFER_POOL_LEAST bytes, and at most BUFFER_POOL_BYTES in all.
 */
#define BUFFER_POOL_BYTES	(128ULL * 1024ULL * 1024ULL)

/*
 * Makes a zeroed buffer of at least bytes bytes.
 */
int
powervr_buffer_create(
	struct powervr_render *render,
	uint64_t bytes,
	struct powervr_buffer **result)
{
	int error;

	/* Made and cleared. */
	error = buffer_make(render, bytes, true, result);
	if (error != 0)
		return error;

	/* Succeeded: one reference, the caller's. */
	return 0;
}

/*
 * Makes a buffer of at least bytes bytes without clearing it: its bytes
 * are what the RAM held, so the caller clears it (the core's transfer
 * clears an image's 8 MB in a few ms, the processor's stores and their
 * write-back took about 20 ms) before anything reads it.
 */
int
powervr_buffer_create_uncleared(
	struct powervr_render *render,
	uint64_t bytes,
	struct powervr_buffer **result)
{
	int error;

	/* Made, not cleared. */
	error = buffer_make(render, bytes, false, result);
	if (error != 0)
		return error;

	/* Succeeded: one reference, the caller's. */
	return 0;
}

/* Makes a buffer of at least bytes bytes, cleared when clear is set. */
static int
buffer_make(
	struct powervr_render *render,
	uint64_t bytes,
	bool clear,
	struct powervr_buffer **result)
{
	struct powervr_buffer *buffer;
	volatile uint64_t *words;
	uint64_t rounded;
	uint64_t index;
	uint64_t started;
	void *direct;
	void *view;
	int error;

	/* Refuses an empty buffer and one over the limit, and rounds the rest to whole pages. */
	*result = NULL;
	if (bytes == 0U || bytes > POWERVR_RENDER_MAX_BYTES)
		return EINVAL;
	rounded = (bytes + POWERVR_GPU_PAGE_BYTES - 1U) & ~(uint64_t)(POWERVR_GPU_PAGE_BYTES - 1U);

	/* The buffer's bookkeeping. */
	buffer = kern_calloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return ENOMEM;

	/* One contiguous run the core can name (each step timed, for the buffers of large images). */
	started = powervr_timing_now();
	error = kern_pmem_alloc_limited((size_t)rounded, POWERVR_GPU_PAGE_BYTES, render->core->description.dma_limit, 0, &buffer->run);
	powervr_timing_add(POWERVR_TIMING_BUFFER_RUN, started);
	if (error != 0) {
		kern_free(buffer);
		return error;
	}

	/*
	 * Cleared through the direct map, where the stores are cached (through
	 * the uncached alias a store is a bus write: an image's 8 MB took
	 * about 30 ms); the alias's making below writes the lines back.
	 */
	started = powervr_timing_now();
	direct = kern_pmem_to_kernel(buffer->run.paddr);
	if (clear && direct != NULL)
		kern_memset(direct, 0, (size_t)rounded);
	if (clear)
		powervr_timing_add(POWERVR_TIMING_BUFFER_CLEAR, started);

	/* Its uncached alias; the run's cached lines are pushed out first. */
	started = powervr_timing_now();
	error = kern_pmem_map_uncached(&buffer->run, &view);
	powervr_timing_add(POWERVR_TIMING_BUFFER_ALIAS, started);
	if (error != 0) {
		(void)kern_pmem_free(&buffer->run);
		kern_free(buffer);
		return error;
	}

	/* Without a direct map, cleared through the alias, so neither the core nor a reader sees old bytes. */
	buffer->view = view;
	if (clear && direct == NULL) {
		words = (volatile uint64_t *)buffer->view;
		for (index = 0; index < rounded / 8U; index++)
			words[index] = 0;
	}

	/* The run counts in the render buffers' bytes until it is freed. */
	(void)atomic_u64_fetch_add_relaxed(&buffer_run_bytes, (uint64_t)buffer->run.size);

	/* Succeeded: one reference, the caller's. */
	buffer->bytes = bytes;
	buffer->references = 1;
	*result = buffer;
	return 0;
}

/*
 * Takes a freed buffer whose run holds a new buffer of bytes bytes, and
 * is at most a quarter larger, from the pool, with one reference, the
 * caller's.  Its
 * contents are what its last holder left, so the caller clears it
 * before anything reads it.  ENOENT when the pool has none.
 */
int
powervr_buffer_reuse(
	struct powervr_render *render,
	uint64_t bytes,
	struct powervr_buffer **result)
{
	struct powervr_buffer **link;
	struct powervr_buffer **best;
	struct powervr_buffer *buffer;
	unsigned long enabled;
	uint64_t rounded;

	/* The run's size such a buffer has. */
	*result = NULL;
	if (bytes == 0U || bytes > POWERVR_RENDER_MAX_BYTES)
		return EINVAL;
	rounded = (bytes + POWERVR_GPU_PAGE_BYTES - 1U) & ~(uint64_t)(POWERVR_GPU_PAGE_BYTES - 1U);

	/* The smallest pooled buffer that holds it and is at most a quarter larger, off the pool. */
	buffer = NULL;
	best = NULL;
	enabled = spin_lock_irqsave(&render->buffer_guard);

	for (link = &render->pool; *link != NULL; link = &(*link)->next) {
		if ((*link)->run.size < rounded || (*link)->run.size > rounded + rounded / 4U)
			continue;
		if (best == NULL || (*link)->run.size < (*best)->run.size)
			best = link;
	}
	if (best != NULL) {
		buffer = *best;
		*best = buffer->next;
		render->pool_bytes -= buffer->run.size;
	}

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	if (buffer == NULL)
		return ENOENT;

	/* A buffer of the new size, held by the caller. */
	buffer->next = NULL;
	buffer->bytes = bytes;
	buffer->references = 1;
	buffer->gpu_cached = 0;
	*result = buffer;
	return 0;
}

/*
 * Puts a freed buffer in the pool, newest first, and frees the oldest
 * past BUFFER_POOL_BYTES; a buffer too small or too large is freed.
 */
static void
pool_put(
	struct powervr_render *render,
	struct powervr_buffer *buffer)
{
	struct powervr_buffer **link;
	struct powervr_buffer *evicted;
	struct powervr_buffer *next;
	unsigned long enabled;

	/* Freed at once: too small to matter, or too large to keep. */
	if (buffer->run.size < POWERVR_BUFFER_POOL_LEAST || buffer->run.size > BUFFER_POOL_BYTES || buffer->view == NULL) {
		buffer_free(buffer);
		return;
	}

	/*
	 * Its page list goes: the list is an export's identity to the display
	 * (powervr_render_lookup), which a later holder's export must not
	 * share with one the display may still know; a new one is made when
	 * it is exported again.
	 */
	if (buffer->pages != NULL)
		kern_free(buffer->pages);
	buffer->pages = NULL;
	buffer->page_count = 0;

	/* First in the pool, then the oldest off its end while it holds too much. */
	evicted = NULL;
	enabled = spin_lock_irqsave(&render->buffer_guard);

	buffer->next = render->pool;
	render->pool = buffer;
	render->pool_bytes += buffer->run.size;
	while (render->pool_bytes > BUFFER_POOL_BYTES) {
		link = &render->pool;
		while ((*link)->next != NULL)
			link = &(*link)->next;
		next = *link;
		*link = NULL;
		render->pool_bytes -= next->run.size;
		next->next = evicted;
		evicted = next;
	}

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	/* The evicted ones, freed outside the guard. */
	while (evicted != NULL) {
		next = evicted->next;
		buffer_free(evicted);
		evicted = next;
	}
}

/*
 * Takes another reference on a buffer.
 */
void
powervr_buffer_retain(
	struct powervr_render *render,
	struct powervr_buffer *buffer)
{
	unsigned long enabled;

	/* Counts the new holder. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	buffer->references++;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);
}

/*
 * Drops a reference; the last one frees the buffer, or keeps it after a
 * fault, since the core may still reach it.
 */
void
powervr_buffer_put(
	struct powervr_render *render,
	struct powervr_buffer *buffer)
{
	unsigned long enabled;
	unsigned left;
	unsigned faulted;

	/* Counts the holder off, and reads whether the device is in quarantine. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	buffer->references--;
	left = buffer->references;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	/* Other holders remain. */
	if (left != 0U)
		return;

	/* After a fault the core may still reach the run: keep it until the reset. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	faulted = render->faulted;
	if (faulted) {
		buffer->next = render->kept_buffers;
		render->kept_buffers = buffer;
	}

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* Kept for the reset. */
	if (faulted)
		return;

	/* A buffer the cache may hold lines of: freed by the service after the cache is flushed (unless the flush is turned off, as mainline never flushes). */
	if (buffer->gpu_cached && POWERVR_SLC_FLUSH_ON_FREE) {
		enabled = spin_lock_irqsave(&render->queue_lock);

		buffer->next = render->slc_pending;
		render->slc_pending = buffer;
		waitq_wake_all(&render->service);

		spin_unlock_irqrestore(&render->queue_lock, enabled);
		return;
	}

	/* Into the pool for reuse, or freed with its run, view and page list. */
	pool_put(render, buffer);
}

/*
 * Gives back a list of cached buffers, linked through next, whose lines
 * the system level cache wrote back and dropped: into the pool for reuse
 * (through the cache no longer), or freed.  A write the cache still made
 * after the flush would land in memory the GPU keeps, not in the
 * kernel's.
 */
void
powervr_buffers_release_flushed(
	struct powervr_render *render,
	struct powervr_buffer *list)
{
	struct powervr_buffer *buffer;

	/* Each buffer of the list. */
	while (list != NULL) {
		buffer = list;
		list = buffer->next;
		buffer->next = NULL;
		buffer->gpu_cached = 0;
		pool_put(render, buffer);
	}
}

/*
 * Frees a list of buffers kept for a reset, linked through next.
 */
void
powervr_buffers_free(
	struct powervr_buffer *list)
{
	struct powervr_buffer *buffer;

	/* Each buffer of the list. */
	while (list != NULL) {
		buffer = list;
		list = buffer->next;
		buffer_free(buffer);
	}
}

/*
 * Frees what the render node kept for a reset once the core is stopped
 * for good.
 */
void
powervr_render_abandon(
	struct powervr_core *core)
{
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_buffer *list;
	unsigned long enabled;

	/* Only the board's node, and only when it was set up. */
	render = &board_render;
	if (render->core != core)
		return;

	/* The kept sessions: their records' heap is gone with the firmware. */
	while (render->kept_sessions != NULL) {
		session = render->kept_sessions;
		render->kept_sessions = session->next;
		powervr_session_firmware_discard(session);
		kern_free(session);
	}

	/* The kept buffers. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	list = render->kept_buffers;
	render->kept_buffers = NULL;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	powervr_buffers_free(list);

	/* The pooled buffers, which no work reaches. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	list = render->pool;
	render->pool = NULL;
	render->pool_bytes = 0;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	powervr_buffers_free(list);
}

/*
 * Waits, bounded, for a session's timeline to reach a value.
 */
int
powervr_session_wait(
	struct powervr_session *session,
	uint32_t value,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t current;

	/* Reads the timeline once a millisecond until it reaches the value or the bound is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds);
	for (;;) {
		current = powervr_session_timeline(session);
		if ((int32_t)(current - value) >= 0)
			break;

		/* The bound is spent. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps a millisecond (a tick at least). */
		sched_sleep(now + kern_ms_to_ticks(RENDER_POLL_MS));
	}

	/* Succeeded: the timeline reached the value. */
	return 0;
}

/*
 * Makes a session's handle on a buffer, mapped at a device address of the
 * session's own, for the Vulkan executor's memory; the handle takes a
 * reference of its own.
 */
int
powervr_resource_make(
	struct powervr_session *session,
	struct powervr_buffer *buffer,
	int mappable,
	int shareable,
	struct powervr_resource **result)
{
	int error;

	/* The handle, mapped. */
	error = resource_make(session, buffer, mappable, shareable, result);
	if (error != 0)
		return error;

	/* Succeeded: the handle holds the buffer. */
	(*result)->origin = (uintptr_t)__builtin_return_address(0);
	return 0;
}

/* Undoes a handle the Vulkan executor made. */
void
powervr_resource_unmake(
	struct powervr_session *session,
	struct powervr_resource *resource)
{
	/* Unmaps it, unless the core may still reach it, and drops its buffer. */
	resource_unmake(session, resource);
}

/* Finds a blob or an import of a session by the identifier the renderer names it by, or NULL. */
struct powervr_resource *
powervr_session_blob(
	struct powervr_session *session,
	uint32_t identifier)
{
	struct powervr_resource *resource;

	/* The session's list, newest first; zero names nothing. */
	if (identifier == 0)
		return NULL;
	for (resource = session->blobs; resource != NULL; resource = resource->next_blob) {
		if (resource->identifier == identifier)
			return resource;
	}

	/* No such blob. */
	return NULL;
}

/* Opens a session with a memory context and a compute context of its own. */
static int
render_open(
	void *device,
	void **result)
{
	struct powervr_render *render;
	struct powervr_session *session;
	unsigned long enabled;
	unsigned faulted;
	int error;

	/* Refuses a new session while the device is in quarantine. */
	render = device;
	*result = NULL;
	enabled = spin_lock_irqsave(&render->queue_lock);

	faulted = render->faulted;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (faulted)
		return EIO;

	/* The session's bookkeeping. */
	session = kern_calloc(1, sizeof(*session));
	if (session == NULL)
		return ENOMEM;
	session->render = render;

	/* The general heap's allocations start on the page after the fixed area. */
	powervr_gpu_space_init(&session->space, POWERVR_GENERAL_HEAP_BASE + POWERVR_FIXED_GENERAL_BYTES, POWERVR_GENERAL_HEAP_BYTES - POWERVR_FIXED_GENERAL_BYTES);

	/* The fixed area's pages, with its programs. */
	error = powervr_session_fixed_make(render, session);
	if (error != 0) {
		powervr_session_fixed_drop(render, session);
		kern_free(session);
		return error;
	}

	/* Makes its tables, with the fixed area mapped, and its firmware records, and lists it. */
	mutex_lock(&render->core->lock);

	session->identity = render->next_identity++;
	error = powervr_session_firmware_create(render, session);
	if (error == 0) {
		session->next = render->sessions;
		render->sessions = session;
	}

	mutex_unlock(&render->core->lock);

	/* The records could not be made; they were undone, and the fixed area's pages go. */
	if (error != 0) {
		powervr_session_fixed_drop(render, session);
		kern_free(session);
		return error;
	}

	/*
	 * The compute context's switch; without it the static state stays
	 * zero and the context cannot be switched.  It is left out while a
	 * compute lockup on the VF2 is looked into (POWERVR_COMPUTE_SWITCH_STATE).
	 */
	if (POWERVR_COMPUTE_SWITCH_STATE) {
		error = powervr_session_compute_switch(render, session);
		if (error != 0)
			kern_logf("powervr: compute context switch state not written (%d)\n", error);
	}

	/* The texture unit's border colour table, all zero (transparent black); without it the commands name none. */
	border_table_make(render, session);

	/* Names the session's page catalogue, which a reset's notice and a BIF fault name. */
	if (POWERVR_DEBUG)
		kern_logf("powervr: session context 0x%x memory context 0x%x page catalogue 0x%llx\n",
			  (unsigned)session->identity, (unsigned)powervr_object_address(&session->memory_context, 0),
			  (unsigned long long)powervr_gpu_mmu_root(&session->mmu));

	/* Succeeded: the session is the core's. */
	*result = session;
	return 0;
}

/*
 * Makes a session's border colour table: zeroed memory of the table's
 * size at an address of the session's general heap.  Without it the
 * commands carry no table.
 *
 * Procedure referenced:
 * Mesa 25.0.7 src/imagination/vulkan/pvr_border.c
 * (pvr_border_color_table_init: one table of every entry in the general
 * heap, aligned to the cache line; entry 0, transparent black, all zero).
 */
static void
border_table_make(
	struct powervr_render *render,
	struct powervr_session *session)
{
	struct powervr_buffer *buffer;
	int error;

	/*
	 * The table's memory is written by the processor (zeroed now, the
	 * border colours later) through an uncached view, so the core reads it
	 * around its system level cache, as every buffer the processor writes.
	 */
	error = powervr_buffer_create(render, POWERVR_BORDER_TABLE_BYTES, &buffer);
	if (error == 0) {
		error = powervr_resource_make(session, buffer, 1, 0, &session->border_table);
		powervr_buffer_put(render, buffer);
	}

	if (error != 0) {
		session->border_table = NULL;
		kern_logf("powervr: border colour table not made (%d)\n", error);
	}
}

/* The device address of a session's border colour table, or zero without one. */
uint64_t
powervr_session_border_table(
	const struct powervr_session *session)
{
	if (session->border_table == NULL)
		return 0;
	return session->border_table->address;
}

/*
 * Closes a session.  The core has drained its callbacks and destroyed its
 * resources.  The firmware cleans the context up first; a session the
 * device may still reach is kept for the reset instead of freed.
 */
static void
render_close(
	void *device,
	void *opaque)
{
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_session **position;
	unsigned long enabled;
	unsigned keep;
	int error;

	/* Unlists the session. */
	render = device;
	session = opaque;
	mutex_lock(&render->core->lock);

	position = &render->sessions;
	while (*position != NULL && *position != session)
		position = &(*position)->next;
	if (*position != NULL)
		*position = session->next;

	mutex_unlock(&render->core->lock);

	/* The Vulkan executor's objects and memory go first; the core destroyed the session's blobs already. */
	powervr_vk_close(session->vk);
	session->vk = NULL;

	/* Reads whether the session or the device may still be reached by the core. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	keep = 0;
	if (render->faulted || session->isolated)
		keep = 1;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* Work that ran out of time may still run: the firmware is not asked to clean up under it. */
	if (session->abandoned)
		keep = 1;

	/* The contexts that ran are cleaned up by the firmware before their records go. */
	if (!keep) {
		error = powervr_session_cleanup(render, session);
		if (error != 0)
			keep = 1;
	}

	/* Then its render targets and free lists, which the context used. */
	if (!keep) {
		error = powervr_session_targets_cleanup(render, session);
		if (error != 0)
			keep = 1;
	}

	/* Kept for the reset: nothing of it is touched again until then. */
	if (keep) {
		mutex_lock(&render->core->lock);

		session->retained = 1;
		session->next = render->kept_sessions;
		render->kept_sessions = session;

		mutex_unlock(&render->core->lock);
		return;
	}

	/* The render jobs' memory, whose work has all ended. */
	powervr_render_jobs_reap(render, session, 2);

	/* The job and transfer memory kept mapped for the next jobs, which will not come. */
	powervr_render_jobs_forget(render, session);
	powervr_transfer_forget(render, session);

	/* The handles whose unmapping waited for the session's work, which has all ended. */
	resources_drain(session, 1);

	/* The border colour table, which no command reaches any more. */
	if (session->border_table != NULL)
		powervr_resource_unmake(session, session->border_table);
	session->border_table = NULL;

	/* Returns its render targets, its tables and records, then the bookkeeping. */
	powervr_session_targets_destroy(render, session);
	resources_drain(session, 1);
	powervr_session_firmware_destroy(render, session);
	kern_free(session);
}

/* Reports the node's capabilities and limits. */
static int
render_info(
	void *device,
	void *session,
	struct gpu_info *info)
{
	struct powervr_render *render;

	/* The node's table holds the capabilities. */
	render = device;
	(void)session;
	info->capabilities = render->ops.capabilities;
	info->max_resources = POWERVR_RENDER_RESOURCES;
	info->max_resource_bytes = POWERVR_RENDER_MAX_BYTES;
	kern_memcpy(info->driver_name, "powervr", sizeof("powervr"));

	/* Succeeded: the snapshot is filled. */
	return 0;
}

/* Makes one storage allocation of the session, mapped and host visible. */
static int
render_resource_create(
	void *device,
	void *opaque,
	const struct gpu_resource_create *request,
	void **result)
{
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_buffer *buffer;
	struct powervr_resource *resource;
	int error;

	/* Only storage, with no flags. */
	render = device;
	session = opaque;
	*result = NULL;
	if (request->usage != GPU_RESOURCE_USAGE_STORAGE || request->flags != 0U)
		return EINVAL;

	/* The allocation. */
	error = powervr_buffer_create(render, request->bytes, &buffer);
	if (error != 0)
		return error;

	/* The session's handle on it, mapped; the handle takes the creation reference. */
	error = resource_make(session, buffer, 1, 0, &resource);
	powervr_buffer_put(render, buffer);
	if (error != 0)
		return error;

	/* Succeeded: the resource is the core's. */
	*result = resource;
	return 0;
}

/* Destroys one resource of the session. */
static void
render_resource_destroy(
	void *device,
	void *opaque,
	void *object)
{
	struct powervr_session *session;
	struct powervr_resource *resource;

	/* Unmaps it, unless the core may still reach it, and drops its buffer. */
	(void)device;
	session = opaque;
	resource = object;
	resource_unmake(session, resource);
}

/* Makes one blob of the session; a mappable blob is host visible and uncached. */
static int
render_blob_create(
	void *device,
	void *opaque,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *identifier)
{
	/* No placement condition. */
	return blob_make(device, opaque, request, NULL, result, identifier);
}

/*
 * Makes one blob that meets a placement.  Every buffer is one contiguous
 * run below the core's DMA limit, so the run is only checked.
 */
static int
render_blob_create_placed(
	void *device,
	void *opaque,
	const struct gpu_blob_create_placed *request,
	void **result,
	uint32_t *identifier)
{
	/* The blob part carries the placed request's size; the rest of it is the plain request. */
	struct gpu_blob_create blob;

	blob = request->blob;
	blob.size = sizeof(blob);
	return blob_make(device, opaque, &blob, &request->placement, result, identifier);
}

/* Makes one blob, checking its run against a placement when one is given. */
static int
blob_make(
	struct powervr_render *render,
	struct powervr_session *session,
	const struct gpu_blob_create *request,
	const struct gpu_placement *placement,
	void **result,
	uint32_t *identifier)
{
	struct powervr_buffer *buffer;
	struct powervr_resource *resource;
	uint64_t limit;
	int mappable;
	int shareable;
	int error;

	/* The defined flags; a cross-device blob must be shareable. */
	*result = NULL;
	*identifier = 0;
	if ((request->flags & ~(GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE | GPU_BLOB_CROSS_DEVICE)) != 0U)
		return EINVAL;
	if ((request->flags & GPU_BLOB_CROSS_DEVICE) != 0U && (request->flags & GPU_BLOB_SHAREABLE) == 0U)
		return EINVAL;

	/* A coherent placement is not offered: the buffers are only reached through uncached views. */
	limit = 0;
	if (placement != NULL) {
		if ((placement->flags & GPU_PLACEMENT_COHERENT) != 0U)
			return ENOTSUP;
		limit = placement->max_dma_address;
		if ((placement->flags & GPU_PLACEMENT_DMA32) != 0U && (limit == 0U || limit > 0xffffffffULL))
			limit = 0xffffffffULL;
	}

	/*
	 * The allocation: a blob of a VkDeviceMemory (its blob id is the
	 * memory's identity) maps the memory's own buffer; any other blob has
	 * a buffer of its own.
	 */
	if (request->blob_id != 0U) {
		if (session->vk == NULL)
			return EINVAL;
		error = powervr_vk_memory_buffer(session->vk, request->blob_id, request->bytes, &buffer);

		/* Memory the system level cache holds lines of: no other view of its pages. */
		if (error == 0 && buffer->gpu_cached) {
			powervr_buffer_put(render, buffer);
			return EINVAL;
		}
	} else {
		error = powervr_buffer_create(render, request->bytes, &buffer);
	}

	if (error != 0)
		return error;

	/* The run must meet the placement; the buffer goes if it does not. */
	if (placement != NULL) {
		error = powervr_gpu_run_fits(buffer->run.paddr, buffer->run.size, limit, placement->alignment);
		if (error != 0) {
			powervr_buffer_put(render, buffer);
			return error;
		}
	}

	/* The session's handle on it, mapped; the handle takes the creation reference. */
	mappable = 0;
	if ((request->flags & GPU_BLOB_MAPPABLE) != 0U)
		mappable = 1;
	shareable = 0;
	if ((request->flags & GPU_BLOB_SHAREABLE) != 0U)
		shareable = 1;
	error = resource_make(session, buffer, mappable, shareable, &resource);
	powervr_buffer_put(render, buffer);
	if (error != 0)
		return error;

	/* Succeeded: the blob, named for the renderer by the session's next number and listed by it. */
	blob_list(session, resource, identifier);
	*result = resource;
	return 0;
}

/* Copies bytes out of a resource. */
static int
render_read(
	void *device,
	void *opaque,
	void *object,
	uint64_t offset,
	void *bytes,
	uint32_t length)
{
	struct powervr_resource *resource;
	uint64_t size;

	/* Refuses a range outside the allocation. */
	(void)device;
	(void)opaque;
	resource = object;
	size = resource->buffer->bytes;
	if (offset > size || length > size - offset)
		return EINVAL;

	/* Reads through the uncached view. */
	copy_out(resource->buffer->view + offset, bytes, length);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Copies bytes into a resource. */
static int
render_write(
	void *device,
	void *opaque,
	void *object,
	uint64_t offset,
	const void *bytes,
	uint32_t length)
{
	struct powervr_resource *resource;
	uint64_t size;

	/* Refuses a range outside the allocation. */
	(void)device;
	(void)opaque;
	resource = object;
	size = resource->buffer->bytes;
	if (offset > size || length > size - offset)
		return EINVAL;

	/* Writes through the uncached view; the bytes are in memory when it returns. */
	copy_in(resource->buffer->view + offset, bytes, length);

	/* Succeeded: the bytes are copied. */
	return 0;
}

/* Describes the CPU view of a host-visible resource: its uncached alias. */
static int
render_map(
	void *device,
	void *opaque,
	void *object,
	struct drv_gpu_mapping *mapping)
{
	struct powervr_resource *resource;

	/* Only a host-visible resource of this session has a view. */
	(void)device;
	resource = object;
	if (resource->session != opaque || !resource->mappable)
		return ENOTSUP;

	/*
	 * The buffer's whole pages, non-cacheable RAM: the user mapping keeps
	 * the kernel's policy.  A buffer from the pool may have a larger run;
	 * the view is the buffer's own size, which the allocation's mapping
	 * expects.
	 */
	mapping->physical = resource->buffer->run.paddr;
	mapping->address = (void *)resource->buffer->view;
	mapping->bytes = (resource->buffer->bytes + POWERVR_GPU_PAGE_BYTES - 1U) & ~(uint64_t)(POWERVR_GPU_PAGE_BYTES - 1U);
	mapping->attributes = DRV_GPU_MAPPING_UNCACHED_RAM;

	/* Succeeded: the core maps the run with the same attributes. */
	return 0;
}

/* Exports a resource's buffer, as an image or as an allocation only. */
static int
render_export(
	void *device,
	void *opaque,
	void *object,
	const struct gpu_image_descriptor *image,
	void **result)
{
	struct powervr_render *render;
	struct powervr_resource *resource;
	struct powervr_shared *shared;
	unsigned long enabled;
	int error;

	/* Only a resource of this session. */
	render = device;
	resource = object;
	*result = NULL;
	if (resource->session != opaque)
		return EINVAL;

	/* An image must fit the buffer, and needs the buffer's page list. */
	if (image != NULL) {
		error = check_image(resource->buffer, image);
		if (error != 0)
			return error;
		mutex_lock(&render->core->lock);

		error = make_pages(resource->buffer);

		mutex_unlock(&render->core->lock);
		if (error != 0)
			return error;
	}

	/* The export's bookkeeping. */
	shared = kern_calloc(1, sizeof(*shared));
	if (shared == NULL)
		return ENOMEM;
	shared->buffer = resource->buffer;
	if (image != NULL) {
		shared->image = *image;
		shared->has_image = 1;
	}

	/* Holds the buffer, and lists the export where a lookup finds it. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	resource->buffer->references++;
	shared->next = render->exports;
	render->exports = shared;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	/* Succeeded: the export lives independently of the session. */
	*result = shared;
	return 0;
}

/* Ends an export; a lookup's own reference keeps the buffer if it has one. */
static void
render_release(
	void *device,
	void *object)
{
	struct powervr_render *render;
	struct powervr_shared *shared;
	struct powervr_shared **position;
	unsigned long enabled;

	/* Unlists the export. */
	render = device;
	shared = object;
	enabled = spin_lock_irqsave(&render->buffer_guard);

	position = &render->exports;
	while (*position != NULL && *position != shared)
		position = &(*position)->next;
	if (*position != NULL)
		*position = shared->next;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	/* Drops the export's hold on the buffer. */
	powervr_buffer_put(render, shared->buffer);
	kern_free(shared);
}

/* Imports an export of this node into a session as a resource of its own. */
static int
render_import(
	void *device,
	void *opaque,
	void *object,
	uint32_t flags,
	void **result,
	uint32_t *identifier)
{
	struct powervr_render *render;
	struct powervr_session *session;
	struct powervr_shared *shared;
	struct powervr_resource *resource;
	int error;

	(void)flags;

	/* The export the core kept alive for this call. */
	render = device;
	session = opaque;
	shared = object;
	*result = NULL;
	*identifier = 0;

	/* A new handle, mapped into this session, holding the buffer on its own. */
	powervr_buffer_retain(render, shared->buffer);
	error = resource_make(session, shared->buffer, 1, 1, &resource);
	powervr_buffer_put(render, shared->buffer);
	if (error != 0)
		return error;

	/* Succeeded: the resource is the session's, under the session's next number, listed by it. */
	blob_list(session, resource, identifier);
	*result = resource;
	return 0;
}

/* Describes an image export's pages for a display; allocation-only exports have none. */
static int
render_backing(
	void *device,
	void *object,
	struct drv_gpu_scanout_backing *backing)
{
	struct powervr_shared *shared;

	/* Only an image export has a layout to show. */
	(void)device;
	shared = object;
	if (!shared->has_image)
		return ENOTSUP;

	/* The run's pages, contiguous, in RAM; the page list is the lookup's key. */
	backing->pages = shared->buffer->pages;
	backing->page_count = shared->buffer->page_count;
	backing->bytes = shared->buffer->bytes;
	backing->page_bytes = POWERVR_GPU_PAGE_BYTES;
	backing->flags = DRV_GPU_BACKING_CONTIGUOUS;

	/* Succeeded: the view stays valid while the export lives. */
	return 0;
}

/* Reports the node's role: rendering only. */
static int
render_query(
	void *device,
	void *session,
	struct gpu_device_info *info)
{
	/* The core fills the identity; the node has no companion to name. */
	(void)device;
	(void)session;
	info->roles = GPU_DEVICE_RENDER;
	info->companion_id = 0;

	/* Succeeded: the role is filled. */
	return 0;
}

/*
 * Makes a session's handle on a buffer and maps it at a device address of
 * the session's own; the handle takes a reference of its own.
 */
static int
resource_make(
	struct powervr_session *session,
	struct powervr_buffer *buffer,
	int mappable,
	int shareable,
	struct powervr_resource **result)
{
	int error;

	/* Handles that waited for work which has since ended are unmapped first. */
	resources_drain(session, 0);

	/* Maps it with no flag beyond what host access asks for. */
	error = resource_make_flags(session, buffer, mappable, shareable, 0, result);
	if (error != 0)
		return error;

	/* Succeeded: the handle is in result, named by its maker. */
	(*result)->origin = (uintptr_t)__builtin_return_address(0);
	return 0;
}

/*
 * Makes a session's handle on a free list: mapped with the parameter
 * manager's protection and around the system level cache, and never
 * mapped for the processor.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_free_list.c
 * (pvr_free_list_create: the free list's object must have
 * DRM_PVR_BO_PM_FW_PROTECT and no user access) and pvr_mmu.c (the flag
 * as the page entry's PM_META_PROTECT); Mesa 25.0.0
 * src/imagination/vulkan/pvr_job_render.c (pvr_free_list_create: the
 * free list's memory GPU_UNCACHED and PM_FW_PROTECT, the render target's
 * other memory GPU_UNCACHED only).
 */
int
powervr_resource_make_free_list(
	struct powervr_session *session,
	struct powervr_buffer *buffer,
	struct powervr_resource **result)
{
	int error;

	/* Maps it protected and around the system level cache, with no processor mapping. */
	error = resource_make_flags(session, buffer, 0, 0, POWERVR_GPU_MAP_SLC_BYPASS | POWERVR_GPU_MAP_PM_PROTECT, result);
	if (error != 0)
		return error;

	/* Succeeded: the free list's handle is in result, named by its maker. */
	(*result)->origin = (uintptr_t)__builtin_return_address(0);
	return 0;
}

/* Makes a session's handle on a render target's region headers, in their own window. */
int
powervr_resource_make_region_headers(
	struct powervr_session *session,
	struct powervr_buffer *buffer,
	struct powervr_resource **result)
{
	int error;

	/* Maps it in the region header window. */
	error = resource_make_flags(session, buffer, 1, 0, RESOURCE_REGION_HEADERS, result);
	if (error != 0)
		return error;

	/* Succeeded: the region headers' handle is in result, named by its maker. */
	(*result)->origin = (uintptr_t)__builtin_return_address(0);
	return 0;
}

/*
 * Makes a session's handle on a buffer with extra mapping flags;
 * RESOURCE_REGION_HEADERS takes the address from the region header
 * window instead of the general one.
 */
static int
resource_make_flags(
	struct powervr_session *session,
	struct powervr_buffer *buffer,
	int mappable,
	int shareable,
	unsigned extra,
	struct powervr_resource **result)
{
	struct powervr_render *render;
	struct powervr_resource *resource;
	unsigned flags;
	int error;

	/* Refuses one more resource than a session may hold. */
	render = session->render;
	*result = NULL;
	if (session->resources >= POWERVR_RENDER_RESOURCES)
		return ENOSPC;

	/* The handle. */
	resource = kern_calloc(1, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;
	resource->session = session;
	resource->buffer = buffer;
	resource->mappable = mappable;
	resource->shareable = shareable != 0;
	resource->origin = (uintptr_t)__builtin_return_address(0);

	/*
	 * Host-visible and shareable memory goes around the core's system level
	 * cache, so the processor and the display read what the core wrote.
	 */
	flags = powervr_gpu_buffer_flags(mappable, shareable) | (extra & ~RESOURCE_REGION_HEADERS);

	/* Takes a device address and maps the run there, then has the core drop stale table entries. */
	mutex_lock(&render->core->lock);

	resource->space = &session->space;
	if ((extra & RESOURCE_REGION_HEADERS) != 0U)
		resource->space = &session->region_space;
	error = powervr_gpu_space_take(resource->space, buffer->run.size, &resource->address);
	if (error == 0) {
		error = powervr_gpu_mmu_map(&session->mmu, resource->address, buffer->run.paddr, buffer->run.size, flags);
		if (error != 0)
			(void)powervr_gpu_space_give(resource->space, resource->address);
	}
	if (error == 0)
		error = powervr_command_mmu_flush(render->core);
	if (error == 0) {
		resource->next_all = session->all;
		session->all = resource;
	}

	mutex_unlock(&render->core->lock);

	/* Nothing is mapped; the handle goes. */
	if (error != 0) {
		kern_free(resource);
		return error;
	}

	/* Succeeded: the handle holds the buffer. */
	powervr_buffer_retain(render, buffer);
	session->resources++;
	*result = resource;
	return 0;
}

/*
 * Undoes a handle: unmaps it and drops its buffer, unless the core may
 * still reach the session, when the mapping and the buffer both stay.
 * While the session has work in flight the unmapping waits (a
 * compositor may drop a client buffer's import while its last frame
 * still samples it, and the core would read a page that is gone or
 * mapped again): the handle is kept, mapped, on the session's deferred list
 * until a later handle operation of the session finds no work left.
 */
static void
resource_unmake(
	struct powervr_session *session,
	struct powervr_resource *resource)
{
	struct powervr_render *render;
	struct powervr_resource **link;
	unsigned long enabled;
	unsigned busy;

	/* A listed blob leaves its session's list. */
	if (resource->identifier != 0) {
		for (link = &session->blobs; *link != NULL; link = &(*link)->next_blob) {
			if (*link == resource) {
				*link = resource->next_blob;
				break;
			}
		}
	}

	/* Earlier handles whose work has ended go first. */
	resources_drain(session, 0);

	/* Work in flight may still read it: it waits, mapped, on the deferred list. */
	render = session->render;
	enabled = spin_lock_irqsave(&render->queue_lock);

	busy = session->outstanding != 0U || session->pending_count != 0U;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (busy) {
		resource->deferred = 1;
		resource->next_blob = session->deferred;
		session->deferred = resource;
		return;
	}

	/* Nothing reads it any more. */
	resource_release(session, resource);
}

/*
 * Unmaps the session's deferred handles once no work of the session is
 * in flight, or all of them when force is set (the session is going).
 */
static void
resources_drain(
	struct powervr_session *session,
	int force)
{
	struct powervr_render *render;
	struct powervr_resource *resource;
	unsigned long enabled;
	unsigned busy;

	/* Nothing waits. */
	if (session->deferred == NULL)
		return;

	/* Only when the session's work has ended. */
	render = session->render;
	enabled = spin_lock_irqsave(&render->queue_lock);

	busy = session->outstanding != 0U || session->pending_count != 0U;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	if (busy && !force)
		return;

	/* Each one unmapped and dropped. */
	while (session->deferred != NULL) {
		resource = session->deferred;
		session->deferred = resource->next_blob;
		resource_release(session, resource);
	}
}

/* Unmaps a handle no work reads any more and drops its buffer, unless the device or session is in quarantine. */
static void
resource_release(
	struct powervr_session *session,
	struct powervr_resource *resource)
{
	struct powervr_gpu_space *space;
	struct powervr_render *render;
	struct powervr_resource **link;
	unsigned long enabled;
	unsigned keep;

	/* Reads whether the device or the session is in quarantine. */
	render = session->render;
	enabled = spin_lock_irqsave(&render->queue_lock);

	keep = 0;
	if (render->faulted || session->isolated)
		keep = 1;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	/* It leaves the diagnostic list of live handles. */
	mutex_lock(&render->core->lock);
	for (link = &session->all; *link != NULL; link = &(*link)->next_all) {
		if (*link == resource) {
			*link = resource->next_all;
			break;
		}
	}
	mutex_unlock(&render->core->lock);

	/* Unmaps it and has the core drop what it cached of the entries. */
	if (!keep) {
		mutex_lock(&render->core->lock);

		powervr_gpu_mmu_unmap(&session->mmu, resource->address, resource->buffer->run.size);
		/* The window it was taken from; one made before windows were recorded is in the general one. */
		space = resource->space;
		if (space == NULL)
			space = &session->space;
		(void)powervr_gpu_space_give(space, resource->address);
		(void)powervr_command_mmu_flush(render->core);

		mutex_unlock(&render->core->lock);
	}

	/* Drops the handle's hold; after a fault the buffer is kept for the reset. */
	powervr_buffer_put(render, resource->buffer);
	session->resources--;
	kern_free(resource);
}

/* Names a new blob or import by the session's next number and lists it by that number. */
static void
blob_list(
	struct powervr_session *session,
	struct powervr_resource *resource,
	uint32_t *identifier)
{
	/* The next number, at the head of the session's list. */
	resource->identifier = ++session->next_resource;
	resource->next_blob = session->blobs;
	session->blobs = resource;
	*identifier = resource->identifier;
}

/* Checks a linear four-byte image fits the buffer and the core can reach it. */
static int
check_image(
	const struct powervr_buffer *buffer,
	const struct gpu_image_descriptor *image)
{
	uint64_t bytes;

	/* Only the two four-byte linear colour layouts, of a non-empty size. */
	if (image->width == 0U || image->height == 0U || image->width > UINT32_MAX / 4U)
		return EINVAL;
	if (image->format != GPU_PIXEL_BGRA8888 && image->format != GPU_PIXEL_RGBA8888)
		return ENOTSUP;
	if (image->tiling != GPU_IMAGE_LINEAR || image->allocation_bytes != buffer->bytes)
		return ENOTSUP;

	/* Rows of whole pixels, and every row inside the allocation. */
	if (image->stride < image->width * 4U || (image->stride & 3U) != 0U)
		return EINVAL;
	if ((image->offset & 3U) != 0U || image->offset > buffer->bytes)
		return EINVAL;
	bytes = (uint64_t)image->stride * image->height;
	if (bytes > buffer->bytes - image->offset)
		return EINVAL;

	/* Succeeded: the image lies inside the buffer. */
	return 0;
}

/* Makes the buffer's page list once; the core's lock is held. */
static int
make_pages(
	struct powervr_buffer *buffer)
{
	uint64_t *pages;
	uint64_t count;
	uint64_t index;

	/* A list made earlier is kept. */
	if (buffer->pages != NULL)
		return 0;

	/* One physical address per page of the run. */
	count = buffer->run.size / POWERVR_GPU_PAGE_BYTES;
	pages = kern_calloc((size_t)count, sizeof(*pages));
	if (pages == NULL)
		return ENOMEM;
	for (index = 0; index < count; index++)
		pages[index] = buffer->run.paddr + index * POWERVR_GPU_PAGE_BYTES;

	/* Succeeded: the list lives as long as the buffer. */
	buffer->pages = pages;
	buffer->page_count = count;
	return 0;
}

/* Reports whether two image descriptions agree on every field the export fixed. */
static int
same_image(
	const struct gpu_image_descriptor *first,
	const struct gpu_image_descriptor *second)
{
	/* The size and format. */
	if (first->width != second->width || first->height != second->height)
		return 0;
	if (first->format != second->format || first->stride != second->stride)
		return 0;

	/* The placement in the allocation. */
	if (first->offset != second->offset || first->allocation_bytes != second->allocation_bytes)
		return 0;

	/* The memory type, the use and the layout. */
	if (first->memory_type != second->memory_type || first->usage != second->usage)
		return 0;
	if (first->tiling != second->tiling || first->reserved != second->reserved)
		return 0;

	/* Succeeded: the same image. */
	return 1;
}

/* Frees a buffer's run, view, page list and bookkeeping. */
static void
buffer_free(
	struct powervr_buffer *buffer)
{
	/* The run no longer counts in the render buffers' bytes (an add of its size's two's complement). */
	(void)atomic_u64_fetch_add_relaxed(&buffer_run_bytes, 0U - (uint64_t)buffer->run.size);

	/* Ends the alias, which drops the run's lines, then returns the run. */
	(void)kern_pmem_unmap_uncached((void *)buffer->view, buffer->run.size);
	(void)kern_pmem_free(&buffer->run);

	/* The page list and the bookkeeping. */
	if (buffer->pages != NULL)
		kern_free(buffer->pages);
	kern_free(buffer);
}

/*
 * Copies out of an uncached view: whole aligned words while both sides
 * allow it, bytes otherwise, so the view never takes an access wider
 * than its alignment.
 */
static void
copy_out(
	volatile const uint8_t *view,
	uint8_t *bytes,
	uint32_t length)
{
	uint32_t index;

	/* Words while both addresses are aligned to eight. */
	index = 0;
	if ((((uintptr_t)view | (uintptr_t)bytes) & 7U) == 0U) {
		for (; index + 8U <= length; index += 8U)
			*(uint64_t *)(bytes + index) = *(volatile const uint64_t *)(view + index);
	}

	/* Bytes for the rest. */
	for (; index < length; index++)
		bytes[index] = view[index];
}

/* Copies into an uncached view, the same way. */
static void
copy_in(
	volatile uint8_t *view,
	const uint8_t *bytes,
	uint32_t length)
{
	uint32_t index;

	/* Words while both addresses are aligned to eight. */
	index = 0;
	if ((((uintptr_t)view | (uintptr_t)bytes) & 7U) == 0U) {
		for (; index + 8U <= length; index += 8U)
			*(volatile uint64_t *)(view + index) = *(const uint64_t *)(bytes + index);
	}

	/* Bytes for the rest. */
	for (; index < length; index++)
		view[index] = bytes[index];
}

/* One registration attempt of the node. */
static int
register_attempt(
	void *context)
{
	struct powervr_render *render;
	int error;

	/* Registers the node with its table and state. */
	render = context;
	error = drv_gpu_register(&render->ops, render, &render->device);
	if (error != 0)
		return error;

	/* Succeeded: the node is published. */
	return 0;
}

/* The pause between registration attempts. */
static void
register_pause(
	void *context)
{
	uint64_t ticks;

	/* Sleeps the pause, rounded up to whole ticks. */
	(void)context;
	ticks = kern_ms_to_ticks(REGISTER_PAUSE_MS);
	sched_sleep(sched_ticks() + ticks);
}

/*
 * Runs the queue once through a session of the driver's own before the
 * node is published: a blob mapped into the session, written and read
 * back, an empty job whose fence retires, and an empty job held by a
 * fence on a word the processor wrote cached and did not clean.
 *
 * The last one tells whether the firmware's processor reads the
 * processor's cached writes; it is reported, not acted on.
 */
static int
self_test(
	struct powervr_render *render,
	const char **step)
{
	struct powervr_session *session;
	struct powervr_resource *resource;
	struct gpu_blob_create blob;
	uint8_t pattern[64];
	uint8_t back[64];
	struct powervr_object probe;
	volatile uint32_t *word;
	const char *reverse;
	uint32_t address;
	uint32_t index;
	void *opaque;
	void *object;
	uint32_t identifier;
	int error;
	int seen;

	/* Opens a session of the driver's own. */
	*step = "open";
	error = render_open(render, &opaque);
	if (error != 0)
		return error;
	session = opaque;

	/* A mappable blob of one page. */
	*step = "blob";
	kern_memset(&blob, 0, sizeof(blob));
	blob.bytes = POWERVR_GPU_PAGE_BYTES;
	blob.flags = GPU_BLOB_MAPPABLE;
	error = render_blob_create(render, session, &blob, &object, &identifier);
	if (error != 0)
		goto close;
	resource = object;

	/* Writes a pattern and reads it back through the transfer operations. */
	*step = "memory";
	for (index = 0; index < sizeof(pattern); index++)
		pattern[index] = (uint8_t)(index * 7U + 1U);
	error = render_write(render, session, resource, 64, pattern, sizeof(pattern));
	if (error == 0)
		error = render_read(render, session, resource, 64, back, sizeof(back));
	if (error == 0 && kern_memcmp(pattern, back, sizeof(pattern)) != 0)
		error = EIO;
	if (error != 0)
		goto destroy;

	/* An empty job, whose fence must retire. */
	*step = "empty job";
	mutex_lock(&render->core->lock);

	session->submitted++;
	error = powervr_session_kick(render, session, session->submitted, 0, 0);

	mutex_unlock(&render->core->lock);
	if (error == 0)
		error = powervr_session_wait(session, session->submitted, SELF_TEST_WAIT_MS);
	if (error != 0)
		goto destroy;

	/*
	 * An empty job held by a fence on a word of a block of its own, which
	 * the processor writes through its cached view without cleaning.  The
	 * block's line holds nothing else, so cleaning it later loses nothing.
	 */
	*step = "fence";
	mutex_lock(&render->core->lock);

	error = powervr_object_create(&render->core->firmware.heap, POWERVR_GPU_PAGE_BYTES, POWERVR_HEAP_ANYWHERE, POWERVR_MIPS_UNCACHED, &probe);

	mutex_unlock(&render->core->lock);
	if (error != 0)
		goto destroy;
	address = powervr_object_address(&probe, 0);
	word = kern_pmem_to_kernel(powervr_object_physical(&probe, 0));
	*word = 0x600dU;
	mutex_lock(&render->core->lock);

	session->submitted++;
	error = powervr_session_kick(render, session, session->submitted, address, 0x600dU);

	mutex_unlock(&render->core->lock);

	/* Whether the firmware saw the uncleaned write; if not, cleaning the line must let the job go. */
	seen = 1;
	if (error == 0)
		error = powervr_session_wait(session, session->submitted, SELF_TEST_HOLD_MS);
	if (error == ETIMEDOUT) {
		seen = 0;
		kern_dcache_clean_range((const void *)word, sizeof(*word));
		mutex_lock(&render->core->lock);

		error = powervr_session_rekick(render, session);

		mutex_unlock(&render->core->lock);
		if (error == 0)
			error = powervr_session_wait(session, session->submitted, SELF_TEST_WAIT_MS);
	}

	/*
	 * Drops the probe's line.  A job that never ran may still wait on the
	 * probe, so on failure the page stays with the firmware, which is then
	 * stopped and powered down and never reaches it again.
	 */
	kern_dcache_invalidate_range((const void *)word, sizeof(*word));
	if (error != 0)
		goto destroy;
	mutex_lock(&render->core->lock);

	powervr_object_destroy(&render->core->firmware.heap, &probe);

	mutex_unlock(&render->core->lock);

	/* Reports the reverse direction; the sharing stays as it is. */
	if (seen) {
		reverse = "seen without a clean";
	} else {
		reverse = "seen after a clean only";
	}
	if (POWERVR_DEBUG)
		kern_logf("powervr: coherency (firmware reads of cached writes): %s\n", reverse);

destroy:
	/* Destroys the blob. */
	render_resource_destroy(render, session, resource);

close:
	/* Closes the session; a failure above leaves the error to report. */
	render_close(render, session);
	if (error != 0)
		return error;

	/* Succeeded: the queue, the fence and the memory work. */
	return 0;
}

/* Logs one handle of a session for a fault report: its range, kind, state and maker. */
static void
resource_log(
	const char *what,
	const struct powervr_resource *resource,
	uint64_t fault)
{
	uint64_t end;

	/* The range, the distance of the fault from its end, and what it is. */
	end = resource->address + resource->buffer->run.size;
	kern_logf("powervr: fault 0x%llx %s: 0x%llx..0x%llx (%llu bytes, past end by %lld) host %u shared %u blob %u deferred %u maker %p\n",
		  (unsigned long long)fault, what, (unsigned long long)resource->address, (unsigned long long)end,
		  (unsigned long long)resource->buffer->run.size, (long long)(fault - end),
		  (unsigned)resource->mappable, resource->shareable, (unsigned)resource->identifier, resource->deferred,
		  (void *)resource->origin);
}

/*
 * Names what a session maps at a fault address, for diagnostics: the
 * handle that covers it, else the nearest below and above, and the fixed
 * area; the core's lock is held.
 */
void
powervr_session_describe_fault(
	struct powervr_session *session,
	uint64_t fault)
{
	const struct powervr_resource *resource;
	const struct powervr_resource *below;
	const struct powervr_resource *above;
	unsigned count;

	/* Nothing to name. */
	if (fault == 0)
		return;

	/* The fixed area at the general heap's start. */
	if (fault >= POWERVR_GENERAL_HEAP_BASE && fault < POWERVR_GENERAL_HEAP_BASE + POWERVR_FIXED_GENERAL_BYTES)
		kern_logf("powervr: fault 0x%llx is in the general heap's fixed area\n", (unsigned long long)fault);

	/* The live handles: the cover, or the neighbours. */
	below = NULL;
	above = NULL;
	count = 0;
	for (resource = session->all; resource != NULL; resource = resource->next_all) {
		count++;
		if (fault >= resource->address && fault < resource->address + resource->buffer->run.size) {
			resource_log("inside", resource, fault);
			return;
		}
		if (resource->address <= fault && (below == NULL || resource->address > below->address))
			below = resource;
		if (resource->address > fault && (above == NULL || resource->address < above->address))
			above = resource;
	}
	kern_logf("powervr: fault 0x%llx in no live handle of %u\n", (unsigned long long)fault, count);
	if (below != NULL)
		resource_log("above handle", below, fault);
	if (above != NULL)
		resource_log("below handle", above, fault);
}

/* Notes the textures of one draw's sets: each image binding's state words and its sampler's first word. */
static void
note_textures(
	struct powervr_note_draw *note,
	const struct powervr_render_draw *draw)
{
	const struct powervr_vk_descriptor_set *set;
	const struct powervr_set_binding *binding;
	struct powervr_note_texture *texture;
	uint32_t slot;
	uint32_t number;
	uint32_t offset;

	/* Each set's image bindings, their first element, as far as the note holds. */
	note->texture_count = 0;
	for (slot = 0; slot < POWERVR_DRAW_SETS; slot++) {
		set = draw->sets[slot];
		if (set == NULL || set->bytes == NULL)
			continue;
		for (number = 0; number < POWERVR_SET_BINDINGS_MAX; number++) {
			binding = &set->layout.bindings[number];
			if (binding->count == 0 || note->texture_count >= POWERVR_NOTE_TEXTURES)
				continue;
			if (powervr_set_descriptor_offset(&set->layout, number, 0, POWERVR_SHADER_PART_IMAGE_STATE, &offset) != 0)
				continue;
			texture = &note->textures[note->texture_count];
			kern_memcpy(texture->image, set->bytes + offset, sizeof(texture->image));
			texture->sampler = 0;
			if (powervr_set_descriptor_offset(&set->layout, number, 0, POWERVR_SHADER_PART_SAMPLER_STATE, &offset) == 0)
				kern_memcpy(&texture->sampler, set->bytes + offset, sizeof(texture->sampler));
			texture->set = slot;
			texture->binding = number;
			note->texture_count++;
		}
	}
}

/*
 * Keeps a compact note of the session's newest render job, for
 * powervr_session_describe_job after a reset: the area, the colour
 * targets, and the draws (the load draw first when there is one).
 */
void
powervr_session_note_job(
	struct powervr_session *session,
	const struct powervr_render_job *job,
	const struct powervr_render_draw *prefix)
{
	struct powervr_job_note *note;
	const struct powervr_render_draw *draw;
	uint32_t index;
	uint32_t count;

	/* The job's shape and its colour targets. */
	note = &session->note;
	note->serial++;
	note->area[0] = job->area.x;
	note->area[1] = job->area.y;
	note->area[2] = job->area.width;
	note->area[3] = job->area.height;
	note->width = job->width;
	note->height = job->height;
	note->color_count = job->color_count;
	for (index = 0; index < job->color_count && index < POWERVR_NOTE_COLORS; index++) {
		note->colors[index].address = job->colors[index].surface.address;
		note->colors[index].vk_format = job->colors[index].surface.format != NULL ? job->colors[index].surface.format->vk_format : 0;
		note->colors[index].width = job->colors[index].surface.width;
		note->colors[index].height = job->colors[index].surface.height;
		note->colors[index].row_pitch = job->colors[index].surface.row_pitch;
		note->colors[index].padded_width = job->colors[index].surface.padded_width;
		note->colors[index].linear = job->colors[index].surface.linear;
		note->colors[index].load_op = job->colors[index].load_op;
	}

	/* The draws, the load draw first. */
	note->prefix = prefix != NULL;
	count = job->draw_count + (prefix != NULL ? 1U : 0U);
	note->draw_count = count;
	for (index = 0; index < count && index < POWERVR_NOTE_DRAWS; index++) {
		draw = prefix != NULL ? (index == 0 ? prefix : &job->draws[index - 1U]) : &job->draws[index];
		note->draws[index].pipeline = draw->pipeline;
		note->draws[index].scissor_x = draw->scissor.offset.x;
		note->draws[index].scissor_y = draw->scissor.offset.y;
		note->draws[index].scissor_width = draw->scissor.extent.width;
		note->draws[index].scissor_height = draw->scissor.extent.height;
		note_textures(&note->draws[index], draw);
	}
}

/*
 * Prints the note of the session's newest render job: for each
 * texture the decoded state (kind, format, size, linear row, address) and
 * the live handle its address lies in, with how far the texture's last
 * linear row reaches past the handle's end.  The core's lock is held.
 */
void
powervr_session_describe_job(
	struct powervr_session *session)
{
	const struct powervr_job_note *note;
	const struct powervr_note_texture *texture;
	const struct powervr_resource *resource;
	uint64_t address;
	uint64_t reach;
	uint32_t kind;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t index;
	uint32_t number;

	/* The job. */
	note = &session->note;
	if (note->serial == 0)
		return;
	kern_logf("powervr: job %llu: area %u,%u %ux%u of %ux%u, %u colour(s), %u draw(s)%s\n",
		  (unsigned long long)note->serial, note->area[0], note->area[1], note->area[2], note->area[3],
		  note->width, note->height, note->color_count, note->draw_count, note->prefix ? " (load draw first)" : "");
	for (index = 0; index < note->color_count && index < POWERVR_NOTE_COLORS; index++) {
		kern_logf("powervr: job colour %u: 0x%llx format %u %ux%u pitch %u padded %u linear %u load %u\n", index,
			  (unsigned long long)note->colors[index].address, note->colors[index].vk_format, note->colors[index].width,
			  note->colors[index].height, note->colors[index].row_pitch, note->colors[index].padded_width,
			  note->colors[index].linear, note->colors[index].load_op);
	}

	/* Each draw and its textures. */
	for (index = 0; index < note->draw_count && index < POWERVR_NOTE_DRAWS; index++) {
		kern_logf("powervr: job draw %u: pipeline %p scissor %d,%d %ux%u, %u texture(s)\n", index,
			  note->draws[index].pipeline, note->draws[index].scissor_x, note->draws[index].scissor_y,
			  note->draws[index].scissor_width, note->draws[index].scissor_height, note->draws[index].texture_count);
		for (number = 0; number < note->draws[index].texture_count; number++) {
			texture = &note->draws[index].textures[number];
			kind = (uint32_t)(texture->image[0] & 7U);
			width = (uint32_t)((texture->image[0] >> 34) & 0x3fffU) + 1U;
			height = (uint32_t)((texture->image[0] >> 48) & 0x3fffU) + 1U;
			stride = (uint32_t)(texture->image[1] & 0x7fffU) + 1U;
			address = ((texture->image[1] >> 16) & ((1ULL << 38) - 1U)) << 2;
			kern_logf("powervr: job draw %u texture set %u binding %u: words %llx %llx sampler %llx: kind %u format %u %ux%u stride %u at 0x%llx\n",
				  index, texture->set, texture->binding, (unsigned long long)texture->image[0], (unsigned long long)texture->image[1],
				  (unsigned long long)texture->sampler, kind, (unsigned)((texture->image[0] >> 27) & 0x7fU), width, height,
				  kind == 4U ? stride : 0U, (unsigned long long)address);

			/* The handle it lies in, and for a strided one how far its rows reach (4-byte texels assumed). */
			for (resource = session->all; resource != NULL; resource = resource->next_all) {
				if (address >= resource->address && address < resource->address + resource->buffer->run.size)
					break;
			}
			if (resource == NULL) {
				kern_logf("powervr: job texture at 0x%llx is in no live handle\n", (unsigned long long)address);
				continue;
			}
			reach = address + (uint64_t)stride * height * 4U;
			kern_logf("powervr: job texture in handle 0x%llx..0x%llx (shared %u blob %u deferred %u maker %p)%s past end %lld\n",
				  (unsigned long long)resource->address, (unsigned long long)(resource->address + resource->buffer->run.size),
				  resource->shareable, (unsigned)resource->identifier, resource->deferred, (void *)resource->origin,
				  kind == 4U ? " strided rows" : "", kind == 4U ? (long long)(reach - (resource->address + resource->buffer->run.size)) : 0LL);
		}
	}
}

/*
 * Reads the GPU's telemetry for hw.gputelemetry: the time callbacks were
 * pending (from a kick to its delivery), the core clock the power-up set
 * (the core has no frequency scaling), and the render buffers in use (not
 * those kept in the pool for reuse) of the heap the device offers.
 */
static int
render_telemetry(
	void *context,
	struct gpu_telemetry_entry *entry)
{
	struct powervr_render *render;
	unsigned long enabled;
	uint64_t now;
	uint64_t busy;
	uint64_t pooled;
	uint64_t runs;
	uint32_t core_mhz;

	/* The node, and the time the busy time is read at. */
	render = context;
	now = powervr_busy_clock();

	/* The busy time to now: what ended, and the pending callbacks' time so far. */
	enabled = spin_lock_irqsave(&render->queue_lock);

	busy = render->busy_total;
	if (render->busy_pending != 0U && now > render->busy_since)
		busy += now - render->busy_since;

	spin_unlock_irqrestore(&render->queue_lock, enabled);

	entry->time_ns = now;
	entry->busy_ns = busy;
	entry->valid |= GPU_TELEMETRY_BUSY;

	/* The core clock's rate, which is what the core runs at and the whole range. */
	core_mhz = (uint32_t)(render->core->core_rate / 1000000U);
	if (core_mhz != 0U) {
		entry->cur_mhz = core_mhz;
		entry->req_mhz = core_mhz;
		entry->min_mhz = core_mhz;
		entry->max_mhz = core_mhz;
		entry->valid |= GPU_TELEMETRY_CUR_MHZ;
		entry->valid |= GPU_TELEMETRY_REQ_MHZ;
		entry->valid |= GPU_TELEMETRY_RANGE_MHZ;
	}

	/* The pooled buffers' bytes, then every buffer's: read apart, so a buffer that moves between them is off for one read. */
	enabled = spin_lock_irqsave(&render->buffer_guard);

	pooled = render->pool_bytes;

	spin_unlock_irqrestore(&render->buffer_guard, enabled);

	runs = atomic_u64_load_acquire(&buffer_run_bytes);

	/* The buffers in use, of the heap the device offers. */
	entry->objects_bytes = 0;
	if (runs > pooled)
		entry->objects_bytes = runs - pooled;
	entry->objects_limit = POWERVR_RENDER_MAX_BYTES;
	entry->valid |= GPU_TELEMETRY_OBJECTS;

	/* Succeeded: the entry holds what the node keeps. */
	return 0;
}
