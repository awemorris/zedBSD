/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws089-p003: the network's backend for the host tests, in place of
 * userland/desktop/settings/network.c (WS131 p011: over kl_system_*): made-up states (host_network_fake)
 * and requests that print what they would ask of the daemon and change the
 * made-up state as the daemon would.  Test code only; the program never
 * has it.
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

void host_network_fake(struct se_app *app, const char *scenario);

/* One made-up network of a scan. */
static void
host_ap(
	struct se_network *network,
	const char *ssid,
	int rssi,
	unsigned secured)
{
	struct kl_network_ap *ap;

	ap = &network->scan[network->scan_count];
	(void)snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
	ap->rssi = rssi;
	ap->secured = secured;
	network->scan_count++;
}

/* One made-up interface. */
static void
host_link(
	struct se_network *network,
	const char *name,
	const char *address,
	unsigned running,
	uint64_t received,
	uint64_t sent)
{
	struct kl_network_link *link;
	int loopback;
	int radio;

	link = &network->links[network->link_count];
	memset(link, 0, sizeof(*link));
	(void)snprintf(link->name, sizeof(link->name), "%s", name);
	link->up = 1;
	link->running = running;
	(void)snprintf(link->address, sizeof(link->address), "%s", address);
	if (address[0] != '\0')
		(void)snprintf(link->netmask, sizeof(link->netmask), "%s", "255.255.255.0");
	(void)snprintf(link->hardware, sizeof(link->hardware), "52:54:00:00:00:%02x", (unsigned)(network->link_count + 0x10));
	link->mtu = 1500;

	/* A wired one takes DHCP, with the router when it has an address (ws089-p022). */
	loopback = strncmp(name, "lo", 2U);
	radio = strncmp(name, "wlan", 4U);
	if (loopback != 0 && radio != 0) {
		link->wired_mode = KL_WIRED_DHCP;
		if (address[0] != '\0')
			(void)snprintf(link->router, sizeof(link->router), "%s", "192.168.1.1");
	}

	/* The counters, and the interface counted. */
	link->received_bytes = received;
	link->sent_bytes = sent;
	network->link_count++;
}

/*
 * Fills the network with a made-up state: "wifi" (on a Wi-Fi network, a
 * wired interface up too), "wired" (wired only, the radio off, a gigabit
 * link), "nocable" (the wired interface up without its cable, BUG-213),
 * "absent" (no radio), "down" (the daemon not running), and for the
 * Welcome's Network step (ws177-p007) "nonet" (no radio and no cable) and
 * "joinfail" (on Wi-Fi, the last join refused with a wrong key).
 */
void
host_network_fake(
	struct se_app *app,
	const char *scenario)
{
	struct se_network *network;
	unsigned index;

	network = &app->network;
	memset(network, 0, sizeof(*network));
	network->live = 1;
	network->state.reachable = 1;
	if (strcmp(scenario, "down") == 0) {
		network->state.reachable = 0;
		return;
	}

	/* The interfaces and the servers. */
	host_link(network, "lo0", "127.0.0.1", 1, 0, 0);
	network->links[0].loopback = 1;
	host_link(network, "em0", "10.0.2.15", 1, 1200000000ULL, 320000000ULL);
	network->links[1].link_mbps = 1000U;
	(void)snprintf(network->dns[0], sizeof(network->dns[0]), "%s", "10.0.2.3");
	(void)snprintf(network->dns[1], sizeof(network->dns[1]), "%s", "1.1.1.1");
	network->dns_count = 2;
	(void)snprintf(network->state.wired, sizeof(network->state.wired), "%s", "em0");
	network->state.connected = 1;
	network->state.kind = KL_NETWORK_WIRED;
	(void)snprintf(network->state.interface, sizeof(network->state.interface), "%s", "em0");

	/* The activity of the last two minutes. */
	for (index = 0; index < SE_USAGE_SAMPLES; index++) {
		network->received[index] = 400000U + (index * 37U % 23U) * 60000U + (index > 70U && index < 90U ? 1500000U : 0U);
		network->sent[index] = 80000U + (index * 11U % 17U) * 20000U;
	}
	network->usage_count = SE_USAGE_SAMPLES;
	network->usage_next = 0;
	network->received_total = 1200000000ULL;
	network->sent_total = 320000000ULL;

	/* No radio and no cable (ws177-p007). */
	if (strcmp(scenario, "nonet") == 0) {
		network->links[1].running = 0;
		network->links[1].address[0] = '\0';
		network->state.connected = 0;
		network->state.kind = KL_NETWORK_NONE;
		network->state.wifi = KL_WIFI_ABSENT;
		return;
	}

	/* The radio. */
	if (strcmp(scenario, "absent") == 0) {
		network->state.wifi = KL_WIFI_ABSENT;
		return;
	}
	if (strcmp(scenario, "wired") == 0) {
		network->state.wifi = KL_WIFI_OFF;
		return;
	}

	/* The cable out of the wired interface, which stays up (BUG-213): no link, no address, no speed. */
	if (strcmp(scenario, "nocable") == 0) {
		network->links[1].running = 0;
		network->links[1].address[0] = '\0';
		network->links[1].netmask[0] = '\0';
		network->links[1].router[0] = '\0';
		network->links[1].link_mbps = 0U;
		network->state.connected = 0;
		network->state.kind = KL_NETWORK_NONE;
		network->state.interface[0] = '\0';
		network->state.wired[0] = '\0';
		network->state.wifi = KL_WIFI_OFF;
		return;
	}

	/* On a Wi-Fi network: the radio, its scan and the saved keys. */
	host_link(network, "wlan0", "192.168.1.24", 1, 50000000ULL, 9000000ULL);
	network->state.wifi = KL_WIFI_CONNECTED;
	network->state.kind = KL_NETWORK_WIFI;
	(void)snprintf(network->state.interface, sizeof(network->state.interface), "%s", "wlan0");
	(void)snprintf(network->state.wifi_interface, sizeof(network->state.wifi_interface), "%s", "wlan0");
	(void)snprintf(network->state.ssid, sizeof(network->state.ssid), "%s", "Kei Lab");
	host_ap(network, "Kei Lab", -48, 1);
	host_ap(network, "Cafe Guest", -63, 1);
	host_ap(network, "OSC Venue", -70, 1);
	host_ap(network, "Neighbor 5G", -79, 1);
	host_ap(network, "Library Free", -82, 0);
	(void)snprintf(network->saved[0], sizeof(network->saved[0]), "%s", "Kei Lab");
	(void)snprintf(network->saved[1], sizeof(network->saved[1]), "%s", "Cafe Guest");
	network->saved_count = 2;

	/* The last join refused (ws177-p007): the Wi-Fi page's message. */
	if (strcmp(scenario, "joinfail") == 0) {
		(void)snprintf(network->message, sizeof(network->message), "%s", "Could not join Cafe Guest: the key is wrong.");
		network->message_bad = 1;
	}
}

