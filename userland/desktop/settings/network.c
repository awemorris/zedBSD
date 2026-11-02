/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network's backend of Settings (ws089-p003, WS131 p011): the
 * compositor's network through libkeiland's kl_system_* (system.c opens
 * it) -- the daemon's state and scans as the compositor tells them, the
 * interfaces, the DNS servers and the saved networks as its details, asked
 * for once a second -- and the requests the network pages make: the Wi-Fi
 * switch, a join, a disconnect, and a join with a new key (the compositor
 * saves the key, tells the daemon and joins, answering once).
 *
 * There is no Scan button (ws089-p021, the user's request of 2026-10-04):
 * while a page that lists the networks around is shown (Network, Wi-Fi),
 * Settings asks the compositor to keep the radios scanning
 * (kl_system_network_set_scanning) and the list follows each new scan; the
 * scan the compositor already has is listed at once.  Leaving those pages
 * or closing the window asks no longer.  The compositor counts every
 * window that asks, its own Wi-Fi menu included, so two Settings windows,
 * or Settings and the menu, keep the scans going until the last one stops.
 * An asking holds a minute in the compositor, so Settings asks again every
 * NETWORK_SCANNING_RENEW_MS while it shows the list (a Settings that hangs
 * stops keeping the radios busy).
 *
 * One network request is out at a time, the system bar's included.  A
 * switch, a disconnect or a join asked for while Settings' own request is
 * out waits in one slot and is sent when that one is answered, rather
 * than being refused (ws089-p012 C1, the system bar's way since
 * ws005-p019); one the compositor answered busy (the system bar's request
 * was out) waits there a moment and is sent again.  The asking for scans is
 * no request and never takes the slot.
 *
 * Nothing here waits: the main loop calls se_network_poll every round, and
 * the answers arrive through system.c.
 *
 * The Wi-Fi switch shows the position asked at once (BUG-183), and a new
 * key moves from its form to the join when it is confirmed, so the form
 * closes while the join is under way and opens again, empty, when the key
 * did not work (BUG-186); the key is kept until the join is answered (a
 * busy answer sends it again) and wiped then.  Settings never speaks networkd's
 * protocol, nor reads the network's files, itself (plan/ws089/design.md
 * section 6, plan/ws131/design.md section 4).
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How often the details (the interfaces and the activity, the DNS servers, the saved networks) are asked for, in milliseconds. */
#define NETWORK_DETAILS_MS	1000U

/* How long a request the compositor answered busy waits before it is sent again, in milliseconds. */
#define NETWORK_RETRY_MS	500U

/* How often the asking for scans is renewed while a list of networks is shown (the compositor drops it after a minute), in milliseconds. */
#define NETWORK_SCANNING_RENEW_MS	30000U

/* How long the switch keeps the position asked after a successful answer, for the state to agree (BUG-183), in milliseconds. */
#define NETWORK_SWITCH_HOLD_MS	4000U

/* How often the main loop polls while a network page is shown or a request is outstanding, in milliseconds. */
#define NETWORK_POLL_MS		250

static int network_take_details(struct se_app *app, uint64_t now);
static void network_outcome(struct se_app *app, int error);
static void network_join_outcome(struct se_app *app, unsigned request, int error);
static void network_ask(struct se_app *app, unsigned request, const char *ssid, unsigned step);
static void network_send_waiting(struct se_app *app);
static void network_send(struct se_app *app, unsigned request, const char *ssid);
static void network_message(struct se_app *app, int bad, const char *format, const char *ssid);
static int network_page_shown(const struct se_app *app);
static int network_page_lists(const struct se_app *app);
static int network_seen(const struct se_app *app);
static int network_state_same(const struct kl_network_state *old_state, const struct kl_network_state *new_state);
static void network_scanning(struct se_app *app, int on);

/*
 * Starts following the network, when the desktop offers it: the state and
 * the scan the compositor told, and the details asked for.
 */
void
se_network_open(
	struct se_app *app)
{
	struct se_network *network;
	unsigned capabilities;
	size_t count;

	/* Without the desktop's network the pages say so. */
	network = &app->network;
	network->request = SE_NETWORK_NONE;
	network->pending_request = SE_NETWORK_NONE;
	if (app->system == NULL) {
		se_log("NETWORK none");
		return;
	}

