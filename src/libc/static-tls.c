/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library static tls support.
 */

#include "syscall.h"
#include <rtld-abi.h>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define STATIC_TLS_PAGE_SIZE 4096U

/*
 * Whether every thread of the process has a control block (BUG-110).
 *
 * A static program's first thread has one from exec when the program has
 * PT_TLS, and otherwise from __rtld_thread_attach; every later thread gets
 * one at thread_create.  Once the first thread is attached (set below, by
 * that thread, before any other exists), the thread pointer is read from
 * the processor instead of asking thread_self.  It is never cleared.
 */
static unsigned static_tls_everywhere;

static uintptr_t static_thread_pointer(void);

/* Allocate a thread from the immutable executable template, never live TLS. */
int
__rtld_thread_alloc(
	void *pthread_private,
	struct __rtld_tcb **out)
{
	struct kern_tls_prefix prefix;
	struct kern_tls_static_layout layout;
	struct __rtld_tcb *tcb;
	intptr_t current;
	void *mapping;

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

	/* Places the control block and the block the way exec placed the first. */
	kern_tls_static_layout(prefix.distance, STATIC_TLS_PAGE_SIZE,
	    KERN_TLS_STATIC_VARIANT, &layout);
	mapping = mmap(NULL, layout.mapping_size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return -1;

	/* Anonymous zero-fill initializes .tbss and the unused TCB tail. */
	tcb = (struct __rtld_tcb *)((unsigned char *)mapping +
	    layout.control_offset);
	if (prefix.template_size != 0)
		memcpy((unsigned char *)mapping + layout.block_offset,
		    (const void *)prefix.template_address, prefix.template_size);
	tcb->tls = prefix;
	tcb->tls.self = (uintptr_t)tcb;
	tcb->tls.mapping_base = (uintptr_t)mapping;
	tcb->tls.mapping_size = layout.mapping_size;
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

		/* From now on every thread has a control block. */
		__atomic_store_n(&static_tls_everywhere, 1U, __ATOMIC_RELEASE);

		/* Reports successful completion. */
		return 0;
	}
	tcb = (struct __rtld_tcb *)(uintptr_t)value;

	/* Handles the pthread private availability. */
	if (tcb->pthread_private != NULL)
		return -1;
	tcb->pthread_private = pthread_private;

	/* From now on every thread has a control block. */
	__atomic_store_n(&static_tls_everywhere, 1U, __ATOMIC_RELEASE);

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
	uintptr_t value;

	/* The calling thread's control block; a thread without one has no pthread record. */
	value = static_thread_pointer();
	if (value == 0)
		return NULL;

	/* Returns the computed result. */
	return ((struct __rtld_tcb *)value)->pthread_private;
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

/*
 * Reads the calling thread's thread pointer (its control block), or 0 when
 * it has none (BUG-110).  Once every thread has a control block, amd64
 * reads the block's first word (kern_tls_prefix.self, its own address) at
 * FS's base, arm64 reads TPIDR_EL0, riscv64 reads tp and 32-bit ARM reads
 * TPIDRURO, as the loader does (src/rtld/rtld.c); before that, and on
 * other processors, thread_self is asked, since FS may have no base yet.
 */
static uintptr_t
static_thread_pointer(
	void)
{
	uintptr_t pointer;
	intptr_t value;
	unsigned everywhere;

	/* The processor's register, once every thread is known to have a block. */
	everywhere = __atomic_load_n(&static_tls_everywhere, __ATOMIC_ACQUIRE);
	if (everywhere != 0U) {
#if defined(__x86_64__)
		__asm__ volatile("movq %%fs:0, %0" : "=r"(pointer));
		return pointer;
#elif defined(__aarch64__)
		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(pointer));
		return pointer;
#elif defined(__riscv)
		__asm__ volatile("mv %0, tp" : "=r"(pointer));
		return pointer;
#elif defined(__arm__)
		__asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(pointer));
		return pointer;
#else
		pointer = 0;
#endif
	}

	/* The kernel's record of the thread pointer. */
	value = __syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (value <= 0)
		return 0;

	/* Succeeded: the thread pointer. */
	pointer = (uintptr_t)value;
	return pointer;
}
