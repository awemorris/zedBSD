/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sandbox_spawn (ws168-p002; plan/ws168/preview-sandbox-design.md section 3).
 *
 * The parent asks for a child that runs a static image in a sandbox from
 * its first instruction: a new address space, only the files the request
 * maps (the parent's descriptor from, as the child's number to), no root
 * and no working directory, the caller's credentials without the image's
 * set-ID bits, default signals, the smaller of the parent's limits and
 * the request's, and only the system calls of the base set and the
 * allow bits.  A call outside the set ends the child with SIGKILL (or
 * answers EPERM with SANDBOX_SPAWN_DENY_ERRNO, for tests).
 *
 * The request is versioned by its size: a kernel that knows a larger
 * structure takes a smaller one of at least the first version, and a
 * kernel that knows a smaller one takes a larger one only when the bytes
 * it does not know are all zero (E2BIG otherwise).  An unknown flag or
 * allow bit is refused (EINVAL): an old kernel never runs a child in a
 * weaker sandbox than asked.
 */

#ifndef KERN_UAPI_SANDBOX_H
#define KERN_UAPI_SANDBOX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* The kinds of calls added to the base set: threads within the child. */
#define SANDBOX_ALLOW_THREADS		(UINT64_C(1) << 0)
#define SANDBOX_ALLOW_KNOWN		SANDBOX_ALLOW_THREADS

/* The request's flags: a call outside the set answers EPERM instead of ending the child (tests). */
#define SANDBOX_SPAWN_DENY_ERRNO	(UINT32_C(1) << 0)
#define SANDBOX_SPAWN_KNOWN		SANDBOX_SPAWN_DENY_ERRNO

/* The most files handed to the child, numbered 0 to 15 there. */
#define SANDBOX_FD_MAX			16U

/* The largest request the kernel reads (a larger one is E2BIG). */
#define SANDBOX_SPAWN_SIZE_MAX		4096U

/* One file handed over: the parent's descriptor, and its number in the child. */
struct sandbox_fd {
	int32_t from;
	int32_t to;
};

/*
 * The request: its size (the version), the flags, the allow bits, the
 * image (a descriptor of a static ELF the caller may execute), the files
 * (fd_count entries at the user address fds), the NULL-ended argument
 * vector (at the user address argv), and the limits (0: the parent's):
 * the address space and the written file size in bytes, the CPU time in
 * seconds.
 */
struct sandbox_spawn {
	uint32_t size;
	uint32_t flags;
	uint64_t allow;
	int32_t image;
	uint32_t fd_count;
	uint64_t fds;
	uint64_t argv;
	uint64_t memory_max;
	uint64_t cpu_seconds;
	uint64_t write_max;
};

/* The size of the first version. */
#define SANDBOX_SPAWN_SIZE_V1		64U

/* The layouts every program and kernel agree on. */
_Static_assert(sizeof(struct sandbox_spawn) == SANDBOX_SPAWN_SIZE_V1, "the first version of the request is 64 bytes");
_Static_assert(sizeof(struct sandbox_fd) == 8U, "a file mapping is 8 bytes");

#ifdef __cplusplus
}
#endif

#endif
