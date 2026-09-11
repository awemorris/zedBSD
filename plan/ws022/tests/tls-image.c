/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>

intptr_t __syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* Compiler-emitted local-exec TLS: values, zero-fill, and over-alignment. */
__thread volatile unsigned tls_initialized = 0x13579bdfU;
__thread volatile unsigned tls_zero;
__thread unsigned char tls_aligned[65] __attribute__((aligned(64)));
volatile unsigned tls_result;
void *volatile tls_address;

void _start(void);

void
_start(void)
{
	uintptr_t tp;
	unsigned status;
	static const char pass[] = "TLS-INITIAL PASS\n";

	tp = (uintptr_t)__builtin_thread_pointer();
	status = 0;
	if (tp == 0)
		status = 1;
	if (tp != (uintptr_t)__syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0))
		status = 2;
	if (tls_initialized != 0x13579bdfU || tls_zero != 0)
		status = 3;
	tls_address = tls_aligned;
	if (((uintptr_t)tls_address & 63U) != 0)
		status = 4;
	tls_result = tls_initialized + tls_zero;
	tls_address = tls_aligned;
	tls_zero = 17U;
	if (status == 0)
		(void)__syscall6(ZEDBSD_SYS_write, 1, (uintptr_t)pass, sizeof(pass) - 1U, 0, 0, 0);
	(void)__syscall6(ZEDBSD_SYS_exit, status, 0, 0, 0, 0, 0);
	for (;;)
		__asm__ volatile("pause");
}
