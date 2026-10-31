/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The unit test of the storage queue (storage/queue.c): the order the
 * backend sees, write-behind, the merging of waiting writes, the limits,
 * errors, lists, flush and close, and many threads using one queue.
 *
 * The backend is a fake kept in memory.  It records every call in order,
 * the thread that made it, and how many calls were inside it at once, and
 * it can be told to wait at a gate, to sleep, or to fail.
 */

#include "check.h"

#include "storage/queue.h"
#include "storage/records.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

/* How many records the fake holds, and how many calls it logs. */
#define FAKE_RECORDS		64
#define FAKE_EVENTS		4096

/* The size of a key text in the fake, with room for the longest key and a NUL. */
#define FAKE_KEY_SIZE		256

/* The kinds of calls the fake counts (the bits of the fake's gate and failure masks). */
#define CALL_READ		0x01
#define CALL_WRITE		0x02
#define CALL_REMOVE		0x04
#define CALL_LIST		0x08
#define CALL_CLEAR		0x10
#define CALL_ALL		0x1f

/* The most failures the fake can be told to produce in a row. */
#define FAKE_FAILURES		4

/* The spaces the tests use. */
#define SPACE_ONE		BROWSER_STORAGE_COOKIES
#define SPACE_TWO		BROWSER_STORAGE_CACHE

/* The number of threads and the passes each makes in the stress test. */
#define STRESS_THREADS		4
#define STRESS_PASSES		1000

/*
 * One call the fake saw: its kind letter ('R', 'W', 'X' for remove, 'L',
 * 'C' for clear, 'Q' for close), the space, the key and the byte count.
 */
struct fake_event {
	char kind;
	unsigned space;
	char key[FAKE_KEY_SIZE];
	size_t length;
};

/* One record the fake holds. */
struct fake_record {
	int used;
	unsigned space;
	char key[FAKE_KEY_SIZE];
	unsigned char *bytes;
	size_t length;
};

/*
 * The fake backend.  It is large, so each test keeps its one as a static
 * (a Windows thread's stack is small).  The lock protects everything so the test's own
 * thread may change the controls and read the log while the storage
 * thread calls the fake; the queue promises the calls themselves are one
 * at a time, which the fake checks and the test asserts.
 */
struct fake {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	struct fake_record records[FAKE_RECORDS];
	struct fake_event events[FAKE_EVENTS];
	size_t event_count;
	int record_events;
	int inside;
	int max_inside;
	int thread_known;
	pthread_t thread;
	int wrong_thread;
	int name_checked;
	int name_wrong;
	int gate_mask;
	int entered;
	unsigned write_delay_ms;
	int fail_mask;
	int fail_errors[FAKE_FAILURES];
	int fail_total;
	int fail_next;
	int list_extras;
	int big_read;
	int closes;
	int closed;
	int calls_after_close;
	int total_calls;
};

/* The one-megabyte block of zeros the big read appends. */
static unsigned char zero_megabyte[1024 * 1024];

static void test_open(void);
static void test_arguments(void);
static void test_order(void);
static void test_write_behind(void);
static void test_merge(void);
static void test_merge_read_barrier(void);
static void test_limits(void);
static void test_item_limit(void);
static void test_errors(void);
static void test_read_errors(void);
static void test_list(void);
static void test_flush_close(void);
static void test_stress(void);

static const struct browser_storage_calls fake_calls;

static void fake_init(struct fake *fake);
static void fake_destroy(struct fake *fake);
static void fake_set_gate(struct fake *fake, int mask);
static void fake_wait_entered(struct fake *fake, int count);
static void fake_fail(struct fake *fake, int mask, int first, int second);
static int fake_enter(struct fake *fake, char kind, unsigned space, const char *key, size_t length);
static void fake_leave(struct fake *fake);
static struct fake_record *fake_find(struct fake *fake, unsigned space, const char *key);
static int fake_read(void *context, unsigned space, const char *key, struct browser_storage_sink *sink, int *found);
static int fake_write(void *context, unsigned space, const char *key, const void *bytes, size_t length);
static int fake_remove(void *context, unsigned space, const char *key);
static int fake_list(void *context, unsigned space, int (*each)(void *each_context, const char *key), void *each_context);
static int fake_clear(void *context, unsigned space);
static void fake_close(void *context);
static void fake_log_text(struct fake *fake, char *text, size_t size);
static size_t fake_event_count(struct fake *fake, char kind, const char *key);
static int fake_has(struct fake *fake, unsigned space, const char *key, const char *bytes);
static struct wb_storage_queue *open_queue(struct fake *fake);
static void put(struct wb_storage_queue *queue, unsigned space, const char *key, const char *text);
static int expect_record(struct wb_storage_queue *queue, unsigned space, const char *key, const char *text);
static int expect_absent(struct wb_storage_queue *queue, unsigned space, const char *key);
static long long now_ms(void);
static void sleep_ms(unsigned ms);
static void key_text(char *text, unsigned number);

int
main(
	int argc,
	char **argv)
{
	(void)argc;
	(void)argv;

	test_open();
	test_arguments();
	test_order();
	test_write_behind();
	test_merge();
	test_merge_read_barrier();
	test_limits();
	test_item_limit();
	test_errors();
	test_read_errors();
	test_list();
	test_flush_close();
	test_stress();

	return check_finish("queue");
}

/* The table of the fake backend. */
static const struct browser_storage_calls fake_calls = {
	BROWSER_STORAGE_VERSION,
	fake_read,
	fake_write,
	fake_remove,
	fake_list,
	fake_clear,
	fake_close
};

/*
 * Opening: a queue without a backend, tables that are refused, a table
 * without a close member.
 */
