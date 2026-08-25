/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_INPUT_QUEUE_H
#define ZEDBSD_KERN_INPUT_QUEUE_H

#include <zedbsd/input.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_QUEUE_CAPACITY 256U

struct input_queue {
	struct input_event events[INPUT_QUEUE_CAPACITY];
	uint64_t first_sequence;
	uint64_t next_sequence;
	int detached;
};

struct input_queue_reader {
	uint64_t sequence;
};

void input_queue_init(struct input_queue *);
void input_queue_reader_init(const struct input_queue *,
			     struct input_queue_reader *);
void input_queue_push(struct input_queue *, const struct input_event *);
size_t input_queue_read(const struct input_queue *, struct input_queue_reader *,
			struct input_event *, size_t);
int input_queue_readable(const struct input_queue *,
			 const struct input_queue_reader *);
void input_queue_detach(struct input_queue *);

#endif
