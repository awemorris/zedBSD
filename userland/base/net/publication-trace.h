/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_NET_PUBLICATION_TRACE_H
#define ZEDBSD_NET_PUBLICATION_TRACE_H

/* Private, compile-time-only stage observations for disposable test images. */
#ifdef ZEDBSD_NCOM_TRACE
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

static inline void
ncom_trace(const char *stage, unsigned opcode)
{
	char message[160];
	int saved = errno;
	int length = snprintf(message, sizeof(message),
	    "[ncom] pid=%ld op=%u stage=%s\n", (long)getpid(), opcode, stage);

	/* Avoid buffered stdio and never change the operation's errno. */
	if (length > 0 && (size_t)length < sizeof(message))
		(void)write(STDERR_FILENO, message, (size_t)length);
	errno = saved;
}
#define NCOM_TRACE(stage, opcode) ncom_trace((stage), (opcode))
#else
#define NCOM_TRACE(stage, opcode) ((void)0)
#endif

#endif
