/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of libvulkan's recording thread (sender.c): the hand-off of
 * fence-less submissions and long recordings' batches through one FIFO,
 * with the transport (vulkan_context_execute) faked.  Checks the order,
 * the drain up to the entry's number, the back-pressure and the wake marks,
 * the latch of a failure, the batch waits, a forked child, and the abort
 * of a drain that would wait with a context mutex held.
 *
 * usage: handoff-host-test [abort-case]
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include "internal.h"

#define TEST_MAGIC_HANDOFF 0x48414e44U
#define TEST_MAGIC_SYNC 0x53594e43U
#define TEST_MAGIC_BATCH 0x42415443U
#define TEST_PRODUCERS 3U
#define TEST_ITEMS 3000U
#define TEST_RECORDED_MAX 65536U

/* Every transaction the fake transport ran, in order: magic, producer, sequence. */
struct test_record {
	uint32_t magic;
	uint32_t producer;
	uint32_t sequence;
};

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static struct test_record test_records[TEST_RECORDED_MAX];
static uint32_t test_record_count;
static uint32_t test_executed[TEST_PRODUCERS];
static uint32_t test_published[TEST_PRODUCERS];
static unsigned test_failures;
static unsigned test_checks;
static uint32_t test_fail_producer = UINT32_MAX;
static uint32_t test_fail_sequence = UINT32_MAX;
static unsigned test_delay_us;
static int test_gate_closed;
static pthread_cond_t test_gate = PTHREAD_COND_INITIALIZER;
static uint32_t test_held;
static uint64_t test_timing_counts[VULKAN_TIMING_PARTS];

/* The context every producer submits on (one session, as in a process with one device). */
static struct vulkan_context test_context;

static void check(int ok, const char *what);
static void writer_put(struct vulkan_writer *writer, uint32_t magic, uint32_t producer, uint32_t sequence, uint32_t replies);
static void *producer_main(void *argument);
static void test_order(void);
static void test_latch(void);
static void test_batches(void);
static void test_fork(void);
static int abort_case(void);
static void test_abort(const char *self);

/* The fakes of what sender.c and wire.c call. */
void *
vulkan_allocate(
	const struct vulkan_allocator *allocator,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	(void)allocator;
	(void)alignment;
	(void)scope;
	return malloc(bytes);
}

void
vulkan_free(
	const struct vulkan_allocator *allocator,
	void *allocation)
{
	(void)allocator;
	free(allocation);
}

struct vulkan_object *
vulkan_nondispatchable_object(
	uint64_t handle)
{
	return (struct vulkan_object *)(uintptr_t)handle;
}

uint64_t
vulkan_object_wire_id(
	const struct vulkan_object *object)
{
	(void)object;
	return 0U;
}

void
vulkan_context_error(
	struct vulkan_context *context,
	VkResult error)
{
	__atomic_store_n(&context->error, error, __ATOMIC_RELEASE);
}

uint32_t
vulkan_context_held(
	void)
{
	return __atomic_load_n(&test_held, __ATOMIC_ACQUIRE);
}

uint64_t
vulkan_timing_start(
	void)
{
	return 1U;
}

void
vulkan_timing_add(
	enum vulkan_timing_part part,
	uint64_t start,
	uint32_t opcode)
{
	(void)start;
	(void)opcode;
	(void)__atomic_add_fetch(&test_timing_counts[part], 1U, __ATOMIC_RELAXED);
}

