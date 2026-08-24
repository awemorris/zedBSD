/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-process file descriptor table
 */

#ifndef ZEDBSD_KERN_FILEDESC_H
#define ZEDBSD_KERN_FILEDESC_H

#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/waitq.h>

#define KERN_OPEN_MAX		32
#define FILEDESC_CLOEXEC	0x00000001U

struct file;
struct process;

enum filedesc_slot_state {
	FILEDESC_SLOT_FREE,
	FILEDESC_SLOT_RESERVED,
	FILEDESC_SLOT_LIVE,
};

struct filedesc_entry {
	struct file *file;
	unsigned flags;
	enum filedesc_slot_state state;
	uint64_t reservation_id;
};

struct filedesc {
	refcount_t refs;
	struct spinlock lock;
	struct process *owner;
	unsigned soft_limit;
	uint64_t reservation_generation;
	struct wait_queue reservation_waitq;
	struct filedesc_entry entries[KERN_OPEN_MAX];
};

struct filedesc_reservation {
	struct filedesc *table;
	unsigned count;
	unsigned flags;
	int slots[KERN_OPEN_MAX];
	uint64_t generation;
	unsigned active;
};

struct filedesc *
filedesc_create(
	struct process *owner);

void
filedesc_ref(
	struct filedesc *fd);

void
filedesc_destroy(
	struct filedesc *fd);

struct file *
filedesc_get_ref(
	struct filedesc *fd,
	int descriptor);

int
filedesc_install(
	struct filedesc *fd,
	struct file *file,
	int *descriptor);

int
filedesc_install_from(
	struct filedesc *fd,
	struct file *file,
	unsigned flags,
	int minimum,
	int *descriptor);

int
filedesc_install_at(
	struct filedesc *fd,
	struct file *file,
	int descriptor);

int
filedesc_take(
	struct filedesc *fd,
	int descriptor,
	struct file **result);

int
filedesc_close(
	struct filedesc *fd,
	int descriptor);

int
filedesc_clone_stdio(
	struct filedesc *source,
	struct filedesc *destination);

int
filedesc_clone(
	struct filedesc *source,
	struct process *owner,
	struct filedesc **result);

int
filedesc_get_flags(
	struct filedesc *fd,
	int descriptor,
	unsigned *flags);

int
filedesc_set_flags(
	struct filedesc *fd,
	int descriptor,
	unsigned flags);

int
filedesc_dup(
	struct filedesc *fd,
	int oldfd,
	int minimum,
	unsigned flags,
	int *result);

int
filedesc_dup2(
	struct filedesc *fd,
	int oldfd,
	int newfd,
	unsigned flags,
	int reject_equal);

int
filedesc_install_pair(
	struct filedesc *fd,
	struct file *first,
	unsigned first_flags,
	struct file *second,
	unsigned second_flags,
	int result[2]);

int
filedesc_install_many(
	struct filedesc *fd,
	struct file **files,
	unsigned count,
	unsigned flags,
	int *result);

int
filedesc_reserve_many(
	struct filedesc *fd,
	unsigned count,
	unsigned flags,
	struct filedesc_reservation *reservation);

int
filedesc_commit_reserved(
	struct filedesc_reservation *reservation,
	struct file **files,
	int *descriptors);

void
filedesc_abort_reserved(
	struct filedesc_reservation *reservation);

int
filedesc_set_limit(
	struct filedesc *fd,
	unsigned limit);

unsigned
filedesc_get_limit(
	struct filedesc *fd);

void
filedesc_close_on_exec(
	struct filedesc *fd);

unsigned
filedesc_count(void);

#endif
