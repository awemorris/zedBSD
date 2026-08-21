/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_RECORD_LOCK_H
#define ZEDBSD_KERN_RECORD_LOCK_H

#include <stdint.h>

struct file;
struct inode;
struct process;
struct flock_record;

int record_lock_fcntl(struct process *, struct file *, int,
	struct flock_record *);
void record_lock_release_process_inode(struct process *, struct inode *);
void record_lock_inode_destroy(struct inode *);

#endif