/*
 * The fake transport: drains first as the real one does, refuses work on
 * a lost context, then records the transaction and answers each reply the
 * test's own encoding names (a failure where the test asks for one).
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	uint32_t words[4];
	uint32_t opcode;
	uint32_t index;
	uint8_t *reply;
	int32_t result;
	VkResult error;

	vulkan_reader_init(reader, NULL, 0);
	if (writer->error != VK_SUCCESS)
		return writer->error;

	/* No earlier hand-off may come after this one. */
	vulkan_sender_drain(context);

	/* A lost context sends nothing. */
	error = __atomic_load_n(&context->error, __ATOMIC_ACQUIRE);
	if (error != VK_SUCCESS)
		return error;

	/* The test's header. */
	memcpy(words, writer->data, sizeof(words));

	/* A closed gate holds the transport (the fork and abort cases). */
	pthread_mutex_lock(&test_lock);
	while (test_gate_closed != 0)
		pthread_cond_wait(&test_gate, &test_lock);

	/* A transaction that drains must find every earlier hand-off of its producer run. */
	if (words[0] == TEST_MAGIC_SYNC)
		check(test_executed[words[1]] == words[2], "a drained transaction comes after every earlier hand-off");

	/* A hand-off runs in its producer's order. */
	if (words[0] == TEST_MAGIC_HANDOFF) {
		check(test_executed[words[1]] == words[2], "hand-offs of one producer run in order");
		test_executed[words[1]] = words[2] + 1U;
	}
	if (test_record_count < TEST_RECORDED_MAX) {
		test_records[test_record_count].magic = words[0];
		test_records[test_record_count].producer = words[1];
		test_records[test_record_count].sequence = words[2];
		test_record_count++;
	}
	pthread_mutex_unlock(&test_lock);

	/* A slow renderer, so the producers outrun the thread. */
	if (test_delay_us != 0U)
		usleep(test_delay_us);

	/* Each reply: its opcode and its result. */
	reply = calloc(1, reply_capacity + 8U);
	if (reply == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	for (index = 0U; index < words[3] && (size_t)(index + 1U) * 8U <= reply_capacity; index++) {
		memcpy(&opcode, writer->data + 16U + index * 4U, sizeof(opcode));
		result = VK_SUCCESS;
		if (words[0] == TEST_MAGIC_HANDOFF && words[1] == test_fail_producer && words[2] == test_fail_sequence &&
		    index + 1U == words[3])
			result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		memcpy(reply + index * 8U, &opcode, sizeof(opcode));
		memcpy(reply + index * 8U + 4U, &result, sizeof(result));
	}
	vulkan_reader_init(reader, reply, reply_capacity);
	return VK_SUCCESS;
}

int
main(
	int argc,
	char **argv)
{
	if (argc > 1 && strcmp(argv[1], "abort-case") == 0)
		return abort_case();

	test_order();
	test_latch();
	test_batches();
	test_fork();
	test_abort(argv[0]);

	printf("handoff-host-test: %u checks, %u failures\n", test_checks, test_failures);
	if (test_failures != 0U) {
		printf("handoff-host-test: FAIL\n");
		return 1;
	}
	printf("handoff-host-test: PASS\n");
	return 0;
}

static void
check(
	int ok,
	const char *what)
{
	__atomic_add_fetch(&test_checks, 1U, __ATOMIC_RELAXED);
	if (ok)
		return;
	if (__atomic_add_fetch(&test_failures, 1U, __ATOMIC_RELAXED) <= 20U)
		printf("FAIL %s\n", what);
}

/* The test's encoding: magic, producer, sequence, reply count, then the reply opcodes. */
static void
writer_put(
	struct vulkan_writer *writer,
	uint32_t magic,
	uint32_t producer,
	uint32_t sequence,
	uint32_t replies)
{
	uint32_t index;

	writer->bytes = 0;
	writer->error = VK_SUCCESS;
	vulkan_write_u32(writer, magic);
	vulkan_write_u32(writer, producer);
	vulkan_write_u32(writer, sequence);
	vulkan_write_u32(writer, replies);
	for (index = 0U; index < replies; index++)
		vulkan_write_u32(writer, 100U + index);
}

