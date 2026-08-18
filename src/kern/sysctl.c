/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/sysctl.h"
#include "kern/buf.h"
#include "kern/klog.h"

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
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_MAX_BYTES }, 3,
	 "vfs.bufcache.max_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_CURRENT_BYTES }, 3,
	 "vfs.bufcache.current_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_DIRTY_BYTES }, 3,
	 "vfs.bufcache.dirty_bytes"},
	{{ CTL_VFS, VFS_BUFCACHE, VFS_BUFCACHE_STATS }, 3,
	 "vfs.bufcache.stats"},
};

void sysctl_init(void) { }

static int
oid_compare(const int *a, unsigned alen, const int *b, unsigned blen)
{
	unsigned i, count = alen < blen ? alen : blen;
	for (i = 0; i < count; i++) {
		if (a[i] < b[i]) return -1;
		if (a[i] > b[i]) return 1;
	}
	return alen < blen ? -1 : alen > blen;
}
static const struct sysctl_leaf *
find_oid(const int *oid, unsigned oidlen)
{
	unsigned i;
	for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++)
		if (oid_compare(oid, oidlen, leaves[i].oid,
		    leaves[i].oidlen) == 0)
			return &leaves[i];
	return NULL;
}

static int
sysctl_output(void *oldp, size_t *oldlenp, const void *value, size_t size)
{
	size_t capacity;
	if (oldlenp == NULL)
		return oldp == NULL ? 0 : EINVAL;
	capacity = *oldlenp;
	*oldlenp = size;
	if (oldp == NULL)
		return 0;
	if (capacity < size)
		return ENOMEM;
	memcpy(oldp, value, size);
	return 0;
}

static int
sysctl_meta(int operation, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen)
{
	unsigned i;
	if (operation == CTL_SYSCTL_NAME2OID) {
		const char *name = newp;
		if (name == NULL || newlen == 0 || newlen > SYSCTL_NAME_MAX ||
		    name[newlen - 1U] != '\0')
			return EINVAL;
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++)
			if (!strcmp(name, leaves[i].name))
				return sysctl_output(oldp, oldlenp, leaves[i].oid,
				    leaves[i].oidlen * sizeof(int));
		return ENOENT;
	}
	if (operation == CTL_SYSCTL_OIDNAME) {
		const struct sysctl_leaf *leaf;
		if (newp == NULL || newlen == 0 || newlen % sizeof(int) != 0 ||
		    newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		leaf = find_oid(newp, (unsigned)(newlen / sizeof(int)));
		return leaf == NULL ? ENOENT : sysctl_output(oldp, oldlenp,
		    leaf->name, strlen(leaf->name) + 1U);
	}
	if (operation == CTL_SYSCTL_NEXT) {
		const int *current = newp;
		unsigned current_len;
		const struct sysctl_leaf *next = NULL;
		if (newlen % sizeof(int) != 0 ||
		    newlen / sizeof(int) > CTL_MAXNAME)
			return EINVAL;
		current_len = (unsigned)(newlen / sizeof(int));
		for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++)
			if ((current_len == 0 || oid_compare(leaves[i].oid,
			    leaves[i].oidlen, current, current_len) > 0) &&
			    (next == NULL || oid_compare(leaves[i].oid,
			    leaves[i].oidlen, next->oid, next->oidlen) < 0))
				next = &leaves[i];
		return next == NULL ? ENOENT : sysctl_output(oldp, oldlenp,
		    next->oid, next->oidlen * sizeof(int));
	}
	return ENOENT;
}

int
kern_sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen, int superuser)
{
	struct zedbsd_bufcache_stats stats;
	uint64_t value;
	int error;
	if (name == NULL || namelen == 0 || namelen > CTL_MAXNAME)
		return EINVAL;
	if (namelen == 2 && name[0] == CTL_SYSCTL)
		return sysctl_meta(name[1], oldp, oldlenp, newp, newlen);
	if (namelen == 2 && name[0] == CTL_HW &&
	    (name[1] == HW_NCPU || name[1] == HW_NCPUONLINE)) {
		uint32_t cpus = hal_cpu_count();
		if (newp != NULL || newlen != 0)
			return EPERM;
		return sysctl_output(oldp, oldlenp, &cpus, sizeof(cpus));
	}
	if (namelen == 2 && name[0] == CTL_KERN) {
		if (newp != NULL || newlen != 0)
			return EPERM;
		if (name[1] == KERN_MSGBUF_SIZE) {
			value = kern_log_capacity();
			return sysctl_output(oldp, oldlenp, &value, sizeof(value));
		}
		if (name[1] == KERN_MSGBUF_DROPPED) {
			(void)kern_log_snapshot(NULL, 0, &value);
			return sysctl_output(oldp, oldlenp, &value, sizeof(value));
		}
		if (name[1] == KERN_MSGBUF) {
			size_t capacity, needed;
			if (oldlenp == NULL)
				return oldp == NULL ? 0 : EINVAL;
			capacity = *oldlenp;
			needed = kern_log_snapshot(NULL, 0, NULL);
			*oldlenp = needed;
			if (oldp == NULL)
				return 0;
			if (capacity < needed)
				return ENOMEM;
			needed = kern_log_snapshot(oldp, capacity, NULL);
			*oldlenp = needed;
			return needed <= capacity ? 0 : ENOMEM;
		}
		return ENOENT;
	}
	if (namelen != 3 || name[0] != CTL_VFS ||
	    name[1] != VFS_BUFCACHE || find_oid(name, namelen) == NULL)
		return ENOENT;
	buf_get_stats(&stats);
	switch (name[2]) {
	case VFS_BUFCACHE_MAX_BYTES:
		value = stats.max_bytes;
		error = sysctl_output(oldp, oldlenp, &value, sizeof(value));
		if (error != 0)
			return error;
		if (newp == NULL)
			return newlen == 0 ? 0 : EINVAL;
		if (!superuser)
			return EPERM;
		if (newlen != sizeof(value))
			return EINVAL;
		memcpy(&value, newp, sizeof(value));
		return buf_set_max_bytes(value);
	case VFS_BUFCACHE_CURRENT_BYTES:
	case VFS_BUFCACHE_DIRTY_BYTES:
		if (newp != NULL || newlen != 0)
			return EPERM;
		value = name[2] == VFS_BUFCACHE_CURRENT_BYTES ?
		    stats.current_bytes : stats.dirty_bytes;
		return sysctl_output(oldp, oldlenp, &value, sizeof(value));
	case VFS_BUFCACHE_STATS:
		if (newp != NULL || newlen != 0)
			return EPERM;
		return sysctl_output(oldp, oldlenp, &stats, sizeof(stats));
	default:
		return ENOENT;
	}
}