	/* A desktop whose system has no network says so too. */
	capabilities = kl_system_capabilities(app->system);
	if ((capabilities & KL_SYSTEM_HAS_NETWORK) == 0U) {
		se_log("NETWORK none capabilities=0x%x", capabilities);
		return;
	}

	/* The network is followed from now on. */
	network->live = 1;

	/*
	 * The state the compositor told when the system opened, logged as a
	 * change is: it is the first state, which the tests wait for, and no
	 * change follows while it holds (WS131 p011, T2-021).
	 */
	kl_system_network_get_state(app->system, &network->state);
	se_log("NETWORK state reachable=%u connected=%u kind=%u interface=%s wifi=%u ssid=%s", network->state.reachable, network->state.connected, network->state.kind, network->state.interface, network->state.wifi, network->state.ssid);

	/* The scan the compositor told when the system opened. */
	count = kl_system_network_get_scan(app->system, network->scan, SE_NETWORK_SCAN);
	if (count > SE_NETWORK_SCAN)
		count = SE_NETWORK_SCAN;
	network->scan_count = count;

	/* The details, asked for now (the log line "NETWORK open" comes with them). */
	network->details_at = app->now;
	(void)kl_system_network_query_details(app->system, &network->details_id);
}

/*
 * Follows what the compositor told; asks for the details now and then, a
 * request that waited for a busy network, and a scan when a Wi-Fi list is
 * shown and the last one is old.
 */
void
se_network_poll(
	struct se_app *app,
	uint64_t now)
{
	struct se_network *network;
	struct kl_network_state state;
	size_t count;
	int addresses;
	int agrees;
	int shown;
	int seen;
	int lists;
	int same;
	int error;

	/* Nothing to follow without the desktop's network. */
	network = &app->network;
	if (network->live == 0)
		return;
	shown = network_page_shown(app);
	seen = network_seen(app);

	/* A new state, drawn when it differs from the last and something on the screen shows it. */
	if ((app->system_changed & KL_SYSTEM_CHANGED_NETWORK) != 0U) {
		kl_system_network_get_state(app->system, &state);
		same = network_state_same(&network->state, &state);
		network->state = state;
		se_log("NETWORK state reachable=%u connected=%u kind=%u interface=%s wifi=%u ssid=%s", network->state.reachable, network->state.connected, network->state.kind, network->state.interface, network->state.wifi, network->state.ssid);
		if (same == 0 && seen != 0)
			app->dirty = 1;
	}

	/* The switch shows the state again once it agrees with what was asked, or a while after the answer (BUG-183). */
	if (network->wifi_wanted != 0) {
		agrees = 0;
		if ((network->wifi_wanted == 2) == (network->state.wifi != KL_WIFI_OFF && network->state.wifi != KL_WIFI_ABSENT))
			agrees = 1;
		if (network->wifi_until != 0U && now >= network->wifi_until)
			agrees = 1;
		if (agrees != 0) {
			network->wifi_wanted = 0;
			network->wifi_until = 0U;
			app->dirty = 1;
			se_log("NETWORK switch settled wifi=%u", network->state.wifi);
		}
	}

	/* A new scan (the compositor's own Wi-Fi menu may ask for them), drawn while a page lists the networks around. */
	lists = network_page_lists(app);
	if ((app->system_changed & KL_SYSTEM_CHANGED_SCAN) != 0U) {
		count = kl_system_network_get_scan(app->system, network->scan, SE_NETWORK_SCAN);
		if (count > SE_NETWORK_SCAN)
			count = SE_NETWORK_SCAN;
		network->scan_count = count;
		network->scan_received = 1;
		se_log("NETWORK scan count=%lu", (unsigned long)network->scan_count);
		if (lists != 0)
			app->dirty = 1;
	}

	/* New details: a network page is drawn again (its activity moves), the other views only when an address changed. */
	if ((app->system_changed & KL_SYSTEM_CHANGED_DETAILS) != 0U) {
		addresses = network_take_details(app, now);
		if (shown != 0)
			app->dirty = 1;
		if (addresses != 0 && seen != 0)
			app->dirty = 1;
	}

	/* The details again, once a second, while something on the screen shows them. */
	if (seen != 0 && network->details_id == 0U && now - network->details_at >= NETWORK_DETAILS_MS) {
		network->details_at = now;
		error = kl_system_network_query_details(app->system, &network->details_id);
		if (error != 0)
			network->details_id = 0U;
	}

	/* A request that waited for a busy network, when its moment came. */
	if (network->request == SE_NETWORK_NONE && network->pending_request != SE_NETWORK_NONE && now >= network->retry_at)
		network_send_waiting(app);

	/* Scans asked for while a page lists the networks around, and no longer when none does. */
	network_scanning(app, lists);
}

