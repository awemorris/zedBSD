/* -*- mode: c; c-file-style: "bsd"; indent-tabs-mode: t; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "kern/backing-claim.h"
#include "kern/block-identity.h"
#include "kern/cdev.h"
#include "kern/devfs.h"
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/namei.h"
#include "kern/poll.h"
#include "kern/uaccess.h"
#include <zedbsd/block.h>
#include <kern/cred.h>
#include <kern/partition.h>
#include "kern/tty.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_INODE_MAX 64U

int copyin(uintptr_t u, void *k, size_t n)
{ if (!u) return EFAULT; memcpy(k, (const void *)u, n); return 0; }
int copyout(const void *k, uintptr_t u, size_t n)
{ if (!u) return EFAULT; memcpy((void *)u, k, n); return 0; }
int disk_block_info(struct disk *d, struct zedbsd_block_info *i)
{ (void)d; (void)i; return EOPNOTSUPP; }
int partition_reload(struct disk *d) { (void)d; return EOPNOTSUPP; }
struct ucred *cred_current_ref(void) { return NULL; }
int cred_is_superuser(const struct ucred *c) { (void)c; return 0; }
void cred_release(struct ucred *c) { (void)c; }

struct test_payload {
	unsigned value;
	unsigned finalized;
};

struct retained_generation {
	const char *name;
	struct cdev *found;
	struct cdev *snapshot[CDEV_MAX];
	unsigned snapshot_count;
	unsigned snapshot_mode;
	volatile unsigned ready;
	volatile unsigned release;
	volatile unsigned observed_unpublished;
};

static struct inode *test_inodes[TEST_INODE_MAX];
static pthread_mutex_t test_inode_lock = PTHREAD_MUTEX_INITIALIZER;
static int test_inode_allocations_before_failure = -1;

static void test_inode_destroy(struct inode *inode);
static void test_inode_evict(struct mount *mountp, ino_t number);
static void test_inode_purge_mount(struct mount *mountp);
static int lookup_name(struct inode *directory, const char *name,
	struct inode **result);
static void payload_finalize(void *data);
static int payload_poll(struct file *file, short requested, short *returned);
static ssize_t payload_read(struct file *file, void *buffer, size_t size);
static void test_generation_lifetime(struct mount *mountp);
static void test_legacy_registration(void);
static void test_registry_capacity_and_reset(void);
static void test_concurrent_lookup_snapshot_reset(void);
static void test_fixed_directory_recreation(struct mount *mountp);
static void test_mount_failure_cleanup(void);
static void *retain_generation_worker(void *data);

static const struct cdev_ops payload_ops = {
	.poll = payload_poll,
	.read = payload_read,
};

const struct file_ops tty_pty_slave_file_ops = { 0 };

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	lock->held.value = 0;
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U,
	    __ATOMIC_ACQUIRE) != 0)
		sched_yield();

	return 1;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	assert(enabled == 1);
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	memset(mutex, 0, sizeof(*mutex));
	spin_init(&mutex->guard, rank, name);
	return 0;
}

void
mutex_lock(
	struct mutex *mutex)
{
	(void)spin_lock_irqsave(&mutex->guard);
	mutex->locked = 1U;
}

void
mutex_unlock(
	struct mutex *mutex)
{
	assert(mutex->locked != 0);
	mutex->locked = 0;
	spin_unlock_irqrestore(&mutex->guard, 1U);
}

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *data)
{
	free(data);
}

struct inode *
inode_alloc(
	struct mount *mountp)
{
	struct inode *inode;
	unsigned index;

	if (test_inode_allocations_before_failure == 0)
		return NULL;
	if (test_inode_allocations_before_failure > 0)
		test_inode_allocations_before_failure--;
	inode = calloc(1, sizeof(*inode));
	if (inode == NULL)
		return NULL;

	inode->i_mount = mountp;
	refcount_init(&inode->i_refs, 2);

	assert(pthread_mutex_lock(&test_inode_lock) == 0);
	for (index = 0; index < TEST_INODE_MAX; index++) {
		if (test_inodes[index] == NULL) {
			test_inodes[index] = inode;
			break;
		}
	}
	assert(pthread_mutex_unlock(&test_inode_lock) == 0);
	if (index == TEST_INODE_MAX) {
		free(inode);
		return NULL;
	}

	return inode;
}

int
inode_get(
	struct mount *mountp,
	ino_t number,
	struct inode **result)
{
	struct inode *inode;
	unsigned index;
	int error;

	error = ENOENT;
	assert(pthread_mutex_lock(&test_inode_lock) == 0);
	for (index = 0; index < TEST_INODE_MAX; index++) {
		inode = test_inodes[index];
		if (inode != NULL && inode->i_mount == mountp &&
		    inode->i_ino == number &&
		    (inode->i_flags & INODE_DEAD) == 0) {
			refcount_get(&inode->i_refs);
			*result = inode;
			error = 0;
			break;
		}
	}
	assert(pthread_mutex_unlock(&test_inode_lock) == 0);
	return error;
}

void
inode_ref(
	struct inode *inode)
{
	if (inode != NULL)
		refcount_get(&inode->i_refs);
}

void
inode_release(
	struct inode *inode)
{
	unsigned index;
	unsigned remaining;
	int destroy;

	if (inode == NULL)
		return;

	remaining = refcount_put_not_last(&inode->i_refs);
	if (remaining != 1 || (inode->i_flags & INODE_DEAD) == 0)
		return;

	destroy = 0;
	assert(pthread_mutex_lock(&test_inode_lock) == 0);
	if (refcount_load(&inode->i_refs) == 1) {
		for (index = 0; index < TEST_INODE_MAX; index++) {
			if (test_inodes[index] == inode) {
				test_inodes[index] = NULL;
				assert(refcount_put(&inode->i_refs));
				destroy = 1;
				break;
			}
		}
	}
	assert(pthread_mutex_unlock(&test_inode_lock) == 0);

	if (destroy)
		test_inode_destroy(inode);
}

int
tty_pty_exists(
	unsigned number)
{
	(void)number;
	return 0;
}

unsigned
tty_pty_snapshot(
	unsigned *indices,
	unsigned capacity)
{
	(void)indices;
	(void)capacity;
	return 0;
}

int
disk_get_info(
	const char *name,
	struct disk_info *result)
{
	(void)name;
	(void)result;
	return ENOENT;
}

int
disk_registry_snapshot(
	struct disk_info *entries,
	unsigned capacity,
	unsigned *count_out)
{
	(void)entries;
	(void)capacity;
	*count_out = 0;
	return 0;
}

int
disk_open_by_dev(
	dev_t device,
	struct disk **result)
{
	(void)device;
	(void)result;
	return ENODEV;
}

void
disk_close(
	struct disk *disk)
{
	(void)disk;
}

int
disk_ioctl(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	(void)disk;
	(void)request;
	(void)argument;
	return ENOTTY;
}

int
disk_read(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	void *data)
{
	(void)disk;
	(void)block;
	(void)count;
	(void)data;
	return EIO;
}

int
disk_write(
	struct disk *disk,
	uint64_t block,
	uint32_t count,
	const void *data)
{
	(void)disk;
	(void)block;
	(void)count;
	(void)data;
	return EIO;
}

int
disk_sync(
	struct disk *disk)
{
	(void)disk;
	return EIO;
}

int
block_identity_get(
	struct disk *disk,
	struct block_identity *identity)
{
	(void)disk;
	(void)identity;
	return ENODEV;
}

int
backing_mutation_begin_disk(
	struct disk *disk,
	uint64_t first,
	uint64_t count,
	const struct backing_claim *claim,
	struct backing_mutation_guard *guard)
{
	(void)disk;
	(void)first;
	(void)count;
	(void)claim;
	memset(guard, 0, sizeof(*guard));
	return 0;
}

void
backing_mutation_end(
	struct backing_mutation_guard *guard)
{
	(void)guard;
}

int
main(
	void)
{
	struct mount mountp;

	memset(&mountp, 0, sizeof(mountp));
	mountp.m_type = &devfs_type;
	assert(mutex_init(&mountp.m_lock, LOCK_RANK_NAMESPACE,
	    "test mount") == 0);
	cdev_reset();
	assert(devfs_type.mount(&mountp) == 0);
	assert(mountp.m_root != NULL);

	test_generation_lifetime(&mountp);
	test_legacy_registration();
	test_registry_capacity_and_reset();
	test_concurrent_lookup_snapshot_reset();
	test_fixed_directory_recreation(&mountp);
	test_mount_failure_cleanup();
	test_inode_purge_mount(&mountp);

	puts("WS006 dynamic cdev/devfs generation lifecycle: PASS");
	return 0;
}

/* Destroys one mock cached inode using the production special-ref contract. */
static void
test_inode_destroy(
	struct inode *inode)
{
	void (*destroy)(void *);
	void *special;

