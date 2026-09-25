/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The request worker (see worker.h).
 *
 * The session operations queue under the device's IRQ lock and kick; the
 * worker takes the work in arrival order, drops the lock, runs it to its
 * end, and delivers the result.  A request is written as the reference's
 * execbuf writes one: the initial breadcrumb (gen8_emit_init_breadcrumb()),
 * the batch start (gen8_emit_bb_start()) unless the request is a marker, and
 * the final breadcrumb the request add writes.  Its end is found by polling
 * the context status buffer, as the reference's wait path does when it runs
 * the submission tasklet inline.
 */

#include "context.h"
#include "device-info.h"
#include "display/present.h"
#include "engine.h"
#include "ggtt.h"
#include "i915.h"
#include "memory.h"
#include "ppgtt.h"
#include "request-queue.h"
#include "request.h"
#include "submit.h"
#include "sync.h"
#include "worker.h"
#include "perf.h"
#include <kern/kcrt.h>

#include <kern/clock.h>
#include <hal/hal.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/commands.h"

/* How many session contexts of the render engine can be live at once. */
#define I915_WORKER_CONTEXTS		8U

/* The ring size of a session context. */
#define I915_WORKER_RING_BYTES		16384U

/* How long a request may run before it is failed as a hang. */
#define I915_WORKER_TIMEOUT_MS		10000U

/* The pause between two polls of the context status buffer, and how many polls make the timeout. */
#define I915_WORKER_POLL_US		50U
#define I915_WORKER_POLLS		(I915_WORKER_TIMEOUT_MS * 20U)

/* The ring room a request needs at most; a ring with less left is rewound. */
#define I915_WORKER_REQUEST_ROOM	512U

/* How long the worker sleeps before it looks again for work it was not woken for (one second). */
#define I915_WORKER_SLEEP_TICKS		(1U * KERN_CLOCK_HZ)

/* The size of a context's timeline page. */
#define I915_WORKER_TIMELINE_BYTES	4096U

/*
 * Why a pass of the worker loop returned: a stop was asked for, a
 * presentation waits outside the display window (the caller lights the
 * panel), or a release waits inside it (it is completed after the stop).
 */
#define I915_WORKER_SERVE_STOP		0
#define I915_WORKER_SERVE_ENTER_DISPLAY	1
#define I915_WORKER_SERVE_LEAVE_DISPLAY	2

/*
 * The hardware context behind one session context of the render engine.
 *
 * A record of the worker's table is free while owner is NULL.  Context
 * create fills it under the device mutex and context destroy empties it;
 * the worker uses it only while running a request of that context.
 */
struct i915_worker_context {
	/* The session context this record stands behind; NULL for a free record. */
	struct i915_context *owner;

	/*
	 * The address space the logical ring context names.  Only top_pd_dma is
	 * read: it is the top table of the session's own address space.
	 */
	struct i915_gt_ppgtt vm;

	/* The logical ring context: its image and its ring. */
	struct i915_gt_context ce;

	/* The timeline page in the GGTT the breadcrumbs land in, and the last seqno given. */
	struct i915_gt_object *tl_page;
	uint32_t tl_seqno;

	/* The one request of the context; the worker runs them one at a time. */
	struct i915_gt_request rq;
};

/*
 * One item a caller sleeps on while the worker does it: a batch, a
 * presentation or a release of the panel.
 *
 * It lives on the caller's stack from the queuing to the completion; the
 * device's IRQ lock protects its link, done and error.
 */
struct i915_worker_sync {
	/* The next queued item. */
	struct i915_worker_sync *next;

	/* What the item asks for. */
	enum i915_worker_sync_kind kind;

	/* A batch: the context it runs in, and the batch's address in its space. */
	struct i915_context *context;
	uint64_t batch_va;

	/* A presentation: the frame, read by the worker while the caller sleeps. */
	const struct i915_worker_present *present;

	/* Nonzero once the worker ran the batch, and how it ended. */
	int done;
	int error;
};

/*
 * The request worker of one device.
 *
 * The device owns it from drv_i915_worker_create() to
 * drv_i915_worker_destroy().  The device's IRQ lock protects the two queues
 * and the stop flag; the context table is changed under the device mutex.
 */
struct i915_worker {
	/* The device the worker serves. */
	struct i915_device *device;

	/* The index of the render engine in the GT's engine set. */
	int render_index;

	/* The hardware contexts behind the session contexts of the render engine. */
	struct i915_worker_context contexts[I915_WORKER_CONTEXTS];

