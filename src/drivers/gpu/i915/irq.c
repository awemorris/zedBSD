/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device interrupts: install, uninstall and the top-level handler (see irq.h).
 *
 * The device has one master control in front of every source.  The GT
 * sources sit behind it in one of two layouts, described by a table:
 *
 *   Gen11 layout  engine class enables and per-engine masks, and two banks
 *                 of pending bits whose sources are named by an identity
 *                 handshake (select the bit, wait for DATA_VALID, write it
 *                 back, then clear the bank)
 *   Gen8 layout   four banks of IMR, IIR and IER whose bits name the
 *                 engines by position; Gen9 uses it
 *
 * The display sources are reached through the display operations table,
 * and GU_MISC (Gen11 layout only) and PCU are plain register sets.
 */

#include "i915.h"
#include "irq.h"
#include "mmio.h"
#include "sync.h"
#include "device-info.h"

#include "intel/gt-regs.h"

#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/waitq.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN11_GU_MISC_IMR, GEN11_GU_MISC_IIR, GEN11_GU_MISC_IER, GEN11_GU_MISC_GSE).
 */
#define I915_IRQ_GU_MISC_IMR		0x444f4U
#define I915_IRQ_GU_MISC_IIR		0x444f8U
#define I915_IRQ_GU_MISC_IER		0x444fcU
#define I915_IRQ_GU_MISC_GSE		(1U << 27)

/*
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/i915_reg.h
 * (GEN8_PCU_IMR, GEN8_PCU_IIR, GEN8_PCU_IER).
 */
#define I915_IRQ_PCU_IMR		0x444e4U
#define I915_IRQ_PCU_IIR		0x444e8U
#define I915_IRQ_PCU_IER		0x444ecU

/*
 * How many identity reads the GT handler makes before it gives up on
 * DATA_VALID.  The handler runs in interrupt context and must not sleep,
 * and each read is a real register access, so the bound is in reads.
 */
#define I915_IRQ_IDENTITY_READS		1000U

/* How many GT interrupt banks the master control reports. */
#define I915_IRQ_GT_BANKS		2U

/*
 * The GT power management source: its bit in GT bank 0 and its instance in
 * the OTHER class.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GTPM, OTHER_GTPM_INSTANCE).
 */
#define I915_IRQ_GTPM_BIT		16U
#define I915_IRQ_GTPM_INSTANCE		1U

/* How many pending GTPM identities a reset clears at most before it gives up. */
#define I915_IRQ_GTPM_RESETS		16U

/*
 * How long drv_i915_synchronize_irq() waits for the handler, and the step
 * it waits in, both in microseconds.
 */
#define I915_IRQ_SYNC_TIMEOUT_US	100000U
#define I915_IRQ_SYNC_STEP_US		10U

/* The I915_GT_GEN8_MASTER_IRQ bits that name GT sources, with the control bit; the rest are display and PCU. */
#define I915_IRQ_GEN8_GT_BITS		(I915_GT_GEN8_MASTER_IRQ_CONTROL | I915_GT_GEN8_GT_RCS_IRQ | I915_GT_GEN8_GT_BCS_IRQ | \
					 I915_GT_GEN8_GT_VCS0_IRQ | I915_GT_GEN8_GT_VCS1_IRQ | I915_GT_GEN8_GT_VECS_IRQ | \
					 I915_GT_GEN8_GT_PM_IRQ | I915_GT_GEN8_GT_GUC_IRQ)

/* How a Gen11 engine interrupt register takes the engine interrupt bits. */
enum i915_irq_engine_reg_kind {
	I915_IRQ_ENABLE_PAIR,	/* a class enable: both engines of the pair */
	I915_IRQ_MASK_PAIR,	/* a mask: both engines of the pair */
	I915_IRQ_MASK_UPPER	/* a mask whose lower half is reserved */
};

/* One engine interrupt register of the Gen11 layout. */
struct i915_irq_engine_reg {
	uint32_t offset;
	enum i915_irq_engine_reg_kind kind;
};

/* One enable and mask pair of a Gen11 source the engines do not own. */
struct i915_irq_other_reg {
	uint32_t enable;
	uint32_t mask;
};

/* One engine of a Gen8 GT bank: its class, instance and bit position. */
struct i915_irq_bank_engine {
	int engine_class;
	int instance;
	unsigned shift;
};

/* One Gen8 GT bank: the master control bits that report it and its engines. */
struct i915_irq_bank {
	uint32_t master_bits;
	unsigned num_engines;
	struct i915_irq_bank_engine engines[2];
	int power;	/* nonzero: the power management bank, served by RPS */
};

/*
 * One GT interrupt layout: the master control, how the GT sources are
 * closed, opened and served, and whether GU_MISC exists.
 */
struct i915_irq_layout {
	uint32_t master;
	uint32_t master_on;
	void (*gt_close)(struct i915_irq_dev *irq);
	void (*gt_open)(struct i915_irq_dev *irq);
	void (*serve)(struct i915_irq_dev *irq, uint32_t master_ctl);
	int has_gu_misc;
};

/*
 * The Gen11 engine interrupt registers, in the order they are written.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_RENDER_COPY_INTR_ENABLE, GEN11_VCS_VECS_INTR_ENABLE,
 * GEN11_RCS0_RSVD_INTR_MASK, GEN11_BCS_RSVD_INTR_MASK,
 * GEN11_VCS0_VCS1_INTR_MASK, GEN11_VCS2_VCS3_INTR_MASK,
 * GEN11_VECS0_VECS1_INTR_MASK).
 */
static const struct i915_irq_engine_reg i915_irq_gen11_engine_regs[] = {
	{ I915_GT_GEN11_RENDER_COPY_INTR_ENABLE, I915_IRQ_ENABLE_PAIR },
	{ I915_GT_GEN11_VCS_VECS_INTR_ENABLE, I915_IRQ_ENABLE_PAIR },
	{ I915_GT_GEN11_RCS0_RSVD_INTR_MASK, I915_IRQ_MASK_UPPER },
	{ I915_GT_GEN11_BCS_RSVD_INTR_MASK, I915_IRQ_MASK_UPPER },
	{ I915_GT_GEN11_VCS0_VCS1_INTR_MASK, I915_IRQ_MASK_PAIR },
	{ I915_GT_GEN11_VCS2_VCS3_INTR_MASK, I915_IRQ_MASK_PAIR },
	{ I915_GT_GEN11_VECS0_VECS1_INTR_MASK, I915_IRQ_MASK_PAIR },
};

