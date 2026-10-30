/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system sleep to idle, S0ix (ws052-p006, README-v2 §7): the
 * coordinator the sleep request runs, the idle loop's hook, and the notes
 * the drivers leave of what woke the system.
 */

#ifndef KERN_KERN_SLEEP_H
#define KERN_KERN_SLEEP_H

#include <kern/types.h>
#include <stddef.h>
#include <stdint.h>

/* What woke the system (the same numbers as the UAPI's KERN_SYSTEM_WAKE_*). */
#define KERN_SLEEP_WAKE_NONE		0U
#define KERN_SLEEP_WAKE_POWER_BUTTON	1U
#define KERN_SLEEP_WAKE_LID		2U
#define KERN_SLEEP_WAKE_KEYBOARD	3U
#define KERN_SLEEP_WAKE_USB		4U
#define KERN_SLEEP_WAKE_AC		5U
#define KERN_SLEEP_WAKE_TIMER		6U
#define KERN_SLEEP_WAKE_SPURIOUS	7U
#define KERN_SLEEP_WAKE_OTHER		8U

/*
 * A note that is not a wake reason: a battery's news, which the embedded
 * controller raises through the same GPE as a key and which does not wake
 * the system on its own.
 */
#define KERN_SLEEP_NOTE_BATTERY		100U

/* The longest name of a device that refused to suspend. */
#define KERN_SLEEP_DEVICE_MAX		48U

/*
 * The outcome of one sleep to idle: why it could not enter (result, 0
 * when it entered), the first error of the devices' resume, what woke the
 * system, and the device that refused to suspend.
 */
struct kern_sleep_outcome {
	int result;
	int resume_result;
	unsigned wake;
	char device[KERN_SLEEP_DEVICE_MAX];
};

int kern_sleep_supported(void);
void kern_sleep_s0idle(struct kern_sleep_outcome *outcome);
void kern_sleep_idle(kern_cpu_id_t cpu);
void kern_sleep_note_wake(unsigned reason);

#endif
