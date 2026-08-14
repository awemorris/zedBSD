/*
 * Per-process file descriptor table
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_FILEDESC_H
#define ZEDBSD_KERN_FILEDESC_H

#define KERN_OPEN_MAX 32

struct file;
struct filedesc {
	unsigned usecount;
	struct file *files[KERN_OPEN_MAX];
};

struct filedesc *filedesc_create(void);
void filedesc_destroy(struct filedesc *);
struct file *filedesc_get(struct filedesc *, int descriptor);
int filedesc_install(struct filedesc *, struct file *, int *descriptor);
int filedesc_install_at(struct filedesc *, struct file *, int descriptor);
int filedesc_take(struct filedesc *, int descriptor, struct file **);
int filedesc_close(struct filedesc *, int descriptor);
int filedesc_clone_stdio(struct filedesc *, struct filedesc *);

#endif
