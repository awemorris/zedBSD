/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/namecache.h"
#include "kern/atomic.h"
#include "kern/namei.h"
#include "kern/lock.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <string.h>

struct namecache_entry {
	struct inode *parent;
	struct inode *child;
	uint64_t parent_dirseq;
	size_t length;
	char name[NAME_MAX + 1U];
};

static struct namecache_entry entries[NAMECACHE_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned replacement;
static struct spinlock namecache_lock = {
	{ 0 }, LOCK_RANK_NAMECACHE, "namecache", 0, 0
};

static int
matches(const struct namecache_entry *entry, struct inode *parent,
	const struct componentname *name)
{
	return entry->parent == parent && entry->length == name->cn_namelen &&
	       memcmp(entry->name, name->cn_nameptr, name->cn_namelen) == 0;
}

static void
detach(struct namecache_entry *entry, struct inode **parent,
	struct inode **child)
{
	*parent = entry->parent;
	*child = entry->child;
	memset(entry, 0, sizeof(*entry));
}

static void
release_pair(struct inode *parent, struct inode *child)
{
	inode_release(parent);
	inode_release(child);
}

int
namecache_lookup(struct inode *parent, const struct componentname *name,
		 struct inode **result)
{
	unsigned i;
	if (parent == NULL || name == NULL || result == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	for (;;) {
		struct inode *old_parent = NULL, *old_child = NULL;
		unsigned long irq = spin_lock_irqsave(&namecache_lock);
		for (i = 0; i < NAMECACHE_MAX; i++) {
			if (entries[i].parent == NULL ||
			    !matches(&entries[i], parent, name))
				continue;
			if (entries[i].parent_dirseq !=
			    atomic_u64_load_acquire(&parent->i_dirseq)) {
				detach(&entries[i], &old_parent, &old_child);
				break;
			}
			KERN_TEST_CHECKPOINT(KERN_TEST_NAMECACHE_HIT_BEFORE_REF,
			    entries[i].child);
			inode_ref(entries[i].child);
			*result = entries[i].child;
			spin_unlock_irqrestore(&namecache_lock, irq);
			return 0;
		}
		spin_unlock_irqrestore(&namecache_lock, irq);
		if (old_parent == NULL)
			return ENOENT;
		release_pair(old_parent, old_child);
	}
}

int
namecache_enter(struct inode *parent, const struct componentname *name,
		struct inode *child)
{
	struct inode *old_parent = NULL, *old_child = NULL;
	unsigned i, slot = NAMECACHE_MAX;
	unsigned long irq;
	if (parent == NULL || child == NULL || name == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	/* Hold the references before publishing the entry. */
	inode_ref(parent);
	inode_ref(child);
	irq = spin_lock_irqsave(&namecache_lock);
	for (i = 0; i < NAMECACHE_MAX; i++) {
		if (entries[i].parent != NULL && matches(&entries[i], parent, name)) {
			if (entries[i].child == child &&
			    entries[i].parent_dirseq ==
			    atomic_u64_load_acquire(&parent->i_dirseq)) {
				spin_unlock_irqrestore(&namecache_lock, irq);
				release_pair(parent, child);
				return 0;
			}
			detach(&entries[i], &old_parent, &old_child);
			slot = i;
			break;
		}
		if (slot == NAMECACHE_MAX && entries[i].parent == NULL)
			slot = i;
	}
	if (slot == NAMECACHE_MAX) {
		slot = replacement++ % NAMECACHE_MAX;
		detach(&entries[slot], &old_parent, &old_child);
	}
	entries[slot].parent = parent;
	entries[slot].child = child;
	entries[slot].parent_dirseq =
	    atomic_u64_load_acquire(&parent->i_dirseq);
	entries[slot].length = name->cn_namelen;
	memcpy(entries[slot].name, name->cn_nameptr, name->cn_namelen);
	entries[slot].name[name->cn_namelen] = '\0';
	spin_unlock_irqrestore(&namecache_lock, irq);
	release_pair(old_parent, old_child);
	return 0;
}

void
namecache_remove(struct inode *parent, const struct componentname *name)
{
	unsigned i;
	if (parent == NULL || name == NULL)
		return;
	for (;;) {
		struct inode *old_parent = NULL, *old_child = NULL;
		unsigned long irq = spin_lock_irqsave(&namecache_lock);
		for (i = 0; i < NAMECACHE_MAX; i++)
			if (entries[i].parent != NULL &&
			    matches(&entries[i], parent, name)) {
				detach(&entries[i], &old_parent, &old_child);
				break;
			}
		spin_unlock_irqrestore(&namecache_lock, irq);
		if (old_parent == NULL)
			return;
		release_pair(old_parent, old_child);
	}
}

static void
purge_matching(struct inode *inode, struct mount *mountp, int all)
{
	for (;;) {
		struct inode *old_parent = NULL, *old_child = NULL;
		unsigned long irq = spin_lock_irqsave(&namecache_lock);
		unsigned i;
		for (i = 0; i < NAMECACHE_MAX; i++) {
			struct namecache_entry *entry = &entries[i];
			int match = all || entry->parent == inode ||
			    entry->child == inode;
			if (mountp != NULL)
				match = entry->parent != NULL &&
				    (entry->parent->i_mount == mountp ||
				     entry->child->i_mount == mountp);
			if (entry->parent != NULL && match) {
				detach(entry, &old_parent, &old_child);
				break;
			}
		}
		if (all && old_parent == NULL)
			replacement = 0;
		spin_unlock_irqrestore(&namecache_lock, irq);
		if (old_parent == NULL)
			return;
		release_pair(old_parent, old_child);
	}
}

void namecache_purge_inode(struct inode *inode)
{ purge_matching(inode, NULL, 0); }

void namecache_purge_mount(struct mount *mountp)
{ purge_matching(NULL, mountp, 0); }

void namecache_reset(void)
{ purge_matching(NULL, NULL, 1); }

unsigned
namecache_count(void)
{
	unsigned i, count = 0;
	unsigned long irq = spin_lock_irqsave(&namecache_lock);
	for (i = 0; i < NAMECACHE_MAX; i++)
		count += entries[i].parent != NULL;
	spin_unlock_irqrestore(&namecache_lock, irq);
	return count;
}
