/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libkeiland asks of Windows: the wake, the lock and the threads.
 */

#include "userland/desktop/libkeiland/platform/platform.h"

#include <errno.h>
#include <stddef.h>

/* Not implemented yet. */
int
keiui_wake_open(
	struct keiui_wake *wake)
{
	(void)wake;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
keiui_wake_close(
	struct keiui_wake *wake)
{
	(void)wake;
}

/* Not implemented yet. */
void
keiui_wake_ring(
	struct keiui_wake *wake)
{
	(void)wake;
}

/* Not implemented yet. */
void
keiui_wake_clear(
	struct keiui_wake *wake)
{
	(void)wake;
}

/* Not implemented yet. */
int
keiui_wake_descriptor(
	const struct keiui_wake *wake)
{
	(void)wake;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void *
keiui_wake_event(
	const struct keiui_wake *wake)
{
	(void)wake;

	/* A skeleton: the function is not written yet. */
	return NULL;
}

/* Not implemented yet. */
int
keiui_lock_open(
	struct keiui_lock *lock)
{
	(void)lock;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
keiui_lock_close(
	struct keiui_lock *lock)
{
	(void)lock;
}

/* Not implemented yet. */
void
keiui_lock_take(
	struct keiui_lock *lock)
{
	(void)lock;
}

/* Not implemented yet. */
void
keiui_lock_give(
	struct keiui_lock *lock)
{
	(void)lock;
}

/* Not implemented yet. */
keiui_thread_id
keiui_thread_self(
	void)
{
	/* A skeleton: the function is not written yet. */
	return 0;
}

/* Not implemented yet. */
int
keiui_thread_is(
	keiui_thread_id thread)
{
	(void)thread;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
keiui_thread_start(
	void *(*run)(void *),
	void *argument)
{
	(void)run;
	(void)argument;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
int
keiui_thread_start_joined(
	void *(*run)(void *),
	void *argument,
	keiui_thread_id *thread)
{
	(void)run;
	(void)argument;
	(void)thread;

	/* A skeleton: the function is not written yet. */
	return ENOSYS;
}

/* Not implemented yet. */
void
keiui_thread_join(
	keiui_thread_id thread)
{
	(void)thread;
}

/* Not implemented yet. */
void
keiui_thread_name(
	const char *name)
{
	(void)name;
}
