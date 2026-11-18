/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An interface's IPv6 addresses (in6.h; ws130-p002, RFC 4862): adding one
 * (tentative while duplicate address detection runs, unless NODAD),
 * renewing its lifetimes, the times that make it preferred, deprecated or
 * gone, a duplicate, and the choice of a source address among them
 * (RFC 6724 section 5).
 */

#include "in6.h"

#include <uapi/errno.h>

static uint64_t in6_address_deadline_of(uint64_t now_ms, uint32_t seconds);
static void in6_address_event_set(struct in6_address_event *event, unsigned kind, const struct in6_address *entry);
static int in6_address_better(const struct in6_candidate *left, const struct in6_candidate *right, const struct in6_addr *destination, int prefer_temporary);

/*
 * Adds an address, or renews the lifetimes and the caller's flags of one
 * the interface has (*added says which).  A new address is tentative and
 * has its first solicitation due now, unless IN6_ADDRESS_NODAD.  Returns
 * 0, EINVAL or ENOSPC.
 */
int
in6_address_add(
	struct in6_address_table *table,
	const struct in6_addr *address,
	unsigned prefixlen,
	unsigned flags,
	uint32_t valid_s,
	uint32_t preferred_s,
	uint64_t now_ms,
	int *added)
{
	struct in6_address *entry;
	unsigned index;
	int multicast;
	int unspecified;

	/* A unicast address with a prefix length, a preferred lifetime within the valid one. */
	*added = 0;
	multicast = in6_is_multicast(address);
	unspecified = in6_is_unspecified(address);
	if (multicast || unspecified)
		return EINVAL;
	if (prefixlen > 128U)
		return EINVAL;
	if (preferred_s > valid_s)
		return EINVAL;

	/* An address the interface has: its lifetimes and the caller's flags renewed. */
	entry = in6_address_find(table, address);
	if (entry != NULL) {
		entry->prefixlen = prefixlen;
		entry->flags = (entry->flags & ~IN6_ADDRESS_CALLER) | (flags & IN6_ADDRESS_CALLER);
		entry->valid_ms = in6_address_deadline_of(now_ms, valid_s);
		entry->preferred_ms = in6_address_deadline_of(now_ms, preferred_s);
		if (preferred_s != 0U)
			entry->flags &= ~IN6_ADDRESS_DEPRECATED;
		if (preferred_s == 0U)
			entry->flags |= IN6_ADDRESS_DEPRECATED;
		return 0;
	}

	/* A free entry. */
	for (index = 0U; index < IN6_ADDRESSES_MAX; index++) {
		if (!table->entries[index].used)
			break;
	}

	/* None free. */
	if (index == IN6_ADDRESSES_MAX)
		return ENOSPC;

	/* The new address: tentative with its first solicitation due now, unless no detection is asked. */
	entry = &table->entries[index];
	entry->address = *address;
	entry->prefixlen = prefixlen;
	entry->flags = flags & IN6_ADDRESS_CALLER;
	entry->valid_ms = in6_address_deadline_of(now_ms, valid_s);
	entry->preferred_ms = in6_address_deadline_of(now_ms, preferred_s);
	if (preferred_s == 0U)
		entry->flags |= IN6_ADDRESS_DEPRECATED;
	entry->dad_left = 0U;
	entry->dad_ms = 0U;
	if ((flags & IN6_ADDRESS_NODAD) == 0U) {
		entry->flags |= IN6_ADDRESS_TENTATIVE;
		entry->dad_left = IN6_DAD_TRANSMITS;
		entry->dad_ms = now_ms;
	}

	/* In use. */
	entry->used = 1;

	/* Succeeded: added. */
	*added = 1;
	return 0;
}

/* Removes an address.  Returns 0 or ENOENT. */
int
in6_address_remove(
	struct in6_address_table *table,
	const struct in6_addr *address)
{
	struct in6_address *entry;

	/* The address's entry, freed. */
	entry = in6_address_find(table, address);
	if (entry == NULL)
		return ENOENT;
	entry->used = 0;

	/* Succeeded: removed. */
	return 0;
}

/* Finds an address's entry, or NULL. */
struct in6_address *
in6_address_find(
	struct in6_address_table *table,
	const struct in6_addr *address)
{
	unsigned index;
	int same;

	/* Each used entry. */
	for (index = 0U; index < IN6_ADDRESSES_MAX; index++) {
		if (!table->entries[index].used)
			continue;
		same = in6_equal(&table->entries[index].address, address);
		if (same)
			return &table->entries[index];
	}

	/* None. */
	return NULL;
}

/*
 * Moves the addresses on to now: a solicitation due for a tentative one
 * (DAD_PROBE, the next due a retransmission later), the detection passed
 * after the last (PREFERRED), a preferred lifetime run out (DEPRECATED),
 * a valid one run out (EXPIRED: removed).  Writes at most capacity events
 * and returns how many; what does not fit waits for the next call.
 */
