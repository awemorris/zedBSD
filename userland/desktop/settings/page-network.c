/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network pages (ws089-p003, the user's "network at the centre"):
 *
 *   Network   the connection's state in four tiles, the Wi-Fi networks
 *             around, the wired interface, the DNS servers, and the
 *             network's activity since the window opened;
 *   Wi-Fi     the switch, the network the machine is on, and every
 *             network around, with the key typed for a new one;
 *   Ethernet  each wired interface's addresses and counters.
 *
 * They draw what network.c keeps (struct se_network) and ask it to act.
 * A click on a network joins it when its key is saved, or opens a line
 * under it to type its key; the key is shown as dots unless Show is on.
 * The list follows the scans by itself while it is shown (ws089-p021:
 * there is no Scan button), and the network the machine is on has a
 * Disconnect button on its row, a picture (a cross in a circle).
 */

#include "settings.h"

#include <stdio.h>
#include <string.h>

/* The controls of the network pages (the hit indices of SE_HIT_CONTROL); a network of the scan is its index past NETWORK_AP_FIRST. */
#define NETWORK_WIFI_SWITCH	1
#define NETWORK_DISCONNECT	3
#define NETWORK_KEY_JOIN	4
#define NETWORK_KEY_CANCEL	5
#define NETWORK_KEY_SHOW	6
#define NETWORK_ETHERNET	7
#define NETWORK_WIFI_ALL	8
#define NETWORK_KEY_FIELD	9
#define NETWORK_AP_FIRST	100

/* The space between two cards, and a card's inner margin. */
#define NETWORK_GAP		16
#define NETWORK_PAD		18

/* A card's header (title and subtitle) and a network's row, and the key's line under a row. */
#define NETWORK_HEADER		64
#define NETWORK_ROW		50
#define NETWORK_KEY_LINE	56
#define NETWORK_MESSAGE_LINE	30

/* How many networks the Network page lists before "All Wi-Fi networks". */
#define NETWORK_COMPACT_ROWS	4

/* The room the Disconnect button takes on the row of the network in use: the button (32) and a margin. */
#define NETWORK_DISCONNECT_ROOM	40

/* A tile of the connection's state. */
#define NETWORK_TILE_HEIGHT	78

/* The graph of the activity. */
#define NETWORK_GRAPH_HEIGHT	110
#define NETWORK_GRAPH_POINTS	60

/* The colours of what was received and sent. */
#define NETWORK_COLOR_DOWN	KL_RGB(0x2f7cf6)
#define NETWORK_COLOR_UP	KL_RGB(0x9b6cf0)

/* The text sizes. */
#define NETWORK_TEXT_TITLE	16U
#define NETWORK_TEXT_SUB	13U
#define NETWORK_TEXT_ROW	15U
#define NETWORK_TEXT_SMALL	12U

/*
 * One row of a list of networks: the network's index in the scan (-1 for
 * the network the machine is on when the scan has not found it), its
 * SSID, its strength and whether it asks for a key.
 */
struct network_row {
	int index;
	const char *ssid;
	int rssi;
	unsigned secured;
};

static int network_status_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void network_tile(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width, unsigned glyph, const char *label, const char *value, const char *detail, int dot);
static int network_wifi_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, int compact);
static int network_rows(const struct se_app *app, struct network_row *rows, int capacity);
static void network_row_draw(struct se_app *app, struct kl_canvas *canvas, const struct network_row *row, int x, int y, int width);
static void network_key_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width);
static void network_key_reveal(struct se_app *app, int top, int bottom);
static int network_ethernet_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int network_dns_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int network_usage_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void network_graph(struct kl_canvas *canvas, const struct se_network *network, int x, int y, int width, int height);
static int network_link_card(struct se_app *app, struct kl_canvas *canvas, const struct kl_network_link *link, int x, int top, int width);
static int network_header(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, int height, const char *title, const char *subtitle);
static int network_message_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width);
static const struct kl_network_link *network_link(const struct se_network *network, const char *name);
static int network_wired(const struct se_network *network, size_t index);
static int network_saved(const struct se_network *network, const char *ssid);
static unsigned network_prefix(const char *netmask);
static void network_speed_text(unsigned mbps, char *text, size_t size);
static const char *network_wifi_words(struct se_network *network);

/*
 * Draws the Network page's cards from a top edge; returns the edge below
 * them.
 */
int
se_network_page_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	int left_width;
	int right_x;
	int left;
	int right;
	int y;

	/* The connection's state across the column. */
	y = network_status_card(app, canvas, x, top, width);
	y += NETWORK_GAP;

	/* The Wi-Fi beside the wired interface and the servers, or under each other in a narrow column. */
	if (width >= 760) {
		left_width = (width - NETWORK_GAP) * 11 / 20;
		right_x = x + left_width + NETWORK_GAP;
		left = network_wifi_card(app, canvas, x, y, left_width, 1);
		right = network_ethernet_card(app, canvas, right_x, y, x + width - right_x);
		right = network_dns_card(app, canvas, right_x, right + NETWORK_GAP, x + width - right_x);
		y = left;
		if (right > y)
			y = right;
	} else {
		y = network_wifi_card(app, canvas, x, y, width, 1);
		y = network_ethernet_card(app, canvas, x, y + NETWORK_GAP, width);
		y = network_dns_card(app, canvas, x, y + NETWORK_GAP, width);
	}

	/* The activity across the column. */
	y = network_usage_card(app, canvas, x, y + NETWORK_GAP, width);

	/* The edge below the last card. */
	return y;
}

/*
 * Draws the Wi-Fi page's cards from a top edge; returns the edge below
 * them.
 */
int
se_wifi_page_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	struct kl_text_line line;
	char words[96];
	int on;
	int y;

	/* The switch's card: the Wi-Fi's state in words and the switch, at the position asked while it waits (BUG-183). */
	network = &app->network;
	on = se_network_wifi_on(network);
	(void)network_header(app, canvas, x, top, width, NETWORK_HEADER + 8, "Wi-Fi", network_wifi_words(&app->network));
	se_toggle_draw(app, canvas, x + width - NETWORK_PAD - 44, top + 20, on, network->state.wifi != KL_WIFI_ABSENT, NETWORK_WIFI_SWITCH);

	/* The networks around, under the switch's card. */
	y = network_wifi_card(app, canvas, x, top + NETWORK_HEADER + 8 + NETWORK_GAP, width, 0);

	/* A note on where the keys are kept. */
	kl_text_metrics(app->text, NETWORK_TEXT_SMALL, &line);
	(void)snprintf(words, sizeof(words), "%s", "Keys are kept in your account and used again when a network is in reach.");
	(void)kl_text_draw_fit(app->text, canvas, x + 4, y + 14 + line.ascent, words, NETWORK_TEXT_SMALL, 0, width - 8, SE_COLOR_TEXT_SECONDARY);

	/* The edge below the note. */
	return y + 14 + line.height;
}