/* A producer: hand-offs on its own ring, a drained transaction now and then (a fenced submission). */
static void *
producer_main(
	void *argument)
{
	struct vulkan_handoff_ring *ring;
	struct vulkan_handoff *slot;
	struct vulkan_allocator allocator;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t producer;
	uint32_t sequence;
	uint32_t replies;
	uint32_t index;
	VkResult status;

	producer = (uint32_t)(uintptr_t)argument;
	memset(&allocator, 0, sizeof(allocator));
	status = vulkan_handoff_ring_create(&ring, &allocator);
	check(status == VK_SUCCESS, "a ring is made");
	if (status != VK_SUCCESS)
		return NULL;
	vulkan_writer_init(&writer);

	for (sequence = 0U; sequence < TEST_ITEMS; sequence++) {
		/* A hand-off with one to three recordings' replies and the submission's. */
		slot = vulkan_handoff_take(ring);
		check(slot->item.waiting == VK_FALSE, "a taken place is free");
		replies = 1U + sequence % 4U;
		writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, producer, sequence, replies);
		for (index = 0U; index < replies; index++)
			slot->expected[index] = 100U + index;
		slot->replies = replies;
		vulkan_handoff_publish(ring, slot, &test_context);
		pthread_mutex_lock(&test_lock);
		test_published[producer] = sequence + 1U;
		pthread_mutex_unlock(&test_lock);

		/* Every 97th a transaction of its own, which must come after this producer's hand-offs. */
		if (sequence % 97U == 96U) {
			writer_put(&writer, TEST_MAGIC_SYNC, producer, sequence + 1U, 1U);
			status = vulkan_context_execute(&test_context, &writer, 8U, &reader);
			check(status == VK_SUCCESS, "a drained transaction runs");
			vulkan_reader_finish(&reader);
		}
	}

	/* The ring goes once its hand-offs are sent. */
	vulkan_sender_drain(&test_context);
	pthread_mutex_lock(&test_lock);
	check(test_executed[producer] == TEST_ITEMS, "every hand-off ran before the drain returned");
	pthread_mutex_unlock(&test_lock);
	vulkan_handoff_ring_destroy(ring, &allocator);
	vulkan_writer_finish(&writer);
	return NULL;
}

/* Three producers on one context, the renderer slow: order, drain, back-pressure and wakes. */
static void
test_order(
	void)
{
	pthread_t threads[TEST_PRODUCERS];
	uint64_t sent;
	uint64_t queued;
	uint32_t index;
	VkBool32 allowed;

	allowed = vulkan_handoff_allowed();
	check(allowed != VK_FALSE, "hand-off is allowed (thread runs, more than one processor)");
	check(vulkan_sender_started() != VK_FALSE, "the recording thread runs");

	test_delay_us = 3U;
	for (index = 0U; index < TEST_PRODUCERS; index++)
		pthread_create(&threads[index], NULL, producer_main, (void *)(uintptr_t)index);
	for (index = 0U; index < TEST_PRODUCERS; index++)
		pthread_join(threads[index], NULL);
	test_delay_us = 0U;

	queued = __atomic_load_n(&test_context.handoff_queued, __ATOMIC_ACQUIRE);
	sent = __atomic_load_n(&test_context.handoff_sent, __ATOMIC_ACQUIRE);
	check(queued == (uint64_t)TEST_PRODUCERS * TEST_ITEMS, "every hand-off was counted");
	check(sent == queued, "every hand-off was sent");
	check(test_context.error == VK_SUCCESS, "no failure was latched");

	/* The producers outran the thread: they slept for room, about once per half ring, not once per hand-off. */
	printf("order: room sleeps %llu, thread sleeps %llu, wakes %llu, drains %llu\n",
	       (unsigned long long)test_timing_counts[VULKAN_TIMING_HANDOFF_ROOM],
	       (unsigned long long)test_timing_counts[VULKAN_TIMING_SENDER_SLEEP],
	       (unsigned long long)test_timing_counts[VULKAN_TIMING_SENDER_WAKE],
	       (unsigned long long)test_timing_counts[VULKAN_TIMING_DRAIN]);
	check(test_timing_counts[VULKAN_TIMING_HANDOFF_ROOM] != 0U, "a full ring made a producer sleep");
	check(test_timing_counts[VULKAN_TIMING_HANDOFF_ROOM] <= (uint64_t)TEST_PRODUCERS * TEST_ITEMS / 16U,
	      "a producer sleeps once per half ring at most");
	check(test_timing_counts[VULKAN_TIMING_SENDER_WAKE] <= test_timing_counts[VULKAN_TIMING_SENDER_SLEEP] + 1U,
	      "the thread is woken only when it sleeps");
	check(test_timing_counts[VULKAN_TIMING_SENDER_WAKE] < (uint64_t)TEST_PRODUCERS * TEST_ITEMS / 4U,
	      "a busy thread is not woken for each hand-off");
}

