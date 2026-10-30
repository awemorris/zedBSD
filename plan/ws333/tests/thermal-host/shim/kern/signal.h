/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's signal header: the
 * disposition and the sending the thermal guard uses, with the kernel's
 * numbers, without the host C library's signal types.
 */

#ifndef KERN_KERN_SIGNAL_H
#define KERN_KERN_SIGNAL_H

#include <stdint.h>

struct process;

/* The kernel's number of SIGUSR2 and its two plain dispositions. */
#define SIGUSR2	17
#define SIG_DFL	((void (*)(int))0)
#define SIG_IGN	((void (*)(int))1)

/* One disposition, laid out as the test's model keeps it. */
struct signal_action {
	uintptr_t handler;
	uintptr_t restorer;
	uint64_t mask;
	unsigned flags;
};

int
signal_action_set(
	struct process *process,
	int signo,
	const struct signal_action *requested,
	struct signal_action *previous);

int
signal_send_process(
	struct process *process,
	int signo);

#endif