	/* The requests the kick handed over, oldest first. */
	struct i915_request *run_head;
	struct i915_request *run_tail;

	/* The synchronous batches waiting to run, oldest first. */
	struct i915_worker_sync *sync_head;
	struct i915_worker_sync *sync_tail;

	/* Where synchronous callers wait for their batch, and where the worker waits for work. */
	struct wait_queue sync_done;
	struct wait_queue work;

	/* Nonzero from the publication of the node to its withdrawal: work may be accepted. */
	int serving;

	/* Nonzero once a stop was asked for: the worker finishes the queued work and withdraws the node. */
	int stop;

	/* How much work ended well and how much failed. */
	unsigned executed;
	unsigned failed;

	/* How many context records are in use, and how many were ever filled. */
	unsigned live_contexts;
	unsigned contexts_ever;
};

static int i915_worker_loop(struct i915_worker *worker, int in_display);
static void i915_worker_run_request(struct i915_worker *worker, struct i915_request *request);
static void i915_worker_run_sync_item(struct i915_worker *worker, struct i915_worker_sync *item, int in_display);
static int i915_worker_queue_sync(struct i915_device *device, struct i915_worker_sync *item);
static int i915_worker_run(struct i915_worker *worker, struct i915_context *context, uint64_t batch_va, int has_batch, uint32_t label);
static int i915_worker_emit(struct i915_gt_request *rq, uint64_t batch_va, int has_batch);
static int i915_worker_wait(struct i915_worker *worker, struct i915_gt_request *rq, uint64_t batch_va, uint32_t label);
static struct i915_worker_context *i915_worker_find(struct i915_worker *worker, const struct i915_context *context);

/*
 * Creates the request worker of a device.
 *
 * The GT engines must be set up.  Returns ENODEV when the GT has no render
 * engine, ENOMEM when the worker's state cannot be allocated, and EBUSY when
 * the device already has a worker.
 */
int
drv_i915_worker_create(
	struct i915_device *device)
{
	struct i915_worker *worker;
	struct i915_gt_engines *engines;
	unsigned index;

	/* A device has one worker. */
	if (device->worker != NULL)
		return EBUSY;

	/* Allocates the worker's state. */
	worker = kern_calloc(1U, sizeof(*worker));
	if (worker == NULL)
		return ENOMEM;

	/* Finds the render engine every request runs on. */
	worker->device = device;
	worker->render_index = -1;
	engines = &device->gt.engines;
	for (index = 0U; index < engines->n; index++) {
		if (engines->ge[index].info->class == I915_RENDER_CLASS) {
			worker->render_index = (int)index;
			break;
		}
	}

	/* A GT without a render engine has nothing to serve on. */
	if (worker->render_index < 0) {
		kern_logf("i915: resident: no render engine; not serving\n");
		kern_free(worker);
		return ENODEV;
	}

	/* Prepares the two wait queues. */
	waitq_init(&worker->work, "i915 resident");
	waitq_init(&worker->sync_done, "i915 resident sync");

	device->worker = worker;

	/* Succeeded: the worker is ready to serve. */
	return 0;
}

/*
 * Publishes the GPU node, serves it until a stop is asked for, and withdraws it.
 *
 * Runs on the thread that brought the device up, which holds forcewake for
 * the whole service.  Returns once the node was withdrawn, or the error of a
 * failed publication.  A withdrawal refused because sessions are still open
 * is logged and the call still returns 0, so the device stop runs anyway.
 */
int
drv_i915_worker_serve(
	struct i915_device *device)
{
	struct i915_worker *worker;
	int outcome;
	int error;

	/* A device without a worker cannot serve. */
	worker = device->worker;
	if (worker == NULL)
		return ENODEV;

	/* Work is accepted from the moment the node can be opened. */
	worker->serving = 1;

	/* Publishes the GPU node. */
	error = drv_i915_publish(device);
	if (error != 0) {
		kern_logf("i915: resident: publish failed: %d\n", error);
		worker->serving = 0;
		return error;
	}

	kern_logf("i915: resident: GPU node published; serving (RCS0, one request at a time, CSB polling) on cpu %u\n", (unsigned)hal_cpu_current());

	/*
	 * Runs the queued work until a stop is asked for.  The first
	 * presentation makes the loop return: the panel is lit and the display
	 * window serves everything until the release, which is completed by the
	 * next pass of the loop.
	 */
	for (;;) {
		outcome = i915_worker_loop(worker, 0);
		if (outcome != I915_WORKER_SERVE_ENTER_DISPLAY)
			break;

		drv_i915_present_window(device);
	}

