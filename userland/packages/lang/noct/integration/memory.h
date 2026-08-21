/*
 * zedBSD Noct memory profile selection
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_NOCT_MEMORY_H
#define ZEDBSD_NOCT_MEMORY_H

#include <stddef.h>
#include <stdint.h>

enum noct_memory_class {
	ZEDBSD_NOCT_MEMORY_5,
	ZEDBSD_NOCT_MEMORY_8,
	ZEDBSD_NOCT_MEMORY_16,
	ZEDBSD_NOCT_MEMORY_32,
	ZEDBSD_NOCT_MEMORY_64,
	ZEDBSD_NOCT_MEMORY_LARGE,
};

struct noct_memory_profile {
	enum noct_memory_class memory_class;
	const char *name;
	uint32_t installed_mib;
	uintptr_t arena_base;
	size_t arena_size;
	size_t source_max;
	size_t repl_source_max;
	size_t gc_nursery_size;
	size_t gc_graduate_size;
	size_t gc_tenure_size;
	size_t jit_code_size;
};

/*
 * low_reserved_bytes is how much of low extended memory, counting from
 * 1 MiB, the resident vmunix high segment occupies; the arena starts
 * above it, rounded up to 64 KiB.
 */
int noct_select_memory(uint32_t low_extended_bytes,
			      uint32_t high_memory_mib,
			      uint32_t low_reserved_bytes,
			      struct noct_memory_profile *profile);

#endif
