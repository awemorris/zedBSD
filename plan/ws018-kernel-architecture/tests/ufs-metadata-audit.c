/* Regression for shared-dinode-block exclusion and directory mutation bounds.
 * Uses the exact private production routine and only an in-memory block.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stdio.h>
#include <stdlib.h>
#include "mount-thread-host.h"
#if UFS_AUDIT_VERSION == 1
#include "drivers/fs/ufs1/ufs1-vfs.c"
#define AUDIT_STATE struct ufs1_mount_state
#define AUDIT_INODE struct ufs1_inode_info
#define AUDIT_DINODE_SIZE UFS1_DINODE_SIZE
#define AUDIT_SIZE UFS1_DI_SIZE
#define AUDIT_GET64 ufs1_get64
#define AUDIT_PUT32 ufs1_put32
#define AUDIT_PUT16 ufs1_put16
#define AUDIT_TRUNCATE ufs1_truncate
#define AUDIT_GETPTR ufs1_get32
#define AUDIT_PUTPTR ufs1_put32
#define AUDIT_STRIDE 4
#define AUDIT_DB UFS1_DI_DB
#define AUDIT_IB UFS1_DI_IB
#define AUDIT_CG_MAGIC UFS1_CG_MAGIC
#define AUDIT_CG_MAGIC_VALUE UFS1_CG_MAGIC_VALUE
#define AUDIT_CG_CGX UFS1_CG_CGX
#define AUDIT_CG_NDBLK UFS1_CG_NDBLK
#define AUDIT_CG_IUSEDOFF UFS1_CG_IUSEDOFF
#define AUDIT_CG_FREEOFF UFS1_CG_FREEOFF
#define AUDIT_CG_NEXTFREEOFF UFS1_CG_NEXTFREEOFF
#define AUDIT_CG_NBFREE UFS1_CG_NBFREE

#else
#include "drivers/fs/ufs2/ufs2-vfs.c"
#define AUDIT_STATE struct ufs2_mount_state
#define AUDIT_INODE struct ufs2_inode_info
#define AUDIT_DINODE_SIZE UFS2_DINODE_SIZE
#define AUDIT_SIZE UFS2_DI_SIZE
#define AUDIT_GET64 ufs2_get64
#define AUDIT_PUT32 ufs2_put32
#define AUDIT_PUT16 ufs2_put16
#define AUDIT_TRUNCATE ufs2_truncate
#define AUDIT_GETPTR ufs2_get64
#define AUDIT_PUTPTR ufs2_put64
#define AUDIT_STRIDE 8
#define AUDIT_DB UFS2_DI_DB
#define AUDIT_IB UFS2_DI_IB
#define AUDIT_CG_MAGIC UFS2_CG_MAGIC
#define AUDIT_CG_MAGIC_VALUE UFS2_CG_MAGIC_VALUE
#define AUDIT_CG_CGX UFS2_CG_CGX
#define AUDIT_CG_NDBLK UFS2_CG_NDBLK
#define AUDIT_CG_IUSEDOFF UFS2_CG_IUSEDOFF
#define AUDIT_CG_FREEOFF UFS2_CG_FREEOFF
#define AUDIT_CG_NEXTFREEOFF UFS2_CG_NEXTFREEOFF
#define AUDIT_CG_NBFREE UFS2_CG_NBFREE

#endif

static unsigned char medium[4096];
static unsigned char storage[512 * 512], durable[512 * 512];
static int storage_mode, failure_write, failure_write_again, failure_sync, commit_error;
static unsigned storage_writes, storage_syncs, functional_checks;
#define REQUIRE(x) do { __atomic_add_fetch(&functional_checks, 1U, __ATOMIC_RELAXED); if (!(x)) { \
 fprintf(stderr, "UFS%d functional check failed at line %d: %s\n", \
 UFS_AUDIT_VERSION, __LINE__, #x); abort(); } } while (0)

static int references(const unsigned char *bytes, uint64_t fragment)
{
	const unsigned char *raw = bytes + 8 * 512 + 2 * AUDIT_DINODE_SIZE;
#if UFS_AUDIT_VERSION == 2
	for (unsigned n = 0; n < 2; n++)
		if (ufs2_get64(raw, UFS2_DI_EXTB + n * 8, 0) == fragment) return 1;
#endif
	for (unsigned n = 0; n < 12; n++)
		if (AUDIT_GETPTR(raw, AUDIT_DB + n * AUDIT_STRIDE, 0) == fragment) return 1;
	for (unsigned n = 0; n < 3; n++)
		if (AUDIT_GETPTR(raw, AUDIT_IB + n * AUDIT_STRIDE, 0) == fragment) return 1;
	/* Fixed indirect blocks in these fixtures; only inspect reachable ones. */
	for (unsigned n = 168; n <= 184; n += 8) {
		int reachable = 0;
		for (unsigned j = 0; j < 3; j++)
			if (AUDIT_GETPTR(raw, AUDIT_IB + j * AUDIT_STRIDE, 0) == n) reachable = 1;
		for (unsigned parent = n + 8; parent <= 184; parent += 8)
			if (AUDIT_GETPTR(bytes + parent * 512, 0, 0) == n) reachable = 1;
		if (reachable)
			for (unsigned j = 0; j < 4096 / AUDIT_STRIDE; j++)
				if (AUDIT_GETPTR(bytes + n * 512, j * AUDIT_STRIDE, 0) == fragment) return 1;
	}
	return 0;
}

