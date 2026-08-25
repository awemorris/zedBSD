/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/input-queue.h"

#include <string.h>

static struct input_event
event(unsigned value)
{
	struct input_event result;
	memset(&result, 0, sizeof(result));
	result.type = EV_KEY;
	result.code = KEY_A;
	result.value = (int32_t)value;
	return result;
}

int
main(void)
{
	struct input_queue queue;
	struct input_queue_reader early, late;
	struct input_event output[4], item;
	size_t count;
	unsigned index;

	input_queue_init(&queue);
	input_queue_reader_init(&queue, &early);
	for (index = 0; index < 3; index++) {
		item = event(index);
		input_queue_push(&queue, &item);
	}
	input_queue_reader_init(&queue, &late);
	count = input_queue_read(&queue, &early, output, 2);
	if (count != 2 || output[0].value != 0 || output[1].value != 1)
		return 1;
	if (input_queue_readable(&queue, &late))
		return 2;
	item = event(3);
	input_queue_push(&queue, &item);
	count = input_queue_read(&queue, &late, output, 4);
	if (count != 1 || output[0].value != 3)
		return 3;

	for (index = 4; index < INPUT_QUEUE_CAPACITY + 8U; index++) {
		item = event(index);
		input_queue_push(&queue, &item);
	}
	count = input_queue_read(&queue, &early, output, 4);
	if (count != 4 || output[0].type != EV_SYN ||
	    output[0].code != SYN_DROPPED || output[1].value != 8)
		return 4;
	input_queue_detach(&queue);
	if (!queue.detached)
		return 5;
	return 0;
}
