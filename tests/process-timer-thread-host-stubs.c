/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "process-timer-thread-host-stubs.h"

#include <threads.h>

extern void abort(void);

static void
host_bytes_copy(void *destination, const void *source, unsigned long size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	while (size-- != 0)
		*to++ = *from++;
}

static void
host_bytes_zero(void *destination, unsigned long size)
{
	unsigned char *to = destination;

	while (size-- != 0)
		*to++ = 0;
}

static mtx_t timer_mutex;
static mtx_t checkpoint_mutex;
static cnd_t checkpoint_condition;

static void
host_require(int result)
{
	if (result != thrd_success)
		abort();
}

int
host_sync_init(void)
{
	/* Production timer creation nests process->lock inside the timer registry
	 * lock.  This host shim maps both spinlocks to one recursive serializer. */
	host_require(mtx_init(&timer_mutex, mtx_recursive));
	host_require(mtx_init(&checkpoint_mutex, mtx_plain));
	host_require(cnd_init(&checkpoint_condition));
	return 0;
}

void host_timer_lock(void) { host_require(mtx_lock(&timer_mutex)); }
void host_timer_unlock(void) { host_require(mtx_unlock(&timer_mutex)); }
void host_checkpoint_lock(void) { host_require(mtx_lock(&checkpoint_mutex)); }
void host_checkpoint_unlock(void)
{ host_require(mtx_unlock(&checkpoint_mutex)); }
void host_checkpoint_wait(void)
{ host_require(cnd_wait(&checkpoint_condition, &checkpoint_mutex)); }
void host_checkpoint_broadcast(void)
{ host_require(cnd_broadcast(&checkpoint_condition)); }

int
host_thread_create(struct host_thread_handle *handle, int (*entry)(void *),
	void *argument)
{
	thrd_t thread;
	int result;

	_Static_assert(sizeof(thread) <= sizeof(handle->opaque),
	    "host thread handle too small");
	result = thrd_create(&thread, entry, argument);
	if (result != thrd_success)
		return -1;
	host_bytes_zero(handle, sizeof(*handle));
	host_bytes_copy(handle->opaque, &thread, sizeof(thread));
	return 0;
}

int
host_thread_join(struct host_thread_handle *handle)
{
	thrd_t thread;

	host_bytes_copy(&thread, handle->opaque, sizeof(thread));
	return thrd_join(thread, NULL) == thrd_success ? 0 : -1;
}