/*
 * Draws the Ethernet page's cards from a top edge: a card each wired
 * interface, and the DNS servers.  Returns the edge below them.
 */
int
se_ethernet_page_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	size_t index;
	int shown;
	int wired;
	int y;

	/* Each wired interface. */
	network = &app->network;
	shown = 0;
	y = top - NETWORK_GAP;
	for (index = 0; index < network->link_count; index++) {
		wired = network_wired(network, index);
		if (wired == 0)
			continue;
		y = network_link_card(app, canvas, &network->links[index], x, y + NETWORK_GAP, width);
		y = se_wired_card(app, canvas, &network->links[index], x, y + NETWORK_GAP, width);
		shown++;
	}

	/* The last Apply's answer once the editor closed (ws089-p022). */
	if (app->wired.interface[0] == '\0' && app->wired.message[0] != '\0') {
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + NETWORK_GAP + 16, app->wired.message, NETWORK_TEXT_SUB, 0, width, SE_COLOR_TEXT_SECONDARY);
		y += NETWORK_GAP + 24;
	}

	/* A machine without one says so. */
	if (shown == 0) {
		(void)network_header(app, canvas, x, y + NETWORK_GAP, width, NETWORK_HEADER, "No wired interface", "This computer has no Ethernet port, or its driver is not there.");
		y += NETWORK_GAP + NETWORK_HEADER;
	}

	/* The servers. */
	y = network_dns_card(app, canvas, x, y + NETWORK_GAP, width);

	/* The edge below the last card. */
	return y;
}

/*
 * Carries out a click on a control of the network pages.
 */
void
se_network_press(
	struct se_app *app,
	int index)
{
	struct se_network *network;
	const struct kl_network_ap *ap;
	int saved;
	int differs;
	int on;

	/* Each control. */
	network = &app->network;
	switch (index) {
	case NETWORK_WIFI_SWITCH:
		/* The other position than the switch shows (the one asked while it waits). */
		on = se_network_wifi_on(network);
		se_network_wifi(app, !on);
		return;
	case NETWORK_DISCONNECT:
		se_network_disconnect(app);
		return;
	case NETWORK_KEY_JOIN:
		/* A key long enough is saved and the network joined. */
		if (network->key.length >= KL_NETWORK_KEY_MIN)
			se_network_join_key(app, network->key_ssid, network->key.text);
		return;
	case NETWORK_KEY_CANCEL:
		/* The line closes and the key typed is wiped. */
		network->key_ssid[0] = '\0';
		se_field_clear(&network->key);
		network->message[0] = '\0';
		return;
	case NETWORK_KEY_SHOW:
		network->key_shown = !network->key_shown;
		return;
	case NETWORK_KEY_FIELD:
		/* The field has the keyboard already while its line is open. */
		return;
	case NETWORK_ETHERNET:
		se_ui_go(app, SE_PAGE_ETHERNET);
		return;
	case NETWORK_WIFI_ALL:
		se_ui_go(app, SE_PAGE_WIFI);
		return;
	default:
		break;
	}

	/* Anything else is a network of the scan. */
	if (index < NETWORK_AP_FIRST || index - NETWORK_AP_FIRST >= (int)network->scan_count)
		return;
	ap = &network->scan[index - NETWORK_AP_FIRST];

	/* The network the machine is on needs nothing. */
	differs = strcmp(ap->ssid, network->state.ssid);
	if (differs == 0 && network->state.wifi == KL_WIFI_CONNECTED)
		return;

	/* A saved network is joined at once. */
	saved = network_saved(network, ap->ssid);
	if (saved != 0) {
		network->key_ssid[0] = '\0';
		se_field_clear(&network->key);
		se_network_join(app, ap->ssid);
		return;
	}

	/* A network without a key cannot be saved yet (the store keeps WPA keys only). */
	if (ap->secured == 0) {
		(void)snprintf(network->message, sizeof(network->message), "%s is an open network; open networks come in a later version of Kei.", ap->ssid);
		network->message_bad = 1;
		return;
	}

	/* A new secured network: the line for its key opens under it, scrolled into sight at the next frame. */
	(void)snprintf(network->key_ssid, sizeof(network->key_ssid), "%s", ap->ssid);
	se_field_clear(&network->key);
	network->key_shown = 0;
	network->key_reveal = 1;
	network->message[0] = '\0';
	se_log("NETWORK key-form ssid=%s", ap->ssid);
}

/*
 * Takes a key press while the line for a key is open: Enter joins, Esc
 * closes the line, other keys type.  Returns 1 when the key was used.
 */
int
se_network_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_network *network;
	int used;

	/* Without the line the keys are the window's. */
	network = &app->network;
	if (network->key_ssid[0] == '\0')
		return 0;

	/* Enter joins with the key typed. */
	if (event->key == SE_KEY_ENTER) {
		se_network_press(app, NETWORK_KEY_JOIN);
		return 1;
	}

	/* Esc closes the line. */
	if (event->key == SE_KEY_ESC) {
		se_network_press(app, NETWORK_KEY_CANCEL);
		return 1;
	}

	/* Anything else types (or is not the field's). */
	used = se_field_key(&network->key, event);
	if (used == 0)
		return 0;

	/* Succeeded: the field took the key. */
	return 1;
}

