/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The directory name cache.
 *
 * An entry remembers that a name in a parent directory resolved to a child
 * inode, together with the parent's directory sequence at that time.  A
 * lookup that finds a stale sequence drops the entry instead of trusting
 * it.  Every entry holds references to both inodes, released outside the
 * cache lock.
 */

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

static int matches(const struct namecache_entry *entry, struct inode *parent, const struct componentname *name);
static void detach(struct namecache_entry *entry, struct inode **parent, struct inode **child);
static void release_pair(struct inode *parent, struct inode *child);
static void purge_matching(struct inode *inode, struct mount *mountp, int all);

/*
 * Resolves a name in a directory from the cache.
 *
 * A hit returns a referenced child.  A stale entry is dropped and reported
 * as a miss so that the caller consults the filesystem.
 */
int
namecache_lookup(
	struct inode *parent,
	const struct componentname *name,
	struct inode **result)
{
	struct inode *old_parent;
	struct inode *old_child;
	unsigned i;
	unsigned long irq;

	/* Rejects a missing operand or an impossible name length. */
	if (parent == NULL ||
	    name == NULL ||
	    result == NULL ||
	    name->cn_namelen == 0 ||
	    name->cn_namelen > NAME_MAX)
		return EINVAL;

	/* Searches until a hit, a miss, or after dropping a stale entry. */
	for (;;) {
		old_parent = NULL;
		old_child = NULL;
		irq = spin_lock_irqsave(&namecache_lock);
		for (i = 0; i < NAMECACHE_MAX; i++) {
			if (entries[i].parent == NULL ||
			    !matches(&entries[i], parent, name))
				continue;

			/* Drops an entry whose directory changed since. */
			if (entries[i].parent_dirseq !=
			    atomic_u64_load_acquire(&parent->i_dirseq)) {
				detach(&entries[i], &old_parent, &old_child);
				break;
			}

			/* Hands out a referenced child. */
			KERN_TEST_CHECKPOINT(KERN_TEST_NAMECACHE_HIT_BEFORE_REF,
			    entries[i].child);
			inode_ref(entries[i].child);
			*result = entries[i].child;
			spin_unlock_irqrestore(&namecache_lock, irq);
			return 0;
		}
		spin_unlock_irqrestore(&namecache_lock, irq);

		/* Reports a miss when nothing stale was dropped. */
		if (old_parent == NULL)
			return ENOENT;

		release_pair(old_parent, old_child);
	}
}

/*
 * Records that a name in a directory resolved to a child.
 *
 * An identical current entry is kept; a conflicting one is replaced.  With
 * no free slot the cache evicts entries round-robin.
 */
int
namecache_enter(
	struct inode *parent,
	const struct componentname *name,
	struct inode *child,
	uint64_t observed_sequence)
{
	struct inode *old_parent;
	struct inode *old_child;
	unsigned i;
	unsigned slot;
	unsigned long irq;

	old_parent = NULL;
	old_child = NULL;
	slot = NAMECACHE_MAX;

	/* Rejects a missing operand or an impossible name length. */
	if (parent == NULL ||
	    child == NULL ||
	    name == NULL ||
	    name->cn_namelen == 0 ||
	    name->cn_namelen > NAME_MAX)
		return EINVAL;

	/* Hold the references before publishing the entry. */
	inode_ref(parent);
	inode_ref(child);

	irq = spin_lock_irqsave(&namecache_lock);

	/* Rejects a lookup invalidated before cache publication. */
	if (atomic_u64_load_acquire(&parent->i_dirseq) != observed_sequence) {
		spin_unlock_irqrestore(&namecache_lock, irq);
		release_pair(parent, child);
		return EAGAIN;
	}

	/* Finds the matching entry or the first free slot. */
	for (i = 0; i < NAMECACHE_MAX; i++) {
		if (entries[i].parent != NULL && matches(&entries[i], parent, name)) {
			/* Keeps an entry that is already current. */
			if (entries[i].child == child &&
			    entries[i].parent_dirseq == observed_sequence) {
				spin_unlock_irqrestore(&namecache_lock, irq);
				release_pair(parent, child);
				return 0;
			}

			/* Replaces a conflicting entry in place. */
			detach(&entries[i], &old_parent, &old_child);
			slot = i;
			break;
		}
		if (slot == NAMECACHE_MAX && entries[i].parent == NULL)
			slot = i;
	}

	/* Evicts round-robin when the cache is full. */
	if (slot == NAMECACHE_MAX) {
		slot = replacement++ % NAMECACHE_MAX;
		detach(&entries[slot], &old_parent, &old_child);
	}

	/* Publishes the entry with the directory sequence it is valid for. */
	entries[slot].parent = parent;
	entries[slot].child = child;
	/*
	 * A concurrent mutation after admission leaves an old sequence, which
	 * lookup rejects.  Never promote an older lookup to the current sequence. */
	entries[slot].parent_dirseq = observed_sequence;
	entries[slot].length = name->cn_namelen;
	memcpy(entries[slot].name, name->cn_nameptr, name->cn_namelen);
	entries[slot].name[name->cn_namelen] = '\0';

	spin_unlock_irqrestore(&namecache_lock, irq);

	/* Releases the references of the evicted entry, if any. */
	release_pair(old_parent, old_child);

	/* Reports the recorded entry. */
	return 0;
}

