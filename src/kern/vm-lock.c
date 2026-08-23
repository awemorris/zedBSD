/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/vm-lock.h"
#include "kern/lock.h"

#include <hal/hal.h>
#include <limits.h>

static struct mutex metadata_lock;
static unsigned metadata_depth;
static int metadata_initialized;

void
vm_metadata_init(void)
{
	if (metadata_initialized)
		return;
	if (mutex_init(&metadata_lock, LOCK_RANK_VMSPACE, "VM metadata") != 0)
		HAL_FATAL("VM metadata lock initialization failed");
	metadata_initialized = 1;
}

void
vm_metadata_enter(void)
{
	vm_metadata_init();
	if (mutex_owned(&metadata_lock)) {
		if (metadata_depth == UINT_MAX)
			HAL_FATAL("VM metadata lock recursion overflow");
		metadata_depth++;
		return;
	}
	mutex_lock(&metadata_lock);
	metadata_depth = 1;
}

void
vm_metadata_leave(void)
{
	if (!metadata_initialized || !mutex_owned(&metadata_lock) ||
	    metadata_depth == 0)
		HAL_FATAL("VM metadata lock ownership mismatch");
	if (--metadata_depth == 0)
		mutex_unlock(&metadata_lock);
}

int
vm_metadata_owned(void)
{
	return metadata_initialized && mutex_owned(&metadata_lock);
}