/* Draws the card of the connection's state: Internet, the network in use, its address, and the wired link. Returns the edge below it. */
static int
network_status_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	const struct kl_network_link *link;
	const struct kl_network_link *wired;
	char address[40];
	char detail[40];
	int columns;
	int tile_width;
	int third;
	int inner;
	int height;
	int y;
	unsigned glyph;

	/* Four tiles in a row, or two rows of two in a narrow column. */
	network = &app->network;
	inner = width - 2 * NETWORK_PAD;
	columns = 4;
	if (inner < 640)
		columns = 2;
	tile_width = (inner - (columns - 1) * 12) / columns;
	height = se_card_height(0, 1) + NETWORK_TILE_HEIGHT;
	if (columns == 2)
		height += NETWORK_TILE_HEIGHT + 12;
	y = se_card_begin(app, canvas, x, top, width, height, "Connection Status", NULL);

	/* Internet: whether an interface is up with an address. */
	if (network->live == 0) {
		network_tile(app, canvas, x + NETWORK_PAD, y, tile_width, SE_GLYPH_GLOBE, "Internet", "Unknown", "Not available on this desktop", 0);
	} else if (network->state.reachable == 0) {
		network_tile(app, canvas, x + NETWORK_PAD, y, tile_width, SE_GLYPH_GLOBE, "Internet", "Unknown", "The network service is not running", 0);
	} else if (network->state.connected != 0) {
		network_tile(app, canvas, x + NETWORK_PAD, y, tile_width, SE_GLYPH_GLOBE, "Internet", "Connected", "Online", 1);
	} else {
		network_tile(app, canvas, x + NETWORK_PAD, y, tile_width, SE_GLYPH_GLOBE, "Internet", "Not connected", "No network in use", -1);
	}

	/* The network in use: the Wi-Fi's SSID or the wired interface. */
	glyph = SE_GLYPH_ETHERNET;
	if (network->state.kind == KL_NETWORK_WIFI)
		glyph = SE_GLYPH_WIFI;
	if (network->state.kind == KL_NETWORK_WIFI) {
		network_tile(app, canvas, x + NETWORK_PAD + (tile_width + 12), y, tile_width, glyph, "Active network", network->state.ssid, "Wi-Fi", 0);
	} else if (network->state.kind == KL_NETWORK_WIRED) {
		network_tile(app, canvas, x + NETWORK_PAD + (tile_width + 12), y, tile_width, glyph, "Active network", network->state.interface, "Wired", 0);
	} else {
		network_tile(app, canvas, x + NETWORK_PAD + (tile_width + 12), y, tile_width, glyph, "Active network", "None", "-", 0);
	}

	/* The address of the interface in use (its IPv4 address and prefix). */
	link = network_link(network, network->state.interface);
	address[0] = '\0';
	detail[0] = '\0';
	if (link != NULL && link->address[0] != '\0') {
		(void)snprintf(address, sizeof(address), "%s", link->address);
		(void)snprintf(detail, sizeof(detail), "IPv4 /%u", network_prefix(link->netmask));
	} else {
		(void)snprintf(address, sizeof(address), "%s", "No address");
		(void)snprintf(detail, sizeof(detail), "%s", "-");
	}

	/* The second two tiles: beside the first two, or on a row of their own. */
	third = x + NETWORK_PAD + 2 * (tile_width + 12);
	if (columns == 2) {
		third = x + NETWORK_PAD;
		y += NETWORK_TILE_HEIGHT + 12;
	}

	/* The address. */
	network_tile(app, canvas, third, y, tile_width, SE_GLYPH_DISK, "IP address", address, detail, 0);

	/* The wired interface: its link and name. */
	wired = network_link(network, network->state.wired);
	if (network->state.wired[0] != '\0' && wired != NULL) {
		network_tile(app, canvas, third + tile_width + 12, y, tile_width, SE_GLYPH_ETHERNET, "Wired", "Connected", wired->name, 1);
	} else {
		network_tile(app, canvas, third + tile_width + 12, y, tile_width, SE_GLYPH_ETHERNET, "Wired", "Not connected", "-", -1);
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws one tile of the connection's state: a picture in a circle, a label, a value (with a green or grey dot when dot is 1 or -1) and a detail. */
static void
network_tile(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width,
	unsigned glyph,
	const char *label,
	const char *value,
	const char *detail,
	int dot)
{
	kl_color dot_color;
	int text_x;
	int text_width;

	/* The tile's ground, and the picture in a pale circle. */
	kl_canvas_round(canvas, (float)x, (float)y, (float)width, (float)NETWORK_TILE_HEIGHT, 12.0f, SE_COLOR_TILE);
	kl_canvas_circle(canvas, (float)x + 32.0f, (float)y + NETWORK_TILE_HEIGHT * 0.5f, 20.0f, KL_RGBA(0x2f7cf6, 30));
	se_glyph_draw(canvas, glyph, (float)x + 20.0f, (float)y + NETWORK_TILE_HEIGHT * 0.5f - 12.0f, 24.0f, SE_COLOR_ACCENT);

	/* The label. */
	text_x = x + 62;
	text_width = width - 70;
	(void)kl_text_draw_fit(app->text, canvas, text_x, y + 24, label, NETWORK_TEXT_SMALL, 0, text_width, SE_COLOR_TEXT_SECONDARY);

	/* The value, after its dot when it has one (green for good, grey for not). */
	dot_color = SE_COLOR_TEXT_FAINT;
	if (dot > 0)
		dot_color = SE_COLOR_GOOD;
	if (dot != 0) {
		se_dot_draw(canvas, (float)text_x + 4.5f, (float)y + 40.0f, dot_color);
		text_x += 14;
		text_width -= 14;
	}

	/* The value itself. */
	(void)kl_text_draw_fit(app->text, canvas, text_x, y + 46, value, NETWORK_TEXT_ROW, 1, text_width, SE_COLOR_TEXT);

	/* The detail under it. */
	(void)kl_text_draw_fit(app->text, canvas, x + 62, y + 64, detail, NETWORK_TEXT_SMALL, 0, width - 70, SE_COLOR_TEXT_FAINT);
}

/*
 * Draws the card of the Wi-Fi networks: its switch, the networks (the one
 * the machine is on first), the line for a key under the network it is
 * for, and the last message.  compact lists a few and a link to the Wi-Fi
 * page.  Returns the edge below the card.
 */
static int
network_wifi_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int compact)
{
	struct network_row rows[SE_NETWORK_SCAN + 1];
	const struct se_network *network;
	struct kl_rect all;
	const char *title;
	const char *subtitle;
	int lit;
	int count;
	int shown;
	int height;
	int y;
	int index;
	int form;
	int differs;
	int on;
	int switch_on;

	/* The networks to list: all of them, or a few in the compact card. */
	network = &app->network;
	count = network_rows(app, rows, SE_NETWORK_SCAN + 1);
	shown = count;
	if (compact != 0 && shown > NETWORK_COMPACT_ROWS)
		shown = NETWORK_COMPACT_ROWS;

	/* Whether the line for a key is under one of the rows listed. */
	form = 0;
	for (index = 0; index < shown; index++) {
		differs = strcmp(rows[index].ssid, network->key_ssid);
		if (network->key_ssid[0] != '\0' && differs == 0)
			form = 1;
	}

	/* The card's height: the header, a line of words or the rows, the key's line, the link, the message. */
	on = 0;
	if (network->state.wifi != KL_WIFI_OFF && network->state.wifi != KL_WIFI_ABSENT)
		on = 1;
	height = NETWORK_HEADER + 8;
	if (on == 0 || shown == 0) {
		height += 36;
	} else {
		height += shown * NETWORK_ROW;
	}

	/* The key's line, the link and the message, when there are. */
	if (form != 0)
		height += NETWORK_KEY_LINE;
	if (compact != 0 && on != 0)
		height += 40;
	if (network->message[0] != '\0')
		height += NETWORK_MESSAGE_LINE;
	height += 8;

	/*
	 * The header: the title, the state in words, and the switch on the
	 * compact card (the Wi-Fi page has it on its own card above).  The list
	 * follows the scans by itself (ws089-p021): "Searching..." stands until
	 * the first scan comes.
	 */
	title = "Networks";
	if (compact != 0)
		title = "Wi-Fi Networks";
	subtitle = "Connect to available wireless networks.";
	if (on != 0 && network->scan_count == 0 && network->scan_received == 0)
		subtitle = "Searching...";
	y = network_header(app, canvas, x, top, width, height, title, subtitle);
	if (compact != 0) {
		switch_on = se_network_wifi_on(network);
		se_toggle_draw(app, canvas, x + width - NETWORK_PAD - 44, top + 20, switch_on, network->state.wifi != KL_WIFI_ABSENT, NETWORK_WIFI_SWITCH);
	}

	/* Without the radio, while it is off, or with nothing listed, a line of words. */
	if (network->state.wifi == KL_WIFI_ABSENT) {
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 2, y + 22, "This computer has no Wi-Fi radio.", NETWORK_TEXT_ROW, 0, width - 2 * NETWORK_PAD, SE_COLOR_TEXT_SECONDARY);
		y += 36;
	} else if (on == 0) {
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 2, y + 22, "Wi-Fi is off.", NETWORK_TEXT_ROW, 0, width - 2 * NETWORK_PAD, SE_COLOR_TEXT_SECONDARY);
		y += 36;
	} else if (shown == 0 && network->scan_received != 0) {
		/* A scan came back empty: nothing is in reach. */
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 2, y + 22, "No networks in reach.", NETWORK_TEXT_ROW, 0, width - 2 * NETWORK_PAD, SE_COLOR_TEXT_SECONDARY);
		y += 36;
	} else if (shown == 0) {
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 2, y + 22, "No networks found yet.", NETWORK_TEXT_ROW, 0, width - 2 * NETWORK_PAD, SE_COLOR_TEXT_SECONDARY);
		y += 36;
	}

	/*
	 * Each network listed, the line for a key under its network, and the
	 * last message under that line, where the person typing reads it (a
	 * long list would put the card's foot out of sight).
	 */
	for (index = 0; on != 0 && index < shown; index++) {
		network_row_draw(app, canvas, &rows[index], x + 10, y, width - 20);
		y += NETWORK_ROW;
		differs = strcmp(rows[index].ssid, network->key_ssid);
		if (network->key_ssid[0] != '\0' && differs == 0) {
			network_key_draw(app, canvas, x + 10, y, width - 20);
			y += NETWORK_KEY_LINE;
			y = network_message_draw(app, canvas, x + NETWORK_PAD + 2, y, width - 2 * NETWORK_PAD);
			network_key_reveal(app, y - NETWORK_ROW - NETWORK_KEY_LINE, y);
		}
	}

	/* The compact card's link to every network. */
	if (compact != 0 && on != 0) {
		all.x = x + 10;
		all.y = y + 2;
		all.width = width - 20;
		all.height = 36;
		lit = se_ui_lit(app, SE_HIT_CONTROL, NETWORK_WIFI_ALL);
		if (lit != 0)
			kl_canvas_round(canvas, (float)all.x, (float)all.y, (float)all.width, (float)all.height, 10.0f, SE_COLOR_HOVER);
		(void)kl_text_draw(app->text, canvas, all.x + 12, kl_text_center(14U, all.y, all.height), "All Wi-Fi networks", strlen("All Wi-Fi networks"), 14U, 0, SE_COLOR_TEXT);
		se_glyph_draw(canvas, SE_GLYPH_CHEVRON, (float)(all.x + all.width) - 26.0f, (float)all.y + 10.0f, 16.0f, SE_COLOR_TEXT_SECONDARY);
		se_ui_hit(app, &all, SE_HIT_CONTROL, NETWORK_WIFI_ALL);
		y += 40;
	}

	/* The last message at the card's foot, unless it stands under the key's line of a listed network. */
	if (form == 0 || on == 0)
		(void)network_message_draw(app, canvas, x + NETWORK_PAD + 2, y, width - 2 * NETWORK_PAD);

	/* The edge below the card. */
	return top + height;
}