static void
test_open(void)
{
	struct wb_storage_queue *queue;
	struct browser_storage_calls table;
	struct wb_buffer out;
	struct wb_buffer keys;
	static struct fake fake;
	size_t count;
	int found;
	int error;
	int i;
	char big[16];

	/* A place for the queue is required. */
	error = wb_storage_queue_open(NULL, NULL, NULL);
	CHECK_EQ(error, EINVAL);

	/* A queue without a backend drops writes and finds nothing. */
	queue = (struct wb_storage_queue *)1;
	error = wb_storage_queue_open(NULL, NULL, &queue);
	CHECK_EQ(error, 0);
	CHECK(queue != NULL);
	if (queue != NULL) {
		wb_buffer_init(&out);
		wb_buffer_init(&keys);
		wb_buffer_append_string(&out, "pre");
		wb_buffer_append_string(&keys, "kept");
		found = 7;
		count = 9;

		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", "xyz", 3), 0);
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", NULL, 0), 0);
		CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "a1"), 0);
		CHECK_EQ(wb_storage_queue_clear(queue, SPACE_ONE), 0);
		CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, &found), 0);
		CHECK_EQ(found, 0);
		CHECK_EQ(out.length, 3);
		CHECK(memcmp(out.data, "pre", 3) == 0);
		CHECK_EQ(wb_storage_queue_list(queue, SPACE_ONE, &keys, &count), 0);
		CHECK_EQ(count, 0);
		CHECK_EQ(keys.length, 4);
		wb_storage_queue_flush(queue);
		CHECK_EQ(wb_storage_queue_take_error(queue), 0);

		/* The arguments are still checked. */
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "A1", "x", 1), EINVAL);
		CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, ""), EINVAL);
		CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, "zz", &out, &found), EINVAL);
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", NULL, 1), EINVAL);
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", big, WB_STORAGE_QUEUE_LIMIT + 1UL), EFBIG);

		wb_buffer_release(&out);
		wb_buffer_release(&keys);
		wb_storage_queue_close(queue);
	}

	/* A table of another version, or with a member missing, is refused and its close is not called. */
	fake_init(&fake);
	for (i = 0; i < 7; i++) {
		table = fake_calls;
		if (i == 0)
			table.version = BROWSER_STORAGE_VERSION + 1;
		if (i == 1)
			table.version = 0;
		if (i == 2)
			table.read = NULL;
		if (i == 3)
			table.write = NULL;
		if (i == 4)
			table.remove = NULL;
		if (i == 5)
			table.list = NULL;
		if (i == 6)
			table.clear = NULL;
		queue = (struct wb_storage_queue *)1;
		error = wb_storage_queue_open(&table, &fake, &queue);
		CHECK_EQ(error, ENOTSUP);
		CHECK(queue == NULL);
	}
	CHECK_EQ(fake.closes, 0);
	CHECK_EQ(fake.total_calls, 0);

	/* A table without a close member opens, works and closes. */
	table = fake_calls;
	table.close = NULL;
	queue = NULL;
	error = wb_storage_queue_open(&table, &fake, &queue);
	CHECK_EQ(error, 0);
	CHECK(queue != NULL);
	if (queue != NULL) {
		put(queue, SPACE_ONE, "a1", "v");
		wb_storage_queue_close(queue);
	}
	CHECK_EQ(fake.closes, 0);
	CHECK(fake_has(&fake, SPACE_ONE, "a1", "v"));

	/* Closing nothing does nothing. */
	wb_storage_queue_close(NULL);

	fake_destroy(&fake);
}

/*
 * Every function refuses NULL arguments and invalid keys, and the
 * backend is never reached for them.
 */
static void
test_arguments(void)
{
	struct wb_storage_queue *queue;
	struct wb_buffer out;
	struct wb_buffer keys;
	static struct fake fake;
	char long_key[FAKE_KEY_SIZE + 8];
	size_t count;
	int found;
	unsigned i;

	/* A queue that is not there is refused by every call. */
	wb_buffer_init(&out);
	wb_buffer_init(&keys);
	CHECK_EQ(wb_storage_queue_write(NULL, SPACE_ONE, "a1", "x", 1), EINVAL);
	CHECK_EQ(wb_storage_queue_remove(NULL, SPACE_ONE, "a1"), EINVAL);
	CHECK_EQ(wb_storage_queue_clear(NULL, SPACE_ONE), EINVAL);
	CHECK_EQ(wb_storage_queue_read(NULL, SPACE_ONE, "a1", &out, &found), EINVAL);
	CHECK_EQ(wb_storage_queue_list(NULL, SPACE_ONE, &keys, &count), EINVAL);
	wb_storage_queue_flush(NULL);
	CHECK_EQ(wb_storage_queue_take_error(NULL), 0);

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* NULL arguments are refused. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, NULL, "x", 1), EINVAL);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", NULL, 1), EINVAL);
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, NULL), EINVAL);
	CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, NULL, &out, &found), EINVAL);
	CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, "a1", NULL, &found), EINVAL);
	CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, NULL), EINVAL);
	CHECK_EQ(wb_storage_queue_list(queue, SPACE_ONE, NULL, &count), EINVAL);
	CHECK_EQ(wb_storage_queue_list(queue, SPACE_ONE, &keys, NULL), EINVAL);

	/* Keys outside [0-9a-f-] or of the wrong length are refused by every call that takes one. */
	for (i = 0; i < 4; i++) {
		const char *bad = "";
		if (i == 0)
			bad = "";
		if (i == 1)
			bad = "A1";
		if (i == 2)
			bad = "a b";
		if (i == 3) {
			memset(long_key, 'a', 201);
			long_key[201] = 0;
			bad = long_key;
		}
		found = 5;
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, bad, "x", 1), EINVAL);
		CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, bad), EINVAL);
		CHECK_EQ(wb_storage_queue_read(queue, SPACE_ONE, bad, &out, &found), EINVAL);
	}

	/* The longest valid key (200 characters) works. */
	memset(long_key, 'b', 200);
	long_key[200] = 0;
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, long_key, "long", 4), 0);
	CHECK(expect_record(queue, SPACE_ONE, long_key, "long"));

	/* A record of no bytes may be written with NULL bytes and is found with no bytes. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "e0", NULL, 0), 0);
	wb_storage_queue_flush(queue);
	CHECK(fake_has(&fake, SPACE_ONE, "e0", ""));
	CHECK(expect_record(queue, SPACE_ONE, "e0", ""));

	/* None of the refused calls reached the backend: only the two writes and the two reads above did. */
	CHECK_EQ(fake_event_count(&fake, 'W', "e0"), 1);
	CHECK_EQ(fake.total_calls, 4);

	wb_storage_queue_close(queue);
	wb_buffer_release(&out);
	wb_buffer_release(&keys);
	fake_destroy(&fake);
}

/*
 * The backend sees the calls in the order they were queued, all from one
 * thread that is not the caller's, never overlapping.
 */
