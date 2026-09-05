/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/disk.h"
#include "kern/backing-claim.h"
#include "kern/buf.h"
#include "kern/sched.h"
#include "kern/atomic.h"
#include "kern/test-checkpoint.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>
#include <zedbsd/block.h>

_Static_assert(DISK_NAME_MAX >= sizeof("nvme0n4294967295"),
    "DISK_NAME_MAX must represent every 32-bit NVMe namespace ID");

extern struct thread *thread_current(void) __attribute__((weak));
extern bool hal_irq_disable(void) __attribute__((weak));
extern void hal_irq_enable(void) __attribute__((weak));

#define DISK_ALLOCATED 1U
#define DISK_LIVE 2U
#define DISK_GONE 3U
#define DISK_HIGH __attribute__((section(".hightext")))
#ifdef ZEDBSD_STORAGE_HOST_TEST
#undef DISK_HIGH
#define DISK_HIGH
#endif

static struct disk disks[DISK_MAX];
static uint8_t disk_used[DISK_MAX];
static struct disk *disk_head;
static unsigned live_count;
static dev_t next_dev = 1;
static atomic_uint_t disk_registry_lock;

static bool
disk_lock(void)
{
	bool enabled = hal_irq_disable != NULL ? hal_irq_disable() : false;
	while (!atomic_try_acquire_zero(&disk_registry_lock))
		hal_compiler_barrier();
	return enabled;
}

static void
disk_unlock(bool enabled)
{
	atomic_store_release(&disk_registry_lock, 0);
	if (enabled && hal_irq_enable != NULL)
		hal_irq_enable();
}

static int
disk_index(const struct disk *disk)
{
	unsigned i;
	for (i = 0; i < DISK_MAX; i++)
		if (&disks[i] == disk)
			return (int)i;
	return -1;
}

static void
zero_bytes(void *p, size_t n)
{
	uint8_t *q = p;
	while (n--)
		*q++ = 0;
}

static int
name_equal(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b)
		a++, b++;
	return *a == *b;
}

static int
name_valid(const char name[DISK_NAME_MAX])
{
	unsigned i;
	if (name[0] == '\0')
		return 0;
	for (i = 0; i < DISK_NAME_MAX; i++)
		if (name[i] == '\0')
			return 1;
	return 0;
}

/* Called with registry lock held. Only physical ancestry is followed here;
 * loop/file consumers are independently covered by backing claims/opens. */
static struct disk *
disk_leaf(struct disk *disk)
{
	while (disk->d_parent != NULL)
		disk = disk->d_parent;
	return disk;
}

static int
disk_reload_idle(struct disk *parent)
{
	struct disk *d;
	if (parent->d_open_count != 1 || parent->d_opening != 0 ||
	    parent->d_closing != 0 || parent->d_inflight != 0 ||
	    parent->d_cache_users != 0)
		return EBUSY;
	for (d = disk_head; d != NULL; d = d->d_next) {
		if (d == parent || disk_leaf(d) != parent)
			continue;
		/* Refuse nested published devices as well as open/mounted children. */
		if (d->d_parent != parent || !(d->d_flags & DISK_PARTITION) ||
		    d->d_open_count || d->d_opening || d->d_closing ||
		    refcount_load(&d->d_refs) != 1)
			return EBUSY;
	}
	return 0;
}

int
disk_reload_begin(struct disk *parent)
{
	struct thread *owner = thread_current != NULL ? thread_current() : NULL;
	bool enabled = disk_lock();
	int error;
	if (parent == NULL || disk_index(parent) < 0 ||
	    parent->d_state != DISK_LIVE || parent->d_parent != NULL ||
	    (parent->d_flags & DISK_PARTITION) != 0 || owner == NULL)
		error = EINVAL;
	else if (parent->d_reload_owner != NULL)
		error = EBUSY;
	else {
		error = disk_reload_idle(parent);
		if (error == 0)
			parent->d_reload_owner = owner;
	}
	disk_unlock(enabled);
	return error;
}

void
disk_reload_end(struct disk *parent)
{
	bool enabled = disk_lock();
	if (parent != NULL && thread_current != NULL &&
	    parent->d_reload_owner == thread_current())
		parent->d_reload_owner = NULL;
	disk_unlock(enabled);
}