/*
 * Takes an answered request when it is the network's: the details', or the
 * request outstanding's.  Returns 1 when it was, 0 otherwise.
 */
int
se_network_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_network *network;

	/* The details' answer: they came before it. */
	network = &app->network;
	if (request != 0U && request == network->details_id) {
		network->details_id = 0U;
		return 1;
	}

	/* A wired interface's configuration (ws089-p022): the editor says how it went, and the details are asked again. */
	if (request != 0U && request == network->wired_request) {
		network->wired_request = 0U;
		se_wired_outcome(app, error);
		network->details_at = 0U;
		return 1;
	}

	/* Another request's. */
	if (network->request == SE_NETWORK_NONE || request != network->request_id)
		return 0;

	/* The outstanding request's outcome, then the request that waited in the slot. */
	network_outcome(app, error);
	if (network->request == SE_NETWORK_NONE && app->now >= network->retry_at)
		network_send_waiting(app);

	/* Succeeded: the answer was the network's. */
	return 1;
}

/*
 * Reports how long the main loop may sleep before the network wants a
 * poll: a short while while a network page is shown, a request is
 * outstanding or one waits; until the details are asked again while
 * another view shows an address (Home); else -1 (nothing is due).
 */
int
se_network_wait(
	struct se_app *app)
{
	struct se_network *network;
	uint64_t due;
	int shown;
	int seen;

	/* Without the desktop's network nothing is due. */
	network = &app->network;
	if (network->live == 0)
		return -1;

	/* An answer awaited, a request waiting, or a page that shows the network. */
	shown = network_page_shown(app);
	if (shown != 0 || network->request != SE_NETWORK_NONE || network->pending_request != SE_NETWORK_NONE)
		return NETWORK_POLL_MS;

	/* Nothing on the screen shows the network, or the details' answer, which wakes the loop, is awaited. */
	seen = network_seen(app);
	if (seen == 0 || network->details_id != 0U)
		return -1;

	/* The details are asked again a second after the last asking. */
	due = network->details_at + NETWORK_DETAILS_MS;
	if (due <= app->now)
		return 0;

	/* Reports the wait until then. */
	return (int)(due - app->now);
}

/*
 * Stops following the network and wipes a key being typed.
 */
void
se_network_close(
	struct se_app *app)
{
	/* The scans are asked for no longer, then the keys and the network go; a request waiting in the slot is dropped with them. */
	network_scanning(app, 0);
	se_field_clear(&app->network.key);
	se_field_clear(&app->network.join_key);
	app->network.live = 0;
	app->network.pending_request = SE_NETWORK_NONE;
}

/*
 * Turns the Wi-Fi on or off.  The switch shows the new position at once and
 * keeps it until the state agrees, the request fails, or a few seconds
 * after a successful answer (BUG-183: turning the radio on takes a second).
 */
void
se_network_wifi(
	struct se_app *app,
	int on)
{
	/* What the switch shows from now on. */
	app->network.wifi_wanted = 1;
	if (on != 0)
		app->network.wifi_wanted = 2;
	app->network.wifi_until = 0U;
	app->dirty = 1;
	se_log("NETWORK switch shows on=%d", on != 0);

	/* The switch's request, sent or kept until the outstanding one is answered. */
	if (on != 0) {
		network_ask(app, KL_NETWORK_WIFI_ON, NULL, SE_JOIN_NONE);
	} else {
		network_ask(app, KL_NETWORK_WIFI_OFF, NULL, SE_JOIN_NONE);
	}
}

/*
 * Tells whether the Wi-Fi switch shows on: the position asked while it
 * waits for the state, else the state.
 */
int
se_network_wifi_on(
	const struct se_network *network)
{
	/* Asked on, or off. */
	if (network->wifi_wanted == 2)
		return 1;
	if (network->wifi_wanted == 1)
		return 0;

	/* The state: on unless off or without a radio. */
	if (network->state.wifi == KL_WIFI_OFF || network->state.wifi == KL_WIFI_ABSENT)
		return 0;
	return 1;
}