	kern_logf("i915: resident: stopping (executed=%u failed=%u presented=%u)\n",
	    worker->executed,
	    worker->failed,
	    drv_i915_present_count(device));

	/* Withdraws the node; open sessions keep it, and the device stop runs anyway. */
	error = drv_i915_unpublish(device);
	if (error != 0)
		kern_logf("i915: resident: XXX unpublish rc=%d (sessions still open); the teardown runs anyway\n", error);

	/* No work is accepted from here on. */
	worker->serving = 0;

	/* Succeeded: the service ended. */
	return 0;
}

/*
 * Asks the worker to stop serving.
 *
 * The worker finishes the work already queued and then withdraws the node;
 * drv_i915_worker_serve() returns once it has.
 */
void
drv_i915_worker_stop(
	struct i915_device *device)
{
	struct i915_worker *worker;
	unsigned long irq;

	/* A device without a worker serves nothing. */
	worker = device->worker;
	if (worker == NULL)
		return;

	/* Marks the stop and wakes the worker to see it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	worker->stop = 1;
	waitq_wake_all(&worker->work);

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Frees the request worker of a device.
 *
 * The worker must have stopped serving and every session context must be
 * gone; a worker that is still needed is kept and reported.
 */
void
drv_i915_worker_destroy(
	struct i915_device *device)
{
	struct i915_worker *worker;

	/* A device without a worker has nothing to free. */
	worker = device->worker;
	if (worker == NULL)
		return;

	/* A serving worker, or one that still holds contexts, is kept. */
	if (worker->serving != 0 || worker->live_contexts != 0U) {
		kern_logf("i915: worker still in use (serving=%d contexts=%u); kept\n",
		    worker->serving,
		    worker->live_contexts);
		return;
	}

	device->worker = NULL;
	kern_free(worker);
}

/*
 * Creates a session context on an engine record.
 *
 * On the render engine the worker fills a hardware context over the
 * session's address space.  The caller holds the device mutex.  Returns
 * ENODEV when the worker is not serving and ENOMEM when every context record
 * is in use.
 */
int
drv_i915_worker_context_create(
	struct i915_device *device,
	struct i915_engine *engine,
	struct i915_ppgtt *vm,
	uint32_t sw_id,
	struct i915_context *context)
{
	struct i915_worker *worker;
	struct i915_worker_context *record;
	unsigned index;
	int error;

	/* Starts the session context as a record of its engine, space and id. */
	kern_memset(context, 0, sizeof(*context));
	context->engine = engine;
	context->vm = vm;
	context->sw_id = sw_id;

	/*
	 * XXX: only the render engine is connected.  Open makes one context per
	 * engine record, so this is reached on every open; the context is a
	 * record only and a request on it fails in the worker.
	 */
	if (engine->index != I915_ENGINE_RCS0) {
		context->created = 1U;
		return 0;
	}

	/* A worker that is not serving accepts no context. */
	worker = device->worker;
	if (worker == NULL || worker->serving == 0)
		return ENODEV;

	/* Finds a free context record. */
	record = NULL;
	for (index = 0U; index < I915_WORKER_CONTEXTS; index++) {
		if (worker->contexts[index].owner == NULL) {
			record = &worker->contexts[index];
			break;
		}
	}

	/* Every record is in use. */
	if (record == NULL) {
		kern_logf("i915: resident shim: XXX no free context record (%u in use)\n", I915_WORKER_CONTEXTS);
		return ENOMEM;
	}

	kern_memset(record, 0, sizeof(*record));

	/*
	 * The address space is the session's own (plain memory, built by
	 * ppgtt.c).  The logical ring context reads exactly one thing from it:
	 * the top-level table address for PDP0.
	 */
	record->vm.top_pd_dma = (uint64_t)vm->pml4.paddr;
	record->vm.inited = 1;

	/* Allocates the image and the ring on the render engine (intel_context_create()). */
	error = drv_i915_lrc_alloc(
		&record->ce,
		&device->gt.engines.ge[worker->render_index],
		&record->vm,
		&device->gt.mem,
		I915_WORKER_RING_BYTES,
		sw_id);
	if (error != 0) {
		kern_logf("i915: resident shim: intel_context_create failed rc=%d\n", error);
		return error;
	}

	/* Allocates the timeline page the breadcrumbs land in. */
	record->tl_page = drv_i915_gt_object_create(&device->gt.mem, I915_WORKER_TIMELINE_BYTES);
	if (record->tl_page == NULL) {
		drv_i915_lrc_release(&record->ce, &device->gt.mem);
		return ENOMEM;
	}

