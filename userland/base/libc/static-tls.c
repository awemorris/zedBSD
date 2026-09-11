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
#include <rtld-abi.h>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define STATIC_TLS_PAGE_SIZE 4096U

/* Allocate a thread from the immutable executable template, never live TLS. */
int
__rtld_thread_alloc(
	void *pthread_private,
	struct __rtld_tcb **out)
{
	struct kern_tls_prefix prefix;
	struct __rtld_tcb *tcb;
	intptr_t current;
	void *mapping;
	size_t payload;
	size_t size;

	if (out == NULL)
		return -1;
	*out = NULL;
	memset(&prefix, 0, sizeof(prefix));
	current = __syscall6(KERN_SYS_thread_self,
	    KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (current < 0)
		return -1;
	if (current != 0)
		prefix = ((struct __rtld_tcb *)(uintptr_t)current)->tls;

	/* Bound the clone before rounding or copying its user-owned metadata. */
	if (prefix.memory_size > KERN_TLS_MEMORY_MAX)
		return -1;
	if (prefix.template_size > prefix.memory_size)
		return -1;
	if (prefix.distance < prefix.memory_size)
		return -1;
	if (prefix.distance > KERN_TLS_MEMORY_MAX + KERN_TLS_ALIGN_MAX)
		return -1;
	if (prefix.template_size != 0 && prefix.template_address == 0)
		return -1;

	payload = (prefix.distance + STATIC_TLS_PAGE_SIZE - 1U) &
	    ~(size_t)(STATIC_TLS_PAGE_SIZE - 1U);
	size = payload + KERN_TLS_TCB_RESERVE;
	mapping = mmap(NULL, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return -1;

	/* Anonymous zero-fill initializes .tbss and the unused TCB tail. */
	tcb = (struct __rtld_tcb *)((unsigned char *)mapping + payload);
	if (prefix.template_size != 0)
		memcpy((unsigned char *)tcb - prefix.distance,
		    (const void *)prefix.template_address, prefix.template_size);
	tcb->tls = prefix;
	tcb->tls.self = (uintptr_t)tcb;
	tcb->tls.mapping_base = (uintptr_t)mapping;
	tcb->tls.mapping_size = size;
	tcb->pthread_private = pthread_private;
	*out = tcb;
	return 0;
}

/* The terminating thread's joiner/reaper owns this complete mapping. */
void
__rtld_thread_free(
	struct __rtld_tcb *tcb)
{
	intptr_t current;

	if (tcb == NULL)
		return;
	current = __syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_GET_TLS,
	    0, 0, 0, 0, 0);
	if (current == (intptr_t)(uintptr_t)tcb)
		return;
	(void)munmap((void *)tcb->tls.mapping_base, tcb->tls.mapping_size);
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

	value = __syscall6(KERN_SYS_thread_self,
				    KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Validates the current value. */
	if (value < 0)
		return -1;

	/* Validates the current value. */
	if (value == 0) {
		/* Handles a failed rtld thread alloc operation. */
		if (__rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value = __syscall6(KERN_SYS_thread_self,
				   KERN_THREAD_SELF_SET_TLS, (uintptr_t)tcb,
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

	value = __syscall6(KERN_SYS_thread_self,
				    KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

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
