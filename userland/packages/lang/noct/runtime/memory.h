/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland memory interface.
 */

#ifndef ZEDBSD_USER_NOCT_MEMORY_H
#define ZEDBSD_USER_NOCT_MEMORY_H

#include <stddef.h>
#include <stdint.h>

struct user_noct_memory_profile {
	uint64_t capacity_bytes;
	uint64_t available_bytes;
	uint32_t profile_mib;
	size_t arena_size;
	size_t jit_code_size;
	size_t gc_nursery_size;
	size_t gc_graduate_size;
	size_t gc_tenure_size;
};

int user_noct_select_memory(uint64_t capacity_bytes, uint64_t available_bytes,
			    struct user_noct_memory_profile *profile);

#endif