static void
test_order(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	char text[512];
	struct wb_buffer out;
	int found;
	int error;

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Queues write A, write B, remove A, then reads of both. */
	put(queue, SPACE_ONE, "a1", "AAA");
	put(queue, SPACE_ONE, "b2", "BB");
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "a1"), 0);
	CHECK(expect_absent(queue, SPACE_ONE, "a1"));
	CHECK(expect_record(queue, SPACE_ONE, "b2", "BB"));

	/* A record in another space is separate, and a clear removes the whole space. */
	put(queue, SPACE_TWO, "b2", "two");
	put(queue, SPACE_TWO, "c3", "three");
	CHECK_EQ(wb_storage_queue_clear(queue, SPACE_TWO), 0);
	CHECK(expect_absent(queue, SPACE_TWO, "b2"));
	CHECK(expect_absent(queue, SPACE_TWO, "c3"));
	CHECK(expect_record(queue, SPACE_ONE, "b2", "BB"));

	/* A read appends after what the buffer holds. */
	wb_buffer_init(&out);
	wb_buffer_append_string(&out, "xy");
	found = 0;
	error = wb_storage_queue_read(queue, SPACE_ONE, "b2", &out, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(out.length, 4);
	CHECK(memcmp(out.data, "xyBB", 4) == 0);
	wb_buffer_release(&out);

	wb_storage_queue_close(queue);

	/* The backend saw exactly this sequence, then the close. */
	fake_log_text(&fake, text, sizeof(text));
	CHECK(strcmp(text,
	    "W1:a1:3;W1:b2:2;X1:a1:0;R1:a1:0;R1:b2:0;W2:b2:3;W2:c3:5;C2::0;R2:b2:0;R2:c3:0;R1:b2:0;R1:b2:0;Q0::0;") == 0);
	if (strcmp(text,
	    "W1:a1:3;W1:b2:2;X1:a1:0;R1:a1:0;R1:b2:0;W2:b2:3;W2:c3:5;C2::0;R2:b2:0;R2:c3:0;R1:b2:0;R1:b2:0;Q0::0;") != 0)
		printf("log: %s\n", text);

	/* One thread, not the caller's, never two calls at once, named for debuggers. */
	CHECK_EQ(fake.wrong_thread, 0);
	CHECK(fake.thread_known);
	CHECK(!pthread_equal(fake.thread, pthread_self()));
	CHECK_EQ(fake.max_inside, 1);
	CHECK_EQ(fake.name_wrong, 0);
	CHECK_EQ(fake.closes, 1);
	CHECK_EQ(fake.calls_after_close, 0);

	fake_destroy(&fake);
}

/*
 * A slow backend does not make the caller wait, and a later read sees the
 * writes.
 */
static void
test_write_behind(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	long long start;
	long long elapsed;
	char key[8];
	char value[16];
	unsigned i;

	fake_init(&fake);
	fake.write_delay_ms = 200;
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Ten writes of different keys return at once although each takes the backend 200 ms. */
	start = now_ms();
	for (i = 0; i < 10; i++) {
		key_text(key, i);
		snprintf(value, sizeof(value), "value-%u", i);
		CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, key, value, strlen(value)), 0);
	}
	elapsed = now_ms() - start;
	CHECK(elapsed < 50);

	/* A read waits for the writes before it and sees all of them. */
	for (i = 0; i < 10; i++) {
		key_text(key, i);
		snprintf(value, sizeof(value), "value-%u", i);
		CHECK(expect_record(queue, SPACE_ONE, key, value));
	}

	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/*
 * Waiting writes of one key may be replaced by the last, but never across
 * a remove, and a remove does not move after a later write.
 */
static void
test_merge(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	char text[512];

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Holds the storage thread inside a call so the next requests wait in the queue. */
	fake_set_gate(&fake, CALL_ALL);
	put(queue, SPACE_ONE, "5a", "hold");
	fake_wait_entered(&fake, 1);

	/* Three writes of one key wait; only the last bytes need to reach the backend. */
	put(queue, SPACE_ONE, "aa", "A");
	put(queue, SPACE_ONE, "aa", "BB");
	put(queue, SPACE_ONE, "aa", "CCC");

	/* A write of another key between them does not stop the replacement of the first key. */
	put(queue, SPACE_ONE, "bb", "1");
	put(queue, SPACE_ONE, "aa", "DDDD");

	/* write, remove, write of one key keeps that order. */
	put(queue, SPACE_ONE, "cc", "A");
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "cc"), 0);
	put(queue, SPACE_ONE, "cc", "B");

	/* A clear of the space also keeps the order of the writes around it. */
	put(queue, SPACE_TWO, "dd", "A");
	CHECK_EQ(wb_storage_queue_clear(queue, SPACE_TWO), 0);
	put(queue, SPACE_TWO, "dd", "B");

	/* A write in another space is not replaced by a write of the same key. */
	put(queue, SPACE_ONE, "ee", "one");
	put(queue, SPACE_TWO, "ee", "two");
	put(queue, SPACE_ONE, "ee", "uno");

	fake_set_gate(&fake, 0);
	wb_storage_queue_flush(queue);

	/* The last bytes reached the backend. */
	CHECK(fake_has(&fake, SPACE_ONE, "aa", "DDDD"));
	CHECK(fake_has(&fake, SPACE_ONE, "bb", "1"));
	CHECK(fake_has(&fake, SPACE_ONE, "cc", "B"));
	CHECK(fake_has(&fake, SPACE_TWO, "dd", "B"));
	CHECK(fake_has(&fake, SPACE_ONE, "ee", "uno"));
	CHECK(fake_has(&fake, SPACE_TWO, "ee", "two"));

	/* This queue replaces: the key aa was written once and ee in space one once. */
	CHECK_EQ(fake_event_count(&fake, 'W', "aa"), 1);
	CHECK_EQ(fake_event_count(&fake, 'W', "ee"), 2);

	/* The remove and the clear stayed between their writes. */
	fake_log_text(&fake, text, sizeof(text));
	CHECK(strstr(text, "W1:cc:1;X1:cc:0;W1:cc:1;") != NULL);
	CHECK(strstr(text, "W2:dd:1;C2::0;W2:dd:1;") != NULL);
	CHECK_EQ(fake.max_inside, 1);

	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/* What the helper thread of the read-barrier test does: one read, noting what it saw. */
struct reader_job {
	struct wb_storage_queue *queue;
	unsigned space;
	const char *key;
	char seen[16];
	int found;
	int error;
};

/*
 * The thread body that reads one record.
 */
static void *
reader_main(
	void *argument)
{
	struct reader_job *job;
	struct wb_buffer out;

	job = argument;
	wb_buffer_init(&out);
	job->found = 0;
	job->error = wb_storage_queue_read(job->queue, job->space, job->key, &out, &job->found);
	memset(job->seen, 0, sizeof(job->seen));
	if (out.length > 0 && out.length < sizeof(job->seen))
		memcpy(job->seen, out.data, out.length);
	wb_buffer_release(&out);

	return NULL;
}

/*
 * A read queued between two writes of a key sees the first, because the
 * second does not replace the first across it.  The read is queued by a
 * second thread, and the test cannot see when, so it tries a few times
 * and needs the first to be seen once; a queue that replaced across the
 * read would never show it.
 */
