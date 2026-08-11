/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/namei.h"
#include "kern/kmem.h"

#include <errno.h>
#include <string.h>

#define CWDINFO_DYNAMIC 0x00000001U

int
cwdinfo_clone(const struct cwdinfo *source, struct cwdinfo **result)
{
	struct cwdinfo *copy;
	if (source == NULL || source->root == NULL || source->cwd == NULL ||
	    result == NULL)
		return EINVAL;
	copy = kern_malloc(sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;
	memcpy(copy, source, sizeof(*copy));
	copy->usecount = 1;
	copy->flags = CWDINFO_DYNAMIC;
	inode_ref(copy->root);
	inode_ref(copy->cwd);
	*result = copy;
	return 0;
}

void
cwdinfo_retain(struct cwdinfo *context)
{
	if (context != NULL && context->usecount != 0)
		context->usecount++;
}

void
cwdinfo_release(struct cwdinfo *context)
{
	unsigned dynamic;
	if (context == NULL || context->usecount == 0)
		return;
	if (--context->usecount != 0)
		return;
	dynamic = context->flags & CWDINFO_DYNAMIC;
	inode_release(context->root);
	inode_release(context->cwd);
	if (dynamic)
		kern_free(context);
	else
		memset(context, 0, sizeof(*context));
}