static unsigned pause_read;
static _Thread_local unsigned identity;
static AUDIT_INODE cached_node;
static unsigned allocations;
static int fail_read, fail_write;
static unsigned read_calls, write_calls;

void *kern_malloc(size_t size) { return malloc(size); }
void kern_free(void *pointer) { free(pointer); }
void clock_realtime(time_t *seconds, long *nanoseconds)
{ *seconds = 1; *nanoseconds = 0; }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void mutex_lock(struct mutex *mutex)
{
	while (__atomic_exchange_n(&mutex->locked, 1U, __ATOMIC_ACQUIRE)) {
		host_gate_signal(2);
		host_thread_yield();
	}
	__atomic_store_n(&mutex->owner, (struct thread *)&identity, __ATOMIC_RELAXED);
}
int mutex_owned(struct mutex *mutex)
{ return __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == (struct thread *)&identity; }
void mutex_unlock(struct mutex *mutex)
{ __atomic_store_n(&mutex->owner, NULL, __ATOMIC_RELAXED);
  __atomic_store_n(&mutex->locked, 0U, __ATOMIC_RELEASE); }

int disk_read(struct disk *disk, uint64_t first, uint32_t count, void *buffer)
{
	(void)disk;
	if (storage_mode) {
		REQUIRE(first + count <= 512);
		memcpy(buffer, storage + first * 512, count * 512);
		if (pause_read) { pause_read = 0; host_gate_pause(1); }
		return 0;
	}
	if (first != 8 || count != sizeof(medium) / 512)
		abort();
	read_calls++;
	memcpy(buffer, medium, sizeof(medium));
	if (pause_read) {
		pause_read = 0;
		host_gate_pause(1);
	}
	if (fail_read) { fail_read = 0; return EIO; }
	return 0;
}

int disk_sync(struct disk *disk)
{
	(void)disk;
	if (storage_mode) {
		storage_syncs++;
		if ((int)storage_syncs == failure_sync) return EIO;
		memcpy(durable, storage, sizeof(storage));
	}
	return 0;
}

int disk_write(struct disk *disk, uint64_t first, uint32_t count, const void *buffer)
{
	(void)disk;
	if (storage_mode) {
		REQUIRE(first + count <= 512);
		storage_writes++;
		if (first == 32) {
			const uint8_t *map = (const uint8_t *)buffer + 264;
			const uint8_t *old = storage + 32 * 512 + 264;
			for (unsigned fragment = 160; fragment <= 184; fragment += 8)
				if (bit_test(map, fragment) && !bit_test(old, fragment)) {
					REQUIRE(!references(storage, fragment));
					REQUIRE(!references(durable, fragment));
				}
		}
		if (((int)storage_writes != failure_write && (int)storage_writes != failure_write_again) || (commit_error && (int)storage_writes != failure_write_again))
			memcpy(storage + first * 512, buffer, count * 512);
		return (int)storage_writes == failure_write || (int)storage_writes == failure_write_again ? EIO : 0;
	}
	if (first != 8 || count != sizeof(medium) / 512)
		abort();
	write_calls++;
	memcpy(medium, buffer, sizeof(medium));
	if (fail_write) { fail_write = 0; return EIO; }
	return 0;
}

