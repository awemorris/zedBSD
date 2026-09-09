/* Production-linked disk/devfs/mount query tests; no real disks.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kern/buf.h>
#include <kern/uaccess.h>
#include "src/kern/disk.c"
#include "src/kern/mount.c"
#include "src/kern/devfs.c"
#include "src/kern/partition.c"
#include <drivers/disklabel.h>

static unsigned checks, reads, writes, flushes, locked;
static uint64_t last_block;
static unsigned char medium[8192];
static int io_error, path_error;
static int claim_error, alloc_error;
static uintptr_t current_owner = 1;
static int superuser = 1;
static unsigned admin_claims;
static const struct backing_claim *last_mutation_owner;
static int invalidation_error;
struct thread *thread_current(void) { return (struct thread *)current_owner; }
#ifdef STORAGE_FOUNDATION_HAL_STUBS
bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
#endif
struct ucred *cred_current_ref(void) { return NULL; }
int cred_is_superuser(const struct ucred *c) { (void)c; return superuser; }
void cred_release(struct ucred *c) { (void)c; }
void *kern_calloc(size_t n, size_t s) { return alloc_error ? NULL : calloc(n, s); }
void kern_free(void *p) { free(p); }
#define CHECK(x) do { __atomic_add_fetch(&checks,1,__ATOMIC_RELAXED); if (!(x)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)

void spin_init(struct spinlock *s, enum lock_rank r, const char *n)
{ memset(s, 0, sizeof(*s)); s->rank = r; s->name = n; }
int mutex_init(struct mutex *m, enum lock_rank r, const char *n)
{ memset(m, 0, sizeof(*m)); spin_init(&m->guard, r, n); return 0; }
/* Single-threaded description serialization; real mount races remain in the
 * claim/concurrency fixtures. Unexpected inode mutation entrypoints abort. */
int mutex_owned(struct mutex *m) { return m->locked && m->owner == thread_current(); }
void mutex_lock(struct mutex *m) { CHECK(!m->locked); m->locked = 1; m->owner = thread_current(); }
void mutex_unlock(struct mutex *m) { CHECK(mutex_owned(m)); m->locked = 0; m->owner = NULL; }
int inode_lookup(struct inode *i, const struct componentname *n, struct inode **r)
{ (void)i; (void)n; (void)r; abort(); }
void inode_dir_changed(struct inode *i) { (void)i; abort(); }
int backing_claim_check_mount(struct disk *d, unsigned f)
{ (void)d; (void)f; return 0; }

