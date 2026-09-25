/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Device interrupts: install, uninstall and the top-level handler (see irq.h).
 *
 * The reset and postinstall steps and the handler follow the reference's
 * gen11_irq_reset(), gen11_irq_postinstall() and gen11_irq_handler() in
 * order.  The GT half follows gt/intel_gt_irq.c; the display half is called
 * through the display operations table at the same points.
 */

#include "i915.h"
#include "irq.h"
#include "mmio.h"
#include "sync.h"
#include "device-info.h"

#include "intel/gt-regs.h"

#include <hal/hal.h>
#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>

/*
 * The GU_MISC interrupt registers and their graphics system event bit
 * (i915_reg.h: GEN11_GU_MISC_IMR, _IIR, _IER, GEN11_GU_MISC_GSE).
 */
#define GEN11_GU_MISC_IMR		0x444f4U
#define GEN11_GU_MISC_IIR		0x444f8U
#define GEN11_GU_MISC_IER		0x444fcU
#define GEN11_GU_MISC_GSE		(1U << 27)

/* The PCU interrupt registers (i915_reg.h: GEN8_PCU_IMR, _IIR, _IER). */
#define GEN8_PCU_IMR			0x444e4U
#define GEN8_PCU_IIR			0x444e8U
#define GEN8_PCU_IER			0x444ecU

/*
 * How many identity reads the GT handler makes before it gives up on
 * DATA_VALID.
 *
 * The reference spins about 100 microseconds as an educated guess.  The
 * handler runs in interrupt context and must not sleep, and each read is a
 * real register access, so the bound is expressed in reads.
 */
#define I915_IRQ_IDENTITY_READS		1000U

/* How many GT interrupt banks the master control reports. */
#define I915_IRQ_GT_BANKS		2U

/*
 * How long drv_i915_synchronize_irq() waits for the handler, and the step
 * it waits in, both in microseconds.
 */
#define I915_IRQ_SYNC_TIMEOUT_US	100000U
#define I915_IRQ_SYNC_STEP_US		10U

static void i915_irq_write(struct i915_irq_dev *irq, uint32_t offset, uint32_t value, unsigned *counter);
static uint32_t i915_master_intr_disable(struct i915_irq_dev *irq);
static void i915_master_intr_enable(struct i915_irq_dev *irq);
static uint32_t i915_gt_engine_identity(struct i915_irq_dev *irq, unsigned bank, unsigned bit);
static void i915_gt_engine_irq(struct i915_irq_dev *irq, uint32_t iir);
static int i915_gt_engine_known(struct i915_irq_dev *irq, unsigned engine_class, unsigned instance);
static void i915_gt_identity_handler(struct i915_irq_dev *irq, uint32_t identity);
static void i915_gt_bank_handler(struct i915_irq_dev *irq, unsigned bank);
static void i915_irq_handler_body(int vector, hal_irq_ack_t acknowledge, void *argument);
static void i915_irq_handler(int vector, hal_irq_ack_t acknowledge, void *argument);

/*
 * Installs the interrupt handler and enables the interrupt sources.
 *
 * This is intel_irq_install().  Returns 0, or EIO when the handler could not
 * be attached, in which case irq_enabled is cleared again as the reference
 * does.
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
	 * Attaches the handler: this is request_irq().  The vector was
	 * allocated without a handler, and until the attach succeeds an
	 * arriving message is masked and acknowledged by the HAL.
	 */
	status = hal_irq_attach_msi(irq->msi_irq, i915_irq_handler, irq);
	if (status != HAL_OK) {
		/* The reference clears only irq_enabled; irqs_enabled stays set. */
		irq->irq_enabled = 0;
		if (irq->display_ops != NULL)
			irq->display_ops->set_irqs_enabled(irq->display_context, 0);
		kern_logf("i915: intel_irq_install: hal_irq_attach_msi(irq=%d) failed rc=%d\n",
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
 * This is intel_irq_uninstall().  The detach waits until no invocation of
 * the handler is running on any CPU.  The MSI vector itself stays allocated;
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
	status = hal_irq_detach_msi_sync(irq->msi_irq, i915_irq_handler, irq);
	if (status != HAL_OK) {
		kern_logf("i915: intel_irq_uninstall: detach_msi_sync (irq=%d) rc=%d\n",
		    irq->msi_irq,
		    status);
	}

	/* The handler is no longer attached, whatever the detach reported. */
	irq->handler_attached = 0;
}

/*
 * Waits until every handler invocation seen at the call has finished.
 *
 * This is intel_synchronize_irq() over the handler's own entry and exit
 * counts.  It equals "every invocation started before the call has
 * finished" only if invocations of this handler never overlap; nothing in
 * the HAL states that an invocation cannot begin after the EOI while the
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
			kern_logf("i915: intel_synchronize_irq: the time base failed while waiting (not a timeout)\n");

			return EIO;
		}
	}

	/* An invocation did not finish within the bound. */
	irq->sync_timeouts++;
	kern_logf("i915: intel_synchronize_irq: a handler invocation did not finish (entries=%u exits=%u)\n",
	    seen,
	    irq->handler_exits);

	return ETIMEDOUT;
}

