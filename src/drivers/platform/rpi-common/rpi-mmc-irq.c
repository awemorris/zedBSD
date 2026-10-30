/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 SD controllers' shared interrupt line.
 *
 * The dispatcher is the kernel's one handler of GIC SPI 126.  It holds two
 * slots, one per SD controller, and calls every filled slot for each
 * interrupt; a controller's handler reports whether the interrupt was its
 * own.  The line is level triggered, so a handler that claims an interrupt
 * has already stopped its controller from raising it.
 *
 * The slots are read and changed only under dispatch_lock, so a slot that
 * removal has emptied is never called afterwards.  Lock order: this lock,
 * then a controller's own interrupt lock; a controller never takes this one.
 */

#include "drivers/platform/rpi4/rpi4-mmc-irq.h"
#include <kern/atomic.h>
#include <kern/clock.h>
#include <kern/irq.h>
#include <kern/lock.h>
#include <uapi/errno.h>

/*
 * The steps of installing the dispatcher on the line.
 *
 * ABSENT: not installed, or the last attempt failed.  PREPARING: one caller
 * is installing it.  READY: installed on line_irq, which never changes.
 */
#define LINE_STATE_ABSENT		0U
#define LINE_STATE_PREPARING		1U
#define LINE_STATE_READY		2U

/* How long a caller waits between looks at another caller's installation. */
#define LINE_PREPARE_POLL_US		10U
#define LINE_PREPARE_POLL_MAX_US	20U

/*
 * How many interrupts in a row no handler may claim before the line is
 * masked.  A level-triggered line nobody serves would otherwise interrupt
 * CPU 0 without end; the next handler added unmasks it.
 */
#define UNCLAIMED_LIMIT			100000U

/*
 * One controller's place on the line.
 *
 * An empty slot has no handler.
 */
struct mmc_irq_slot {
	drv_rpi4_mmc_irq_fn handler;
	void *argument;
};

/*
 * How far the dispatcher's installation has come: one of LINE_STATE_*.
 *
 * The caller that moves it from ABSENT to PREPARING with a
 * compare-and-exchange is the only one that installs the dispatcher and
 * fills line_irq; it then stores READY with release.  A failed installation
 * stores ABSENT again, so a later caller may retry.
 */
static volatile unsigned line_state;

/*
 * The line the dispatcher is installed on.
 *
 * Filled once, before line_state becomes READY, and read only after
 * line_state has been seen READY with an acquire load.
 */
static int line_irq;

/*
 * The controllers' handlers.
 *
 * Protected by dispatch_lock; zero means no handler.  They live for as long
 * as the kernel runs.
 */
static struct mmc_irq_slot slots[DRV_RPI4_MMC_IRQ_SLOTS];

/*
 * How many interrupts in a row no handler has claimed, and whether that
 * count reached UNCLAIMED_LIMIT and the line was masked.
 *
 * Protected by dispatch_lock.  A claimed interrupt sets the count back to
 * zero; adding a handler unmasks a masked line.
 */
static unsigned unclaimed_count;
static unsigned line_masked;

/*
 * Serializes the slots against the interrupt.
 *
 * Taken with interrupts held off.  A controller's interrupt lock may be
 * taken inside it, by the handler the dispatcher calls.
 */
static struct spinlock dispatch_lock = {
	{ 0 }, LOCK_RANK_DEVICE, "rpi4 mmc irq", 0, 0
};

static int install_dispatcher(int irq);
static void dispatch(int irq, kern_irq_ack_t acknowledge, void *argument);

/*
 * Adds a controller's handler to the shared line.
 */
int
drv_rpi4_mmc_irq_add(
	int irq,
	drv_rpi4_mmc_irq_fn handler,
	void *argument)
{
	unsigned long enabled;
	unsigned free_slot;
	unsigned i;
	int unmask;
	int error;

	/* Refuses a missing handler or a line number the kernel cannot name. */
	if (handler == NULL)
		return EINVAL;
	if (irq < 0)
		return EINVAL;

	/* Installs the dispatcher on the line, or finds it installed. */
	error = install_dispatcher(irq);
	if (error != 0)
		return error;

	/* Fills a free slot, refusing a second add of the same handler. */
	free_slot = DRV_RPI4_MMC_IRQ_SLOTS;
	unmask = 0;
	enabled = spin_lock_irqsave(&dispatch_lock);

	for (i = 0; i < DRV_RPI4_MMC_IRQ_SLOTS; i++) {
		/* Remembers the first free slot. */
		if (slots[i].handler == NULL) {
			if (free_slot == DRV_RPI4_MMC_IRQ_SLOTS)
				free_slot = i;
			continue;
		}

		/* Refuses the same controller twice. */
		if (slots[i].handler == handler && slots[i].argument == argument) {
			spin_unlock_irqrestore(&dispatch_lock, enabled);

			return EEXIST;
		}
	}

	/* Refuses a third controller. */
	if (free_slot == DRV_RPI4_MMC_IRQ_SLOTS) {
		spin_unlock_irqrestore(&dispatch_lock, enabled);

		return EBUSY;
	}

	slots[free_slot].handler = handler;
	slots[free_slot].argument = argument;

	/*
	 * A new handler gives the line another chance: a masked line is
	 * unmasked, and the run of unclaimed interrupts starts again.  The
	 * installation leaves the line masked, so the first add unmasks it.
	 */
	if (line_masked != 0) {
		line_masked = 0;
		unmask = 1;
	}
	unclaimed_count = 0;

	spin_unlock_irqrestore(&dispatch_lock, enabled);

	/* Lets the line interrupt again, now that a handler serves it. */
	if (unmask)
		kern_irq_unmask(irq);

	/* Succeeded: the handler is asked about every interrupt of the line. */
	return 0;
}