/*
 * The Gen11 sources the engines do not own: power management first (kept
 * closed when the GT opens, RPS opens it itself), then GuC and crypto.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN11_GPM_WGBOXPERF_INTR_ENABLE, GEN11_GPM_WGBOXPERF_INTR_MASK,
 * GEN11_GUC_SG_INTR_ENABLE, GEN11_GUC_SG_INTR_MASK,
 * GEN11_CRYPTO_RSVD_INTR_ENABLE, GEN11_CRYPTO_RSVD_INTR_MASK).
 */
static const struct i915_irq_other_reg i915_irq_gen11_other_regs[] = {
	{ I915_GT_GEN11_GPM_WGBOXPERF_INTR_ENABLE, I915_GT_GEN11_GPM_WGBOXPERF_INTR_MASK },
	{ I915_GT_GEN11_GUC_SG_INTR_ENABLE, I915_GT_GEN11_GUC_SG_INTR_MASK },
	{ I915_GT_GEN11_CRYPTO_RSVD_INTR_ENABLE, I915_GT_GEN11_CRYPTO_RSVD_INTR_MASK },
};

/* The index of the power management pair in i915_irq_gen11_other_regs. */
#define I915_IRQ_GEN11_OTHER_PM		0U

/* The number of entries of a table. */
#define I915_IRQ_COUNT(table)		(sizeof(table) / sizeof((table)[0]))

/* How many GT banks the Gen8 layout has. */
#define I915_IRQ_GEN8_BANKS		4U

/*
 * The Gen8 GT banks: 0 render and copy, 1 video decode, 2 power management
 * and GuC, 3 video enhancement.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/i915/gt/intel_gt_regs.h
 * (GEN8_GT_RCS_IRQ, GEN8_GT_BCS_IRQ, GEN8_GT_VCS0_IRQ, GEN8_GT_VCS1_IRQ,
 * GEN8_GT_PM_IRQ, GEN8_GT_GUC_IRQ, GEN8_GT_VECS_IRQ, GEN8_RCS_IRQ_SHIFT,
 * GEN8_BCS_IRQ_SHIFT, GEN8_VCS0_IRQ_SHIFT, GEN8_VCS1_IRQ_SHIFT,
 * GEN8_VECS_IRQ_SHIFT).
 */
static const struct i915_irq_bank i915_irq_gen8_banks[I915_IRQ_GEN8_BANKS] = {
	{
		I915_GT_GEN8_GT_RCS_IRQ | I915_GT_GEN8_GT_BCS_IRQ, 2U,
		{ { I915_RENDER_CLASS, 0, I915_GT_GEN8_RCS_IRQ_SHIFT },
		  { I915_COPY_ENGINE_CLASS, 0, I915_GT_GEN8_BCS_IRQ_SHIFT } },
		0
	},
	{
		I915_GT_GEN8_GT_VCS0_IRQ | I915_GT_GEN8_GT_VCS1_IRQ, 2U,
		{ { I915_VIDEO_DECODE_CLASS, 0, I915_GT_GEN8_VCS0_IRQ_SHIFT },
		  { I915_VIDEO_DECODE_CLASS, 1, I915_GT_GEN8_VCS1_IRQ_SHIFT } },
		0
	},
	{
		I915_GT_GEN8_GT_PM_IRQ | I915_GT_GEN8_GT_GUC_IRQ, 0U,
		{ { 0, 0, 0U }, { 0, 0, 0U } },
		1
	},
	{
		I915_GT_GEN8_GT_VECS_IRQ, 1U,
		{ { I915_VIDEO_ENHANCEMENT_CLASS, 0, I915_GT_GEN8_VECS_IRQ_SHIFT },
		  { 0, 0, 0U } },
		0
	},
};

/* The order the handler serves the Gen8 banks in: the engines, then power management. */
static const unsigned i915_irq_gen8_serve_order[I915_IRQ_GEN8_BANKS] = { 0U, 1U, 3U, 2U };

static void i915_irq_write(struct i915_irq_dev *irq, uint32_t offset, uint32_t value, unsigned *counter);
static const struct i915_irq_layout *i915_irq_layout_of(const struct i915_irq_dev *irq);
static uint32_t i915_irq_master_off(struct i915_irq_dev *irq);
static void i915_irq_master_on(struct i915_irq_dev *irq);
static uint32_t i915_irq_engine_bits(const struct i915_irq_dev *irq);
static void i915_irq_engine_event(struct i915_irq_dev *irq, uint32_t bits);
static void i915_irq_engine_wake(struct i915_irq_dev *irq);
static int i915_irq_engine_exists(const struct i915_irq_dev *irq, unsigned engine_class, unsigned instance);
static void i915_irq_power_event(struct i915_irq_dev *irq, uint32_t bits);
static void i915_irq_gen11_close(struct i915_irq_dev *irq);
static void i915_irq_gen11_open(struct i915_irq_dev *irq);
static void i915_irq_gen11_serve(struct i915_irq_dev *irq, uint32_t master_ctl);
static uint32_t i915_irq_gen11_identity(struct i915_irq_dev *irq, unsigned bank, unsigned bit);
static void i915_irq_gen11_dispatch(struct i915_irq_dev *irq, uint32_t identity);
static void i915_irq_gen11_bank(struct i915_irq_dev *irq, unsigned bank);
static void i915_irq_gen8_close(struct i915_irq_dev *irq);
static void i915_irq_gen8_open(struct i915_irq_dev *irq);
static void i915_irq_gen8_serve(struct i915_irq_dev *irq, uint32_t master_ctl);
static uint32_t i915_irq_gen8_bank_value(const struct i915_irq_bank *bank, uint32_t bits);
static uint32_t i915_irq_gen8_bank_take(struct i915_irq_dev *irq, unsigned bank);
static void i915_irq_gen8_engine(struct i915_irq_dev *irq, const struct i915_irq_bank_engine *engine, uint32_t iir);
static void i915_irq_handler_body(int vector, kern_irq_ack_t acknowledge, void *argument);
static void i915_irq_handler(int vector, kern_irq_ack_t acknowledge, void *argument);