#if UFS_AUDIT_VERSION == 2
int ufs2_snapshot_preserve(struct ufs2_snapshot *snapshot, uint64_t first, uint32_t count)
{ (void)snapshot; (void)first; (void)count; abort(); }
int ufs2_journal_commit(struct ufs2_journal *journal, uint64_t target,
    const void *payload, uint32_t sectors)
{ (void)journal; (void)target; (void)payload; (void)sectors; abort(); }
#endif

struct inode *inode_alloc(struct mount *mountp)
{
	REQUIRE(allocations++ == 0);
	memset(&cached_node, 0, sizeof(cached_node));
	cached_node.inode.i_mount = mountp;
	return &cached_node.inode;
}
int inode_get(struct mount *mountp, ino_t ino, struct inode **result)
{
	if (allocations && cached_node.inode.i_mount == mountp &&
	    cached_node.inode.i_ino == ino) {
		*result = &cached_node.inode;
		return 0;
	}
	return ENOENT;
}
void inode_release(struct inode *node) { (void)node; }
int inode_sync(struct inode *node) { (void)node; abort(); }
const struct file_ops fifo_file_ops = { 0 };
int inode_creation_prepare(struct inode *parent, struct inode *child,
    const struct inode_creation_request *request)
{ (void)parent; (void)child; (void)request; abort(); }
struct load_request { struct mount *mountp; struct inode *result; };
static void load_worker(void *argument)
{
	struct load_request *request = argument;
	REQUIRE(load_inode(request->mountp, 2, &request->result) == 0);
}
static void update(void *argument)
{ if (persist_inode(argument) != 0) abort(); }

static void storage_fixture(AUDIT_STATE *filesystem, AUDIT_INODE *node,
    struct mount *mountp, struct disk *disk, unsigned depth, int allocation)
{
	memset(storage, 0, sizeof(storage));
	memset(filesystem, 0, sizeof(*filesystem));
	memset(node, 0, sizeof(*node));
	memset(mountp, 0, sizeof(*mountp));
	memset(disk, 0, sizeof(*disk));
	filesystem->super.bsize = 4096;
	filesystem->super.frag = 8;
	filesystem->super.size = filesystem->super.fpg = 512;
	filesystem->super.ncg = 1;
	filesystem->super.ipg = 32;
	filesystem->super.inopb = 4096 / AUDIT_DINODE_SIZE;
	filesystem->super.iblkno = 8;
	filesystem->super.cblkno = 32;
	filesystem->super.dblkno = 160;
	filesystem->super.cgsize = 4096;
	filesystem->super.nindir = 4096 / AUDIT_STRIDE;
	filesystem->super.maxfilesize = (uint64_t)1 << 48;
	filesystem->cg = calloc(1, 4096);
	REQUIRE(filesystem->cg != NULL);
	filesystem->writable = 1;
	mountp->m_data = filesystem;
	mountp->m_disk = disk;
	node->inode.i_mount = mountp;
	node->inode.i_type = INODE_REG;
	node->inode.i_mode = S_IFREG | 0600;
	node->inode.i_ino = 2;
	node->inode.i_linkcount = 1;
	node->inode.i_size = 4096 * 13;
	uint8_t *cg = storage + 32 * 512;
	AUDIT_PUT32(cg, AUDIT_CG_MAGIC, AUDIT_CG_MAGIC_VALUE, 0);
	AUDIT_PUT32(cg, AUDIT_CG_NDBLK, 512, 0);
	AUDIT_PUT32(cg, AUDIT_CG_IUSEDOFF, 256, 0);
	AUDIT_PUT32(cg, AUDIT_CG_FREEOFF, 264, 0);
	AUDIT_PUT32(cg, AUDIT_CG_NEXTFREEOFF, 328, 0);
	if (allocation) {
		for (unsigned n = 160; n < 168; n++) bit_set(cg + 264, n);
		AUDIT_PUT32(cg, AUDIT_CG_NBFREE, 1, 0);
		filesystem->super.cstotal_nbfree = 1;
	} else if (depth == 0) {
		node->direct[0] = 160;
		node->blocks = 8;
	} else {
		node->indirect[depth - 1] = 160 + depth * 8;
		node->blocks = (depth + 1) * 8;
		for (unsigned n = 1; n <= depth; n++)
			AUDIT_PUTPTR(storage + (160 + n * 8) * 512, 0, 160 + (n - 1) * 8, 0);
	}
	storage_mode = 1;
	failure_write = failure_write_again = failure_sync = 0;
	storage_writes = storage_syncs = 0;
	REQUIRE(persist_inode(&node->inode) == 0);
	memcpy(durable, storage, sizeof(storage));
	storage_writes = storage_syncs = 0;
}