#ifndef STORAGE_FOUNDATION_CUSTOM_LOCK
unsigned long spin_lock_irqsave(struct spinlock *s)
{ CHECK(!s->held.value); s->held.value = 1; locked++; return 0; }
void spin_unlock_irqrestore(struct spinlock *s, unsigned long irq)
{ (void)irq; CHECK(s->held.value); s->held.value = 0; locked--; }
#endif
void waitq_init(struct wait_queue *q, const char *n)
{ memset(q, 0, sizeof(*q)); q->name = n; }
uint64_t waitq_sequence(const struct wait_queue *q) { return __atomic_load_n(&q->sequence,__ATOMIC_ACQUIRE); }
#ifndef STORAGE_FOUNDATION_CUSTOM_WAIT
void waitq_wake_all(struct wait_queue *q) { q->sequence++; }
int waitq_sleep(struct wait_queue *q, struct spinlock *s, uint64_t o,
	uint64_t d, unsigned f)
{ (void)q; (void)s; (void)o; (void)d; (void)f; abort(); }
#endif
void inode_ref(struct inode *n) { refcount_get(&n->i_refs); }
void inode_release(struct inode *n) { (void)refcount_put_not_last(&n->i_refs); }
int fs_getcwd(const struct cwdinfo *c, char *b, size_t n)
{
	const char *p;
	CHECK(locked == 0);
	CHECK(c->root.p_inode != NULL && c->cwd.p_inode != NULL);
	if (path_error) return path_error;
	p = c->cwd.p_inode->i_data;
	CHECK(strlen(p) < n); strcpy(b, p); return 0;
}
int backing_mutation_begin_disk(struct disk *d, uint64_t b, uint64_t n,
	const struct backing_claim *c, struct backing_mutation_guard *g)
{ (void)d; (void)b; (void)n; last_mutation_owner = c; memset(g, 0, sizeof(*g)); return claim_error; }
int backing_claim_prepare_disk(struct disk *d, uint64_t b, uint64_t n,
	enum backing_claim_owner o, struct backing_claim **result)
{
	CHECK(d && b == 0 && n == d->d_block_count && o == BACKING_CLAIM_ADMIN);
	*result = NULL;
	if (claim_error) return claim_error;
	*result = kern_calloc(1, 1);
	if (!*result) return ENOMEM;
	admin_claims++;
	return 0;
}
#ifndef STORAGE_FOUNDATION_CUSTOM_CLAIM_RELEASE
void backing_claim_release(struct backing_claim *claim)
{ CHECK(claim && admin_claims); admin_claims--; free(claim); }
#endif
void backing_mutation_end(struct backing_mutation_guard *g) { (void)g; }
void buf_reset(void) {}
int buf_sync(struct disk *d) { (void)d; return io_error; }
int buf_read(struct disk *d, uint64_t b, uint32_t n, void *p)
{ return disk_read_direct(d, b, n, p); }
int buf_read_view(struct disk *d, uint64_t b, uint32_t n, void *p, struct buf_view *v)
{ (void)v; return buf_read(d, b, n, p); }
int buf_view_matches(const struct buf_view *v) { return v->valid; }
int buf_write(struct disk *d, uint64_t b, uint32_t n, const void *p)
{ return disk_write_direct(d, b, n, p); }
int buf_write_context(struct disk *d, uint64_t b, uint32_t n, const void *p,
	const struct io_context *context)
{ return disk_write_direct_context(d, b, n, p, context); }
int buf_invalidate_disk(struct disk *d, unsigned f) { (void)d; (void)f; return invalidation_error; }
int buf_invalidate(struct disk *d, uint64_t b, uint64_t n, unsigned f)
{ (void)d; (void)b; (void)n; (void)f; return invalidation_error; }
int copyin(uintptr_t u, void *k, size_t n)
{ if (!u) return EFAULT; memcpy(k, (void *)u, n); return 0; }
int copyout(const void *k, uintptr_t u, size_t n)
{ if (!u) return EFAULT; memcpy((void *)u, k, n); return 0; }
int block_identity_get(struct disk *d, struct block_identity *i)
{ (void)d; (void)i; return EOPNOTSUPP; }

static int submit(struct disk *d, struct bio *b)
{
	CHECK(!locked);
	if (io_error) return io_error;
	last_block = b->b_mapped_block;
	if (b->b_op == BIO_FLUSH) flushes++;
	else {
		unsigned offset = (unsigned)(last_block % 2) * d->d_block_size;
		CHECK(b->b_block_count == 1);
		if (b->b_op == BIO_READ) {
			reads++; memcpy(b->b_data, medium + offset, d->d_block_size);
		} else {
			writes++; memcpy(medium + offset, b->b_data, d->d_block_size);
		}
	}
	bio_complete(b, 0, b->b_block_count * d->d_block_size);
	return 0;
}
static const struct disk_ops ops = { .submit = submit };

