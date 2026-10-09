/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a client's output queue (ws197-p003, plan/ws197/
 * phase003/phase.md section 4.1), built with the host's compiler under
 * ASan and UBSan.  The client is a send that takes a set number of bytes
 * at a time, or nothing (EAGAIN), or fails.
 *
 *   partial   bytes taken in pieces, in order; EAGAIN keeps the rest; a
 *             later flush sends it; an empty queue starts at its front
 *   limit     an add that would pass the limit is refused whole (ENOBUFS)
 *             and the queue keeps what it held; room comes back as the
 *             client reads
 *   gone      a failing send and a send of 0 report the client gone
 *   clear     the buffer freed, the limit kept, the queue usable again
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/outq.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The most the test client keeps of what it was sent. */
#define TEST_RECEIVED_MAX	65536U

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The test's client: how many bytes a send takes (0: none, EAGAIN),
 * whether it takes a budget of bytes in all before it takes none, an error
 * to fail with instead (0: none), whether a send returns 0, and what it
 * received.
 */
struct client {
	size_t take;
	int budgeted;
	size_t budget;
	int fail;
	int zero;
	size_t received_length;
	uint8_t received[TEST_RECEIVED_MAX];
};

static void check(int condition, const char *what);
static ssize_t client_send(void *context, const uint8_t *data, size_t length);
static void test_partial(void);
static void test_limit(void);
static void test_gone(void);
static void test_clear(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_partial();
	test_limit();
	test_gone();
	test_clear();

	/* The count of what failed. */
	printf("bt-outq-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* The test client's send: up to take bytes, EAGAIN when it takes none, or its failure. */
static ssize_t
client_send(
	void *context,
	const uint8_t *data,
	size_t length)
{
	struct client *client;
	size_t taken;

	/* The client. */
	client = context;

	/* A client gone. */
	if (client->fail != 0) {
		errno = client->fail;
		return -1;
	}

	/* A send that takes nothing and says nothing. */
	if (client->zero)
		return 0;

	/* A client that takes nothing now. */
	if (client->take == 0U || (client->budgeted && client->budget == 0U)) {
		errno = EAGAIN;
		return -1;
	}

	/* As much as it takes. */
	taken = length;
	if (taken > client->take)
		taken = client->take;
	if (client->budgeted && taken > client->budget)
		taken = client->budget;
	if (client->budgeted)
		client->budget -= taken;
	if (client->received_length + taken > sizeof(client->received))
		taken = sizeof(client->received) - client->received_length;
	memcpy(client->received + client->received_length, data, taken);
	client->received_length += taken;

	/* Succeeded: what it took. */
	return (ssize_t)taken;
}

/* Bytes taken in pieces, EAGAIN keeping the rest. */
static void
test_partial(void)
{
	static struct client client;
	struct btd_outq queue;
	int error;

	/* Two adds, a client taking 3 bytes a send. */
	btd_outq_init(&queue, 64U);
	memset(&client, 0, sizeof(client));
	error = btd_outq_append(&queue, "STATE ready\n", 12U);
	check(error == 0, "partial: the first add");
	error = btd_outq_append(&queue, "DONE\n", 5U);
	check(error == 0 && btd_outq_pending(&queue) == 17U, "partial: both held");
	client.take = 3U;
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == 0 && btd_outq_pending(&queue) == 0U, "partial: all sent in pieces");
	check(client.received_length == 17U && memcmp(client.received, "STATE ready\nDONE\n", 17U) == 0, "partial: in order");
	check(queue.first == 0U && queue.used == 0U, "partial: the empty queue starts at its front");

	/* A client that takes 4 bytes, then nothing: the rest stays. */
	client.received_length = 0U;
	error = btd_outq_append(&queue, "0123456789", 10U);
	client.take = 4U;
	client.zero = 0;
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == 0 && client.received_length == 10U, "partial: a client that takes all");
	error = btd_outq_append(&queue, "abcdefghij", 10U);
	client.take = 0U;
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == 0 && btd_outq_pending(&queue) == 10U, "partial: EAGAIN keeps the rest");
	client.take = 4U;
	client.budgeted = 1;
	client.budget = 6U;
	(void)btd_outq_flush(&queue, client_send, &client);
	check(btd_outq_pending(&queue) == 4U, "partial: part sent, part kept");
	client.budgeted = 0;
	error = btd_outq_append(&queue, "XY", 2U);
	check(error == 0 && btd_outq_pending(&queue) == 6U, "partial: an add after a part was sent");
	client.take = 100U;
	(void)btd_outq_flush(&queue, client_send, &client);
	check(client.received_length == 22U && memcmp(client.received + 10U, "abcdefghijXY", 12U) == 0, "partial: the rest in order");
	btd_outq_clear(&queue);
}

