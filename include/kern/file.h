/*
 * File operation
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_FILE_H
#define ZEDBSD_KERN_FILE_H

#include "kern/inode.h"
#include "kern/mount.h"
#include "kern/atomic.h"
#include "kern/lock.h"
#include "kern/vm-object.h"
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#ifndef NAME_MAX
#define NAME_MAX 255U
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x0100
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x40000
#endif

struct cwdinfo;
struct ucred;
struct file;

enum file_io_kind {
	FILE_IO_READ,
	FILE_IO_WRITE,
	FILE_IO_PREAD,
	FILE_IO_PWRITE,
};

struct file_io {
	struct file *file;
	/* The visible inode owns syscall-level ordering; this is the final inode
	 * whose pages and EOF form the shared VM content domain. */
	struct inode *content_inode;
	/* Borrowed for the lifetime of this operation.  Syscall dispatch keeps
	 * process credentials alive until file_io_end(). */
	const struct ucred *credential;
	enum file_io_kind kind;
	off_t offset;
	unsigned internal_flags;
	unsigned held_position;
	unsigned held_inode_io;
	/* Stacked writes retain the authoritative final inode mutex for the whole
	 * file_io.  This makes a multi-chunk write/O_APPEND one operation relative
	 * to direct aliases of the lower inode. */
	unsigned require_content_inode_io;
	unsigned held_content_inode_io;
	/* A stacked operation temporarily drops the visible mutex while the final
	 * content domain performs I/O.  This gate prevents outer setattr/I/O from
	 * taking that mutex and then waiting back on the final domain. */
	unsigned held_visible_gate;
	/* A coherent shared-cache read lease spans every transfer belonging to
	 * this file_io, not merely one syscall copy chunk. */
	unsigned held_content_read;
	unsigned coherent_read;
	/* O_APPEND selects the final content inode's EOF once, at the first
	 * transfer, while its VM content transaction excludes competing writers. */
	unsigned append_requested;
	unsigned append_positioned;
	uint64_t growth_limit;
	unsigned growth_limit_enabled;
	unsigned growth_limit_hit;
	/* Set after the first regular-file backend write has had its set-id
	 * metadata transition prepared.  One file_io can span many syscall
	 * chunks and iovecs, but the transition is performed exactly once. */
	unsigned setid_prepared;
	unsigned transferred;
};

/* An exclusive, immutable view of one regular file.  Begin revokes writable
 * MAP_SHARED translations, writes their dirty cache image to the backend and
 * then keeps the inode content transaction plus i_io lock until end.  This is
 * intended for multi-read consumers such as exec image loading, not ordinary
 * read(2), whose shared lease is carried by struct file_io. */
struct file_content_lease {
	struct file *file;
	struct inode *io_inode;
	struct inode *content_inode;
	struct vm_object_content content;
	off_t size;
	unsigned held_content_inode_io;
	unsigned held_visible_gate;
	unsigned active;
	unsigned transferred;
};

struct dirent {
	ino_t d_ino;
	enum inode_type d_type;
	char d_name[NAME_MAX + 1U];
};

struct file_ops {
	int (*open)(struct file *);
	ssize_t (*read)(struct file *, void *, size_t);
	ssize_t (*write)(struct file *, const void *, size_t);
	ssize_t (*pread)(struct file *, void *, size_t, off_t);
	ssize_t (*pwrite)(struct file *, const void *, size_t, off_t);
	/* Stacking filesystems forward VM/content-owner I/O without re-entering
	 * the lower inode's generic coherence transaction. */
	ssize_t (*pread_internal)(struct file *, void *, size_t, off_t, unsigned);
	ssize_t (*pwrite_internal)(struct file *, const void *, size_t, off_t,
		unsigned, const struct ucred *);
	int (*readdir)(struct file *, struct dirent *, int *);
	off_t (*seek)(struct file *, off_t, int);
	int (*ioctl)(struct file *, unsigned long, uintptr_t);
	int (*poll)(struct file *, short, short *);
	int (*fsync)(struct file *);
	int (*close)(struct file *);
};

struct file {
	struct path f_path;
	struct inode *f_inode;
	struct inode *f_vm_inode;
	const struct file_ops *f_ops;
	off_t f_offset;
	atomic_uint_t f_flags;
	volatile int f_signal_owner;
	refcount_t f_refs;
	struct mutex f_lock;
	unsigned f_mount_cursor;
	void *f_data;
};

int file_openat(struct cwdinfo *, const char *, int, mode_t,
		struct file **);
int file_openat_cred(struct cwdinfo *, const struct ucred *, const char *,
		     int, mode_t, struct file **);
int file_open_resolved(const struct path *, int, struct file **);
int file_create_pseudo(const struct file_ops *, int, void *, struct file **);
int file_io_begin_cred(struct file *, enum file_io_kind, off_t, unsigned,
		       const struct ucred *, struct file_io *);
int file_io_begin(struct file *, enum file_io_kind, off_t, unsigned,
		  struct file_io *);
void file_io_set_growth_limit(struct file_io *, uint64_t);
int file_io_take_growth_limit_hit(struct file_io *);
ssize_t file_io_transfer(struct file_io *, void *, size_t);
void file_io_end(struct file_io *);
int file_content_lease_begin(struct file *, struct file_content_lease *);
ssize_t file_content_lease_pread(struct file_content_lease *, void *, size_t,
	off_t);
void file_content_lease_end(struct file_content_lease *);
ssize_t file_read(struct file *, void *, size_t);
ssize_t file_pread(struct file *, void *, size_t, off_t);
ssize_t file_pread_internal(struct file *, void *, size_t, off_t, unsigned);
ssize_t file_pwrite(struct file *, const void *, size_t, off_t);
#define FILE_IO_LOOP_BACKING 0x00000001U
#define FILE_IO_VM_OBJECT    0x00000002U
#define FILE_IO_INODE_IO_OWNED 0x00000004U
#define FILE_IO_CONTENT_CHANGE 0x00000008U
ssize_t file_pwrite_internal(struct file *, const void *, size_t, off_t,
			     unsigned);
ssize_t file_pwrite_internal_cred(struct file *, const void *, size_t, off_t,
				  unsigned, const struct ucred *);
ssize_t file_write(struct file *, const void *, size_t);
int file_readdir(struct file *, struct dirent *, int *);
off_t file_seek(struct file *, off_t, int);
int file_ioctl(struct file *, unsigned long, uintptr_t);
/* Flush this open file/backend; VM pages are synchronized by vm-object. */
int file_fsync(struct file *);
int file_close(struct file *);
void file_ref(struct file *);
static inline int
file_status_flags_get(const struct file *file)
{
	return file != NULL ? (int)atomic_load_acquire(&file->f_flags) : 0;
}
void file_status_flags_update(struct file *, int, int);
struct inode *file_vm_inode(struct file *);
void file_pool_reset(void);
unsigned file_count(void);

#endif