/* The Gen11 layout. */
static const struct i915_irq_layout i915_irq_gen11_layout = {
	I915_GT_GEN11_GFX_MSTR_IRQ, I915_GT_GEN11_MASTER_IRQ,
	i915_irq_gen11_close, i915_irq_gen11_open, i915_irq_gen11_serve, 1
};

/* The Gen8 layout, which Gen9 uses. */
static const struct i915_irq_layout i915_irq_gen8_layout = {
	I915_GT_GEN8_MASTER_IRQ, I915_GT_GEN8_MASTER_IRQ_CONTROL,
	i915_irq_gen8_close, i915_irq_gen8_open, i915_irq_gen8_serve, 0
};

/*
 * Installs the interrupt handler and enables the interrupt sources.
 *
 * Returns 0, or EIO when the handler could not be attached, in which case
 * irq_enabled is cleared again (irqs_enabled stays set).
 */
int
drv_i915_irq_install(
	struct i915_irq_dev *irq)
{
	int status;

	/*
	 * Some sources are enabled by the postinstall steps, so interrupts are
	 * marked enabled before any of them is, to avoid special cases in the
	 * ordering checks.
	 */
	irq->irqs_enabled = 1;
	irq->irq_enabled = 1;

	/*
	 * Prepares the queue the engine interrupts wake, once for the device's
	 * life, before the handler that wakes it can run.
	 */
	if (irq->engine_wait_ready == 0) {
		spin_init(&irq->engine_lock, LOCK_RANK_DEVICE, "i915 engine irq");
		waitq_init(&irq->engine_waitq, "i915 engine irq");
		irq->engine_wait_ready = 1;
	}

	/*
	 * Without a display table no display source is reset, enabled or
	 * acknowledged.  The device start always binds one: the display half
	 * (display/interrupts.c) or, without a display, the interim hooks that
	 * keep the display summary off.
	 */
	if (irq->display_ops == NULL)
		kern_logf("i915: display interrupts are not connected: no display table is bound\n");

	/* Lets the display power wells enable their pipe interrupts from now on. */
	if (irq->display_ops != NULL)
		irq->display_ops->set_irqs_enabled(irq->display_context, 1);

	/* Masks, disables and clears every source before the handler exists. */
	drv_i915_irq_reset(irq);

	/*
	 * Attaches the handler.  The vector was allocated without a handler,
	 * and until the attach succeeds an arriving message is masked and
	 * acknowledged by the kernel.
	 */
	status = kern_irq_attach_msi(irq->msi_irq, i915_irq_handler, irq);
	if (status != 0) {
		/* Only irq_enabled is cleared; irqs_enabled stays set. */
		irq->irq_enabled = 0;
		if (irq->display_ops != NULL)
			irq->display_ops->set_irqs_enabled(irq->display_context, 0);
		kern_logf("i915: irq install: kern_irq_attach_msi(irq=%d) failed rc=%d\n",
		    irq->msi_irq,
		    status);

		return EIO;
	}

	/* The handler may now run; uninstall must detach it. */
	irq->handler_attached = 1;

	/* Enables the sources and then the master control. */
	drv_i915_irq_postinstall(irq);

	/* Succeeded: the handler is attached and the sources are enabled. */
	return 0;
}

/*
 * Resets the interrupt sources and detaches the handler.
 *
 * The detach waits until no invocation of the handler is running on any
 * CPU.  The MSI vector itself stays allocated;
 * the PCI part frees it.
 */
void
drv_i915_irq_uninstall(
	struct i915_irq_dev *irq)
{
	int status;

	/* Nothing was installed. */
	if (irq->irq_enabled == 0)
		return;

	/* Reports display state that should already have been released. */
	if (irq->display_ops != NULL)
		irq->display_ops->uninstall_check(irq->display_context);

	/*
	 * Masks and disables every source first: detaching the handler does
	 * not stop the device from sending messages.
	 */
	drv_i915_irq_reset(irq);

	/* Interrupts are off from here on for the handler and the power wells. */
	irq->irq_enabled = 0;
	irq->irqs_enabled = 0;
	if (irq->display_ops != NULL)
		irq->display_ops->set_irqs_enabled(irq->display_context, 0);

	/* A handler that was never attached has nothing to detach. */
	if (irq->handler_attached == 0)
		return;

	/* Detaches the handler and waits for any invocation still running. */
	status = kern_irq_detach_msi_sync(irq->msi_irq, i915_irq_handler, irq);
	if (status != 0) {
		kern_logf("i915: irq uninstall: detach_msi_sync (irq=%d) rc=%d\n",
		    irq->msi_irq,
		    status);
	}

	/* The handler is no longer attached, whatever the detach reported. */
	irq->handler_attached = 0;
}

/*
 * Waits until every handler invocation seen at the call has finished.
 *
 * It waits on the handler's own entry and exit counts, which equals "every
 * invocation started before the call has finished" only if invocations of
 * this handler never overlap; nothing in
 * the kernel states that an invocation cannot begin after the EOI while the
 * previous one is still returning, so a path that must be exact drains its
 * own in-flight count instead.  Returns 0, ETIMEDOUT after 100 ms, or EIO
 * when the time base fails while waiting.
 */
int
drv_i915_synchronize_irq(
	struct i915_irq_dev *irq)
{
	unsigned seen;
	unsigned exits;
	unsigned waited;
	int delay_error;

	/* Samples how many invocations had started at the call, and counts the call. */
	seen = __atomic_load_n(&irq->handler_entries, __ATOMIC_SEQ_CST);
	irq->sync_calls++;

	/* Waits for the exits to catch up with the entries seen. */
	for (waited = 0U; waited < I915_IRQ_SYNC_TIMEOUT_US; waited += I915_IRQ_SYNC_STEP_US) {
		/* Every invocation seen at the call has left the handler. */
		exits = __atomic_load_n(&irq->handler_exits, __ATOMIC_SEQ_CST);
		if ((int)(exits - seen) >= 0)
			return 0;

		/* Lets the running invocation make progress. */
		delay_error = drv_i915_udelay(I915_IRQ_SYNC_STEP_US);
		if (delay_error != 0) {
			irq->sync_time_faults++;
			kern_logf("i915: irq sync: the time base failed while waiting (not a timeout)\n");

			return EIO;
		}
	}

	/* An invocation did not finish within the bound. */
	irq->sync_timeouts++;
	kern_logf("i915: irq sync: a handler invocation did not finish (entries=%u exits=%u)\n",
	    seen,
	    irq->handler_exits);