static void
test_merge_read_barrier(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	struct reader_job job;
	pthread_t reader;
	int attempt;
	int saw_first;
	int created;

	saw_first = 0;
	for (attempt = 0; attempt < 5 && !saw_first; attempt++) {
		fake_init(&fake);
		queue = open_queue(&fake);
		if (queue == NULL) {
			fake_destroy(&fake);
			return;
		}

		/* Holds the storage thread so the write, the read and the second write all wait. */
		fake_set_gate(&fake, CALL_ALL);
		put(queue, SPACE_ONE, "5a", "hold");
		fake_wait_entered(&fake, 1);
		put(queue, SPACE_ONE, "aa", "first");

		/* Another thread reads the key; give it time to queue behind the write. */
		memset(&job, 0, sizeof(job));
		job.queue = queue;
		job.space = SPACE_ONE;
		job.key = "aa";
		created = pthread_create(&reader, NULL, reader_main, &job);
		CHECK_EQ(created, 0);
		sleep_ms(250);

		/* Queues the second write, then lets everything run. */
		put(queue, SPACE_ONE, "aa", "second");
		fake_set_gate(&fake, 0);
		if (created == 0)
			pthread_join(reader, NULL);
		wb_storage_queue_flush(queue);

		/* The read found one of the two values, and the final record is the second. */
		CHECK_EQ(job.error, 0);
		CHECK_EQ(job.found, 1);
		CHECK(strcmp(job.seen, "first") == 0 || strcmp(job.seen, "second") == 0);
		CHECK(fake_has(&fake, SPACE_ONE, "aa", "second"));
		if (strcmp(job.seen, "first") == 0)
			saw_first = 1;

		wb_storage_queue_close(queue);
		fake_destroy(&fake);
	}
	CHECK(saw_first);
}

/*
 * The limit on queued bytes and the limit on one record.
 */
static void
test_limits(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	unsigned char *big;
	long long start;
	long long elapsed;
	int error;

	big = malloc(WB_STORAGE_QUEUE_LIMIT + 1UL);
	CHECK(big != NULL);
	if (big == NULL)
		return;
	memset(big, 'x', WB_STORAGE_QUEUE_LIMIT + 1UL);

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		free(big);
		fake_destroy(&fake);
		return;
	}

	/* Holds the storage thread in a write of no bytes, so what follows waits and counts. */
	fake_set_gate(&fake, CALL_ALL);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "5a", NULL, 0), 0);
	fake_wait_entered(&fake, 1);

	/* A record of exactly the limit fits in an empty queue. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "0a", big, WB_STORAGE_QUEUE_LIMIT), 0);

	/* One more byte of another key does not fit, and the refusal does not wait. */
	start = now_ms();
	error = wb_storage_queue_write(queue, SPACE_ONE, "0b", "x", 1);
	elapsed = now_ms() - start;
	CHECK_EQ(error, ENOSPC);
	CHECK(elapsed < 50);

	/* A rewrite of the full key replaces its bytes, so the same size fits again. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "0a", big, WB_STORAGE_QUEUE_LIMIT), 0);

	/* A larger replacement is refused and the earlier bytes stay queued. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "0a", big, WB_STORAGE_QUEUE_LIMIT + 1UL), EFBIG);

	/* Removes, clears and empty writes carry no bytes and still fit. */
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "0b"), 0);
	CHECK_EQ(wb_storage_queue_clear(queue, SPACE_TWO), 0);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "0c", NULL, 0), 0);

	/* After the backend has done them all, the room is back. */
	fake_set_gate(&fake, 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);
	CHECK_EQ(fake_event_count(&fake, 'W', "0a"), 1);
	{
		struct fake_record *record;

		pthread_mutex_lock(&fake.lock);
		record = fake_find(&fake, SPACE_ONE, "0a");
		CHECK(record != NULL);
		if (record != NULL) {
			CHECK_EQ(record->length, WB_STORAGE_QUEUE_LIMIT);
			CHECK(memcmp(record->bytes, big, 64) == 0);
		}
		pthread_mutex_unlock(&fake.lock);
	}
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "0b", "x", 1), 0);
	CHECK(expect_record(queue, SPACE_ONE, "0b", "x"));

	wb_storage_queue_close(queue);
	free(big);
	fake_destroy(&fake);
}

/*
 * The limit on how many writes, removes and clears wait.
 */
static void
test_item_limit(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	char key[16];
	unsigned accepted;
	unsigned i;
	int error;

	fake_init(&fake);
	fake.record_events = 0;
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Holds the storage thread in one remove; it counts as waiting until it is done. */
	fake_set_gate(&fake, CALL_ALL);
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "5a"), 0);
	fake_wait_entered(&fake, 1);

	/* Queues removes of different keys until one is refused. */
	accepted = 0;
	error = 0;
	for (i = 0; i < 70000 && error == 0; i++) {
		key_text(key, i);
		error = wb_storage_queue_remove(queue, SPACE_ONE, key);
		if (error == 0)
			accepted++;
	}
	CHECK_EQ(error, ENOSPC);
	CHECK_EQ(accepted, 65535);

	/* Every kind is refused while the queue is full. */
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "fffff", "x", 1), ENOSPC);
	CHECK_EQ(wb_storage_queue_clear(queue, SPACE_ONE), ENOSPC);
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "fffff"), ENOSPC);

	/* Once everything is done the queue takes requests again. */
	fake_set_gate(&fake, 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "fffff", "x", 1), 0);
	CHECK(expect_record(queue, SPACE_ONE, "fffff", "x"));
	CHECK_EQ(fake.total_calls, 65536 + 2);

	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/*
 * Errors of writes, removes and clears are kept, first one first, and
 * reported once.
 */
static void
test_errors(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Nothing failed yet. */
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);

	/* A failing write is queued without error and its error is reported once. */
	fake_fail(&fake, CALL_WRITE, EIO, 0);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", "x", 1), 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), EIO);
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);
	CHECK(!fake_has(&fake, SPACE_ONE, "a1", "x"));

	/* Of two failures the first is kept. */
	fake_fail(&fake, CALL_WRITE, ENOSPC, EIO);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a1", "x", 1), 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a2", "y", 1), 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), ENOSPC);
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);

	/* A failing remove and a failing clear are kept too, and an unusual value is passed on as it is. */
	fake_fail(&fake, CALL_REMOVE, 4242, 0);
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "a1"), 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), 4242);
	fake_fail(&fake, CALL_CLEAR, EIO, 0);
	CHECK_EQ(wb_storage_queue_clear(queue, SPACE_ONE), 0);
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), EIO);
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);

	/* Closing with an error nobody took is clean. */
	fake_fail(&fake, CALL_WRITE, EIO, 0);
	CHECK_EQ(wb_storage_queue_write(queue, SPACE_ONE, "a3", "z", 1), 0);
	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/*
 * Errors of reads belong to the reader and leave its buffer as it was.
 */