int
disk_reload_replace(struct disk *parent, struct disk **new_disks, unsigned count)
{
	struct disk *d, **link, **tail;
	unsigned i, j;
	bool enabled = disk_lock();
	int error = EINVAL;
	if (parent == NULL || count > DISK_MAX ||
	    (count != 0 && new_disks == NULL) || thread_current == NULL ||
	    parent->d_reload_owner != thread_current())
		goto out;
	error = disk_reload_idle(parent);
	if (error != 0)
		goto out;
	if (next_dev == 0 || count > UINT32_MAX - (uint32_t)next_dev) {
		error = ENOSPC;
		goto out;
	}
	for (i = 0; i < count; i++) {
		d = new_disks[i];
		if (d == NULL || disk_index(d) < 0 || d->d_state != DISK_ALLOCATED ||
		    d->d_parent != parent || !(d->d_flags & DISK_PARTITION) ||
		    d->d_block_size != parent->d_block_size || !name_valid(d->d_name) ||
		    d->d_block_count == 0 || d->d_parent_offset >= parent->d_block_count ||
		    d->d_block_count > parent->d_block_count - d->d_parent_offset) {
			error = EINVAL;
			goto out;
		}
		for (j = 0; j < i; j++)
			if (name_equal(d->d_name, new_disks[j]->d_name)) {
				error = EEXIST;
				goto out;
			}
		for (struct disk *other = disk_head; other != NULL; other = other->d_next)
			if (other->d_parent != parent && name_equal(d->d_name, other->d_name)) {
				error = EEXIST;
				goto out;
			}
	}
	/* Everything that can fail precedes this one namespace commit. Old slots
	 * stay allocated/GONE until the owner releases their partition records. */
	for (link = &disk_head; (d = *link) != NULL;) {
		if (d->d_parent != parent) {
			link = &d->d_next;
			continue;
		}
		*link = d->d_next;
		d->d_next = NULL;
		d->d_state = DISK_GONE;
		live_count--;
	}
	tail = link;
	for (i = 0; i < count; i++) {
		d = new_disks[i];
		d->d_dev = next_dev++;
		d->d_state = DISK_LIVE;
		d->d_next = NULL;
		refcount_get(&parent->d_refs);
		*tail = d;
		tail = &d->d_next;
		live_count++;
	}
	parent->d_identity_valid = 0;
	error = 0;
out:
	disk_unlock(enabled);
	return error;
}

struct disk *
disk_alloc(void)
{
	unsigned i;
	bool enabled = disk_lock();
	for (i = 0; i < DISK_MAX; i++) {
		if (disk_used[i])
			continue;
		disk_used[i] = 1;
		zero_bytes(&disks[i], sizeof(disks[i]));
		disks[i].d_state = DISK_ALLOCATED;
		refcount_init(&disks[i].d_refs, 1);
		spin_init(&disks[i].d_lock, LOCK_RANK_DISK, "disk");
		waitq_init(&disks[i].d_waitq, "disk state");
		disk_unlock(enabled);
		return &disks[i];
	}
	disk_unlock(enabled);
	return NULL;
}