/* Lists the networks to show: the one the machine is on first (also when the scan has not found it), then the scan's. Returns how many. */
static int
network_rows(
	const struct se_app *app,
	struct network_row *rows,
	int capacity)
{
	const struct se_network *network;
	const char *current;
	size_t index;
	int count;
	int differs;
	int found;

	/* The network the machine is on or joining (none when not). */
	network = &app->network;
	current = NULL;
	if (network->state.ssid[0] != '\0')
		current = network->state.ssid;

	/* The current one first: from the scan when it is there, else on its own. */
	count = 0;
	found = 0;
	for (index = 0; current != NULL && index < network->scan_count; index++) {
		differs = strcmp(network->scan[index].ssid, current);
		if (differs != 0)
			continue;
		rows[count].index = (int)index;
		rows[count].ssid = network->scan[index].ssid;
		rows[count].rssi = network->scan[index].rssi;
		rows[count].secured = network->scan[index].secured;
		count++;
		found = 1;
		break;
	}

	/* The current one the scan has not found yet stands on its own. */
	if (current != NULL && found == 0) {
		rows[count].index = -1;
		rows[count].ssid = current;
		rows[count].rssi = -50;
		rows[count].secured = 1;
		count++;
	}

	/* Every other network of the scan, strongest first. */
	for (index = 0; index < network->scan_count && count < capacity; index++) {
		if (current != NULL) {
			differs = strcmp(network->scan[index].ssid, current);
			if (differs == 0)
				continue;
		}

		/* The network's row. */
		rows[count].index = (int)index;
		rows[count].ssid = network->scan[index].ssid;
		rows[count].rssi = network->scan[index].rssi;
		rows[count].secured = network->scan[index].secured;
		count++;
	}

	/* The rows. */
	return count;
}