static void
test_read_errors(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	struct wb_buffer out;
	int found;
	int error;

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}
	put(queue, SPACE_ONE, "a1", "abcdef");
	wb_storage_queue_flush(queue);

	/* The backend fails after appending a byte: the error comes back, found is 0, the buffer is as it was. */
	wb_buffer_init(&out);
	wb_buffer_append_string(&out, "pre");
	fake_fail(&fake, CALL_READ, EIO, 0);
	found = 1;
	error = wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, &found);
	CHECK_EQ(error, EIO);
	CHECK_EQ(found, 0);
	CHECK_EQ(out.length, 3);
	CHECK(memcmp(out.data, "pre", 4) == 0);

	/* Another error value is passed on as it is. */
	fake_fail(&fake, CALL_READ, ENOSPC, 0);
	error = wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, &found);
	CHECK_EQ(error, ENOSPC);
	CHECK_EQ(found, 0);

	/* A read error is the reader's: take_error does not report it. */
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);

	/* After the failures the read works. */
	error = wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, &found);
	CHECK_EQ(error, 0);
	CHECK_EQ(found, 1);
	CHECK_EQ(out.length, 9);
	CHECK(memcmp(out.data, "preabcdef", 9) == 0);

	/* A record larger than the engine's largest fails with EFBIG and the buffer is put back. */
	fake.big_read = 1;
	error = wb_storage_queue_read(queue, SPACE_ONE, "a1", &out, &found);
	CHECK_EQ(error, EFBIG);
	CHECK_EQ(found, 0);
	CHECK_EQ(out.length, 9);
	CHECK(memcmp(out.data, "preabcdef", 10) == 0);
	fake.big_read = 0;

	wb_buffer_release(&out);
	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/*
 * Lists: invalid keys from the backend are skipped, the keys come as
 * "key\0key\0", and an error leaves the buffer and the count alone.
 */
static void
test_list(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	struct wb_buffer keys;
	size_t count;
	int error;

	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* An empty space lists nothing and leaves the buffer as it was. */
	wb_buffer_init(&keys);
	count = 99;
	error = wb_storage_queue_list(queue, SPACE_ONE, &keys, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 0);
	CHECK_EQ(keys.length, 0);

	/* Three keys in one space and one in another. */
	put(queue, SPACE_ONE, "a1", "1");
	put(queue, SPACE_ONE, "b-2", "2");
	put(queue, SPACE_ONE, "0", "3");
	put(queue, SPACE_TWO, "cc", "4");

	/* The backend also hands out keys the engine never makes; they are skipped. */
	fake.list_extras = 1;
	wb_buffer_append_string(&keys, "kept");
	count = 99;
	error = wb_storage_queue_list(queue, SPACE_ONE, &keys, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 3);
	CHECK_EQ(keys.length, 4 + 3 + 4 + 2);
	CHECK(memcmp(keys.data, "kept" "a1\0" "b-2\0" "0\0", keys.length) == 0);

	/* A backend error after one key: the error is returned, buffer and count are as before. */
	fake_fail(&fake, CALL_LIST, EIO, 0);
	count = 99;
	error = wb_storage_queue_list(queue, SPACE_ONE, &keys, &count);
	CHECK_EQ(error, EIO);
	CHECK_EQ(count, 99);
	CHECK_EQ(keys.length, 13);
	CHECK(memcmp(keys.data, "kept" "a1\0" "b-2\0" "0\0", 13) == 0);

	/* The other space lists its own key. */
	fake.list_extras = 0;
	wb_buffer_release(&keys);
	wb_buffer_init(&keys);
	error = wb_storage_queue_list(queue, SPACE_TWO, &keys, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 1);
	CHECK_EQ(keys.length, 3);
	CHECK(memcmp(keys.data, "cc\0", 3) == 0);

	/* A list sees the writes queued before it. */
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_TWO, "cc"), 0);
	put(queue, SPACE_TWO, "dd", "5");
	wb_buffer_clear(&keys);
	error = wb_storage_queue_list(queue, SPACE_TWO, &keys, &count);
	CHECK_EQ(error, 0);
	CHECK_EQ(count, 1);
	CHECK(memcmp(keys.data, "dd\0", 3) == 0);

	wb_buffer_release(&keys);
	wb_storage_queue_close(queue);
	fake_destroy(&fake);
}

/* What the helper thread of the close test does: lets the gate go after a while. */
struct opener_job {
	struct fake *fake;
	unsigned delay_ms;
};

/*
 * The thread body that opens the fake's gate after a delay.
 */
static void *
opener_main(
	void *argument)
{
	struct opener_job *job;

	job = argument;
	sleep_ms(job->delay_ms);
	fake_set_gate(job->fake, 0);

	return NULL;
}

/*
 * Flush waits for everything queued; close does everything queued, stops
 * the thread, and closes the backend once, last.
 */
static void
test_flush_close(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	struct opener_job job;
	pthread_t opener;
	char key[8];
	char text[512];
	unsigned i;
	int created;

	/* Flush returns only when ten slow writes are all done. */
	fake_init(&fake);
	fake.write_delay_ms = 20;
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}
	for (i = 0; i < 10; i++) {
		key_text(key, i);
		put(queue, SPACE_ONE, key, "v");
	}
	wb_storage_queue_flush(queue);
	for (i = 0; i < 10; i++) {
		key_text(key, i);
		CHECK(fake_has(&fake, SPACE_ONE, key, "v"));
	}

	/* A flush of an idle queue returns at once, also twice in a row. */
	wb_storage_queue_flush(queue);
	wb_storage_queue_flush(queue);
	wb_storage_queue_close(queue);
	fake_destroy(&fake);

	/* Close with requests still waiting does them all before it closes the backend. */
	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}
	fake_set_gate(&fake, CALL_ALL);
	put(queue, SPACE_ONE, "5a", "hold");
	fake_wait_entered(&fake, 1);
	for (i = 0; i < 5; i++) {
		key_text(key, 0x10 + i);
		put(queue, SPACE_ONE, key, "queued");
	}
	CHECK_EQ(wb_storage_queue_remove(queue, SPACE_ONE, "5a"), 0);
	CHECK_EQ(fake.closes, 0);

	job.fake = &fake;
	job.delay_ms = 150;
	created = pthread_create(&opener, NULL, opener_main, &job);
	CHECK_EQ(created, 0);
	if (created != 0)
		fake_set_gate(&fake, 0);
	wb_storage_queue_close(queue);
	if (created == 0)
		pthread_join(opener, NULL);

	for (i = 0; i < 5; i++) {
		key_text(key, 0x10 + i);
		CHECK(fake_has(&fake, SPACE_ONE, key, "queued"));
	}
	CHECK(!fake_has(&fake, SPACE_ONE, "5a", "hold"));
	CHECK_EQ(fake.closes, 1);
	CHECK_EQ(fake.calls_after_close, 0);
	fake_log_text(&fake, text, sizeof(text));
	CHECK(strlen(text) > 3 && strcmp(text + strlen(text) - 6, "Q0::0;") == 0);
	CHECK_EQ(fake.max_inside, 1);
	fake_destroy(&fake);

	/* A queue that was opened and closed with nothing queued closes the backend once. */
	fake_init(&fake);
	queue = open_queue(&fake);
	if (queue != NULL)
		wb_storage_queue_close(queue);
	CHECK_EQ(fake.closes, 1);
	CHECK_EQ(fake.total_calls, 0);
	fake_destroy(&fake);
}

/* What each thread of the stress test is given. */
struct stress_job {
	struct wb_storage_queue *queue;
	unsigned number;
};

/*
 * The thread body of the stress test: writes, reads, removes and lists,
 * checking that its own writes are seen by its own reads.
 */
