/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_UAPI_PROCESS_H
#define ZEDBSD_UAPI_PROCESS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* exec argument and environment strings plus their pointer tables share this
 * byte budget.  The vector bound is derived from the budget, so an otherwise
 * valid small-string invocation is not rejected by an unrelated item count. */
#define ZEDBSD_ARG_MAX (16U * 1024U)
#define ZEDBSD_EXEC_VECTOR_MAX (ZEDBSD_ARG_MAX / sizeof(uintptr_t))
#define ZEDBSD_SPAWN_ARG_MAX ZEDBSD_EXEC_VECTOR_MAX
#define ZEDBSD_SPAWN_ENV_MAX ZEDBSD_EXEC_VECTOR_MAX
#define ZEDBSD_SPAWN_STRING_MAX ZEDBSD_ARG_MAX

struct process_times_record {
	uint64_t self_ticks;
	uint64_t child_ticks;
	uint64_t elapsed_ticks;
};


#endif