static void test_block(unsigned sector)
{
	struct disk *d;
	struct inode inode;
	struct file f;
	struct zedbsd_block_info info;
	unsigned char data[8192];
	uint64_t offset = 0x100000000ULL;
	disk_registry_reset(); d = disk_alloc(); CHECK(d != NULL);
	strcpy(d->d_name, "nvme0n1"); d->d_block_size = sector;
	d->d_block_count = offset / sector + 100; d->d_ops = &ops;
	CHECK(disk_create(d) == 0);
	memset(&inode, 0, sizeof(inode)); inode.i_rdev = d->d_dev;
	memset(&f, 0, sizeof(f)); f.f_inode = &inode;
	CHECK(block_open(&f) == 0);
	memset(&info, 0, sizeof(info));
	CHECK(block_ioctl(&f, BLKGETINFO, 0) == EFAULT);
	CHECK(block_ioctl(&f, BLKGETINFO, (uintptr_t)&info) == EINVAL);
	info.version = 1; info.struct_size = sizeof(info); info.reserved[3] = 1;
	CHECK(block_ioctl(&f, BLKGETINFO, (uintptr_t)&info) == EINVAL);
	info.reserved[3] = 0;
	CHECK(block_ioctl(&f, BLKGETINFO, (uintptr_t)&info) == 0);
	CHECK(info.device == d->d_dev && info.sector_size == sector);
	CHECK(info.sector_count == d->d_block_count && info.parent_device == 0);
	CHECK(!strcmp(info.name, d->d_name));
	memset(data, 0x71, sizeof(data)); memset(medium, 0x33, sizeof(medium));
	reads = writes = flushes = 0;
	CHECK(block_pwrite(&f, data, sector, (off_t)offset) == sector);
	CHECK(writes == 1 && reads == 0 && last_block == offset / sector);
	CHECK(block_pwrite(&f, data, sector, (off_t)offset + 1) == sector);
	CHECK(writes == 3 && reads == 2);
	CHECK(medium[0] == 0x71 && medium[sector] == 0x71);
	CHECK(medium[sector + 1] == 0x33);
	CHECK(block_pread(&f, data, sector, (off_t)offset) == sector);
	CHECK(block_pread(&f, data, 1, -1) == -EINVAL);
	CHECK(block_pread(&f, data, 1, (off_t)(d->d_block_count * sector)) == 0);
	CHECK(block_fsync(&f) == 0 && flushes == 1);
	io_error = EIO;
	CHECK(block_pread(&f, data, 1, 0) == -EIO);
	CHECK(block_pwrite(&f, data, sector, 0) == -EIO);
	CHECK(block_fsync(&f) == EIO); io_error = 0;
	d->d_flags |= DISK_READ_ONLY;
	CHECK(block_pwrite(&f, data, 1, 0) == -EROFS);
	CHECK(block_close(&f) == 0);
	d->d_block_size = 1024;
	CHECK(block_open(&f) == EOPNOTSUPP && d->d_open_count == 0);
}