	return ETIMEDOUT;
}

/*
 * Reads the sequence of the engine interrupts.
 *
 * A waiter reads it before it looks at the engine and hands it to
 * drv_i915_irq_engine_wait(), so an interrupt that comes between the look
 * and the sleep ends the sleep at once instead of being missed.
 */
uint64_t
drv_i915_irq_engine_sequence(
	struct i915_irq_dev *irq)
{
	uint64_t sequence;

	/* Reads the sequence each engine interrupt advances. */
	sequence = waitq_sequence(&irq->engine_waitq);

	/* Succeeded: reports the sequence. */
	return sequence;
}

/*
 * Sleeps until an engine interrupt comes after the observed sequence, or
 * until a number of scheduler ticks has passed.
 *
 * Returns 0 once an interrupt came, also one that came before the sleep
 * started; ETIMEDOUT when the ticks passed without one; or ENODEV when the
 * handler is not attached, so no interrupt will come and the caller polls
 * instead.
 */
int
drv_i915_irq_engine_wait(
	struct i915_irq_dev *irq,
	uint64_t observed,
	unsigned ticks)
{
	unsigned long flags;
	uint64_t deadline;
	int error;

	/* Without the handler no engine interrupt wakes the queue. */
	if (irq->engine_wait_ready == 0 || irq->handler_attached == 0)
		return ENODEV;

	/* The sleep ends at this tick at the latest. */
	deadline = sched_ticks() + ticks;

	/* Sleeps on the queue unless an interrupt came since the observation. */
	flags = spin_lock_irqsave(&irq->engine_lock);

	error = waitq_sleep(&irq->engine_waitq, &irq->engine_lock, observed, deadline, 0U);

	spin_unlock_irqrestore(&irq->engine_lock, flags);

	/* An interrupt that came before the sleep started is an interrupt too. */
	if (error == EAGAIN)
		return 0;

	/* Reports a sleep that ran out, or that could not start. */
	if (error != 0)
		return error;

	/* Succeeded: an engine interrupt came, or the sleep ended early. */
	return 0;
}

/*
 * Masks, disables and clears every interrupt source: the master control
 * first, then the GT, the display, GU_MISC and PCU sources.
 */
void
drv_i915_irq_reset(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_layout *layout;

	/* Picks the register layout of the device. */
	layout = i915_irq_layout_of(irq);

	/* Stops the device from raising any interrupt while the sources change. */
	(void)i915_irq_master_off(irq);

	/* Closes the GT sources. */
	layout->gt_close(irq);

	/* Without a display table the display sources keep what the firmware left. */
	if (irq->display_ops != NULL)
		irq->display_ops->reset(irq->display_context);

	/* Closes GU_MISC where the layout has it, then PCU. */
	if (layout->has_gu_misc != 0)
		drv_i915_irq_regs_close(irq, I915_IRQ_GU_MISC_IMR, I915_IRQ_GU_MISC_IIR, I915_IRQ_GU_MISC_IER);
	drv_i915_irq_regs_close(irq, I915_IRQ_PCU_IMR, I915_IRQ_PCU_IIR, I915_IRQ_PCU_IER);
}

/*
 * Enables the interrupt sources and then the master control: the GT, the
 * display and GU_MISC, then the master control with a posting read.
 */
void
drv_i915_irq_postinstall(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_layout *layout;

	/* Picks the register layout of the device. */
	layout = i915_irq_layout_of(irq);

	/* Opens the GT sources the submission backend needs. */
	layout->gt_open(irq);

	/* Opens the display sources. */
	if (irq->display_ops != NULL)
		irq->display_ops->postinstall(irq->display_context);

	/* Opens the GU_MISC graphics system event, which feeds the OpRegion. */
	if (layout->has_gu_misc != 0) {
		drv_i915_irq_regs_open(irq,
		    I915_IRQ_GU_MISC_IMR,
		    ~I915_IRQ_GU_MISC_GSE,
		    I915_IRQ_GU_MISC_IER,
		    I915_IRQ_GU_MISC_GSE,
		    I915_IRQ_GU_MISC_IIR);
	}

	/* Records that every source has been set up. */
	irq->reached_postinstall = 1;

	/* Lets the device raise interrupts, and flushes the enable. */
	i915_irq_master_on(irq);
	drv_i915_posting_read32(irq->m, layout->master);
	irq->reached_master_enable = 1;
}

/*
 * Serves the Gen11 GT banks the master control reports.
 *
 * Each bank whose bit is in the master control is served through the
 * identity handshake and then cleared.
 */
void
drv_i915_irq_gt_banks_serve(
	struct i915_irq_dev *irq,
	uint32_t master_ctl)
{
	unsigned bank;

	/* Serves each bank whose bit the master control carries. */
	for (bank = 0U; bank < I915_IRQ_GT_BANKS; bank++) {
		if ((master_ctl & I915_GT_GEN11_GT_DW_IRQ(bank)) != 0U)
			i915_irq_gen11_bank(irq, bank);
	}
}

/*
 * Clears the GT power management source of GT bank 0 while it is pending.
 *
 * A pending bit cannot be cleared before its identity has been served.  The
 * caller has the RPS events masked, so the handler does not race it for
 * this bit.
 */
void
drv_i915_gt_pm_reset_iir(
	struct i915_irq_dev *irq)
{
	uint32_t pending;
	unsigned round;

	/* Serves and clears the source while it stays pending, a few times at most. */
	for (round = 0U; round < I915_IRQ_GTPM_RESETS; round++) {
		pending = drv_i915_raw_read32(irq->m, I915_GT_GEN11_GT_INTR_DW(0U));
		if ((pending & (1U << I915_IRQ_GTPM_BIT)) == 0U)
			return;

		/* The identity first, then the bit, or the register stays locked for everybody. */
		(void)i915_irq_gen11_identity(irq, 0U, I915_IRQ_GTPM_BIT);
		drv_i915_raw_write32(irq->m, I915_GT_GEN11_GT_INTR_DW(0U), 1U << I915_IRQ_GTPM_BIT);
	}
}

/*
 * Closes one register set of mask, identity and enable.
 *
 * Every source is masked (flushed), disabled, and the identity register is
 * cleared twice, because it can hold a second event behind the first.  The
 * writes are counted as reset writes.
 */
