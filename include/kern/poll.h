/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef KERN_KERN_POLL_H
#define KERN_KERN_POLL_H

#include <uapi/poll.h>
#include <stdint.h>

struct file;
struct process;

void
poll_init(void);

void
poll_notify(void);

uint64_t
poll_sequence(void);

int
poll_wait(
	uint64_t observed,
	uint64_t deadline,
	unsigned flags);

int
file_poll(
	struct file *file,
	short events,
	short *revents);

int
kern_poll_wait(
	struct process *process,
	struct pollfd *fds,
	nfds_t count,
	uint64_t deadline,
	int immediate,
	int *ready);

#endif
