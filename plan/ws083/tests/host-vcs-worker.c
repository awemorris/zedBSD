/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host fixture of the i915 request worker's video engine (ws083-p003a):
 * worker.c is built in with stand-ins for the GT, and the fixture checks
 * that a session's VCS0 context is a record until it is attached, that the
 * attach makes one hardware context on VCS0 and only one, that a request
 * of the video context is submitted to VCS0, and that a video request that
 * hung keeps its context and has the video engine reset (ws083-p007): the
 * kept context is freed once a reset recovered its hang, a reset that
 * fails or a hang past the limit stops video, and the reset of the video
 * engine record lifts the stop.
 */

#include "../../../src/drivers/gpu/i915/worker.c"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fixture's GT has a render engine, a copy engine and VCS0, at these indexes of the engine set. */
#define FIXTURE_GT_RENDER	0U
#define FIXTURE_GT_COPY		1U
#define FIXTURE_GT_VIDEO	2U

/* What the GT's stand-ins saw: the engine each context image was made on, and the one a request was submitted to. */
struct fixture_record {
	unsigned lrc_allocs;
	unsigned lrc_releases;
	const struct i915_gt_engine *last_lrc_engine;
	const struct i915_gt_engine *last_submit_engine;
	const struct i915_execlists *last_submit_lists;
	unsigned engine_resets;
	unsigned last_reset_index;
};

/* The fixture's device, its engines' descriptions and what the stand-ins saw; the one test thread owns them. */
static struct i915_device fixture_device;
static struct i915_engine_info fixture_infos[3];
static struct fixture_record fixture_seen;

/* What the engine reset's stand-in reports: 0, or the error of a reset that failed. */
static int fixture_reset_error;

static void fixture_device_make(int with_video);
static void fixture_device_free(void);
static void fixture_context_make(struct i915_context *context, unsigned engine, struct i915_ppgtt *vm);
static void fixture_render_and_video(void);
static void fixture_hang(void);
static void fixture_no_video(void);
static void fixture_hang_recovered(void);
static void fixture_hang_limit(void);
static void fixture_record_reset(void);

/*
 * Runs the fixture's cases.
 */
int
main(void)
{
	/* The cases, each on a fresh device. */
	fixture_render_and_video();
	fixture_hang();
	fixture_no_video();
	fixture_hang_recovered();
	fixture_hang_limit();
	fixture_record_reset();

	/* Succeeded: every case held. */
	printf("ws083 vcs worker host test PASS\n");
	return 0;
}

/* Makes the fixture's device: a GT of the render, copy and (when asked) VCS0 engines, the engine records, and a serving worker. */
static void
fixture_device_make(
	int with_video)
{
	unsigned index;
	int error;

	/* A clean device and record of what the stand-ins saw; engine resets succeed. */
	memset(&fixture_device, 0, sizeof(fixture_device));
	memset(&fixture_seen, 0, sizeof(fixture_seen));
	fixture_reset_error = 0;

	/* The GT's engines: rcs0, bcs0 and vcs0 (or a second copy engine without VCS0). */
	fixture_infos[0].class = I915_RENDER_CLASS;
	fixture_infos[0].name = "rcs0";
	fixture_infos[1].class = I915_COPY_ENGINE_CLASS;
	fixture_infos[1].name = "bcs0";
	fixture_infos[2].class = I915_VIDEO_DECODE_CLASS;
	fixture_infos[2].instance = 0;
	fixture_infos[2].name = "vcs0";
	if (!with_video) {
		fixture_infos[2].class = I915_COPY_ENGINE_CLASS;
		fixture_infos[2].instance = 1;
		fixture_infos[2].name = "bcs1";
	}
	fixture_device.gt.engines.n = 3U;
	for (index = 0U; index < 3U; index++)
		fixture_device.gt.engines.ge[index].info = &fixture_infos[index];

	/* The engine records, numbered as the node publishes them. */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		fixture_device.engines[index].device = &fixture_device;
		fixture_device.engines[index].index = index;
	}

	/* The worker, serving. */
	error = drv_i915_worker_create(&fixture_device);
	assert(error == 0);
	fixture_device.worker->serving = 1;
}

