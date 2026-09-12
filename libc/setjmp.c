/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compiler-assisted, architecture-neutral nonlocal jumps.
 */

#include <setjmp.h>

#if defined(__aarch64__)
void __zed_longjmp(void *context, int value) __attribute__((__noreturn__));
#endif

void
longjmp(jmp_buf environment, int value)
{
	/* A zero value becomes one, so the jump is always distinguishable. */
	environment[0].result = value == 0 ? 1 : value;
#if defined(__aarch64__)
	__zed_longjmp(environment[0].context, 1);
#else
	__builtin_longjmp(environment[0].context, 1);
#endif
}

void
siglongjmp(sigjmp_buf environment, int value)
{
	if (environment[0].save_mask)
		(void)sigprocmask(SIG_SETMASK, &environment[0].mask, NULL);
	longjmp(environment[0].jump, value);
}

void
_longjmp(jmp_buf environment, int value)
{
	longjmp(environment, value);
}