static int
sd_name(char name[DISK_NAME_MAX], unsigned number)
{
	char reverse[DISK_NAME_MAX];
	unsigned count = 0, at = 2, i;

	name[0] = 's';
	name[1] = 'd';
	do {
		if (count >= sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('a' + number % 26U);
		number /= 26U;
		if (number != 0)
			number--;
	} while (number != 0);
	if (at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[at++] = reverse[count - i - 1U];
	name[at] = '\0';
	return 0;
}

int
disk_alloc_sd_name(struct disk *disk)
{
	char candidate[DISK_NAME_MAX];
	unsigned number, i;
	bool enabled;

	if (disk == NULL)
		return EINVAL;
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}
	for (number = 0; number < DISK_MAX; number++) {
		int used = 0;
		if (sd_name(candidate, number) != 0)
			break;
		for (i = 0; i < DISK_MAX; i++)
			if (disk_used[i] && &disks[i] != disk &&
			    name_equal(disks[i].d_name, candidate)) {
				used = 1;
				break;
			}
		if (!used) {
			for (i = 0; i < DISK_NAME_MAX; i++) {
				disk->d_name[i] = candidate[i];
				if (candidate[i] == '\0')
					break;
			}
			disk_unlock(enabled);
			return 0;
		}
	}
	disk_unlock(enabled);
	return ENOSPC;
}

static int
name_append_unsigned(char name[DISK_NAME_MAX], unsigned *at, unsigned value)
{
	char reverse[10];
	unsigned count = 0, i;

	do {
		if (count == sizeof(reverse))
			return ENAMETOOLONG;
		reverse[count++] = (char)('0' + value % 10U);
		value /= 10U;
	} while (value != 0);
	if (*at + count >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	for (i = 0; i < count; i++)
		name[(*at)++] = reverse[count - i - 1U];
	return 0;
}

int
disk_alloc_nvme_name(struct disk *disk, unsigned controller,
	unsigned namespace_id)
{
	char candidate[DISK_NAME_MAX];
	unsigned at = 0, i;
	bool enabled;

	if (disk == NULL || namespace_id == 0)
		return EINVAL;
	candidate[at++] = 'n';
	candidate[at++] = 'v';
	candidate[at++] = 'm';
	candidate[at++] = 'e';
	if (name_append_unsigned(candidate, &at, controller) != 0 ||
	    at + 1U >= DISK_NAME_MAX)
		return ENAMETOOLONG;
	candidate[at++] = 'n';
	if (name_append_unsigned(candidate, &at, namespace_id) != 0)
		return ENAMETOOLONG;
	candidate[at] = '\0';

	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}
	for (i = 0; i < DISK_MAX; i++)
		if (disk_used[i] && &disks[i] != disk &&
		    name_equal(disks[i].d_name, candidate)) {
			disk_unlock(enabled);
			return EEXIST;
		}
	for (i = 0; i <= at; i++)
		disk->d_name[i] = candidate[i];
	disk_unlock(enabled);
	return 0;
}

int
disk_create(struct disk *disk)
{
	struct disk **tail;
	struct disk *found;
	bool enabled;
	if (disk == NULL || disk->d_state != DISK_ALLOCATED ||
	    !name_valid(disk->d_name) || disk->d_block_size == 0 ||
	    disk->d_block_count == 0 ||
	    (disk->d_parent == NULL &&
	     (disk->d_ops == NULL || disk->d_ops->submit == NULL)))
		return EINVAL;
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_ALLOCATED) {
		disk_unlock(enabled);
		return EINVAL;
	}
	if (disk->d_parent != NULL && disk_leaf(disk)->d_reload_owner != NULL) {
		disk_unlock(enabled);
		return EBUSY;
	}
	for (found = disk_head; found != NULL; found = found->d_next)
		if (name_equal(found->d_name, disk->d_name)) {
			disk_unlock(enabled);
			return EEXIST;
		}
	if (next_dev == 0) {
		disk_unlock(enabled);
		return ENOSPC;
	}
	disk->d_dev = next_dev++;
	disk->d_state = DISK_LIVE;
	disk->d_next = NULL;
	if (disk->d_parent != NULL)
		refcount_get(&disk->d_parent->d_refs);
	for (tail = &disk_head; *tail != NULL; tail = &(*tail)->d_next)
		;
	*tail = disk;
	live_count++;
	disk_unlock(enabled);
	return 0;
}

void
disk_gone(struct disk *disk)
{
	struct disk **link;
	struct backing_mutation_guard guard;
	bool enabled;

	if (disk == NULL ||
	    backing_mutation_begin_disk(disk, 0, disk->d_block_count, NULL,
					&guard) != 0)
		return;
	enabled = disk_lock();
	if (disk_leaf(disk)->d_reload_owner != NULL)
		goto out;
	if (disk->d_state == DISK_GONE)
		goto out;
	if (disk->d_state != DISK_LIVE)
		goto out;
	for (link = &disk_head; *link != NULL; link = &(*link)->d_next) {
		if (*link == disk) {
			*link = disk->d_next;
			live_count--;
			break;
		}
	}
	disk->d_next = NULL;
	disk->d_state = DISK_GONE;
out:
	disk_unlock(enabled);
	backing_mutation_end(&guard);
}

