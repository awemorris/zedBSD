/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register model of the hotplug path, for the model tests.
 *
 * It defines the checkpoints the production hotplug code calls for a
 * model instance: the reads of the hotplug status and control registers
 * and of a GMBUS controller whose DDC bus has a sink at 0x50 (or nobody),
 * the GMBUS writes, and the write-back of a read-modify-write, where the
 * status fields (bits 0-1 of each nibble) are write-1-to-clear as on the
 * hardware.  It also delivers synthetic interrupts and runs the storm
 * re-enable work on demand.
 */

#include "../../display/hotplug-internal.h"
#include "../../display/internal.h"
#include "hpd-model.h"

#include <kern/lock.h>

#include <uapi/errno.h>
#include <stddef.h>

/* The hotplug and GMBUS registers of the south display the model answers. */
#define I915_TEST_HPD_SDEISR		0xc4000u
#define I915_TEST_HPD_SHOTPLUG_CTL_DDI	0xc4030u
#define I915_TEST_HPD_SHOTPLUG_CTL_TC	0xc4034u
#define I915_TEST_HPD_GMBUS0		0xc5100u
#define I915_TEST_HPD_GMBUS1		0xc5104u
#define I915_TEST_HPD_GMBUS2		0xc5108u
#define I915_TEST_HPD_GMBUS3		0xc510cu
#define I915_TEST_HPD_GMBUS5		0xc5120u

/* The status fields of a hotplug control register: bits 0-1 of each nibble, write-1-to-clear. */
#define I915_TEST_HPD_STATUS_FIELDS	0x33333333u

/* The sink address the model's DDC bus answers at. */
#define I915_TEST_HPD_DDC_ADDRESS	0x50u

/*
 * The lock a synthetic interrupt runs under.
 *
 * Taken with interrupts disabled, as the Linux handler runs: a thread that
 * takes the irq_lock with a plain spin_lock() and is then interrupted on
 * the same CPU would deadlock against it.  Initialised on first use (live
 * says so) and kept for the boot.
 */
static struct spinlock i915_test_hpd_irq_lock;
static int i915_test_hpd_irq_lock_live;

u32 drv_i915_hpd_model_read(struct i915_hpd_fake_regs *fake, u32 reg);
void drv_i915_hpd_model_write(struct i915_hpd_fake_regs *fake, u32 reg, u32 val);
void drv_i915_hpd_model_rmw_write(struct i915_hpd_fake_regs *fake, u32 reg, u32 val);

/*
 * Answers a read of a model's registers: the hotplug registers, the GMBUS
 * status and data, and the other GMBUS registers as written.
 */
u32
drv_i915_hpd_model_read(
	struct i915_hpd_fake_regs *fake,
	u32 reg)
{
	u32 value;
	u32 byte;

	/* One answer per register. */
	switch (reg) {
	case I915_TEST_HPD_SDEISR:
		return fake->sdeisr;
	case I915_TEST_HPD_SHOTPLUG_CTL_DDI:
		return fake->shotplug_ddi;
	case I915_TEST_HPD_SHOTPLUG_CTL_TC:
		return fake->shotplug_tc;
	case I915_TEST_HPD_GMBUS2:
		/* A NAK, or ready and in the wait phase, active while bytes remain. */
		if (fake->gm_nak)
			return GMBUS_SATOER;

		value = GMBUS_HW_RDY | GMBUS_HW_WAIT_PHASE;
		if (fake->gm_active && fake->gm_left != 0u)
			value |= GMBUS_ACTIVE;
		return value;
	case I915_TEST_HPD_GMBUS3:
		/* The next four bytes of the sink's EDID. */
		value = 0u;
		for (byte = 0u; byte < 4u && fake->gm_left != 0u; byte++) {
			if (fake->ddc_edid != NULL && fake->gm_ptr < fake->ddc_edid_len)
				value |= (u32)fake->ddc_edid[fake->gm_ptr] << (8u * byte);

			fake->gm_left--;
			fake->gm_ptr++;
		}

		fake->gm_reads++;
		return value;
	default:
		break;
	}

	/* The other GMBUS registers answer what was written. */
	if (reg >= I915_TEST_HPD_GMBUS0 && reg <= I915_TEST_HPD_GMBUS5)
		return fake->gmbus[(reg - I915_TEST_HPD_GMBUS0) / 4u % 6u];

	/* Anything else reads 0. */
	return 0u;
}

/*
 * Takes a write into a model's GMBUS controller: a sink at 0x50 answers
 * index reads of its EDID.
 */