/* A failure of one hand-off loses the context; later ones are not sent but still counted, so a drain ends. */
static void
test_latch(
	void)
{
	struct vulkan_handoff_ring *ring;
	struct vulkan_handoff *slot;
	struct vulkan_allocator allocator;
	uint32_t sequence;
	uint32_t before;
	VkResult status;

	memset(&allocator, 0, sizeof(allocator));
	status = vulkan_handoff_ring_create(&ring, &allocator);
	check(status == VK_SUCCESS, "a ring is made");
	if (status != VK_SUCCESS)
		return;

	/* Producer 0 again, its sequences going on. */
	before = test_executed[0];
	test_fail_producer = 0U;
	test_fail_sequence = before + 5U;
	for (sequence = before; sequence < before + 20U; sequence++) {
		slot = vulkan_handoff_take(ring);
		writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, 0U, sequence, 2U);
		slot->expected[0] = 100U;
		slot->expected[1] = 101U;
		slot->replies = 2U;
		vulkan_handoff_publish(ring, slot, &test_context);
	}
	vulkan_sender_drain(&test_context);

	check(test_context.error == VK_ERROR_DEVICE_LOST, "a failed hand-off loses the context");
	check(test_executed[0] == before + 6U, "nothing after the failed hand-off was sent");
	check(test_context.handoff_sent == test_context.handoff_queued, "the hand-offs after a failure still count as sent");
	vulkan_handoff_ring_destroy(ring, &allocator);
	test_context.error = VK_SUCCESS;
	test_fail_producer = UINT32_MAX;
}

/* A batch: its pending count, decremented by its sent callback under the lock. */
struct test_batch {
	struct vulkan_sender_item item;
	struct vulkan_writer part;
	uint32_t *pending;
	uint32_t sequence;
};

static VkResult
test_batch_send(
	struct vulkan_sender_item *item)
{
	struct test_batch *batch;
	struct vulkan_reader reader;
	VkResult status;

	batch = (struct test_batch *)item;
	status = vulkan_context_execute(&test_context, &batch->part, 8U, &reader);
	vulkan_reader_finish(&reader);
	return status;
}

static void
test_batch_sent(
	struct vulkan_sender_item *item,
	VkResult status)
{
	struct test_batch *batch;

	(void)status;
	batch = (struct test_batch *)item;
	(*batch->pending)--;
}

/* Batches and hand-offs share one FIFO: their order is kept, and a batch wait ends when its pieces are sent. */
static void
test_batches(
	void)
{
	struct vulkan_handoff_ring *ring;
	struct vulkan_handoff *slot;
	struct vulkan_allocator allocator;
	struct test_batch batches[8];
	uint32_t pending;
	uint32_t index;
	uint32_t first;
	uint32_t at;
	uint32_t sequence;
	VkResult status;

	memset(&allocator, 0, sizeof(allocator));
	status = vulkan_handoff_ring_create(&ring, &allocator);
	check(status == VK_SUCCESS, "a ring is made");
	if (status != VK_SUCCESS)
		return;
	memset(batches, 0, sizeof(batches));
	pending = 0U;
	first = test_record_count;
	sequence = test_executed[1];

	/* A hand-off, a batch, in turn. */
	for (index = 0U; index < 8U; index++) {
		slot = vulkan_handoff_take(ring);
		writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, 1U, sequence + index, 1U);
		slot->expected[0] = 100U;
		slot->replies = 1U;
		vulkan_handoff_publish(ring, slot, &test_context);

		vulkan_writer_init(&batches[index].part);
		writer_put(&batches[index].part, TEST_MAGIC_BATCH, 1U, index, 1U);
		batches[index].pending = &pending;
		batches[index].item.send = test_batch_send;
		batches[index].item.sent = test_batch_sent;
		vulkan_sender_lock();
		pending++;
		vulkan_sender_queue_locked(&batches[index].item);
		vulkan_sender_unlock();
	}

	/* The batches' wait, as command_sent_wait() does. */
	vulkan_sender_lock();
	while (pending != 0U)
		vulkan_sender_wait_locked();
	vulkan_sender_unlock();

	pthread_mutex_lock(&test_lock);
	check(test_record_count - first == 16U, "every batch and hand-off ran");
	for (at = first, index = 0U; at < test_record_count; at++, index++) {
		if (index % 2U == 0U)
			check(test_records[at].magic == TEST_MAGIC_HANDOFF && test_records[at].sequence == sequence + index / 2U,
			      "a hand-off runs in its place among batches");
		else
			check(test_records[at].magic == TEST_MAGIC_BATCH && test_records[at].sequence == index / 2U,
			      "a batch runs in its place among hand-offs");
	}
	pthread_mutex_unlock(&test_lock);
	for (index = 0U; index < 8U; index++)
		vulkan_writer_finish(&batches[index].part);
	vulkan_sender_drain(&test_context);
	vulkan_handoff_ring_destroy(ring, &allocator);
}

