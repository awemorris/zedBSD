/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_RECORD_LOCK_H
#define ZEDBSD_KERN_RECORD_LOCK_H

#include <stdint.h>

struct file;
struct inode;
struct process;
struct flock_record;

int
record_lock_fcntl(
	struct process *owner,
	struct file *file,
	int command,
	struct flock_record *request);

void
record_lock_release_process_inode(
	struct process *owner,
	struct inode *inode);

void
record_lock_release_file(
	struct file *file);

void
record_lock_inode_destroy(
	struct inode *inode);

#endif
