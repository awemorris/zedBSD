/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/libc/syscall.h"
#include <zedbsd/rtld-abi.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#define STATIC_TCB_MAPPING_SIZE 4096U

int
__zedbsd_rtld_thread_alloc(void *pthread_private,
	struct zedbsd_rtld_tcb **out)
{
	struct zedbsd_rtld_tcb *tcb;

	if (out == NULL)
		return -1;
	*out = NULL;
	tcb = mmap(NULL, STATIC_TCB_MAPPING_SIZE, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (tcb == MAP_FAILED)
		return -1;
	memset(tcb, 0, sizeof(*tcb));
	tcb->pthread_private = pthread_private;
	*out = tcb;
	return 0;
}

void
__zedbsd_rtld_thread_free(struct zedbsd_rtld_tcb *tcb)
{
	intptr_t current;

	if (tcb == NULL)
		return;
	current = zedbsd_syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (current == (intptr_t)(uintptr_t)tcb)
		return;
	(void)munmap(tcb, STATIC_TCB_MAPPING_SIZE);
}

int
__zedbsd_rtld_thread_attach(void *pthread_private)
{
	intptr_t value = zedbsd_syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	struct zedbsd_rtld_tcb *tcb;

	if (value < 0)
		return -1;
	if (value == 0) {
		if (__zedbsd_rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value = zedbsd_syscall6(ZEDBSD_SYS_thread_self,
		    ZEDBSD_THREAD_SELF_SET_TLS, (uintptr_t)tcb, 0, 0, 0, 0);
		if (value < 0) {
			__zedbsd_rtld_thread_free(tcb);
			return -1;
		}
		return 0;
	}
	tcb = (struct zedbsd_rtld_tcb *)(uintptr_t)value;
	if (tcb->pthread_private != NULL)
		return -1;
	tcb->pthread_private = pthread_private;
	return 0;
}

void *
__zedbsd_rtld_pthread_private(void)
{
	intptr_t value = zedbsd_syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (value <= 0)
		return NULL;
	return ((struct zedbsd_rtld_tcb *)(uintptr_t)value)->pthread_private;
}

void __zedbsd_rtld_fork_prepare(void) { }
void __zedbsd_rtld_fork_parent(void) { }
void __zedbsd_rtld_fork_child(void) { }
