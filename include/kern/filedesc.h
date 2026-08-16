/*
 * Per-process file descriptor table
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_FILEDESC_H
#define ZEDBSD_KERN_FILEDESC_H

#include <kern/atomic.h>
#include <kern/lock.h>

#define KERN_OPEN_MAX 32
#define FILEDESC_CLOEXEC 0x00000001U

struct file;
struct filedesc_entry {
	struct file *file;
	unsigned flags;
};

struct filedesc {
	refcount_t refs;
	struct spinlock lock;
	struct filedesc_entry entries[KERN_OPEN_MAX];
};

struct filedesc *filedesc_create(void);
void filedesc_ref(struct filedesc *);
void filedesc_destroy(struct filedesc *);
struct file *filedesc_get_ref(struct filedesc *, int descriptor);
int filedesc_install(struct filedesc *, struct file *, int *descriptor);
int filedesc_install_from(struct filedesc *, struct file *, unsigned,
			  int minimum, int *descriptor);
int filedesc_install_at(struct filedesc *, struct file *, int descriptor);
int filedesc_take(struct filedesc *, int descriptor, struct file **);
int filedesc_close(struct filedesc *, int descriptor);
int filedesc_clone_stdio(struct filedesc *, struct filedesc *);
int filedesc_clone(struct filedesc *, struct filedesc **);
int filedesc_get_flags(struct filedesc *, int, unsigned *);
int filedesc_set_flags(struct filedesc *, int, unsigned);
int filedesc_dup(struct filedesc *, int, int, unsigned, int *);
int filedesc_dup2(struct filedesc *, int, int, unsigned, int);
int filedesc_install_pair(struct filedesc *, struct file *, unsigned,
			  struct file *, unsigned, int [2]);
void filedesc_close_on_exec(struct filedesc *);
unsigned filedesc_count(void);

#endif
