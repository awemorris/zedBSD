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
 *
 * The reset of one engine follows __intel_gt_reset(engine->mask) through
 * gen8_reset_engines() and __gen11_reset_engines() with one attempt, as the
 * reference makes for an engine mask: the engine is asked to be ready
 * (gen8_engine_reset_prepare()), its scaler and format converter is locked
 * when it uses one (gen11_lock_sfc()), its GDRST domain is reset, and the
 * lock and the ready request are given back (ws083-p007).
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
#include <stddef.h>
#include <stdint.h>

#include "intel/gt-regs.h"

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

/* How long an engine is given to say it is ready for its reset (gen8_engine_reset_prepare()). */
#define I915_RESET_READY_US		700U

/* How long a converter lock is given to be acknowledged (gen11_lock_sfc()). */
#define I915_SFC_LOCK_ACK_US		1000U

/* A masked register word that sets, or clears, the named bits. */
#define I915_RESET_MASKED_ENABLE(bits)	((((uint32_t)(bits)) << 16) | ((uint32_t)(bits)))
#define I915_RESET_MASKED_DISABLE(bits)	(((uint32_t)(bits)) << 16)

/*
 * The converter registers of a video decode engine (GEN11_VCS_SFC_*): the
 * forced lock a reset asks for, and the status that says whether the MFX
 * pipe uses the converter and whether the lock was acknowledged.
 */
#define I915_VCS_SFC_FORCED_LOCK(base)	((base) + 0x88cU)
#define I915_VCS_SFC_FORCED_LOCK_BIT	(1U << 0)
#define I915_VCS_SFC_LOCK_STATUS(base)	((base) + 0x890U)
#define I915_VCS_SFC_USAGE_BIT		(1U << 0)
#define I915_VCS_SFC_LOCK_ACK_BIT	(1U << 1)

/* Whether the HEVC pipe of a video decode engine uses the converter (GEN12_HCP_SFC_LOCK_STATUS). */
#define I915_HCP_SFC_LOCK_STATUS(base)	((base) + 0x2914U)
#define I915_HCP_SFC_USAGE_BIT		(1U << 0)

/* The converter registers of a video enhancement engine (GEN11_VECS_SFC_*). */
#define I915_VECS_SFC_FORCED_LOCK(base)	((base) + 0x201cU)
#define I915_VECS_SFC_FORCED_LOCK_BIT	(1U << 0)
#define I915_VECS_SFC_LOCK_ACK(base)	((base) + 0x2018U)
#define I915_VECS_SFC_LOCK_ACK_BIT	(1U << 0)
#define I915_VECS_SFC_USAGE(base)	((base) + 0x2014U)
#define I915_VECS_SFC_USAGE_BIT		(1U << 0)

/*
 * The GDRST bit of the first converter (GEN11_GRDOM_SFC0).  A video decode
 * engine's converter is the one of its instance / 2, a video enhancement
 * engine's the one of its instance.
 */
#define I915_GRDOM_SFC0			(1U << 17)

/*
 * How the converter of one engine is locked for a reset (struct
 * sfc_lock_data): the forced lock, its acknowledge, the bit that says the
 * engine uses the converter, and the converter's own GDRST bit.
 *
 * It lives on the stack of one engine reset.
 */
struct i915_sfc_lock {
	/* The register and bit that ask for the forced lock. */
	uint32_t lock_reg;
	uint32_t lock_bit;

	/* The register and bit that acknowledge it. */
	uint32_t ack_reg;
	uint32_t ack_bit;

	/* The register and bit that say the engine uses the converter. */
	uint32_t usage_reg;
	uint32_t usage_bit;

	/* The converter's GDRST bit. */
	uint32_t reset_bit;
};

