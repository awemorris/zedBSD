/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT reset (see reset.h).
 *
 * GDRST resets the graphics domains whose bits are written to it; the
 * hardware clears each bit when its domain is through.  A reset of the
 * whole GT pulses the full-reset bit.  A reset of one engine pulses the
 * engine's own bit, after a handshake on the engine's RESET_CTL that lets
 * the engine reach a point where it may be reset, and with the scaler and
 * format converter it uses held for the engine and reset with it.
 *
 * Every pulse writes the bits and waits for them to clear twice on the GTs
 * the driver serves, and the engines are given 50 us to settle after it.
 * A pulse that does not clear is the only failure worth trying again, and
 * only for the whole GT.  The whole-GT reset runs before the engines exist,
 * so no engine needs a handshake then.
 */

#include "reset.h"
#include "device-info.h"
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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intel/gt-regs.h"

/*
 * The graphics domain reset register, its full-reset bit, and the bit of
 * the first scaler and format converter (the others follow it).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN6_GDRST, GEN11_GRDOM_FULL, GEN11_GRDOM_SFC0).
 */
#define I915_RESET_GDRST		0x941cU
#define I915_RESET_FULL			0x1U
#define I915_RESET_CONVERTER0		(1U << 17)

/*
 * The limits: the whole-GT attempts, the write and wait passes of one
 * pulse, the settle time after a pulse, the wait for an engine's handshake
 * and for a converter's lock.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_reset.c
 * (RESET_MAX_RETRIES, gen6_hw_domain_reset, gen8_engine_reset_prepare,
 * gen11_lock_sfc).
 */
#define I915_RESET_ATTEMPTS		3U
#define I915_RESET_PASSES		2U
#define I915_RESET_SETTLE_US		50U
#define I915_RESET_HANDSHAKE_US		700U
#define I915_RESET_CONVERTER_US		1000U

/* A masked register word that sets, or clears, the named bits. */
#define I915_RESET_MASKED_SET(bits)	((((uint32_t)(bits)) << 16) | ((uint32_t)(bits)))
#define I915_RESET_MASKED_CLEAR(bits)	(((uint32_t)(bits)) << 16)

/*
 * Where an engine class reaches its scaler and format converter: the
 * forced lock, its acknowledge and the in-use bit (offsets from the
 * engine's base), and which converter it is (the engine's instance shifted
 * right by converter_shift).  The decode engines share one converter
 * between two instances; each enhancement engine has its own.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN11_VCS_SFC_FORCED_LOCK, GEN11_VCS_SFC_FORCED_LOCK_BIT,
 * GEN11_VCS_SFC_LOCK_STATUS, GEN11_VCS_SFC_LOCK_ACK_BIT,
 * GEN11_VCS_SFC_USAGE_BIT, GEN11_VECS_SFC_FORCED_LOCK,
 * GEN11_VECS_SFC_FORCED_LOCK_BIT, GEN11_VECS_SFC_LOCK_ACK,
 * GEN11_VECS_SFC_LOCK_ACK_BIT, GEN11_VECS_SFC_USAGE,
 * GEN11_VECS_SFC_USAGE_BIT) and drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_VCS_SFC_RESET_BIT, GEN11_VECS_SFC_RESET_BIT).
 */
struct i915_reset_converter {
	uint32_t lock_off;
	uint32_t lock_bit;
	uint32_t ack_off;
	uint32_t ack_bit;
	uint32_t use_off;
	uint32_t use_bit;
	unsigned converter_shift;
};

static const struct i915_reset_converter i915_reset_converter_vcs = {
	0x88cU, 1U << 0, 0x890U, 1U << 1, 0x890U, 1U << 0, 1U
};
static const struct i915_reset_converter i915_reset_converter_vecs = {
	0x201cU, 1U << 0, 0x2018U, 1U << 0, 0x2014U, 1U << 0, 0U
};

/*
 * The decode engine's HEVC pipe: whether it, rather than the engine's own
 * pipe, uses the converter.
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/i915/gt/intel_engine_regs.h
 * (GEN12_HCP_SFC_LOCK_STATUS, GEN12_HCP_SFC_USAGE_BIT).
 */
#define I915_RESET_HEVC_USE(base)	((base) + 0x2914U)
#define I915_RESET_HEVC_USE_BIT		(1U << 0)

/* A converter held for a reset: the engine it was locked through and its layout. */
struct i915_reset_hold {
	const struct i915_engine_info *through;
	const struct i915_reset_converter *layout;
};