/* Frees the fixture's worker, with the timeline pages of the records a hang retained (the device's end, which the kernel does not reach). */
static void
fixture_device_free(void)
{
	struct i915_worker *worker;
	unsigned index;

	/* The retained records' pages. */
	worker = fixture_device.worker;
	for (index = 0U; index < I915_WORKER_VIDEO_CONTEXTS; index++) {
		if (worker->video_contexts[index].retained != 0)
			drv_i915_gt_object_destroy(&fixture_device.gt.mem, worker->video_contexts[index].tl_page);
	}

	/* The worker. */
	free(worker);
	fixture_device.worker = NULL;
}

/* Makes a session context on an engine record over an address space, as open does. */
static void
fixture_context_make(
	struct i915_context *context,
	unsigned engine,
	struct i915_ppgtt *vm)
{
	int error;

	/* The worker makes it: a hardware context on the render record, a record only on the others. */
	error = drv_i915_worker_context_create(&fixture_device, &fixture_device.engines[engine], vm, 7U, context);
	assert(error == 0);
	assert(context->created == 1U);
}

/* A render context gets its hardware context at once; a video one only when attached, once, on VCS0, and its request goes to VCS0. */
static void
fixture_render_and_video(void)
{
	struct i915_ppgtt vm;
	struct i915_context render;
	struct i915_context video;
	struct i915_context other_render;
	struct i915_worker *worker;
	struct i915_worker_context *record;
	int error;

	fixture_device_make(1);
	worker = fixture_device.worker;
	memset(&vm, 0, sizeof(vm));

	/* The worker found the engines by class. */
	assert(worker->render_index == (int)FIXTURE_GT_RENDER);
	assert(worker->video_index == (int)FIXTURE_GT_VIDEO);

	/* The render context: a record of the render table, its image on the render engine. */
	fixture_context_make(&render, I915_ENGINE_RCS0, &vm);
	assert(fixture_seen.lrc_allocs == 1U);
	assert(fixture_seen.last_lrc_engine == &fixture_device.gt.engines.ge[FIXTURE_GT_RENDER]);
	record = i915_worker_find(worker, &render);
	assert(record == &worker->contexts[0]);

	/* The video context: a record only, until attached. */
	fixture_context_make(&video, I915_ENGINE_VCS0, &vm);
	assert(fixture_seen.lrc_allocs == 1U);
	record = i915_worker_find(worker, &video);
	assert(record == NULL);

	/* A request of the video context before the attach finds no hardware context. */
	error = i915_worker_run(worker, &video, 0x1000U, 1, 1U);
	assert(error == EINVAL);

	/* The attach makes its image on VCS0, in the video table. */
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == 0);
	assert(fixture_seen.lrc_allocs == 2U);
	assert(fixture_seen.last_lrc_engine == &fixture_device.gt.engines.ge[FIXTURE_GT_VIDEO]);
	record = i915_worker_find(worker, &video);
	assert(record == &worker->video_contexts[0]);

	/* A second attach of the same context makes nothing more. */
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == 0);
	assert(fixture_seen.lrc_allocs == 2U);

	/* A render context is not attached here. */
	error = drv_i915_worker_context_attach(&fixture_device, &render);
	assert(error == EINVAL);

	/* A request of the video context is submitted to VCS0's execlists (the stand-in then refuses it). */
	error = i915_worker_run(worker, &video, 0x1000U, 1, 1U);
	assert(error == EIO);
	assert(fixture_seen.last_submit_engine == &fixture_device.gt.engines.ge[FIXTURE_GT_VIDEO]);
	assert(fixture_seen.last_submit_lists == &fixture_device.gt.engines.el[FIXTURE_GT_VIDEO]);

	/* A refused submission is not a hang: the video engine still runs. */
	assert(worker->video_dead == 0);

	/* A request of the render context is submitted to the render engine. */
	error = i915_worker_run(worker, &render, 0x2000U, 1, 2U);
	assert(error == EIO);
	assert(fixture_seen.last_submit_engine == &fixture_device.gt.engines.ge[FIXTURE_GT_RENDER]);

	/* Destroying the contexts releases both images. */
	drv_i915_worker_context_destroy(&fixture_device, &video);
	drv_i915_worker_context_destroy(&fixture_device, &render);
	assert(fixture_seen.lrc_releases == 2U);
	assert(worker->live_contexts == 0U);

	/* A new render context takes the first record again. */
	fixture_context_make(&other_render, I915_ENGINE_RCS0, &vm);
	record = i915_worker_find(worker, &other_render);
	assert(record == &worker->contexts[0]);
	drv_i915_worker_context_destroy(&fixture_device, &other_render);
	fixture_device_free();
}