	special = inode->i_special;
	destroy = inode->i_special_destroy;
	inode->i_special = NULL;
	inode->i_special_destroy = NULL;
	if (special != NULL && destroy != NULL)
		destroy(special);
	free(inode);
}

/* Simulates ordinary inode-cache pressure against one cache-only inode. */
static void
test_inode_evict(
	struct mount *mountp,
	ino_t number)
{
	struct inode *inode;
	unsigned index;

	inode = NULL;
	assert(pthread_mutex_lock(&test_inode_lock) == 0);
	for (index = 0; index < TEST_INODE_MAX; index++) {
		if (test_inodes[index] != NULL &&
		    test_inodes[index]->i_mount == mountp &&
		    test_inodes[index]->i_ino == number) {
			inode = test_inodes[index];
			assert(refcount_load(&inode->i_refs) == 1U);
			test_inodes[index] = NULL;
			assert(refcount_put(&inode->i_refs));
			break;
		}
	}
	assert(pthread_mutex_unlock(&test_inode_lock) == 0);
	assert(inode != NULL);
	test_inode_destroy(inode);
}

/* Purges all mock cache refs belonging to one unmounted filesystem. */
static void
test_inode_purge_mount(
	struct mount *mountp)
{
	struct inode *inode;
	unsigned index;

