/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queries of the computer waiting for a reading (machine-wait.h,
 * ws188-p002).
 */

#include "machine-wait.h"

#include <errno.h>
#include <string.h>

/*
 * Adds a query, within the table's and its client's bounds.
 */
int
kwl_machine_wait_add(
	struct kwl_machine_wait *wait,
	uint64_t client,
	uint32_t object,
	uint32_t request,
	uint32_t what)
{
	struct kwl_machine_query *free_slot;
	unsigned index;
	unsigned own;

	/* A free slot, and how many its client holds already. */
	free_slot = NULL;
	own = 0U;
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		/* A free slot is remembered, the first one found. */
		if (!wait->queries[index].used) {
			if (free_slot == NULL)
				free_slot = &wait->queries[index];
			continue;
		}

		/* One of the same client. */
		if (wait->queries[index].client == client)
			own++;
	}

	/* No room in the table, or the client holds its share. */
	if (free_slot == NULL || own >= KWL_MACHINE_CLIENT_MAX)
		return EBUSY;

	/* The query, waiting for the next reading. */
	memset(free_slot, 0, sizeof(*free_slot));
	free_slot->client = client;
	free_slot->object = object;
	free_slot->request = request;
	free_slot->what = what;
	free_slot->order = wait->next_order;
	free_slot->used = 1U;
	free_slot->reading = 0U;
	wait->next_order++;

	/* Succeeded: the query waits. */
	return 0;
}

/*
 * Drops an object's queries.
 */
void
kwl_machine_wait_drop(
	struct kwl_machine_wait *wait,
	uint64_t client,
	uint32_t object)
{
	struct kwl_machine_query *query;
	unsigned index;

	/* Each query of the object, taken by a reading or not. */
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		query = &wait->queries[index];
		if (!query->used)
			continue;

		/* The object's own goes. */
		if (query->client == client && query->object == object)
			memset(query, 0, sizeof(*query));
	}
}

/*
 * Starts a reading of every waiting query; returns the parts asked.
 */
uint32_t
kwl_machine_wait_start(
	struct kwl_machine_wait *wait)
{
	struct kwl_machine_query *query;
	uint32_t what;
	unsigned index;

	/* Each query not taken yet is the new reading's. */
	what = 0U;
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		query = &wait->queries[index];
		if (!query->used || query->reading)
			continue;

		/* Taken, with its parts. */
		query->reading = 1U;
		what |= query->what;
	}

	/* Succeeded: the parts the reading reads. */
	return what;
}

/*
 * Takes the oldest query the reading took.
 */
int
kwl_machine_wait_take(
	struct kwl_machine_wait *wait,
	struct kwl_machine_query *query)
{
	struct kwl_machine_query *oldest;
	unsigned index;

	/* The oldest taken by the reading. */
	oldest = NULL;
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		/* Only one the reading took. */
		if (!wait->queries[index].used || !wait->queries[index].reading)
			continue;

		/* An older one than the oldest so far. */
		if (oldest == NULL || wait->queries[index].order < oldest->order)
			oldest = &wait->queries[index];
	}

	/* None is left. */
	if (oldest == NULL)
		return 0;

	/* Succeeded: the query, out of the table. */
	*query = *oldest;
	memset(oldest, 0, sizeof(*oldest));
	return 1;
}

/*
 * Tells whether a query waits for the next reading.
 */
int
kwl_machine_wait_pending(
	const struct kwl_machine_wait *wait)
{
	unsigned index;

	/* Each query. */
	for (index = 0; index < KWL_MACHINE_WAITING_MAX; index++) {
		/* One used that no reading took. */
		if (wait->queries[index].used && !wait->queries[index].reading)
			return 1;
	}

	/* None waits. */
	return 0;
}
