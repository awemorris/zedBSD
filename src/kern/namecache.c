/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <string.h>

struct namecache_entry {
	struct inode *parent;
	struct inode *child;
	size_t length;
	char name[NAME_MAX + 1U];
};

static struct namecache_entry entries[NAMECACHE_MAX]
	__attribute__((section(".vfs_bss")));
static unsigned replacement;

static int
matches(const struct namecache_entry *entry, struct inode *parent,
	const struct componentname *name)
{
	return entry->parent == parent && entry->length == name->cn_namelen &&
	       memcmp(entry->name, name->cn_nameptr, name->cn_namelen) == 0;
}

static void
drop(struct namecache_entry *entry)
{
	if (entry->parent == NULL)
		return;
	inode_release(entry->parent);
	inode_release(entry->child);
	memset(entry, 0, sizeof(*entry));
}

int
namecache_lookup(struct inode *parent, const struct componentname *name,
		 struct inode **result)
{
	unsigned i;
	if (parent == NULL || name == NULL || result == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	for (i = 0; i < NAMECACHE_MAX; i++) {
		if (entries[i].parent != NULL && matches(&entries[i], parent, name)) {
			inode_ref(entries[i].child);
			*result = entries[i].child;
			return 0;
		}
	}
	return ENOENT;
}

int
namecache_enter(struct inode *parent, const struct componentname *name,
		struct inode *child)
{
	unsigned i, slot = NAMECACHE_MAX;
	if (parent == NULL || child == NULL || name == NULL ||
	    name->cn_namelen == 0 || name->cn_namelen > NAME_MAX)
		return EINVAL;
	for (i = 0; i < NAMECACHE_MAX; i++) {
		if (entries[i].parent != NULL && matches(&entries[i], parent, name)) {
			if (entries[i].child == child)
				return 0;
			drop(&entries[i]);
			slot = i;
			break;
		}
		if (slot == NAMECACHE_MAX && entries[i].parent == NULL)
			slot = i;
	}
	if (slot == NAMECACHE_MAX) {
		slot = replacement++ % NAMECACHE_MAX;
		drop(&entries[slot]);
	}
	inode_ref(parent);
	inode_ref(child);
	entries[slot].parent = parent;
	entries[slot].child = child;
	entries[slot].length = name->cn_namelen;
	memcpy(entries[slot].name, name->cn_nameptr, name->cn_namelen);
	entries[slot].name[name->cn_namelen] = '\0';
	return 0;
}

void
namecache_remove(struct inode *parent, const struct componentname *name)
{
	unsigned i;
	if (parent == NULL || name == NULL)
		return;
	for (i = 0; i < NAMECACHE_MAX; i++)
		if (entries[i].parent != NULL && matches(&entries[i], parent, name))
			drop(&entries[i]);
}

void
namecache_purge_inode(struct inode *inode)
{
	unsigned i;
	for (i = 0; i < NAMECACHE_MAX; i++)
		if (entries[i].parent == inode || entries[i].child == inode)
			drop(&entries[i]);
}

void
namecache_purge_mount(struct mount *mountp)
{
	unsigned i;
	for (i = 0; i < NAMECACHE_MAX; i++)
		if (entries[i].parent != NULL &&
		    (entries[i].parent->i_mount == mountp ||
		     entries[i].child->i_mount == mountp))
			drop(&entries[i]);
}

void
namecache_reset(void)
{
	unsigned i;
	for (i = 0; i < NAMECACHE_MAX; i++)
		drop(&entries[i]);
	replacement = 0;
}
