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
	struct process *);

void
filedesc_ref(
	struct filedesc *);

void
filedesc_destroy(
	struct filedesc *);

struct file *
filedesc_get_ref(
	struct filedesc *,
	int descriptor);

int
filedesc_install(
	struct filedesc *,
	struct file *,
	int *descriptor);

int
filedesc_install_from(
	struct filedesc *,
	struct file *,
	unsigned,
	int minimum,
	int *descriptor);

int
filedesc_install_at(
	struct filedesc *,
	struct file *,
	int descriptor);

int
filedesc_take(
	struct filedesc *,
	int descriptor,
	struct file **);

int
filedesc_close(
	struct filedesc *,
	int descriptor);

int
filedesc_clone_stdio(
	struct filedesc *,
	struct filedesc *);

int
filedesc_clone(
	struct filedesc *,
	struct process *,
	struct filedesc **);

int
filedesc_get_flags(
	struct filedesc *,
	int,
	unsigned *);

int
filedesc_set_flags(
	struct filedesc *,
	int,
	unsigned);

int
filedesc_dup(
	struct filedesc *,
	int,
	int,
	unsigned,
	int *);

int
filedesc_dup2(
	struct filedesc *,
	int,
	int,
	unsigned,
	int);

int
filedesc_install_pair(
	struct filedesc *,
	struct file *,
	unsigned,
	struct file *,
	unsigned,
	int[2]);

int
filedesc_install_many(
	struct filedesc *,
	struct file **,
	unsigned,
	unsigned,
	int *);

int
filedesc_reserve_many(
	struct filedesc *,
	unsigned,
	unsigned,
	struct filedesc_reservation *);

int
filedesc_commit_reserved(
	struct filedesc_reservation *,
	struct file **,
	int *);

void
filedesc_abort_reserved(
	struct filedesc_reservation *);

int
filedesc_set_limit(
	struct filedesc *,
	unsigned);

unsigned
filedesc_get_limit(
	struct filedesc *);

void
filedesc_close_on_exec(
	struct filedesc *);

unsigned
filedesc_count(void);

#endif
