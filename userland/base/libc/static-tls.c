/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library static tls support.
 */

#include "userland/base/libc/syscall.h"
#include <zedbsd/rtld-abi.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define STATIC_TCB_MAPPING_SIZE 4096U

/*
 * Implements the rtld thread alloc operation.
 */
int
__rtld_thread_alloc(
	void *pthread_private,
	struct __rtld_tcb **out)
{
	struct __rtld_tcb *tcb;

	/* Handles the out availability. */
	if (out == NULL)
		return -1;
	*out = NULL;
	tcb = mmap(NULL, STATIC_TCB_MAPPING_SIZE, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	/* Handles an operation failure. */
	if (tcb == MAP_FAILED)
		return -1;
	memset(tcb, 0, sizeof(*tcb));
	tcb->pthread_private = pthread_private;
	*out = tcb;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rtld thread free operation.
 */
void
__rtld_thread_free(
	struct __rtld_tcb *tcb)
{
	intptr_t current;

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return;
	current = __syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS,
			     0, 0, 0, 0, 0);

	/* Handles the current condition. */
	if (current == (intptr_t)(uintptr_t)tcb)
		return;
	(void)munmap(tcb, STATIC_TCB_MAPPING_SIZE);
}

/*
 * Implements the rtld thread attach operation.
 */
int
__rtld_thread_attach(
	void *pthread_private)
{
	intptr_t value;
	struct __rtld_tcb *tcb;

	value = __syscall6(ZEDBSD_SYS_thread_self,
				    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Validates the current value. */
	if (value < 0)
		return -1;

	/* Validates the current value. */
	if (value == 0) {
		/* Handles a failed rtld thread alloc operation. */
		if (__rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value = __syscall6(ZEDBSD_SYS_thread_self,
				   ZEDBSD_THREAD_SELF_SET_TLS, (uintptr_t)tcb,
				   0, 0, 0, 0);

		/* Validates the current value. */
		if (value < 0) {
			__rtld_thread_free(tcb);

			/* Reports operation failure. */
			return -1;
		}

		/* Reports successful completion. */
		return 0;
	}
	tcb = (struct __rtld_tcb *)(uintptr_t)value;

	/* Handles the pthread private availability. */
	if (tcb->pthread_private != NULL)
		return -1;
	tcb->pthread_private = pthread_private;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rtld pthread private operation.
 */
void *
__rtld_pthread_private(
	void)
{
	intptr_t value;

	value = __syscall6(ZEDBSD_SYS_thread_self,
				    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Validates the current value. */
	if (value <= 0)
		return NULL;

	/* Returns the computed result. */
	return ((struct __rtld_tcb *)(uintptr_t)value)->pthread_private;
}

/*
 * Implements the rtld fork prepare operation.
 */
void
__rtld_fork_prepare(
	void)
{
}

/*
 * Implements the rtld fork parent operation.
 */
void
__rtld_fork_parent(
	void)
{
}

/*
 * Implements the rtld fork child operation.
 */
void
__rtld_fork_child(
	void)
{
}
