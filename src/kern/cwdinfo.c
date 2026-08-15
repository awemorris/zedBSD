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
	unsigned long irq;
	if (source == NULL || result == NULL)
		return EINVAL;
	copy = kern_malloc(sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;
	memset(copy, 0, sizeof(*copy));
	refcount_init(&copy->refs, 1);
	spin_init(&copy->lock, LOCK_RANK_PROCESS, "cwdinfo");
	copy->flags = CWDINFO_DYNAMIC;
	irq = spin_lock_irqsave((struct spinlock *)&source->lock);
	if (source->root.p_inode == NULL || source->cwd.p_inode == NULL) {
		spin_unlock_irqrestore((struct spinlock *)&source->lock, irq);
		kern_free(copy);
		return EINVAL;
	}
	path_set(&copy->root, source->root.p_mount, source->root.p_inode);
	path_set(&copy->cwd, source->cwd.p_mount, source->cwd.p_inode);
	spin_unlock_irqrestore((struct spinlock *)&source->lock, irq);
	*result = copy;
	return 0;
}

void
cwdinfo_retain(struct cwdinfo *context)
{
	if (context != NULL)
		refcount_get(&context->refs);
}

void
cwdinfo_release(struct cwdinfo *context)
{
	unsigned dynamic;
	if (context == NULL)
		return;
	if (!refcount_put(&context->refs))
		return;
	dynamic = context->flags & CWDINFO_DYNAMIC;
	path_release(&context->root);
	path_release(&context->cwd);
	if (dynamic)
		kern_free(context);
	else
		memset(context, 0, sizeof(*context));
}