/*
 * Forgets every entry for a name in a directory.
 */
void
namecache_remove(
	struct inode *parent,
	const struct componentname *name)
{
	struct inode *old_parent;
	struct inode *old_child;
	unsigned i;
	unsigned long irq;

	/* Ignores a missing directory or name. */
	if (parent == NULL || name == NULL)
		return;

	/* Drops matching entries one at a time, releasing outside the lock. */
	for (;;) {
		old_parent = NULL;
		old_child = NULL;
		irq = spin_lock_irqsave(&namecache_lock);
		for (i = 0; i < NAMECACHE_MAX; i++) {
			if (entries[i].parent != NULL &&
			    matches(&entries[i], parent, name)) {
				detach(&entries[i], &old_parent, &old_child);
				break;
			}
		}
		spin_unlock_irqrestore(&namecache_lock, irq);

		/* Stops when no entry matched. */
		if (old_parent == NULL)
			return;

		release_pair(old_parent, old_child);
	}
}

/*
 * Forgets every entry that names an inode as parent or child.
 */
void
namecache_purge_inode(
	struct inode *inode)
{
	purge_matching(inode, NULL, 0);
}

/*
 * Forgets every entry that belongs to a mount.
 */
void
namecache_purge_mount(
	struct mount *mountp)
{
	purge_matching(NULL, mountp, 0);
}

/*
 * Forgets every entry.
 */
void
namecache_reset(
	void)
{
	purge_matching(NULL, NULL, 1);
}

/*
 * Counts the live entries.
 */
unsigned
namecache_count(
	void)
{
	unsigned i;
	unsigned count;
	unsigned long irq;

	count = 0;

	/* Counts the occupied slots under the lock. */
	irq = spin_lock_irqsave(&namecache_lock);
	for (i = 0; i < NAMECACHE_MAX; i++) {
		if (entries[i].parent != NULL)
			count++;
	}
	spin_unlock_irqrestore(&namecache_lock, irq);

	/* Reports the count. */
	return count;
}

/* Tests whether an entry records a name in a directory. */
static int
matches(
	const struct namecache_entry *entry,
	struct inode *parent,
	const struct componentname *name)
{
	/* The directory and the name length must match first. */
	if (entry->parent != parent)
		return 0;
	if (entry->length != name->cn_namelen)
		return 0;

	/* The name bytes must match. */
	if (memcmp(entry->name, name->cn_nameptr, name->cn_namelen) != 0)
		return 0;

	/* Reports a match. */
	return 1;
}

/* Empties an entry and hands its references to the caller. */
static void
detach(
	struct namecache_entry *entry,
	struct inode **parent,
	struct inode **child)
{
	/* Transfers the references before clearing the slot. */
	*parent = entry->parent;
	*child = entry->child;
	memset(entry, 0, sizeof(*entry));
}

/* Releases the references that an entry held. */
static void
release_pair(
	struct inode *parent,
	struct inode *child)
{
	inode_release(parent);
	inode_release(child);
}

/* Drops every entry that matches an inode, a mount, or everything. */
static void
purge_matching(
	struct inode *inode,
	struct mount *mountp,
	int all)
{
	struct namecache_entry *entry;
	struct inode *old_parent;
	struct inode *old_child;
	unsigned long irq;
	unsigned i;
	int match;

	/* Drops matching entries one at a time, releasing outside the lock. */
	for (;;) {
		old_parent = NULL;
		old_child = NULL;
		irq = spin_lock_irqsave(&namecache_lock);
		for (i = 0; i < NAMECACHE_MAX; i++) {
			entry = &entries[i];

			/* Matches by inode, or by mount when one is given. */
			match = all || entry->parent == inode || entry->child == inode;
			if (mountp != NULL)
				match = entry->parent != NULL &&
				    (entry->parent->i_mount == mountp ||
				     entry->child->i_mount == mountp);
			if (entry->parent != NULL && match) {
				detach(entry, &old_parent, &old_child);
				break;
			}
		}

		/* Restarts the eviction cursor once a full purge is complete. */
		if (all && old_parent == NULL)
			replacement = 0;
		spin_unlock_irqrestore(&namecache_lock, irq);

		/* Stops when no entry matched. */
		if (old_parent == NULL)
			return;

		release_pair(old_parent, old_child);
	}
}
