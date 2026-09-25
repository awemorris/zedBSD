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

#include <stdio.h>
#include <string.h>
#include <uapi/route.h>

static struct networkd_lan_interface *find_interface(struct networkd_lan *,
						     const char *);
static const struct networkd_lan_policy *find_policy(
	const struct networkd_lan *, const char *);
static struct networkd_lan_interface *find_by_index(struct networkd_lan *,
						    uint32_t);
static int name_valid(const char *);

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
	}
	item->ifindex = ifindex;
	item->generation = generation;
	item->present = 1;

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

	/* Handles a cable that arrived. */
	if (event->rtm_transition == RTM_IFINFO_CARRIER_UP) {
		item->carrier = 1;
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
		policy = find_policy(lan, item->name);

		/* An interface the configuration does not name is left alone. */
		if (policy == NULL ||
		    policy->mode == NETWORKD_LAN_MODE_DISABLED)
			continue;
		work->action = NETWORKD_LAN_ACTION_CONFIGURE;
		strncpy(work->interface, item->name,
			sizeof(work->interface) - 1U);
		work->policy = *policy;

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
		policy = find_policy(lan, item->name);

		/* An interface the configuration does not name is left alone. */
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
	 * interface carries the link-local address that says so, and is
	 * not tried again until its cable moves: a server that did not
	 * answer will not answer any sooner for being asked in a loop.
	 */
	item->state = obtained ? NETWORKD_LAN_CONFIGURED :
		      NETWORKD_LAN_UNCONFIGURED;

	/* Reports successful completion. */
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