	if (mountp->m_root != NULL) {
		mountp->m_root->i_flags &= ~INODE_ROOT;
		mountp->m_root->i_flags |= INODE_DEAD;
		inode_release(mountp->m_root);
		mountp->m_root = NULL;
	}

	for (;;) {
		inode = NULL;
		assert(pthread_mutex_lock(&test_inode_lock) == 0);
		for (index = 0; index < TEST_INODE_MAX; index++) {
			if (test_inodes[index] != NULL &&
			    test_inodes[index]->i_mount == mountp) {
				inode = test_inodes[index];
				inode_ref(inode);
				inode->i_flags |= INODE_DEAD;
				break;
			}
		}
		assert(pthread_mutex_unlock(&test_inode_lock) == 0);
		if (inode == NULL)
			break;
		inode_release(inode);
	}
}

/* Looks up one component through the production devfs inode operations. */
static int
lookup_name(
	struct inode *directory,
	const char *name,
	struct inode **result)
{
	struct componentname component;

	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = COMPONENT_LAST;
	return directory->i_op->lookup(directory, &component, result);
}

/* Records terminal destruction of one payload generation. */
static void
payload_finalize(
	void *data)
{
	struct test_payload *payload;

	payload = data;
	(void)__atomic_add_fetch(&payload->finalized, 1U, __ATOMIC_RELEASE);
}

/* Returns the immutable payload selected when this file was opened. */
static ssize_t
payload_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct test_payload *payload;

	if (size < sizeof(payload->value))
		return -EINVAL;
	payload = file->f_data;
	memcpy(buffer, &payload->value, sizeof(payload->value));
	return (ssize_t)sizeof(payload->value);
}

/* Reports readiness from the cdev generation, independent of inode caching. */
static int
payload_poll(
	struct file *file,
	short requested,
	short *returned)
{
	(void)file;
	*returned = requested & (POLLIN | POLLRDNORM);
	return 0;
}

/* Verifies namespace removal, old-fd isolation, and generation replacement. */
static void
test_generation_lifetime(
	struct mount *mountp)
{
	struct cdev *new_device;
	struct cdev *old_device;
	struct dirent entry;
	struct file listed_directory;
	struct file new_file;
	struct file old_file;
	struct file stale_directory;
	struct inode *input;
	struct inode *new_inode;
	struct inode *old_inode;
	struct test_payload new_payload;
	struct test_payload old_payload;
	uint64_t old_generation;
	unsigned value;
	short returned;
	int eof;

	memset(&new_payload, 0, sizeof(new_payload));
	memset(&old_payload, 0, sizeof(old_payload));
	new_payload.value = 22;
	old_payload.value = 11;

	assert(cdev_register_managed("event0", 0x30000U, &payload_ops,
	    &old_payload, payload_finalize, &old_device) == 0);
	old_generation = cdev_generation(old_device);
	assert(lookup_name(mountp->m_root, "input", &input) == 0);

	memset(&listed_directory, 0, sizeof(listed_directory));
	listed_directory.f_inode = input;
	assert(input->i_fop->open(&listed_directory) == 0);
	eof = 0;
	assert(input->i_fop->readdir(&listed_directory, &entry, &eof) == 0);
	assert(!eof && !strcmp(entry.d_name, "event0"));
	assert(input->i_fop->close(&listed_directory) == 0);