/* A video request that hung and whose engine reset failed stops the video engine, keeps the context, and no later attach, request or destroy touches it. */
static void
fixture_hang(void)
{
	struct i915_ppgtt vm;
	struct i915_context video;
	struct i915_context later;
	struct i915_worker *worker;
	struct i915_worker_context *record;
	int error;

	fixture_device_make(1);
	worker = fixture_device.worker;
	memset(&vm, 0, sizeof(vm));

	/* An attached video context. */
	fixture_context_make(&video, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == 0);
	record = i915_worker_find(worker, &video);
	assert(record != NULL);

	/* Its request hangs, and the engine reset fails. */
	fixture_reset_error = ETIMEDOUT;
	i915_worker_video_hung(worker, record, ETIMEDOUT);
	assert(fixture_seen.engine_resets == 1U);
	assert(fixture_seen.last_reset_index == FIXTURE_GT_VIDEO);
	assert(worker->video_dead == 1);
	assert(worker->video_hangs == 1U);
	assert(worker->video_recovered == 0U);
	assert(record->retained == 1U);
	assert(record->owner == NULL);

	/* The session context no longer finds it, and its destroy releases nothing. */
	assert(i915_worker_find(worker, &video) == NULL);
	drv_i915_worker_context_destroy(&fixture_device, &video);
	assert(fixture_seen.lrc_releases == 0U);
	assert(worker->live_contexts == 1U);

	/* No later video context is attached, and no video request runs. */
	fixture_context_make(&later, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &later);
	assert(error == EIO);
	error = i915_worker_run(worker, &later, 0x1000U, 1, 3U);
	assert(error == ECANCELED);
	fixture_device_free();
}

/* A GT without VCS0 attaches no video context. */
static void
fixture_no_video(void)
{
	struct i915_ppgtt vm;
	struct i915_context video;
	int error;

	fixture_device_make(0);
	memset(&vm, 0, sizeof(vm));

	/* No VCS0 in the engine set. */
	assert(fixture_device.worker->video_index == -1);

	/* The video record's context stays a record. */
	fixture_context_make(&video, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == ENODEV);
	fixture_device_free();
}

/* A hang whose engine reset succeeds keeps the context until a reclaim, and the engine takes the next context and request. */
static void
fixture_hang_recovered(void)
{
	struct i915_ppgtt vm;
	struct i915_context video;
	struct i915_context later;
	struct i915_worker *worker;
	struct i915_worker_context *record;
	int error;

	fixture_device_make(1);
	worker = fixture_device.worker;
	memset(&vm, 0, sizeof(vm));

	/* An attached video context whose request hangs; the reset succeeds. */
	fixture_context_make(&video, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == 0);
	record = i915_worker_find(worker, &video);
	assert(record == &worker->video_contexts[0]);
	i915_worker_video_hung(worker, record, EIO);

	/* The engine was reset, video goes on, and the record is kept but recovered. */
	assert(fixture_seen.engine_resets == 1U);
	assert(fixture_seen.last_reset_index == FIXTURE_GT_VIDEO);
	assert(worker->video_dead == 0);
	assert(worker->video_hangs == 1U);
	assert(worker->video_recovered == 1U);
	assert(record->retained == 1U);
	assert(record->owner == NULL);
	error = drv_i915_worker_video_state(&fixture_device);
	assert(error == 0);

	/* Destroying the session context frees the recovered record under the device mutex. */
	assert(fixture_seen.lrc_releases == 0U);
	drv_i915_worker_context_destroy(&fixture_device, &video);
	assert(fixture_seen.lrc_releases == 1U);
	assert(record->retained == 0U);
	assert(record->tl_page == NULL);
	assert(worker->live_contexts == 0U);

	/* A later video context takes the freed record, and its request goes to VCS0. */
	fixture_context_make(&later, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &later);
	assert(error == 0);
	assert(i915_worker_find(worker, &later) == &worker->video_contexts[0]);
	error = i915_worker_run(worker, &later, 0x1000U, 1, 4U);
	assert(error == EIO);
	assert(fixture_seen.last_submit_engine == &fixture_device.gt.engines.ge[FIXTURE_GT_VIDEO]);
	drv_i915_worker_context_destroy(&fixture_device, &later);
	assert(worker->live_contexts == 0U);
	fixture_device_free();
}

