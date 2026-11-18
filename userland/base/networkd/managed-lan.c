/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements networkd's wired interface policy.
 */

#include "userland/base/networkd/managed-lan.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <uapi/route.h>

static struct networkd_lan_interface *find_interface(struct networkd_lan *,
						     const char *);
static const struct networkd_lan_policy *find_policy(
	const struct networkd_lan *, const char *);
static const struct networkd_lan_policy *policy_for(
	const struct networkd_lan *, const char *);
static struct networkd_lan_interface *find_by_index(struct networkd_lan *,
						    uint32_t);
static int name_valid(const char *);
static int retry_wanted(const struct networkd_lan *, const struct networkd_lan_interface *);

/*
 * Implements the networkd lan init operation.
 */
void
networkd_lan_init(
	struct networkd_lan *lan)
{
	/* Handles the lan availability. */
	if (lan == NULL)
		return;
	memset(lan, 0, sizeof(*lan));
}

/*
 * Implements the networkd lan set policy operation.
 */
int
networkd_lan_set_policy(
	struct networkd_lan *lan,
	const struct networkd_lan_policy *policy,
	size_t count)
{
	size_t index;

	/* Rejects what cannot be held. */
	if (lan == NULL || (count != 0U && policy == NULL) ||
	    count > NETWORKD_LAN_MAX)
		return -1;

	/* Process each remaining element. */
	for (index = 0U; index < count; index++) {
		/* Rejects a name that could not be an interface. */
		if (!name_valid(policy[index].interface))
			return -1;
	}
	memset(lan->policy, 0, sizeof(lan->policy));
	if (count != 0U)
		memcpy(lan->policy, policy, count * sizeof(*policy));
	lan->policy_count = count;

	/*
	 * What each interface is to be told has changed, so an interface
	 * that has a cable is considered again.  One that is configured
	 * stays as it is until its cable moves: taking a working interface
	 * down because a file was rewritten would cost more than it gains.
	 */
	for (index = 0U; index < lan->interface_count; index++) {
		if (lan->interfaces[index].state == NETWORKD_LAN_IDLE &&
		    lan->interfaces[index].carrier)
			lan->interfaces[index].state = NETWORKD_LAN_PENDING;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Replaces what the configuration says of one interface, and asks for it
 * to be configured again when a cable is in it.
 */
int
networkd_lan_set_interface(
	struct networkd_lan *lan,
	const struct networkd_lan_policy *policy)
{
	struct networkd_lan_interface *item;
	size_t index;
	int valid;
	int differs;

	/* Rejects what cannot be held. */
	if (lan == NULL || policy == NULL)
		return -1;
	valid = name_valid(policy->interface);
	if (!valid)
		return -1;

	/* The interface's record. */
	for (index = 0U; index < lan->policy_count; index++) {
		differs = strcmp(lan->policy[index].interface, policy->interface);
		if (differs == 0)
			break;
	}

	/* A new one when there is none, while there is room. */
	if (index == lan->policy_count) {
		if (lan->policy_count >= NETWORKD_LAN_MAX)
			return -1;
		lan->policy_count++;
	}

	/* Replaced by what was asked. */
	lan->policy[index] = *policy;

	/*
	 * An interface with a cable is configured again, as the user asked:
	 * a plain configure, not a DHCP retry that resets the link first.
	 */
	item = find_interface(lan, policy->interface);
	if (item != NULL && item->carrier) {
		item->state = NETWORKD_LAN_PENDING;
		item->retry_pending = 0;
	}

	/* Reports successful completion. */
	return 0;
}

/* Records the MTU an interface was given, for its next configuration. */
int
networkd_lan_set_mtu(
	struct networkd_lan *lan,
	const char *interface,
	unsigned mtu)
{
	size_t index;
	int differs;
	int valid;

	/* Rejects what cannot be held. */
	if (lan == NULL || interface == NULL)
		return -1;
	valid = name_valid(interface);
	if (!valid)
		return -1;

	/* The interface's record, when the configuration names it. */
	for (index = 0U; index < lan->policy_count; index++) {
		differs = strcmp(lan->policy[index].interface, interface);
		if (differs == 0) {
			lan->policy[index].mtu = mtu;
			break;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the networkd lan enable operation.
 */
int
networkd_lan_enable(
	struct networkd_lan *lan)
{
	size_t index;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;
	lan->enabled = 1;

	/*
	 * Everything is read again, because what happened while nothing was
	 * managing them was not recorded and cannot be inferred.
	 */
	lan->resnapshot_due = 1;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		if (lan->interfaces[index].carrier)
			lan->interfaces[index].state = NETWORKD_LAN_PENDING;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the networkd lan disable operation.
 *
 * The interfaces are left as they stand.  Disabling says the daemon stops
 * deciding, and an interface that is carrying traffic keeps carrying it;
 * taking the network down is what `net down' is for.
 */
int
networkd_lan_disable(
	struct networkd_lan *lan)
{
	size_t index;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;
	lan->enabled = 0;
	lan->resnapshot_due = 0;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		if (lan->interfaces[index].state == NETWORKD_LAN_PENDING)
			lan->interfaces[index].state = NETWORKD_LAN_IDLE;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the networkd lan snapshot begin operation.
 */
void
networkd_lan_snapshot_begin(
	struct networkd_lan *lan)
{
	size_t index;

	/* Handles the lan availability. */
	if (lan == NULL)
		return;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++)
		lan->interfaces[index].present = 0;
}

/*
 * Implements the networkd lan snapshot end operation.
 */
void
networkd_lan_snapshot_end(
	struct networkd_lan *lan)
{
	size_t index;
	size_t kept;

	/* Handles the lan availability. */
	if (lan == NULL)
		return;
	kept = 0U;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		/* An interface that is gone is no longer anybody's to decide. */
		if (!lan->interfaces[index].present)
			continue;
		if (kept != index)
			lan->interfaces[kept] = lan->interfaces[index];
		kept++;
	}
	lan->interface_count = kept;
	lan->resnapshot_due = 0;
}

/*
 * Implements the networkd lan observe operation.
 */
int
networkd_lan_observe(
	struct networkd_lan *lan,
	const char *name,
	uint32_t ifindex,
	uint64_t generation,
	int carrier)
{
	struct networkd_lan_interface *item;

	/* Rejects what cannot be recorded. */
	if (lan == NULL || !name_valid(name))
		return -1;
	carrier = carrier != 0;
	item = find_interface(lan, name);

	/* Handles an interface seen for the first time. */
	if (item == NULL) {
		/* Handles a table with no room for another. */
		if (lan->interface_count == NETWORKD_LAN_MAX)
			return -1;
		item = &lan->interfaces[lan->interface_count];
		memset(item, 0, sizeof(*item));
		strncpy(item->name, name, sizeof(item->name) - 1U);
		item->state = NETWORKD_LAN_IDLE;
		lan->interface_count++;
	} else if (item->generation != generation) {
		/*
		 * The name was reused by another device.  Nothing this
		 * daemon decided about the old one is true of the new one:
		 * it has not been brought up and no cable has been seen in it.
		 */
		item->state = NETWORKD_LAN_IDLE;
		item->raised = 0;
		item->carrier = 0;

		/* The administrator took the old device down, not this one. */
		item->held_down = 0;
	}
	item->ifindex = ifindex;
	item->generation = generation;
	item->present = 1;

	/*
	 * A running state read while a retry resets the link says what the
	 * reset is doing, not where the cable is; the daemon reads the
	 * interfaces again once the reset is over.
	 */
	if (item->resetting)
		return 0;

	/* A cable that arrived asks for the interface to be configured. */
	if (carrier && !item->carrier && item->state == NETWORKD_LAN_IDLE)
		item->state = NETWORKD_LAN_PENDING;

	/* A cable that went leaves nothing to keep. */
	if (!carrier && item->carrier)
		item->state = NETWORKD_LAN_IDLE;

	/* A first sight with a cable is a cable that arrived. */
	if (carrier && item->state == NETWORKD_LAN_IDLE && lan->enabled)
		item->state = NETWORKD_LAN_PENDING;
	item->carrier = carrier;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the networkd lan event operation.
 */
enum networkd_lan_action
networkd_lan_event(
	struct networkd_lan *lan,
	const struct rtm_ifinfo *event)
{
	struct networkd_lan_interface *item;

	/* Rejects what cannot be read. */
	if (lan == NULL || event == NULL ||
	    event->rtm_version != RTM_VERSION ||
	    event->rtm_type != RTM_IFINFO)
		return NETWORKD_LAN_ACTION_NONE;

	/*
	 * An overflow means events were lost, so what is held may be wrong
	 * about any interface.  Reading them all again is the only way back
	 * to a state that can be trusted.
	 */
	if ((event->rtm_flags & RTM_IFINFO_F_OVERFLOW) != 0U) {
		lan->resnapshot_due = 1;
		return NETWORKD_LAN_ACTION_RESNAPSHOT;
	}
	item = find_by_index(lan, event->rtm_ifindex);

	/* An event about an interface that is not held asks for a snapshot. */
	if (item == NULL) {
		/* Handles an event about something that has just gone. */
		if (event->rtm_transition == RTM_IFINFO_REMOVAL)
			return NETWORKD_LAN_ACTION_NONE;
		lan->resnapshot_due = 1;

		/* Reports what is to be done. */
		return NETWORKD_LAN_ACTION_RESNAPSHOT;
	}

	/* Handles a device that is gone. */
	if (event->rtm_transition == RTM_IFINFO_REMOVAL) {
		item->present = 0;
		item->carrier = 0;
		item->state = NETWORKD_LAN_IDLE;
		networkd_lan_snapshot_end(lan);

		/* Reports what is to be done. */
		return NETWORKD_LAN_ACTION_NONE;
	}

	/*
	 * A carrier change while a retry takes the interface down and up is
	 * the reset's own doing.  The daemon reads the interface again when
	 * the reset is over, which says whether the cable is still there.
	 */
	if (item->resetting) {
		if (event->rtm_transition == RTM_IFINFO_CARRIER_UP)
			return NETWORKD_LAN_ACTION_NONE;
		if (event->rtm_transition == RTM_IFINFO_CARRIER_DOWN)
			return NETWORKD_LAN_ACTION_NONE;
	}

	/* Handles a cable that arrived. */
	if (event->rtm_transition == RTM_IFINFO_CARRIER_UP) {
		item->carrier = 1;

		/*
		 * An interface the administrator took down stays down: the
		 * cable is remembered, and asks for it only once it is
		 * brought up again (BUG-293).
		 */
		if (item->held_down)
			return NETWORKD_LAN_ACTION_NONE;

		/* A cable in an interface nothing was decided for asks for it. */
		if (lan->enabled && item->state == NETWORKD_LAN_IDLE)
			item->state = NETWORKD_LAN_PENDING;

		/* Reports what is to be done. */
		return lan->enabled ? NETWORKD_LAN_ACTION_CONFIGURE :
		       NETWORKD_LAN_ACTION_NONE;
	}

	/* Handles a cable that went. */
	if (event->rtm_transition == RTM_IFINFO_CARRIER_DOWN) {
		item->carrier = 0;

		/* Nothing was decided about it, so there is nothing to undo. */
		if (item->state == NETWORKD_LAN_IDLE)
			return NETWORKD_LAN_ACTION_NONE;
		item->state = NETWORKD_LAN_IDLE;

		/* Reports what is to be done. */
		return lan->enabled ? NETWORKD_LAN_ACTION_DOWN :
		       NETWORKD_LAN_ACTION_NONE;
	}

	/* Reports that nothing is to be done. */
	return NETWORKD_LAN_ACTION_NONE;
}

/*
 * Implements the networkd lan next operation.
 */
int
networkd_lan_next(
	const struct networkd_lan *lan,
	struct networkd_lan_work *work)
{
	const struct networkd_lan_policy *policy;
	size_t index;

	/* Rejects what cannot be answered. */
	if (lan == NULL || work == NULL)
		return -1;
	memset(work, 0, sizeof(*work));
	work->action = NETWORKD_LAN_ACTION_NONE;

	/* Everything must be read again before anything is decided. */
	if (lan->resnapshot_due) {
		work->action = NETWORKD_LAN_ACTION_RESNAPSHOT;
		return 0;
	}

	/* Nothing is decided while the daemon is not managing them. */
	if (!lan->enabled)
		return 0;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		const struct networkd_lan_interface *item =
		    &lan->interfaces[index];

		/* Only an interface with a cable and no configuration. */
		if (item->state != NETWORKD_LAN_PENDING || !item->carrier)
			continue;

		/* One the administrator took down is not configured (BUG-293). */
		if (item->held_down)
			continue;
		policy = policy_for(lan, item->name);

		/* An interface the configuration disables is left alone. */
		if (policy == NULL ||
		    policy->mode == NETWORKD_LAN_MODE_DISABLED)
			continue;
		work->action = NETWORKD_LAN_ACTION_CONFIGURE;
		strncpy(work->interface, item->name,
			sizeof(work->interface) - 1U);
		work->policy = *policy;

		/* A DHCP retry resets the link before it asks (ws254). */
		work->reset = item->retry_pending;

		/* Reports successful completion. */
		return 0;
	}

	/*
	 * An interface the configuration wants but that has no cable is
	 * brought up once, so that its driver can report the link at all;
	 * the carrier event then asks for it to be configured.
	 */
	for (index = 0U; index < lan->interface_count; index++) {
		const struct networkd_lan_interface *item =
		    &lan->interfaces[index];

		/* Only a present interface without a cable, not raised yet. */
		if (item->carrier || item->raised)
			continue;

		/* One the administrator took down is not raised (BUG-293). */
		if (item->held_down)
			continue;
		policy = policy_for(lan, item->name);

		/* An interface the configuration disables is left alone. */
		if (policy == NULL ||
		    policy->mode == NETWORKD_LAN_MODE_DISABLED)
			continue;
		work->action = NETWORKD_LAN_ACTION_RAISE;
		strncpy(work->interface, item->name,
			sizeof(work->interface) - 1U);
		work->policy = *policy;

		/* Reports successful completion. */
		return 0;
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Records that the daemon has brought an interface up to watch its link.
 */
int
networkd_lan_raised(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;

	/* Rejects what cannot be recorded. */
	if (lan == NULL || !name_valid(name))
		return -1;
	item = find_interface(lan, name);

	/* Handles an interface that went while the work was being done. */
	if (item == NULL)
		return -1;
	item->raised = 1;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the networkd lan configured operation.
 */
int
networkd_lan_configured(
	struct networkd_lan *lan,
	const char *name,
	int obtained)
{
	struct networkd_lan_interface *item;

	/* Rejects what cannot be recorded. */
	if (lan == NULL || !name_valid(name))
		return -1;
	item = find_interface(lan, name);

	/* Handles an interface that went while the work was being done. */
	if (item == NULL)
		return -1;

	/*
	 * An address of its own means configured.  Without one the
	 * interface is left without an IPv4 address (no 169.254 fallback is
	 * given).  It is tried again when its cable moves, when the
	 * administrator brings it up again, or, for DHCP, at the retry the
	 * daemon plans next with networkd_lan_retry_schedule (ws254).
	 */
	item->state = obtained ? NETWORKD_LAN_CONFIGURED :
		      NETWORKD_LAN_UNCONFIGURED;

	/* The outcome ends the retry that asked for it, and any planned one. */
	item->retry_pending = 0;
	item->retry_at = 0U;

	/* Reports successful completion. */
	return 0;
}

/*
 * Plans a DHCP retry for an interface whose configure obtained nothing.
 *
 * The retry is one minute from now.  An interface that is configured,
 * held down, without a cable, or configured statically gets none.
 */
int
networkd_lan_retry_schedule(
	struct networkd_lan *lan,
	const char *name,
	uint64_t now)
{
	struct networkd_lan_interface *item;
	int valid;
	int wanted;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;

	/* Rejects a name that could not be an interface. */
	valid = name_valid(name);
	if (!valid)
		return -1;

	/* Finds the interface whose configure failed. */
	item = find_interface(lan, name);
	if (item == NULL)
		return -1;

	/* Decides whether the interface is one that DHCP is asked for again. */
	wanted = retry_wanted(lan, item);
	if (!wanted) {
		item->retry_at = 0U;
		return 0;
	}

	/* RETRY_AT is when the event loop wakes to ask again. */
	item->retry_at = now + NETWORKD_LAN_DHCP_RETRY_US;

	/* Succeeded: a retry is planned. */
	return 1;
}

/*
 * Makes each interface whose DHCP retry is due pending as a retry.
 */
int
networkd_lan_retry_due(
	struct networkd_lan *lan,
	uint64_t now)
{
	struct networkd_lan_interface *item;
	size_t index;
	int count;
	int wanted;

	/* Rejects a missing state. */
	if (lan == NULL)
		return 0;
	count = 0;

	/* Looks at every interface with a planned retry. */
	for (index = 0U; index < lan->interface_count; index++) {
		item = &lan->interfaces[index];

		/* Skips one with no retry planned, or not due yet. */
		if (item->retry_at == 0U)
			continue;
		if (item->retry_at > now)
			continue;

		/* The planned retry is used up, whether it is still wanted or not. */
		item->retry_at = 0U;

		/* Skips one whose cable, configuration or administrator changed meanwhile. */
		wanted = retry_wanted(lan, item);
		if (!wanted)
			continue;

		/*
		 * PENDING with RETRY_PENDING has the next configure reset the
		 * link before it asks DHCP again.
		 */
		item->state = NETWORKD_LAN_PENDING;
		item->retry_pending = 1;
		count++;
	}

	/* Reports how many interfaces are to be asked again. */
	return count;
}

/*
 * Reports the milliseconds until the next DHCP retry is due.
 */
int
networkd_lan_retry_timeout(
	const struct networkd_lan *lan,
	uint64_t now)
{
	const struct networkd_lan_interface *item;
	uint64_t earliest;
	uint64_t milliseconds;
	size_t index;
	int wanted;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;
	earliest = 0U;

	/* Finds the earliest retry that is still wanted. */
	for (index = 0U; index < lan->interface_count; index++) {
		item = &lan->interfaces[index];

		/* Skips one with no retry planned. */
		if (item->retry_at == 0U)
			continue;

		/* Skips one that would not be asked again when it is due. */
		wanted = retry_wanted(lan, item);
		if (!wanted)
			continue;

		/* Keeps the earlier of the two. */
		if (earliest == 0U || item->retry_at < earliest)
			earliest = item->retry_at;
	}

	/* Nothing is planned: nothing is waited for. */
	if (earliest == 0U)
		return -1;

	/* A retry that is due already is due now. */
	if (earliest <= now)
		return 0;

	/* Rounds up, so that the loop does not wake a moment too early. */
	milliseconds = (earliest - now + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;

	/* Reports the wait until the retry. */
	return (int)milliseconds;
}

/*
 * Records that the daemon is about to take an interface down and up for
 * a DHCP retry.
 */
int
networkd_lan_reset_begin(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;
	int valid;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;

	/* Rejects a name that could not be an interface. */
	valid = name_valid(name);
	if (!valid)
		return -1;

	/* Finds the interface being reset. */
	item = find_interface(lan, name);
	if (item == NULL)
		return -1;

	/* RESETTING has the carrier changes the reset causes change nothing. */
	item->resetting = 1;

	/* Succeeded: the reset may begin. */
	return 0;
}

/*
 * Records that the down and up of a DHCP retry are over.
 *
 * The caller reads the interfaces again afterwards, which says whether a
 * cable is still in it.
 */
int
networkd_lan_reset_end(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;
	int valid;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;

	/* Rejects a name that could not be an interface. */
	valid = name_valid(name);
	if (!valid)
		return -1;

	/* Finds the interface that was reset. */
	item = find_interface(lan, name);
	if (item == NULL)
		return -1;

	/* RESETTING cleared: carrier changes mean a cable again. */
	item->resetting = 0;

	/* Succeeded: the reset is over. */
	return 0;
}

/*
 * Implements the networkd lan down operation.
 */
int
networkd_lan_down(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;

	/* Rejects what cannot be recorded. */
	if (lan == NULL || !name_valid(name))
		return -1;
	item = find_interface(lan, name);

	/* Handles an interface that is no longer held. */
	if (item == NULL)
		return -1;
	item->state = NETWORKD_LAN_IDLE;

	/* A down interface cannot report its next cable; it is raised again. */
	item->raised = 0;

	/* Reports successful completion. */
	return 0;
}

/*
 * Records that the administrator took an interface down.
 *
 * The interface is not raised again to see its next cable, and a cable
 * does not configure it.  A down that the daemon undid at once, by raising
 * the interface and falling back to a link-local address when DHCP did not
 * answer, is what left that address on an interface the administrator had
 * taken down (BUG-293).
 */
int
networkd_lan_hold_down(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;
	int valid;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;

	/* Rejects a name that could not be an interface. */
	valid = name_valid(name);
	if (!valid)
		return -1;

	/* Finds the interface the administrator named. */
	item = find_interface(lan, name);
	if (item == NULL)
		return -1;

	/*
	 * HELD_DOWN keeps the policy from raising or configuring the
	 * interface until the administrator brings it up again.  It holds
	 * nothing it was given any more, and it is no longer up to watch its
	 * link.
	 */
	item->held_down = 1;
	item->state = NETWORKD_LAN_IDLE;
	item->raised = 0;

	/* A held interface is not asked again by itself. */
	item->retry_pending = 0;
	item->retry_at = 0U;

	/* Succeeded: the interface stays down until it is released. */
	return 0;
}

/*
 * Records that the administrator brought an interface up again.
 *
 * The policy decides for the interface again.  One with a cable is
 * configured again, and so is one whose configuration had not succeeded:
 * bringing it up is how the administrator asks for another try without
 * moving the cable (BUG-293).  A configured interface keeps what it has,
 * and one without a cable is raised to see its next cable as before.
 */
int
networkd_lan_release(
	struct networkd_lan *lan,
	const char *name)
{
	struct networkd_lan_interface *item;
	int valid;

	/* Rejects a missing state. */
	if (lan == NULL)
		return -1;

	/* Rejects a name that could not be an interface. */
	valid = name_valid(name);
	if (!valid)
		return -1;

	/* Finds the interface the administrator named. */
	item = find_interface(lan, name);
	if (item == NULL)
		return -1;

	/* HELD_DOWN cleared: the policy raises and configures it again. */
	item->held_down = 0;

	/*
	 * `net up' configures it as it always has, without the reset of a
	 * retry; a retry is planned again if this configure fails too.
	 */
	item->retry_pending = 0;
	item->retry_at = 0U;

	/* A configured interface keeps the address it has. */
	if (item->state == NETWORKD_LAN_CONFIGURED)
		return 0;

	/* One without a cable waits for its next cable. */
	if (!item->carrier) {
		item->state = NETWORKD_LAN_IDLE;
		return 0;
	}

	/* A cable in it asks for it to be configured again. */
	item->state = NETWORKD_LAN_PENDING;

	/* Succeeded: the interface is managed again. */
	return 0;
}

/*
 * Implements the networkd lan link local operation.
 */
int
networkd_lan_link_local(
	const unsigned char *hardware,
	size_t length,
	char *output,
	size_t capacity)
{
	unsigned third;
	unsigned fourth;
	int written;

	/* Rejects a hardware address that cannot name one. */
	if (hardware == NULL || length < 2U || output == NULL || capacity == 0U)
		return -1;

	/*
	 * The last two bytes of the hardware address are the ones that
	 * differ between two cards of the same make, so they are what the
	 * host part is taken from.
	 */
	third = hardware[length - 2U];
	fourth = hardware[length - 1U];

	/*
	 * 169.254.0.0/24 and 169.254.255.0/24 are reserved, so the third
	 * byte is moved into what is left of the range rather than being
	 * allowed to fall outside it.
	 */
	if (third == 0U)
		third = 1U;
	else if (third == 255U)
		third = 254U;
	written = snprintf(output, capacity, "169.254.%u.%u", third, fourth);

	/* Handles output that did not fit. */
	if (written < 0 || (size_t)written >= capacity)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the find interface operation. */
static struct networkd_lan_interface *
find_interface(
	struct networkd_lan *lan,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		if (strcmp(lan->interfaces[index].name, name) == 0)
			return &lan->interfaces[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the find by index operation. */
static struct networkd_lan_interface *
find_by_index(
	struct networkd_lan *lan,
	uint32_t ifindex)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0U; index < lan->interface_count; index++) {
		if (lan->interfaces[index].ifindex == ifindex)
			return &lan->interfaces[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * The DHCP timeout, in seconds, of an interface the configuration does not
 * name (the same default net gives a configured "dhcp" without a timeout).
 */
#define NETWORKD_LAN_DEFAULT_DHCP_SECONDS	10U

/*
 * Reports what an interface is to be told: its record in the
 * configuration, or, for a wired interface the configuration does not
 * name, DHCP (2026-09-29 user decision: interfaces come and go with the
 * adapters plugged in, so one that is not named takes an address by
 * itself).  A wireless interface ("wlan...") is the wireless manager's,
 * which asks for its address when it has associated, and gets no default.
 * The default lives in one buffer the next call overwrites; the caller
 * copies it before asking again.
 */
static const struct networkd_lan_policy *
policy_for(
	const struct networkd_lan *lan,
	const char *name)
{
	static struct networkd_lan_policy fallback;
	const struct networkd_lan_policy *policy;
	int wireless;

	/* The configuration's record, when there is one. */
	policy = find_policy(lan, name);
	if (policy != NULL)
		return policy;

	/* A wireless interface is not a cable's. */
	wireless = strncmp(name, "wlan", 4U) == 0;
	if (wireless)
		return NULL;

	/* DHCP, as a configured "dhcp" without a timeout would be. */
	memset(&fallback, 0, sizeof(fallback));
	strncpy(fallback.interface, name, sizeof(fallback.interface) - 1U);
	fallback.mode = NETWORKD_LAN_MODE_DHCP;
	fallback.dhcp_timeout = NETWORKD_LAN_DEFAULT_DHCP_SECONDS;

	/* Reports the default. */
	return &fallback;
}

/* Supports the find policy operation. */
static const struct networkd_lan_policy *
find_policy(
	const struct networkd_lan *lan,
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0U; index < lan->policy_count; index++) {
		if (strcmp(lan->policy[index].interface, name) == 0)
			return &lan->policy[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * Tells whether an interface is one whose DHCP is asked again: managed,
 * unconfigured, with its cable, not held down, and configured for DHCP.
 */
static int
retry_wanted(
	const struct networkd_lan *lan,
	const struct networkd_lan_interface *item)
{
	const struct networkd_lan_policy *policy;

	/* Nothing is asked while the daemon is not managing interfaces. */
	if (!lan->enabled)
		return 0;

	/* A configured one has its address; one that is pending is being asked. */
	if (item->state != NETWORKD_LAN_UNCONFIGURED)
		return 0;

	/* Without a cable there is nobody to ask; the next cable asks. */
	if (!item->carrier)
		return 0;

	/* One the administrator took down stays down (BUG-293). */
	if (item->held_down)
		return 0;

	/* Only DHCP is asked again: a static address does not answer later. */
	policy = policy_for(lan, item->name);
	if (policy == NULL)
		return 0;
	if (policy->mode != NETWORKD_LAN_MODE_DHCP)
		return 0;

	/* Succeeded: DHCP is asked again. */
	return 1;
}

/* Supports the name valid operation. */
static int
name_valid(
	const char *name)
{
	size_t length;

	/* Handles a name that is absent or could not fit. */
	if (name == NULL)
		return 0;

	/*
	 * The length is counted here rather than taken from strnlen, which
	 * is not in every C library this is built against.
	 */
	for (length = 0U; length < IFNAMSIZ; length++)
		if (name[length] == '\0')
			break;

	/* Returns the computed result. */
	return length != 0U && length < IFNAMSIZ;
}
