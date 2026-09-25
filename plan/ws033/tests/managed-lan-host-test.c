/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises networkd's wired interface policy on the host.
 *
 * The policy takes no device and runs no command, so what it decides can be
 * put to it directly: a cable arrives, a cable goes, an event is lost, a
 * lease is not answered.  What it decides is the whole of the behaviour the
 * daemon has, so this is where that behaviour is checked.
 */

#include "userland/base/networkd/managed-lan.h"

#include <stdio.h>
#include <string.h>
#include <uapi/route.h>

static int failures;

static void
check(
	const char *name,
	int held)
{
	/* Handles the held condition. */
	if (held) {
		printf("ok %s\n", name);
		return;
	}
	printf("FAIL %s\n", name);
	failures++;
}

/* Builds one route-socket event. */
static struct rtm_ifinfo
event(
	uint32_t ifindex,
	uint32_t transition,
	uint32_t flags)
{
	struct rtm_ifinfo item;

	memset(&item, 0, sizeof(item));
	item.rtm_version = RTM_VERSION;
	item.rtm_type = RTM_IFINFO;
	item.rtm_length = sizeof(item);
	item.rtm_ifindex = ifindex;
	item.rtm_transition = transition;
	item.rtm_flags = flags;

	/* Returns the computed result. */
	return item;
}

/* Fills one interface's policy. */
static struct networkd_lan_policy
policy(
	const char *name,
	enum networkd_lan_mode mode)
{
	struct networkd_lan_policy item;

	memset(&item, 0, sizeof(item));
	strncpy(item.interface, name, sizeof(item.interface) - 1U);
	item.mode = mode;
	item.dhcp_timeout = 10U;
	if (mode == NETWORKD_LAN_MODE_STATIC) {
		strcpy(item.address, "192.0.2.10");
		strcpy(item.netmask, "255.255.255.0");
	}

	/* Returns the computed result. */
	return item;
}

