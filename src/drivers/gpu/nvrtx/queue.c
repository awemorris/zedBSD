/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared-memory queues between the driver and the GSP.
 *
 * One shared region holds a page listing the bus address of each of its
 * pages, then the command queue (driver to GSP), then the status queue (GSP
 * to driver).  A queue is a header page and 63 slots of one page.  A message
 * fills one to sixteen consecutive slots, wrapping at the end: an element
 * header with a checksum, the call header, and the payload.
 *
 * Each pointer has one writer.  The driver writes the command queue's write
 * pointer and its own read pointer of the status queue, which lives in the
 * command queue's header page; the GSP writes the other two.  The data of a
 * message is complete in memory before its write pointer moves, and a slot
 * is copied out before the read pointer gives it back.
 *
 * Nothing here rings the doorbell or waits: the caller polls and rings.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The command queue's header values the driver writes once.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_shared_init).
 */
#define QUEUE_VERSION			0U
#define QUEUE_FLAGS_SWAP_RX		1U
#define QUEUE_READER_OFFSET		NVRTX_QHDR_READ_AT

static uint32_t queue_load(const struct nvrtx_queue *queue, size_t header_at, size_t field);
static void queue_store(const struct nvrtx_queue *queue, size_t header_at, size_t field, uint32_t value);
static uint8_t *queue_slot(const struct nvrtx_queue *queue, uint32_t slot);
static uint32_t queue_used(uint32_t write, uint32_t read);
static uint32_t queue_slots_for(size_t call_length);
static void queue_copy_out(const struct nvrtx_queue *queue, uint32_t first, size_t offset, uint8_t *destination, size_t length);

/*
 * Prepares the shared region before the GSP starts.
 *
 * Writes the page list, clears both header pages and writes the command
 * queue's header.  The status queue's header is the GSP's to write.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_shared_init).
 */