/*
 * Joins a network whose key is saved.
 */
void
se_network_join(
	struct se_app *app,
	const char *ssid)
{
	struct se_network *network;

	/* The join, sent or kept until the outstanding request is answered. */
	network = &app->network;
	network_ask(app, KL_NETWORK_JOIN, ssid, SE_JOIN_CONNECT);

	/* A join sent or waiting says so (a refusal has said why instead). */
	if (network->request == KL_NETWORK_JOIN || network->pending_request == KL_NETWORK_JOIN)
		network_message(app, 0, "Connecting to %s...", ssid);
}

/*
 * Joins a network with a key just typed (the key form's): the compositor
 * saves it in the user's store, tells the daemon and joins, answering once.
 */
void
se_network_join_key(
	struct se_app *app,
	const char *ssid,
	const char *key)
{
	struct se_network *network;
	char name[KL_NETWORK_SSID_MAX];
	size_t length;

	/* A key of a WPA key's length (the compositor checks it too). */
	network = &app->network;
	length = strlen(key);
	if (length < KL_NETWORK_KEY_MIN || length > KL_NETWORK_KEY_MAX) {
		network_message(app, 1, "The key of %s must be 8 to 63 characters.", ssid);
		return;
	}

	/* The name is copied first: the form's own name and key (which the caller may pass) are wiped below (T1-150). */
	(void)snprintf(name, sizeof(name), "%s", ssid);
	ssid = name;

	/*
	 * The key goes from the form to the join (BUG-186): the form closes as
	 * the join starts, and the key is kept only until it is sent.
	 */
	se_field_clear(&network->join_key);
	(void)snprintf(network->join_key.text, sizeof(network->join_key.text), "%s", key);
	network->join_key.length = length;
	network->key_ssid[0] = '\0';
	se_field_clear(&network->key);
	se_log("NETWORK key-form closed ssid=%s", ssid);

	/* The request, sent or kept until the outstanding one is answered. */
	network_ask(app, SE_NETWORK_SAVE_KEY, ssid, SE_JOIN_KEY);

	/* A join under way or waiting says so (a refusal has said why instead). */
	if (network->request == SE_NETWORK_SAVE_KEY || network->pending_request == SE_NETWORK_SAVE_KEY)
		network_message(app, 0, "Connecting to %s...", ssid);
}

/*
 * Asks for a wired interface's configuration (ws089-p022); its answer comes
 * to se_wired_outcome.  It goes beside the Wi-Fi's requests: the compositor
 * answers busy while another network request is out.  Returns 0 when
 * asked, EBUSY while the last is not answered, or the errno value of
 * asking (ENOTSUP from an older desktop).
 */
int
se_network_configure_wired(
	struct se_app *app,
	const struct kl_network_wired_config *config)
{
	struct se_network *network;
	int error;

	/* One at a time, on a desktop that offers the network. */
	network = &app->network;
	if (network->live == 0 || app->system == NULL)
		return ENOTSUP;
	if (network->wired_request != 0U)
		return EBUSY;

	/* Asked; the answer names this request. */
	error = kl_system_network_configure_wired(app->system, config, &network->wired_request);
	se_log("NETWORK wired interface=%s mode=%u address=%s netmask=%s router=%s dns=%s,%s error=%d", config->interface, config->mode, config->address, config->netmask, config->router, config->dns[0], config->dns[1], error);
	if (error != 0)
		network->wired_request = 0U;

	/* Reports how the asking went. */
	return error;
}

/*
 * Leaves the Wi-Fi network the machine is on.
 */
void
se_network_disconnect(
	struct se_app *app)
{
	/* The disconnect's request, sent or kept until the outstanding one is answered. */
	network_ask(app, KL_NETWORK_DISCONNECT, NULL, SE_JOIN_NONE);
}

/*
 * Takes the details the compositor sent: the interfaces, with a second of
 * activity recorded (the bytes of every interface but the loopback), the
 * DNS servers and the saved networks.  Returns 1 when the interfaces or
 * their addresses differ from the last ones (Home shows an address).
 */