void
drv_i915_hpd_model_write(
	struct i915_hpd_fake_regs *fake,
	u32 reg,
	u32 val)
{
	u32 cycle;
	u32 addr;

	/* A GMBUS1 write clears the controller or starts a cycle; a GMBUS3 write outside a read sets the offset. */
	if (reg == I915_TEST_HPD_GMBUS1) {
		cycle = (val >> 25) & 7u;
		addr = (val >> GMBUS_SLAVE_ADDR_SHIFT) & 0x7fu;

		/* The software clear resets the NAK and ends the cycle. */
		if (val & GMBUS_SW_CLR_INT) {
			fake->gm_nak = 0;
			fake->gm_active = 0;
			return;
		}

		/* A write without software ready starts nothing. */
		if (!(val & GMBUS_SW_RDY))
			return;

		/* A STOP cycle ends the transfer. */
		if (cycle & 4u) {
			fake->gm_active = 0;
			return;
		}

		/* Nobody but a present sink at 0x50 answers. */
		if (!fake->ddc_present || addr != I915_TEST_HPD_DDC_ADDRESS) {
			fake->gm_nak = 1;
			fake->gm_naks++;
			return;
		}

		/* An INDEX (| WAIT) cycle carries the offset byte. */
		if (cycle & 2u)
			fake->gm_ptr = (val >> GMBUS_SLAVE_INDEX_SHIFT) & 0xffu;

		/* A read cycle has its byte count left to deliver. */
		fake->gm_left = 0u;
		if (val & GMBUS_SLAVE_READ)
			fake->gm_left = (val >> GMBUS_BYTE_COUNT_SHIFT) & 0x1ffu;

		fake->gm_active = 1;
	} else if (reg == I915_TEST_HPD_GMBUS3 && !(fake->gmbus[1] & GMBUS_SLAVE_READ)) {
		/* A written offset. */
		fake->gm_ptr = val & 0xffu;
	}

	/* Keeps the value of every GMBUS register. */
	if (reg >= I915_TEST_HPD_GMBUS0 && reg <= I915_TEST_HPD_GMBUS5)
		fake->gmbus[(reg - I915_TEST_HPD_GMBUS0) / 4u % 6u] = val;
}

/*
 * Takes the write-back of a read-modify-write: the status fields are
 * write-1-to-clear, so a status written back as read clears itself.
 */
void
drv_i915_hpd_model_rmw_write(
	struct i915_hpd_fake_regs *fake,
	u32 reg,
	u32 val)
{
	/* Counts the write-back and keeps its value. */
	fake->rmw_writes++;
	fake->last_rmw_write = val;

	/* The hotplug control registers clear the status bits written as 1. */
	if (reg == I915_TEST_HPD_SHOTPLUG_CTL_DDI) {
		fake->shotplug_ddi = val & ~(val & I915_TEST_HPD_STATUS_FIELDS);
	} else if (reg == I915_TEST_HPD_SHOTPLUG_CTL_TC) {
		fake->shotplug_tc = val & ~(val & I915_TEST_HPD_STATUS_FIELDS);
	}
}

/*
 * Delivers a synthetic SDE interrupt to a running model instance.
 *
 * It enters the Linux handler with interrupts disabled, as the hardware
 * interrupt does; an instance on the MMIO BAR takes none.
 */
void
drv_i915_test_hpd_model_irq(
	struct i915_display *display,
	uint32_t sde_iir)
{
	struct i915_hpd_world *world;
	unsigned long enabled;

	/* Only a running model instance takes synthetic interrupts. */
	world = display->hpd_world;
	if (world == NULL)
		return;
	if (world->hpd.fake == NULL)
		return;

	/* Prepares the lock on first use. */
	if (!i915_test_hpd_irq_lock_live) {
		spin_init(&i915_test_hpd_irq_lock, LOCK_RANK_DEVICE, "hpd-model-irq");
		i915_test_hpd_irq_lock_live = 1;
	}

	/* Enters the Linux handler with interrupts disabled. */
	enabled = spin_lock_irqsave(&i915_test_hpd_irq_lock);

	drv_i915_hpd_icp_entry(world, sde_iir);

	spin_unlock_irqrestore(&i915_test_hpd_irq_lock, enabled);
}

/*
 * Reports whether a model instance may run on a display.
 *
 * Returns 0 once a hardware instance ran on it in this boot: the model
 * tests must not take over the device's hotplug state.
 */
int
drv_i915_test_hpd_model_allowed(
	struct i915_display *display)
{
	struct i915_hpd_world *world;

	/* A display without a world has run no hardware instance. */
	world = display->hpd_world;
	if (world == NULL)
		return 1;

	/* A hardware instance forbids the models for the rest of the boot. */
	if (world->i915_hpd_real_seen)
		return 0;

	/* No hardware instance has run. */
	return 1;
}

/*
 * Runs the armed storm re-enable work now (it is armed for
 * HPD_STORM_REENABLE_DELAY).
 *
 * Returns 1 when the work was armed, pending or running and has now run,
 * 0 when it was idle, or ENOENT when it was never armed.
 */
int
drv_i915_test_hpd_flush_reenable(
	struct i915_display *display)
{
	struct i915_hpd_world *world;
	struct delayed_work *delayed;
	uint64_t deadline;
	int disarmed;
	int flushed;

	/* A work never armed has no queue to be flushed on. */
	world = display->hpd_world;
	if (world == NULL)
		return ENOENT;

	delayed = &world->hpd_i915.display.hotplug.reenable_work;
	if (delayed->work.q == NULL)
		return ENOENT;

	/* Gives the work a bounded time to finish. */
	deadline = drv_i915_hpd_sync_deadline();

	/*
	 * Takes the work off its timer and hands it to the work queue at once.
	 * The driver's timer queue has no flush of its own; the cancel also
	 * takes a work already pending out of the work queue, so that work is
	 * queued again at the tail instead of running from its place.  It runs
	 * once either way.
	 */
	disarmed = drv_i915_delayed_cancel(&delayed->work.q->tq, &delayed->dw);
	if (disarmed != 0)
		(void)drv_i915_queue_work(&delayed->work.q->wq, &delayed->dw.work);

	/* Waits until the work has run. */
	flushed = drv_i915_flush_work(&delayed->work.q->wq, &delayed->dw.work, deadline);
	if (flushed != 0)
		return 1;
	if (disarmed != 0)
		return 1;

	/* The work was idle and nothing ran. */
	return 0;
}