/*
 * Removes a controller's handler from the shared line.
 */
int
drv_rpi4_mmc_irq_remove(
	int irq,
	drv_rpi4_mmc_irq_fn handler,
	void *argument)
{
	unsigned long enabled;
	unsigned state;
	unsigned i;

	/* Refuses a line the dispatcher was never installed on. */
	state = atomic_raw_load_acquire(&line_state);
	if (state != LINE_STATE_READY)
		return EINVAL;
	if (irq != line_irq)
		return EINVAL;

	/* Empties the handler's slot; the interrupt cannot be inside it meanwhile. */
	enabled = spin_lock_irqsave(&dispatch_lock);

	for (i = 0; i < DRV_RPI4_MMC_IRQ_SLOTS; i++) {
		/* Passes over another controller's slot. */
		if (slots[i].handler != handler)
			continue;
		if (slots[i].argument != argument)
			continue;

		slots[i].handler = NULL;
		slots[i].argument = NULL;

		spin_unlock_irqrestore(&dispatch_lock, enabled);

		/* Succeeded: the handler is not called again. */
		return 0;
	}

	spin_unlock_irqrestore(&dispatch_lock, enabled);

	/* Reports a handler that was not on the line. */
	return ENOENT;
}

/*
 * Installs the dispatcher on the line, once.
 *
 * Several controllers may race to add their handlers; exactly one of them
 * installs the dispatcher and the others wait until it is ready.  The line
 * is left masked: the first handler added unmasks it.
 */
static int
install_dispatcher(
	int irq)
{
	unsigned long enabled;
	unsigned state;
	unsigned expected;
	int claimed;
	int error;

	/*
	 * Takes the installation, or waits while another caller holds it.  The
	 * holder waits on nothing but the interrupt controller, so the wait
	 * ends.
	 */
	for (;;) {
		/* Uses the installed dispatcher, which must serve the same line. */
		state = atomic_raw_load_acquire(&line_state);
		if (state == LINE_STATE_READY) {
			if (irq != line_irq)
				return EINVAL;
			return 0;
		}

		/* Becomes the one caller that installs the dispatcher. */
		expected = LINE_STATE_ABSENT;
		claimed = atomic_raw_compare_exchange(&line_state, &expected, LINE_STATE_PREPARING);
		if (claimed)
			break;

		/* Waits one polling interval for the other caller's installation. */
		kern_usleep_range(LINE_PREPARE_POLL_US, LINE_PREPARE_POLL_MAX_US);
	}

	/* Holds the line off until a handler can serve it. */
	kern_irq_mask(irq);

	/* Both controllers raise the line by level, active high (GIC binding type 4). */
	error = kern_irq_set_mode(irq, KERN_IRQ_TRIGGER_LEVEL, KERN_IRQ_POLARITY_HIGH);
	if (error != 0) {
		atomic_raw_store_release(&line_state, LINE_STATE_ABSENT);
		return error;
	}

	/* Makes the dispatcher the line's one kernel handler. */
	error = kern_irq_register(irq, dispatch, NULL);
	if (error != 0) {
		atomic_raw_store_release(&line_state, LINE_STATE_ABSENT);
		return error;
	}

	/* Records that the line waits masked for its first handler. */
	enabled = spin_lock_irqsave(&dispatch_lock);

	line_masked = 1;
	unclaimed_count = 0;

	spin_unlock_irqrestore(&dispatch_lock, enabled);

	/* READY publishes line_irq: a CPU that loads it with acquire sees it filled. */
	line_irq = irq;
	atomic_raw_store_release(&line_state, LINE_STATE_READY);

	/* Succeeded: the dispatcher serves the line. */
	return 0;
}

/* Asks every controller on the line whether the interrupt was its own. */
static void
dispatch(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	unsigned long enabled;
	unsigned i;
	bool claimed;
	bool mine;
	int mask;

	(void)argument;

	/* Calls each filled slot; both are asked even when the first claims. */
	claimed = false;
	mask = 0;
	enabled = spin_lock_irqsave(&dispatch_lock);

	for (i = 0; i < DRV_RPI4_MMC_IRQ_SLOTS; i++) {
		/* Passes over an empty slot. */
		if (slots[i].handler == NULL)
			continue;

		/* Lets the controller serve and silence its own interrupt. */
		mine = slots[i].handler(slots[i].argument);
		if (mine)
			claimed = true;
	}

	/*
	 * Counts an interrupt nobody claimed, and masks the line when the run
	 * grows so long that the line must be stuck.
	 */
	if (claimed) {
		unclaimed_count = 0;
	} else {
		unclaimed_count++;
		if (unclaimed_count >= UNCLAIMED_LIMIT && line_masked == 0) {
			line_masked = 1;
			mask = 1;
		}
	}

	spin_unlock_irqrestore(&dispatch_lock, enabled);

	/* Holds a stuck line off; the next handler added lets it in again. */
	if (mask)
		kern_irq_mask(irq);

	/* Ends the interrupt at the controller of the GIC. */
	kern_irq_send_eoi(acknowledge);
}
