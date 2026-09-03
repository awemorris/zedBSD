/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 descriptor-table and task-state-segment contract.
 */

#ifndef ZEDBSD_HAL_AMD64_DESCRIPTOR_H
#define ZEDBSD_HAL_AMD64_DESCRIPTOR_H

#include <hal/types.h>

void
amd64_descriptor_init(void);

void
amd64_set_tss_rsp0(
	uintptr_t stack_top);

void
amd64_load_gdt(
	const void *descriptor);

#endif