static void allocation_and_truncate(void)
{
	AUDIT_STATE filesystem;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	for (unsigned depth = 0; depth <= 3; depth++) {
		for (int kind = 0; kind < 3; kind++) {
			for (int fail = 0; fail <= 18; fail++) {
				storage_fixture(&filesystem, &node, &mountp, &disk, depth, 0);
				failure_write = kind < 2 ? fail : 0;
				failure_sync = kind == 2 ? fail : 0;
				commit_error = kind == 1;
				int error = AUDIT_TRUNCATE(&node.inode, 0);
				REQUIRE(error == 0 || error == EIO);
				if (error == 0) {
					REQUIRE(node.inode.i_size == 0 && node.blocks == 0);
					REQUIRE(!references(storage, 160));
				}
				REQUIRE(!node.inode.i_lock.locked && !filesystem.lock.locked);
				free(filesystem.cg);
			}
		}
	}
	/* Direct and indirect-root publication failures, including writes that
	 * committed before reporting an error; rollback must precede reuse. */
	for (unsigned indirect = 0; indirect < 2; indirect++) {
		for (int committed = 0; committed < 2; committed++) {
			storage_fixture(&filesystem, &node, &mountp, &disk, 0, 1);
			failure_write = 4; /* CG, super, zero block, inode publication. */
			commit_error = committed;
			__typeof__(node.direct[0]) result;
			REQUIRE(bmap_ensure(&node.inode, indirect ? 12 : 0, &result) == EIO);
			REQUIRE(!references(storage, 160));
			REQUIRE(bit_test(storage + 32 * 512 + 264, 160));
			free(filesystem.cg);
		}
	}
#if UFS_AUDIT_VERSION == 2
	for (int fail = 0; fail <= 8; fail++) {
		for (int kind = 0; kind < 3; kind++) {
			storage_fixture(&filesystem, &node, &mountp, &disk, 0, 1);
			commit_error = kind == 1;
			failure_write = kind < 2 ? fail : 0;
			failure_sync = kind == 2 ? fail : 0;
			int error = ufs2_setxattr(&node.inode, "user.x", "v", 1, 0);
			REQUIRE(error == 0 || error == EIO);
			if (error == 0) {
				storage_writes = storage_syncs = 0;
				error = ufs2_removexattr(&node.inode, "user.x");
				REQUIRE(error == 0 || error == EIO);
			}
			free(filesystem.cg);
		}
	}
	storage_fixture(&filesystem, &node, &mountp, &disk, 0, 1);
	failure_write = 5; failure_write_again = 6; commit_error = 1;
	REQUIRE(ufs2_setxattr(&node.inode, "user.x", "v", 1, 0) == EIO);
	REQUIRE(!filesystem.writable);
	REQUIRE(references(storage, 160));
	REQUIRE(!bit_test(storage + 32 * 512 + 264, 160));
	{
		struct componentname name = { .cn_nameptr = "x", .cn_namelen = 1 };
		struct stat status = { .st_mode = 0644 };
		unsigned writes = storage_writes;
		REQUIRE(ufs2_unlink(&node.inode, &name) == EROFS);
		REQUIRE(ufs2_rmdir(&node.inode, &name) == EROFS);
		REQUIRE(ufs2_link(&node.inode, &name, &node.inode) == EROFS);
		REQUIRE(ufs2_setattr(&node.inode, &status, INODE_ATTR_MODE) == EROFS);
		REQUIRE(ufs2_inode_sync(&node.inode) == EROFS);
		REQUIRE(storage_writes == writes);
	}
	free(filesystem.cg);
#endif
	storage_fixture(&filesystem, &node, &mountp, &disk, 0, 0);
	allocations = 0;
	host_gate_reset(1); host_gate_reset(2);
	pause_read = 1;
	struct load_request left = { .mountp = &mountp }, right = { .mountp = &mountp };
	void *first = host_thread_start(load_worker, &left);
	host_gate_wait(1);
	void *second = host_thread_start(load_worker, &right);
	host_gate_wait(2); host_gate_release(1);
	host_thread_join(first); host_thread_join(second);
	REQUIRE(allocations == 1 && left.result == right.result);
	REQUIRE(left.result->i_size == node.inode.i_size);
	free(filesystem.cg);
	storage_mode = 0;
	printf("UFS%d allocation/truncate failure ordering: PASS (%u checks)\n",
	    UFS_AUDIT_VERSION, functional_checks);
}

