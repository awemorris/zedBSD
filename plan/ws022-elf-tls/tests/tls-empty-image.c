/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdint.h>
#include <zedbsd/syscall.h>

intptr_t __syscall6(uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#ifdef TLS_ZERO_ONLY
static __thread volatile unsigned zero_value;
#endif
/* Retain a non-TLS writable LOAD for the zero-only TLS location. */
volatile unsigned image_result;
void _start(void);

void
_start(void)
{
	unsigned status;

	status = 0;
#ifdef TLS_ZERO_ONLY
	if (zero_value != 0)
		status = 1;
	zero_value = 123;
	image_result = zero_value;
#else
	image_result = 123;
#endif
	(void)__syscall6(ZEDBSD_SYS_exit, status, 0, 0, 0, 0, 0);
	for (;;)
		__asm__ volatile("pause");
}