DISK_HIGH int
disk_gone_if_idle(struct disk *disk)
{
	struct disk **link;
	struct backing_mutation_guard guard;
	bool enabled;
	int error;

	if (disk == NULL)
		return EINVAL;
	error = backing_mutation_begin_disk(disk, 0, disk->d_block_count, NULL,
					    &guard);
	if (error != 0)
		return error;
	enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		error = ENXIO;
		goto out;
	}
	if (disk->d_open_count != 0 || disk->d_inflight != 0 ||
	    disk->d_opening || disk->d_closing ||
	    disk_leaf(disk)->d_reload_owner != NULL) {
		disk_unlock(enabled);
		error = EBUSY;
		goto out;
	}
	disk_unlock(enabled);
	/* Resident buffers pin their leaf disk.  Flush and invalidate them
	 * before applying the final external-reference test. */
	error = buf_invalidate_disk(disk, 0);
	if (error != 0)
		goto out;
	enabled = disk_lock();
	if (disk_index(disk) < 0 || disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		error = ENXIO;
		goto out;
	}
	if (disk->d_open_count != 0 || disk->d_inflight != 0 ||
	    disk->d_opening || disk->d_closing ||
	    disk_leaf(disk)->d_reload_owner != NULL ||
	    refcount_load(&disk->d_refs) != 1) {
		disk_unlock(enabled);
		error = EBUSY;
		goto out;
	}
	for (link = &disk_head; *link != NULL; link = &(*link)->d_next)
		if (*link == disk) {
			*link = disk->d_next;
			live_count--;
			break;
		}
	disk->d_next = NULL;
	disk->d_state = DISK_GONE;
	disk_unlock(enabled);
	error = 0;
out:
	backing_mutation_end(&guard);
	return error;
}

DISK_HIGH int
disk_destroy(struct disk *disk)
{
	int i;
	struct disk *parent;
	bool enabled = disk_lock();
	i = disk_index(disk);
	if (i < 0 || !disk_used[i]) {
		disk_unlock(enabled);
		return EINVAL;
	}
	if (disk->d_state == DISK_LIVE) {
		disk_unlock(enabled);
		return EBUSY;
	}
	if (disk->d_state != DISK_ALLOCATED && disk->d_state != DISK_GONE) {
		disk_unlock(enabled);
		return EINVAL;
	}
	if (disk->d_open_count != 0 || disk->d_inflight != 0 ||
	    disk->d_opening || disk->d_closing ||
	    refcount_load(&disk->d_refs) != 1) {
		disk_unlock(enabled);
		return EBUSY;
	}
	parent = disk->d_parent;
	if (disk->d_parent != NULL)
		(void)refcount_put_not_last(&parent->d_refs);
	zero_bytes(disk, sizeof(*disk));
	disk_used[i] = 0;
	disk_unlock(enabled);
	return 0;
}

struct disk *
disk_find(const char *name)
{
	struct disk *disk;
	bool enabled;
	if (name == NULL)
		return NULL;
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (name_equal(disk->d_name, name))
			break;
	if (disk != NULL) {
		KERN_TEST_CHECKPOINT(KERN_TEST_DISK_LOOKUP_BEFORE_REF, disk);
		refcount_get(&disk->d_refs);
	}
	disk_unlock(enabled);
	return disk;
}

struct disk *
disk_find_by_dev(dev_t dev)
{
	struct disk *disk;
	bool enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (disk->d_dev == dev)
			break;
	if (disk != NULL)
		refcount_get(&disk->d_refs);
	disk_unlock(enabled);
	return disk;
}

unsigned
disk_count(void)
{
	unsigned count;
	bool enabled = disk_lock();
	count = live_count;
	disk_unlock(enabled);
	return count;
}

unsigned
disk_inflight_count(void)
{
	struct disk *disk;
	unsigned count = 0;
	bool enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next) {
		unsigned long irq = spin_lock_irqsave(&disk->d_lock);
		count += disk->d_inflight;
		spin_unlock_irqrestore(&disk->d_lock, irq);
	}
	disk_unlock(enabled);
	return count;
}

struct disk *
disk_at(unsigned index)
{
	struct disk *disk;
	bool enabled = disk_lock();
	for (disk = disk_head; disk != NULL && index != 0;
	     disk = disk->d_next, index--)
		;
	if (disk != NULL)
		refcount_get(&disk->d_refs);
	disk_unlock(enabled);
	return disk;
}

