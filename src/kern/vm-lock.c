/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The recursive VM metadata lock.
 *
 * One mutex protects the VM metadata; a depth counter lets the owning
 * thread re-enter it, so nested metadata operations need no lock passing.
 */

#include "kern/vm-lock.h"
#include "kern/lock.h"

#include <hal/hal.h>
#include <limits.h>

static struct mutex metadata_lock;
static unsigned metadata_depth;
static int metadata_initialized;

/*
 * Initializes the metadata lock once.
 */
void
vm_metadata_init(
	void)
{
	/* Ignores a repeated initialization. */
	if (metadata_initialized)
		return;

	/* Creates the mutex; failure here leaves the VM unusable. */
	if (mutex_init(&metadata_lock, LOCK_RANK_VMSPACE, "VM metadata") != 0)
		HAL_FATAL("VM metadata lock initialization failed");
	metadata_initialized = 1;
}

/*
 * Enters the metadata lock, re-entering it when already owned.
 */
void
vm_metadata_enter(
	void)
{
	vm_metadata_init();

	/* Deepens an entry that the current thread already owns. */
	if (mutex_owned(&metadata_lock)) {
		if (metadata_depth == UINT_MAX)
			HAL_FATAL("VM metadata lock recursion overflow");
		metadata_depth++;
		return;
	}

	/* Takes the first entry. */
	mutex_lock(&metadata_lock);
	metadata_depth = 1;
}

/*
 * Leaves one level of the metadata lock.
 */
void
vm_metadata_leave(
	void)
{
	/* Traps on a release by a thread that does not own the lock. */
	if (!metadata_initialized ||
	    !mutex_owned(&metadata_lock) ||
	    metadata_depth == 0)
		HAL_FATAL("VM metadata lock ownership mismatch");

	/* Releases the mutex when the outermost entry leaves. */
	if (--metadata_depth == 0)
		mutex_unlock(&metadata_lock);
}

/*
 * Tests whether the current thread owns the metadata lock.
 */
int
vm_metadata_owned(
	void)
{
	int owned;

	/* An uninitialized lock is owned by nobody. */
	if (!metadata_initialized)
		return 0;

	/* Asks the mutex. */
	owned = mutex_owned(&metadata_lock);

	/* Reports the ownership. */
	return owned;
}