/* Draws one network's row: the picture, the SSID and its state, the lock and the signal; lit when it is the network in use or under the pointer. */
static void
network_row_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct network_row *row,
	int x,
	int y,
	int width)
{
	const struct se_network *network;
	struct kl_rect rect;
	const char *state;
	kl_color ink;
	kl_color glyph;
	int current;
	int joining;
	int differs;
	int control;
	int saved;
	int words;
	int lit;

	/* Whether this is the network in use, or the one being joined. */
	network = &app->network;
	differs = strcmp(row->ssid, network->state.ssid);
	current = 0;
	if (differs == 0 && network->state.wifi == KL_WIFI_CONNECTED)
		current = 1;
	joining = 0;
	differs = strcmp(row->ssid, network->join_ssid);
	if (differs == 0 && network->join_step != SE_JOIN_NONE)
		joining = 1;

	/*
	 * A join still waiting in the slot behind the outstanding request (the
	 * page's own scan, usually) is being joined too: the row says so from
	 * the click, not only once the join is sent (BUG-154).
	 */
	if (network->pending_request != SE_NETWORK_NONE && network->pending_step != SE_JOIN_NONE) {
		differs = strcmp(row->ssid, network->pending_ssid);
		if (differs == 0)
			joining = 1;
	}
	if (network->state.wifi == KL_WIFI_CONNECTING) {
		differs = strcmp(row->ssid, network->state.ssid);
		if (differs == 0)
			joining = 1;
	}

	/* The row's ground: the accent's for the network in use, a shade under the pointer. */
	control = NETWORK_AP_FIRST + row->index;
	rect.x = x;
	rect.y = y + 2;
	rect.width = width;
	rect.height = NETWORK_ROW - 4;
	lit = se_ui_lit(app, SE_HIT_CONTROL, control);
	if (current != 0) {
		kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 10.0f, SE_COLOR_SELECTION);
	} else if (row->index >= 0 && lit != 0) {
		kl_canvas_round(canvas, (float)rect.x, (float)rect.y, (float)rect.width, (float)rect.height, 10.0f, SE_COLOR_HOVER);
	}

	/* The state in words. */
	saved = network_saved(network, row->ssid);
	state = "Open";
	if (row->secured != 0)
		state = "Secured";
	if (saved != 0)
		state = "Saved";
	if (joining != 0)
		state = "Connecting...";
	if (current != 0)
		state = "Connected";

	/* The colours: the accent's for the network in use. */
	ink = SE_COLOR_TEXT_SECONDARY;
	glyph = SE_COLOR_ICON;
	if (current != 0) {
		ink = SE_COLOR_ACCENT_TEXT;
		glyph = SE_COLOR_ACCENT;
	}

	/* The room of the words: less on the network in use, whose row has Disconnect too. */
	words = width - 140;
	if (current != 0)
		words = width - 140 - NETWORK_DISCONNECT_ROOM;

	/* The picture, the SSID and the state. */
	se_glyph_draw(canvas, SE_GLYPH_WIFI, (float)x + 12.0f, (float)y + 13.0f, 24.0f, glyph);
	(void)kl_text_draw_fit(app->text, canvas, x + 50, y + 22, row->ssid, NETWORK_TEXT_ROW, current, words, SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + 50, y + 40, state, NETWORK_TEXT_SMALL, 0, words, ink);

	/* The lock of a secured network, and the signal. */
	if (row->secured != 0)
		se_glyph_draw(canvas, SE_GLYPH_LOCK, (float)(x + width) - 62.0f, (float)y + 16.0f, 18.0f, SE_COLOR_ICON);
	se_signal_draw(canvas, (float)(x + width) - 34.0f, (float)y + 34.0f, row->rssi, SE_COLOR_ACCENT);

	/* A network of the scan is clickable. */
	if (row->index >= 0)
		se_ui_hit(app, &rect, SE_HIT_CONTROL, control);

	/* The network in use has Disconnect left of its lock, over the row's region (ws089-p021: a picture, not a word). */
	if (current != 0)
		se_icon_button_draw(app, canvas, x + width - 70 - NETWORK_DISCONNECT_ROOM + 4, y + (NETWORK_ROW - 32) / 2, KL_ICON_DISCONNECT, NETWORK_DISCONNECT);
}

/*
 * Scrolls the page pane, once after the line for a key opened, so that the
 * network's row and the line under it (top to bottom, on the canvas) are
 * in sight (BUG-160: a network low on the page opened its line out of
 * sight, at the bottom of the screen).  The next frame draws the new scroll.
 */
static void
network_key_reveal(
	struct se_app *app,
	int top,
	int bottom)
{
	const struct kl_rect *pane;
	int scroll;

	/* Only once for a line that opened. */
	if (app->network.key_reveal == 0)
		return;
	app->network.key_reveal = 0;

	/* A line below the pane's bottom comes up to it, without taking the row above the pane's top. */
	pane = &app->layout.page;
	scroll = app->page_scroll;
	if (bottom > pane->y + pane->height)
		scroll += bottom - (pane->y + pane->height);
	if (top - (scroll - app->page_scroll) < pane->y)
		scroll = app->page_scroll + (top - pane->y);
	if (scroll < 0)
		scroll = 0;

	/* A change needs a frame (the page's extent keeps it within the page). */
	if (scroll != app->page_scroll) {
		app->page_scroll = scroll;
		app->dirty = 1;
		se_log("NETWORK key-form reveal scroll=%d", scroll);
	}
}

