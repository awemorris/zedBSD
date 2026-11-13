/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the client buffers' returns (objects.c's kwl_buffer_put,
 * kwl_buffers_returned and kwl_buffers_client_gone): a buffer replaced while
 * a drawing given still reads it keeps its last hold and is released only
 * once that drawing has completed, in the order they waited; one no
 * drawing reads is released at once; one attached again while it waits is
 * not released by the drawing's completion; a destroyed one is freed only
 * after the drawing; a client that goes takes its waiting buffers out at
 * once, and the others keep waiting in order.  A release shows as the
 * buffer's busy mark going to 0 (kwl_emit is a stand-in).
 */

#include "kwl.h"
#include "compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The number of failed checks. */
static unsigned test_failures;

static void check(int condition, const char *what);
static struct kwl_object *buffer_make(struct kwl_client *client, uint32_t id, uint64_t read_drawing);
static unsigned in_client(const struct kwl_client *client, const struct kwl_object *object);
static unsigned waiting(const struct kwl_compose *compose);

/* Arms no timer: the gone clients' pace is not part of this test. */
void
kwl_timer_set(
	struct kwl_server *server,
	struct kwl_timer *timer,
	uint64_t at_ms)
{
	/* Nothing. */
	(void)server;
	(void)timer;
	(void)at_ms;
}

/* Runs the checks with a server, a compose and two clients of its own, and says PASS or how many failed. */
int
main(
	void)
{
	struct kwl_server *server;
	struct kwl_compose *compose;
	struct kwl_client *first;
	struct kwl_client *second;
	struct kwl_object *a;
	struct kwl_object *b;
	struct kwl_object *c;
	struct kwl_object *d;
	struct kwl_object *e;

	/* The server with a compose (three drawings given, none completed) and two clients. */
	server = calloc(1U, sizeof(*server));
	compose = calloc(1U, sizeof(*compose));
	first = calloc(1U, sizeof(*first));
	second = calloc(1U, sizeof(*second));
	if (server == NULL || compose == NULL || first == NULL || second == NULL) {
		printf("returns-test: FAIL (memory)\n");
		return 1;
	}

	/* Their links and counts. */
	server->compose = compose;
	compose->drawings_given = 3U;
	compose->drawings_done = 0U;
	first->server = server;
	first->number = 1U;
	second->server = server;
	second->number = 2U;

	/* A buffer no drawing reads: released at once. */
	a = buffer_make(first, 10U, 0U);
	kwl_buffer_put(a);
	check(a->holds == 0U && a->busy == 0U && !a->returning, "unread: released at once");

	/* Two read by drawings 2 and 1, replaced: both wait with their last hold, nothing released. */
	b = buffer_make(first, 11U, 2U);
	c = buffer_make(second, 12U, 1U);
	kwl_buffer_put(b);
	kwl_buffer_put(c);
	check(b->holds == 1U && b->busy == 1U && b->returning, "read: b waits");
	check(c->holds == 1U && c->busy == 1U && c->returning, "read: c waits");
	check(waiting(compose) == 2U && compose->returns == b && compose->returns_tail == c, "read: in the order they waited");

	/* Drawing 1 completes: c (read by it) is released, b (read by 2) still waits. */
	compose->drawings_done = 1U;
	kwl_buffers_returned(server);
	check(c->holds == 0U && c->busy == 0U && !c->returning, "done 1: c released");
	check(b->holds == 1U && b->busy == 1U && b->returning, "done 1: b still waits");
	check(waiting(compose) == 1U && compose->returns_tail == b, "done 1: one waits, the list's end kept");

	/* b attached and committed again while it waits: drawing 2's completion lets go of the wait's hold only. */
	kwl_buffer_get(b);
	b->busy = 1U;
	compose->drawings_done = 2U;
	kwl_buffers_returned(server);
	check(b->holds == 1U && b->busy == 1U && !b->returning, "again: not released while it is current");
	check(waiting(compose) == 0U && compose->returns_tail == NULL, "again: the list is empty");

	/* Replaced once more after drawing 2: released at once. */
	kwl_buffer_put(b);
	check(b->holds == 0U && b->busy == 0U, "again: released when replaced");

	/* A destroyed buffer read by drawing 3: kept (in its client's list) until drawing 3 completes, then freed. */
	d = buffer_make(first, 13U, 3U);
	kwl_buffer_put(d);
	check(d->returning, "destroyed: waits");
	kwl_object_destroy(d);
	check(in_client(first, d), "destroyed: not freed while drawing 3 may read it");
	compose->drawings_done = 3U;
	kwl_buffers_returned(server);
	check(!in_client(first, d), "destroyed: freed once drawing 3 completed");

	/* A client that goes: its waiting buffer is put at once (untold), the other's keeps waiting. */
	compose->drawings_given = 5U;
	e = buffer_make(second, 14U, 5U);
	kwl_buffer_put(e);
	c = buffer_make(first, 15U, 4U);
	kwl_buffer_put(c);
	check(waiting(compose) == 2U, "gone: two wait");
	second->fatal = 1;
	kwl_buffers_client_gone(server, second);
	check(e->holds == 0U && !e->returning, "gone: the going client's put now");
	check(waiting(compose) == 1U && compose->returns == c && compose->returns_tail == c, "gone: the other keeps waiting");

	/* A fatal client's buffer read by a drawing is not kept. */
	b = buffer_make(second, 16U, 5U);
	kwl_buffer_put(b);
	check(b->holds == 0U && !b->returning, "fatal: not kept");

	/* Without window mode nothing waits. */
	server->compose = NULL;
	a = buffer_make(first, 17U, 9U);
	kwl_buffer_put(a);
	check(a->holds == 0U && a->busy == 0U, "no window mode: released at once");
	server->compose = compose;

	/* The rest completes. */
	compose->drawings_done = 5U;
	kwl_buffers_returned(server);
	check(waiting(compose) == 0U && c->holds == 0U && c->busy == 0U, "all done: the last released");

	/* The verdict (the records are left to the process's end). */
	if (test_failures != 0U) {
		printf("returns-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("returns-test: PASS\n");
	return 0;
}

/* Counts and prints a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;
	printf("returns-test: FAIL %s\n", what);
	test_failures++;
}

/* Makes a committed buffer of a client, held once as a surface's current, last read by a drawing. */
static struct kwl_object *
buffer_make(
	struct kwl_client *client,
	uint32_t id,
	uint64_t read_drawing)
{
	struct kwl_object *buffer;

	/* The client's buffer, committed (to be released) and held as current. */
	buffer = kwl_create(client, id, KWL_BUFFER, 1U);
	if (buffer == NULL) {
		printf("returns-test: FAIL (create)\n");
		exit(1);
	}

	/* Committed, read by the drawing, and held. */
	buffer->busy = 1U;
	buffer->read_drawing = read_drawing;
	kwl_buffer_get(buffer);
	return buffer;
}

/* Tells whether an object is still in its client's list (not freed). */
static unsigned
in_client(
	const struct kwl_client *client,
	const struct kwl_object *object)
{
	const struct kwl_object *each;

	/* Each of the client's objects. */
	for (each = client->objects; each != NULL; each = each->next) {
		if (each == object)
			return 1U;
	}

	/* Not there: freed. */
	return 0U;
}

/* Counts the buffers waiting in the returns. */
static unsigned
waiting(
	const struct kwl_compose *compose)
{
	const struct kwl_object *buffer;
	unsigned count;

	/* Each, oldest first. */
	count = 0U;
	for (buffer = compose->returns; buffer != NULL; buffer = buffer->return_next)
		count++;
	return count;
}