	memset(&stale_directory, 0, sizeof(stale_directory));
	stale_directory.f_inode = input;
	assert(input->i_fop->open(&stale_directory) == 0);
	assert(lookup_name(input, "event0", &old_inode) == 0);
	assert((old_inode->i_flags & INODE_DEAD) != 0);
	memset(&old_file, 0, sizeof(old_file));
	old_file.f_inode = old_inode;
	old_file.f_flags.value = O_RDONLY;
	assert(cdev_file_ops.open(&old_file) == 0);
	returned = 0;
	assert(cdev_file_ops.poll(&old_file, POLLIN | POLLRDNORM,
	    &returned) == 0);
	assert((returned & (POLLIN | POLLRDNORM)) != 0);
	assert((returned & POLLHUP) == 0);

	assert(cdev_unregister(old_device) == 0);
	assert(cdev_count() == 0);
	assert(lookup_name(input, "event0", &new_inode) == ENOENT);
	eof = 0;
	assert(input->i_fop->readdir(&stale_directory, &entry, &eof) == 0);
	assert(eof);
	assert(input->i_fop->close(&stale_directory) == 0);
	cdev_release(old_device);
	assert(old_payload.finalized == 0);
	returned = 0;
	assert(cdev_file_ops.poll(&old_file, POLLIN | POLLRDNORM,
	    &returned) == 0);
	assert((returned & (POLLIN | POLLRDNORM)) != 0);
	assert((returned & POLLHUP) == 0);

	assert(cdev_register_managed("event0", 0x30000U, &payload_ops,
	    &new_payload, payload_finalize, &new_device) == 0);
	assert(cdev_generation(new_device) != old_generation);
	assert(lookup_name(input, "event0", &new_inode) == 0);
	memset(&new_file, 0, sizeof(new_file));
	new_file.f_inode = new_inode;
	new_file.f_flags.value = O_RDONLY;
	assert(cdev_file_ops.open(&new_file) == 0);

	value = 0;
	assert(cdev_file_ops.read(&old_file, &value, sizeof(value)) ==
	    (ssize_t)sizeof(value));
	assert(value == old_payload.value);
	value = 0;
	assert(cdev_file_ops.read(&new_file, &value, sizeof(value)) ==
	    (ssize_t)sizeof(value));
	assert(value == new_payload.value);

	assert(cdev_file_ops.close(&new_file) == 0);
	inode_release(new_inode);
	assert(cdev_unregister(new_device) == 0);
	cdev_release(new_device);
	assert(new_payload.finalized == 1);

	assert(cdev_file_ops.close(&old_file) == 0);
	inode_release(old_inode);
	assert(old_payload.finalized == 1);
	inode_release(input);
}

/* Verifies legacy publication remains usable through the ref-safe API. */
static void
test_legacy_registration(
	void)
{
	struct test_payload payload;
	struct cdev *device;

	memset(&payload, 0, sizeof(payload));
	payload.value = 44;
	assert(cdev_register("legacy", 0x30000U, &payload_ops, &payload) == 0);
	device = cdev_find_ref("legacy");
	assert(device != NULL);
	assert(device->data == &payload);
	assert(cdev_unregister(device) == 0);
	assert(cdev_find_ref("legacy") == NULL);
	cdev_release(device);
	assert(cdev_count() == 0);
}

/* Verifies bounded publication failure and reset-delayed finalization. */
static void
test_registry_capacity_and_reset(
	void)
{
	struct cdev *devices[CDEV_MAX];
	struct cdev *overflow;
	struct test_payload payloads[CDEV_MAX + 1U];
	char name[32];
	unsigned index;

	memset(devices, 0, sizeof(devices));
	memset(payloads, 0, sizeof(payloads));
	for (index = 0; index < CDEV_MAX; index++) {
		(void)snprintf(name, sizeof(name), "capacity%u", index);
		assert(cdev_register_managed(name, (dev_t)index, &payload_ops,
		    &payloads[index], payload_finalize, &devices[index]) == 0);
	}
	assert(cdev_count() == CDEV_MAX);
	overflow = NULL;
	assert(cdev_register_managed("overflow", 99, &payload_ops,
	    &payloads[CDEV_MAX], payload_finalize, &overflow) == ENOSPC);
	assert(overflow == NULL && payloads[CDEV_MAX].finalized == 0);

	cdev_reset();
	assert(cdev_count() == 0);
	for (index = 0; index < CDEV_MAX; index++) {
		assert(payloads[index].finalized == 0);
		cdev_release(devices[index]);
		assert(payloads[index].finalized == 1);
	}
}