void
nvrtx_queue_shared_init(
	const struct nvrtx_pages *shared)
{
	uint8_t *page;
	size_t index;
	size_t at;

	/* Lists the bus address of every page of the region, the list's own page included. */
	page = nvrtx_pages_cpu(shared, 0U);
	for (at = 0U; at < NVRTX_GSP_PAGE_SIZE; at++)
		page[at] = 0U;

	/* Each entry is one page's bus address, in order. */
	for (index = 0U; index < shared->page_count; index++)
		nvrtx_put64(page, index * 8U, nvrtx_pages_bus(shared, index));

	/* Clears the two header pages. */
	page = nvrtx_pages_cpu(shared, NVRTX_QUEUE_COMMAND_AT / NVRTX_GSP_PAGE_SIZE);
	for (at = 0U; at < NVRTX_GSP_PAGE_SIZE; at++)
		page[at] = 0U;

	/* The status queue's page starts empty until the GSP writes its header. */
	page = nvrtx_pages_cpu(shared, NVRTX_QUEUE_STATUS_AT / NVRTX_GSP_PAGE_SIZE);
	for (at = 0U; at < NVRTX_GSP_PAGE_SIZE; at++)
		page[at] = 0U;

	/* Writes the command queue's header: its size, its slots, and where its reader word is. */
	page = nvrtx_pages_cpu(shared, NVRTX_QUEUE_COMMAND_AT / NVRTX_GSP_PAGE_SIZE);
	nvrtx_put32(page, NVRTX_QHDR_VERSION_AT, QUEUE_VERSION);
	nvrtx_put32(page, NVRTX_QHDR_SIZE_AT, NVRTX_QUEUE_BYTES);
	nvrtx_put32(page, NVRTX_QHDR_SLOT_SIZE_AT, NVRTX_QUEUE_SLOT_BYTES);
	nvrtx_put32(page, NVRTX_QHDR_SLOT_COUNT_AT, NVRTX_QUEUE_SLOT_COUNT);
	nvrtx_put32(page, NVRTX_QHDR_WRITE_AT, 0U);
	nvrtx_put32(page, NVRTX_QHDR_FLAGS_AT, QUEUE_FLAGS_SWAP_RX);
	nvrtx_put32(page, NVRTX_QHDR_READER_AT, QUEUE_READER_OFFSET);
	nvrtx_put32(page, NVRTX_QHDR_SLOTS_AT, NVRTX_QUEUE_SLOT_BYTES);

	/* Publishes the headers before anything reads them. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/*
 * Sets up the driver's side of the two queues.
 *
 * The staging buffer holds one whole message (sixteen slots) while it is
 * built and checksummed; it belongs to the channel.
 */
void
nvrtx_channel_init(
	struct nvrtx_channel *channel,
	const struct nvrtx_pages *shared,
	uint8_t *staging)
{
	/* The command queue: its header here, its reader's pointer in the status queue's page. */
	channel->command.pages = shared;
	channel->command.at = NVRTX_QUEUE_COMMAND_AT;
	channel->command.peer_at = NVRTX_QUEUE_STATUS_AT;

	/* The status queue: its header here, the driver's read pointer in the command queue's page. */
	channel->status.pages = shared;
	channel->status.at = NVRTX_QUEUE_STATUS_AT;
	channel->status.peer_at = NVRTX_QUEUE_COMMAND_AT;

	/* Element numbering starts at zero; the staging buffer is the channel's. */
	channel->element_sequence = 0U;
	channel->staging = staging;
}

/*
 * Puts one message into the command queue.
 *
 * Returns 0 when the message is in the queue and its write pointer moved,
 * EAGAIN when the GSP has not yet freed enough slots, or E2BIG when the
 * payload does not fit one message.  The caller rings the doorbell.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c
 * (r535_gsp_cmdq_push).
 */
int
nvrtx_channel_send(
	struct nvrtx_channel *channel,
	uint32_t function,
	uint32_t sequence,
	const uint8_t *payload,
	size_t length)
{
	struct nvrtx_queue *queue;
	uint32_t slots;
	uint32_t write;
	uint32_t read;
	uint32_t free_slots;
	uint32_t checksum;
	uint32_t slot;
	size_t bytes;
	size_t at;
	uint8_t *destination;

	/* Refuses a payload larger than sixteen slots can carry. */
	if (length > NVRTX_CALL_PAYLOAD_MAX)
		return E2BIG;

	/* Counts the slots and the free room; one slot always stays empty. */
	queue = &channel->command;
	slots = queue_slots_for(NVRTX_CALL_HEADER_BYTES + length);
	write = queue_load(queue, queue->at, NVRTX_QHDR_WRITE_AT);
	read = queue_load(queue, queue->peer_at, NVRTX_QHDR_READ_AT);
	free_slots = NVRTX_QUEUE_SLOT_COUNT - 1U - queue_used(write, read);
	if (free_slots < slots)
		return EAGAIN;

	/* Clears the staging slots, so the padding of the last slot is zero. */
	bytes = (size_t)slots * NVRTX_QUEUE_SLOT_BYTES;
	for (at = 0U; at < bytes; at++)
		channel->staging[at] = 0U;

	/* Writes the element header with a zero checksum. */
	nvrtx_put32(channel->staging, NVRTX_ELEMENT_CHECKSUM_AT, 0U);
	nvrtx_put32(channel->staging, NVRTX_ELEMENT_SEQUENCE_AT, channel->element_sequence);
	nvrtx_put32(channel->staging, NVRTX_ELEMENT_SLOTS_AT, slots);

	/* Writes the call header; the results stay all ones until the GSP answers. */
	at = NVRTX_ELEMENT_HEADER_BYTES;
	nvrtx_put32(channel->staging, at + NVRTX_CALL_VERSION_AT, NVRTX_CALL_VERSION);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_SIGNATURE_AT, NVRTX_CALL_SIGNATURE);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_LENGTH_AT, (uint32_t)(NVRTX_CALL_HEADER_BYTES + length));
	nvrtx_put32(channel->staging, at + NVRTX_CALL_FUNCTION_AT, function);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_RESULT_AT, NVRTX_CALL_RESULT_PENDING);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_RESULT_PRIVATE_AT, NVRTX_CALL_RESULT_PENDING);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_SEQUENCE_AT, sequence);
	nvrtx_put32(channel->staging, at + NVRTX_CALL_SPARE_AT, 0U);

	/* Copies the payload after the call header. */
	at = NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_HEADER_BYTES;
	for (bytes = 0U; bytes < length; bytes++)
		channel->staging[at + bytes] = payload[bytes];

	/* Writes the checksum that makes the fold of the whole element zero. */
	checksum = nvrtx_fold_checksum(channel->staging, (size_t)slots * NVRTX_QUEUE_SLOT_BYTES);
	nvrtx_put32(channel->staging, NVRTX_ELEMENT_CHECKSUM_AT, checksum);

	/* Copies each slot into the queue, wrapping at the last slot. */
	for (slot = 0U; slot < slots; slot++) {
		destination = queue_slot(queue, (write + slot) % NVRTX_QUEUE_SLOT_COUNT);
		for (at = 0U; at < NVRTX_QUEUE_SLOT_BYTES; at++)
			destination[at] = channel->staging[(size_t)slot * NVRTX_QUEUE_SLOT_BYTES + at];
	}

	/*
	 * The message is complete in memory before the write pointer moves, so
	 * the GSP never sees a slot it may read before its bytes are there.
	 */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	queue_store(queue, queue->at, NVRTX_QHDR_WRITE_AT, (write + slots) % NVRTX_QUEUE_SLOT_COUNT);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* The next element carries the next number. */
	channel->element_sequence++;

	/* Succeeded: the message waits for the GSP. */
	return 0;
}