int
main(
	void)
{
	struct networkd_lan lan;
	struct networkd_lan_policy table[3];
	struct networkd_lan_work work;
	struct rtm_ifinfo record;
	char address[NETWORKD_LAN_ADDRESS_MAX];
	unsigned char hardware[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

	table[0] = policy("eth0", NETWORKD_LAN_MODE_DHCP);
	table[1] = policy("eth1", NETWORKD_LAN_MODE_STATIC);
	table[2] = policy("eth2", NETWORKD_LAN_MODE_DISABLED);

	networkd_lan_init(&lan);
	check("nothing to do before anything is known",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	check("policy is accepted",
	      networkd_lan_set_policy(&lan, table, 3U) == 0);

	/* Not managing yet: a cable decides nothing. */
	check("observing works", networkd_lan_observe(&lan, "eth0", 1U, 1U, 1) == 0);
	check("a cable decides nothing while not managing",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	check("enable works", networkd_lan_enable(&lan) == 0);
	check("enabling asks for everything to be read again",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_RESNAPSHOT);

	networkd_lan_snapshot_begin(&lan);
	(void)networkd_lan_observe(&lan, "eth0", 1U, 1U, 1);
	(void)networkd_lan_observe(&lan, "eth1", 2U, 1U, 0);
	(void)networkd_lan_observe(&lan, "eth2", 3U, 1U, 1);
	networkd_lan_snapshot_end(&lan);

	check("a cabled dhcp interface is configured",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_CONFIGURE &&
	      strcmp(work.interface, "eth0") == 0 &&
	      work.policy.mode == NETWORKD_LAN_MODE_DHCP);

	/* A lease arrived. */
	check("configuring is recorded",
	      networkd_lan_configured(&lan, "eth0", 1) == 0);

	/*
	 * eth1 is wanted but has no cable: it is brought up once so that its
	 * driver can report the link (a USB adapter reports it only when up).
	 */
	check("an uncabled interface is raised to watch its link",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_RAISE &&
	      strcmp(work.interface, "eth1") == 0);
	check("raising is recorded", networkd_lan_raised(&lan, "eth1") == 0);
	check("a configured interface is not configured again",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	/* eth2 has a cable but is written down as disabled. */
	check("a disabled interface is left alone",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	/* The cable arrives in the static one. */
	record = event(2U, RTM_IFINFO_CARRIER_UP, 0U);
	check("a cable that arrives asks for the interface",
	      networkd_lan_event(&lan, &record) ==
	      NETWORKD_LAN_ACTION_CONFIGURE);
	check("the static interface is the one asked for",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_CONFIGURE &&
	      strcmp(work.interface, "eth1") == 0 &&
	      work.policy.mode == NETWORKD_LAN_MODE_STATIC);

	/* No answer: the interface is unconfigured and is not retried. */
	check("a configure that obtained nothing is recorded",
	      networkd_lan_configured(&lan, "eth1", 0) == 0);
	check("an unconfigured interface is not asked for again",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	/* The cable goes. */
	record = event(1U, RTM_IFINFO_CARRIER_DOWN, 0U);
	check("a cable that goes takes the interface down",
	      networkd_lan_event(&lan, &record) == NETWORKD_LAN_ACTION_DOWN);
	check("taking it down is recorded",
	      networkd_lan_down(&lan, "eth0") == 0);
	check("a down interface is raised again to see its next cable",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_RAISE &&
	      strcmp(work.interface, "eth0") == 0);
	check("raising it again is recorded",
	      networkd_lan_raised(&lan, "eth0") == 0);

	/* And comes back. */
	record = event(1U, RTM_IFINFO_CARRIER_UP, 0U);
	check("a cable that comes back asks for the interface again",
	      networkd_lan_event(&lan, &record) ==
	      NETWORKD_LAN_ACTION_CONFIGURE);
	check("the interface that came back is the one asked for",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_CONFIGURE &&
	      strcmp(work.interface, "eth0") == 0);
	(void)networkd_lan_configured(&lan, "eth0", 1);

	/* ws035-p047: an adapter that arrives has an index not held yet. */
	record = event(9U, RTM_IFINFO_ARRIVAL, 0U);
	check("an arriving adapter asks for everything to be read again",
	      networkd_lan_event(&lan, &record) ==
	      NETWORKD_LAN_ACTION_RESNAPSHOT);

	/*
	 * ws035-p047: the adapter is unplugged and another takes its name, a
	 * new generation that is not up, so no cable is seen in it.  It is
	 * raised again: nothing decided about the old adapter holds for it.
	 */
	networkd_lan_snapshot_begin(&lan);
	(void)networkd_lan_observe(&lan, "eth0", 1U, 2U, 0);
	(void)networkd_lan_observe(&lan, "eth1", 2U, 1U, 0);
	(void)networkd_lan_observe(&lan, "eth2", 3U, 1U, 1);
	networkd_lan_snapshot_end(&lan);
	check("an adapter that took a used name is raised again",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_RAISE &&
	      strcmp(work.interface, "eth0") == 0);
	(void)networkd_lan_raised(&lan, "eth0");

	/*
	 * A request configures it by hand before its cable is seen.  The
	 * cable event that bringing it up causes does not configure it again.
	 */
	check("a configuration by request is recorded",
	      networkd_lan_configured(&lan, "eth0", 1) == 0);
	record = event(1U, RTM_IFINFO_CARRIER_UP, 0U);
	(void)networkd_lan_event(&lan, &record);
	check("the cable of an interface configured by request asks nothing",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);

	/* A lost event means nothing held can be trusted. */
	record = event(1U, RTM_IFINFO_CARRIER_UP, RTM_IFINFO_F_OVERFLOW);
	check("a lost event asks for everything to be read again",
	      networkd_lan_event(&lan, &record) ==
	      NETWORKD_LAN_ACTION_RESNAPSHOT);
	check("nothing else is decided until it has been",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_RESNAPSHOT);
	networkd_lan_snapshot_begin(&lan);
	(void)networkd_lan_observe(&lan, "eth0", 1U, 1U, 1);
	networkd_lan_snapshot_end(&lan);
	check("an interface the snapshot did not name is forgotten",
	      lan.interface_count == 1U);

	/* A device that goes away. */
	record = event(1U, RTM_IFINFO_REMOVAL, 0U);
	check("a device that is removed decides nothing",
	      networkd_lan_event(&lan, &record) == NETWORKD_LAN_ACTION_NONE);
	check("a removed device is no longer held", lan.interface_count == 0U);

	/* Disabling stops the deciding. */
	networkd_lan_init(&lan);
	(void)networkd_lan_set_policy(&lan, table, 3U);
	(void)networkd_lan_enable(&lan);
	networkd_lan_snapshot_begin(&lan);
	(void)networkd_lan_observe(&lan, "eth0", 1U, 1U, 1);
	networkd_lan_snapshot_end(&lan);
	check("a cabled interface is asked for",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_CONFIGURE);
	check("disable works", networkd_lan_disable(&lan) == 0);
	check("nothing is decided once disabled",
	      networkd_lan_next(&lan, &work) == 0 &&
	      work.action == NETWORKD_LAN_ACTION_NONE);
	record = event(1U, RTM_IFINFO_CARRIER_UP, 0U);
	check("a cable decides nothing once disabled",
	      networkd_lan_event(&lan, &record) == NETWORKD_LAN_ACTION_NONE);

	/* The link-local address. */
	check("a link-local address is derived",
	      networkd_lan_link_local(hardware, sizeof(hardware), address,
				      sizeof(address)) == 0 &&
	      strcmp(address, "169.254.52.86") == 0);
	hardware[4] = 0x00;
	check("the reserved first block is avoided",
	      networkd_lan_link_local(hardware, sizeof(hardware), address,
				      sizeof(address)) == 0 &&
	      strcmp(address, "169.254.1.86") == 0);
	hardware[4] = 0xff;
	check("the reserved last block is avoided",
	      networkd_lan_link_local(hardware, sizeof(hardware), address,
				      sizeof(address)) == 0 &&
	      strcmp(address, "169.254.254.86") == 0);
	check("the same hardware gives the same address twice",
	      networkd_lan_link_local(hardware, sizeof(hardware), address,
				      sizeof(address)) == 0 &&
	      strcmp(address, "169.254.254.86") == 0);

	/* Reports the outcome. */
	if (failures != 0) {
		printf("MLAN-T001 managed-lan: FAIL (%d)\n", failures);
		return 1;
	}
	printf("MLAN-T001 managed-lan: PASS\n");

	/* Reports successful completion. */
	return 0;
}
