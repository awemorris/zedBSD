/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SETJMP_H
#define ZEDBSD_SETJMP_H

#include <zedbsd/features.h>
#include <signal.h>

typedef struct {
	void *context[5];
	int result;
} jmp_buf[1];

#define setjmp(environment)                                                    \
	(__builtin_setjmp((environment)[0].context) ? (environment)[0].result  \
						    : 0)
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