/* Draws the line for a key under its network: the field (dots unless shown), Show, Join and Cancel. */
static void
network_key_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct se_network *network;
	struct kl_rect field;
	const char *reveal;
	unsigned kind;
	int join;
	int cancel;
	int show;
	int right;

	/* The buttons at the right, from the right: Cancel, Join, Show. */
	network = &app->network;
	right = x + width - 6;
	reveal = "Show";
	if (network->key_shown != 0)
		reveal = "Hide";
	cancel = se_button_width(app, "Cancel");
	join = se_button_width(app, "Join");
	show = se_button_width(app, reveal);
	(void)se_button_draw(app, canvas, right - cancel, y + 10, "Cancel", 0, 1, NETWORK_KEY_CANCEL);
	(void)se_button_draw(app, canvas, right - cancel - 8 - join, y + 10, "Join", 1, network->key.length >= KL_NETWORK_KEY_MIN, NETWORK_KEY_JOIN);
	(void)se_button_draw(app, canvas, right - cancel - 8 - join - 8 - show, y + 10, reveal, 0, 1, NETWORK_KEY_SHOW);

	/* The field, which has the keyboard while the line is open. */
	field.x = x + 50;
	field.y = y + 8;
	field.width = right - cancel - 8 - join - 8 - show - 12 - field.x;
	field.height = 36;
	se_ui_hit(app, &field, SE_HIT_CONTROL, NETWORK_KEY_FIELD);

	/* libkeiland's field: the key as dots, or plain when shown; never an input method (ws090-p007). */
	kind = SE_FIELD_SECRET;
	if (network->key_shown != 0)
		kind = SE_FIELD_PLAIN;
	(void)se_field_draw(app, canvas, &network->key, &field, "Key (8 to 63 characters)", kind, 1);
}

/* Draws the card of the wired interfaces: each one's link, name and address, and Details. Returns the edge below it. */
static int
network_ethernet_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	const struct kl_network_link *link;
	char name[48];
	const char *status;
	const char *address;
	kl_color status_color;
	kl_color status_ink;
	int rows;
	int height;
	int y;
	int button;
	int wired;
	size_t index;

	/* How many wired interfaces there are (two at most here). */
	network = &app->network;
	rows = 0;
	for (index = 0; index < network->link_count && rows < 2; index++) {
		wired = network_wired(network, index);
		if (wired != 0)
			rows++;
	}

	/* The card: a line each of them, or one line that says there is none. */
	height = NETWORK_HEADER + 8 + 58 + 10;
	if (rows > 1)
		height += (rows - 1) * 58;
	y = network_header(app, canvas, x, top, width, height, "Ethernet / LAN", "Wired network connection.");

	/* A machine without one says so. */
	if (rows == 0) {
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 2, y + 24, "No wired interface.", NETWORK_TEXT_ROW, 0, width - 2 * NETWORK_PAD, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* Each wired interface: the picture, its name, the link with its dot, the address, and Details. */
	rows = 0;
	for (index = 0; index < network->link_count && rows < 2; index++) {
		wired = network_wired(network, index);
		if (wired == 0)
			continue;

		/* Its state in words and colour. */
		link = &network->links[index];
		status = "Not connected";
		status_color = SE_COLOR_TEXT_FAINT;
		status_ink = SE_COLOR_TEXT_SECONDARY;
		if (link->running != 0 && link->address[0] != '\0') {
			status = "Connected";
			status_color = SE_COLOR_GOOD;
			status_ink = SE_COLOR_GOOD;
		}

		/* Its address ("No address" for none). */
		address = "No address";
		if (link->address[0] != '\0')
			address = link->address;

		/* The line. */
		se_glyph_draw(canvas, SE_GLYPH_MONITOR, (float)x + NETWORK_PAD + 2.0f, (float)y + 14.0f, 28.0f, SE_COLOR_ICON);
		(void)snprintf(name, sizeof(name), "Ethernet (%s)", link->name);
		button = se_button_width(app, "Details");
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 44, y + 18, name, NETWORK_TEXT_ROW, 1, width - 2 * NETWORK_PAD - 56 - button, SE_COLOR_TEXT);
		se_dot_draw(canvas, (float)x + NETWORK_PAD + 48.5f, (float)y + 32.0f, status_color);
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 58, y + 37, status, NETWORK_TEXT_SUB, 0, width - 2 * NETWORK_PAD - 70 - button, status_ink);
		(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD + 44, y + 54, address, NETWORK_TEXT_SMALL, 0, width - 2 * NETWORK_PAD - 56 - button, SE_COLOR_TEXT_SECONDARY);
		(void)se_button_draw(app, canvas, x + width - NETWORK_PAD - button, y + 12, "Details", 0, 1, NETWORK_ETHERNET);
		y += 58;
		rows++;
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws the card of the DNS servers. Returns the edge below it. */
static int
network_dns_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	char label[32];
	size_t index;
	int rows;
	int height;
	int y;

	/* A row a server, or one that says there are none. */
	network = &app->network;
	rows = (int)network->dns_count;
	if (rows == 0)
		rows = 1;
	height = se_card_height(rows, 1) + 20;
	y = se_card_begin(app, canvas, x, top, width, height, "DNS", "The servers that find names on the internet.");

	/* The servers, or the absence of any. */
	if (network->dns_count == 0) {
		(void)se_row_value(app, canvas, x, y, width, "Servers", "None", 1);
		return top + height;
	}

	/* A row each server. */
	for (index = 0; index < network->dns_count; index++) {
		(void)snprintf(label, sizeof(label), "Server %u", (unsigned)(index + 1U));
		y = se_row_value(app, canvas, x, y, width, label, network->dns[index], index + 1U == network->dns_count);
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws the card of the network's activity: the totals received and sent, and the rates of the last minutes as a graph. Returns the edge below it. */
static int
network_usage_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	const struct se_network *network;
	char total[32];
	int height;
	int y;
	int legend;

	/* The card. */
	network = &app->network;
	height = NETWORK_HEADER + NETWORK_GRAPH_HEIGHT + 34;
	y = network_header(app, canvas, x, top, width, height, "Network Activity", "The graph shows every interface's traffic since this window opened.");

	/* The legend at the left: received and sent since the computer started. */
	legend = 190;
	se_dot_draw(canvas, (float)x + NETWORK_PAD + 6.0f, (float)y + 22.0f, NETWORK_COLOR_DOWN);
	(void)kl_text_draw(app->text, canvas, x + NETWORK_PAD + 18, y + 27, "Download", 8U, 14U, 0, SE_COLOR_TEXT_SECONDARY);
	se_bytes_text(network->received_total, total, sizeof(total));
	(void)kl_text_draw(app->text, canvas, x + NETWORK_PAD + 104, y + 27, total, strlen(total), 14U, 1, SE_COLOR_TEXT);
	se_dot_draw(canvas, (float)x + NETWORK_PAD + 6.0f, (float)y + 52.0f, NETWORK_COLOR_UP);
	(void)kl_text_draw(app->text, canvas, x + NETWORK_PAD + 18, y + 57, "Upload", 6U, 14U, 0, SE_COLOR_TEXT_SECONDARY);
	se_bytes_text(network->sent_total, total, sizeof(total));
	(void)kl_text_draw(app->text, canvas, x + NETWORK_PAD + 104, y + 57, total, strlen(total), 14U, 1, SE_COLOR_TEXT);
	(void)kl_text_draw_fit(app->text, canvas, x + NETWORK_PAD, y + 84, "Since the computer started", NETWORK_TEXT_SMALL, 0, legend - 8, SE_COLOR_TEXT_FAINT);

	/* The graph to its right. */
	network_graph(canvas, network, x + NETWORK_PAD + legend, y + 4, width - 2 * NETWORK_PAD - legend, NETWORK_GRAPH_HEIGHT);

	/* The edge below the card. */
	return top + height;
}