void
drv_i915_irq_regs_close(
	struct i915_irq_dev *irq,
	uint32_t imr,
	uint32_t iir,
	uint32_t ier)
{
	/* Masks every source, and flushes the mask. */
	i915_irq_write(irq, imr, 0xffffffffU, &irq->reset_writes);
	drv_i915_posting_read32(irq->m, imr);

	/* Disables every source. */
	i915_irq_write(irq, ier, 0U, &irq->reset_writes);

	/* Clears both events the identity register can hold. */
	i915_irq_write(irq, iir, 0xffffffffU, &irq->reset_writes);
	drv_i915_posting_read32(irq->m, iir);
	i915_irq_write(irq, iir, 0xffffffffU, &irq->reset_writes);
	drv_i915_posting_read32(irq->m, iir);
}

/*
 * Warns about and clears a stale identity register.
 *
 * A stale event is a warning, not a failure.  The clearing writes are not
 * counted.
 */
void
drv_i915_irq_regs_check_clear(
	struct i915_irq_dev *irq,
	uint32_t iir)
{
	uint32_t pending;

	/* Reads what is still latched from before the reset. */
	pending = drv_i915_read32(irq->m, iir);
	if (pending == 0U)
		return;

	/* Reports the stale events and clears both, as the close does. */
	kern_logf("i915: WARN interrupt register 0x%x is not zero: 0x%08x\n",
	    iir,
	    pending);
	i915_irq_write(irq, iir, 0xffffffffU, NULL);
	drv_i915_posting_read32(irq->m, iir);
	i915_irq_write(irq, iir, 0xffffffffU, NULL);
	drv_i915_posting_read32(irq->m, iir);
}

/*
 * Opens one register set of mask, identity and enable.
 *
 * The identity register is checked clean first, so no stale event arrives
 * the moment the source opens; then the enable and the mask are written and
 * the mask is flushed.  The writes are counted as postinstall writes.
 */
void
drv_i915_irq_regs_open(
	struct i915_irq_dev *irq,
	uint32_t imr,
	uint32_t imr_value,
	uint32_t ier,
	uint32_t ier_value,
	uint32_t iir)
{
	/* Makes sure no stale event is delivered. */
	drv_i915_irq_regs_check_clear(irq, iir);

	/* Enables the sources, then unmasks them and flushes the mask. */
	i915_irq_write(irq, ier, ier_value, &irq->postinstall_writes);
	i915_irq_write(irq, imr, imr_value, &irq->postinstall_writes);
	drv_i915_posting_read32(irq->m, imr);
}

/* Writes one interrupt register and counts the write in the given counter. */
static void
i915_irq_write(
	struct i915_irq_dev *irq,
	uint32_t offset,
	uint32_t value,
	unsigned *counter)
{
	/* Writes the register through the held access path. */
	drv_i915_write32(irq->m, offset, value);

	/* Counts the write for the reset or postinstall diagnostics. */
	if (counter != NULL)
		(*counter)++;
}

/* Returns the GT interrupt layout of the device. */
static const struct i915_irq_layout *
i915_irq_layout_of(
	const struct i915_irq_dev *irq)
{
	/* A Gen9 GT has the Gen8 layout. */
	if (irq->gt != NULL && irq->gt->graphics_ver == 9)
		return &i915_irq_gen8_layout;

	/* Every other device, and one whose GT information is not bound yet, has the Gen11 layout. */
	return &i915_irq_gen11_layout;
}

/* Turns the master control off and samples the pending level indications. */
static uint32_t
i915_irq_master_off(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_layout *layout;
	uint32_t master_ctl;

	/* Stops the device from raising further interrupts. */
	layout = i915_irq_layout_of(irq);
	drv_i915_raw_write32(irq->m, layout->master, 0U);

	/*
	 * With the master control off, samples the level indications; they
	 * are cleared by the acknowledges of the sources they name.
	 */
	master_ctl = drv_i915_raw_read32(irq->m, layout->master);

	/* Succeeded: reports the sources that were pending. */
	return master_ctl;
}

/* Turns the master control back on. */
static void
i915_irq_master_on(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_layout *layout;

	/* Lets the device raise interrupts again. */
	layout = i915_irq_layout_of(irq);
	drv_i915_raw_write32(irq->m, layout->master, layout->master_on);
}

/*
 * Returns the interrupt bits each engine opens.
 *
 * Every backend needs the user interrupt that completes a request.  With
 * execlists the driver owns the context switch and also needs the command
 * streamer error, context switch and semaphore wait interrupts; the GuC
 * backend leaves them to the GuC.
 */
static uint32_t
i915_irq_engine_bits(
	const struct i915_irq_dev *irq)
{
	uint32_t bits;

	/* The user interrupt, for every backend. */
	bits = I915_GT_GT_RENDER_USER_INTERRUPT;

	/* The command streamer interrupts, for execlists. */
	if (irq->submission != I915_SUBMISSION_GUC) {
		bits |= I915_GT_GT_CS_MASTER_ERROR_INTERRUPT;
		bits |= I915_GT_GT_CONTEXT_SWITCH_INTERRUPT;
		bits |= I915_GT_GT_WAIT_SEMAPHORE_INTERRUPT;
	}

	/* Succeeded: reports the bits. */
	return bits;
}

/*
 * Counts the sources of one engine interrupt and wakes the engine waiters.
 *
 * The follow-up work (the error state, the semaphore, the context status
 * buffer, request completion) belongs to the submission backend: a user
 * interrupt or a context switch wakes the threads waiting for an engine,
 * which read the status themselves.
 */
static void
i915_irq_engine_event(
	struct i915_irq_dev *irq,
	uint32_t bits)
{
	/* The command streamer reported an error. */
	if ((bits & I915_GT_GT_CS_MASTER_ERROR_INTERRUPT) != 0U)
		irq->gt_error_intr++;

	/* The engine is waiting on a semaphore. */
	if ((bits & I915_GT_GT_WAIT_SEMAPHORE_INTERRUPT) != 0U)
		irq->gt_semaphore_intr++;

	/* The engine switched context; the status buffer has new entries. */
	if ((bits & I915_GT_GT_CONTEXT_SWITCH_INTERRUPT) != 0U)
		irq->gt_ctx_switch_intr++;

	/* A user interrupt completed a request. */
	if ((bits & I915_GT_GT_RENDER_USER_INTERRUPT) != 0U)
		irq->gt_user_intr++;

	/* Wakes a thread waiting for a request's end or for new status entries. */
	if ((bits & (I915_GT_GT_RENDER_USER_INTERRUPT | I915_GT_GT_CONTEXT_SWITCH_INTERRUPT)) != 0U)
		i915_irq_engine_wake(irq);
}

