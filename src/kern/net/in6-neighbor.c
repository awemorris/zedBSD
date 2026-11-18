/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A neighbor's state (in6.h; ws130-p002, RFC 4861 section 7.3): what a
 * packet to it, an advertisement, a link address seen in another message,
 * a confirmation from above and the passing of time do to the entry, and
 * what the caller is to send, flush or tell.  The caller has the entry's
 * interface and address set before the first call.
 */

#include "in6.h"

static int in6_neighbor_same_link(const struct in6_neighbor *neighbor, const uint8_t link[6]);
static void in6_neighbor_set_link(struct in6_neighbor *neighbor, const uint8_t link[6]);

/*
 * A packet is to go to the neighbor.  A new entry starts resolution (a
 * multicast solicitation, the packet held); a stale one waits a while for
 * a confirmation before probing.  The caller sends now when the entry has
 * its link address.
 */
unsigned
in6_neighbor_resolve(
	struct in6_neighbor *neighbor,
	uint64_t now_ms,
	unsigned retrans_ms)
{
	/* A new neighbor: the first solicitation. */
	if (neighbor->state == ND6_NONE) {
		neighbor->state = ND6_INCOMPLETE;
		neighbor->have_link = 0;
		neighbor->router = 0;
		neighbor->probes = 1U;
		neighbor->deadline_ms = now_ms + retrans_ms;
		return ND6_ACTION_SEND_MULTICAST;
	}

	/* A stale neighbor is used, and probed if nothing confirms it soon. */
	if (neighbor->state == ND6_STALE) {
		neighbor->state = ND6_DELAY;
		neighbor->deadline_ms = now_ms + ND6_DELAY_MS;
	}

	/* Nothing to send yet. */
	return 0U;
}

/*
 * A link address for the neighbor came in its own solicitation, a router
 * solicitation or advertisement, or a redirect: a new or changed one makes
 * the entry stale, and a held packet can go.
 */
unsigned
in6_neighbor_link(
	struct in6_neighbor *neighbor,
	const uint8_t link[6],
	uint64_t now_ms)
{
	unsigned actions;
	int same;

	/* An entry waiting for its address: the held packet goes. */
	(void)now_ms;
	actions = 0U;
	if (neighbor->state == ND6_INCOMPLETE)
		actions = ND6_ACTION_FLUSH;

	/* The same address on a known entry changes nothing. */
	if (neighbor->state != ND6_NONE && neighbor->state != ND6_INCOMPLETE) {
		same = in6_neighbor_same_link(neighbor, link);
		if (same)
			return 0U;
	}

	/* The new address, not yet confirmed. */
	in6_neighbor_set_link(neighbor, link);
	neighbor->state = ND6_STALE;
	neighbor->probes = 0U;
	neighbor->deadline_ms = 0U;

	/* Succeeded: what to do. */
	return actions;
}

/*
 * The neighbor advertised (RFC 4861 section 7.2.5): link is its target
 * link address (NULL without one), solicited, override and router its
 * flags.
 */
unsigned
in6_neighbor_advert(
	struct in6_neighbor *neighbor,
	const uint8_t *link,
	int solicited,
	int override,
	int router,
	uint64_t now_ms,
	unsigned reachable_ms)
{
	unsigned actions;
	int same;

	/* Nothing asked about a free entry. */
	if (neighbor->state == ND6_NONE)
		return 0U;

	/* An entry waiting for its address: an answer without one is no use. */
	if (neighbor->state == ND6_INCOMPLETE) {
		if (link == NULL)
			return 0U;
		in6_neighbor_set_link(neighbor, link);
		neighbor->router = router;
		neighbor->probes = 0U;
		if (solicited) {
			neighbor->state = ND6_REACHABLE;
			neighbor->deadline_ms = now_ms + reachable_ms;
		} else {
			neighbor->state = ND6_STALE;
			neighbor->deadline_ms = 0U;
		}

		/* The held packet can go. */
		return ND6_ACTION_FLUSH;
	}

	/* Without override, another link address only makes a reachable entry doubtful. */
	same = 1;
	if (link != NULL)
		same = in6_neighbor_same_link(neighbor, link);
	if (!override && !same) {
		if (neighbor->state == ND6_REACHABLE) {
			neighbor->state = ND6_STALE;
			neighbor->deadline_ms = 0U;
		}

		/* The cached address stays. */
		return 0U;
	}

	/* The address taken, and the state: confirmed when solicited, stale when the address changed. */
	actions = 0U;
	if (link != NULL && !same)
		in6_neighbor_set_link(neighbor, link);
	if (solicited) {
		neighbor->state = ND6_REACHABLE;
		neighbor->probes = 0U;
		neighbor->deadline_ms = now_ms + reachable_ms;
	} else if (!same) {
		neighbor->state = ND6_STALE;
		neighbor->probes = 0U;
		neighbor->deadline_ms = 0U;
	}

	/* A router that says it no longer is one. */
	if (neighbor->router && !router)
		actions |= ND6_ACTION_UNREACHABLE;
	neighbor->router = router;

	/* Succeeded: what to do. */
	return actions;
}

