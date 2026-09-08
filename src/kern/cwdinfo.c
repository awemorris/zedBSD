/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process root and working directory state.
 */

#include "kern/namei.h"
#include "kern/kmem.h"

#include <errno.h>
#include <string.h>

#define CWDINFO_DYNAMIC 0x00000001U

/*
 * Copies a process's root and working directory into a new record.
 *
 * The copy holds its own path references and is freed by the last
 * cwdinfo_release().
 */
int
cwdinfo_clone(
	const struct cwdinfo *source,
	struct cwdinfo **result)
{
	struct cwdinfo *copy;
	unsigned long irq;

	/* Rejects a missing source or result pointer. */
	if (source == NULL || result == NULL)
		return EINVAL;

	/* Allocates the copy. */
	copy = kern_malloc(sizeof(*copy));
	if (copy == NULL)
		return ENOMEM;

	/* Initializes the copy as a dynamically owned record. */
	memset(copy, 0, sizeof(*copy));
	refcount_init(&copy->refs, 1);
	spin_init(&copy->lock, LOCK_RANK_PROCESS, "cwdinfo");
	copy->flags = CWDINFO_DYNAMIC;

	/* Takes references to the source paths under the source lock. */
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

	/* Reports the completed copy. */
	return 0;
}

/*
 * Takes one more reference to a directory record.
 */
void
cwdinfo_retain(
	struct cwdinfo *context)
{
	/* Ignores a missing record. */
	if (context != NULL)
		refcount_get(&context->refs);
}

/*
 * Drops one reference to a directory record, releasing it on the last.
 *
 * A dynamically allocated record is freed; a statically embedded one is
 * cleared in place.
 */
void
cwdinfo_release(
	struct cwdinfo *context)
{
	unsigned dynamic;

	/* Ignores a missing record. */
	if (context == NULL)
		return;

	/* Keeps a record that is still referenced. */
	if (!refcount_put(&context->refs))
		return;

	/* Releases the paths before the record itself. */
	dynamic = context->flags & CWDINFO_DYNAMIC;
	path_release(&context->root);
	path_release(&context->cwd);

	/* Frees a dynamic record and clears an embedded one. */
	if (dynamic)
		kern_free(context);
	else
		memset(context, 0, sizeof(*context));
}
