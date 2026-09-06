/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sysctl tree.
 *
 * The kernel exposes a fixed set of leaves: CPU counts, the message buffer,
 * the host name, and the buffer cache knobs.  The meta operations translate
 * between names and object identifiers and enumerate the leaves.
 */

#include "kern/sysctl.h"
#include "kern/buf.h"
#include "kern/klog.h"
#include "kern/lock.h"

#include <errno.h>
#include <hal/hal.h>
#include <stdint.h>
#include <string.h>
#include <zedbsd/sysctl.h>

#define SYSCTL_NAME_MAX 64U

struct sysctl_leaf {
	int oid[3];
	unsigned oidlen;
	const char *name;
};

static const struct sysctl_leaf leaves[] = {
	{{ CTL_HW, HW_NCPU, 0 }, 2, "hw.ncpu"},
	{{ CTL_HW, HW_NCPUONLINE, 0 }, 2, "hw.ncpuonline"},
	{{ CTL_KERN, KERN_MSGBUF, 0 }, 2, "kern.msgbuf"},
	{{ CTL_KERN, KERN_MSGBUF_SIZE, 0 }, 2, "kern.msgbuf_size"},
	{{ CTL_KERN, KERN_MSGBUF_DROPPED, 0 }, 2, "kern.msgbuf_dropped"},
	{{ CTL_KERN, KERN_HOSTNAME, 0 }, 2, "kern.hostname"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_MAX_BYTES }, 3,
	 "vfs.bufcache.max_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_CURRENT_BYTES }, 3,
	 "vfs.bufcache.current_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_DIRTY_BYTES }, 3,
	 "vfs.bufcache.dirty_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_STATS }, 3,
	 "vfs.bufcache.stats"},
};

static struct spinlock hostname_lock;
static char hostname[ZEDBSD_HOST_NAME_MAX + 1U] = "zedbsd";

static int oid_compare(const int *a, unsigned alen, const int *b, unsigned blen);
static const struct sysctl_leaf *find_oid(const int *oid, unsigned oidlen);
static int sysctl_output(void *oldp, size_t *oldlenp, const void *value, size_t size);
static int sysctl_meta(int operation, void *oldp, size_t *oldlenp, const void *newp, size_t newlen);

/*
 * Initializes the host name lock.
 */
void
sysctl_init(
	void)
{
	spin_init(&hostname_lock, LOCK_RANK_DEVICE, "hostname");
}

/*
 * Reads or writes one sysctl leaf.
 *
 * Reads follow the usual protocol: with oldp NULL the required size is
 * reported, otherwise the value is copied when it fits.  Writes need the
 * superuser and an exact value size.
 */
