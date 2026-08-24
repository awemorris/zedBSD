/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/test-checkpoint.h"

#include <hal/atomic.h>

#ifdef ZEDBSD_TEST_CHECKPOINTS
static kern_test_checkpoint_fn checkpoint_handler;
static void *checkpoint_argument;

void
kern_test_checkpoint_set(kern_test_checkpoint_fn handler, void *argument)
{
	checkpoint_argument = argument;
	hal_atomic_store_release(&checkpoint_handler, handler);
}

void
kern_test_checkpoint(enum kern_test_checkpoint_id id, void *object)
{
	kern_test_checkpoint_fn handler =
	    hal_atomic_load_acquire(&checkpoint_handler);
	if (handler != 0)
		handler(id, object, checkpoint_argument);
}
#endif
