/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/test-checkpoint.h"

#ifdef ZEDBSD_TEST_CHECKPOINTS
static kern_test_checkpoint_fn checkpoint_handler;
static void *checkpoint_argument;

void
kern_test_checkpoint_set(kern_test_checkpoint_fn handler, void *argument)
{
	checkpoint_argument = argument;
	__atomic_store_n(&checkpoint_handler, handler, __ATOMIC_RELEASE);
}

void
kern_test_checkpoint(enum kern_test_checkpoint_id id, void *object)
{
	kern_test_checkpoint_fn handler =
	    __atomic_load_n(&checkpoint_handler, __ATOMIC_ACQUIRE);
	if (handler != 0)
		handler(id, object, checkpoint_argument);
}
#endif