static int i915_gt_reset_attempt(struct spinlock *uncore_lock, struct i915_mmio *mmio, unsigned fast_us, unsigned attempt);
static int i915_gt_domain_reset(struct i915_mmio *mmio, uint32_t domains, unsigned fast_us, unsigned *passes);
static int i915_engine_reset_locked(struct i915_mmio *mmio, const struct i915_gt_info *gt, const struct i915_engine_info *engine, unsigned fast_us, uint32_t *domains, unsigned *passes);
static int i915_engine_reset_ready(struct i915_mmio *mmio, const struct i915_engine_info *engine);
static void i915_engine_reset_unready(struct i915_mmio *mmio, const struct i915_engine_info *engine);
static void i915_sfc_lock_data(const struct i915_engine_info *engine, struct i915_sfc_lock *lock);
static const struct i915_engine_info *i915_sfc_paired_vecs(const struct i915_gt_info *gt, const struct i915_engine_info *engine);
static int i915_sfc_lock(struct i915_mmio *mmio, const struct i915_gt_info *gt, const struct i915_engine_info *engine, uint32_t *domains, const struct i915_engine_info **locked);
static void i915_sfc_unlock(struct i915_mmio *mmio, const struct i915_engine_info *engine);

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
 * Resets one engine, and its scaler and format converter when it uses one.
 *
 * The GT forcewake domain and the engine's own are held across the reset,
 * and the uncore lock with interrupts off.  One attempt is made, as the
 * reference makes for an engine mask: an engine that never says it is ready
 * is not reset.  Each poll is a real-time busy wait.  Returns 0, EINVAL for
 * an engine without a reset domain, a forcewake error, ETIMEDOUT when the
 * engine was not ready or the converter lock or the reset was not
 * acknowledged, or the time-base fault of the settle delay.
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
	int forcewake_error;
	int error;

	/* An engine without a reset domain cannot be reset alone. */
	if (engine == NULL || engine->reset_domain == 0U)
		return EINVAL;

	/* Wakes the GT domain for the whole reset, outside the uncore lock. */
	forcewake_error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_GT);
	if (forcewake_error != 0)
		return forcewake_error;

	/* Wakes the engine's own domain as well, giving the GT hold back if it cannot. */
	engine_domain = drv_i915_mmio_domain_of(mmio, engine->mmio_base);
	if (engine_domain >= 0) {
		forcewake_error = drv_i915_forcewake_get(mmio, engine_domain);
		if (forcewake_error != 0) {
			(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
			return forcewake_error;
		}
	}

	/* Resets the engine under the uncore lock. */
	irq = spin_lock_irqsave(uncore_lock);

	error = i915_engine_reset_locked(mmio, gt, engine, fast_us, &domains, &passes);

	spin_unlock_irqrestore(uncore_lock, irq);

	/* Records how the reset ended. */
	kern_logf("i915: engine_reset %s domains=0x%x passes=%u rc=%d\n",
		  engine->name,
		  domains,
		  passes,
		  error);

	/* Lets the engine's domain and the GT domain sleep again. */
	if (engine_domain >= 0)
		(void)drv_i915_forcewake_put(mmio, engine_domain);
	(void)drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);

	/* Reports why the engine was not reset. */
	if (error != 0)
		return error;

	/* Succeeded: the engine's domains acknowledged their reset. */
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

	/* Requests the full reset under the uncore lock (gen8_reset_engines()). */
	irq = spin_lock_irqsave(uncore_lock);

	acknowledge_error = i915_gt_domain_reset(mmio, I915_GRDOM_FULL, fast_us, &passes);

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
 * Requests a reset of GDRST domains and waits for the hardware to clear the
 * request bits, twice on Alder Lake-P, stopping at the first pass that times
 * out (gen6_hw_domain_reset() before its settle delay).  The caller holds
 * the uncore lock.  Returns 0 or ETIMEDOUT, with the passes made.
 */