static int i915_reset_pulse(struct i915_mmio *mmio, uint32_t domains, unsigned fast_us, unsigned *passes);
static int i915_reset_gt_once(struct spinlock *uncore_lock, struct i915_mmio *mmio, unsigned fast_us, unsigned attempt);
static int i915_reset_engine_locked(struct i915_mmio *mmio, const struct i915_gt_info *gt, const struct i915_engine_info *engine, unsigned fast_us, uint32_t *domains, unsigned *passes);
static int i915_reset_handshake_enter(struct i915_mmio *mmio, const struct i915_engine_info *engine);
static void i915_reset_handshake_leave(struct i915_mmio *mmio, const struct i915_engine_info *engine);
static const struct i915_reset_converter *i915_reset_converter_of(const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static const struct i915_engine_info *i915_reset_converter_partner(const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static int i915_reset_converter_hold(struct i915_mmio *mmio, const struct i915_gt_info *gt, const struct i915_engine_info *engine, uint32_t *domains, struct i915_reset_hold *hold);
static void i915_reset_converter_release(struct i915_mmio *mmio, const struct i915_reset_hold *hold);

/*
 * Resets every graphics domain.
 *
 * The render and GT forcewake domains are held across the reset.  Each
 * attempt runs under the uncore lock with interrupts off, and a new one is
 * made, up to three in all, only while the pulse does not clear.  Returns
 * 0, a forcewake error, ETIMEDOUT, or the time-base fault of the settle
 * time.
 */
int
drv_i915_gt_reset_all(
	struct spinlock *uncore_lock,
	struct i915_mmio *mmio,
	unsigned fast_us)
{
	unsigned attempt;
	int error;

	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_RENDER);
	if (error != 0)
		return error;
	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_GT);
	if (error != 0) {
		(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
		return error;
	}

	error = ETIMEDOUT;
	for (attempt = 0U; attempt < I915_RESET_ATTEMPTS && error == ETIMEDOUT; attempt++)
		error = i915_reset_gt_once(uncore_lock, mmio, fast_us, attempt);

	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
	return error;
}

/*
 * Resets one engine, with the converter it uses.
 *
 * The GT forcewake domain and the engine's own are held across the reset,
 * and the uncore lock with interrupts off.  One attempt is made: an engine
 * that never reaches the point where it may be reset is not reset.
 * Returns 0, EINVAL for an engine without a reset domain, a forcewake
 * error, ETIMEDOUT when the handshake, the converter's lock or the pulse
 * did not complete, or the time-base fault of the settle time.
 */
int
drv_i915_gt_reset_engine(
	struct spinlock *uncore_lock,
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine,
	unsigned fast_us)
{
	unsigned long irq;
	unsigned passes;
	uint32_t domains;
	int engine_domain;
	int error;

	if (engine == NULL || engine->reset_domain == 0U)
		return EINVAL;

	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_GT);
	if (error != 0)
		return error;
	engine_domain = drv_i915_mmio_domain_of(mmio, engine->mmio_base);
	if (engine_domain >= 0) {
		error = drv_i915_forcewake_get(mmio, engine_domain);
		if (error != 0) {
			(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
			return error;
		}
	}

	irq = spin_lock_irqsave(uncore_lock);
	error = i915_reset_engine_locked(mmio, gt, engine, fast_us, &domains, &passes);
	spin_unlock_irqrestore(uncore_lock, irq);

	kern_logf("i915: engine_reset %s domains=0x%x passes=%u rc=%d\n", engine->name, domains, passes, error);

	if (engine_domain >= 0)
		(void)drv_i915_forcewake_put(mmio, engine_domain);
	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
	return error;
}

/*
 * Writes domains to GDRST and waits for the hardware to clear them, pass
 * after pass, stopping at the first pass that does not clear.  The caller
 * holds the uncore lock.  Returns 0 or ETIMEDOUT, with the passes made.
 */
static int
i915_reset_pulse(
	struct i915_mmio *mmio,
	uint32_t domains,
	unsigned fast_us,
	unsigned *passes)
{
	int error;

	error = 0;
	for (*passes = 0U; *passes < I915_RESET_PASSES && error == 0; (*passes)++) {
		drv_i915_raw_write32(mmio, I915_RESET_GDRST, domains);
		error = drv_i915_wait_reg(mmio, I915_RESET_GDRST, domains, 0U, fast_us, 0U, NULL);
	}
	return error;
}

/*
 * One whole-GT attempt under the uncore lock: the pulse and the settle
 * time.  A settle time whose time base faulted cannot bound another
 * attempt either, so its fault is returned as it is and not retried.
 */
static int
i915_reset_gt_once(
	struct spinlock *uncore_lock,
	struct i915_mmio *mmio,
	unsigned fast_us,
	unsigned attempt)
{
	unsigned long irq;
	unsigned passes;
	int pulse_error;
	int settle_error;

	irq = spin_lock_irqsave(uncore_lock);
	pulse_error = i915_reset_pulse(mmio, I915_RESET_FULL, fast_us, &passes);
	settle_error = drv_i915_udelay(I915_RESET_SETTLE_US);
	spin_unlock_irqrestore(uncore_lock, irq);

	if (settle_error != 0) {
		kern_logf("i915: gt_reset attempt=%u passes=%u time-base fault rc=%d\n", attempt, passes, settle_error);
		return settle_error;
	}
	kern_logf("i915: gt_reset attempt=%u passes=%u rc=%d\n", attempt, passes, pulse_error);
	return pulse_error;
}

/*
 * The reset of one engine with the uncore lock held: the handshake, the
 * converter's hold, the pulse and the settle time, then the converter and
 * the handshake given back whatever came of the pulse.  Returns 0,
 * ETIMEDOUT, or the time-base fault of the settle time, with the domains
 * pulsed and the passes made.
 */
static int
i915_reset_engine_locked(
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine,
	unsigned fast_us,
	uint32_t *domains,
	unsigned *passes)
{
	struct i915_reset_hold hold;
	int settle_error;
	int error;

	*domains = 0U;
	*passes = 0U;

	error = i915_reset_handshake_enter(mmio, engine);
	if (error != 0) {
		i915_reset_handshake_leave(mmio, engine);
		return error;
	}

	*domains = engine->reset_domain;
	hold.through = NULL;
	hold.layout = NULL;
	error = i915_reset_converter_hold(mmio, gt, engine, domains, &hold);

	settle_error = 0;
	if (error == 0) {
		error = i915_reset_pulse(mmio, *domains, fast_us, passes);
		settle_error = drv_i915_udelay(I915_RESET_SETTLE_US);
	}

	i915_reset_converter_release(mmio, &hold);
	i915_reset_handshake_leave(mmio, engine);

	if (error != 0)
		return error;
	return settle_error;
}

/*
 * Brings an engine to the point where it may be reset.
 *
 * RESET_CTL says where the engine stands: an engine already ready needs
 * nothing, an engine not yet ready is asked and waited for, and an engine
 * in a catastrophic error skips the handshake: the error bit is written
 * and the hardware clears it.  The caller holds the uncore lock.  Returns
 * 0 or ETIMEDOUT.
 */
static int
i915_reset_handshake_enter(
	struct i915_mmio *mmio,
	const struct i915_engine_info *engine)
{
	uint32_t reg;
	uint32_t state;
	uint32_t ask;
	uint32_t watch;
	uint32_t want;
	int error;

	reg = I915_GT_RING_RESET_CTL(engine->mmio_base);
	state = drv_i915_raw_read32(mmio, reg);
	if ((state & I915_GT_RESET_CTL_CAT_ERROR) != 0U) {
		ask = I915_GT_RESET_CTL_CAT_ERROR;
		watch = I915_GT_RESET_CTL_CAT_ERROR;
		want = 0U;
	} else if ((state & I915_GT_RESET_CTL_READY_TO_RESET) == 0U) {
		ask = I915_GT_RESET_CTL_REQUEST_RESET;
		watch = I915_GT_RESET_CTL_READY_TO_RESET;
		want = I915_GT_RESET_CTL_READY_TO_RESET;
	} else {
		return 0;
	}

	drv_i915_raw_write32(mmio, reg, I915_RESET_MASKED_SET(ask));
	error = drv_i915_wait_reg(mmio, reg, watch, want, I915_RESET_HANDSHAKE_US, 0U, NULL);
	if (error != 0) {
		kern_logf("i915: %s reset request timed out: request %08x RESET_CTL %08x\n", engine->name, ask, drv_i915_raw_read32(mmio, reg));
		return error;
	}
	return 0;
}

/* Withdraws the engine's reset request; the caller holds the uncore lock. */
static void
i915_reset_handshake_leave(
	struct i915_mmio *mmio,
	const struct i915_engine_info *engine)
{
	drv_i915_raw_write32(mmio, I915_GT_RING_RESET_CTL(engine->mmio_base), I915_RESET_MASKED_CLEAR(I915_GT_RESET_CTL_REQUEST_RESET));
}

/*
 * The converter layout of an engine that can have one: an enhancement
 * engine, or a decode engine whose fuses give it converter access.  NULL
 * for every other engine and for Gen9, which resets no converter.
 */
static const struct i915_reset_converter *
i915_reset_converter_of(
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	if (gt->graphics_ver == 9)
		return NULL;
	if (engine->class == I915_VIDEO_ENHANCEMENT_CLASS)
		return &i915_reset_converter_vecs;
	if (engine->class == I915_VIDEO_DECODE_CLASS && (gt->vdbox_sfc_access & (1U << (unsigned)engine->instance)) != 0U)
		return &i915_reset_converter_vcs;
	return NULL;
}

/* The enhancement engine that shares a decode engine's converter, or NULL when it is fused off. */
static const struct i915_engine_info *
i915_reset_converter_partner(
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	unsigned i;

	for (i = 0U; i < gt->num_engines; i++) {
		if (gt->engines[i].class == I915_VIDEO_ENHANCEMENT_CLASS && gt->engines[i].instance == engine->instance / 2)
			return &gt->engines[i];
	}
	return NULL;
}

/*
 * Holds the converter an engine uses for its reset.
 *
 * The lock goes through the engine itself when its own pipe uses the
 * converter, or, on graphics version 12, for a decode engine whose HEVC
 * pipe uses it, through the enhancement engine that shares it
 * (Wa_14010733141).  A converter nobody
 * uses is not held.  Once the lock was asked for, *hold names it for the
 * release, whatever else comes of it.  The converter joins *domains when,
 * after the lock, the engine that uses it is the one it was locked
 * through; a converter that moved between the two is left alone and the
 * engine is reset by itself.  Returns 0 or ETIMEDOUT.
 */
static int
i915_reset_converter_hold(
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine,
	uint32_t *domains,
	struct i915_reset_hold *hold)
{
	const struct i915_reset_converter *layout;
	const struct i915_engine_info *through;
	bool via_partner;
	bool in_use;
	uint32_t base;
	int error;

	layout = i915_reset_converter_of(gt, engine);
	if (layout == NULL)
		return 0;

	/* Whose pipe uses the converter decides whom the lock goes through. */
	through = engine;
	via_partner = false;
	if ((drv_i915_raw_read32(mmio, engine->mmio_base + layout->use_off) & layout->use_bit) == 0U) {
		if (engine->class != I915_VIDEO_DECODE_CLASS)
			return 0;
		if (gt->graphics_ver != 12)
			return 0;
		if ((drv_i915_raw_read32(mmio, I915_RESET_HEVC_USE(engine->mmio_base)) & I915_RESET_HEVC_USE_BIT) == 0U)
			return 0;
		through = i915_reset_converter_partner(gt, engine);
		if (through == NULL)
			return 0;
		layout = &i915_reset_converter_vecs;
		via_partner = true;
	}

	base = through->mmio_base;
	drv_i915_raw_write32(mmio, base + layout->lock_off, drv_i915_raw_read32(mmio, base + layout->lock_off) | layout->lock_bit);
	hold->through = through;
	hold->layout = layout;

	error = drv_i915_wait_reg(mmio, base + layout->ack_off, layout->ack_bit, layout->ack_bit, I915_RESET_CONVERTER_US, 0U, NULL);
	in_use = (drv_i915_raw_read32(mmio, base + layout->use_off) & layout->use_bit) != 0U;
	if (in_use == via_partner)
		return 0;

	if (error != 0) {
		kern_logf("i915: %s: converter forced lock not acknowledged\n", engine->name);
		return error;
	}
	*domains |= I915_RESET_CONVERTER0 << ((unsigned)through->instance >> layout->converter_shift);
	return 0;
}

/* Gives back a held converter's lock, leaving the rest of the register; the caller holds the uncore lock. */
static void
i915_reset_converter_release(
	struct i915_mmio *mmio,
	const struct i915_reset_hold *hold)
{
	uint32_t reg;

	if (hold->through == NULL)
		return;
	reg = hold->through->mmio_base + hold->layout->lock_off;
	drv_i915_raw_write32(mmio, reg, drv_i915_raw_read32(mmio, reg) & ~hold->layout->lock_bit);
}

/*
 * ------------------------------------------------------------------------
 * Recovery of the GPU node.
 *
 * The GPU core's recovery operations (drv_gpu_recovery_ops).  Stop begin
 * and poll track a session's pending requests; the fault fails every
 * request and refuses new work; the checked reset frees what quarantine
 * retained once the hardware has been reset; isolate quarantines one
 * session.  Of the engine and GT resets they need, only the video engine's
 * is connected to a published node (worker.h): the others log
 * and fail, and the operation fails with them.
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

/* Frees every quarantined object and address space, and the retained video records, after a reset; the caller holds the device mutex. */
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

	/* Frees the video engine's records a hang retained, which the engine reset has recovered. */
	drv_i915_worker_video_reclaim(device);
}