static void *
stress_main(
	void *argument)
{
	struct stress_job *job;
	struct wb_buffer out;
	struct wb_buffer keys;
	char own[8];
	char value[32];
	size_t count;
	size_t length;
	unsigned pass;
	int found;
	int error;

	job = argument;
	found = 0;
	count = 0;
	key_text(own, job->number);
	wb_buffer_init(&out);
	wb_buffer_init(&keys);

	/* Mixes the operations; the key "ff" is shared by all threads, the other keys are one thread's. */
	for (pass = 0; pass < STRESS_PASSES; pass++) {
		snprintf(value, sizeof(value), "t%u-p%u", job->number, pass);
		length = strlen(value);

		switch (pass % 5) {
		case 0:
			CHECK_EQ(wb_storage_queue_write(job->queue, SPACE_ONE, own, value, length), 0);
			wb_buffer_clear(&out);
			error = wb_storage_queue_read(job->queue, SPACE_ONE, own, &out, &found);
			CHECK_EQ(error, 0);
			CHECK_EQ(found, 1);
			CHECK_EQ(out.length, length);
			if (out.length == length)
				CHECK(memcmp(out.data, value, length) == 0);
			break;
		case 1:
			CHECK_EQ(wb_storage_queue_write(job->queue, SPACE_ONE, own, value, length), 0);
			CHECK_EQ(wb_storage_queue_remove(job->queue, SPACE_ONE, own), 0);
			wb_buffer_clear(&out);
			error = wb_storage_queue_read(job->queue, SPACE_ONE, own, &out, &found);
			CHECK_EQ(error, 0);
			CHECK_EQ(found, 0);
			CHECK_EQ(out.length, 0);
			break;
		case 2:
			CHECK_EQ(wb_storage_queue_write(job->queue, SPACE_TWO, "ff", value, length), 0);
			CHECK_EQ(wb_storage_queue_write(job->queue, SPACE_TWO, "ff", value, length), 0);
			break;
		case 3:
			wb_buffer_clear(&out);
			error = wb_storage_queue_read(job->queue, SPACE_TWO, "ff", &out, &found);
			CHECK_EQ(error, 0);
			if (found != 0 && out.length > 0)
				CHECK(out.data[0] == 't');
			break;
		default:
			wb_buffer_clear(&keys);
			error = wb_storage_queue_list(job->queue, SPACE_TWO, &keys, &count);
			CHECK_EQ(error, 0);
			CHECK(count <= 1);
			break;
		}

		/* Now and then waits for the queue and collects errors. */
		if (pass % 97 == 0) {
			wb_storage_queue_flush(job->queue);
			CHECK_EQ(wb_storage_queue_take_error(job->queue), 0);
		}
	}

	wb_buffer_release(&out);
	wb_buffer_release(&keys);

	return NULL;
}

/*
 * Many threads use one queue at once.
 */
static void
test_stress(void)
{
	struct wb_storage_queue *queue;
	static struct fake fake;
	struct stress_job jobs[STRESS_THREADS];
	pthread_t threads[STRESS_THREADS];
	int created[STRESS_THREADS];
	unsigned i;

	fake_init(&fake);
	fake.record_events = 0;
	queue = open_queue(&fake);
	if (queue == NULL) {
		fake_destroy(&fake);
		return;
	}

	/* Starts the threads. */
	for (i = 0; i < STRESS_THREADS; i++) {
		jobs[i].queue = queue;
		jobs[i].number = i;
		created[i] = pthread_create(&threads[i], NULL, stress_main, &jobs[i]);
		CHECK_EQ(created[i], 0);
	}

	/* Waits for them. */
	for (i = 0; i < STRESS_THREADS; i++) {
		if (created[i] == 0)
			pthread_join(threads[i], NULL);
	}

	/* Everything the threads queued is done once the queue is flushed and closed. */
	wb_storage_queue_flush(queue);
	CHECK_EQ(wb_storage_queue_take_error(queue), 0);
	wb_storage_queue_close(queue);

	/* The backend was called by one thread, one call at a time, and closed once at the end. */
	CHECK_EQ(fake.wrong_thread, 0);
	CHECK_EQ(fake.max_inside, 1);
	CHECK_EQ(fake.closes, 1);
	CHECK_EQ(fake.calls_after_close, 0);
	CHECK(fake.total_calls > STRESS_THREADS * STRESS_PASSES / 2);

	fake_destroy(&fake);
}

/*
 * Prepares a fake with an empty store, logging every call.
 */
static void
fake_init(
	struct fake *fake)
{
	memset(fake, 0, sizeof(*fake));
	pthread_mutex_init(&fake->lock, NULL);
	pthread_cond_init(&fake->changed, NULL);
	fake->record_events = 1;
}

/*
 * Frees what the fake holds.
 */
static void
fake_destroy(
	struct fake *fake)
{
	int i;

	for (i = 0; i < FAKE_RECORDS; i++)
		free(fake->records[i].bytes);
	pthread_cond_destroy(&fake->changed);
	pthread_mutex_destroy(&fake->lock);
}

/*
 * Sets which kinds of calls wait at the gate (0 lets all go and wakes the waiting ones).
 */
static void
fake_set_gate(
	struct fake *fake,
	int mask)
{
	pthread_mutex_lock(&fake->lock);

	fake->gate_mask = mask;
	pthread_cond_broadcast(&fake->changed);

	pthread_mutex_unlock(&fake->lock);
}

/*
 * Waits until the given number of calls have reached the gate; fails after ten seconds.
 */
static void
fake_wait_entered(
	struct fake *fake,
	int count)
{
	int reached;
	int tries;

	reached = 0;
	for (tries = 0; tries < 10000 && !reached; tries++) {
		pthread_mutex_lock(&fake->lock);
		if (fake->entered >= count)
			reached = 1;
		pthread_mutex_unlock(&fake->lock);
		if (!reached)
			sleep_ms(1);
	}
	CHECK(reached);
}

/*
 * Makes the next calls of the given kinds fail with the given errors in turn (second 0: only one failure).
 */
static void
fake_fail(
	struct fake *fake,
	int mask,
	int first,
	int second)
{
	pthread_mutex_lock(&fake->lock);

	fake->fail_mask = mask;
	fake->fail_errors[0] = first;
	fake->fail_errors[1] = second;
	fake->fail_total = 1;
	if (second != 0)
		fake->fail_total = 2;
	fake->fail_next = 0;

	pthread_mutex_unlock(&fake->lock);
}

/*
 * What every fake call does first: checks the one-thread and one-at-a-time promises, logs the call, waits at the gate, and
 * says whether the call must fail.  The matching fake_leave ends the call.
 */
