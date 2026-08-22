/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SETJMP_H
#define ZEDBSD_SETJMP_H

typedef struct {
	void *context[5];
	int result;
} jmp_buf[1];

#define setjmp(environment) \
	(__builtin_setjmp((environment)[0].context) ? \
	(environment)[0].result : 0)
void longjmp(jmp_buf, int) __attribute__((noreturn));

#endif