/* Wakes every thread sleeping on the engine interrupts; runs in the handler. */
static void
i915_irq_engine_wake(
	struct i915_irq_dev *irq)
{
	unsigned long flags;

	/* The queue is not prepared before install. */
	if (irq->engine_wait_ready == 0)
		return;

	/* Advances the sequence and wakes the sleepers under the queue's condition lock. */
	flags = spin_lock_irqsave(&irq->engine_lock);

	irq->engine_wakeups++;
	waitq_wake_all(&irq->engine_waitq);

	spin_unlock_irqrestore(&irq->engine_lock, flags);
}

/* Reports nonzero when an engine of this class and instance exists. */
static int
i915_irq_engine_exists(
	const struct i915_irq_dev *irq,
	unsigned engine_class,
	unsigned instance)
{
	unsigned index;

	/* No engine is known before the engine table has been bound. */
	if (irq->gt == NULL)
		return 0;

	/* Looks the class and instance up in the engine table. */
	for (index = 0U; index < irq->gt->num_engines; index++) {
		if ((unsigned)irq->gt->engines[index].class == engine_class &&
		    (unsigned)irq->gt->engines[index].instance == instance)
			return 1;
	}

	/* No engine of this class and instance exists. */
	return 0;
}

/* Hands power management events to the RPS handler, when RPS has registered one; runs in the handler. */
static void
i915_irq_power_event(
	struct i915_irq_dev *irq,
	uint32_t bits)
{
	void (*handler)(void *context, uint32_t pm_iir);

	/* Without a registered handler the events are only counted. */
	handler = irq->pm_handler;
	if (handler == NULL)
		return;

	/* One more served power management event. */
	irq->gt_pm_intrs++;
	handler(irq->pm_context, bits);
}

/*
 * Closes the Gen11 GT sources: the engine registers, then each other
 * source's enable and mask.
 */
static void
i915_irq_gen11_close(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_engine_reg *reg;
	const struct i915_irq_other_reg *other;
	uint32_t closed;
	unsigned index;

	/* Disables the engine classes and masks every engine. */
	for (index = 0U; index < I915_IRQ_COUNT(i915_irq_gen11_engine_regs); index++) {
		reg = &i915_irq_gen11_engine_regs[index];
		closed = ~0U;
		if (reg->kind == I915_IRQ_ENABLE_PAIR)
			closed = 0U;
		i915_irq_write(irq, reg->offset, closed, &irq->reset_writes);
	}

	/* Disables and masks power management, GuC and crypto. */
	for (index = 0U; index < I915_IRQ_COUNT(i915_irq_gen11_other_regs); index++) {
		other = &i915_irq_gen11_other_regs[index];
		i915_irq_write(irq, other->enable, 0U, &irq->reset_writes);
		i915_irq_write(irq, other->mask, ~0U, &irq->reset_writes);
	}
}

/*
 * Opens the Gen11 engine sources and keeps power management closed.
 *
 * Each register holds two engines, one per 16-bit half; a mask whose lower
 * half is reserved opens only the upper engine.  RPS opens power management
 * itself when it starts.
 */
static void
i915_irq_gen11_open(
	struct i915_irq_dev *irq)
{
	const struct i915_irq_engine_reg *reg;
	const struct i915_irq_other_reg *power;
	uint32_t bits;
	uint32_t pair;
	uint32_t upper;
	uint32_t value;
	unsigned index;

	/* The bits for both engines of a pair, and for the upper one only. */
	bits = i915_irq_engine_bits(irq);
	pair = bits << 16 | bits;
	upper = bits << 16;

	/* Keeps the masks for the diagnostics. */
	irq->gt_irqs = bits;
	irq->gt_dmask = pair;
	irq->gt_smask = upper;

	/* Enables the engine classes and unmasks the engines. */
	for (index = 0U; index < I915_IRQ_COUNT(i915_irq_gen11_engine_regs); index++) {
		reg = &i915_irq_gen11_engine_regs[index];
		if (reg->kind == I915_IRQ_ENABLE_PAIR) {
			value = pair;
		} else if (reg->kind == I915_IRQ_MASK_PAIR) {
			value = ~pair;
		} else {
			value = ~upper;
		}
		i915_irq_write(irq, reg->offset, value, &irq->postinstall_writes);
	}

	/* Leaves power management disabled and masked. */
	power = &i915_irq_gen11_other_regs[I915_IRQ_GEN11_OTHER_PM];
	i915_irq_write(irq, power->enable, 0U, &irq->postinstall_writes);
	i915_irq_write(irq, power->mask, ~0U, &irq->postinstall_writes);
}

/*
 * Serves a Gen11 interrupt with the master control off: the GT banks, the
 * display, then GU_MISC is taken, the master control is turned back on,
 * and a graphics system event goes to the OpRegion.
 */
static void
i915_irq_gen11_serve(
	struct i915_irq_dev *irq,
	uint32_t master_ctl)
{
	uint32_t gu_misc;

	/* Serves the GT banks the master control reports. */
	if ((master_ctl & (I915_GT_GEN11_GT_DW_IRQ(0) | I915_GT_GEN11_GT_DW_IRQ(1))) != 0U) {
		irq->gt_irq_count++;
		drv_i915_irq_gt_banks_serve(irq, master_ctl);
	}

	/* Without a display table the display sources are counted but not acknowledged. */
	if ((master_ctl & I915_GT_GEN11_DISPLAY_IRQ) != 0U) {
		irq->display_irq_count++;
		if (irq->display_ops != NULL)
			irq->display_ops->handle(irq->display_context, master_ctl);
	}

	/* Takes GU_MISC while the master control is still off. */
	gu_misc = 0U;
	if ((master_ctl & I915_GT_GEN11_GU_MISC_IRQ) != 0U) {
		gu_misc = drv_i915_read32(irq->m, I915_IRQ_GU_MISC_IIR);
		if (gu_misc != 0U)
			drv_i915_write32(irq->m, I915_IRQ_GU_MISC_IIR, gu_misc);
	}
	irq->last_gu_misc_iir = gu_misc;

	/* Lets the device raise interrupts again. */
	i915_irq_master_on(irq);

	/* Hands a graphics system event to the OpRegion. */
	if ((gu_misc & I915_IRQ_GU_MISC_GSE) != 0U) {
		irq->gse_count++;
		if (irq->display_ops != NULL)
			irq->display_ops->gse(irq->display_context);
	}
}