static int
i915_gt_domain_reset(
	struct i915_mmio *mmio,
	uint32_t domains,
	unsigned fast_us,
	unsigned *passes)
{
	int acknowledge_error;

	/* Writes the request and polls for its acknowledge, pass after pass. */
	*passes = 0U;
	for (;;) {
		drv_i915_raw_write32(mmio, I915_GDRST, domains);
		acknowledge_error = drv_i915_wait_reg(mmio, I915_GDRST, domains, 0U, fast_us, 0U, NULL);
		(*passes)++;

		/* A pass that timed out ends the reset. */
		if (acknowledge_error != 0)
			break;

		/* The reset is complete after its last pass. */
		if (*passes >= I915_RESET_PASSES)
			break;
	}

	/* Reports a pass whose acknowledge timed out. */
	if (acknowledge_error != 0)
		return acknowledge_error;

	/* Succeeded: every pass was acknowledged. */
	return 0;
}

/*
 * Runs the reset of one engine; the caller holds the uncore lock
 * (gen8_reset_engines() with one attempt).
 *
 * Returns 0, ETIMEDOUT, or the time-base fault of the settle delay, with
 * the GDRST domains asked for and the passes made.
 */
static int
i915_engine_reset_locked(
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine,
	unsigned fast_us,
	uint32_t *domains,
	unsigned *passes)
{
	const struct i915_engine_info *locked;
	int error;
	int delay_error;

	/* Nothing is asked of GDRST yet. */
	*domains = 0U;
	*passes = 0U;

	/* Asks the engine to be ready; an engine that never is, is not reset. */
	error = i915_engine_reset_ready(mmio, engine);
	if (error != 0) {
		i915_engine_reset_unready(mmio, engine);
		return error;
	}

	/* Locks the converter the engine uses, which takes the converter into the reset. */
	*domains = engine->reset_domain;
	locked = NULL;
	error = i915_sfc_lock(mmio, gt, engine, domains, &locked);

	/* Resets the domains, then lets the engine state settle (gen6_hw_domain_reset()). */
	delay_error = 0;
	if (error == 0) {
		error = i915_gt_domain_reset(mmio, *domains, fast_us, passes);
		delay_error = drv_i915_udelay(I915_RESET_SETTLE_US);
	}

	/* Gives back the converter lock that was asked for, whatever came of it. */
	if (locked != NULL)
		i915_sfc_unlock(mmio, locked);

	/* Withdraws the ready request, as the reference does after every reset. */
	i915_engine_reset_unready(mmio, engine);

	/* Reports a lock or a reset that was not acknowledged. */
	if (error != 0)
		return error;

	/* Reports a time base that faulted in the settle delay. */
	if (delay_error != 0)
		return delay_error;

	/* Succeeded: the engine's domains acknowledged their reset. */
	return 0;
}

/*
 * Asks an engine to be ready for its reset (gen8_engine_reset_prepare()).
 *
 * An engine already ready needs nothing.  A catastrophic error bypasses the
 * ready handshake (HAS#396813) and is cleared by the hardware.  The caller
 * holds the uncore lock.  Returns 0 or ETIMEDOUT.
 */
static int
i915_engine_reset_ready(
	struct i915_mmio *mmio,
	const struct i915_engine_info *engine)
{
	uint32_t reg;
	uint32_t status;
	uint32_t request;
	uint32_t mask;
	uint32_t ack;
	int error;

	/* Reads where the engine's reset handshake stands. */
	reg = RING_RESET_CTL(engine->mmio_base);
	status = drv_i915_raw_read32(mmio, reg);

	/* Chooses what to ask the engine for. */
	if ((status & RESET_CTL_CAT_ERROR) != 0U) {
		/* A catastrophic error is cleared rather than waited out. */
		request = RESET_CTL_CAT_ERROR;
		mask = RESET_CTL_CAT_ERROR;
		ack = 0U;
	} else if ((status & RESET_CTL_READY_TO_RESET) == 0U) {
		/* An engine not yet ready is asked to become ready. */
		request = RESET_CTL_REQUEST_RESET;
		mask = RESET_CTL_READY_TO_RESET;
		ack = RESET_CTL_READY_TO_RESET;
	} else {
		/* An engine already ready needs nothing. */
		return 0;
	}

	/* Asks, and waits for the engine's answer. */
	drv_i915_raw_write32(mmio, reg, I915_RESET_MASKED_ENABLE(request));
	error = drv_i915_wait_reg(mmio, reg, mask, ack, I915_RESET_READY_US, 0U, NULL);
	if (error != 0) {
		status = drv_i915_raw_read32(mmio, reg);
		kern_logf("i915: %s reset request timed out: request %08x RESET_CTL %08x\n",
			  engine->name,
			  request,
			  status);
		return error;
	}

	/* Succeeded: the engine is ready for its reset. */
	return 0;
}