/* Hangs up to the limit are reset; the next one stops video without a reset, and a reclaim frees only the recovered records. */
static void
fixture_hang_limit(void)
{
	struct i915_ppgtt vm;
	struct i915_context contexts[I915_WORKER_VIDEO_HANG_LIMIT + 1U];
	struct i915_worker *worker;
	struct i915_worker_context *record;
	unsigned hang;
	int error;

	fixture_device_make(1);
	worker = fixture_device.worker;
	memset(&vm, 0, sizeof(vm));

	/* Each hang retains its own record; the ones up to the limit are reset. */
	for (hang = 0U; hang <= I915_WORKER_VIDEO_HANG_LIMIT; hang++) {
		fixture_context_make(&contexts[hang], I915_ENGINE_VCS0, &vm);
		error = drv_i915_worker_context_attach(&fixture_device, &contexts[hang]);
		assert(error == 0);
		record = i915_worker_find(worker, &contexts[hang]);
		assert(record != NULL);
		i915_worker_video_hung(worker, record, ETIMEDOUT);
		assert(record->retained == hang + 1U);
	}

	/* The last hang was past the limit: no reset was made for it, and video stopped. */
	assert(fixture_seen.engine_resets == I915_WORKER_VIDEO_HANG_LIMIT);
	assert(worker->video_hangs == I915_WORKER_VIDEO_HANG_LIMIT + 1U);
	assert(worker->video_recovered == I915_WORKER_VIDEO_HANG_LIMIT);
	assert(worker->video_dead == 1);
	error = drv_i915_worker_video_state(&fixture_device);
	assert(error == EIO);

	/*
	 * The attaches freed the records recovered before them, so only the
	 * last recovered record and the one past the limit are still kept; a
	 * reclaim frees the recovered one and keeps the other.
	 */
	drv_i915_worker_video_reclaim(&fixture_device);
	for (hang = 0U; hang < I915_WORKER_VIDEO_CONTEXTS; hang++) {
		record = &worker->video_contexts[hang];
		assert(record->retained == 0U || record->retained == I915_WORKER_VIDEO_HANG_LIMIT + 1U);
	}
	assert(worker->live_contexts == 1U);
	fixture_device_free();
}

