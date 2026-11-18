/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shader ISAs the GHCI driver compiles for (isa.c): the compiler a
 * device takes, and the options its capabilities give every compile.
 */

#ifndef DRIVERS_GPU_GHCI_COMPILER_GHCI_ISA_H
#define DRIVERS_GPU_GHCI_COMPILER_GHCI_ISA_H

#include "gsi1-compile.h"

#include <stddef.h>
#include <stdint.h>

struct ghci_caps;

/*
 * One shader ISA the driver compiles for: its FourCC, its name for the
 * log, and its compiler.  The table lives as long as the kernel.
 */
struct ghci_isa_backend {
	uint32_t isa_id;
	const char *name;
	int (*compile)(const uint32_t *words, size_t word_count, enum drv_gpu_shader_stage stage, const struct ghci_gsi1_options *options, struct ghci_gsi1_program **program, struct ghci_gsi1_diagnostic *diagnostic);
	void (*program_free)(struct ghci_gsi1_program *program);
};

/* Reports the compiler of a device, or NULL for a device without a shader unit that runs GSI1. */
const struct ghci_isa_backend *ghci_isa_select(const struct ghci_caps *caps);

/* Reports whether the device runs GSI1 natively (NATIVE_ISA_ID), rather than translated by its command processor. */
int ghci_isa_native(const struct ghci_caps *caps);

/* Fills the options a device's capabilities give every compile; the pipeline adds its own. */
void ghci_isa_options(const struct ghci_caps *caps, struct ghci_gsi1_options *options);

#endif /* DRIVERS_GPU_GHCI_COMPILER_GHCI_ISA_H */