	/* Binds the timeline page into the GGTT, where the breadcrumbs address it. */
	error = drv_i915_gt_ggtt_bind(&device->gt.mem, record->tl_page);
	if (error != 0) {
		drv_i915_gt_object_destroy(&device->gt.mem, record->tl_page);
		drv_i915_lrc_release(&record->ce, &device->gt.mem);
		return error;
	}

	/* Lays out the register state and points the image at the empty ring. */
	drv_i915_lrc_init_state(&record->ce);
	(void)drv_i915_lrc_update_regs(&record->ce, record->ce.ring.tail);

	/* The owner makes the record the session context's; the counts keep the worker alive. */
	record->tl_seqno = 0U;
	record->owner = context;
	worker->live_contexts++;
	worker->contexts_ever++;
	context->created = 1U;
	kern_logf("i915: resident shim: context sw_id=%u lrca=%08x pml4=0x%llx ring=%u bytes\n",
	    sw_id,
	    record->ce.lrca,
	    (unsigned long long)record->vm.top_pd_dma,
	    I915_WORKER_RING_BYTES);

	/* Succeeded: requests of the session context run in the hardware context. */
	return 0;
}

/*
 * Destroys a session context.
 *
 * The hardware context behind it is released while the worker serves.  The
 * caller holds the device mutex.
 */
void
drv_i915_worker_context_destroy(
	struct i915_device *device,
	struct i915_context *context)
{
	struct i915_worker *worker;
	struct i915_worker_context *record;

	/* Finds the hardware context behind the session context, if any. */
	worker = device->worker;
	record = NULL;
	if (worker != NULL)
		record = i915_worker_find(worker, context);

	/*
	 * Releases the hardware context while the worker serves.
	 *
	 * XXX: happy path only -- the context is idle here because every
	 * request ran to its end.
	 */
	if (record != NULL && worker->serving != 0) {
		/* Frees the timeline page. */
		if (record->tl_page != NULL)
			drv_i915_gt_object_destroy(&device->gt.mem, record->tl_page);

		/* Frees the image and the ring. */
		drv_i915_lrc_release(&record->ce, &device->gt.mem);

		/* NULL frees the record; the live count no longer keeps the worker. */
		record->owner = NULL;
		if (worker->live_contexts != 0U)
			worker->live_contexts--;
	}

	context->created = 0U;
}

/*
 * Hands every queued request of an engine record to the worker, in order.
 *
 * The caller holds the device's IRQ lock.  Each request is given the
 * record's next sequence number and becomes ACTIVE.
 */
void
drv_i915_worker_kick(
	struct i915_engine *engine)
{
	struct i915_worker *worker;
	struct i915_request *request;

	/* A device without a worker leaves the requests queued. */
	worker = engine->device->worker;
	if (worker == NULL) {
		kern_logf("i915: XXX request kicked with no worker; left queued\n");
		return;
	}

	/* Moves the engine queue onto the worker's run list. */
	request = engine->queue_head;
	while (request != NULL) {
		/* Takes the head request off the engine queue. */
		engine->queue_head = request->next;
		if (engine->queue_head == NULL)
			engine->queue_tail = NULL;

		request->next = NULL;

		/* ACTIVE with a sequence number: the request now belongs to the worker. */
		request->seqno = engine->next_seqno;
		engine->next_seqno++;
		request->state = I915_REQUEST_ACTIVE;

		/* Appends it to the run list. */
		if (worker->run_tail == NULL) {
			worker->run_head = request;
		} else {
			worker->run_tail->next = request;
		}

		worker->run_tail = request;
		request = engine->queue_head;
	}