unsigned
in6_address_tick(
	struct in6_address_table *table,
	uint64_t now_ms,
	struct in6_address_event *events,
	unsigned capacity)
{
	struct in6_address *entry;
	unsigned index;
	unsigned count;

	/* Each used entry, while there is room for its event. */
	count = 0U;
	for (index = 0U; index < IN6_ADDRESSES_MAX && count < capacity; index++) {
		entry = &table->entries[index];
		if (!entry->used)
			continue;

		/* Its valid lifetime over: the address goes. */
		if (entry->valid_ms != 0U && now_ms >= entry->valid_ms) {
			in6_address_event_set(&events[count], IN6_EVENT_EXPIRED, entry);
			count++;
			entry->used = 0;
			continue;
		}

		/* Its detection's next step. */
		if ((entry->flags & IN6_ADDRESS_TENTATIVE) != 0U && entry->dad_ms != 0U && now_ms >= entry->dad_ms) {
			if (entry->dad_left > 0U) {
				/* One more solicitation, and a retransmission's wait. */
				entry->dad_left--;
				entry->dad_ms = now_ms + IN6_RETRANS_MS;
				in6_address_event_set(&events[count], IN6_EVENT_DAD_PROBE, entry);
			} else {
				/* No answer to the last: the address is the interface's. */
				entry->flags &= ~IN6_ADDRESS_TENTATIVE;
				entry->dad_ms = 0U;
				in6_address_event_set(&events[count], IN6_EVENT_PREFERRED, entry);
			}

			/* One event for the step. */
			count++;
			continue;
		}

		/* Its preferred lifetime over: no longer chosen as a source. */
		if ((entry->flags & IN6_ADDRESS_DEPRECATED) == 0U && entry->preferred_ms != 0U && now_ms >= entry->preferred_ms) {
			entry->flags |= IN6_ADDRESS_DEPRECATED;
			in6_address_event_set(&events[count], IN6_EVENT_DEPRECATED, entry);
			count++;
		}
	}

	/* Succeeded: the events. */
	return count;
}

/*
 * Takes the news that another node has a tentative address (its
 * advertisement, or its own detection's solicitation): the address is
 * marked duplicated and not used.  Returns 1 with the event, 0 when the
 * address is not tentative here.
 */
int
in6_address_duplicate(
	struct in6_address_table *table,
	const struct in6_addr *address,
	struct in6_address_event *event)
{
	struct in6_address *entry;

	/* A tentative address of the interface's. */
	entry = in6_address_find(table, address);
	if (entry == NULL)
		return 0;
	if ((entry->flags & IN6_ADDRESS_TENTATIVE) == 0U)
		return 0;

	/* Not used: duplicated, no longer tentative. */
	entry->flags &= ~IN6_ADDRESS_TENTATIVE;
	entry->flags |= IN6_ADDRESS_DUPLICATED;
	entry->dad_ms = 0U;
	entry->dad_left = 0U;
	in6_address_event_set(event, IN6_EVENT_DUPLICATE, entry);

	/* Succeeded: the duplicate is told. */
	return 1;
}

/*
 * Starts duplicate address detection again on every address that takes it
 * (the link came back: another node may have taken one meanwhile).
 */
void
in6_address_restart(
	struct in6_address_table *table,
	uint64_t now_ms)
{
	struct in6_address *entry;
	unsigned index;

	/* Each used address that detection is for. */
	for (index = 0U; index < IN6_ADDRESSES_MAX; index++) {
		entry = &table->entries[index];
		if (!entry->used || (entry->flags & IN6_ADDRESS_NODAD) != 0U)
			continue;
		entry->flags |= IN6_ADDRESS_TENTATIVE;
		entry->flags &= ~IN6_ADDRESS_DUPLICATED;
		entry->dad_left = IN6_DAD_TRANSMITS;
		entry->dad_ms = now_ms;
	}
}

/* Gives when the next step of any address is due (0 for none). */
uint64_t
in6_address_deadline(
	const struct in6_address_table *table)
{
	const struct in6_address *entry;
	uint64_t deadline;
	unsigned index;

	/* The earliest of every entry's times. */
	deadline = 0U;
	for (index = 0U; index < IN6_ADDRESSES_MAX; index++) {
		entry = &table->entries[index];
		if (!entry->used)
			continue;
		if (entry->valid_ms != 0U && (deadline == 0U || entry->valid_ms < deadline))
			deadline = entry->valid_ms;
		if (entry->dad_ms != 0U && (deadline == 0U || entry->dad_ms < deadline))
			deadline = entry->dad_ms;
		if ((entry->flags & IN6_ADDRESS_DEPRECATED) == 0U && entry->preferred_ms != 0U &&
		    (deadline == 0U || entry->preferred_ms < deadline))
			deadline = entry->preferred_ms;
	}

	/* Succeeded: the earliest, or none. */
	return deadline;
}