/* An add past the limit is refused whole. */
static void
test_limit(void)
{
	static struct client client;
	static uint8_t block[300];
	struct btd_outq queue;
	int error;

	/* A queue of 256 bytes. */
	btd_outq_init(&queue, 256U);
	memset(&client, 0, sizeof(client));
	memset(block, 'z', sizeof(block));
	error = btd_outq_append(&queue, block, 200U);
	check(error == 0, "limit: 200 of 256");
	error = btd_outq_append(&queue, block, 57U);
	check(error == ENOBUFS && btd_outq_pending(&queue) == 200U, "limit: 257 refused, 200 kept");
	error = btd_outq_append(&queue, block, 56U);
	check(error == 0 && btd_outq_pending(&queue) == 256U, "limit: exactly 256");
	error = btd_outq_append(&queue, block, 300U);
	check(error == ENOBUFS, "limit: more than the limit at once");

	/* The client reads 100: room for 100 again. */
	client.take = 100U;
	(void)btd_outq_flush(&queue, client_send, &client);
	client.take = 0U;
	check(btd_outq_pending(&queue) == 0U, "limit: all read");
	error = btd_outq_append(&queue, block, 256U);
	check(error == 0, "limit: room again");
	btd_outq_clear(&queue);
}

/* A client that is gone. */
static void
test_gone(void)
{
	static struct client client;
	struct btd_outq queue;
	int error;

	/* A failing send. */
	btd_outq_init(&queue, 64U);
	memset(&client, 0, sizeof(client));
	(void)btd_outq_append(&queue, "DONE\n", 5U);
	client.fail = EPIPE;
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == EPIPE && btd_outq_pending(&queue) == 5U, "gone: a failing send reported");

	/* A send of 0. */
	client.fail = 0;
	client.zero = 1;
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == EPIPE, "gone: a send of nothing reported");
	btd_outq_clear(&queue);
}

/* The queue cleared and used again. */
static void
test_clear(void)
{
	static struct client client;
	struct btd_outq queue;
	int error;

	/* Cleared: nothing held, its limit kept. */
	btd_outq_init(&queue, 32U);
	memset(&client, 0, sizeof(client));
	(void)btd_outq_append(&queue, "abc", 3U);
	btd_outq_clear(&queue);
	check(queue.bytes == NULL && btd_outq_pending(&queue) == 0U && queue.limit == 32U, "clear: empty, limit kept");

	/* Used again. */
	error = btd_outq_append(&queue, "def", 3U);
	client.take = 10U;
	(void)btd_outq_flush(&queue, client_send, &client);
	check(error == 0 && client.received_length == 3U && memcmp(client.received, "def", 3U) == 0, "clear: used again");
	btd_outq_clear(&queue);

	/* An empty flush, and a clear of a queue never used. */
	btd_outq_init(&queue, 32U);
	error = btd_outq_flush(&queue, client_send, &client);
	check(error == 0, "clear: an empty flush");
	btd_outq_clear(&queue);
}
