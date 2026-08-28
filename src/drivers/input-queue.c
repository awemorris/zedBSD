/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-queue.h"

#include <string.h>

void
input_queue_init(struct input_queue *queue)
{
	memset(queue, 0, sizeof(*queue));
}

void
input_queue_reader_init(const struct input_queue *queue,
			struct input_queue_reader *reader)
{
	reader->sequence = queue->next_sequence;
}

void
input_queue_push(struct input_queue *queue, const struct input_event *event)
{
	queue->events[queue->next_sequence % INPUT_QUEUE_CAPACITY] = *event;
	queue->next_sequence++;
	if (queue->next_sequence - queue->first_sequence > INPUT_QUEUE_CAPACITY)
		queue->first_sequence =
		    queue->next_sequence - INPUT_QUEUE_CAPACITY;
}

size_t
input_queue_read(const struct input_queue *queue,
		 struct input_queue_reader *reader, struct input_event *events,
		 size_t capacity)
{
	size_t count = 0;
	if (capacity == 0)
		return 0;
	if (reader->sequence < queue->first_sequence) {
		memset(&events[count], 0, sizeof(events[count]));
		events[count].type = EV_SYN;
		events[count].code = SYN_DROPPED;
		count++;
		reader->sequence = queue->first_sequence;
	}
	while (count < capacity && reader->sequence < queue->next_sequence) {
		events[count++] =
		    queue->events[reader->sequence % INPUT_QUEUE_CAPACITY];
		reader->sequence++;
	}
	return count;
}

int
input_queue_readable(const struct input_queue *queue,
		     const struct input_queue_reader *reader)
{
	return reader->sequence < queue->first_sequence ||
	       reader->sequence < queue->next_sequence;
}

void
input_queue_detach(struct input_queue *queue)
{
	queue->detached = 1;
}