/*
 * Chooses a source address for a destination among candidates (RFC 6724
 * section 5): never a tentative or duplicated one; then the destination
 * itself, the right scope, not deprecated, the outgoing interface's, the
 * same label, a temporary one (when preferred), the longest common
 * prefix.  Returns the entry, or NULL when none may be used.
 */
const struct in6_address *
in6_address_select(
	const struct in6_candidate *candidates,
	unsigned count,
	const struct in6_addr *destination,
	int prefer_temporary)
{
	const struct in6_candidate *best;
	unsigned index;
	unsigned unusable;
	int better;

	/* Each usable candidate against the best so far. */
	best = NULL;
	unusable = IN6_ADDRESS_TENTATIVE | IN6_ADDRESS_DUPLICATED;
	for (index = 0U; index < count; index++) {
		if ((candidates[index].entry->flags & unusable) != 0U)
			continue;
		if (best == NULL) {
			best = &candidates[index];
			continue;
		}

		/* The better of the two. */
		better = in6_address_better(&candidates[index], best, destination, prefer_temporary);
		if (better)
			best = &candidates[index];
	}

	/* No usable address. */
	if (best == NULL)
		return NULL;

	/* Succeeded: the chosen address. */
	return best->entry;
}

/* Gives when a lifetime of so many seconds runs out (0 for one that never does). */
static uint64_t
in6_address_deadline_of(
	uint64_t now_ms,
	uint32_t seconds)
{
	/* Forever. */
	if (seconds == IN6_LIFETIME_FOREVER)
		return 0U;

	/* So many seconds from now (an expiry is never 0). */
	return now_ms + (uint64_t)seconds * 1000U + 1U;
}

/* Writes an event about an address. */
static void
in6_address_event_set(
	struct in6_address_event *event,
	unsigned kind,
	const struct in6_address *entry)
{
	event->kind = kind;
	event->address = entry->address;
	event->prefixlen = entry->prefixlen;
	event->flags = entry->flags;
}

/*
 * Tells whether left is the better source than right (RFC 6724's rules 1,
 * 2, 3, 5, 6, 7 and 8; rule 4, home addresses, does not apply).
 */
static int
in6_address_better(
	const struct in6_candidate *left,
	const struct in6_candidate *right,
	const struct in6_addr *destination,
	int prefer_temporary)
{
	unsigned destination_scope;
	unsigned left_scope;
	unsigned right_scope;
	unsigned destination_label;
	unsigned left_label;
	unsigned right_label;
	unsigned left_common;
	unsigned right_common;
	unsigned left_temporary;
	unsigned right_temporary;
	int same;

	/* Rule 1: the destination itself. */
	same = in6_equal(&left->entry->address, destination);
	if (same)
		return 1;
	same = in6_equal(&right->entry->address, destination);
	if (same)
		return 0;

	/* Rule 2: a scope too small loses; between two large enough, the smaller wins. */
	destination_scope = in6_scope(destination);
	left_scope = in6_scope(&left->entry->address);
	right_scope = in6_scope(&right->entry->address);
	if (left_scope < right_scope && left_scope < destination_scope)
		return 0;
	if (left_scope < right_scope)
		return 1;
	if (right_scope < left_scope && right_scope < destination_scope)
		return 1;
	if (right_scope < left_scope)
		return 0;

	/* Rule 3: not deprecated. */
	if ((left->entry->flags & IN6_ADDRESS_DEPRECATED) == 0U && (right->entry->flags & IN6_ADDRESS_DEPRECATED) != 0U)
		return 1;
	if ((left->entry->flags & IN6_ADDRESS_DEPRECATED) != 0U && (right->entry->flags & IN6_ADDRESS_DEPRECATED) == 0U)
		return 0;

	/* Rule 5: the outgoing interface's. */
	if (left->outgoing && !right->outgoing)
		return 1;
	if (!left->outgoing && right->outgoing)
		return 0;

	/* Rule 6: the destination's label. */
	destination_label = in6_policy_label(destination);
	left_label = in6_policy_label(&left->entry->address);
	right_label = in6_policy_label(&right->entry->address);
	if (left_label == destination_label && right_label != destination_label)
		return 1;
	if (left_label != destination_label && right_label == destination_label)
		return 0;

	/* Rule 7: a temporary address, when they are preferred (RFC 8981), otherwise a public one. */
	left_temporary = left->entry->flags & IN6_ADDRESS_TEMPORARY;
	right_temporary = right->entry->flags & IN6_ADDRESS_TEMPORARY;
	if (left_temporary != right_temporary) {
		if (prefer_temporary && left_temporary != 0U)
			return 1;
		if (!prefer_temporary && left_temporary == 0U)
			return 1;
		return 0;
	}

	/* Rule 8: the longest common prefix with the destination. */
	left_common = in6_common_prefix(&left->entry->address, destination);
	right_common = in6_common_prefix(&right->entry->address, destination);
	if (left_common > right_common)
		return 1;

	/* Otherwise the earlier stays. */
	return 0;
}