static int
fake_enter(
	struct fake *fake,
	char kind,
	unsigned space,
	const char *key,
	size_t length)
{
	struct fake_event *event;
	int call_bit;
	int error;
#ifndef _WIN32
	char name[32];
#endif

	call_bit = 0;
	if (kind == 'R')
		call_bit = CALL_READ;
	if (kind == 'W')
		call_bit = CALL_WRITE;
	if (kind == 'X')
		call_bit = CALL_REMOVE;
	if (kind == 'L')
		call_bit = CALL_LIST;
	if (kind == 'C')
		call_bit = CALL_CLEAR;

	pthread_mutex_lock(&fake->lock);

	/* Counts the call, and notes one made after the close. */
	fake->total_calls++;
	if (fake->closed)
		fake->calls_after_close++;

	/* Every call must come from the same thread and not overlap another. */
	if (!fake->thread_known) {
		fake->thread = pthread_self();
		fake->thread_known = 1;
	} else if (!pthread_equal(fake->thread, pthread_self())) {
		fake->wrong_thread = 1;
	}
	fake->inside++;
	if (fake->inside > fake->max_inside)
		fake->max_inside = fake->inside;

#ifndef _WIN32
	/* The storage thread is named for debuggers. */
	if (!fake->name_checked) {
		fake->name_checked = 1;
		name[0] = 0;
		if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 || strcmp(name, WB_STORAGE_QUEUE_THREAD) != 0)
			fake->name_wrong = 1;
	}
#endif

	/* Logs the call. */
	if (fake->record_events && fake->event_count < FAKE_EVENTS) {
		event = &fake->events[fake->event_count];
		event->kind = kind;
		event->space = space;
		snprintf(event->key, sizeof(event->key), "%s", key);
		event->length = length;
		fake->event_count++;
	}

	/* Waits at the gate while it is closed for this kind of call. */
	if ((fake->gate_mask & call_bit) != 0) {
		fake->entered++;
		pthread_cond_broadcast(&fake->changed);
		while ((fake->gate_mask & call_bit) != 0)
			pthread_cond_wait(&fake->changed, &fake->lock);
	}

	/* Decides whether this call fails. */
	error = 0;
	if ((fake->fail_mask & call_bit) != 0 && fake->fail_next < fake->fail_total) {
		error = fake->fail_errors[fake->fail_next];
		fake->fail_next++;
	}

	pthread_mutex_unlock(&fake->lock);

	/* A slow disk makes writes take time. */
	if (kind == 'W' && fake->write_delay_ms != 0)
		sleep_ms(fake->write_delay_ms);

	return error;
}

/*
 * What every fake call does last.
 */
static void
fake_leave(
	struct fake *fake)
{
	pthread_mutex_lock(&fake->lock);

	fake->inside--;

	pthread_mutex_unlock(&fake->lock);
}

/*
 * Finds a record in the fake's store; the fake's lock is held.
 */
static struct fake_record *
fake_find(
	struct fake *fake,
	unsigned space,
	const char *key)
{
	int i;

	for (i = 0; i < FAKE_RECORDS; i++) {
		if (fake->records[i].used && fake->records[i].space == space && strcmp(fake->records[i].key, key) == 0)
			return &fake->records[i];
	}

	return NULL;
}

/*
 * The fake's read.
 */
static int
fake_read(
	void *context,
	unsigned space,
	const char *key,
	struct browser_storage_sink *sink,
	int *found)
{
	struct fake *fake;
	struct fake_record *record;
	int error;
	int sink_error;
	size_t total;

	fake = context;
	*found = 0;
	error = fake_enter(fake, 'R', space, key, 0);

	/* Appends a megabyte at a time beyond the largest record. */
	if (fake->big_read) {
		sink_error = 0;
		for (total = 0; total < WB_STORAGE_RECORD_MAX + 4UL * sizeof(zero_megabyte) && sink_error == 0; total += sizeof(zero_megabyte))
			sink_error = browser_storage_sink_append(sink, zero_megabyte, sizeof(zero_megabyte));
		*found = 1;
		fake_leave(fake);
		return sink_error;
	}

	pthread_mutex_lock(&fake->lock);
	record = fake_find(fake, space, key);
	if (record != NULL) {
		/* A failing read gives one byte first, which the queue must take back. */
		if (error != 0 && record->length > 0) {
			browser_storage_sink_append(sink, record->bytes, 1);
		} else if (error == 0) {
			*found = 1;
			if (record->length > 2) {
				browser_storage_sink_append(sink, record->bytes, 1);
				browser_storage_sink_append(sink, record->bytes + 1, record->length - 1);
			} else if (record->length > 0) {
				browser_storage_sink_append(sink, record->bytes, record->length);
			}
		}
	}
	pthread_mutex_unlock(&fake->lock);

	fake_leave(fake);
	if (error != 0)
		*found = 1;
	return error;
}

/*
 * The fake's write.
 */
static int
fake_write(
	void *context,
	unsigned space,
	const char *key,
	const void *bytes,
	size_t length)
{
	struct fake *fake;
	struct fake_record *record;
	unsigned char *copy;
	int error;
	int i;

	fake = context;
	error = fake_enter(fake, 'W', space, key, length);
	if (error != 0) {
		fake_leave(fake);
		return error;
	}

	/* Copies the bytes; the copy of a record of no bytes is one byte so it is not NULL. */
	copy = malloc(length + 1U);
	if (copy != NULL && length > 0)
		memcpy(copy, bytes, length);

	pthread_mutex_lock(&fake->lock);
	record = fake_find(fake, space, key);
	if (record == NULL) {
		for (i = 0; i < FAKE_RECORDS && record == NULL; i++) {
			if (!fake->records[i].used)
				record = &fake->records[i];
		}
	}
	if (record != NULL && copy != NULL) {
		free(record->bytes);
		record->used = 1;
		record->space = space;
		snprintf(record->key, sizeof(record->key), "%s", key);
		record->bytes = copy;
		record->length = length;
	} else {
		free(copy);
		error = ENOSPC;
	}
	pthread_mutex_unlock(&fake->lock);

	fake_leave(fake);
	return error;
}

/*
 * The fake's remove.
 */
static int
fake_remove(
	void *context,
	unsigned space,
	const char *key)
{
	struct fake *fake;
	struct fake_record *record;
	int error;

	fake = context;
	error = fake_enter(fake, 'X', space, key, 0);
	if (error != 0) {
		fake_leave(fake);
		return error;
	}

	pthread_mutex_lock(&fake->lock);
	record = fake_find(fake, space, key);
	if (record != NULL) {
		free(record->bytes);
		memset(record, 0, sizeof(*record));
	}
	pthread_mutex_unlock(&fake->lock);

	fake_leave(fake);
	return 0;
}

/*
 * The fake's list: the keys of the space in the fake's order, with keys that are not valid around them when asked.
 */