int main(void)
{
	AUDIT_STATE filesystem;
	AUDIT_INODE left, right;
	struct mount mountp;
	struct disk disk;
	uint64_t left_size, right_size;
	memset(&filesystem, 0, sizeof(filesystem));
	memset(&mountp, 0, sizeof(mountp));
	memset(&disk, 0, sizeof(disk));
	memset(&left, 0, sizeof(left));
	memset(&right, 0, sizeof(right));
	filesystem.super.bsize = sizeof(medium);
	filesystem.super.frag = 8;
	filesystem.super.size = 1024;
	filesystem.super.ipg = 32;
	filesystem.super.inopb = sizeof(medium) / AUDIT_DINODE_SIZE;
	filesystem.super.iblkno = 8;
	mountp.m_data = &filesystem;
	mountp.m_disk = &disk;
	left.inode.i_mount = right.inode.i_mount = &mountp;
	left.inode.i_type = right.inode.i_type = INODE_REG;
	left.inode.i_mode = right.inode.i_mode = S_IFREG | 0600;
	left.inode.i_ino = 2;
	right.inode.i_ino = 3;
	left.inode.i_size = 111;
	right.inode.i_size = 222;
	host_gate_reset(1);
	host_gate_reset(2);
	pause_read = 1;
	void *first = host_thread_start(update, &left.inode);
	host_gate_wait(1);
	void *second = host_thread_start(update, &right.inode);
	/* B has attempted the production critical section while A holds it. */
	host_gate_wait(2);
	host_gate_release(1);
	host_thread_join(first);
	host_thread_join(second);
	left_size = AUDIT_GET64(medium + 2 * AUDIT_DINODE_SIZE, AUDIT_SIZE, 0);
	right_size = AUDIT_GET64(medium + 3 * AUDIT_DINODE_SIZE, AUDIT_SIZE, 0);
	printf("UFS%d shared-block diagnostic: reads=%u writes=%u left=%llu right=%llu expected=111,222\n",
	    UFS_AUDIT_VERSION, read_calls, write_calls,
	    (unsigned long long)left_size, (unsigned long long)right_size);
	if (read_calls != 2 || write_calls != 2)
		return 2;
	if (left_size != 111 || right_size != 222) return 1;
	fail_read = 1;
	if (persist_inode(&left.inode) != EIO || filesystem.lock.locked) return 1;
	fail_write = 1;
	if (persist_inode(&right.inode) != EIO || filesystem.lock.locked) return 1;
	if (persist_inode(&left.inode) != 0) return 1;
	filesystem.writable = 1;
	struct componentname name = { .cn_nameptr = "x", .cn_namelen = 1 };
	left.inode.i_type = INODE_DIR;
	left.direct[0] = 8;
	left.inode.i_size = sizeof(medium) * 2;
	unsigned reads = read_calls, writes = write_calls;
	if (dir_add(&left.inode, &name, 4, 8) != EIO ||
	    read_calls != reads || write_calls != writes) return 1;
	left.inode.i_size = sizeof(medium) - 1;
	if (dir_add(&left.inode, &name, 4, 8) != EIO ||
	    read_calls != reads || write_calls != writes) return 1;
	/* Last record ends four bytes before EOF: reject its incomplete successor
	 * before decoding a header beyond the one-block buffer. */
	left.inode.i_size = sizeof(medium);
	memset(medium, 0, sizeof(medium));
	for (unsigned pos = 0; pos < sizeof(medium); pos += 512) {
		AUDIT_PUT32(medium, pos, 9, 0);
		AUDIT_PUT16(medium, pos + 4, pos == 3584 ? 508 : 512, 0);
		medium[pos + 7] = 255;
	}
	/* A long requested name cannot fit in any record's unused tail. */
	char long_name[255]; memset(long_name, 'q', sizeof(long_name));
	struct componentname large = { .cn_nameptr = long_name, .cn_namelen = 255 };
	if (dir_add(&left.inode, &large, 4, 8) != EIO || write_calls != writes) return 1;
	uint32_t offset, previous, number;
	if (dir_find_record(&left.inode, &name, medium, &offset, &previous, &number) != EIO) return 1;
	puts("PASS: shared-block exclusion, read/write error unlock, directory bounds");
	allocation_and_truncate();
	return 0;
}
