/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A sandboxed process's set of calls (ws168-p002; uapi/sandbox.h): made
 * when sandbox_spawn creates the process, checked at the head of every
 * system call's dispatch, freed with the process.
 */

#ifndef KERN_KERN_SANDBOX_H
#define KERN_KERN_SANDBOX_H

#include <stddef.h>
#include <stdint.h>

/* The call numbers the set covers (one bit each); a larger number is never allowed. */
#define SANDBOX_CALLS_MAX	256U

/* The denials logged for one process, at most. */
#define SANDBOX_DENIALS_LOGGED	8U

struct process;

/*
 * A process's sandbox: the request's flags and allow bits, the calls
 * allowed (a bitmap by number), and the denials logged so far.
 */
struct sandbox {
	uint32_t flags;
	uint64_t allow;
	uint8_t calls[SANDBOX_CALLS_MAX / 8U];
	unsigned denials;
};

struct sandbox *sandbox_create(uint32_t flags, uint64_t allow);
void sandbox_free(struct sandbox *sandbox);
int sandbox_permits(const struct sandbox *sandbox, uint32_t number, const uintptr_t args[6]);
int sandbox_answers(uint32_t number, const uintptr_t args[6], intptr_t *result);
intptr_t sandbox_deny(struct process *process, uint32_t number);

#endif
