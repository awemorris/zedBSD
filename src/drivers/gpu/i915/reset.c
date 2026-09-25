/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT reset (see reset.h).
 *
 * The full reset follows Linux's __intel_gt_reset(ALL_ENGINES) through
 * gen8_reset_engines(), __gen11_reset_engines() and gen6_hw_domain_reset()
 * for an empty engine set: the reset runs before the engines are created, so
 * the reference's per-engine prepare and cancel iterations have nothing to
 * visit.
 */

#include "reset.h"
#include "mmio.h"
#include "sync.h"

#include "i915.h"
#include "memory.h"
#include "ppgtt.h"
#include "request-queue.h"
#include "session.h"
#include "worker.h"

#include <drivers/gpu/gpu.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/* GEN6_GDRST: the graphics domain reset register. */
#define I915_GDRST			0x941cU

/* GEN11_GRDOM_FULL: a full soft reset of every engine. */
#define I915_GRDOM_FULL			0x1U

/* How many attempts a reset whose acknowledge times out is given. */
#define I915_RESET_MAX_RETRIES		3U

/* How many write and poll passes one attempt makes on graphics IP before 12.70. */
#define I915_RESET_PASSES		2U

/* How long the engines are left to settle after the acknowledge (udelay(50)). */
#define I915_RESET_SETTLE_US		50U

static int i915_gt_reset_attempt(struct spinlock *uncore_lock, struct i915_mmio *mmio, unsigned fast_us, unsigned attempt);

/*
 * Resets every graphics domain (GEN11_GRDOM_FULL).
 *
 * The render and GT forcewake domains are held across the whole reset and,
 * inside each attempt, the uncore lock with interrupts off.  An attempt is
 * repeated, up to three in all, only while the acknowledge times out.  Each
 * acknowledge poll is a real-time busy wait bounded by fast_us.  Returns 0, a
 * forcewake error, ETIMEDOUT, or the time-base fault of the settle delay.
 */
int
drv_i915_gt_reset_all(
	struct spinlock *uncore_lock,
	struct i915_mmio *mmio,
	unsigned fast_us)
{
	unsigned attempt;
	int forcewake_error;
	int error;

	/* Wakes the render domain for the whole reset, outside the uncore lock. */
	forcewake_error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_RENDER);
	if (forcewake_error != 0)
		return forcewake_error;

	/* Wakes the GT domain as well, giving the render hold back if it cannot. */
	forcewake_error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_GT);
	if (forcewake_error != 0) {
		(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
		return forcewake_error;
	}

	/* Repeats the reset only while its acknowledge times out. */
	error = ETIMEDOUT;
	for (attempt = 0U; attempt < I915_RESET_MAX_RETRIES; attempt++) {
		error = i915_gt_reset_attempt(uncore_lock, mmio, fast_us, attempt);
		if (error != ETIMEDOUT)
			break;
	}

	/* Lets the GT and render domains sleep again. */
	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);

	/* Reports why the reset did not complete. */
	if (error != 0)
		return error;

	/* Succeeded: every graphics domain acknowledged its reset. */
	return 0;
}

/*
 * Runs one reset attempt under the uncore lock.
 *
 * Returns 0, ETIMEDOUT when the acknowledge never came, or the time-base
 * fault of the settle delay, which the caller must not retry.
 */