/*
 * Masks, disables and clears every interrupt source.
 *
 * This is gen11_irq_reset(): the master control first, then the GT, the
 * display, GU_MISC and PCU sources.
 */
void
drv_i915_irq_reset(
	struct i915_irq_dev *irq)
{
	/* Stops the device from raising any interrupt while the sources change. */
	(void)i915_master_intr_disable(irq);

	/* Resets the engine interrupt enables and masks. */
	drv_i915_gen11_gt_irq_reset(irq);

	/*
	 * Resets the display sources (gen11_display_irq_reset()).
	 *
	 * Without a display table the display sources keep whatever
	 * state the firmware left.
	 */
	if (irq->display_ops != NULL)
		irq->display_ops->reset(irq->display_context);

	/* Resets the GU_MISC and PCU sources. */
	drv_i915_gen3_irq_reset(irq, GEN11_GU_MISC_IMR, GEN11_GU_MISC_IIR, GEN11_GU_MISC_IER);
	drv_i915_gen3_irq_reset(irq, GEN8_PCU_IMR, GEN8_PCU_IIR, GEN8_PCU_IER);
}

/*
 * Enables the interrupt sources and then the master control.
 *
 * This is gen11_irq_postinstall(): the GT, the display and the GU_MISC
 * sources, then the master control with a posting read.
 */
void
drv_i915_irq_postinstall(
	struct i915_irq_dev *irq)
{
	uint32_t gu_misc_masked;

	/* Enables the engine interrupts the submission backend needs. */
	drv_i915_gen11_gt_irq_postinstall(irq);

	/* Enables the display sources (gen11_de_irq_postinstall()). */
	if (irq->display_ops != NULL)
		irq->display_ops->postinstall(irq->display_context);

	/* Enables the GU_MISC graphics system event, which feeds the OpRegion. */
	gu_misc_masked = GEN11_GU_MISC_GSE;
	drv_i915_gen3_irq_init(irq, GEN11_GU_MISC_IMR, ~gu_misc_masked, GEN11_GU_MISC_IER, gu_misc_masked, GEN11_GU_MISC_IIR);

	/* Records that every source has been set up. */
	irq->reached_postinstall = 1;

	/* Lets the device raise interrupts, and flushes the enable. */
	i915_master_intr_enable(irq);
	drv_i915_posting_read32(irq->m, GEN11_GFX_MSTR_IRQ);
	irq->reached_master_enable = 1;
}

/*
 * Disables and masks every engine interrupt.
 *
 * This is gen11_gt_irq_reset() for Alder Lake-P, which has RCS0, BCS0,
 * VECS0, VCS0 and VCS2 and no CCS, GSC0 or HECI-GSC, so the Xe-HP-only
 * registers of the reference are not written.
 */
void
drv_i915_gen11_gt_irq_reset(
	struct i915_irq_dev *irq)
{
	/* Disables the RCS, BCS, VCS and VECS class interrupts. */
	i915_irq_write(irq, GEN11_RENDER_COPY_INTR_ENABLE, 0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_VCS_VECS_INTR_ENABLE, 0U, &irq->reset_writes);