void
disk_ref(struct disk *disk)
{
	bool enabled = disk_lock();
	if (disk != NULL && disk_index(disk) >= 0)
		refcount_get(&disk->d_refs);
	disk_unlock(enabled);
}

void
disk_release(struct disk *disk)
{
	bool enabled = disk_lock();
	if (disk != NULL && disk_index(disk) >= 0)
		(void)refcount_put_not_last(&disk->d_refs);
	disk_unlock(enabled);
}

void
disk_registry_reset(void)
{
	unsigned i;
	buf_reset();
	bool enabled = disk_lock();
	zero_bytes(disks, sizeof(disks));
	for (i = 0; i < DISK_MAX; i++)
		disk_used[i] = 0;
	disk_head = NULL;
	live_count = 0;
	next_dev = 1;
	disk_unlock(enabled);
}

static void
disk_copy_info(const struct disk *disk, struct disk_info *info)
{
	unsigned i;
	for (i = 0; i < DISK_NAME_MAX; i++)
		info->name[i] = disk->d_name[i];
	info->dev = disk->d_dev;
	info->flags = disk->d_flags;
	info->block_size = disk->d_block_size;
	info->block_count = disk->d_block_count;
}

DISK_HIGH int
disk_block_info(struct disk *disk, struct zedbsd_block_info *info)
{
	bool enabled;
	unsigned i;
	if (info == NULL || info->version != ZEDBSD_BLOCK_VERSION ||
	    info->struct_size != sizeof(*info))
		return EINVAL;
	for (i = 0; i < 4; i++)
		if (info->reserved[i] != 0)
			return EINVAL;
	enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	memset(info, 0, sizeof(*info));
	info->version = ZEDBSD_BLOCK_VERSION;
	info->struct_size = sizeof(*info);
	info->device = (uint32_t)disk->d_dev;
	info->parent_device = disk->d_parent != NULL ?
	    (uint32_t)disk->d_parent->d_dev : 0;
	info->flags = disk->d_flags & (DISK_READ_ONLY | DISK_REMOVABLE |
	    DISK_PARTITION);
	info->sector_size = disk->d_block_size;
	info->sector_count = disk->d_block_count;
	info->parent_offset = disk->d_parent_offset;
	memcpy(info->name, disk->d_name, sizeof(info->name));
	disk_unlock(enabled);
	return 0;
}

DISK_HIGH int
disk_get_info(const char *name, struct disk_info *result)
{
	struct disk *disk;
	bool enabled;
	if (name == NULL || result == NULL)
		return EINVAL;
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (name_equal(name, disk->d_name)) {
			disk_copy_info(disk, result);
			disk_unlock(enabled);
			return 0;
		}
	disk_unlock(enabled);
	return ENOENT;
}

DISK_HIGH int
disk_registry_snapshot(struct disk_info *entries, unsigned capacity,
		       unsigned *count_out)
{
	struct disk *disk;
	unsigned count = 0;
	bool enabled;
	if (count_out == NULL || (capacity != 0 && entries == NULL))
		return EINVAL;
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		count++;
	*count_out = count;
	if (count > capacity) {
		disk_unlock(enabled);
		return ENOSPC;
	}
	count = 0;
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		disk_copy_info(disk, &entries[count++]);
	disk_unlock(enabled);
	return 0;
}

int
disk_open(struct disk *disk)
{
	int error = 0;
	bool enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	if (disk_leaf(disk)->d_reload_owner != NULL) {
		disk_unlock(enabled);
		return EBUSY;
	}
	disk->d_opening++;
	refcount_get(&disk->d_refs); /* Temporary lifetime reference. */
	disk_unlock(enabled);
	if (disk->d_ops != NULL && disk->d_ops->open != NULL)
		error = disk->d_ops->open(disk);
	enabled = disk_lock();
	disk->d_opening--;
	if (error == 0) {
		if (disk->d_state != DISK_LIVE)
			error = ENXIO;
		else {
			disk->d_open_count++;
			refcount_get(&disk->d_refs);
		}
	}
	(void)refcount_put_not_last(&disk->d_refs);
	disk_unlock(enabled);
	if (error == ENXIO && disk->d_ops != NULL && disk->d_ops->close != NULL)
		disk->d_ops->close(disk);
	return error;
}

