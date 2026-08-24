/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_STDIO_INTERNAL_H
#define ZEDBSD_STDIO_INTERNAL_H

#include <stdio.h>
#include <stdint.h>

/* FILE is deliberately opaque outside libc.  The first five fields retain
 * the legacy kernel-mode adapter layout while that adapter is being retired;
 * no application ABI depends on them. */
struct __stdio_file {
	void *context;
	uint64_t position;
	int error;
	int eof;
	unsigned mode;
	volatile uint32_t lock;
	uintptr_t lock_owner;
	unsigned lock_depth;
	unsigned char *buffer;
	size_t buffer_size;
	size_t buffer_start;
	size_t buffer_length;
	int buffering_mode;
	int last_operation;
	int ungot_character;
	unsigned buffer_owned;
	unsigned heap_allocated;
	unsigned io_started;
	pid_t child_pid;
	int orientation;
	int (*cookie_read)(void *, char *, int);
	int (*cookie_write)(void *, const char *, int);
	fpos_t (*cookie_seek)(void *, fpos_t, int);
	int (*cookie_close)(void *);
	struct __stdio_file *registry_next;
};

#endif
