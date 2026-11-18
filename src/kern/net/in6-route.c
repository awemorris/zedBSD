/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The IPv6 routes (in6.h; ws130-p002): a route is its destination prefix
 * on an interface; adding one that is there renews it.  The lookup takes
 * the longest prefix, then the lowest metric, and skips a route whose
 * lifetime ran out.  The table carries each route's device pointer for
 * the caller, which owns the reference: a route removed or replaced gives
 * its pointer back.
 */

#include "in6.h"

#include <uapi/errno.h>

static struct in6_route *in6_route_find(struct in6_route_table *table, const struct in6_addr *destination, unsigned prefixlen, unsigned ifindex);

/*
 * Adds a route (its destination kept as the prefix alone), or renews the
 * route to the same prefix on the same interface; *replaced is then the
 * old route's device pointer, for the caller to release (NULL for a new
 * route).  Returns 0, EINVAL or ENOSPC.
 */
int
in6_route_add(
	struct in6_route_table *table,
	const struct in6_route *route,
	void **replaced)
{
	struct in6_route *entry;
	struct in6_addr prefix;
	unsigned index;

	/* A prefix that fits, on an interface. */
	*replaced = NULL;
	if (route->prefixlen > 128U || route->ifindex == 0U)
		return EINVAL;
	in6_prefix_mask(&prefix, &route->destination, route->prefixlen);

	/* The route there already, or a free entry. */
	entry = in6_route_find(table, &prefix, route->prefixlen, route->ifindex);
	if (entry != NULL) {
		*replaced = entry->device;
	} else {
		for (index = 0U; index < IN6_ROUTES_MAX; index++) {
			if (!table->entries[index].used)
				break;
		}

		/* None free. */
		if (index == IN6_ROUTES_MAX)
			return ENOSPC;
		entry = &table->entries[index];
	}

	/* The route, its destination the prefix. */
	*entry = *route;
	entry->destination = prefix;
	entry->used = 1;

	/* Succeeded: added or renewed. */
	return 0;
}

/*
 * Deletes the route to a prefix on an interface (ifindex 0: on any; the
 * first found).  *removed is its device pointer, for the caller to
 * release.  Returns 0 or ENOENT.
 */
int
in6_route_delete(
	struct in6_route_table *table,
	const struct in6_addr *destination,
	unsigned prefixlen,
	unsigned ifindex,
	void **removed)
{
	struct in6_route *entry;
	struct in6_addr prefix;

	/* The route. */
	*removed = NULL;
	if (prefixlen > 128U)
		return ENOENT;
	in6_prefix_mask(&prefix, destination, prefixlen);
	entry = in6_route_find(table, &prefix, prefixlen, ifindex);
	if (entry == NULL)
		return ENOENT;

	/* Freed, its device handed back. */
	*removed = entry->device;
	entry->used = 0;
	entry->device = NULL;

	/* Succeeded: deleted. */
	return 0;
}

/*
 * Finds the route to a destination: the longest prefix, then the lowest
 * metric, on the interface asked (0: any), whose lifetime has not run
 * out.  Returns it, or NULL.
 */
const struct in6_route *
in6_route_lookup(
	const struct in6_route_table *table,
	const struct in6_addr *destination,
	unsigned ifindex,
	uint64_t now_ms)
{
	const struct in6_route *entry;
	const struct in6_route *best;
	unsigned index;
	int match;

	/* Each live route that covers the destination. */
	best = NULL;
	for (index = 0U; index < IN6_ROUTES_MAX; index++) {
		entry = &table->entries[index];
		if (!entry->used)
			continue;
		if (ifindex != 0U && entry->ifindex != ifindex)
			continue;
		if (entry->expires_ms != 0U && now_ms >= entry->expires_ms)
			continue;
		match = in6_prefix_match(destination, &entry->destination, entry->prefixlen);
		if (!match)
			continue;

		/* The longer prefix, or the lower metric for the same length. */
		if (best == NULL || entry->prefixlen > best->prefixlen) {
			best = entry;
		} else if (entry->prefixlen == best->prefixlen && entry->metric < best->metric) {
			best = entry;
		}
	}

	/* Succeeded: the route, or none. */
	return best;
}

/*
 * Removes the routes whose lifetime ran out, writing their device
 * pointers (at most capacity routes a call).  Returns how many.
 */
unsigned
in6_route_expire(
	struct in6_route_table *table,
	uint64_t now_ms,
	void **removed,
	unsigned capacity)
{
	struct in6_route *entry;
	unsigned index;
	unsigned count;

	/* Each route run out, while there is room. */
	count = 0U;
	for (index = 0U; index < IN6_ROUTES_MAX && count < capacity; index++) {
		entry = &table->entries[index];
		if (!entry->used || entry->expires_ms == 0U || now_ms < entry->expires_ms)
			continue;
		removed[count] = entry->device;
		count++;
		entry->used = 0;
		entry->device = NULL;
	}

	/* Succeeded: the routes removed. */
	return count;
}

/* Removes every route of an interface, writing their device pointers (at most capacity a call).  Returns how many. */
unsigned
in6_route_purge(
	struct in6_route_table *table,
	unsigned ifindex,
	void **removed,
	unsigned capacity)
{
	struct in6_route *entry;
	unsigned index;
	unsigned count;

	/* Each route of the interface, while there is room. */
	count = 0U;
	for (index = 0U; index < IN6_ROUTES_MAX && count < capacity; index++) {
		entry = &table->entries[index];
		if (!entry->used || entry->ifindex != ifindex)
			continue;
		removed[count] = entry->device;
		count++;
		entry->used = 0;
		entry->device = NULL;
	}

	/* Succeeded: the routes removed. */
	return count;
}

/* Gives when the next route runs out (0 for none). */
uint64_t
in6_route_deadline(
	const struct in6_route_table *table)
{
	const struct in6_route *entry;
	uint64_t deadline;
	unsigned index;

	/* The earliest expiry. */
	deadline = 0U;
	for (index = 0U; index < IN6_ROUTES_MAX; index++) {
		entry = &table->entries[index];
		if (!entry->used || entry->expires_ms == 0U)
			continue;
		if (deadline == 0U || entry->expires_ms < deadline)
			deadline = entry->expires_ms;
	}

	/* Succeeded: the earliest, or none. */
	return deadline;
}

/* Finds the route to a prefix (already masked) on an interface (0: any), or NULL. */
static struct in6_route *
in6_route_find(
	struct in6_route_table *table,
	const struct in6_addr *destination,
	unsigned prefixlen,
	unsigned ifindex)
{
	struct in6_route *entry;
	unsigned index;
	int same;

	/* Each route of that prefix and length on that interface. */
	for (index = 0U; index < IN6_ROUTES_MAX; index++) {
		entry = &table->entries[index];
		if (!entry->used || entry->prefixlen != prefixlen)
			continue;
		if (ifindex != 0U && entry->ifindex != ifindex)
			continue;
		same = in6_equal(&entry->destination, destination);
		if (same)
			return entry;
	}

	/* None. */
	return NULL;
}
