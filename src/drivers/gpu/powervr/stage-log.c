/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one stage record of the boot: the driver's paths note
 * a stage's ok or failure here, the line goes to the console through
 * kern_logf, and hw.gpu.powervr.stages reads the record.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sysctl.h>

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/stage.h"

static size_t stages_read(char *text, size_t capacity);

/*
 * The record of the boot and the lock around it, taken from any thread
 * (never in an interrupt) with interrupts off: a holder preempted by the
 * clock tick could resume on another CPU and trap in its release, or be
 * met by another thread's acquisition on the same CPU.
 */
static struct powervr_stage_record stage_record;
static struct spinlock stage_lock;

/*
 * Readies the record and installs hw.gpu.powervr.stages; called once on
 * the boot thread while the driver attaches.
 */
void
powervr_stage_init(void)
{
	/* The lock, then the sysctl that takes it. */
	spin_init(&stage_lock, LOCK_RANK_DEVICE, "powervr stages");
	kern_gpu_stages_set(stages_read);
}

/* Notes that stage succeeded, printing its line the first time. */
void
powervr_stage_ok(
	enum powervr_stage stage)
{
	char line[POWERVR_STAGE_LINE_MAX];
	unsigned long irq;
	bool print;

	/* Notes it under the lock; prints after. */
	irq = spin_lock_irqsave(&stage_lock);

	print = powervr_stage_note_ok(&stage_record, stage, line, sizeof(line));

	spin_unlock_irqrestore(&stage_lock, irq);

	/* The line, outside the lock (a release image says only what failed). */
	if (print && POWERVR_DEBUG)
		kern_logf("%s\n", line);
}

/* Notes that stage failed for reason, printing its line while under the limit. */
void
powervr_stage_fail(
	enum powervr_stage stage,
	enum powervr_stage_reason reason)
{
	char line[POWERVR_STAGE_LINE_MAX];
	unsigned long irq;
	bool print;

	/* Notes it under the lock; prints after. */
	irq = spin_lock_irqsave(&stage_lock);

	print = powervr_stage_note_fail(&stage_record, stage, reason, line, sizeof(line));

	spin_unlock_irqrestore(&stage_lock, irq);

	/* The line, outside the lock. */
	if (print)
		kern_logf("%s\n", line);
}

/* Notes that stage failed for reason with the value it carries (a fault's address, a reset's reason). */
void
powervr_stage_fail_value(
	enum powervr_stage stage,
	enum powervr_stage_reason reason,
	uint64_t value)
{
	char line[POWERVR_STAGE_LINE_MAX];
	unsigned long irq;
	bool print;

	/* Notes it under the lock; prints after. */
	irq = spin_lock_irqsave(&stage_lock);

	print = powervr_stage_note_fail_value(&stage_record, stage, reason, value, line, sizeof(line));

	spin_unlock_irqrestore(&stage_lock, irq);

	/* The line, outside the lock. */
	if (print)
		kern_logf("%s\n", line);
}

/* The text of hw.gpu.powervr.stages. */
static size_t
stages_read(
	char *text,
	size_t capacity)
{
	unsigned long irq;
	size_t length;

	/* Writes it under the lock. */
	irq = spin_lock_irqsave(&stage_lock);

	length = powervr_stage_text(&stage_record, text, capacity);

	spin_unlock_irqrestore(&stage_lock, irq);
	return length;
}