static void test_mounts(void)
{
	struct zedbsd_mount_info info[3];
	struct mount a, b, c;
	struct inode ia, ib;
	struct disk d;
	const struct filesystem_type type = { .fs_name = "tmpfs" };
	unsigned count;
	memset(&a, 0, sizeof(a)); memset(&b, 0, sizeof(b));
	memset(&c, 0, sizeof(c)); memset(&ia, 0, sizeof(ia));
	memset(&ib, 0, sizeof(ib)); memset(&d, 0, sizeof(d));
	refcount_init(&a.m_refs, 1); refcount_init(&b.m_refs, 1);
	refcount_init(&c.m_refs, 1);
	refcount_init(&ia.i_refs, 1); refcount_init(&ib.i_refs, 1);
	ia.i_data = "/"; ib.i_data = "/nested/actual";
	a.m_root = &ia; b.m_root = &ib;
	a.m_type = b.m_type = &type;
	a.m_state = b.m_state = MOUNT_STATE_LIVE;
	c.m_state = MOUNT_STATE_PREPARING;
	strcpy(b.m_path, "/wrong-saved-path");
	strcpy(d.d_name, "nvme0n1p1"); d.d_dev = 42;
	b.m_disk = &d; b.m_flags = MOUNT_READ_ONLY;
	mount_head = root_mount = NULL;
	CHECK(mount_info_snapshot(info, 3, &count) == 0 && count == 0);
	mount_head = root_mount = &a; a.m_next = &b; b.m_next = &c;
	memset(info, 0x55, sizeof(info));
	CHECK(mount_info_snapshot(info, 1, &count) == ENOSPC && count == 2);
	CHECK(info[0].reserved == 0x55555555U);
	CHECK(mount_info_snapshot(info, 3, &count) == 0 && count == 2);
	CHECK(!strcmp(info[0].target, "/") && info[0].device == 0);
	CHECK(!strcmp(info[1].target, "/nested/actual"));
	CHECK(info[1].device == 42 && info[1].flags == MOUNT_READ_ONLY);
	CHECK(!strcmp(info[1].source, "nvme0n1p1"));
	CHECK(info[1].reserved == 0);
	CHECK(refcount_load(&a.m_refs) == 1 && refcount_load(&ia.i_refs) == 1);
	path_error = EAGAIN;
	CHECK(mount_info_snapshot(info, 3, &count) == EAGAIN);
	CHECK(refcount_load(&b.m_refs) == 1 && refcount_load(&ib.i_refs) == 1);
	path_error = 0;
	mount_head = root_mount = NULL;
}
static void put32(unsigned char *p, uint32_t v)
{ p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static void mbr_entry(unsigned slot, unsigned start, unsigned length)
{
	unsigned char *p = medium + 446 + slot * 16;
	memset(p, 0, 16); p[4] = length ? 0x83 : 0;
	put32(p + 8, start); put32(p + 12, length);
}

static void test_reload(void)
{
	struct disk *d, *child, *found;
	struct inode node;
	struct file f;
	struct disk *held[DISK_MAX];
	unsigned held_count, iteration;
	dev_t old_dev;
	unsigned char byte[512];
	struct buf_view view = {0};
	disk_registry_reset(); partition_reset();
	partition_set_scheme(&drv_partition_scheme_mbr);
	d = disk_alloc(); CHECK(d != NULL);
	strcpy(d->d_name, "nvme0n1"); d->d_block_size = 512;
	d->d_block_count = 4096; d->d_ops = &ops;
	CHECK(disk_create(d) == 0);
	memset(&node, 0, sizeof(node)); node.i_rdev = d->d_dev;
	memset(&f, 0, sizeof(f)); f.f_inode = &node;
	CHECK(block_open(&f) == 0);
	memset(medium, 0, sizeof(medium)); medium[510] = 0x55; medium[511] = 0xaa;
	mbr_entry(0, 100, 100);
	superuser = 0;
	CHECK(block_ioctl(&f, BLKREREADPART, 0) == EPERM);
	superuser = 1;
	CHECK(block_ioctl(&f, BLKREREADPART, 1) == EINVAL);
	CHECK(block_ioctl(&f, BLKREREADPART, 0) == 0);
	CHECK(partition_count() == 1 && disk_count() == 2);
	child = partition_at(0)->p_disk; old_dev = child->d_dev;
	CHECK(partition_reload(child) == EINVAL);
	CHECK(disk_open(child) == 0); /* Same admission used by ro/rw mounts. */
	CHECK(partition_reload(d) == EBUSY); /* No-op is busy, too. */
	mbr_entry(1, 300, 100); mbr_entry(2, 500, 100);
	CHECK(partition_reload(d) == EBUSY);
	CHECK(partition_count() == 1 && child->d_dev == old_dev);
	disk_close(child);
	claim_error = EBUSY; /* Swap/loop claim admission. */
	CHECK(partition_reload(d) == EBUSY); claim_error = 0;
	d->d_opening = 1; CHECK(partition_reload(d) == EBUSY); d->d_opening = 0;
	child->d_closing = 1; CHECK(partition_reload(d) == EBUSY); child->d_closing = 0;
	d->d_cache_users = 1; CHECK(partition_reload(d) == EBUSY); d->d_cache_users = 0;
	CHECK(disk_open(d) == 0); CHECK(partition_reload(d) == EBUSY); disk_close(d);
	disk_ref(child); CHECK(partition_reload(d) == EBUSY); disk_release(child);
	alloc_error = 1; CHECK(partition_reload(d) == ENOMEM); alloc_error = 0;
	io_error = EIO; CHECK(partition_reload(d) == EIO); io_error = 0;
	CHECK(child->d_dev == old_dev && partition_count() == 1);
	medium[510] = 0; CHECK(partition_reload(d) == EINVAL); medium[510] = 0x55;
	mbr_entry(1, 150, 100); CHECK(partition_reload(d) == EINVAL);
	mbr_entry(1, 300, 100);
	medium[446 + 16 + 4] = 5; CHECK(partition_reload(d) == EOPNOTSUPP);
	medium[446 + 16 + 4] = 0x83;
	for (held_count = 0; held_count < DISK_MAX; held_count++) {
		held[held_count] = disk_alloc();
		if (!held[held_count]) break;
	}
	CHECK(partition_reload(d) == ENOSPC);
	CHECK(partition_count() == 1 && child->d_dev == old_dev);
	while (held_count) CHECK(disk_destroy(held[--held_count]) == 0);
	view.disk=d;view.valid=1;
	CHECK(disk_view_matches(d,&view));
	CHECK(!disk_view_matches(child,&view));
	CHECK(disk_reload_begin(d) == 0);
	CHECK(disk_open(d) == EBUSY && disk_open(child) == EBUSY);
	CHECK(disk_reload_begin(d) == EBUSY);
	current_owner = 2;
	CHECK(!disk_view_matches(d,&view));
	CHECK(disk_read_view(d,0,1,byte,&view)==EBUSY);
	CHECK(disk_read(d, 0, 1, byte) == EBUSY);
	CHECK(disk_write(d, 0, 1, byte) == EBUSY);
	CHECK(disk_read_direct(d, 0, 1, byte) == EBUSY);
	disk_reload_end(d); CHECK(d->d_reload_owner != NULL);
	current_owner = 1;
	CHECK(disk_read(d, 0, 1, byte) == 0);
	CHECK(disk_read(child, 0, 1, byte) == EBUSY);
	disk_gone(child); CHECK(child->d_state == DISK_LIVE);
	disk_reload_end(d);
	CHECK(disk_view_matches(d,&view));
	d->d_state=DISK_GONE;
	CHECK(!disk_view_matches(d,&view));
	CHECK(disk_read_view(child,0,1,byte,&view)==ENXIO);
	d->d_state=DISK_LIVE;
	CHECK(partition_reload(d) == 0 && partition_count() == 3);
	CHECK(disk_open_by_dev(old_dev, &found) == ENXIO);
	found = disk_find("nvme0n1p1"); CHECK(found && found->d_dev != old_dev);
	disk_release(found);
	for (iteration = 0; iteration < 1000; iteration++) {
		mbr_entry(2, iteration & 1 ? 0 : 500, iteration & 1 ? 0 : 100);
		CHECK(partition_reload(d) == 0);
		CHECK(disk_count() == partition_count() + 1);
		unsigned allocated = 0;
		for (unsigned i = 0; i < DISK_MAX; i++) allocated += disk_used[i] != 0;
		CHECK(allocated == disk_count());
	}
	CHECK(block_close(&f) == 0);
	partition_reset(); disk_registry_reset();
}

static void test_admin(void)
{
	struct disk *disk, *child;
	struct inode node = {0};
	struct file file = {0};
	struct zedbsd_block_info info, changed;
	unsigned char byte = 0x37;

	disk_registry_reset(); partition_reset();
	partition_set_scheme(&drv_partition_scheme_mbr);
	disk = disk_alloc(); CHECK(disk != NULL);
	strcpy(disk->d_name, "nvme0n1"); disk->d_block_size = 512;
	disk->d_block_count = 4096; disk->d_ops = &ops;
	CHECK(disk_create(disk) == 0);
	node.i_rdev = disk->d_dev; file.f_inode = &node;
	CHECK(block_open(&file) == 0);
	memset(medium, 0, sizeof(medium)); medium[510] = 0x55; medium[511] = 0xaa;
	mbr_entry(0, 100, 100);
	CHECK(block_ioctl(&file, BLKREREADPART, 0) == 0);
	child = partition_at(0)->p_disk;
	memset(&info, 0, sizeof(info)); info.version = ZEDBSD_BLOCK_VERSION; info.struct_size = sizeof(info);
	CHECK(block_ioctl(&file, BLKGETINFO, (uintptr_t)&info) == 0);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBADF);
	atomic_store_release(&file.f_flags, O_RDWR);
	superuser = 0; CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EPERM); superuser = 1;
	CHECK(block_ioctl(&file, BLKRESERVE, 0) == EFAULT);
	changed = info; changed.device++;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&changed) == ESTALE);
	changed = info; changed.sector_count++;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&changed) == ESTALE);
	CHECK(!admin_claims && disk->d_admin_owner == NULL);
	CHECK(disk_open(child) == 0);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY);
	CHECK(!admin_claims && disk->d_admin_owner == NULL); disk_close(child);
	CHECK(disk_open(disk) == 0);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk_close(disk);
	disk->d_inflight = 1;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk->d_inflight = 0;
	disk->d_opening = 1;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk->d_opening = 0;
	disk->d_cache_users = 1;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk->d_cache_users = 0;
	disk_ref(child);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk_release(child);
	claim_error = EBUSY;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY); claim_error = 0;
	alloc_error = 1;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == ENOMEM); alloc_error = 0;
	CHECK(!admin_claims && disk->d_admin_owner == NULL);
	disk->d_flags |= DISK_FILE_BACKED;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EOPNOTSUPP);
	disk->d_flags &= ~DISK_FILE_BACKED;
	disk->d_flags |= DISK_READ_ONLY;
	changed = info; CHECK(block_ioctl(&file, BLKGETINFO, (uintptr_t)&changed) == 0);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&changed) == EROFS);
	disk->d_flags &= ~DISK_READ_ONLY;
	invalidation_error = EBUSY;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY);
	CHECK(!admin_claims && !disk->d_admin_owner && !disk->d_admin_thread);
	invalidation_error = 0;
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == 0);
	CHECK(admin_claims == 1 && disk->d_admin_owner == file.f_block_claim);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == EBUSY);
	CHECK(disk_open(disk) == EBUSY && disk_open(child) == EBUSY);
	CHECK(disk_read(disk, 0, 1, medium) == EBUSY);
	CHECK(disk_read_direct(disk, 0, 1, medium) == EBUSY);
	CHECK(disk_admin_io_begin(disk, file.f_block_claim) == 0);
	CHECK(disk_admin_io_begin(disk, file.f_block_claim) == EBUSY);
	current_owner = 2;
	CHECK(disk_read(disk, 0, 1, medium) == EBUSY);
	disk_admin_io_end(disk, file.f_block_claim);
	CHECK(disk->d_admin_thread == (struct thread *)1);
	current_owner = 1;
	disk_admin_io_end(disk, file.f_block_claim);
	CHECK(!disk->d_admin_thread);
	CHECK(partition_reload(disk) == EBUSY);
	invalidation_error = EBUSY;
	CHECK(block_pwrite(&file, &byte, 1, 4) == -EBUSY && !disk->d_admin_thread);
	invalidation_error = 0;
	CHECK(block_pwrite(&file, &byte, 1, 4) == 1);
	CHECK(last_mutation_owner == file.f_block_claim);
	io_error = EIO;
	CHECK(block_fsync(&file) == EIO && !disk->d_admin_thread);
	io_error = 0;
	CHECK(block_fsync(&file) == 0);
	CHECK(!disk->d_admin_thread);
	CHECK(block_ioctl(&file, BLKREREADPART, 0) == 0);
	CHECK(disk->d_admin_owner == file.f_block_claim && disk_open(disk) == EBUSY);
	CHECK(block_close(&file) == 0);
	CHECK(!admin_claims && !disk->d_admin_owner && !file.f_block_claim);
	CHECK(block_open(&file) == 0);
	CHECK(block_ioctl(&file, BLKRESERVE, (uintptr_t)&info) == 0);
	disk_media_revoke(disk);
	CHECK(block_pwrite(&file, &byte, 1, 4) == -ENXIO);
	CHECK(block_ioctl(&file, BLKGETINFO, (uintptr_t)&info) == ENXIO);
	CHECK(block_close(&file) == 0 && !admin_claims && !disk->d_admin_owner);
	partition_reset(); disk_registry_reset();
}