static int
network_take_details(
	struct se_app *app,
	uint64_t now)
{
	struct se_network *network;
	char addresses[SE_NETWORK_LINKS][KL_NETWORK_ADDRESS_MAX];
	unsigned modes[SE_NETWORK_LINKS];
	unsigned inet6[SE_NETWORK_LINKS];
	const char *first6;
	const char *router6;
	size_t old_count;
	int differs;
	int moved;
	uint64_t received;
	uint64_t sent;
	uint64_t elapsed;
	uint64_t received_rate;
	uint64_t sent_rate;
	size_t count;
	size_t index;

	/* The addresses of the last details, to tell whether they moved. */
	network = &app->network;
	old_count = network->link_count;
	for (index = 0; index < old_count; index++) {
		(void)snprintf(addresses[index], sizeof(addresses[index]), "%s", network->links[index].address);
		modes[index] = network->links[index].wired_mode;
		inet6[index] = network->links[index].inet6_count;
	}

	/* The interfaces, as many as are kept. */
	count = kl_system_network_get_links(app->system, network->links, SE_NETWORK_LINKS);
	if (count > SE_NETWORK_LINKS)
		count = SE_NETWORK_LINKS;
	network->link_count = count;

	/* The servers and the saved networks. */
	network->dns_count = kl_system_network_get_dns(app->system, network->dns, SE_NETWORK_DNS);
	network->saved_count = kl_system_network_get_saved(app->system, network->saved, SE_NETWORK_SAVED);

	/* The first details: the log line the tests read. */
	if (network->details_known == 0) {
		network->details_known = 1;
		se_log("NETWORK open links=%lu dns=%lu saved=%lu", (unsigned long)network->link_count, (unsigned long)network->dns_count, (unsigned long)network->saved_count);
	}

	/* Interfaces that came or went count as moved. */
	moved = 0;
	if (count != old_count)
		moved = 1;

	/* So does an address that changed. */
	for (index = 0; moved == 0 && index < count; index++) {
		differs = strcmp(addresses[index], network->links[index].address);
		if (differs != 0)
			moved = 1;
	}

	/* The bytes of every interface but the loopback. */
	received = 0;
	sent = 0;
	for (index = 0; index < count; index++) {
		if (network->links[index].loopback != 0)
			continue;
		received += network->links[index].received_bytes;
		sent += network->links[index].sent_bytes;
	}

	/* The rates since the last sample (none for the first, or after a counter went back). */
	elapsed = now - network->sampled_at;
	if (network->sampled_at != 0U &&
	    elapsed > 0U &&
	    received >= network->received_total &&
	    sent >= network->sent_total) {
		received_rate = (received - network->received_total) * 1000U / elapsed;
		sent_rate = (sent - network->sent_total) * 1000U / elapsed;
		if (received_rate > 0xffffffffU)
			received_rate = 0xffffffffU;
		if (sent_rate > 0xffffffffU)
			sent_rate = 0xffffffffU;
		network->received[network->usage_next] = (uint32_t)received_rate;
		network->sent[network->usage_next] = (uint32_t)sent_rate;
		network->usage_next = (network->usage_next + 1U) % SE_USAGE_SAMPLES;
		if (network->usage_count < SE_USAGE_SAMPLES)
			network->usage_count++;
	}

	/* The totals and the time of this sample. */
	network->received_total = received;
	network->sent_total = sent;
	network->sampled_at = now;

	/* The wired interfaces' configuration when it or their count of IPv6 addresses changed, the log line the tests read (ws089-p022). */
	for (index = 0; index < count; index++) {
		if (network->links[index].wired_mode == KL_WIRED_UNKNOWN)
			continue;
		if (moved == 0 && index < old_count && modes[index] == network->links[index].wired_mode && inet6[index] == network->links[index].inet6_count)
			continue;
		first6 = "-";
		if (network->links[index].inet6_count != 0U)
			first6 = network->links[index].inet6[0];
		router6 = "-";
		if (network->links[index].router6[0] != '\0')
			router6 = network->links[index].router6;
		se_log("NETWORK wired-link name=%s mode=%u address=%s router=%s inet6=%u first6=%s router6=%s", network->links[index].name, network->links[index].wired_mode, network->links[index].address, network->links[index].router, network->links[index].inet6_count, first6, router6);
	}

	/* Succeeded: whether the interfaces moved. */
	return moved;
}

/*
 * Takes the outstanding request's outcome: one answered busy waits to be
 * sent again, a join's says how it went, anything else that failed says
 * so.
 */