	/* Masks every interrupt of the RCS, BCS, VCS and VECS engines. */
	i915_irq_write(irq, GEN11_RCS0_RSVD_INTR_MASK, ~0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_BCS_RSVD_INTR_MASK, ~0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_VCS0_VCS1_INTR_MASK, ~0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_VCS2_VCS3_INTR_MASK, ~0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_VECS0_VECS1_INTR_MASK, ~0U, &irq->reset_writes);

	/* Disables and masks the power management and GuC interrupts. */
	i915_irq_write(irq, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_GPM_WGBOXPERF_INTR_MASK, ~0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_GUC_SG_INTR_ENABLE, 0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_GUC_SG_INTR_MASK, ~0U, &irq->reset_writes);

	/* Disables and masks the crypto interrupts. */
	i915_irq_write(irq, GEN11_CRYPTO_RSVD_INTR_ENABLE, 0U, &irq->reset_writes);
	i915_irq_write(irq, GEN11_CRYPTO_RSVD_INTR_MASK, ~0U, &irq->reset_writes);
}

/*
 * Enables the engine interrupts the submission backend needs.
 *
 * This is gen11_gt_irq_postinstall().  The user interrupt is always
 * enabled; execlists submission adds the command streamer error, context
 * switch and semaphore wait interrupts.
 */
void
drv_i915_gen11_gt_irq_postinstall(
	struct i915_irq_dev *irq)
{
	uint32_t irqs;
	uint32_t guc_mask;
	uint32_t dmask;
	uint32_t smask;

	/* Every backend needs the user interrupt that completes a request. */
	irqs = GT_RENDER_USER_INTERRUPT;

	/* intel_uc_wants_guc() is false with enable_guc=0. */
	guc_mask = 0U;

	/*
	 * With execlists submission the driver owns the context switch, so it
	 * needs the command streamer interrupts.  The GuC arm leaves them out.
	 */
	if (irq->submission != I915_SUBMISSION_GUC) {
		irqs |= GT_CS_MASTER_ERROR_INTERRUPT;
		irqs |= GT_CONTEXT_SWITCH_INTERRUPT;
		irqs |= GT_WAIT_SEMAPHORE_INTERRUPT;
	}

	/* Places the bits for both engines of a pair, or for the upper one only. */
	dmask = irqs << 16 | irqs;
	smask = irqs << 16;

	/* Keeps the masks for the diagnostics. */
	irq->gt_irqs = irqs;
	irq->gt_dmask = dmask;
	irq->gt_smask = smask;

	/* Enables the RCS, BCS, VCS and VECS class interrupts. */
	i915_irq_write(irq, GEN11_RENDER_COPY_INTR_ENABLE, dmask, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_VCS_VECS_INTR_ENABLE, dmask, &irq->postinstall_writes);

	/* Unmasks the interrupts of the RCS, BCS, VCS and VECS engines. */
	i915_irq_write(irq, GEN11_RCS0_RSVD_INTR_MASK, ~smask, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_BCS_RSVD_INTR_MASK, ~smask, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_VCS0_VCS1_INTR_MASK, ~dmask, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_VCS2_VCS3_INTR_MASK, ~dmask, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_VECS0_VECS1_INTR_MASK, ~dmask, &irq->postinstall_writes);

	/* Enables the GuC interrupts when the GuC is used. */
	if (guc_mask != 0U)
		i915_irq_write(irq, GEN11_GUC_SG_INTR_ENABLE, guc_mask << 16, &irq->postinstall_writes);

	/*
	 * Leaves the RPS interrupts disabled and masked: they are enabled on
	 * demand when RPS itself is (pm_ier = 0, pm_imr = ~0).
	 */
	i915_irq_write(irq, GEN11_GPM_WGBOXPERF_INTR_ENABLE, 0U, &irq->postinstall_writes);
	i915_irq_write(irq, GEN11_GPM_WGBOXPERF_INTR_MASK, ~0U, &irq->postinstall_writes);
}

/*
 * Serves the GT interrupt banks the master control reports.
 *
 * This is gen11_gt_irq_handler().
 */
void
drv_i915_gen11_gt_irq_handler(
	struct i915_irq_dev *irq,
	uint32_t master_ctl)
{
	unsigned bank;

	/* Serves each bank whose bit the master control carries. */
	for (bank = 0U; bank < I915_IRQ_GT_BANKS; bank++) {
		if ((master_ctl & GEN11_GT_DW_IRQ(bank)) != 0U)
			i915_gt_bank_handler(irq, bank);
	}
}