/* Something above (a TCP acknowledgement) confirmed the neighbor reachable. */
unsigned
in6_neighbor_confirm(
	struct in6_neighbor *neighbor,
	uint64_t now_ms,
	unsigned reachable_ms)
{
	/* Only an entry with its address. */
	if (neighbor->state == ND6_NONE || neighbor->state == ND6_INCOMPLETE)
		return 0U;

	/* Reachable for a while. */
	neighbor->state = ND6_REACHABLE;
	neighbor->probes = 0U;
	neighbor->deadline_ms = now_ms + reachable_ms;
	return 0U;
}

/*
 * The entry's time came (the caller calls this when deadline_ms passed):
 * another solicitation, a reachable entry gone stale, a delay over and
 * probing begun, or a neighbor that never answered given up.
 */
unsigned
in6_neighbor_timer(
	struct in6_neighbor *neighbor,
	uint64_t now_ms,
	unsigned retrans_ms)
{
	unsigned actions;

	/* What the state does when its time comes. */
	actions = 0U;
	switch (neighbor->state) {
	case ND6_INCOMPLETE:
		/* Another multicast solicitation, or no answer: given up. */
		if (neighbor->probes < ND6_MULTICAST_SOLICIT) {
			neighbor->probes++;
			neighbor->deadline_ms = now_ms + retrans_ms;
			return ND6_ACTION_SEND_MULTICAST;
		}

		/* The held packet goes with the entry. */
		actions = ND6_ACTION_DROP | ND6_ACTION_FREE;
		break;
	case ND6_REACHABLE:
		/* Not confirmed for a while: stale. */
		neighbor->state = ND6_STALE;
		neighbor->deadline_ms = 0U;
		return 0U;
	case ND6_DELAY:
		/* No confirmation: probing. */
		neighbor->state = ND6_PROBE;
		neighbor->probes = 1U;
		neighbor->deadline_ms = now_ms + retrans_ms;
		return ND6_ACTION_SEND_UNICAST;
	case ND6_PROBE:
		/* Another unicast probe, or no answer: given up. */
		if (neighbor->probes < ND6_UNICAST_SOLICIT) {
			neighbor->probes++;
			neighbor->deadline_ms = now_ms + retrans_ms;
			return ND6_ACTION_SEND_UNICAST;
		}

		/* The entry goes. */
		actions = ND6_ACTION_FREE;
		break;
	default:
		/* A stale or free entry has no time. */
		neighbor->deadline_ms = 0U;
		return 0U;
	}

	/* Given up: a router's loss is told, and the entry is free. */
	if (neighbor->router)
		actions |= ND6_ACTION_UNREACHABLE;
	neighbor->state = ND6_NONE;
	neighbor->have_link = 0;
	neighbor->deadline_ms = 0U;
	return actions;
}

/* Tells whether the entry has this link address. */
static int
in6_neighbor_same_link(
	const struct in6_neighbor *neighbor,
	const uint8_t link[6])
{
	unsigned index;

	/* No address yet is not the same. */
	if (!neighbor->have_link)
		return 0;

	/* Each byte. */
	for (index = 0U; index < 6U; index++) {
		if (neighbor->link[index] != link[index])
			return 0;
	}

	/* The same. */
	return 1;
}

/* Keeps a link address. */
static void
in6_neighbor_set_link(
	struct in6_neighbor *neighbor,
	const uint8_t link[6])
{
	unsigned index;

	/* Each byte. */
	for (index = 0U; index < 6U; index++)
		neighbor->link[index] = link[index];
	neighbor->have_link = 1;
}