/*
 * Looks at the next message of the status queue without taking it.
 *
 * Returns 0 with its function, the caller's sequence, the payload length and
 * the result word, or EAGAIN when no whole message is there.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c
 * (r535_gsp_msgq_peek).
 */
int
nvrtx_channel_peek(
	const struct nvrtx_channel *channel,
	uint32_t *function,
	uint32_t *sequence,
	size_t *length,
	uint32_t *result)
{
	const struct nvrtx_queue *queue;
	uint32_t write;
	uint32_t read;
	uint32_t used;
	uint32_t call_length;
	uint32_t slots;
	uint8_t *first;

	/* Nothing to look at while the pointers are equal. */
	queue = &channel->status;
	write = queue_load(queue, queue->at, NVRTX_QHDR_WRITE_AT);
	read = queue_load(queue, queue->peer_at, NVRTX_QHDR_READ_AT);
	used = queue_used(write, read);
	if (used == 0U)
		return EAGAIN;

	/* Reads the call header from the message's first slot, after the pointer was read. */
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	first = queue_slot(queue, read);
	call_length = nvrtx_get32(first, NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_LENGTH_AT);

	/* A length shorter than the header or longer than a message is a broken queue. */
	if (call_length < NVRTX_CALL_HEADER_BYTES)
		return EIO;
	if (call_length > NVRTX_CALL_HEADER_BYTES + NVRTX_CALL_PAYLOAD_MAX)
		return EIO;

	/* The whole message must be in the queue. */
	slots = queue_slots_for(call_length);
	if (used < slots)
		return EAGAIN;

	/* Reports the message's header. */
	*function = nvrtx_get32(first, NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_FUNCTION_AT);
	*sequence = nvrtx_get32(first, NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_SEQUENCE_AT);
	*result = nvrtx_get32(first, NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_RESULT_AT);
	*length = call_length - NVRTX_CALL_HEADER_BYTES;

	/* Succeeded: the next message is whole. */
	return 0;
}

/*
 * Takes the next message of the status queue.
 *
 * Copies its payload (up to the capacity, the rest dropped) and gives its
 * slots back.  Returns EAGAIN when no whole message is there; the payload
 * length is reported in full even when it was cut.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c
 * (r535_gsp_msgq_recv_one_elem).
 */