/* Draws the rates of the last samples: received as a filled area, sent as a line, over a faint grid, scaled to the largest. */
static void
network_graph(
	struct kl_canvas *canvas,
	const struct se_network *network,
	int x,
	int y,
	int width,
	int height)
{
	float area[2 * (NETWORK_GRAPH_POINTS + 2)];
	float line[2 * NETWORK_GRAPH_POINTS];
	uint32_t largest;
	unsigned sample;
	unsigned slot;
	unsigned step;
	unsigned points;
	unsigned index;
	float px;
	float py;

	/* The grid: three faint lines. */
	for (index = 0; index < 3U; index++)
		kl_canvas_line(canvas, (float)x, (float)y + (float)height * (float)index / 2.0f, (float)(x + width), (float)y + (float)height * (float)index / 2.0f, 1.0f, KL_RGBA(0x8a96aa, 40));

	/* Too few samples draw nothing yet. */
	if (network->usage_count < 2U)
		return;

	/* The largest rate, at least a kilobyte a second so that an idle line lies low. */
	largest = 1000U;
	for (sample = 0; sample < network->usage_count; sample++) {
		slot = (network->usage_next + SE_USAGE_SAMPLES - network->usage_count + sample) % SE_USAGE_SAMPLES;
		if (network->received[slot] > largest)
			largest = network->received[slot];
		if (network->sent[slot] > largest)
			largest = network->sent[slot];
	}

	/* One point every step samples, the oldest at the left of its share of the width. */
	step = (network->usage_count + NETWORK_GRAPH_POINTS - 1U) / NETWORK_GRAPH_POINTS;
	if (step == 0U)
		step = 1U;
	points = 0;
	for (sample = 0; sample < network->usage_count && points < NETWORK_GRAPH_POINTS; sample += step) {
		slot = (network->usage_next + SE_USAGE_SAMPLES - network->usage_count + sample) % SE_USAGE_SAMPLES;
		px = (float)x + (float)width * (float)(SE_USAGE_SAMPLES - network->usage_count + sample) / (float)(SE_USAGE_SAMPLES - 1U);
		py = (float)(y + height) - (float)height * (float)network->received[slot] / (float)largest;
		area[2U * points] = px;
		area[2U * points + 1U] = py;
		line[2U * points] = px;
		line[2U * points + 1U] = (float)(y + height) - (float)height * (float)network->sent[slot] / (float)largest;
		points++;
	}

	/* The received area closed along the bottom, and its top edge. */
	area[2U * points] = area[2U * (points - 1U)];
	area[2U * points + 1U] = (float)(y + height);
	area[2U * points + 2U] = area[0];
	area[2U * points + 3U] = (float)(y + height);
	kl_canvas_polygon(canvas, area, (int)points + 2, KL_RGBA(0x2f7cf6, 60));
	for (index = 0; index + 1U < points; index++)
		kl_canvas_line(canvas, area[2U * index], area[2U * index + 1U], area[2U * index + 2U], area[2U * index + 3U], 2.0f, NETWORK_COLOR_DOWN);

	/* The sent line. */
	for (index = 0; index + 1U < points; index++)
		kl_canvas_line(canvas, line[2U * index], line[2U * index + 1U], line[2U * index + 2U], line[2U * index + 3U], 2.0f, NETWORK_COLOR_UP);
}

/* Draws the card of one wired interface on the Ethernet page: its link, addresses, MTU and counters. Returns the edge below it. */
static int
network_link_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	const struct kl_network_link *link,
	int x,
	int top,
	int width)
{
	char title[48];
	char number[32];
	const char *address;
	const char *netmask;
	int height;
	int y;

	/* The card, titled with the interface's name. */
	(void)snprintf(title, sizeof(title), "Ethernet (%s)", link->name);
	height = se_card_height(8, 1);
	y = se_card_begin(app, canvas, x, top, width, height, title, NULL);

	/*
	 * The link: connected with an address, a cable without an address, up
	 * without a cable (BUG-213: that was "No address", beside an address
	 * the interface still held), or down.
	 */
	if (link->running != 0 && link->address[0] != '\0') {
		y = se_row_value(app, canvas, x, y, width, "Status", "Connected", 0);
	} else if (link->running != 0) {
		y = se_row_value(app, canvas, x, y, width, "Status", "No address", 0);
	} else if (link->up != 0) {
		y = se_row_value(app, canvas, x, y, width, "Status", "No cable", 0);
	} else {
		y = se_row_value(app, canvas, x, y, width, "Status", "Down", 0);
	}

	/* The speed the link was brought up at, as its driver last heard it (BUG-222). */
	network_speed_text(link->link_mbps, number, sizeof(number));
	y = se_row_value(app, canvas, x, y, width, "Link speed", number, 0);

	/* The addresses ("-" for none). */
	address = "-";
	if (link->address[0] != '\0')
		address = link->address;
	netmask = "-";
	if (link->netmask[0] != '\0')
		netmask = link->netmask;
	y = se_row_value(app, canvas, x, y, width, "IPv4 address", address, 0);
	y = se_row_value(app, canvas, x, y, width, "Subnet mask", netmask, 0);
	y = se_row_value(app, canvas, x, y, width, "Hardware address", link->hardware, 0);

	/* The MTU and the counters. */
	(void)snprintf(number, sizeof(number), "%u", link->mtu);
	y = se_row_value(app, canvas, x, y, width, "MTU", number, 0);
	se_bytes_text(link->received_bytes, number, sizeof(number));
	y = se_row_value(app, canvas, x, y, width, "Received", number, 0);
	se_bytes_text(link->sent_bytes, number, sizeof(number));
	(void)se_row_value(app, canvas, x, y, width, "Sent", number, 1);

	/* The edge below the card. */
	return top + height;
}