/* The reset of the video engine record recovers every hang and lifts the stop; the other records still have none. */
static void
fixture_record_reset(void)
{
	struct i915_ppgtt vm;
	struct i915_context video;
	struct i915_context later;
	struct i915_worker *worker;
	struct i915_worker_context *record;
	int error;

	fixture_device_make(1);
	worker = fixture_device.worker;
	memset(&vm, 0, sizeof(vm));

	/* A hang whose engine reset fails stops video. */
	fixture_context_make(&video, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &video);
	assert(error == 0);
	record = i915_worker_find(worker, &video);
	fixture_reset_error = EIO;
	i915_worker_video_hung(worker, record, ETIMEDOUT);
	assert(worker->video_dead == 1);

	/* A reset of the video record that fails leaves the stop. */
	error = drv_i915_worker_engine_reset(&fixture_device.engines[I915_ENGINE_VCS0]);
	assert(error == EIO);
	assert(worker->video_dead == 1);
	assert(worker->video_recovered == 0U);

	/* One that succeeds recovers the hang and lifts the stop; the reclaim then frees the record. */
	fixture_reset_error = 0;
	error = drv_i915_worker_engine_reset(&fixture_device.engines[I915_ENGINE_VCS0]);
	assert(error == 0);
	assert(worker->video_dead == 0);
	assert(worker->video_recovered == 1U);
	drv_i915_worker_video_reclaim(&fixture_device);
	assert(record->retained == 0U);
	assert(worker->live_contexts == 0U);

	/* Video takes a new context again. */
	fixture_context_make(&later, I915_ENGINE_VCS0, &vm);
	error = drv_i915_worker_context_attach(&fixture_device, &later);
	assert(error == 0);
	drv_i915_worker_context_destroy(&fixture_device, &later);

	/* The render and copy records are not reset here yet. */
	error = drv_i915_worker_engine_reset(&fixture_device.engines[I915_ENGINE_RCS0]);
	assert(error == ENOTSUP);
	error = drv_i915_worker_engine_reset(&fixture_device.engines[I915_ENGINE_BCS0]);
	assert(error == ENOTSUP);
	fixture_device_free();
}

/*
 * The stand-ins of the GT and the kernel services worker.c uses.  The ones a
 * case reaches do the least that case needs; any other ends the run.
 */

/* Allocates zeroed memory from the host. */
void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

/* Frees host memory. */
void
kern_free(
	void *pointer)
{
	free(pointer);
}

/* Prints a kernel log line on the host's output. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

/* Records the engine a context image is made on and gives it a context address. */
int
drv_i915_lrc_alloc(
	struct i915_gt_context *ce,
	struct i915_gt_engine *ge,
	struct i915_gt_ppgtt *vm,
	struct i915_gt_mem *gm,
	uint32_t ring_size,
	uint32_t sw_id)
{
	(void)vm;
	(void)gm;
	(void)sw_id;
	fixture_seen.lrc_allocs++;
	fixture_seen.last_lrc_engine = ge;
	ce->lrca = 0x1000U * fixture_seen.lrc_allocs;
	ce->ring.size = ring_size;
	return 0;
}

/* Counts a released context image. */
void
drv_i915_lrc_release(
	struct i915_gt_context *ce,
	struct i915_gt_mem *gm)
{
	(void)ce;
	(void)gm;
	fixture_seen.lrc_releases++;
}

/* Lays out nothing: the image is not read on the host. */
void
drv_i915_lrc_init_state(
	struct i915_gt_context *ce)
{
	(void)ce;
}

/* Writes no registers. */
uint32_t
drv_i915_lrc_update_regs(
	struct i915_gt_context *ce,
	uint32_t head)
{
	(void)ce;
	(void)head;
	return 0U;
}

/* Makes a GT object of host memory. */
struct i915_gt_object *
drv_i915_gt_object_create(
	struct i915_gt_mem *gm,
	uint32_t bytes)
{
	struct i915_gt_object *object;

	(void)gm;
	object = calloc(1U, sizeof(*object));
	assert(object != NULL);
	object->cpu = calloc(1U, bytes);
	assert(object->cpu != NULL);
	object->bytes = bytes;
	return object;
}

/* Frees a GT object of host memory. */
void
drv_i915_gt_object_destroy(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	(void)gm;
	free(o->cpu);
	free(o);
}

/* Binds nothing: the GGTT is not modelled. */
int
drv_i915_gt_ggtt_bind(
	struct i915_gt_mem *gm,
	struct i915_gt_object *o)
{
	(void)gm;
	(void)o;
	return 0;
}

/* Starts a request: nothing to write on the host. */
int
drv_i915_request_create(
	struct i915_gt_request *rq,
	struct i915_gt_context *ce,
	uint32_t seqno,
	uint32_t hwsp_ggtt,
	volatile uint32_t *hwsp_cpu)
{
	(void)rq;
	(void)ce;
	(void)seqno;
	(void)hwsp_ggtt;
	(void)hwsp_cpu;
	return 0;
}