/* A child forked while hand-offs wait: no thread, no hand-off, no wait for the parent's pieces, a usable lock. */
static void
test_fork(
	void)
{
	struct vulkan_handoff_ring *ring;
	struct vulkan_handoff *slot;
	struct vulkan_allocator allocator;
	pid_t child;
	int status;
	int code;
	VkResult made;

	memset(&allocator, 0, sizeof(allocator));
	made = vulkan_handoff_ring_create(&ring, &allocator);
	check(made == VK_SUCCESS, "a ring is made");
	if (made != VK_SUCCESS)
		return;

	/* Two hand-offs held at the transport. */
	pthread_mutex_lock(&test_lock);
	test_gate_closed = 1;
	pthread_mutex_unlock(&test_lock);
	slot = vulkan_handoff_take(ring);
	writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, 2U, test_executed[2], 1U);
	slot->expected[0] = 100U;
	slot->replies = 1U;
	vulkan_handoff_publish(ring, slot, &test_context);
	slot = vulkan_handoff_take(ring);
	writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, 2U, test_executed[2] + 1U, 1U);
	slot->expected[0] = 100U;
	slot->replies = 1U;
	vulkan_handoff_publish(ring, slot, &test_context);

	fflush(stdout);
	child = fork();
	if (child == 0) {
		code = 0;
		if (vulkan_handoff_allowed() != VK_FALSE)
			code |= 1;
		if (vulkan_sender_started() != VK_FALSE)
			code |= 2;
		vulkan_sender_drain(&test_context);
		vulkan_sender_lock();
		vulkan_sender_unlock();
		_exit(code);
	}
	check(child > 0, "fork");
	status = 0;
	if (child > 0)
		waitpid(child, &status, 0);
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the forked child has no hand-off and does not wait");

	/* The parent goes on: the gate opens, the hand-offs are sent. */
	pthread_mutex_lock(&test_lock);
	test_gate_closed = 0;
	pthread_cond_broadcast(&test_gate);
	pthread_mutex_unlock(&test_lock);
	vulkan_sender_drain(&test_context);
	check(test_context.handoff_sent == test_context.handoff_queued, "the parent's hand-offs were sent after the fork");
	check(vulkan_handoff_allowed() != VK_FALSE, "the parent still hands off");
	vulkan_handoff_ring_destroy(ring, &allocator);
}

/* In a fresh process: a drain that would wait with a context mutex held aborts. */
static int
abort_case(
	void)
{
	struct vulkan_handoff_ring *ring;
	struct vulkan_handoff *slot;
	struct vulkan_allocator allocator;

	memset(&allocator, 0, sizeof(allocator));
	if (vulkan_handoff_allowed() == VK_FALSE || vulkan_handoff_ring_create(&ring, &allocator) != VK_SUCCESS)
		return 3;
	test_gate_closed = 1;
	slot = vulkan_handoff_take(ring);
	writer_put(&slot->transaction, TEST_MAGIC_HANDOFF, 0U, 0U, 1U);
	slot->expected[0] = 100U;
	slot->replies = 1U;
	vulkan_handoff_publish(ring, slot, &test_context);
	__atomic_store_n(&test_held, 1U, __ATOMIC_RELEASE);
	vulkan_sender_drain(&test_context);
	return 4;
}

static void
test_abort(
	const char *self)
{
	pid_t child;
	int status;

	fflush(stdout);
	child = fork();
	if (child == 0) {
		execl(self, self, "abort-case", (char *)NULL);
		_exit(5);
	}
	status = 0;
	if (child > 0)
		waitpid(child, &status, 0);
	check(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "a drain with a context mutex held aborts");
}
