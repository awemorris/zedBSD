/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Test checkpoints.
 *
 * A test build lets a fixture install one handler that is called at named
 * points inside the kernel, so a host test can observe or perturb an
 * intermediate state.  Production builds compile the checkpoints out.
 */

#include "kern/test-checkpoint.h"

#include <hal/atomic.h>

#ifdef KERN_TEST_CHECKPOINTS
static kern_test_checkpoint_fn checkpoint_handler;
static void *checkpoint_argument;

/*
 * Installs or clears the checkpoint handler.
 */
void
kern_test_checkpoint_set(
	kern_test_checkpoint_fn handler,
	void *argument)
{
	/* Publishes the argument before the handler that reads it. */
	checkpoint_argument = argument;
	hal_atomic_store_release(&checkpoint_handler, handler);
}

/*
 * Reports one checkpoint to the installed handler, if any.
 */
void
kern_test_checkpoint(
	enum kern_test_checkpoint_id id,
	void *object)
{
	kern_test_checkpoint_fn handler;

	/* Calls the handler that was installed at the time of the checkpoint. */
	handler = hal_atomic_load_acquire(&checkpoint_handler);
	if (handler != 0)
		handler(id, object, checkpoint_argument);
}
#endif