/* Gives room in a ring of the fixture's own. */
uint32_t *
drv_i915_ring_begin(
	struct i915_gt_request *rq,
	unsigned num_dwords)
{
	static uint32_t ring[64];

	(void)rq;
	assert(num_dwords <= 64U);
	return ring;
}

/* Advances nothing. */
void
drv_i915_ring_advance(
	struct i915_gt_request *rq,
	uint32_t *cs)
{
	(void)rq;
	(void)cs;
}

/* Closes a request: nothing to write on the host. */
int
drv_i915_request_add(
	struct i915_gt_request *rq)
{
	(void)rq;
	return 0;
}

/* Records the engine and the execlists a request is submitted to, and refuses it, so that no wait follows. */
int
drv_i915_execlists_submit(
	struct i915_gt_engine *ge,
	struct i915_execlists *el,
	struct i915_mmio *mmio,
	struct i915_gt_request *rq)
{
	(void)mmio;
	(void)rq;
	fixture_seen.last_submit_engine = ge;
	fixture_seen.last_submit_lists = el;
	return EIO;
}

/* Records the engine a reset was asked of and reports what the case set. */
int
drv_i915_engine_reset(
	struct i915_gt_engines *es,
	struct i915_gt_init *gi,
	const struct i915_gt_info *gt,
	unsigned index,
	struct i915_mmio *mmio,
	struct spinlock *uncore_lock)
{
	(void)es;
	(void)gi;
	(void)gt;
	(void)mmio;
	(void)uncore_lock;
	fixture_seen.engine_resets++;
	fixture_seen.last_reset_index = index;
	return fixture_reset_error;
}

/* Takes no lock on the host: the fixture is one thread. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0UL;
}

/* Releases nothing. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

/* Prepares nothing. */
void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	(void)queue;
	(void)name;
}

/* Prepares nothing. */
void
drv_i915_park_init(
	struct i915_park *park)
{
	(void)park;
}

/* Every other service ends the run: no case reaches it. */
#define FIXTURE_UNREACHED(name) \
	do { \
		fprintf(stderr, "ws083 vcs worker host test: %s reached\n", name); \
		abort(); \
	} while (0)

