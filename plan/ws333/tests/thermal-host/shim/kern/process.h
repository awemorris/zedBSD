/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test's stand-in for the kernel's process header: only the
 * lookup and the release the thermal guard uses.  The test models init.
 */

#ifndef KERN_KERN_PROCESS_H
#define KERN_KERN_PROCESS_H

/* A process as the test models it. */
struct process {
	int pid;
};

struct process *
process_find_ref(
	int pid);

void
process_release(
	struct process *process);

#endif
