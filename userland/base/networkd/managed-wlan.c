/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements networkd's global managed-WLAN policy state.
 */

#include "userland/base/networkd/managed-wlan.h"

#include <errno.h>
#include <string.h>

static void clear_bytes(void *, size_t);
static void clear_connection(struct networkd_managed_wlan *);
static int connection_active(const struct networkd_managed_wlan *);
static int idle_state(enum networkd_managed_wlan_state);
static int managed_l3_valid(const struct networkd_managed_l3 *);
static int managed_route_equal(const struct networkd_managed_route *, const struct networkd_managed_route *);
static int route_event_valid(const struct rtm_ifinfo *);

/*
 * Initializes one disabled global WLAN policy.
 */
void
networkd_managed_wlan_init(
	struct networkd_managed_wlan *managed)
{
	/* Initializes every policy and connection field together. */
	if (managed == NULL)
		return;
	clear_bytes(managed, sizeof(*managed));
	managed->state = NETWORKD_WLAN_DISABLED;
}

/*
 * Enables automatic WLAN policy for one authenticated owner.
 *
 * A caller must retire an active connection before replacing its owner.
 */
int
networkd_managed_wlan_enable(
	struct networkd_managed_wlan *managed,
	uid_t owner_uid)
{
	/* Rejects an invalid policy object. */
	if (managed == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Preserves an active connection and its exact cleanup token. */
	if (connection_active(managed)) {
		errno = EBUSY;
		return -1;
	}

	/* Replaces the inactive policy owner and starts automatic discovery. */
	clear_bytes(managed, sizeof(*managed));
	managed->owner_uid = owner_uid;
	managed->owner_valid = 1;
	managed->state = NETWORKD_WLAN_AUTO_SEARCHING;

	/* Reports successful policy activation. */
	return 0;
}

/*
 * Disables one inactive global WLAN policy.
 *
 * A caller must retire an active connection before clearing its owner.
 */
int
networkd_managed_wlan_disable(
	struct networkd_managed_wlan *managed)
{
	/* Rejects an invalid policy object. */
	if (managed == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Preserves an active connection and its exact cleanup token. */
	if (connection_active(managed)) {
		errno = EBUSY;
		return -1;
	}

	/* Clears the owner and every inactive policy field. */
	clear_bytes(managed, sizeof(*managed));
	managed->state = NETWORKD_WLAN_DISABLED;

	/* Reports successful policy deactivation. */
	return 0;
}

/*
 * Tests whether one peer owns the enabled WLAN policy.
 */
int
networkd_managed_wlan_owner_matches(
	const struct networkd_managed_wlan *managed,
	uid_t owner_uid)
{
	/* Rejects a missing, disabled, or ownerless policy. */
	if (managed == NULL || managed->state == NETWORKD_WLAN_DISABLED ||
	    !managed->owner_valid)
		return 0;

	/* Reports whether the immutable peer UID matches the active owner. */
	return managed->owner_uid == owner_uid;
}

/*
 * Begins one connection to the orchestrator-selected profile and radio.
 *
 * The caller supplies the first eligible profile and first matching radio in
 * their stable orders. This state object admits only one such connection.
 */
int
networkd_managed_wlan_begin_connect(
	struct networkd_managed_wlan *managed,
	const char *interface,
	uint32_t ifindex,
	uint64_t event_floor,
	const void *ssid,
	size_t ssid_length)
{
	size_t interface_length;

	/* Validates the selected connection before changing global state. */
	if (managed == NULL || interface == NULL || ssid == NULL ||
	    ifindex == 0U || ssid_length == 0U ||
	    ssid_length > WLAN_SSID_MAX || !managed->owner_valid) {
		errno = EINVAL;
		return -1;
	}
	interface_length = strlen(interface);

	/* Rejects an invalid interface name or a second active connection. */
	if (interface_length == 0U || interface_length >= IFNAMSIZ) {
		errno = EINVAL;
		return -1;
	}
	if (connection_active(managed)) {
		errno = EBUSY;
		return -1;
	}

	/* Requires automatic search or an explicit manual-disconnect state. */
	if (managed->state != NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed->state != NETWORKD_WLAN_MANUAL_DISCONNECTED) {
		errno = EINVAL;
		return -1;
	}

	/* Publishes the selected nonsecret identity as one connecting session. */
	clear_connection(managed);
	memcpy(managed->connection.interface, interface, interface_length);
	managed->connection.interface[interface_length] = '\0';
	managed->connection.ifindex = ifindex;
	managed->connection.event_floor = event_floor;
	memcpy(managed->connection.ssid, ssid, ssid_length);
	managed->connection.ssid_length = ssid_length;
	managed->state = NETWORKD_WLAN_CONNECTING;

	/* Reports successful ownership transfer. */
	return 0;
}

/*
 * Commits a successful connection with its exact L3 ownership token.
 */
int
networkd_managed_wlan_commit_l3(
	struct networkd_managed_wlan *managed,
	const struct networkd_managed_l3 *snapshot)
{
	/* Requires one active connection transaction. */
	if (managed == NULL || managed->state != NETWORKD_WLAN_CONNECTING) {
		errno = EINVAL;
		return -1;
	}

	/* Validates the complete L3 token before retaining resolver contents. */
	if (!managed_l3_valid(snapshot)) {
		errno = EINVAL;
		return -1;
	}

	/* Requires an optional default route to name the selected interface. */
	if (snapshot->default_route_present &&
	    snapshot->default_route.ifindex != managed->connection.ifindex) {
		errno = EINVAL;
		return -1;
	}

	/* Publishes the complete ownership snapshot and connected state. */
	memcpy(
		&managed->connection.l3,
		snapshot,
		sizeof(managed->connection.l3));
	managed->connection.owns_l3 = 1;
	managed->state = NETWORKD_WLAN_CONNECTED;

	/* Reports successful ownership transfer. */
	return 0;
}

/*
 * Retires one connection into an enabled idle policy state.
 *
 * The caller must finish external L2 and L3 cleanup before this function
 * discards the exact identity and ownership token.
 */
int
networkd_managed_wlan_finish_connection(
	struct networkd_managed_wlan *managed,
	enum networkd_managed_wlan_state next_state)
{
	/* Requires an enabled policy owner. */
	if (managed == NULL || !managed->owner_valid) {
		errno = EINVAL;
		return -1;
	}

	/* Requires one valid enabled idle destination. */
	if (!idle_state(next_state)) {
		errno = EINVAL;
		return -1;
	}

	/* Clears only connection state while retaining the active policy owner. */
	clear_connection(managed);
	managed->state = next_state;

	/* Reports successful connection retirement. */
	return 0;
}

/*
 * Plans cleanup operations whose live values still match exact ownership.
 */
int
networkd_managed_wlan_plan_l3_cleanup(
	const struct networkd_managed_wlan *managed,
	uint32_t current_ifindex,
	const struct networkd_managed_l3 *current,
	struct networkd_managed_l3_cleanup *cleanup)
{
	const struct networkd_managed_wlan_connection *connection;
	int resolver_equal;
	int route_equal;

	/* Initializes a fail-closed plan before inspecting live state. */
	if (cleanup == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(cleanup, 0, sizeof(*cleanup));

	/* Rejects missing state without authorizing a mutation. */
	if (managed == NULL || current == NULL) {
		errno = EINVAL;
		cleanup->degraded = 1;
		return -1;
	}

	/* Validates the retained ownership token. */
	if (!managed_l3_valid(&managed->connection.l3)) {
		errno = EINVAL;
		cleanup->degraded = 1;
		return -1;
	}

	/* Validates the current comparison snapshot. */
	if (!managed_l3_valid(current)) {
		errno = EINVAL;
		cleanup->degraded = 1;
		return -1;
	}
	connection = &managed->connection;

	/* Leaves all preexisting resources outside networkd ownership. */
	if (!connection->owns_l3)
		return 0;

	/* Preserves every resource when the interface identity was reused. */
	if (current_ifindex != connection->ifindex) {
		errno = ENODEV;
		cleanup->degraded = 1;
		return -1;
	}

	/* Clears the address tuple only while all owned values match. */
	if (connection->l3.ipv4_owned) {
		if (current->address == connection->l3.address &&
		    current->netmask == connection->l3.netmask &&
		    current->broadcast == connection->l3.broadcast)
			cleanup->clear_ipv4 = 1;
		else
			cleanup->degraded = 1;
	}

	/* Deletes only the exact default route acquired by this connection. */
	if (connection->l3.default_route_owned) {
		route_equal = 0;
		if (current->default_route_present) {
			route_equal = managed_route_equal(
				&current->default_route,
				&connection->l3.default_route);
		}
		if (route_equal) {
			cleanup->delete_default_route = 1;
		} else {
			cleanup->degraded = 1;
		}
	}

	/* Unlinks only an owned resolver file with byte-identical contents. */
	if (connection->l3.resolver_owned) {
		resolver_equal = current->resolver_present &&
		    current->resolver_length == connection->l3.resolver_length;
		if (resolver_equal && current->resolver_length != 0U) {
			resolver_equal = memcmp(
				current->resolver,
				connection->l3.resolver,
				current->resolver_length) == 0;
		}
		if (resolver_equal)
			cleanup->unlink_resolver = 1;
		else
			cleanup->degraded = 1;
	}

	/* Reports partial preservation as a degraded cleanup result. */
	if (cleanup->degraded) {
		errno = ESTALE;
		return -1;
	}

	/* Reports that every owned resource remains safe to remove. */
	return 0;
}

/*
 * Applies one validated routing-socket event to the global connection.
 */
enum networkd_managed_wlan_action
networkd_managed_wlan_event(
	struct networkd_managed_wlan *managed,
	const struct rtm_ifinfo *event)
{
	struct networkd_managed_wlan_connection *connection;

	/* Rejects a missing global policy. */
	if (managed == NULL)
		return NETWORKD_WLAN_ACTION_NONE;

	/* Rejects records outside the fixed read-only event ABI. */
	if (!route_event_valid(event))
		return NETWORKD_WLAN_ACTION_NONE;

	/* Requires a conservative resnapshot for an enabled policy overflow. */
	if ((event->rtm_flags & RTM_IFINFO_F_OVERFLOW) != 0U) {
		if (managed->state == NETWORKD_WLAN_DISABLED)
			return NETWORKD_WLAN_ACTION_NONE;
		return NETWORKD_WLAN_ACTION_RESNAPSHOT;
	}
	connection = &managed->connection;

	/* Ignores events while no selected connection exists. */
	if (!connection_active(managed))
		return NETWORKD_WLAN_ACTION_NONE;

	/* Ignores events which cannot identify the selected connection. */
	if (connection->ifindex != event->rtm_ifindex ||
	    event->rtm_sequence <= connection->event_floor ||
	    event->rtm_sequence <= connection->last_event_sequence)
		return NETWORKD_WLAN_ACTION_NONE;
	connection->last_event_sequence = event->rtm_sequence;

	/* Rejects a stale event after learning the device generation. */
	if (connection->device_generation != 0U &&
	    connection->device_generation != event->rtm_device_generation)
		return NETWORKD_WLAN_ACTION_NONE;

	/* Retires a matching physical device before index reuse is possible. */
	if (event->rtm_transition == RTM_IFINFO_REMOVAL)
		return NETWORKD_WLAN_ACTION_RETIRE;

	/* Learns the generation from the first accepted device transition. */
	if (connection->device_generation == 0U)
		connection->device_generation = event->rtm_device_generation;

	/* Starts exactly one recovery for a post-success carrier loss. */
	if (event->rtm_transition == RTM_IFINFO_CARRIER_DOWN &&
	    managed->state == NETWORKD_WLAN_CONNECTED) {
		managed->state = NETWORKD_WLAN_RECONNECTING;
		return NETWORKD_WLAN_ACTION_RECOVER;
	}

	/* Reports that the event requires no orchestrator action. */
	return NETWORKD_WLAN_ACTION_NONE;
}

/*
 * Completes the single in-flight RF recovery operation.
 *
 * Failure retires the connection but preserves its owner and resumes global
 * automatic search. External L2 and L3 cleanup must already be complete.
 */
void
networkd_managed_wlan_recovery_complete(
	struct networkd_managed_wlan *managed,
	int succeeded)
{
	/* Applies only to the one active recovery generation. */
	if (managed == NULL || managed->state != NETWORKD_WLAN_RECONNECTING)
		return;

	/* Restores the connected state after a successful same-profile recovery. */
	if (succeeded) {
		managed->state = NETWORKD_WLAN_CONNECTED;
		return;
	}

	/* Returns a failed recovery to automatic search without losing its owner. */
	clear_connection(managed);
	managed->state = NETWORKD_WLAN_AUTO_SEARCHING;
}

/* Clears a fixed byte extent through volatile stores. */
static void
clear_bytes(
	void *storage,
	size_t length)
{
	volatile unsigned char *byte;

	/* Ignores a missing extent. */
	if (storage == NULL)
		return;

	/* Clears every byte without permitting dead-store removal. */
	byte = storage;
	while (length != 0U) {
		*byte++ = 0U;
		length--;
	}
}

/* Clears one connection while preserving global owner and policy state. */
static void
clear_connection(
	struct networkd_managed_wlan *managed)
{
	/* Clears every identity, selection, and L3 ownership byte together. */
	clear_bytes(&managed->connection, sizeof(managed->connection));
}

/* Tests whether the policy currently retains a selected connection. */
static int
connection_active(
	const struct networkd_managed_wlan *managed)
{
	/* Rejects a missing or idle connection identity. */
	if (managed == NULL || managed->connection.interface[0] == '\0' ||
	    managed->connection.ifindex == 0U)
		return 0;

	/* Reports one complete retained interface identity. */
	return 1;
}

/* Tests whether a state is an enabled state without an active connection. */
static int
idle_state(
	enum networkd_managed_wlan_state state)
{
	/* Accepts only the two enabled idle policy states. */
	if (state == NETWORKD_WLAN_AUTO_SEARCHING)
		return 1;
	if (state == NETWORKD_WLAN_MANUAL_DISCONNECTED)
		return 1;

	/* Rejects every active or disabled state. */
	return 0;
}

/* Validates one fixed-size L3 snapshot before retaining or comparing it. */
static int
managed_l3_valid(
	const struct networkd_managed_l3 *snapshot)
{
	/* Rejects inconsistent optional objects and oversized file contents. */
	if (snapshot == NULL ||
	    snapshot->resolver_length > NETWORKD_MANAGED_RESOLVER_MAX)
		return 0;
	if (!snapshot->default_route_present &&
	    (snapshot->default_route.flags != 0U ||
	     snapshot->default_route.ifindex != 0U ||
	     snapshot->default_route.destination != 0U ||
	     snapshot->default_route.gateway != 0U ||
	     snapshot->default_route.netmask != 0U))
		return 0;
	if (snapshot->default_route_owned &&
	    !snapshot->default_route_present)
		return 0;
	if (!snapshot->resolver_present && snapshot->resolver_length != 0U)
		return 0;
	if (snapshot->resolver_owned && !snapshot->resolver_present)
		return 0;

	/* Accepts one internally consistent bounded snapshot. */
	return 1;
}

/* Compares one complete canonical route ownership record. */
static int
managed_route_equal(
	const struct networkd_managed_route *left,
	const struct networkd_managed_route *right)
{
	/* Compares every field instead of relying on structure padding. */
	if (left->flags != right->flags)
		return 0;
	if (left->ifindex != right->ifindex)
		return 0;
	if (left->destination != right->destination)
		return 0;
	if (left->gateway != right->gateway)
		return 0;
	if (left->netmask != right->netmask)
		return 0;

	/* Reports exact route equality. */
	return 1;
}

/* Validates one fixed-width routing-socket interface event. */
static int
route_event_valid(
	const struct rtm_ifinfo *event)
{
	/* Rejects missing, malformed, reserved, or unknown event values. */
	if (event == NULL || event->rtm_version != RTM_VERSION ||
	    event->rtm_type != RTM_IFINFO ||
	    event->rtm_length != sizeof(*event) ||
	    event->rtm_sequence == 0U ||
	    event->rtm_device_generation == 0U ||
	    event->rtm_ifindex == 0U ||
	    (event->rtm_flags & ~RTM_IFINFO_F_OVERFLOW) != 0U ||
	    event->rtm_reserved[0] != 0U || event->rtm_reserved[1] != 0U ||
	    (event->rtm_transition != RTM_IFINFO_CARRIER_UP &&
	     event->rtm_transition != RTM_IFINFO_CARRIER_DOWN &&
	     event->rtm_transition != RTM_IFINFO_REMOVAL))
		return 0;

	/* Accepts one canonical event record. */
	return 1;
}
