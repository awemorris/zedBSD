/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_TEST_CHECKPOINT_H
#define ZEDBSD_KERN_TEST_CHECKPOINT_H

enum kern_test_checkpoint_id {
	KERN_TEST_DISK_LOOKUP_BEFORE_REF = 1,
	KERN_TEST_FD_LOOKUP_BEFORE_REF,
	KERN_TEST_NAMECACHE_HIT_BEFORE_REF,
	KERN_TEST_WAIT_BEFORE_REGISTER,
	KERN_TEST_VM_PAGE_BEFORE_IO,
	KERN_TEST_SOCKET_BEFORE_WAIT,
};

#ifdef ZEDBSD_TEST_CHECKPOINTS
typedef void (*kern_test_checkpoint_fn)(enum kern_test_checkpoint_id,
	void *, void *);
void kern_test_checkpoint_set(kern_test_checkpoint_fn, void *);
void kern_test_checkpoint(enum kern_test_checkpoint_id, void *);
#define KERN_TEST_CHECKPOINT(id, object) kern_test_checkpoint((id), (object))
#else
#define KERN_TEST_CHECKPOINT(id, object) ((void)0)
#endif

#endif