DISK_HIGH int
disk_open_by_dev(dev_t dev, struct disk **result)
{
	struct disk *disk;
	int error;
	bool enabled;
	if (result == NULL)
		return EINVAL;
	*result = NULL;
	enabled = disk_lock();
	for (disk = disk_head; disk != NULL; disk = disk->d_next)
		if (disk->d_dev == dev)
			break;
	if (disk == NULL) {
		disk_unlock(enabled);
		return ENXIO;
	}
	refcount_get(&disk->d_refs);
	disk_unlock(enabled);
	error = disk_open(disk);
	disk_release(disk);
	if (error == 0)
		*result = disk;
	return error;
}

void
disk_close(struct disk *disk)
{
	bool enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 || disk->d_open_count == 0) {
		disk_unlock(enabled);
		return;
	}
	disk->d_open_count--;
	disk->d_closing++;
	disk_unlock(enabled);
	if (disk->d_ops != NULL && disk->d_ops->close != NULL)
		disk->d_ops->close(disk);
	enabled = disk_lock();
	disk->d_closing--;
	disk_unlock(enabled);
	disk_release(disk);
}

int
disk_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	int error;
	bool enabled = disk_lock();
	if (disk == NULL || disk_index(disk) < 0 ||
	    disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	refcount_get(&disk->d_refs);
	disk_unlock(enabled);
	if (disk->d_ops == NULL || disk->d_ops->ioctl == NULL) {
		disk_release(disk);
		return EOPNOTSUPP;
	}
	error = disk->d_ops->ioctl(disk, request, argument);
	disk_release(disk);
	return error;
}