	/* Wakes the worker to run them. */
	waitq_wake_all(&worker->work);
}

/*
 * Runs one batch of a context to its end and reports how it ended.
 *
 * The caller sleeps; the worker runs the batch in the order it arrived,
 * exactly as it runs a request.  Returns 0, ETIMEDOUT for a hang (no
 * recovery follows), or ENODEV when the worker is not serving.
 */
int
drv_i915_worker_run_sync(
	struct i915_device *device,
	struct i915_context *context,
	uint64_t batch_va)
{
	struct i915_worker_sync item;
	uint64_t start;
	int error;

	/* Describes the batch. */
	kern_memset(&item, 0, sizeof(item));
	item.kind = I915_WORKER_SYNC_BATCH;
	item.context = context;
	item.batch_va = batch_va;

	/* Queues the batch and sleeps until it has run; the whole round trip is timed. */
	start = drv_i915_perf_now();
	error = i915_worker_queue_sync(device, &item);
	drv_i915_perf_add(&device->perf, I915_PERF_RUN, start);
	if (error != 0)
		return error;

	/* Succeeded: the batch ran to its end. */
	return 0;
}

/*
 * Queues a presentation or a release of the panel and waits until the
 * worker has done it.
 *
 * The worker enters the display window for the first presentation and
 * leaves it for the release, in the order the items arrived.  Returns 0,
 * ENODEV when the worker is not serving, or the item's error.
 */
int
drv_i915_worker_sync_display(
	struct i915_device *device,
	enum i915_worker_sync_kind kind,
	const struct i915_worker_present *present)
{
	struct i915_worker_sync item;
	int error;

	/* Describes the item. */
	kern_memset(&item, 0, sizeof(item));
	item.kind = kind;
	item.present = present;

	/* Queues it and sleeps until the worker has done it. */
	error = i915_worker_queue_sync(device, &item);
	if (error != 0)
		return error;

	/* Succeeded: the panel shows the frame, or is given back. */
	return 0;
}

/*
 * Serves every piece of work from inside the display window.
 *
 * Runs on the worker, called back by the display window while the panel
 * is up; returns when a release reaches the head of the queue or a stop is
 * asked for.
 */
void
drv_i915_worker_serve_window(
	struct i915_device *device)
{
	/* The display window's pass of the loop; its outcome is the caller's. */
	(void)i915_worker_loop(device->worker, 1);
}

/*
 * Runs one batch of a context to its end on the worker's own thread.
 *
 * Only the worker calls it, from inside the display window (the GPU copy of
 * a presentation).  Returns 0, or the run's error.
 */
int
drv_i915_worker_run_batch(
	struct i915_device *device,
	struct i915_context *context,
	uint64_t batch_va)
{
	int error;

	/* Runs the batch as a request with a batch. */
	error = i915_worker_run(device->worker, context, batch_va, 1, 0U);
	if (error != 0)
		return error;

	/* Succeeded: the batch ran to its end. */
	return 0;
}

/*
 * Resets one engine record's engine.
 *
 * XXX: unimplemented path.  Linux: intel_engine_reset() -> execlists
 * reset_prepare/rewind/finish.  Logs and returns ENOTSUP.
 */
int
drv_i915_worker_engine_reset(
	struct i915_engine *engine)
{
	/* Names the missing path in the log. */
	kern_logf("i915: resident shim: XXX unimplemented path: engine_reset(engine %u)\n", engine->index);

	/* The reset is not implemented. */
	return ENOTSUP;
}

/*
 * Ends a session's work on one engine record and recovers the engine.
 *
 * XXX: unimplemented path.  The intended sequence is: stop the engine, fail
 * the requests of the session, reset, resume.  Logs and returns ENOTSUP.
 */
int
drv_i915_worker_engine_recover(
	struct i915_engine *engine,
	struct i915_session *session,
	int error)
{
	UNUSED_PARAMETER(session);

	/* Names the missing path in the log. */
	kern_logf("i915: resident shim: XXX unimplemented path: engine_recover(engine %u, error %d)\n",
	    engine->index,
	    error);

	/* The recovery is not implemented. */
	return ENOTSUP;
}

/*
 * Resets the whole GT under a published node.
 *
 * XXX: unimplemented path.  The GT reset exists (drv_i915_gt_reset_all());
 * it is not wired to a published node.  Logs and returns ENOTSUP.
 */
int
drv_i915_worker_gt_reset(
	struct i915_device *device)
{
	UNUSED_PARAMETER(device);

	/* Names the missing path in the log. */
	kern_logf("i915: resident shim: XXX unimplemented path: gt_reset on a published node\n");