/* Asks for the identity behind one bank bit, waits for it and acknowledges it. */
static uint32_t
i915_irq_gen11_identity(
	struct i915_irq_dev *irq,
	unsigned bank,
	unsigned bit)
{
	uint32_t identity;
	unsigned reads;

	/* Selects the bit. */
	drv_i915_raw_write32(irq->m, I915_GT_GEN11_IIR_REG_SELECTOR(bank), 1U << bit);

	/* Waits for DATA_VALID, a bounded spin because it runs in interrupt context. */
	identity = 0U;
	for (reads = 0U; reads < I915_IRQ_IDENTITY_READS; reads++) {
		identity = drv_i915_raw_read32(irq->m, I915_GT_GEN11_INTR_IDENTITY_REG(bank));
		if ((identity & I915_GT_GEN11_INTR_DATA_VALID) != 0U)
			break;
	}

	/* Counts one handshake, whether or not it completed. */
	irq->gt_identity_reads++;

	/* The identity never became valid; there is nothing to acknowledge. */
	if ((identity & I915_GT_GEN11_INTR_DATA_VALID) == 0U) {
		kern_logf("i915: INTR_IDENTITY_REG%u:%u 0x%08x not valid!\n",
		    bank,
		    bit,
		    identity);
		irq->gt_identity_invalid++;
		return 0U;
	}

	/* Acknowledges the identity by writing DATA_VALID back. */
	drv_i915_raw_write32(irq->m, I915_GT_GEN11_INTR_IDENTITY_REG(bank), I915_GT_GEN11_INTR_DATA_VALID);

	/* Succeeded: reports the class, instance and interrupt bits. */
	return identity;
}

/* Hands one identity to its engine or to power management, or counts it as unknown. */
static void
i915_irq_gen11_dispatch(
	struct i915_irq_dev *irq,
	uint32_t identity)
{
	unsigned engine_class;
	unsigned instance;
	uint32_t bits;

	/* Splits the identity into class, instance and interrupt bits. */
	engine_class = I915_GT_GEN11_INTR_ENGINE_CLASS(identity);
	instance = I915_GT_GEN11_INTR_ENGINE_INSTANCE(identity);
	bits = I915_GT_GEN11_INTR_ENGINE_INTR(identity);
	irq->last_gt_identity = identity;

	/* An identity without interrupt bits, or one that never became valid. */
	if (bits == 0U)
		return;

	/* An engine of the GT. */
	if (engine_class <= (unsigned)I915_MAX_ENGINE_CLASS &&
	    instance <= (unsigned)I915_MAX_ENGINE_INSTANCE &&
	    i915_irq_engine_exists(irq, engine_class, instance) != 0) {
		irq->gt_engine_intrs++;
		i915_irq_engine_event(irq, bits);
		return;
	}

	/* The OTHER class: power management goes to RPS; GuC, KCR and GSC are only counted. */
	if (engine_class == (unsigned)I915_OTHER_CLASS) {
		irq->gt_other_intrs++;
		if (instance == I915_IRQ_GTPM_INSTANCE)
			i915_irq_power_event(irq, bits);
		return;
	}

	/* An identity that names nothing this driver knows. */
	kern_logf("i915: unknown interrupt class=0x%x instance=0x%x intr=0x%x\n",
	    engine_class,
	    instance,
	    bits);
	irq->gt_unknown_class++;
}

/* Serves every pending bit of one Gen11 GT bank, then clears the bank. */
static void
i915_irq_gen11_bank(
	struct i915_irq_dev *irq,
	unsigned bank)
{
	uint32_t pending;
	uint32_t identity;
	unsigned bit;

	/* Reads which sources of the bank are pending. */
	pending = drv_i915_raw_read32(irq->m, I915_GT_GEN11_GT_INTR_DW(bank));
	irq->last_gt_intr_dw[bank] = pending;

	/* Serves each pending source through the identity handshake. */
	for (bit = 0U; bit < 32U; bit++) {
		if ((pending & (1U << bit)) == 0U)
			continue;

		identity = i915_irq_gen11_identity(irq, bank, bit);
		i915_irq_gen11_dispatch(irq, identity);
	}

	/* Clears the bank only after every identity behind it has been served. */
	drv_i915_raw_write32(irq->m, I915_GT_GEN11_GT_INTR_DW(bank), pending);

	/* Counts a bank that actually had something to acknowledge. */
	if (pending != 0U)
		irq->gt_bank_acks[bank]++;
}

/* Closes the four Gen8 GT banks. */
static void
i915_irq_gen8_close(
	struct i915_irq_dev *irq)
{
	unsigned bank;

	/* Closes each bank's register set. */
	for (bank = 0U; bank < I915_IRQ_GEN8_BANKS; bank++)
		drv_i915_irq_regs_close(irq, I915_GT_GEN8_GT_IMR(bank), I915_GT_GEN8_GT_IIR(bank), I915_GT_GEN8_GT_IER(bank));
}

/*
 * Opens the engines of the Gen8 GT banks.
 *
 * Each engine gets the interrupt bits at its position in its bank.  The
 * power management bank has no engine and stays closed: RPS opens its
 * sources itself.
 */
static void
i915_irq_gen8_open(
	struct i915_irq_dev *irq)
{
	uint32_t bits;
	uint32_t value;
	unsigned bank;

	/* Every engine of the execlists backend takes the same bits. */
	bits = I915_GT_GT_CS_MASTER_ERROR_INTERRUPT;
	bits |= I915_GT_GT_RENDER_USER_INTERRUPT;
	bits |= I915_GT_GT_CONTEXT_SWITCH_INTERRUPT;
	bits |= I915_GT_GT_WAIT_SEMAPHORE_INTERRUPT;

	/* Keeps the masks for the diagnostics: the render and copy bank, and the enhancement bank. */
	irq->gt_irqs = bits;
	irq->gt_dmask = i915_irq_gen8_bank_value(&i915_irq_gen8_banks[0], bits);
	irq->gt_smask = i915_irq_gen8_bank_value(&i915_irq_gen8_banks[3], bits);

