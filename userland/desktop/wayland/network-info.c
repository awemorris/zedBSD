/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network's details for the system bar (ws099-p032, the 2026-10-04
 * user request: "画面右上通知領域のWiFiアイコンをAlt＋クリックすると、IP
 * アドレスや統計情報が出るとうれしいです。").
 *
 * The rows, in order, each left out when its value is not known:
 *   the title (Wi-Fi, Ethernet, or Network when nothing is connected);
 *   Network (the Wi-Fi's SSID); Interface; Status; IPv4 address; Subnet
 *   mask; DNS (up to two servers); MAC address; MTU; Signal (the Wi-Fi's
 *   strength in dBm, from the last scan); Received and Sent (the bytes
 *   since the interface came up, and the rate since the last reading).
 *
 * The interface shown is the one that carries the connection, else the
 * Wi-Fi's, else the wired one that is up, else the first that is not the
 * loopback.  The router, the access point's BSSID, the channel, the radio's
 * rate, the packet and error counts and the time connected are not shown:
 * libkeiland-backend does not read them yet (plan/ws099/phase032).
 */

#include "userland/desktop/wayland/network-info.h"

#include <stdio.h>
#include <string.h>

static const struct kl_backend_network_link *info_link(const struct kwl_network_info_input *input);
static const struct kl_backend_network_link *info_link_named(const struct kwl_network_info_input *input, const char *name);
static unsigned info_add(struct kwl_network_info_row *rows, unsigned count, unsigned capacity, const char *label, const char *value);
static void info_transfer(uint64_t bytes, uint64_t before, uint64_t elapsed_ms, int rated, char *text, size_t size);

/*
 * Works out the rows and returns how many there are, and keeps this
 * reading's sample in previous.
 */
unsigned
kwl_network_info_build(
	const struct kwl_network_info_input *input,
	struct kwl_network_info_sample *previous,
	struct kwl_network_info_row *rows,
	unsigned capacity)
{
	const struct kl_backend_network_state *state;
	const struct kl_backend_network_link *link;
	char value[KWL_NETWORK_INFO_VALUE];
	uint64_t elapsed;
	unsigned count;
	size_t index;
	int same;
	int rated;
	int wifi;

	/* The title: what carries the connection. */
	state = input->state;
	wifi = state->connected && state->kind == KL_BACKEND_NETWORK_WIFI;
	(void)snprintf(value, sizeof(value), "Network");
	if (wifi)
		(void)snprintf(value, sizeof(value), "Wi-Fi");
	if (state->connected && state->kind == KL_BACKEND_NETWORK_WIRED)
		(void)snprintf(value, sizeof(value), "Ethernet");
	count = info_add(rows, 0U, capacity, "", value);

	/* The Wi-Fi's network. */
	if (wifi)
		count = info_add(rows, count, capacity, "Network", state->ssid);

	/* The interface, and whether the connection is up. */
	link = info_link(input);
	if (link != NULL)
		count = info_add(rows, count, capacity, "Interface", link->name);
	(void)snprintf(value, sizeof(value), "Not connected");
	if (state->connected)
		(void)snprintf(value, sizeof(value), "Connected");
	if (!state->reachable)
		(void)snprintf(value, sizeof(value), "Network service not running");
	count = info_add(rows, count, capacity, "Status", value);

	/* The addresses. */
	if (link != NULL) {
		count = info_add(rows, count, capacity, "IPv4 address", link->address);
		count = info_add(rows, count, capacity, "Subnet mask", link->netmask);
	}

	/* The DNS servers, two at most. */
	value[0] = '\0';
	if (input->dns_count > 0U)
		(void)snprintf(value, sizeof(value), "%s", input->dns[0]);
	if (input->dns_count > 1U)
		(void)snprintf(value, sizeof(value), "%s, %s", input->dns[0], input->dns[1]);
	count = info_add(rows, count, capacity, "DNS", value);

	/* The hardware address and the MTU. */
	if (link != NULL) {
		(void)snprintf(value, sizeof(value), "%02x:%02x:%02x:%02x:%02x:%02x", link->hardware[0], link->hardware[1],
			       link->hardware[2], link->hardware[3], link->hardware[4], link->hardware[5]);
		same = memcmp(link->hardware, "\0\0\0\0\0\0", 6U);
		if (same != 0)
			count = info_add(rows, count, capacity, "MAC address", value);
		value[0] = '\0';
		if (link->mtu != 0U)
			(void)snprintf(value, sizeof(value), "%u", link->mtu);
		count = info_add(rows, count, capacity, "MTU", value);
	}

	/* The Wi-Fi's signal, from the last scan of its network. */
	if (wifi) {
		for (index = 0; index < input->scan_count; index++) {
			same = strcmp(input->scan[index].ssid, state->ssid);
			if (same != 0)
				continue;
			(void)snprintf(value, sizeof(value), "%d dBm", input->scan[index].rssi);
			count = info_add(rows, count, capacity, "Signal", value);
			break;
		}
	}

	/* No interface: nothing was carried, and no sample is kept. */
	if (link == NULL) {
		previous->valid = 0U;
		return count;
	}

	/* The bytes carried, with the rates since the last reading of the same interface. */
	rated = 0;
	elapsed = 0;
	same = strcmp(previous->name, link->name);
	if (previous->valid && same == 0 && input->now_ms > previous->ms) {
		rated = 1;
		elapsed = input->now_ms - previous->ms;
	}

	/* The two counts. */
	info_transfer(link->received_bytes, previous->received, elapsed, rated, value, sizeof(value));
	count = info_add(rows, count, capacity, "Received", value);
	info_transfer(link->sent_bytes, previous->sent, elapsed, rated, value, sizeof(value));
	count = info_add(rows, count, capacity, "Sent", value);

	/* This reading, for the next one's rates. */
	previous->valid = 1U;
	previous->ms = input->now_ms;
	(void)snprintf(previous->name, sizeof(previous->name), "%s", link->name);
	previous->received = link->received_bytes;
	previous->sent = link->sent_bytes;

	/* Succeeded: the rows. */
	return count;
}

/* Writes a count of bytes as B, KB, MB or GB. */
void
kwl_network_info_bytes(
	uint64_t bytes,
	char *text,
	size_t size)
{
	/* Under a kilobyte, whole bytes. */
	if (bytes < 1024U) {
		(void)snprintf(text, size, "%u B", (unsigned)bytes);
		return;
	}

	/* Kilobytes, with a decimal. */
	if (bytes < 1024U * 1024U) {
		(void)snprintf(text, size, "%.1f KB", (double)bytes / 1024.0);
		return;
	}

	/* Megabytes. */
	if (bytes < 1024U * 1024U * 1024U) {
		(void)snprintf(text, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
		return;
	}

	/* Succeeded: gigabytes. */
	(void)snprintf(text, size, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

/* Chooses the interface to show: the connection's, the Wi-Fi's, the wired one, or the first not the loopback. */
static const struct kl_backend_network_link *
info_link(
	const struct kwl_network_info_input *input)
{
	const struct kl_backend_network_link *link;
	size_t index;

	/* The interface that carries the connection. */
	link = NULL;
	if (input->state->connected)
		link = info_link_named(input, input->state->interface);
	if (link != NULL)
		return link;

	/* The Wi-Fi's, then the wired one that is up. */
	link = info_link_named(input, input->state->wifi_interface);
	if (link != NULL)
		return link;
	link = info_link_named(input, input->state->wired);
	if (link != NULL)
		return link;

	/* The first that is not the loopback. */
	for (index = 0; index < input->link_count; index++) {
		if (!input->links[index].loopback)
			return &input->links[index];
	}

	/* No interface to show. */
	return NULL;
}

/* Finds an interface by its name; NULL for an empty name or none of that name. */
static const struct kl_backend_network_link *
info_link_named(
	const struct kwl_network_info_input *input,
	const char *name)
{
	size_t index;
	int same;

	/* No name, no interface. */
	if (name[0] == '\0')
		return NULL;

	/* The interface of that name. */
	for (index = 0; index < input->link_count; index++) {
		same = strcmp(input->links[index].name, name);
		if (same == 0)
			return &input->links[index];
	}

	/* None. */
	return NULL;
}

/* Adds a row when its value is known and there is room; returns the new count. */
static unsigned
info_add(
	struct kwl_network_info_row *rows,
	unsigned count,
	unsigned capacity,
	const char *label,
	const char *value)
{
	/* An unknown value, or no room. */
	if (value[0] == '\0' || count >= capacity)
		return count;

	/* The row. */
	(void)snprintf(rows[count].label, sizeof(rows[count].label), "%s", label);
	(void)snprintf(rows[count].value, sizeof(rows[count].value), "%s", value);

	/* Succeeded: one row more. */
	return count + 1U;
}

/*
 * Writes the bytes carried, and with rated the rate since the reading
 * elapsed_ms ago that counted before (a counter that went back, as one
 * started again, shows no rate).
 */
static void
info_transfer(
	uint64_t bytes,
	uint64_t before,
	uint64_t elapsed_ms,
	int rated,
	char *text,
	size_t size)
{
	char total[24];
	char rate[24];
	uint64_t per_second;

	/* The total. */
	kwl_network_info_bytes(bytes, total, sizeof(total));

	/* Without a rate. */
	if (!rated || elapsed_ms == 0U || bytes < before) {
		(void)snprintf(text, size, "%s", total);
		return;
	}

	/* Succeeded: the total and the rate. */
	per_second = (bytes - before) * 1000U / elapsed_ms;
	kwl_network_info_bytes(per_second, rate, sizeof(rate));
	(void)snprintf(text, size, "%s (%s/s)", total, rate);
}