static int
i915_gt_reset_attempt(
	struct spinlock *uncore_lock,
	struct i915_mmio *mmio,
	unsigned fast_us,
	unsigned attempt)
{
	unsigned long irq;
	unsigned passes;
	int acknowledge_error;
	int delay_error;

	/*
	 * Requests the full reset and waits for the hardware to clear the
	 * request bit, twice on Alder Lake-P, stopping at the first pass that
	 * times out (gen8_reset_engines() under the uncore lock).
	 */
	irq = spin_lock_irqsave(uncore_lock);

	passes = 0U;
	for (;;) {
		drv_i915_raw_write32(mmio, I915_GDRST, I915_GRDOM_FULL);
		acknowledge_error = drv_i915_wait_reg(mmio, I915_GDRST, I915_GRDOM_FULL, 0U, fast_us, 0U, NULL);
		passes++;

		/* A pass that timed out ends the attempt. */
		if (acknowledge_error != 0)
			break;

		/* The attempt is complete after its last pass. */
		if (passes >= I915_RESET_PASSES)
			break;
	}

	/*
	 * Lets the engine state settle; it stays volatile briefly after the
	 * acknowledge.  A time base that faulted cannot bound the next wait
	 * either, so the reset is abandoned with the fault and not retried.
	 */
	delay_error = drv_i915_udelay(I915_RESET_SETTLE_US);
	if (delay_error != 0) {
		spin_unlock_irqrestore(uncore_lock, irq);
		kern_logf("i915: gt_reset attempt=%u passes=%u time-base fault rc=%d\n",
			  attempt,
			  passes,
			  delay_error);
		return delay_error;
	}

	spin_unlock_irqrestore(uncore_lock, irq);

	/* Records how the attempt ended. */
	kern_logf("i915: gt_reset attempt=%u passes=%u rc=%d\n",
		  attempt,
		  passes,
		  acknowledge_error);

	/* Reports a pass whose acknowledge timed out. */
	if (acknowledge_error != 0)
		return acknowledge_error;

	/* Succeeded: both passes were acknowledged. */
	return 0;
}

/*
 * ------------------------------------------------------------------------
 * Recovery of the GPU node.
 *
 * The GPU core's recovery operations (drv_gpu_recovery_ops).  Stop begin
 * and poll track a session's pending requests; the fault fails every
 * request and refuses new work; the checked reset frees what quarantine
 * retained once the hardware has been reset; isolate quarantines one
 * session.  The engine and GT resets they need are not connected to a
 * published node yet (worker.h): those calls log and fail, and the
 * operation fails with them.
 * ------------------------------------------------------------------------
 */

static int i915_stop_begin(void *opaque, void *private_session, int error);
static int i915_stop_poll(void *opaque, void *private_session);
static void i915_fault(void *opaque, int error);
static int i915_reset_device(void *opaque);
static int i915_isolate(void *opaque, void *private_session);
static void i915_quarantine_release(struct i915_device *device);

/* The recovery operations of the node. */
static const struct drv_gpu_recovery_ops i915_recovery_ops = {
	i915_stop_begin,
	i915_stop_poll,
	i915_fault,
	i915_reset_device,
	i915_isolate
};

/*
 * Binds the recovery operations into the GPU node's operation table.
 */
void
drv_i915_recovery_bind_ops(
	struct drv_gpu_ops *ops)
{
	/* Session stop, device fault, checked reset and session isolation. */
	ops->recovery = &i915_recovery_ops;
}

/* Marks a session as stopping; the GPU core already refuses new admission. */
static int
i915_stop_begin(
	void *opaque,
	void *private_session,
	int error)
{
	struct i915_device *device;
	struct i915_session *session;
	unsigned long irq;

	/* The reason is the GPU core's; the node only records the transition. */
	UNUSED_PARAMETER(error);

	device = opaque;
	session = private_session;

	/* Stopping makes reservations refuse the session from now on. */
	irq = spin_lock_irqsave(&device->irq_lock);

	session->stopping = 1U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: polls report whether the session's native work has retired. */
	return 0;
}

/* Reports whether every request of a stopping session has retired. */
static int
i915_stop_poll(
	void *opaque,
	void *private_session)
{
	struct i915_device *device;
	struct i915_session *session;
	unsigned pending;
	unsigned long irq;

	device = opaque;
	session = private_session;

	/* Samples the count of queued, running, reserved and callback-in-progress requests. */
	irq = spin_lock_irqsave(&device->irq_lock);

	pending = session->pending_requests;

	/* A session that was never told to stop cannot be polled. */
	if (session->stopping == 0U) {
		spin_unlock_irqrestore(&device->irq_lock, irq);
		return EINVAL;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Work still pending keeps the stop unconfirmed. */
	if (pending != 0U)
		return EAGAIN;

	/* Succeeded: no native work of this session remains. */
	return 0;
}

/* Ends every request on both engine records with the reported error and refuses new work. */
static void
i915_fault(
	void *opaque,
	int error)
{
	struct i915_device *device;
	struct i915_request *retired;
	unsigned index;
	unsigned long irq;

	device = opaque;

	/* The failed mark refuses new sessions, submissions and reservations from now on. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->failed = 1U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/*
	 * Fails each engine record's requests and delivers their callbacks
	 * outside the lock.
	 *
	 * XXX: requests already handed to the worker are not on the engine
	 * queue; they still run and complete with their own outcome.
	 */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		retired = NULL;

		/* Collects the failed requests under the IRQ lock. */
		irq = spin_lock_irqsave(&device->irq_lock);

		drv_i915_request_fail(&device->engines[index], NULL, error, &retired);

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* Delivers their completions. */
		drv_i915_request_complete_list(&device->engines[index], retired);
	}

	kern_logf("i915: device fault %d; objects retained for checked reset\n", error);
}