int
kern_sysctl(
	const int *name,
	unsigned namelen,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen,
	int superuser)
{
	struct bufcache_stats stats;
	const char *new_name;
	uint64_t value;
	uint32_t cpus;
	unsigned long irq;
	size_t length;
	size_t i;
	size_t capacity;
	size_t needed;
	int error;

	/* Rejects an empty or overlong object identifier. */
	if (name == NULL || namelen == 0 || namelen > CTL_MAXNAME)
		return EINVAL;

	/* Routes the meta operations. */
	if (namelen == 2 && name[0] == CTL_SYSCTL) {
		error = sysctl_meta(name[1], oldp, oldlenp, newp, newlen);
		return error;
	}

	/* Reports the CPU count for both hardware leaves. */
	if (namelen == 2 &&
	    name[0] == CTL_HW &&
	    (name[1] == HW_NCPU || name[1] == HW_NCPUONLINE)) {
		cpus = hal_cpu_count();
		if (newp != NULL || newlen != 0)
			return EPERM;
		error = sysctl_output(oldp, oldlenp, &cpus, sizeof(cpus));
		return error;
	}

	/* Handles the kernel leaves. */
	if (namelen == 2 && name[0] == CTL_KERN) {
		/* The host name is the only writable kernel leaf. */
		if (name[1] == KERN_HOSTNAME) {
			if (newp != NULL) {
				new_name = newp;
				if (!superuser)
					return EPERM;
				if (newlen == 0 || newlen > ZEDBSD_HOST_NAME_MAX)
					return EINVAL;

				/* Rejects a name with a terminator, slash, or whitespace. */
				for (i = 0; i < newlen; i++) {
					if (new_name[i] == '\0' ||
					    new_name[i] == '/' ||
					    new_name[i] == ' ' ||
					    new_name[i] == '\t' ||
					    new_name[i] == '\n')
						return EINVAL;
				}

				/* Installs the new name. */
				irq = spin_lock_irqsave(&hostname_lock);
				memcpy(hostname, new_name, newlen);
				hostname[newlen] = '\0';
				spin_unlock_irqrestore(&hostname_lock, irq);
			} else if (newlen != 0) {
				return EINVAL;
			}

			/* Reports the current name with its terminator. */
			irq = spin_lock_irqsave(&hostname_lock);
			length = strlen(hostname) + 1U;
			error = sysctl_output(oldp, oldlenp, hostname, length);
			spin_unlock_irqrestore(&hostname_lock, irq);
			return error;
		}

		/* Every other kernel leaf is read-only. */
		if (newp != NULL || newlen != 0)
			return EPERM;

		/* Reports the message buffer capacity. */
		if (name[1] == KERN_MSGBUF_SIZE) {
			value = kern_log_capacity();
			error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
			return error;
		}

		/* Reports the bytes dropped from the message buffer. */
		if (name[1] == KERN_MSGBUF_DROPPED) {
			(void)kern_log_snapshot(NULL, 0, &value);
			error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
			return error;
		}

		/* Copies the message buffer, sizing it on a NULL buffer. */
		if (name[1] == KERN_MSGBUF) {
			if (oldlenp == NULL) {
				if (oldp == NULL)
					return 0;
				return EINVAL;
			}
			capacity = *oldlenp;
			needed = kern_log_snapshot(NULL, 0, NULL);
			*oldlenp = needed;
			if (oldp == NULL)
				return 0;
			if (capacity < needed)
				return ENOMEM;
			needed = kern_log_snapshot(oldp, capacity, NULL);
			*oldlenp = needed;
			if (needed > capacity)
				return ENOMEM;
			return 0;
		}

		/* Reports an unknown kernel leaf. */
		return ENOENT;
	}

	/* Everything else must be a known buffer cache leaf. */
	if (namelen != 3 ||
	    name[0] != CTL_VFS ||
	    name[1] != VFS_BUFCACHE ||
	    find_oid(name, namelen) == NULL)
		return ENOENT;

	/* Serves the buffer cache leaves from one statistics snapshot. */
	buf_get_stats(&stats);
	switch (name[2]) {
	case VFS_BUFCACHE_MAX_BYTES:
		/* Reports the limit, then applies a superuser's new limit. */
		value = stats.max_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		if (error != 0)
			return error;
		if (newp == NULL) {
			if (newlen == 0)
				return 0;
			return EINVAL;
		}
		if (!superuser)
			return EPERM;
		if (newlen != sizeof(value))
			return EINVAL;
		memcpy(&value, newp, sizeof(value));
		error = buf_set_max_bytes(value);
		return error;
	case VFS_BUFCACHE_CURRENT_BYTES:
	case VFS_BUFCACHE_DIRTY_BYTES:
		if (newp != NULL || newlen != 0)
			return EPERM;
		if (name[2] == VFS_BUFCACHE_CURRENT_BYTES)
			value = stats.current_bytes;
		else
			value = stats.dirty_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		return error;
	case VFS_BUFCACHE_STATS:
		if (newp != NULL || newlen != 0)
			return EPERM;
		error = sysctl_output(oldp, oldlenp, &stats, sizeof(stats));
		return error;
	default:
		break;
	}

	/* Reports an unknown buffer cache leaf. */
	return ENOENT;
}