int
nvrtx_channel_receive(
	struct nvrtx_channel *channel,
	uint8_t *payload,
	size_t capacity,
	size_t *length)
{
	struct nvrtx_queue *queue;
	uint32_t function;
	uint32_t sequence;
	uint32_t result;
	uint32_t read;
	uint32_t slots;
	size_t copied;
	int error;

	/* Finds a whole message. */
	error = nvrtx_channel_peek(channel, &function, &sequence, length, &result);
	if (error != 0)
		return error;

	/* Copies the payload, as much as fits. */
	queue = &channel->status;
	read = queue_load(queue, queue->peer_at, NVRTX_QHDR_READ_AT);
	copied = *length;
	if (copied > capacity)
		copied = capacity;

	/* The payload starts after both headers of the message's first slot. */
	queue_copy_out(queue, read, NVRTX_ELEMENT_HEADER_BYTES + NVRTX_CALL_HEADER_BYTES, payload, copied);

	/* Gives the slots back only after the copy is complete. */
	slots = queue_slots_for(NVRTX_CALL_HEADER_BYTES + *length);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	queue_store(queue, queue->peer_at, NVRTX_QHDR_READ_AT, (read + slots) % NVRTX_QUEUE_SLOT_COUNT);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Succeeded: the message is taken. */
	return 0;
}

/* Reads one 32-bit pointer word of a queue header page, as the other side last wrote it. */
static uint32_t
queue_load(
	const struct nvrtx_queue *queue,
	size_t header_at,
	size_t field)
{
	const volatile uint32_t *word;
	uint8_t *page;

	/* Finds the word in its header page; the words are 4-byte aligned. */
	page = nvrtx_pages_cpu(queue->pages, header_at / NVRTX_GSP_PAGE_SIZE);
	word = (const volatile uint32_t *)(const void *)(page + field);

	/* Succeeded: the word's current value. */
	return *word;
}

/* Writes one 32-bit pointer word of a queue header page in one store. */
static void
queue_store(
	const struct nvrtx_queue *queue,
	size_t header_at,
	size_t field,
	uint32_t value)
{
	volatile uint32_t *word;
	uint8_t *page;

	/* Finds the word in its header page and stores the value whole. */
	page = nvrtx_pages_cpu(queue->pages, header_at / NVRTX_GSP_PAGE_SIZE);
	word = (volatile uint32_t *)(void *)(page + field);
	*word = value;
}

/* Finds the CPU address of one slot of a queue. */
static uint8_t *
queue_slot(
	const struct nvrtx_queue *queue,
	uint32_t slot)
{
	size_t page;

	/* The slots start one page after the queue's header page. */
	page = queue->at / NVRTX_GSP_PAGE_SIZE + 1U + slot;

	/* Succeeded: the slot's page. */
	return nvrtx_pages_cpu(queue->pages, page);
}

/* Counts the slots written and not yet read, given the two pointers. */
static uint32_t
queue_used(
	uint32_t write,
	uint32_t read)
{
	/* The writer may be behind the reader after wrapping. */
	if (write >= read)
		return write - read;

	/* Succeeded: the count across the wrap. */
	return write + NVRTX_QUEUE_SLOT_COUNT - read;
}

/* Counts the slots a message of a given call length fills. */
static uint32_t
queue_slots_for(
	size_t call_length)
{
	size_t bytes;

	/* The element header comes first; the message is rounded up to whole slots. */
	bytes = NVRTX_ELEMENT_HEADER_BYTES + call_length;

	/* Succeeded: the slot count. */
	return (uint32_t)((bytes + NVRTX_QUEUE_SLOT_BYTES - 1U) / NVRTX_QUEUE_SLOT_BYTES);
}

/* Copies bytes of a message that starts in a given slot, following the wrap. */
static void
queue_copy_out(
	const struct nvrtx_queue *queue,
	uint32_t first,
	size_t offset,
	uint8_t *destination,
	size_t length)
{
	size_t done;
	size_t at;
	uint32_t slot;
	uint8_t *source;

	/* Copies byte by byte, finding the slot of each one. */
	for (done = 0U; done < length; done++) {
		at = offset + done;
		slot = (first + (uint32_t)(at / NVRTX_QUEUE_SLOT_BYTES)) % NVRTX_QUEUE_SLOT_COUNT;
		source = queue_slot(queue, slot);
		destination[done] = source[at % NVRTX_QUEUE_SLOT_BYTES];
	}
}