/* The backend's calls, printed; the made-up daemon answers at once. */
void
se_network_open(
	struct se_app *app)
{
	(void)app;
}

void
se_network_poll(
	struct se_app *app,
	uint64_t now)
{
	(void)app;
	(void)now;
}

int
se_network_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	(void)app;
	(void)request;
	(void)error;
	return 0;
}

int
se_network_wait(
	struct se_app *app)
{
	(void)app;
	return -1;
}

void
se_network_close(
	struct se_app *app)
{
	se_field_clear(&app->network.key);
}

void
se_network_wifi(
	struct se_app *app,
	int on)
{
	printf("NETWORK wifi on=%d\n", on);
	app->network.state.wifi = on != 0 ? KL_WIFI_SEARCHING : KL_WIFI_OFF;
}

/* The switch shows the state here (the made-up network answers at once). */
int
se_network_wifi_on(
	const struct se_network *network)
{
	if (network->state.wifi == KL_WIFI_OFF || network->state.wifi == KL_WIFI_ABSENT)
		return 0;
	return 1;
}

void
se_network_join(
	struct se_app *app,
	const char *ssid)
{
	printf("NETWORK join ssid=%s\n", ssid);
	(void)snprintf(app->network.join_ssid, sizeof(app->network.join_ssid), "%s", ssid);
	app->network.join_step = SE_JOIN_CONNECT;
	(void)snprintf(app->network.message, sizeof(app->network.message), "Connecting to %s...", ssid);
	app->network.message_bad = 0;
}

void
se_network_join_key(
	struct se_app *app,
	const char *ssid,
	const char *key)
{
	printf("NETWORK join-key ssid=%s key-length=%u\n", ssid, (unsigned)strlen(key));
	(void)snprintf(app->network.join_ssid, sizeof(app->network.join_ssid), "%s", ssid);
	app->network.join_step = SE_JOIN_KEY;
	(void)snprintf(app->network.message, sizeof(app->network.message), "Connecting to %s...", ssid);
	app->network.message_bad = 0;
}

void
se_network_disconnect(
	struct se_app *app)
{
	printf("NETWORK disconnect\n");
	app->network.state.wifi = KL_WIFI_DISCONNECTED;
	app->network.state.ssid[0] = '\0';
}

/* Prints a wired interface's configuration asked for (ws089-p022), as network.c would send it, and answers it at once. */
int
se_network_configure_wired(
	struct se_app *app,
	const struct kl_network_wired_config *config)
{
	printf("ZSETTINGS NETWORK wired interface=%s mode=%u address=%s netmask=%s router=%s dns=%s,%s\n", config->interface, config->mode, config->address, config->netmask, config->router, config->dns[0], config->dns[1]);
	app->network.wired_request = 1U;
	return 0;
}