/*
 * Masks, disables and clears one interrupt register set.
 *
 * This is gen3_irq_reset(): IMR all set, IER cleared, then IIR cleared
 * twice because it can queue up two events.  The writes are counted as
 * reset writes.
 */
void
drv_i915_gen3_irq_reset(
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

	/* IIR can theoretically queue up two events, so it is cleared twice. */
	i915_irq_write(irq, iir, 0xffffffffU, &irq->reset_writes);
	drv_i915_posting_read32(irq->m, iir);
	i915_irq_write(irq, iir, 0xffffffffU, &irq->reset_writes);
	drv_i915_posting_read32(irq->m, iir);
}

/*
 * Warns about and clears a stale interrupt identity register.
 *
 * This is gen3_assert_iir_is_zero(): a stale IIR is a warning, not a
 * failure.  The clearing writes are not counted.
 */
void
drv_i915_gen3_assert_iir_is_zero(
	struct i915_irq_dev *irq,
	uint32_t iir)
{
	uint32_t pending;

	/* Reads what is still latched from before the reset. */
	pending = drv_i915_read32(irq->m, iir);
	if (pending == 0U)
		return;

	/* Reports the stale events and clears them twice, as the reset does. */
	kern_logf("i915: WARN interrupt register 0x%x is not zero: 0x%08x\n",
	    iir,
	    pending);
	i915_irq_write(irq, iir, 0xffffffffU, NULL);
	drv_i915_posting_read32(irq->m, iir);
	i915_irq_write(irq, iir, 0xffffffffU, NULL);
	drv_i915_posting_read32(irq->m, iir);
}

/*
 * Enables one interrupt register set.
 *
 * This is gen3_irq_init(): the IIR is checked clean, then IER and IMR are
 * written, and IMR is flushed.  The writes are counted as postinstall
 * writes.
 */
