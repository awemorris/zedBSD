/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of a network link's IPv6 in libkeiland's system extension
 * (BUG-413): the protocol's tables say link_inet6 and link_router6 come since
 * version 35, and the view (system/system-view.c) keeps what those events
 * say, or nothing when an older compositor sends none of them (Settings then
 * shows "None").
 *
 * Exit 0 when every check passes; each failure is printed.
 */

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"
#include "userland/desktop/libkeiland/system/system-private.h"
#include "userland/desktop/libkeiland/system/system-protocol.h"

#include <keiland/keiland.h>
#include <wayland-client.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...)					\
	do {							\
		if (!(condition)) {				\
			printf("FAIL %s:%d: ", __FILE__, __LINE__);	\
			printf(__VA_ARGS__);			\
			printf("\n");				\
			failures++;				\
		}						\
	} while (0)

/* One interface of the details, as the link event gives it. */
static void
add_link(struct system_view *view, const char *name)
{
	struct kl_network_link link;

	memset(&link, 0, sizeof(link));
	snprintf(link.name, sizeof(link.name), "%s", name);
	snprintf(link.address, sizeof(link.address), "10.0.2.15");
	link.up = 1U;
	link.running = 1U;
	system_view_link(view, &link);
}

/* The protocol's tables: the version, the events and since when they come. */
static void
test_tables(void)
{
	const struct wl_interface *network;

	network = &kl_system_network_v1_interface;
	CHECK(KL_SYSTEM_NETWORK_SINCE_INET6 == 35U, "since %u", KL_SYSTEM_NETWORK_SINCE_INET6);
	CHECK(KL_SYSTEM_MANAGER_VERSION >= KL_SYSTEM_NETWORK_SINCE_INET6, "the manager offers %u", KL_SYSTEM_MANAGER_VERSION);
	CHECK(network->version == (int)KL_SYSTEM_NETWORK_SINCE_INET6, "network table version %d", network->version);
	CHECK(network->event_count == 13, "network events %d", network->event_count);
	if (network->event_count < 13)
		return;
	CHECK(strcmp(network->events[KL_SYSTEM_NETWORK_EVENT_LINK_INET6].name, "link_inet6") == 0, "event 11 %s", network->events[11].name);
	CHECK(strcmp(network->events[KL_SYSTEM_NETWORK_EVENT_LINK_INET6].signature, "35ss") == 0, "link_inet6 since 35");
	CHECK(strcmp(network->events[KL_SYSTEM_NETWORK_EVENT_LINK_ROUTER6].name, "link_router6") == 0, "event 12 %s", network->events[12].name);
	CHECK(strcmp(network->events[KL_SYSTEM_NETWORK_EVENT_LINK_ROUTER6].signature, "35ss") == 0, "link_router6 since 35");
	CHECK(strcmp(network->events[KL_SYSTEM_NETWORK_EVENT_LINK_SPEED].signature, "12su") == 0, "link_speed still since 12");
}

/* An older compositor (before version 35) sends no IPv6: the link has none. */
static void
test_older_compositor(void)
{
	struct system_view view;

	system_view_init(&view);
	add_link(&view, "em0");
	system_view_wired(&view, "em0", KL_WIRED_DHCP, "10.0.2.2");
	system_view_link_speed(&view, "em0", 1000U);
	system_view_details_done(&view);
	CHECK(view.link_count == 1U, "older: one link (%lu)", (unsigned long)view.link_count);
	CHECK(view.links[0].inet6_count == 0U && view.links[0].router6[0] == '\0', "older: no IPv6 (%u)", view.links[0].inet6_count);
	CHECK(strcmp(view.links[0].router, "10.0.2.2") == 0 && view.links[0].link_mbps == 1000U, "older: the rest as before");
}

/* A compositor of version 35: the addresses in their order up to the room, the router, the next details anew. */
static void
test_newer_compositor(void)
{
	static const char *const addresses[] = {
		"fec0::5054:ff:fe12:3456/64", "2001:db8::2/64", "2001:db8::3/64", "2001:db8::4/64", "fe80::5054:ff:fe12:3456/64"
	};
	struct system_view view;
	unsigned index;

	system_view_init(&view);
	add_link(&view, "lo0");
	add_link(&view, "em0");
	for (index = 0U; index < 5U; index++)
		system_view_link_inet6(&view, "em0", addresses[index]);
	system_view_link_inet6(&view, "em9", "2001:db8::9/64");
	system_view_link_inet6(&view, "lo0", "");
	system_view_link_router6(&view, "em0", "fe80::2");
	system_view_link_router6(&view, "em9", "fe80::9");

	/* Not seen before the details' end. */
	CHECK(view.link_count == 0U, "newer: details seen before their done");
	system_view_details_done(&view);
	CHECK(view.link_count == 2U, "newer: two links (%lu)", (unsigned long)view.link_count);
	CHECK(view.links[0].inet6_count == 0U && view.links[0].router6[0] == '\0', "newer: lo0 untouched (%u)", view.links[0].inet6_count);
	CHECK(view.links[1].inet6_count == KL_NETWORK_INET6_COUNT, "newer: em0 keeps %u (%u)", KL_NETWORK_INET6_COUNT, view.links[1].inet6_count);
	for (index = 0U; index < KL_NETWORK_INET6_COUNT && index < view.links[1].inet6_count; index++)
		CHECK(strcmp(view.links[1].inet6[index], addresses[index]) == 0, "newer: address %u %s", index, view.links[1].inet6[index]);
	CHECK(strcmp(view.links[1].router6, "fe80::2") == 0, "newer: router %s", view.links[1].router6);

	/* After the done the events change nothing; the next details start empty. */
	system_view_link_inet6(&view, "em0", "2001:db8::5/64");
	CHECK(view.links[1].inet6_count == KL_NETWORK_INET6_COUNT, "newer: an address outside the details kept");
	add_link(&view, "em0");
	system_view_details_done(&view);
	CHECK(view.link_count == 1U && view.links[0].inet6_count == 0U && view.links[0].router6[0] == '\0', "newer: the next details without IPv6");
}

int
main(void)
{
	/* Each part. */
	test_tables();
	test_older_compositor();
	test_newer_compositor();

	/* The verdict. */
	if (failures != 0) {
		printf("host-inet6: FAIL (%d)\n", failures);
		return 1;
	}
	printf("host-inet6: PASS\n");
	return 0;
}
