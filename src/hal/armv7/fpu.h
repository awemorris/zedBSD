/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The lazy switch of the armv7 floating-point and Advanced SIMD unit.
 */

#ifndef KERN_HAL_ARMV7_FPU_H
#define KERN_HAL_ARMV7_FPU_H

#include <hal/hal.h>

/*
 * The floating-point exception register's bits: the exceptional state
 * (EX), the unit's enable (EN) and the asynchronous exception of a
 * vector operation (DEX).
 *
 * Values referenced: ARM DDI 0406C, FPEXC (EX bit 31, EN bit 30, DEX bit
 * 29).
 */
#define ARMV7_FPEXC_EX		0x80000000U
#define ARMV7_FPEXC_EN		0x40000000U
#define ARMV7_FPEXC_DEX		0x20000000U

/* The processor number of a context the unit of no processor holds. */
#define ARMV7_FPU_NOWHERE	(-1)

/*
 * The floating-point registers of one context, a task or a test.
 *
 * regs holds the context's registers whenever the unit of no processor is
 * on for it.  loaded_cpu names the processor whose unit last loaded them,
 * so a context that ran elsewhere since is loaded again; it is
 * ARMV7_FPU_NOWHERE for a context whose registers were replaced or that
 * no unit loaded yet.  A context lives as long as its holder.
 */
struct armv7_fpu_context {
	struct hal_fpregs regs __attribute__((aligned(8)));
	int loaded_cpu;
};

void
armv7_fpu_init(
	void);

int
armv7_fpu_usable(
	void);

unsigned
armv7_fpu_register_count(
	void);

void
armv7_fpu_context_init(
	struct armv7_fpu_context *context);

void
armv7_fpu_select(
	struct armv7_fpu_context *context);

struct armv7_fpu_context *
armv7_fpu_selected(
	void);

struct armv7_fpu_context *
armv7_fpu_owner(
	void);

int
armv7_fpu_take_trap(
	int from_user);

int
armv7_fpu_take_exception(
	void);

void
armv7_fpu_sync(
	struct armv7_fpu_context *context);

void
armv7_fpu_forget(
	struct armv7_fpu_context *context);

uint32_t
armv7_fpu_sanitize_fpscr(
	uint32_t fpscr);

unsigned long
armv7_fpu_load_count(
	void);

uint32_t
armv7_fpu_read_fpexc(
	void);

void
armv7_fpu_write_fpexc(
	uint32_t value);

uint32_t
armv7_fpu_read_fpsid(
	void);

uint32_t
armv7_fpu_read_mvfr0(
	void);

void
armv7_fpu_save(
	struct hal_fpregs *registers,
	uint32_t upper_bank);

void
armv7_fpu_load(
	const struct hal_fpregs *registers,
	uint32_t upper_bank);

#endif
