/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS floating-point unit, switched lazily between the contexts
 * that use it.
 */

#ifndef KERN_HAL_MIPS_FPU_H
#define KERN_HAL_MIPS_FPU_H

#include <hal/hal.h>

void
mips_fpu_save(
	struct hal_fpregs *state);

void
mips_fpu_restore(
	const struct hal_fpregs *state);

void
mips_fpu_init(
	void);

bool
mips_fpu_usable(
	void);

void
mips_fpu_select(
	const void *context,
	struct hal_fpregs *state);

bool
mips_fpu_owned_by(
	const void *context);

bool
mips_fpu_take(
	void);

void
mips_fpu_sync(
	const void *context);

void
mips_fpu_forget(
	const void *context);

uint32_t
mips_fpu_clear_cause(
	void);

#endif