static void
network_outcome(
	struct se_app *app,
	int error)
{
	struct se_network *network;
	unsigned request;

	/* The request that finished and how. */
	network = &app->network;
	request = network->request;
	network->request = SE_NETWORK_NONE;
	network->request_id = 0U;
	se_log("NETWORK done request=%u errno=%d", request, error);
	app->dirty = 1;

	/* The network was busy with the system bar's request: it waits a moment in the slot, unless something newer waits there. */
	if (error == EBUSY) {
		if (network->pending_request == SE_NETWORK_NONE) {
			network->pending_request = request;
			network->pending_step = network->join_step;
			(void)snprintf(network->pending_ssid, sizeof(network->pending_ssid), "%s", network->join_ssid);
		}

		/* Nothing is out now; the slot's request goes again at retry_at. */
		network->join_step = SE_JOIN_NONE;
		network->retry_at = app->now + NETWORK_RETRY_MS;
		se_log("NETWORK request=%u waits busy=system", request);
		return;
	}

	/* A join's outcome. */
	if (request == KL_NETWORK_JOIN || request == SE_NETWORK_SAVE_KEY) {
		network_join_outcome(app, request, error);
		return;
	}

	/* The switch's answer: a failure puts it back at once, a success leaves the state a few seconds to agree (BUG-183). */
	if (request == KL_NETWORK_WIFI_ON || request == KL_NETWORK_WIFI_OFF) {
		network->wifi_until = app->now + NETWORK_SWITCH_HOLD_MS;
		if (error != 0)
			network->wifi_wanted = 0;
	}

	/* Anything else that failed says so. */
	if (error != 0) {
		network->join_step = SE_JOIN_NONE;
		network_message(app, 1, "The network could not do that (%s).", strerror(error));
		return;
	}

	/* The switch's and the disconnect's success need no words; the state shows them. */
	if (request == KL_NETWORK_WIFI_OFF || request == KL_NETWORK_DISCONNECT)
		network->message[0] = '\0';
}

/* Says how a join (of a saved network, or with a new key) went; the key form closes when it worked. */
static void
network_join_outcome(
	struct se_app *app,
	unsigned request,
	int error)
{
	struct se_network *network;
	char reason[SE_MESSAGE];
	int differs;

	/* A key that was saved, or not (the log lines the tests read, never the key). */
	network = &app->network;
	network->join_step = SE_JOIN_NONE;
	if (request == SE_NETWORK_SAVE_KEY) {
		if (error == EINVAL || error == EIO || error == ENODEV || error == ENOTSUP) {
			se_log("NETWORK save-key failed errno=%d", error);
		} else {
			se_log("NETWORK save-key ok");
		}
	}

	/*
	 * A new key that did not work opens the network's key form again,
	 * empty, with the reason under it (BUG-186); the key is gone.
	 */
	if (request == SE_NETWORK_SAVE_KEY)
		se_field_clear(&network->join_key);
	if (error != 0 && request == SE_NETWORK_SAVE_KEY) {
		(void)snprintf(network->key_ssid, sizeof(network->key_ssid), "%s", network->join_ssid);
		se_field_clear(&network->key);
		network->key_shown = 0;
		network->key_reveal = 1;
		se_log("NETWORK key-form again ssid=%s errno=%d", network->join_ssid, error);
	}

	/* Each outcome in words. */
	if (error == 0) {
		differs = strcmp(network->key_ssid, network->join_ssid);
		if (differs == 0) {
			network->key_ssid[0] = '\0';
			se_field_clear(&network->key);
		}

		/* Joined. */
		network_message(app, 0, "Connected to %s.", network->join_ssid);
	} else if (error == EINVAL && request == SE_NETWORK_SAVE_KEY) {
		network_message(app, 1, "The key of %s must be 8 to 63 characters.", network->join_ssid);
	} else if (error == EIO && request == SE_NETWORK_SAVE_KEY) {
		network_message(app, 1, "The key of %s could not be saved, or the network not joined.", network->join_ssid);
	} else if (error == ENOENT) {
		network_message(app, 1, "%s has no saved key.", network->join_ssid);
	} else if (error == EACCES) {
		/* The network refused the key during its handshake (ws005-p020, q631). */
		network_message(app, 1, "%s did not accept the key. Check the key and try again.", network->join_ssid);
	} else if (error == ENETUNREACH) {
		/* No radio sees the network. */
		network_message(app, 1, "%s is not in reach.", network->join_ssid);
	} else if (error == EPERM && network->state.wifi == KL_WIFI_OFF) {
		/* networkd refuses a join while Wi-Fi is off. */
		network_message(app, 1, "Wi-Fi is off. Turn it on to join %s.", network->join_ssid);
	} else if (error == EPERM) {
		/* Only root and the network group may control Wi-Fi (2026-10-02, ws005-p019). */
		network_message(app, 1, "This account may not control Wi-Fi, so it cannot join %s. Ask an administrator to add it to the network group.", network->join_ssid);
	} else {
		/* Any other failure names its reason. */
		(void)snprintf(reason, sizeof(reason), "Could not join %%s (%s).", strerror(error));
		network_message(app, 1, reason, network->join_ssid);
	}
}