/* Withdraws an engine's ready request (gen8_engine_reset_cancel()); the caller holds the uncore lock. */
static void
i915_engine_reset_unready(
	struct i915_mmio *mmio,
	const struct i915_engine_info *engine)
{
	/* Clears the request bit through the register's mask half. */
	drv_i915_raw_write32(mmio, RING_RESET_CTL(engine->mmio_base), I915_RESET_MASKED_DISABLE(RESET_CTL_REQUEST_RESET));
}

/* Names the converter lock registers of a video decode or video enhancement engine (get_sfc_forced_lock_data()). */
static void
i915_sfc_lock_data(
	const struct i915_engine_info *engine,
	struct i915_sfc_lock *lock)
{
	uint32_t base;

	/* The registers are relative to the engine's base. */
	base = engine->mmio_base;

	/* Chooses the layout of the engine's class. */
	if (engine->class == I915_VIDEO_ENHANCEMENT_CLASS) {
		/* A video enhancement engine owns the converter of its instance. */
		lock->lock_reg = I915_VECS_SFC_FORCED_LOCK(base);
		lock->lock_bit = I915_VECS_SFC_FORCED_LOCK_BIT;
		lock->ack_reg = I915_VECS_SFC_LOCK_ACK(base);
		lock->ack_bit = I915_VECS_SFC_LOCK_ACK_BIT;
		lock->usage_reg = I915_VECS_SFC_USAGE(base);
		lock->usage_bit = I915_VECS_SFC_USAGE_BIT;
		lock->reset_bit = I915_GRDOM_SFC0 << (unsigned)engine->instance;
	} else {
		/* A video decode engine shares the converter of its instance / 2. */
		lock->lock_reg = I915_VCS_SFC_FORCED_LOCK(base);
		lock->lock_bit = I915_VCS_SFC_FORCED_LOCK_BIT;
		lock->ack_reg = I915_VCS_SFC_LOCK_STATUS(base);
		lock->ack_bit = I915_VCS_SFC_LOCK_ACK_BIT;
		lock->usage_reg = I915_VCS_SFC_LOCK_STATUS(base);
		lock->usage_bit = I915_VCS_SFC_USAGE_BIT;
		lock->reset_bit = I915_GRDOM_SFC0 << ((unsigned)engine->instance >> 1);
	}
}

/* Finds the video enhancement engine that shares a video decode engine's converter (find_sfc_paired_vecs_engine()), or NULL. */
static const struct i915_engine_info *
i915_sfc_paired_vecs(
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine)
{
	unsigned index;

	/* Looks for the enhancement instance that is the decode instance / 2. */
	for (index = 0U; index < gt->num_engines; index++) {
		if (gt->engines[index].class != I915_VIDEO_ENHANCEMENT_CLASS)
			continue;
		if (gt->engines[index].instance != engine->instance / 2)
			continue;

		/* Succeeded: the partner shares the converter. */
		return &gt->engines[index];
	}

	/* The partner was fused off. */
	return NULL;
}

/*
 * Locks the converter an engine uses before its reset (gen11_lock_sfc()).
 *
 * The lock is asked of the engine itself, or, when only the HEVC pipe of a
 * video decode engine uses the converter, of the video enhancement engine
 * that shares it (Wa_14010733141; the driver serves graphics 12 only).  The
 * engine the lock was asked of is left in *locked, for the caller to unlock
 * whatever this reports.  When the converter ended up locked to the engine
 * that uses it, its GDRST bit joins *domains.  Returns 0 or ETIMEDOUT.
 */