void drv_i915_device_park_gt(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_device_park_gt"); }
int drv_i915_device_unpark_gt(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_device_unpark_gt"); }
int drv_i915_display_backlight_register(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_display_backlight_register"); }
int drv_i915_display_backlight_serve(struct i915_device *device, int in_display, int op, uint32_t *value) { (void)device; (void)in_display; (void)op; (void)value; FIXTURE_UNREACHED("drv_i915_display_backlight_serve"); }
void drv_i915_display_backlight_unregister(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_display_backlight_unregister"); }
struct i915_gt_request *drv_i915_execlists_process_csb(struct i915_gt_engine *ge, struct i915_execlists *el, struct i915_mmio *mmio) { (void)ge; (void)el; (void)mmio; FIXTURE_UNREACHED("drv_i915_execlists_process_csb"); }
uint64_t drv_i915_irq_engine_sequence(struct i915_irq_dev *irq) { (void)irq; FIXTURE_UNREACHED("drv_i915_irq_engine_sequence"); }
int drv_i915_irq_engine_wait(struct i915_irq_dev *irq, uint64_t observed, unsigned ticks) { (void)irq; (void)observed; (void)ticks; FIXTURE_UNREACHED("drv_i915_irq_engine_wait"); }
enum i915_park_action drv_i915_park_action(const struct i915_park *park, int in_display, int stop) { (void)park; (void)in_display; (void)stop; FIXTURE_UNREACHED("drv_i915_park_action"); }
int drv_i915_park_begin(struct i915_park *park) { (void)park; FIXTURE_UNREACHED("drv_i915_park_begin"); }
int drv_i915_park_end(struct i915_park *park) { (void)park; FIXTURE_UNREACHED("drv_i915_park_end"); }
void drv_i915_park_entered(struct i915_park *park) { (void)park; FIXTURE_UNREACHED("drv_i915_park_entered"); }
void drv_i915_park_left(struct i915_park *park) { (void)park; FIXTURE_UNREACHED("drv_i915_park_left"); }
int drv_i915_park_reached(const struct i915_park *park, uint32_t generation) { (void)park; (void)generation; FIXTURE_UNREACHED("drv_i915_park_reached"); }
int drv_i915_park_stays(const struct i915_park *park, int stop) { (void)park; (void)stop; FIXTURE_UNREACHED("drv_i915_park_stays"); }
void drv_i915_perf_add(struct i915_perf *perf, enum i915_perf_stage stage, uint64_t start) { (void)perf; (void)stage; (void)start; FIXTURE_UNREACHED("drv_i915_perf_add"); }
uint64_t drv_i915_perf_now(void) { FIXTURE_UNREACHED("drv_i915_perf_now"); }
int drv_i915_present_blob_frame(struct i915_device *device, const struct i915_worker_present *frame) { (void)device; (void)frame; FIXTURE_UNREACHED("drv_i915_present_blob_frame"); }
unsigned drv_i915_present_count(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_count"); }
int drv_i915_present_frame(struct i915_device *device, const struct i915_worker_present *frame) { (void)device; (void)frame; FIXTURE_UNREACHED("drv_i915_present_frame"); }
int drv_i915_present_hold_over(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_hold_over"); }
void drv_i915_present_hold_prepare(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_hold_prepare"); }
int drv_i915_present_hold_start(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_hold_start"); }
void drv_i915_present_window(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_window"); }
int drv_i915_present_window_ready(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_present_window_ready"); }
int drv_i915_publish(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_publish"); }
int drv_i915_request_completed(const struct i915_gt_request *rq) { (void)rq; FIXTURE_UNREACHED("drv_i915_request_completed"); }
void drv_i915_request_complete_list(struct i915_engine *engine, struct i915_request *retired) { (void)engine; (void)retired; FIXTURE_UNREACHED("drv_i915_request_complete_list"); }
void drv_i915_request_fail(struct i915_engine *engine, struct i915_session *session, int error, struct i915_request **retired) { (void)engine; (void)session; (void)error; (void)retired; FIXTURE_UNREACHED("drv_i915_request_fail"); }
void drv_i915_rps_boost_begin(struct i915_rps *rps) { (void)rps; FIXTURE_UNREACHED("drv_i915_rps_boost_begin"); }
void drv_i915_rps_boost_end(struct i915_rps *rps) { (void)rps; FIXTURE_UNREACHED("drv_i915_rps_boost_end"); }
void drv_i915_rps_busy_begin(struct i915_rps *rps) { (void)rps; FIXTURE_UNREACHED("drv_i915_rps_busy_begin"); }
void drv_i915_rps_busy_end(struct i915_rps *rps) { (void)rps; FIXTURE_UNREACHED("drv_i915_rps_busy_end"); }
int drv_i915_udelay(unsigned microseconds) { (void)microseconds; FIXTURE_UNREACHED("drv_i915_udelay"); }
int drv_i915_unpublish(struct i915_device *device) { (void)device; FIXTURE_UNREACHED("drv_i915_unpublish"); }
hal_cpu_id_t hal_cpu_current(void) { FIXTURE_UNREACHED("hal_cpu_current"); }
uint64_t sched_ticks(void) { FIXTURE_UNREACHED("sched_ticks"); }
uint64_t waitq_sequence(const struct wait_queue *queue) { (void)queue; FIXTURE_UNREACHED("waitq_sequence"); }
int waitq_sleep(struct wait_queue *queue, struct spinlock *condition_lock, uint64_t observed, uint64_t deadline, unsigned flags) { (void)queue; (void)condition_lock; (void)observed; (void)deadline; (void)flags; FIXTURE_UNREACHED("waitq_sleep"); }
void waitq_wake_all(struct wait_queue *queue) { (void)queue; FIXTURE_UNREACHED("waitq_wake_all"); }