void
drv_i915_gen3_irq_init(
	struct i915_irq_dev *irq,
	uint32_t imr,
	uint32_t imr_value,
	uint32_t ier,
	uint32_t ier_value,
	uint32_t iir)
{
	/* Makes sure no stale event is delivered the moment the source opens. */
	drv_i915_gen3_assert_iir_is_zero(irq, iir);

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

/* Turns the master control off and samples the pending level indications. */
static uint32_t
i915_master_intr_disable(
	struct i915_irq_dev *irq)
{
	uint32_t master_ctl;

	/* Stops the device from raising further interrupts. */
	drv_i915_raw_write32(irq->m, GEN11_GFX_MSTR_IRQ, 0U);

	/*
	 * With the master control off, samples the level indications; they
	 * are cleared by the acknowledges of the sources they name.
	 */
	master_ctl = drv_i915_raw_read32(irq->m, GEN11_GFX_MSTR_IRQ);

	/* Succeeded: reports the sources that were pending. */
	return master_ctl;
}

/* Turns the master control back on. */
static void
i915_master_intr_enable(
	struct i915_irq_dev *irq)
{
	/* Lets the device raise interrupts again. */
	drv_i915_raw_write32(irq->m, GEN11_GFX_MSTR_IRQ, GEN11_MASTER_IRQ);
}

/* Selects one bank bit, waits for its identity and acknowledges it. */
static uint32_t
i915_gt_engine_identity(
	struct i915_irq_dev *irq,
	unsigned bank,
	unsigned bit)
{
	uint32_t identity;
	unsigned spins;

	/* Asks the device for the identity behind this bit. */
	drv_i915_raw_write32(irq->m, GEN11_IIR_REG_SELECTOR(bank), 1U << bit);

	/*
	 * Waits for the identity to become valid.  This is a bounded spin,
	 * not a sleep, because it runs in interrupt context.
	 */
	identity = 0U;
	for (spins = 0U; spins < I915_IRQ_IDENTITY_READS; spins++) {
		identity = drv_i915_raw_read32(irq->m, GEN11_INTR_IDENTITY_REG(bank));
		if ((identity & GEN11_INTR_DATA_VALID) != 0U)
			break;
	}

	/* Counts one identity handshake, whether or not it completed. */
	irq->gt_identity_reads++;

	/* The identity never became valid; there is nothing to acknowledge. */
	if ((identity & GEN11_INTR_DATA_VALID) == 0U) {
		kern_logf("i915: INTR_IDENTITY_REG%u:%u 0x%08x not valid!\n",
		    bank,
		    bit,
		    identity);
		irq->gt_identity_invalid++;
		return 0U;
	}

	/* Acknowledges the identity by writing DATA_VALID back. */
	drv_i915_raw_write32(irq->m, GEN11_INTR_IDENTITY_REG(bank), GEN11_INTR_DATA_VALID);

	/* Succeeded: reports the class, instance and interrupt bits. */
	return identity;
}

/*
 * Counts the sources of one engine interrupt.
 *
 * This is the decode of execlists_irq_handler().  The bottom halves
 * (RING_EIR handling, the semaphore yield, the CSB tasklet, the breadcrumb
 * signal) belong to the submission backend; here each source is counted
 * and the handler stays a pure acknowledge.
 */
static void
i915_gt_engine_irq(
	struct i915_irq_dev *irq,
	uint32_t iir)
{
	/* The command streamer reported an error. */
	if ((iir & GT_CS_MASTER_ERROR_INTERRUPT) != 0U)
		irq->gt_error_intr++;

	/* The engine is waiting on a semaphore. */
	if ((iir & GT_WAIT_SEMAPHORE_INTERRUPT) != 0U)
		irq->gt_semaphore_intr++;

	/* The engine switched context; the CSB has new entries. */
	if ((iir & GT_CONTEXT_SWITCH_INTERRUPT) != 0U)
		irq->gt_ctx_switch_intr++;

	/* A user interrupt completed a request. */
	if ((iir & GT_RENDER_USER_INTERRUPT) != 0U)
		irq->gt_user_intr++;
}

/* Reports nonzero when an engine of this class and instance exists. */
static int
i915_gt_engine_known(
	struct i915_irq_dev *irq,
	unsigned engine_class,
	unsigned instance)
{
	unsigned index;

	/* No engine is known before the engine table has been bound. */
	if (irq->gt == NULL)
		return 0;

	/* Looks the class and instance up in the engine table. */
	for (index = 0U; index < irq->gt->num_engines; index++) {
		if ((unsigned)irq->gt->engines[index].class != engine_class)
			continue;
		if ((unsigned)irq->gt->engines[index].instance != instance)
			continue;

		/* Succeeded: the identity names this engine. */
		return 1;
	}

	/* No engine of this class and instance exists. */
	return 0;
}

/* Dispatches one interrupt identity to its engine or counts it. */
static void
i915_gt_identity_handler(
	struct i915_irq_dev *irq,
	uint32_t identity)
{
	unsigned engine_class;
	unsigned instance;
	uint32_t intr;
	int known;

	/* Splits the identity into class, instance and interrupt bits. */
	engine_class = GEN11_INTR_ENGINE_CLASS(identity);
	instance = GEN11_INTR_ENGINE_INSTANCE(identity);
	intr = GEN11_INTR_ENGINE_INTR(identity);
	irq->last_gt_identity = identity;

	/* An identity without interrupt bits, or one that never became valid. */
	if (intr == 0U)
		return;

	/* Hands an engine interrupt to the engine's decode. */
	if (engine_class <= (unsigned)I915_MAX_ENGINE_CLASS &&
	    instance <= (unsigned)I915_MAX_ENGINE_INSTANCE) {
		known = i915_gt_engine_known(irq, engine_class, instance);
		if (known != 0) {
			irq->gt_engine_intrs++;
			i915_gt_engine_irq(irq, intr);
			return;
		}
	}

	/* GuC, GTPM (RPS), KCR and GSC have no bottom half in this port. */
	if (engine_class == (unsigned)I915_OTHER_CLASS) {
		irq->gt_other_intrs++;
		return;
	}

	/* Reports an identity that names nothing this driver knows. */
	kern_logf("i915: unknown interrupt class=0x%x instance=0x%x intr=0x%x\n",
	    engine_class,
	    instance,
	    intr);
	irq->gt_unknown_class++;
}

/* Serves every bit of one GT bank, then acknowledges the bank. */
static void
i915_gt_bank_handler(
	struct i915_irq_dev *irq,
	unsigned bank)
{
	uint32_t intr_dw;
	uint32_t identity;
	unsigned bit;

	/* Reads which sources of the bank are pending. */
	intr_dw = drv_i915_raw_read32(irq->m, GEN11_GT_INTR_DW(bank));
	irq->last_gt_intr_dw[bank] = intr_dw;

	/* Serves each pending source through the identity handshake. */
	for (bit = 0U; bit < 32U; bit++) {
		if ((intr_dw & (1U << bit)) == 0U)
			continue;

		identity = i915_gt_engine_identity(irq, bank, bit);
		i915_gt_identity_handler(irq, identity);
	}

	/* Clears the bank only after every shared identity has been served. */
	drv_i915_raw_write32(irq->m, GEN11_GT_INTR_DW(bank), intr_dw);

	/* Counts a bank that actually had something to acknowledge. */
	if (intr_dw != 0U)
		irq->gt_bank_acks[bank]++;
}

/*
 * Serves one interrupt; this is gen11_irq_handler(), and the EOI is sent on every path.
 */
static void
i915_irq_handler_body(
	int vector,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	struct i915_irq_dev *irq;
	uint32_t master_ctl;
	uint32_t gu_misc_iir;

	UNUSED_PARAMETER(vector);

	/* The handler was attached with the interrupt device as its argument. */
	irq = argument;
	irq->irq_count++;

	/* Interrupts are not enabled: the message is not ours to serve. */
	if (irq->irqs_enabled == 0) {
		irq->irq_none_count++;
		hal_irq_send_eoi(acknowledge);
		return;
	}

	/* Turns the master control off and samples what is pending. */
	master_ctl = i915_master_intr_disable(irq);
	irq->last_master_ctl = master_ctl;

	/* Nothing is pending: a spurious message, and the master control reopens. */
	if (master_ctl == 0U) {
		i915_master_intr_enable(irq);
		irq->irq_none_count++;
		hal_irq_send_eoi(acknowledge);
		return;
	}

	/* Serves the GT banks the master control reports. */
	if ((master_ctl & (GEN11_GT_DW_IRQ(0) | GEN11_GT_DW_IRQ(1))) != 0U) {
		irq->gt_irq_count++;
		drv_i915_gen11_gt_irq_handler(irq, master_ctl);
	}

	/*
	 * Serves the display sources (gen11_display_irq_handler()).
	 *
	 * Without a display table the display sources are counted but
	 * not acknowledged.
	 */
	if ((master_ctl & GEN11_DISPLAY_IRQ) != 0U) {
		irq->display_irq_count++;
		if (irq->display_ops != NULL)
			irq->display_ops->handle(irq->display_context, master_ctl);
	}

	/* Reads and clears GU_MISC while the master control is off (gen11_gu_misc_irq_ack()). */
	gu_misc_iir = 0U;
	if ((master_ctl & GEN11_GU_MISC_IRQ) != 0U) {
		gu_misc_iir = drv_i915_read32(irq->m, GEN11_GU_MISC_IIR);
		if (gu_misc_iir != 0U)
			drv_i915_write32(irq->m, GEN11_GU_MISC_IIR, gu_misc_iir);
	}

	/* Keeps what GU_MISC reported for the diagnostics. */
	irq->last_gu_misc_iir = gu_misc_iir;

	/* Lets the device raise interrupts again. */
	i915_master_intr_enable(irq);

	/* Hands a graphics system event to the OpRegion (gen11_gu_misc_irq_handler()). */
	if ((gu_misc_iir & GEN11_GU_MISC_GSE) != 0U) {
		irq->gse_count++;
		if (irq->display_ops != NULL)
			irq->display_ops->gse(irq->display_context);
	}

	/* Finishes the interrupt. */
	irq->irq_handled_count++;
	hal_irq_send_eoi(acknowledge);
}

/* Brackets each handler invocation by the counts drv_i915_synchronize_irq() waits on. */
static void
i915_irq_handler(
	int vector,
	hal_irq_ack_t acknowledge,
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