static int
fake_list(
	void *context,
	unsigned space,
	int (*each)(void *each_context, const char *key),
	void *each_context)
{
	struct fake *fake;
	char long_key[FAKE_KEY_SIZE];
	char keys[FAKE_RECORDS][FAKE_KEY_SIZE];
	int count;
	int i;
	int error;
	int result;

	fake = context;
	error = fake_enter(fake, 'L', space, "", 0);

	/* Copies the keys out, because each may take time and the lock is not held across it. */
	count = 0;
	pthread_mutex_lock(&fake->lock);
	for (i = 0; i < FAKE_RECORDS; i++) {
		if (fake->records[i].used && fake->records[i].space == space) {
			memcpy(keys[count], fake->records[i].key, FAKE_KEY_SIZE);
			count++;
		}
	}
	pthread_mutex_unlock(&fake->lock);

	memset(long_key, 'a', 201);
	long_key[201] = 0;
	result = 0;

	/* Keys the engine never makes come first. */
	if (fake->list_extras) {
		each(each_context, "");
		each(each_context, "ZZ");
		each(each_context, NULL);
	}

	for (i = 0; i < count && result == 0; i++) {
		result = each(each_context, keys[i]);

		/* A failing list stops after its first key. */
		if (error != 0) {
			fake_leave(fake);
			return error;
		}

		/* More invalid keys in the middle of the list. */
		if (fake->list_extras && i == 0) {
			each(each_context, "a b");
			each(each_context, long_key);
		}
	}

	if (fake->list_extras)
		each(each_context, "G-0");

	fake_leave(fake);
	if (error != 0)
		return error;
	return result;
}

/*
 * The fake's clear.
 */
static int
fake_clear(
	void *context,
	unsigned space)
{
	struct fake *fake;
	int error;
	int i;

	fake = context;
	error = fake_enter(fake, 'C', space, "", 0);
	if (error != 0) {
		fake_leave(fake);
		return error;
	}

	pthread_mutex_lock(&fake->lock);
	for (i = 0; i < FAKE_RECORDS; i++) {
		if (fake->records[i].used && fake->records[i].space == space) {
			free(fake->records[i].bytes);
			memset(&fake->records[i], 0, sizeof(fake->records[i]));
		}
	}
	pthread_mutex_unlock(&fake->lock);

	fake_leave(fake);
	return 0;
}

/*
 * The fake's close: counts, and notes a call still inside.
 */
static void
fake_close(
	void *context)
{
	struct fake *fake;
	struct fake_event *event;

	fake = context;

	pthread_mutex_lock(&fake->lock);

	fake->closes++;
	if (fake->inside != 0)
		fake->wrong_thread = 1;
	if (fake->event_count < FAKE_EVENTS && fake->record_events) {
		event = &fake->events[fake->event_count];
		event->kind = 'Q';
		event->space = 0;
		event->key[0] = 0;
		event->length = 0;
		fake->event_count++;
	}
	fake->closed = 1;

	pthread_mutex_unlock(&fake->lock);
}

/*
 * Writes the log as text: kind, space, ':', key, ':', length and ';' for each call.
 */
static void
fake_log_text(
	struct fake *fake,
	char *text,
	size_t size)
{
	size_t i;
	size_t used;
	int wrote;

	text[0] = 0;
	used = 0;

	pthread_mutex_lock(&fake->lock);
	for (i = 0; i < fake->event_count; i++) {
		wrote = snprintf(text + used, size - used, "%c%u:%s:%u;",
		    fake->events[i].kind, fake->events[i].space, fake->events[i].key, (unsigned)fake->events[i].length);
		if (wrote < 0 || (size_t)wrote >= size - used)
			break;
		used += (size_t)wrote;
	}
	pthread_mutex_unlock(&fake->lock);
}

/*
 * Counts the logged calls of a kind on a key (any space).
 */
static size_t
fake_event_count(
	struct fake *fake,
	char kind,
	const char *key)
{
	size_t i;
	size_t count;

	count = 0;

	pthread_mutex_lock(&fake->lock);
	for (i = 0; i < fake->event_count; i++) {
		if (fake->events[i].kind == kind && strcmp(fake->events[i].key, key) == 0)
			count++;
	}
	pthread_mutex_unlock(&fake->lock);

	return count;
}

/*
 * Reports whether the fake's store holds a record with exactly these bytes.
 */
static int
fake_has(
	struct fake *fake,
	unsigned space,
	const char *key,
	const char *bytes)
{
	struct fake_record *record;
	int has;

	has = 0;

	pthread_mutex_lock(&fake->lock);
	record = fake_find(fake, space, key);
	if (record != NULL && record->length == strlen(bytes) && memcmp(record->bytes, bytes, record->length) == 0)
		has = 1;
	pthread_mutex_unlock(&fake->lock);

	return has;
}

/*
 * Opens a queue over the fake and checks that it opened.
 */
static struct wb_storage_queue *
open_queue(
	struct fake *fake)
{
	struct wb_storage_queue *queue;
	int error;

	queue = NULL;
	error = wb_storage_queue_open(&fake_calls, fake, &queue);
	CHECK_EQ(error, 0);
	CHECK(queue != NULL);

	return queue;
}

/*
 * Queues a write of a text's bytes and checks that it was accepted.
 */
static void
put(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	const char *text)
{
	int error;

	error = wb_storage_queue_write(queue, space, key, text, strlen(text));
	CHECK_EQ(error, 0);
}

/*
 * Reads a record and reports whether it was found with exactly the text's bytes.
 */
static int
expect_record(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key,
	const char *text)
{
	struct wb_buffer out;
	int found;
	int error;
	int same;

	wb_buffer_init(&out);
	found = 0;
	error = wb_storage_queue_read(queue, space, key, &out, &found);
	same = 0;
	if (error == 0 && found == 1 && out.length == strlen(text) && (out.length == 0 || memcmp(out.data, text, out.length) == 0))
		same = 1;
	wb_buffer_release(&out);

	return same;
}

/*
 * Reads a record and reports whether it was not found (and no bytes came).
 */
static int
expect_absent(
	struct wb_storage_queue *queue,
	unsigned space,
	const char *key)
{
	struct wb_buffer out;
	int found;
	int error;
	int absent;

	wb_buffer_init(&out);
	found = 1;
	error = wb_storage_queue_read(queue, space, key, &out, &found);
	absent = 0;
	if (error == 0 && found == 0 && out.length == 0)
		absent = 1;
	wb_buffer_release(&out);

	return absent;
}

/*
 * The time in milliseconds from some starting point, never going back.
 */
static long long
now_ms(void)
{
#ifdef _WIN32
	return (long long)GetTickCount64();
#else
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000L);
#endif
}

/*
 * Sleeps for at least the given milliseconds.
 */
static void
sleep_ms(
	unsigned ms)
{
#ifdef _WIN32
	Sleep(ms);
#else
	usleep(ms * 1000U);
#endif
}

/*
 * Makes the hexadecimal key text of a number (a valid key).
 */
static void
key_text(
	char *text,
	unsigned number)
{
	snprintf(text, 8, "%x", number);
}
