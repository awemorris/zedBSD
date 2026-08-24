/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SETJMP_H
#define ZEDBSD_SETJMP_H

#include <zedbsd/features.h>

typedef struct {
	void *context[5];
	int result;
} jmp_buf[1];

#define setjmp(environment) \
	(__builtin_setjmp((environment)[0].context) ? \
	(environment)[0].result : 0)
void longjmp(jmp_buf, int) __attribute__((__noreturn__));
#if __ZEDBSD_LEGACY_VISIBLE
#define _setjmp(environment) setjmp(environment)
void _longjmp(jmp_buf, int) __attribute__((__noreturn__));
#endif

#endif
