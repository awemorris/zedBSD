/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_SETJMP_H
#define LIBC_SETJMP_H

#include <features.h>
#include <signal.h>

/*
 * The size of the resumable state.
 *
 * Most targets let the compiler record it through __builtin_setjmp,
 * whose buffer is five words. clang has no such builtin for AArch64, so
 * that target saves the callee-saved registers itself: x19 through x28,
 * the frame pointer, the link register, the stack pointer, and the low
 * halves of v8 to v15.
 */
#if defined(__aarch64__)
#define __ZED_JMP_BUF_WORDS 21
#else
#define __ZED_JMP_BUF_WORDS 5
#endif

typedef struct {
	void *context[__ZED_JMP_BUF_WORDS];
	int result;
} jmp_buf[1];

#if defined(__aarch64__)
int __zed_setjmp(void *context);

#define setjmp(environment)                                                    \
	(__zed_setjmp((environment)[0].context) ? (environment)[0].result      \
						: 0)
#else
#define setjmp(environment)                                                    \
	(__builtin_setjmp((environment)[0].context) ? (environment)[0].result  \
						    : 0)
#endif
void longjmp(jmp_buf, int) __attribute__((__noreturn__));

typedef struct {
	jmp_buf jump;
	sigset_t mask;
	int save_mask;
} sigjmp_buf[1];

#define sigsetjmp(environment, save)                                           \
	(((environment)[0].save_mask = (save) != 0),                           \
	 (void)((environment)[0].save_mask &&                                  \
		sigprocmask(SIG_SETMASK, NULL, &(environment)[0].mask)),       \
	 setjmp((environment)[0].jump))

void siglongjmp(sigjmp_buf, int) __attribute__((__noreturn__));

#if __ZEDBSD_LEGACY_VISIBLE
#define _setjmp(environment) setjmp(environment)
void _longjmp(jmp_buf, int) __attribute__((__noreturn__));
#endif

#endif