static void test_partition_admin(void)
{
	struct disk *parent, *part, *sibling, *alias;
	struct inode parent_node = {0}, part_node = {0}, sibling_node = {0};
	struct file parent_file = {0}, part_file = {0}, sibling_file = {0};
	struct zedbsd_block_info info = {0}, sibling_info = {0};
	unsigned char data[512] = {0x71};
	unsigned before;

	disk_registry_reset(); partition_reset();
	partition_set_scheme(&drv_partition_scheme_mbr);
	parent = disk_alloc(); CHECK(parent != NULL);
	strcpy(parent->d_name, "nvme0n1"); parent->d_block_size = 512;
	parent->d_block_count = 8192; parent->d_ops = &ops;
	CHECK(disk_create(parent) == 0);
	parent_node.i_rdev = parent->d_dev; parent_file.f_inode = &parent_node;
	atomic_store_release(&parent_file.f_flags, O_RDWR);
	CHECK(block_open(&parent_file) == 0);
	memset(medium, 0, sizeof(medium)); medium[510] = 0x55; medium[511] = 0xaa;
	mbr_entry(0, 128, 128); mbr_entry(1, 512, 128);
	CHECK(block_ioctl(&parent_file, BLKREREADPART, 0) == 0);
	part = partition_at(0)->p_disk; sibling = partition_at(1)->p_disk;
	alias = disk_alloc(); CHECK(alias != NULL);
	strcpy(alias->d_name, "overlap"); alias->d_parent = parent;
	alias->d_parent_offset = 192; alias->d_block_count = 64;
	alias->d_block_size = 512; alias->d_flags = DISK_PARTITION;
	CHECK(disk_create(alias) == 0);
	part_node.i_rdev = part->d_dev; part_file.f_inode = &part_node;
	atomic_store_release(&part_file.f_flags, O_RDWR);
	CHECK(block_open(&part_file) == 0);
	info.version = 1; info.struct_size = sizeof(info);
	CHECK(block_ioctl(&part_file, BLKGETINFO, (uintptr_t)&info) == 0);
	CHECK(block_ioctl(&part_file, BLKRESERVE, (uintptr_t)&info) == EBUSY); /* parent FD */
	CHECK(block_close(&parent_file) == 0);
	CHECK(disk_open(alias) == 0);
	CHECK(block_ioctl(&part_file, BLKRESERVE, (uintptr_t)&info) == EBUSY); disk_close(alias);
	CHECK(disk_open(sibling) == 0); /* An idle disjoint mounted/open volume is allowed. */
	CHECK(block_ioctl(&part_file, BLKRESERVE, (uintptr_t)&info) == 0);
	CHECK(admin_count == 1 && part->d_admin_owner == part_file.f_block_claim);
	CHECK(disk_open(parent) == EBUSY && disk_open(alias) == EBUSY && disk_open(part) == EBUSY);
	CHECK(disk_open(sibling) == 0); disk_close(sibling);
	CHECK(disk_read(sibling, 0, 1, data) == 0 && last_block == 512);
	CHECK(disk_write(sibling, 0, 1, data) == 0 && last_block == 512);
	CHECK(disk_read_direct(parent, 128, 1, data) == EBUSY);
	CHECK(disk_read_direct(parent, 0, 1, data) == 0); /* a nonoverlapping physical probe */
	CHECK(disk_read(parent, 0, 1, data) == EBUSY); /* whole-volume cache admission */
	CHECK(block_pwrite(&part_file, data, 1, 65535) == 1 && last_block == 255);
	before = writes;
	CHECK(block_pwrite(&part_file, data, 1, 65536) < 0 && writes == before);
	CHECK(block_ioctl(&part_file, BLKREREADPART, 0) == EINVAL);
	CHECK(!part->d_admin_thread && block_fsync(&part_file) == 0);
	disk_close(sibling);
	sibling_node.i_rdev = sibling->d_dev; sibling_file.f_inode = &sibling_node;
	atomic_store_release(&sibling_file.f_flags, O_RDWR);
	CHECK(block_open(&sibling_file) == 0);
	sibling_info.version = 1; sibling_info.struct_size = sizeof(sibling_info);
	CHECK(block_ioctl(&sibling_file, BLKGETINFO, (uintptr_t)&sibling_info) == 0);
	CHECK(block_ioctl(&sibling_file, BLKRESERVE, (uintptr_t)&sibling_info) == 0);
	CHECK(admin_count == 2);
	CHECK(block_pwrite(&sibling_file, data, 1, 0) == 1 && last_block == 512);
	CHECK(block_pread(&part_file, data, 1, 0) == 1 && last_block == 128);
	CHECK(block_close(&part_file) == 0 && admin_count == 1);
	CHECK(disk_open(parent) == EBUSY && disk_open(part) == 0); disk_close(part);
	CHECK(block_close(&sibling_file) == 0 && admin_count == 0 && !admin_claims);
	CHECK(disk_open(parent) == 0); disk_close(parent);
	partition_reset(); disk_registry_reset();
}

int main(void)
{
	test_block(512); test_block(4096); test_mounts(); test_reload();
	test_admin();
	test_partition_admin();
	printf("storage foundations: %u checks PASS\n", checks);
	return 0;
}
