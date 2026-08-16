/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_PIPE_H
#define ZEDBSD_KERN_PIPE_H

struct file;
struct file_ops;

#define KERN_PIPE_CAPACITY 4096U
#define KERN_PIPE_BUF 512U

int pipe_create(int flags, struct file **read_file, struct file **write_file);
unsigned pipe_count(void);
extern const struct file_ops fifo_file_ops;

#endif