/*
 * Asks for something: sent now, or kept in the slot while another request
 * is outstanding (a later ask replaces what waited).
 * step is the join the request starts (SE_JOIN_NONE for a request that is
 * no join), and ssid names the join's network.
 */
static void
network_ask(
	struct se_app *app,
	unsigned request,
	const char *ssid,
	unsigned step)
{
	struct se_network *network;

	/* Without the desktop's network nothing is sent or kept. */
	network = &app->network;
	if (network->live == 0) {
		network_message(app, 1, "%s", "Network settings are not available on this desktop.");
		return;
	}

	/* Another request is out: anything else waits in the slot for its answer, in place of what waited before. */
	if (network->request != SE_NETWORK_NONE) {
		network->pending_request = request;
		network->pending_step = step;
		network->pending_ssid[0] = '\0';
		if (ssid != NULL)
			(void)snprintf(network->pending_ssid, sizeof(network->pending_ssid), "%s", ssid);
		se_log("NETWORK request=%u waits busy=%u", request, network->request);
		app->dirty = 1;
		return;
	}

	/* A join carries its network through its answer. */
	network->join_step = step;
	if (step != SE_JOIN_NONE && ssid != NULL)
		(void)snprintf(network->join_ssid, sizeof(network->join_ssid), "%s", ssid);

	/* Only a join names its network. */
	if (request == KL_NETWORK_JOIN || request == SE_NETWORK_SAVE_KEY) {
		network_send(app, request, ssid);
	} else {
		network_send(app, request, NULL);
	}
}

/* Sends the request that waited in the slot, once no request is outstanding. */
static void
network_send_waiting(
	struct se_app *app)
{
	struct se_network *network;
	char ssid[KL_NETWORK_SSID_MAX];
	unsigned request;
	unsigned step;

	/* Nothing waits, or the outstanding request still has to be answered. */
	network = &app->network;
	if (network->pending_request == SE_NETWORK_NONE)
		return;
	if (network->request != SE_NETWORK_NONE)
		return;

	/* The slot is emptied before the request goes (the ask may keep nothing again). */
	request = network->pending_request;
	step = network->pending_step;
	(void)snprintf(ssid, sizeof(ssid), "%s", network->pending_ssid);
	network->pending_request = SE_NETWORK_NONE;
	se_log("NETWORK request=%u from-slot", request);

	/* The request, as if it were asked now. */
	if (ssid[0] != '\0') {
		network_ask(app, request, ssid, step);
	} else {
		network_ask(app, request, NULL, step);
	}
}

/* Sends a request to the compositor, unless one is outstanding; a refusal is shown. */
static void
network_send(
	struct se_app *app,
	unsigned request,
	const char *ssid)
{
	struct se_network *network;
	int error;

	/* One request at a time. */
	network = &app->network;
	if (network->request != SE_NETWORK_NONE) {
		network_message(app, 1, "%s", "Wait for the network to answer, then try again.");
		return;
	}

	/* A key with its network (the join's, taken from the form when it was confirmed), or a request of the daemon's. */
	if (request == SE_NETWORK_SAVE_KEY) {
		error = kl_system_network_save_key(app->system, ssid, network->join_key.text, &network->request_id);
	} else {
		error = kl_system_network_request(app->system, request, ssid, &network->request_id);
	}

	/* A request the library refused is said at once. */
	if (error != 0) {
		se_log("NETWORK request=%u refused errno=%d", request, error);
		se_field_clear(&network->join_key);
		network->join_step = SE_JOIN_NONE;
		network_message(app, 1, "The network could not do that (%s).", strerror(error));
		return;
	}

	/* Outstanding until its answer. */
	network->request = request;
	se_log("NETWORK request=%u sent", request);
	app->dirty = 1;
}

