/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Supplies the m68k thread pointer to compiled thread-local accesses.
 *
 * The MC68030 has no thread-pointer register, so the m68k compiler reads
 * one by calling __m68k_read_tp and takes the result from A0.  The m68k
 * TLS ABI (variant I) places the static TLS block 0x7000 bytes below the
 * thread pointer.  zedBSD keeps the thread's control block (struct
 * __rtld_tcb, the value thread_self(KERN_THREAD_SELF_GET_TLS) reports) at
 * the start of a KERN_TLS_TCB_RESERVE-byte area and the static TLS block
 * right after that area, so the thread pointer is computed from the
 * control block's address.
 *
 * Only the static user ABI of the X68000 links this.  The loader builds
 * the first thread's mapping in that placement at exec and the C library
 * builds every later thread's (KERN_TLS_VARIANT_I in uapi/tls.h).
 */

#include "syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <uapi/tls.h>

/* The distance from the static TLS block up to the m68k thread pointer. */
#define M68K_TLS_TP_OFFSET	0x7000U

void *__m68k_read_tp(void);

/* Reports the calling thread's m68k thread pointer, or NULL without one. */
void *
__m68k_read_tp(
	void)
{
	intptr_t control;
	uintptr_t pointer;

	/* Asks the kernel where the thread's control block is. */
	control = __syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_GET_TLS,
			     0, 0, 0, 0, 0);
	if (control <= 0)
		return NULL;

	/* The TLS block follows the control block's reserve. */
	pointer = (uintptr_t)control + KERN_TLS_TCB_RESERVE +
		  M68K_TLS_TP_OFFSET;

	/* Reports the thread pointer the compiled accesses offset from. */
	return (void *)pointer;
}