static int
i915_sfc_lock(
	struct i915_mmio *mmio,
	const struct i915_gt_info *gt,
	const struct i915_engine_info *engine,
	uint32_t *domains,
	const struct i915_engine_info **locked)
{
	const struct i915_engine_info *target;
	struct i915_sfc_lock lock;
	uint32_t usage;
	uint32_t hcp_status;
	uint32_t lock_value;
	int lock_to_other;
	int lock_obtained;
	int error;

	/* Only a decode engine with a converter, or an enhancement engine, has one to lock. */
	if (engine->class == I915_VIDEO_DECODE_CLASS) {
		if ((gt->vdbox_sfc_access & (1U << (unsigned)engine->instance)) == 0U)
			return 0;
	} else if (engine->class != I915_VIDEO_ENHANCEMENT_CLASS) {
		return 0;
	}

	/* Starts from the engine's own converter registers. */
	i915_sfc_lock_data(engine, &lock);
	target = engine;
	lock_to_other = 0;

	/* Reads whether the engine's own pipe uses the converter. */
	usage = drv_i915_raw_read32(mmio, lock.usage_reg);
	if ((usage & lock.usage_bit) == 0U) {
		/* Only a decode engine looks further, at its HEVC pipe. */
		if (engine->class != I915_VIDEO_DECODE_CLASS)
			return 0;

		/* A converter neither pipe uses needs no lock. */
		hcp_status = drv_i915_raw_read32(mmio, I915_HCP_SFC_LOCK_STATUS(engine->mmio_base));
		if ((hcp_status & I915_HCP_SFC_USAGE_BIT) == 0U)
			return 0;

		/* The HEVC pipe uses it: the lock is asked of the enhancement engine that shares it. */
		target = i915_sfc_paired_vecs(gt, engine);
		if (target == NULL)
			return 0;

		/* The partner's converter registers stand in for the engine's. */
		i915_sfc_lock_data(target, &lock);
		lock_to_other = 1;
	}

	/* Asks for the forced lock; from here the caller gives it back. */
	lock_value = drv_i915_raw_read32(mmio, lock.lock_reg);
	drv_i915_raw_write32(mmio, lock.lock_reg, lock_value | lock.lock_bit);
	*locked = target;

	/* Waits for the lock's acknowledge. */
	error = drv_i915_wait_reg(mmio, lock.ack_reg, lock.ack_bit, lock.ack_bit, I915_SFC_LOCK_ACK_US, 0U, NULL);

	/* Reads whether the converter is now held by the engine that uses it. */
	usage = drv_i915_raw_read32(mmio, lock.usage_reg);
	lock_obtained = 0;
	if ((usage & lock.usage_bit) != 0U)
		lock_obtained = 1;

	/*
	 * The converter is reset with the engine only when it was locked to
	 * this engine and stayed in use, or was locked to the partner but went
	 * free meanwhile; otherwise the engine is reset by itself.
	 */
	if (lock_obtained == lock_to_other)
		return 0;

	/* A lock that was never acknowledged fails the reset. */
	if (error != 0) {
		kern_logf("i915: %s: converter forced lock not acknowledged\n", engine->name);
		return error;
	}

	/* Succeeded: the converter's bit joins the reset. */
	*domains |= lock.reset_bit;
	return 0;
}

/* Gives back a converter's forced lock (gen11_unlock_sfc()); the caller holds the uncore lock. */
static void
i915_sfc_unlock(
	struct i915_mmio *mmio,
	const struct i915_engine_info *engine)
{
	struct i915_sfc_lock lock;
	uint32_t lock_value;

	/* Clears the lock bit and leaves the rest of the register. */
	i915_sfc_lock_data(engine, &lock);
	lock_value = drv_i915_raw_read32(mmio, lock.lock_reg);
	drv_i915_raw_write32(mmio, lock.lock_reg, lock_value & ~lock.lock_bit);
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
 * is connected to a published node (worker.h, ws083-p007): the others log
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

	/* Frees the video engine's records a hang retained, which the engine reset has recovered (ws083-p007). */
	drv_i915_worker_video_reclaim(device);
}