/* Sets the message the network pages show, with an SSID or another text put in (bad: shown in red). */
static void
network_message(
	struct se_app *app,
	int bad,
	const char *format,
	const char *ssid)
{
	/* The message, and its colour. */
	(void)snprintf(app->network.message, sizeof(app->network.message), format, ssid);
	app->network.message_bad = bad;
	app->dirty = 1;
	se_log("NETWORK message bad=%d text=%s", bad, app->network.message);
}

/* Tells whether a page that shows the network is shown. */
static int
network_page_shown(
	const struct se_app *app)
{
	/* Network, Wi-Fi and Ethernet show it. */
	if (app->page == SE_PAGE_NETWORK)
		return 1;
	if (app->page == SE_PAGE_WIFI)
		return 1;
	if (app->page == SE_PAGE_ETHERNET)
		return 1;

	/* No other page does. */
	return 0;
}

/*
 * Tells whether anything on the screen shows the network's state or
 * addresses: a network page, Home's tiles, the Sharing page's address,
 * or the Welcome.
 */
static int
network_seen(
	const struct se_app *app)
{
	int shown;

	/* The network's own pages. */
	shown = network_page_shown(app);
	if (shown != 0)
		return 1;

	/* Home and Sharing show an address, and the Welcome the connection. */
	if (app->page == SE_PAGE_HOME)
		return 1;
	if (app->page == SE_PAGE_SHARING)
		return 1;
	if (app->welcome != 0)
		return 1;

	/* No other view does. */
	return 0;
}

/* Tells whether two states of the network say the same (a report that changes nothing needs no frame). */
static int
network_state_same(
	const struct kl_network_state *old_state,
	const struct kl_network_state *new_state)
{
	int differs;

	/* The numbers. */
	if (old_state->reachable != new_state->reachable)
		return 0;
	if (old_state->connected != new_state->connected)
		return 0;
	if (old_state->kind != new_state->kind)
		return 0;
	if (old_state->wifi != new_state->wifi)
		return 0;

	/* The interfaces' names. */
	differs = strncmp(old_state->interface, new_state->interface, sizeof(old_state->interface));
	if (differs != 0)
		return 0;
	differs = strncmp(old_state->wired, new_state->wired, sizeof(old_state->wired));
	if (differs != 0)
		return 0;
	differs = strncmp(old_state->wifi_interface, new_state->wifi_interface, sizeof(old_state->wifi_interface));
	if (differs != 0)
		return 0;

	/* The network the Wi-Fi is on or joining. */
	differs = strncmp(old_state->ssid, new_state->ssid, sizeof(old_state->ssid));
	if (differs != 0)
		return 0;

	/* Succeeded: nothing differs. */
	return 1;
}

/* Tells whether a page that lists the networks around is shown (Network and Wi-Fi; Ethernet lists none). */
static int
network_page_lists(
	const struct se_app *app)
{
	/* Network and Wi-Fi list them. */
	if (app->page == SE_PAGE_NETWORK)
		return 1;
	if (app->page == SE_PAGE_WIFI)
		return 1;

	/* No other page does. */
	return 0;
}

/*
 * Asks the compositor to keep the radios scanning (on 1), or no longer
 * (on 0), when that differs from what it was last told, and asks again
 * while on when the last asking is NETWORK_SCANNING_RENEW_MS old
 * (ws089-p021: an asking holds a minute).
 */
static void
network_scanning(
	struct se_app *app,
	int on)
{
	struct se_network *network;
	int renew;
	int error;

	/* Nothing to tell without the desktop's network. */
	network = &app->network;
	if (network->live == 0)
		return;

	/* An asking still on that is due to be asked again. */
	renew = 0;
	if (on != 0 && network->scanning != 0 && app->now - network->scanning_at >= NETWORK_SCANNING_RENEW_MS)
		renew = 1;

	/* Told already, and not due. */
	if (on == network->scanning && renew == 0)
		return;

	/* The compositor is told; one that does not know the asking (ENOTSUP) is told the same way again later, harmlessly. */
	error = kl_system_network_set_scanning(app->system, (unsigned)on);
	network->scanning = on;
	network->scanning_at = app->now;
	se_log("NETWORK scanning on=%d renew=%d errno=%d", on, renew, error);
}