	/* Opens each bank for its engines. */
	for (bank = 0U; bank < I915_IRQ_GEN8_BANKS; bank++) {
		value = i915_irq_gen8_bank_value(&i915_irq_gen8_banks[bank], bits);
		drv_i915_irq_regs_open(irq,
		    I915_GT_GEN8_GT_IMR(bank),
		    ~value,
		    I915_GT_GEN8_GT_IER(bank),
		    value,
		    I915_GT_GEN8_GT_IIR(bank));
	}
}

/*
 * Serves a Gen8 interrupt with the master control off: the GT banks, the
 * display, then the master control is turned back on.
 *
 * The Gen9 display sources stay disabled except the flip done of the flip
 * display (display/xd9-flip.c), which the display table acknowledges.
 */
static void
i915_irq_gen8_serve(
	struct i915_irq_dev *irq,
	uint32_t master_ctl)
{
	const struct i915_irq_bank *bank;
	uint32_t iir;
	unsigned order;
	unsigned index;
	unsigned engine;

	/* Serves the banks the master control reports. */
	irq->gt_irq_count++;
	for (order = 0U; order < I915_IRQ_GEN8_BANKS; order++) {
		index = i915_irq_gen8_serve_order[order];
		bank = &i915_irq_gen8_banks[index];
		if ((master_ctl & bank->master_bits) == 0U)
			continue;

		iir = i915_irq_gen8_bank_take(irq, index);

		/* The power management bank goes to RPS. */
		if (bank->power != 0) {
			if (iir != 0U) {
				irq->gt_other_intrs++;
				i915_irq_power_event(irq, iir & 0xffffU);
			}
			continue;
		}

		/* An engine bank: each engine's 16 bits at its position. */
		for (engine = 0U; engine < bank->num_engines; engine++)
			i915_irq_gen8_engine(irq, &bank->engines[engine], (iir >> bank->engines[engine].shift) & 0xffffU);
	}

	/* The rest of the master control is display and PCU. */
	if ((master_ctl & ~I915_IRQ_GEN8_GT_BITS) != 0U) {
		irq->display_irq_count++;
		if (irq->display_ops != NULL)
			irq->display_ops->handle(irq->display_context, master_ctl);
	}

	/* Lets the device raise interrupts again. */
	i915_irq_master_on(irq);
}

/* Returns a Gen8 bank's enable value: the bits at each engine's position. */
static uint32_t
i915_irq_gen8_bank_value(
	const struct i915_irq_bank *bank,
	uint32_t bits)
{
	uint32_t value;
	unsigned engine;

	/* Places the bits of each engine. */
	value = 0U;
	for (engine = 0U; engine < bank->num_engines; engine++)
		value |= bits << bank->engines[engine].shift;

	/* Succeeded: reports the value. */
	return value;
}

/* Reads one Gen8 GT bank's identity register and acknowledges what it holds. */
static uint32_t
i915_irq_gen8_bank_take(
	struct i915_irq_dev *irq,
	unsigned bank)
{
	uint32_t iir;

	/* Reads which sources of the bank are pending. */
	iir = drv_i915_raw_read32(irq->m, I915_GT_GEN8_GT_IIR(bank));

	/* Nothing pending: there is nothing to acknowledge. */
	if (iir == 0U)
		return 0U;

	/* Acknowledges the bank, for every engine in it. */
	drv_i915_raw_write32(irq->m, I915_GT_GEN8_GT_IIR(bank), iir);

	/* The diagnostics keep the first two banks, as for the Gen11 layout. */
	if (bank < I915_IRQ_GT_BANKS) {
		irq->gt_bank_acks[bank]++;
		irq->last_gt_intr_dw[bank] = iir;
	}

	/* Succeeded: reports what was pending. */
	return iir;
}

/* Hands one engine's part of a Gen8 GT bank to the engine, when the engine exists. */
static void
i915_irq_gen8_engine(
	struct i915_irq_dev *irq,
	const struct i915_irq_bank_engine *engine,
	uint32_t iir)
{
	/* An engine with nothing pending has nothing to serve. */
	if (iir == 0U)
		return;

	/* Bits of an engine the GT does not have are counted as unknown. */
	if (i915_irq_engine_exists(irq, (unsigned)engine->engine_class, (unsigned)engine->instance) == 0) {
		irq->gt_unknown_class++;
		return;
	}

	/* Hands the bits to the engine. */
	irq->gt_engine_intrs++;
	i915_irq_engine_event(irq, iir);
}

/* Serves one interrupt; the EOI is sent on every path. */
static void
i915_irq_handler_body(
	int vector,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	const struct i915_irq_layout *layout;
	struct i915_irq_dev *irq;
	uint32_t master_ctl;

	UNUSED_PARAMETER(vector);

	/* The handler was attached with the interrupt device as its argument. */
	irq = argument;
	irq->irq_count++;

	/* Picks the register layout of the device. */
	layout = i915_irq_layout_of(irq);

	/* Interrupts are not enabled: the message is not ours to serve. */
	if (irq->irqs_enabled == 0) {
		irq->irq_none_count++;
		kern_irq_send_eoi(acknowledge);
		return;
	}

	/* Turns the master control off and samples what is pending. */
	master_ctl = i915_irq_master_off(irq);
	irq->last_master_ctl = master_ctl;

	/* Nothing is pending: a spurious message, and the master control reopens. */
	if (master_ctl == 0U) {
		i915_irq_master_on(irq);
		irq->irq_none_count++;
		kern_irq_send_eoi(acknowledge);
		return;
	}

	/* Serves the sources; the layout turns the master control back on. */
	layout->serve(irq, master_ctl);

	/* Finishes the interrupt. */
	irq->irq_handled_count++;
	kern_irq_send_eoi(acknowledge);
}

/* Brackets each handler invocation by the counts drv_i915_synchronize_irq() waits on. */
static void
i915_irq_handler(
	int vector,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	struct i915_irq_dev *irq;

	/* The handler was attached with the interrupt device as its argument. */
	irq = argument;

	/* Counts the entry before any register is touched. */
	(void)__atomic_add_fetch(&irq->handler_entries, 1U, __ATOMIC_SEQ_CST);

	/* Serves the interrupt. */
	i915_irq_handler_body(vector, acknowledge, argument);

	/* Counts the exit once the invocation no longer touches the device. */
	(void)__atomic_add_fetch(&irq->handler_exits, 1U, __ATOMIC_SEQ_CST);
}