/* Retains one generation through either lookup or snapshot publication. */
static void *
retain_generation_worker(
	void *data)
{
	struct retained_generation *retained;
	unsigned index;

	retained = data;
	if (retained->snapshot_mode) {
		retained->snapshot_count = cdev_snapshot(retained->snapshot,
		    CDEV_MAX);
	} else {
		retained->found = cdev_find_ref(retained->name);
	}
	__atomic_store_n(&retained->ready, 1U, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&retained->release, __ATOMIC_ACQUIRE))
		sched_yield();

	if (retained->snapshot_mode) {
		for (index = 0; index < retained->snapshot_count; index++) {
			if (!cdev_is_published(retained->snapshot[index]))
				retained->observed_unpublished = 1U;
			cdev_release(retained->snapshot[index]);
		}
	} else if (retained->found != NULL) {
		if (!cdev_is_published(retained->found))
			retained->observed_unpublished = 1U;
		cdev_release(retained->found);
	}
	return NULL;
}

/* Verifies reset against concurrent ref-safe lookup and directory snapshot. */
static void
test_concurrent_lookup_snapshot_reset(
	void)
{
	struct retained_generation lookup;
	struct retained_generation snapshot;
	struct test_payload payload;
	struct cdev *device;
	pthread_t lookup_thread;
	pthread_t snapshot_thread;

	memset(&lookup, 0, sizeof(lookup));
	memset(&snapshot, 0, sizeof(snapshot));
	memset(&payload, 0, sizeof(payload));
	payload.value = 33;
	lookup.name = "race";
	snapshot.snapshot_mode = 1U;
	assert(cdev_register_managed("race", 0x30000U, &payload_ops,
	    &payload, payload_finalize, &device) == 0);
	assert(pthread_create(&lookup_thread, NULL, retain_generation_worker,
	    &lookup) == 0);
	assert(pthread_create(&snapshot_thread, NULL, retain_generation_worker,
	    &snapshot) == 0);
	while (!__atomic_load_n(&lookup.ready, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&snapshot.ready, __ATOMIC_ACQUIRE))
		sched_yield();

	assert(lookup.found == device);
	assert(snapshot.snapshot_count == 1U);
	assert(snapshot.snapshot[0] == device);
	cdev_reset();
	assert(cdev_find_ref("race") == NULL);
	assert(cdev_count() == 0);
	cdev_release(device);
	assert(__atomic_load_n(&payload.finalized, __ATOMIC_ACQUIRE) == 0);
	__atomic_store_n(&lookup.release, 1U, __ATOMIC_RELEASE);
	__atomic_store_n(&snapshot.release, 1U, __ATOMIC_RELEASE);
	assert(pthread_join(lookup_thread, NULL) == 0);
	assert(pthread_join(snapshot_thread, NULL) == 0);
	assert(lookup.observed_unpublished != 0);
	assert(snapshot.observed_unpublished != 0);
	assert(__atomic_load_n(&payload.finalized, __ATOMIC_ACQUIRE) == 1U);
}

/* Verifies fixed directories reappear after ordinary cache eviction. */
static void
test_fixed_directory_recreation(
	struct mount *mountp)
{
	struct inode *directory;

	assert(lookup_name(mountp->m_root, "input", &directory) == 0);
	assert(directory->i_ino == 4U);
	inode_release(directory);
	test_inode_evict(mountp, 4U);
	assert(lookup_name(mountp->m_root, "input", &directory) == 0);
	assert(directory->i_ino == 4U);
	inode_release(directory);
}

/* Verifies a partial fixed-directory mount leaves no cache-owned residue. */
static void
test_mount_failure_cleanup(
	void)
{
	struct mount mountp;
	unsigned index;

	memset(&mountp, 0, sizeof(mountp));
	mountp.m_type = &devfs_type;
	assert(mutex_init(&mountp.m_lock, LOCK_RANK_NAMESPACE,
	    "failure mount") == 0);
	test_inode_allocations_before_failure = 2;
	assert(devfs_type.mount(&mountp) == ENOSPC);
	assert(mountp.m_root == NULL);
	test_inode_allocations_before_failure = -1;

	assert(pthread_mutex_lock(&test_inode_lock) == 0);
	for (index = 0; index < TEST_INODE_MAX; index++)
		assert(test_inodes[index] == NULL ||
		    test_inodes[index]->i_mount != &mountp);
	assert(pthread_mutex_unlock(&test_inode_lock) == 0);

	assert(devfs_type.mount(&mountp) == 0);
	assert(mountp.m_root != NULL);
	test_inode_purge_mount(&mountp);
}