/* Reinitializes the hardware once no session owns it and frees quarantined state. */
static int
i915_reset_device(
	void *opaque)
{
	struct i915_device *device;
	unsigned index;
	int error;

	device = opaque;

	/* Resets under the device mutex, so no session can race it. */
	mutex_lock(&device->mutex);

	/* Ends every GPU access with a full GT reset before quarantined memory is freed. */
	error = drv_i915_worker_gt_reset(device);
	if (error != 0) {
		mutex_unlock(&device->mutex);
		return error;
	}

	/* Reprograms each engine from scratch with an empty queue. */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		error = drv_i915_worker_engine_reset(&device->engines[index]);
		if (error != 0) {
			mutex_unlock(&device->mutex);
			return error;
		}
	}

	/* Frees the quarantined objects and address spaces, which the GPU can no longer touch. */
	i915_quarantine_release(device);

	/* Clearing the failed mark lets fresh sessions open. */
	device->failed = 0U;
	kern_logf("i915: checked reset complete\n");

	mutex_unlock(&device->mutex);

	/* Succeeded: fresh sessions may open on the reinitialized device. */
	return 0;
}

/* Quarantines one session: its work ends with EIO and a stuck engine is reset. */
static int
i915_isolate(
	void *opaque,
	void *private_session)
{
	struct i915_device *device;
	struct i915_session *session;
	unsigned index;
	unsigned long irq;
	int error;

	device = opaque;
	session = private_session;

	/* Quarantine makes later destroy and close keep the session's objects for the checked reset. */
	irq = spin_lock_irqsave(&device->irq_lock);

	session->quarantined = 1U;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Fails the session's requests on each engine record and resets an engine that was running one. */
	for (index = 0U; index < I915_ENGINE_COUNT; index++) {
		error = drv_i915_worker_engine_recover(&device->engines[index], session, EIO);
		if (error != 0)
			return error;
	}

	kern_logf("i915: session %u quarantined; objects retained for reset\n", session->identifier);

	/* Succeeded: other sessions continue on both engine records. */
	return 0;
}

/* Frees every quarantined object and address space after a reset; the caller holds the device mutex. */
static void
i915_quarantine_release(
	struct i915_device *device)
{
	struct i915_gem_object *object;
	struct i915_gem_object *next;
	struct i915_ppgtt *vm;

	/*
	 * Unmaps and frees each quarantined object.  Session objects are never
	 * bound into the GGTT, so only the session binding is undone.
	 */
	object = device->gem.objects;
	while (object != NULL) {
		next = object->next;

		/* Only a quarantined object is released here. */
		if (object->quarantined != 0U) {
			/* Undoes the session binding the object may still have. */
			if (object->vm != NULL)
				drv_i915_gem_unbind_vm(object);

			/* The object is no longer retained, so destroy frees it. */
			object->quarantined = 0U;
			drv_i915_gem_destroy(&device->gem, object);
		}

		object = next;
	}

	device->gem.quarantined_objects = 0U;

	/* Frees the quarantined address spaces after their objects. */
	while (device->quarantined_vms != NULL) {
		vm = device->quarantined_vms;
		device->quarantined_vms = vm->next;
		drv_i915_ppgtt_destroy(vm);
		kern_free(vm);
	}
}