/* Draws a card of a height with a title and a subtitle in its header; returns the edge below the header. */
static int
network_header(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int height,
	const char *title,
	const char *subtitle)
{
	/* The card and its header. */
	(void)se_card_begin(app, canvas, x, top, width, height, title, subtitle);

	/* The content starts under the header. */
	return top + NETWORK_HEADER;
}

/* Draws the network's last message on a line (red when it tells of a failure); returns the edge below it. */
static int
network_message_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	const struct se_network *network;
	kl_color ink;

	/* No message, no line. */
	network = &app->network;
	if (network->message[0] == '\0')
		return y;

	/* The message, red when it tells of a failure. */
	ink = SE_COLOR_TEXT_SECONDARY;
	if (network->message_bad != 0)
		ink = SE_COLOR_BAD;
	(void)kl_text_draw_fit(app->text, canvas, x, y + 20, network->message, NETWORK_TEXT_SUB, 0, width, ink);

	/* The edge below the line. */
	return y + NETWORK_MESSAGE_LINE;
}

/* Finds an interface by its name, or NULL (also for an empty name). */
static const struct kl_network_link *
network_link(
	const struct se_network *network,
	const char *name)
{
	size_t index;
	int differs;

	/* An empty name is no interface. */
	if (name[0] == '\0')
		return NULL;

	/* Each interface's name. */
	for (index = 0; index < network->link_count; index++) {
		differs = strcmp(network->links[index].name, name);
		if (differs == 0)
			return &network->links[index];
	}

	/* No interface has that name. */
	return NULL;
}

/* Tells whether an interface is a wired one: not the loopback, not the Wi-Fi's radio. */
static int
network_wired(
	const struct se_network *network,
	size_t index)
{
	int differs;

	/* The loopback is not. */
	if (network->links[index].loopback != 0)
		return 0;

	/* Nor is the radio the network service names. */
	differs = strcmp(network->links[index].name, network->state.wifi_interface);
	if (network->state.wifi_interface[0] != '\0' && differs == 0)
		return 0;

	/*
	 * Nor any radio the system tells (KL_VERSION 76): the service names its
	 * radio only while it has one in use, and a radio that is up without a
	 * network was drawn as an Ethernet card with no cable (BUG-284).
	 */
	if (network->links[index].wireless != 0)
		return 0;

	/* Any other interface is. */
	return 1;
}

/* Tells whether the user has a key saved for a network. */
static int
network_saved(
	const struct se_network *network,
	const char *ssid)
{
	size_t index;
	int differs;

	/* Each saved network's SSID. */
	for (index = 0; index < network->saved_count; index++) {
		differs = strcmp(network->saved[index], ssid);
		if (differs == 0)
			return 1;
	}

	/* Not saved. */
	return 0;
}

/*
 * Writes a link's speed in words: "Unknown" while the driver has not
 * heard it, whole megabits below a gigabit, and gigabits from there
 * ("2.5 Gb/s" for the speed between one and the next).
 */
static void
network_speed_text(
	unsigned mbps,
	char *text,
	size_t size)
{
	unsigned whole;
	unsigned tenths;

	/* A speed the driver has not heard. */
	if (mbps == 0U) {
		(void)snprintf(text, size, "%s", "Unknown");
		return;
	}

	/* Below a gigabit: the megabits. */
	if (mbps < 1000U) {
		(void)snprintf(text, size, "%u Mb/s", mbps);
		return;
	}

	/* A gigabit and above: the gigabits, with their tenth when there is one. */
	whole = mbps / 1000U;
	tenths = (mbps % 1000U) / 100U;
	if (tenths == 0U) {
		(void)snprintf(text, size, "%u Gb/s", whole);
	} else {
		(void)snprintf(text, size, "%u.%u Gb/s", whole, tenths);
	}
}

/* Counts the bits of a dotted netmask (the prefix length). */
static unsigned
network_prefix(
	const char *netmask)
{
	unsigned parts[4];
	unsigned bits;
	unsigned part;
	int count;

	/* The four numbers. */
	count = sscanf(netmask, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]);
	if (count != 4)
		return 0;

	/* The bits set in each. */
	bits = 0;
	for (part = 0; part < 4U; part++) {
		while (parts[part] != 0U) {
			bits += parts[part] & 1U;
			parts[part] >>= 1;
		}
	}

	/* The prefix length. */
	return bits;
}

/* Says the Wi-Fi's state in words for the Wi-Fi page's header (the network being joined named, BUG-185). */
static const char *
network_wifi_words(
	struct se_network *network)
{
	/* A desktop without Keiland's system extension, the daemon out of reach, then each state. */
	if (network->live == 0)
		return "Wi-Fi settings are not available on this desktop.";
	if (network->state.reachable == 0)
		return "The network service is not running.";

	/* Each state of the radio. */
	switch (network->state.wifi) {
	case KL_WIFI_ABSENT:
		return "This computer has no Wi-Fi radio.";
	case KL_WIFI_OFF:
		return "Wi-Fi is off.";
	case KL_WIFI_SEARCHING:
		return "Looking for a saved network.";
	case KL_WIFI_CONNECTING:
		if (network->state.ssid[0] == '\0')
			return "Connecting...";
		(void)snprintf(network->wifi_words, sizeof(network->wifi_words), "Connecting to %s...", network->state.ssid);
		return network->wifi_words;
	case KL_WIFI_CONNECTED:
		return "Connected.";
	default:
		break;
	}

	/* On, and left unconnected. */
	return "On, and not connected.";
}