	/* The reset is not implemented. */
	return ENOTSUP;
}

/*
 * Runs queued work in arrival order until a stop is asked for and nothing
 * is left to run, or until a display item belongs to the other side of the
 * display window: a presentation outside it (the caller lights the panel)
 * or a release inside it (it is completed after the panel stops).  Returns
 * one of I915_WORKER_SERVE_*.
 */
static int
i915_worker_loop(
	struct i915_worker *worker,
	int in_display)
{
	struct i915_device *device;
	struct i915_request *request;
	struct i915_worker_sync *item;
	uint64_t observed;
	unsigned long irq;
	int ready;

	device = worker->device;

	/* Takes each piece of work under the IRQ lock and runs it with the lock dropped. */
	irq = spin_lock_irqsave(&device->irq_lock);

	for (;;) {
		/* Sleeps in bounded steps until work arrives or a stop is asked for. */
		while (worker->run_head == NULL &&
		    worker->sync_head == NULL &&
		    worker->stop == 0) {
			observed = waitq_sequence(&worker->work);
			(void)waitq_sleep(&worker->work, &device->irq_lock, observed, sched_ticks() + I915_WORKER_SLEEP_TICKS, 0U);
		}

		/* Runs a synchronous item before the next request. */
		if (worker->sync_head != NULL) {
			item = worker->sync_head;

			/* A presentation outside the window enters it, unless the panel cannot come up. */
			if ((item->kind == I915_WORKER_SYNC_PRESENT || item->kind == I915_WORKER_SYNC_PRESENT_BLOB) && !in_display) {
				ready = drv_i915_present_window_ready(device);
				if (ready) {
					spin_unlock_irqrestore(&device->irq_lock, irq);
					return I915_WORKER_SERVE_ENTER_DISPLAY;
				}
			}

			/* A release inside the window leaves it; it stays at the head. */
			if (item->kind == I915_WORKER_SYNC_RELEASE && in_display) {
				spin_unlock_irqrestore(&device->irq_lock, irq);
				return I915_WORKER_SERVE_LEAVE_DISPLAY;
			}

			worker->sync_head = item->next;
			if (worker->sync_head == NULL)
				worker->sync_tail = NULL;

			spin_unlock_irqrestore(&device->irq_lock, irq);

			i915_worker_run_sync_item(worker, item, in_display);

			irq = spin_lock_irqsave(&device->irq_lock);
			continue;
		}

		/* Ends once nothing is left to run and a stop was asked for. */
		if (worker->run_head == NULL && worker->stop != 0)
			break;

		/* Takes the oldest request off the run list. */
		request = worker->run_head;
		worker->run_head = request->next;
		if (worker->run_head == NULL)
			worker->run_tail = NULL;

		request->next = NULL;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		i915_worker_run_request(worker, request);

		irq = spin_lock_irqsave(&device->irq_lock);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A stop was asked for and nothing is left to run. */
	return I915_WORKER_SERVE_STOP;
}

/* Runs one request, retires it and delivers its completion; the IRQ lock is not held. */
static void
i915_worker_run_request(
	struct i915_worker *worker,
	struct i915_request *request)
{
	struct i915_device *device;
	struct i915_engine *engine;
	unsigned long irq;
	uint64_t start;
	int error;
	int has_batch;

	device = worker->device;
	engine = &device->engines[I915_ENGINE_RCS0];

	/* A request without a batch is a marker: the breadcrumbs alone. */
	has_batch = 0;
	if (request->batch != NULL)
		has_batch = 1;

	/* A marker's wait for the worker, and the start of its run, are timed. */
	start = 0U;
	if (!has_batch && request->queued_at != 0U) {
		drv_i915_perf_add(&device->perf, I915_PERF_MARKER_WAIT, request->queued_at);
		start = drv_i915_perf_now();
	}

	/*
	 * Runs the request to its end.  The worker runs one request at a time
	 * to completion, so a marker, which only orders itself after the
	 * requests before it, is complete the moment the worker reaches it: it
	 * is not sent to the engine.
	 */
	error = 0;
	if (has_batch)
		error = i915_worker_run(worker, request->context, request->batch_va, has_batch, request->seqno);

	/* Counts how it ended. */
	if (error == 0) {
		worker->executed++;
	} else {
		worker->failed++;
	}

	/* DONE with its outcome: the request has retired and waits for its completion. */
	irq = spin_lock_irqsave(&device->irq_lock);

	engine->completed_seqno = request->seqno;
	request->state = I915_REQUEST_DONE;
	request->error = error;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Delivers the completion, drops the session's count, frees the slot and wakes the waiters. */
	drv_i915_request_complete_list(engine, request);
	if (start != 0U)
		drv_i915_perf_add(&device->perf, I915_PERF_MARKER_RUN, start);
}

/* Does one synchronous item and wakes its caller; the IRQ lock is not held. */
static void
i915_worker_run_sync_item(
	struct i915_worker *worker,
	struct i915_worker_sync *item,
	int in_display)
{
	struct i915_device *device;
	unsigned long irq;
	uint64_t start;
	int error;

	device = worker->device;

	/* Does what the item asks for. */
	switch (item->kind) {
	case I915_WORKER_SYNC_BATCH:
		/* Runs the batch to its end, timing it on the engine, and counts how it ended. */
		start = drv_i915_perf_now();
		error = i915_worker_run(worker, item->context, item->batch_va, 1, 0U);
		drv_i915_perf_add(&device->perf, I915_PERF_GPU, start);
		if (error == 0) {
			worker->executed++;
		} else {
			worker->failed++;
		}
		break;
	case I915_WORKER_SYNC_PRESENT:
		/* Inside the window; outside it only when the panel could not be brought up. */
		error = EIO;
		if (in_display)
			error = drv_i915_present_frame(device, item->present);
		break;
	case I915_WORKER_SYNC_PRESENT_BLOB:
		/* Inside the window; outside it only when the panel could not be brought up. */
		error = EIO;
		if (in_display)
			error = drv_i915_present_blob_frame(device, item->present);
		break;
	default:
		/* A release with the panel down: nothing to stop. */
		error = 0;
		break;
	}

	/* Done with its outcome: the caller may return and its item goes away with its stack. */
	irq = spin_lock_irqsave(&device->irq_lock);

	item->error = error;
	item->done = 1;
	waitq_wake_all(&worker->sync_done);

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/* Runs one request of a session context on the render engine to its end. */
static int
i915_worker_run(
	struct i915_worker *worker,
	struct i915_context *context,
	uint64_t batch_va,
	int has_batch,
	uint32_t label)
{
	struct i915_device *device;
	struct i915_worker_context *record;
	struct i915_gt_request *rq;
	int error;

	device = worker->device;

	/* XXX: only the render engine is connected; a request on any other engine record fails. */
	if (context == NULL ||
	    context->engine == NULL ||
	    context->engine->index != I915_ENGINE_RCS0) {
		kern_logf("i915: resident shim: XXX unimplemented path: request on an engine other than RCS0\n");
		return ENOTSUP;
	}

	/* Finds the hardware context behind the session context. */
	record = i915_worker_find(worker, context);
	if (record == NULL)
		return EINVAL;

	/*
	 * XXX: the ring never wraps (a request that would is refused).  The
	 * context is idle between requests here, so the ring is simply rewound
	 * when it is nearly full.
	 */
	if (record->ce.ring.emit + I915_WORKER_REQUEST_ROOM > record->ce.ring.size) {
		record->ce.ring.head = 0U;
		record->ce.ring.tail = 0U;
		record->ce.ring.emit = 0U;
		(void)drv_i915_lrc_update_regs(&record->ce, 0U);
		kern_logf("i915: resident shim: ring rewound (idle context)\n");
	}

	/* Starts the request on the context's timeline; each request takes two seqnos. */
	rq = &record->rq;
	kern_memset(rq, 0, sizeof(*rq));
	record->tl_seqno += 2U;
	error = drv_i915_request_create(
		rq,
		&record->ce,
		record->tl_seqno,
		(uint32_t)record->tl_page->ggtt_offset,
		(volatile uint32_t *)record->tl_page->cpu);
	if (error != 0)
		return error;

	/* Writes the initial breadcrumb and the batch start. */
	error = i915_worker_emit(rq, batch_va, has_batch);
	if (error != 0)
		return error;

	/* Closes the request with the final breadcrumb. */
	error = drv_i915_request_add(rq);
	if (error != 0)
		return error;

	/* Submits the request through the render engine's execlists. */
	error = drv_i915_execlists_submit(
		&device->gt.engines.ge[worker->render_index],
		&device->gt.engines.el[worker->render_index],
		&device->gt.mmio,
		rq);
	if (error != 0) {
		kern_logf("i915: resident shim: execlists_submit rc=%d\n", error);
		return error;
	}

	/* Waits for the request to end. */
	error = i915_worker_wait(worker, rq, batch_va, label);
	if (error != 0)
		return error;

	/* Succeeded: the request ran to its end. */
	return 0;
}

/* Writes a request's initial breadcrumb and, unless it is a marker, its batch start. */
static int
i915_worker_emit(
	struct i915_gt_request *rq,
	uint64_t batch_va,
	int has_batch)
{
	uint32_t *cs;

	/* Reserves room for the initial breadcrumb (gen8_emit_init_breadcrumb()). */
	cs = drv_i915_ring_begin(rq, 6U);
	if (cs == NULL)
		return rq->error;

	/* Stores seqno - 1 in the timeline page: the request has started. */
	*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
	*cs++ = rq->hwsp_ggtt;
	*cs++ = 0U;
	*cs++ = rq->seqno - 1U;
	*cs++ = MI_NOOP;
	*cs++ = MI_ARB_CHECK;
	drv_i915_ring_advance(rq, cs);

	/* A marker (a job with no batch) is the breadcrumbs alone. */
	if (has_batch == 0)
		return 0;

	/* Reserves room for the batch start (gen8_emit_bb_start()). */
	cs = drv_i915_ring_begin(rq, 6U);
	if (cs == NULL)
		return rq->error;

	/* Starts the non-secure batch in the context's address space with arbitration on around it. */
	*cs++ = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	*cs++ = MI_BATCH_BUFFER_START_GEN8 | MI_BATCH_NON_SECURE_I965;
	*cs++ = (uint32_t)batch_va;
	*cs++ = (uint32_t)(batch_va >> 32);
	*cs++ = MI_ARB_ON_OFF;
	*cs++ = MI_NOOP;
	drv_i915_ring_advance(rq, cs);

	/* Succeeded: the request's commands are in the ring. */
	return 0;
}

/* Polls the context status buffer until a submitted request has ended, or times out. */
static int
i915_worker_wait(
	struct i915_worker *worker,
	struct i915_gt_request *rq,
	uint64_t batch_va,
	uint32_t label)
{
	struct i915_device *device;
	struct i915_gt_engine *ge;
	struct i915_execlists *el;
	unsigned poll;
	int completed;
	int error;

	device = worker->device;
	ge = &device->gt.engines.ge[worker->render_index];
	el = &device->gt.engines.el[worker->render_index];

	/* Processes the status buffer every 50 microseconds, for at most the timeout. */
	for (poll = 0U; poll < I915_WORKER_POLLS; poll++) {
		/* Applies what the engine reported. */
		(void)drv_i915_execlists_process_csb(ge, el, &device->gt.mmio);

		/* An event with nothing to apply to fails the request. */
		if (el->csb_errors != 0U)
			return EIO;

		/* Ends once the breadcrumb landed and the engine holds nothing more. */
		completed = drv_i915_request_completed(rq);
		if (completed != 0) {
			if (el->have_active == 0 && el->pending[0] == NULL)
				return 0;
		}

		/* Pauses before the next poll; a failed time base fails the request. */
		error = drv_i915_udelay(I915_WORKER_POLL_US);
		if (error != 0)
			return EIO;
	}

	/* XXX: unimplemented path -- a hang.  No reset, no recovery: the request fails and the log says so. */
	kern_logf("i915: resident shim: XXX request seqno=%u batch_va=0x%llx did not complete in %u ms "
	    "(no recovery path; hwsp=%u last_csb=%08x:%08x)\n",
	    label,
	    (unsigned long long)batch_va,
	    I915_WORKER_TIMEOUT_MS,
	    (unsigned)*rq->hwsp_cpu,
	    el->last_csb_hi,
	    el->last_csb_lo);

	/* The request did not end in time. */
	return ETIMEDOUT;
}

/* Queues one synchronous item, wakes the worker and sleeps until the item is done. */
static int
i915_worker_queue_sync(
	struct i915_device *device,
	struct i915_worker_sync *item)
{
	struct i915_worker *worker;
	uint64_t observed;
	unsigned long irq;

	/* A worker that is not serving does nothing. */
	worker = device->worker;
	if (worker == NULL || worker->serving == 0) {
		kern_logf("i915: resident shim: XXX unimplemented path: work outside resident mode\n");
		return ENODEV;
	}

	/* Queues the item and wakes the worker under the IRQ lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	if (worker->sync_tail == NULL) {
		worker->sync_head = item;
	} else {
		worker->sync_tail->next = item;
	}

	worker->sync_tail = item;
	waitq_wake_all(&worker->work);

	/* Sleeps in bounded steps until the worker marks the item done. */
	while (item->done == 0) {
		observed = waitq_sequence(&worker->sync_done);
		(void)waitq_sleep(&worker->sync_done, &device->irq_lock, observed, sched_ticks() + I915_WORKER_SLEEP_TICKS, 0U);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Reports how the item ended. */
	if (item->error != 0)
		return item->error;

	/* Succeeded: the item was done. */
	return 0;
}

/* Finds the hardware context record behind a session context, or NULL. */
static struct i915_worker_context *
i915_worker_find(
	struct i915_worker *worker,
	const struct i915_context *context)
{
	unsigned index;

	/* Scans the context table for the record the session context owns. */
	for (index = 0U; index < I915_WORKER_CONTEXTS; index++) {
		if (worker->contexts[index].owner == context)
			return &worker->contexts[index];
	}

	/* No record stands behind the session context. */
	return NULL;
}