/* Orders two object identifiers lexicographically. */
static int
oid_compare(
	const int *a,
	unsigned alen,
	const int *b,
	unsigned blen)
{
	unsigned i;
	unsigned count;

	/* Compares the common prefix element by element. */
	count = blen;
	if (alen < blen)
		count = alen;
	for (i = 0; i < count; i++) {
		if (a[i] < b[i])
			return -1;
		if (a[i] > b[i])
			return 1;
	}

	/* A shorter identifier with an equal prefix orders first. */
	if (alen < blen)
		return -1;
	if (alen > blen)
		return 1;

	/* Reports equal identifiers. */
	return 0;
}

/* Finds the leaf with an exact object identifier. */
static const struct sysctl_leaf *
find_oid(
	const int *oid,
	unsigned oidlen)
{
	unsigned i;

	/* Searches the fixed leaf table. */
	for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
		if (oid_compare(oid, oidlen, leaves[i].oid, leaves[i].oidlen) == 0)
			return &leaves[i];
	}

	/* Reports an unknown identifier. */
	return NULL;
}

/* Copies a value out following the sysctl sizing protocol. */
static int
sysctl_output(
	void *oldp,
	size_t *oldlenp,
	const void *value,
	size_t size)
{
	size_t capacity;

	/* Without a length there is nothing to size or copy. */
	if (oldlenp == NULL) {
		if (oldp == NULL)
			return 0;
		return EINVAL;
	}

	/* Reports the required size before copying. */
	capacity = *oldlenp;
	*oldlenp = size;
	if (oldp == NULL)
		return 0;
	if (capacity < size)
		return ENOMEM;

	/* Copies the value. */
	memcpy(oldp, value, size);

	/* Reports the copied value. */
	return 0;
}

/* Handles the name, identifier, and enumeration meta operations. */
static int
sysctl_meta(
	int operation,
	void *oldp,
	size_t *oldlenp,
	const void *newp,
	size_t newlen)
{
	const struct sysctl_leaf *leaf;
	const struct sysctl_leaf *next;
	const char *name;
	const int *current;
	unsigned current_len;
	unsigned i;
	int error;

	/* Translates a terminated name to its identifier. */
	if (operation == CTL_SYSCTL_NAME2OID) {
		name = newp;
		if (name == NULL ||
		    newlen == 0 ||
		    newlen > SYSCTL_NAME_MAX ||
		    name[newlen - 1U] != '\0')
			return EINVAL;
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
			if (!strcmp(name, leaves[i].name)) {
				error = sysctl_output(oldp, oldlenp, leaves[i].oid,
				    leaves[i].oidlen * sizeof(int));
				return error;
			}
		}
		return ENOENT;
	}

	/* Translates an identifier to its terminated name. */
	if (operation == CTL_SYSCTL_OIDNAME) {
		if (newp == NULL ||
		    newlen == 0 ||
		    newlen % sizeof(int) != 0 ||
		    newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		leaf = find_oid(newp, (unsigned)(newlen / sizeof(int)));
		if (leaf == NULL)
			return ENOENT;
		error = sysctl_output(oldp, oldlenp, leaf->name, strlen(leaf->name) + 1U);
		return error;
	}

	/* Reports the smallest identifier after the given one. */
	if (operation == CTL_SYSCTL_NEXT) {
		current = newp;
		next = NULL;
		if (newlen % sizeof(int) != 0 || newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		current_len = (unsigned)(newlen / sizeof(int));
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
			if (current_len != 0 &&
			    oid_compare(leaves[i].oid, leaves[i].oidlen,
			    current, current_len) <= 0)
				continue;
			if (next != NULL &&
			    oid_compare(leaves[i].oid, leaves[i].oidlen,
			    next->oid, next->oidlen) >= 0)
				continue;
			next = &leaves[i];
		}
		if (next == NULL)
			return ENOENT;
		error = sysctl_output(oldp, oldlenp, next->oid, next->oidlen * sizeof(int));
		return error;
	}

	/* Reports an unknown meta operation. */
	return ENOENT;
}
