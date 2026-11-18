/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The queries of the computer waiting for a reading (ws188-p002,
 * plan/ws188/phase001/phase.md section D3; machine-shell.c): at most
 * sixteen at once and four of one client, each until the reading that
 * took it ends.  It knows no Wayland and no thread, so that the host tests
 * build it alone.
 */

#ifndef KWL_MACHINE_WAIT_H
#define KWL_MACHINE_WAIT_H

#include <stdint.h>

/* The most queries waiting at once, and of one client. */
#define KWL_MACHINE_WAITING_MAX		16U
#define KWL_MACHINE_CLIENT_MAX		4U

/*
 * One query waiting: its client (by number), object and request, the
 * parts it asks, the order it came in, whether its slot is used, and
 * whether the reading under way took it (its answer is that reading's;
 * one not taken waits for the next).
 */
struct kwl_machine_query {
	uint64_t client;
	uint32_t object;
	uint32_t request;
	uint32_t what;
	uint64_t order;
	unsigned used;
	unsigned reading;
};

/*
 * The queries waiting, and the order the next one gets (it only grows, so
 * the oldest is the one with the smallest).
 */
struct kwl_machine_wait {
	struct kwl_machine_query queries[KWL_MACHINE_WAITING_MAX];
	uint64_t next_order;
};

/*
 * Adds a query.  Returns 0, or EBUSY when sixteen wait or four of its
 * client do (those taken by the reading under way count).
 */
int kwl_machine_wait_add(struct kwl_machine_wait *wait, uint64_t client, uint32_t object, uint32_t request, uint32_t what);

/*
 * Drops an object's queries (it went), whether a reading took them or not.
 */
void kwl_machine_wait_drop(struct kwl_machine_wait *wait, uint64_t client, uint32_t object);

/*
 * Starts a reading: every waiting query is taken by it.  Returns the parts
 * they ask together (0 when none waits: no reading is needed).
 */
uint32_t kwl_machine_wait_start(struct kwl_machine_wait *wait);

/*
 * Takes the oldest query the reading took out of the table.  Returns 1
 * with it, 0 when none is left.
 */
int kwl_machine_wait_take(struct kwl_machine_wait *wait, struct kwl_machine_query *query);

/*
 * Tells whether any query waits for the next reading (not taken by the one
 * under way).
 */
int kwl_machine_wait_pending(const struct kwl_machine_wait *wait);

#endif