int
bio_submit(struct disk *disk, struct bio *bio)
{
	struct disk *leaf;
	uint64_t mapped;
	int error;
	bool enabled;
	if (disk == NULL || bio == NULL || bio->b_state != BIO_NEW ||
	    disk->d_state != DISK_LIVE)
		return EINVAL;
	if (!bio->b_initialized) {
		spin_init(&bio->b_lock, LOCK_RANK_DISK, "bio");
		waitq_init(&bio->b_waitq, "bio completion");
		bio->b_initialized = 1;
	}
	if (bio->b_op != BIO_FLUSH) {
		if (bio->b_block_count == 0 || bio->b_data == NULL)
			return EINVAL;
		if (bio->b_block >= disk->d_block_count ||
		    bio->b_block_count > disk->d_block_count - bio->b_block)
			return EOVERFLOW;
		if (bio->b_op == BIO_WRITE && (disk->d_flags & DISK_READ_ONLY))
			return EROFS;
	} else if (bio->b_block_count != 0 || bio->b_data != NULL) {
		return EINVAL;
	}
	leaf = disk;
	mapped = bio->b_block;
	while (leaf->d_parent != NULL) {
		if (mapped >= leaf->d_block_count ||
		    bio->b_block_count > leaf->d_block_count - mapped ||
		    mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = leaf->d_parent;
		if (leaf->d_state != DISK_LIVE)
			return ENXIO;
	}
	if (leaf->d_ops == NULL || leaf->d_ops->submit == NULL)
		return EOPNOTSUPP;
	if (bio->b_op != BIO_FLUSH &&
	    (mapped >= leaf->d_block_count ||
	     bio->b_block_count > leaf->d_block_count - mapped))
		return EOVERFLOW;
	enabled = disk_lock();
	if (disk->d_state != DISK_LIVE || leaf->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	if (leaf->d_reload_owner != NULL &&
	    (disk != leaf || thread_current == NULL ||
	     leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}
	bio->b_disk = disk;
	bio->b_leaf_disk = leaf;
	bio->b_mapped_block = mapped;
	bio->b_transferred = 0;
	bio->b_error = 0;
	bio->b_state = BIO_SUBMITTED;
	leaf->d_inflight++;
	refcount_get(&leaf->d_refs);
	disk_unlock(enabled);
	error = leaf->d_ops->submit(leaf, bio);
	if (error != 0) {
		enabled = disk_lock();
		leaf->d_inflight--;
		(void)refcount_put_not_last(&leaf->d_refs);
		bio->b_state = BIO_NEW;
		bio->b_leaf_disk = NULL;
		disk_unlock(enabled);
	}
	return error;
}

void
bio_complete(struct bio *bio, int error, size_t transferred)
{
	struct disk *leaf;
	void (*done)(struct bio *);
	unsigned long bio_irq;
	bool enabled;
	if (bio == NULL || !bio->b_initialized)
		return;
	bio_irq = spin_lock_irqsave(&bio->b_lock);
	if (bio->b_state != BIO_SUBMITTED) {
		spin_unlock_irqrestore(&bio->b_lock, bio_irq);
		return;
	}
	leaf = bio->b_leaf_disk;
	bio->b_error = error;
	bio->b_transferred = transferred;
	bio->b_state = BIO_COMPLETED;
	waitq_wake_all(&bio->b_waitq);
	spin_unlock_irqrestore(&bio->b_lock, bio_irq);
	enabled = disk_lock();
	if (leaf != NULL && leaf->d_inflight != 0)
		leaf->d_inflight--;
	if (leaf != NULL)
		(void)refcount_put_not_last(&leaf->d_refs);
	done = bio->b_done;
	disk_unlock(enabled);
	if (done != NULL)
		done(bio);
}

int
disk_resolve_range(struct disk *disk, uint64_t block, uint32_t count,
		   struct disk **leaf_out, uint64_t *mapped_out)
{
	struct disk *leaf;
	uint64_t mapped;
	if (disk == NULL || leaf_out == NULL || mapped_out == NULL ||
	    count == 0)
		return EINVAL;
	if (disk->d_state != DISK_LIVE)
		return ENXIO;
	if (block >= disk->d_block_count || count > disk->d_block_count - block)
		return EOVERFLOW;
	leaf = disk;
	mapped = block;
	while (leaf->d_parent != NULL) {
		struct disk *parent = leaf->d_parent;
		if (parent->d_state != DISK_LIVE)
			return ENXIO;
		if (parent->d_block_size != leaf->d_block_size)
			return EOPNOTSUPP;
		if (mapped > UINT64_MAX - leaf->d_parent_offset)
			return EOVERFLOW;
		mapped += leaf->d_parent_offset;
		leaf = parent;
	}
	if (mapped >= leaf->d_block_count ||
	    count > leaf->d_block_count - mapped)
		return EOVERFLOW;
	*leaf_out = leaf;
	*mapped_out = mapped;
	return 0;
}

int
bio_wait(struct bio *bio)
{
	struct thread *thread;
	unsigned long irq;
	int error;

	if (bio == NULL || !bio->b_initialized)
		return EINVAL;
	thread = thread_current != NULL ? thread_current() : NULL;
	if (thread != NULL) {
		irq = spin_lock_irqsave(&bio->b_lock);
		while (bio->b_state == BIO_SUBMITTED) {
			uint64_t sequence = waitq_sequence(&bio->b_waitq);
			error = waitq_sleep(&bio->b_waitq, &bio->b_lock,
					    sequence, 0, 0);
			if (error != 0 && error != EAGAIN) {
				spin_unlock_irqrestore(&bio->b_lock, irq);
				return error;
			}
		}
		error = bio->b_state == BIO_COMPLETED ? bio->b_error : EINVAL;
		spin_unlock_irqrestore(&bio->b_lock, irq);
		return error;
	}
	for (;;) {
		irq = spin_lock_irqsave(&bio->b_lock);
		if (bio->b_state != BIO_SUBMITTED)
			break;
		spin_unlock_irqrestore(&bio->b_lock, irq);
		hal_compiler_barrier();
	}
	error = bio->b_state == BIO_COMPLETED ? bio->b_error : EINVAL;
	spin_unlock_irqrestore(&bio->b_lock, irq);
	return error;
}

int
bio_flush(struct disk *disk)
{
	struct bio bio = {.b_op = BIO_FLUSH};
	int error = bio_submit(disk, &bio);
	return error != 0 ? error : bio_wait(&bio);
}

static int
disk_transfer_direct(struct disk *disk, enum bio_op op, uint64_t block,
		     uint32_t count, void *data,
		     const struct backing_claim *claim)
{
	uint8_t *bytes = data;
	struct backing_mutation_guard guard;
	int guarded = 0;
	if (disk == NULL || data == NULL || count == 0 ||
	    disk->d_block_size == 0 ||
	    (size_t)count > SIZE_MAX / disk->d_block_size)
		return EINVAL;
	if (op == BIO_WRITE) {
		int error = backing_mutation_begin_disk(disk, block, count,
							claim, &guard);
		if (error != 0)
			return error;
		guarded = 1;
	}
	while (count != 0) {
		uint32_t chunk = count;
		struct bio bio;
		size_t expected;
		int error;
		if (disk->d_max_transfer_blocks != 0 &&
		    chunk > disk->d_max_transfer_blocks)
			chunk = disk->d_max_transfer_blocks;
		memset(&bio, 0, sizeof(bio));
		bio.b_op = op;
		bio.b_block = block;
		bio.b_block_count = chunk;
		bio.b_data = bytes;
		error = bio_submit(disk, &bio);
		if (error != 0) {
			if (guarded)
				backing_mutation_end(&guard);
			return error;
		}
		error = bio_wait(&bio);
		if (error != 0) {
			if (guarded)
				backing_mutation_end(&guard);
			return error;
		}
		expected = (size_t)chunk * disk->d_block_size;
		if (bio.b_transferred != expected) {
			if (guarded)
				backing_mutation_end(&guard);
			return EIO;
		}
		block += chunk;
		count -= chunk;
		bytes += expected;
	}
	if (guarded)
		backing_mutation_end(&guard);
	return 0;
}

int
disk_read_direct(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	return disk_transfer_direct(disk, BIO_READ, block, count, data, NULL);
}

int
disk_write_direct(struct disk *disk, uint64_t block, uint32_t count,
		  const void *data)
{
	return disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				    NULL);
}

int
disk_write_direct_claimed(struct disk *disk, uint64_t block, uint32_t count,
			  const void *data, const struct backing_claim *claim)
{
	if (claim == NULL)
		return EINVAL;
	return disk_transfer_direct(disk, BIO_WRITE, block, count, (void *)data,
				    claim);
}

int
disk_sync(struct disk *disk)
{
	int error = buf_sync(disk);
	return error != 0 ? error : bio_flush(disk);
}

int
disk_read(struct disk *disk, uint64_t block, uint32_t count, void *data)
{
	struct disk *leaf;
	bool enabled = disk_lock();
	int error;
	if (disk == NULL || disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	leaf = disk_leaf(disk);
	if (leaf->d_reload_owner != NULL && (disk != leaf ||
	    thread_current == NULL || leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}
	leaf->d_cache_users++;
	disk_unlock(enabled);
	error = buf_read(disk, block, count, data);
	enabled = disk_lock();
	leaf->d_cache_users--;
	disk_unlock(enabled);
	return error;
}

/* Keep cache-hit writes inside the same admission boundary as device I/O. */
static int
disk_cached_write(struct disk *disk, uint64_t block, uint32_t count,
		  const void *data)
{
	struct disk *leaf;
	bool enabled = disk_lock();
	int error;
	if (disk == NULL || disk->d_state != DISK_LIVE) {
		disk_unlock(enabled);
		return ENXIO;
	}
	leaf = disk_leaf(disk);
	if (leaf->d_reload_owner != NULL && (disk != leaf ||
	    thread_current == NULL || leaf->d_reload_owner != thread_current())) {
		disk_unlock(enabled);
		return EBUSY;
	}
	leaf->d_cache_users++;
	disk_unlock(enabled);
	error = buf_write(disk, block, count, data);
	enabled = disk_lock();
	leaf->d_cache_users--;
	disk_unlock(enabled);
	return error;
}

int
disk_write(struct disk *disk, uint64_t block, uint32_t count, const void *data)
{
	struct backing_mutation_guard guard;
	int error =
	    backing_mutation_begin_disk(disk, block, count, NULL, &guard);
	if (error != 0)
		return error;
	error = disk_cached_write(disk, block, count, data);
	backing_mutation_end(&guard);
	return error;
}

int
disk_write_filesystem(struct disk *disk, uint64_t block, uint32_t count,
		      const void *data)
{
	struct backing_mutation_guard guard;
	int error = backing_mutation_begin_disk_filesystem(disk, block, count,
							  &guard);
	if (error != 0)
		return error;
	error = disk_cached_write(disk, block, count, data);
	backing_mutation_end(&guard);
	return error;
}
