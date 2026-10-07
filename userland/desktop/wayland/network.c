/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The network in the glass look's system bar (ws035-p013): an icon beside
 * the battery that shows how the machine is connected, and a menu under it
 * to act on the network.
 *
 * The icon is four rising bars for Wi-Fi (all of them dark when connected,
 * pale while searching or joining, pale and struck through when the Wi-Fi
 * is off) and a small tree of three boxes for a wired connection.  With no
 * connection at all it is pale bars.
 *
 * A click on the icon opens the menu: the Wi-Fi switch with the Wi-Fi's
 * state under it, the networks the radio sees (the one it is on first,
 * checked, with a Disconnect button on its row; a padlock on those that
 * ask for a key, the signal as bars), and the wired connection.  The list
 * is cut to what fits under the bar, the rest named in a note (BUG-148:
 * the bottom row "Disconnect from X" fell off the screen when many
 * networks were seen).  A click on a
 * network joins it with its saved profile; a network that asks for a key
 * and has none saved opens a key field in the menu instead, and Enter saves
 * the key in the user's store, tells the daemon and joins (ws005-p019,
 * BUG-138), the same three steps Settings takes, done by the system
 * extension's thread (system.c).  A click on the switch
 * turns the Wi-Fi on or off.  A click elsewhere, or Esc, closes the menu.
 * While the menu is open the radios are kept scanning (ws089-p021): the
 * menu is one of the holders the compositor counts with the system
 * extension's network objects that asked for scans, and while any holder
 * is there libkeiland-backend asks the daemon for fresh scans.  A client's
 * asking holds a minute unless it asks again (system.c); the menu's is the
 * compositor's own and holds exactly while the menu is open (it cannot
 * outlive the compositor, which is what the minute guards against).
 *
 * From the click on a network until its join is answered, the line under
 * the switch says "Connecting to X..." and the network's row says
 * "Connecting..." (BUG-154: switching networks took about ten seconds,
 * the menu's scan and then the join, with nothing shown).
 *
 * libkeiland-backend carries one request at a time.  A switch or a network clicked
 * while another request (a Settings window's, say) is still out waits
 * in one slot and is sent when that one is answered, rather than being
 * refused with "busy".
 *
 * All of it comes through libkeiland-backend (kl_backend_network_*): the compositor never
 * speaks networkd's protocol.  Nothing here waits for the daemon; each tick
 * reads what has arrived.
 *
 * The watch is shared with Keiland's system extension (system.c, WS131
 * p010): each tick tells it the new state and scan, and hands it the
 * answer of a request it sent; the slot is still sent after that answer.
 *
 * An Alt+click on the icon (ws099-p032, as Option+click on macOS) opens
 * the details instead of the menu: the network, the interface, its IPv4
 * address and mask, the DNS servers, its hardware address and MTU, the
 * Wi-Fi's signal and the bytes received and sent with their rates
 * (network-info.c).  The interfaces and the DNS servers are read on the
 * system extension's thread (the details reading, system.c), at the
 * opening and again each NETWORK_INFO_REFRESH_MS while the details show.
 * A click anywhere, or Esc, closes them.
 */

#include "glass.h"
#include "notify.h"
#include "userland/desktop/wayland/network-info.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* A parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The evdev codes of Esc, Backspace, Enter and the keypad's Enter. */
#define NETWORK_KEY_ESC		1U
#define NETWORK_KEY_BACKSPACE	14U
#define NETWORK_KEY_ENTER	28U
#define NETWORK_KEY_KPENTER	96U

/* How many evdev codes the character tables cover (up to the space bar). */
#define NETWORK_KEYS		58U

/* How long the switch keeps the position asked after a successful answer, for the state to agree (BUG-183). */
#define NETWORK_SWITCH_HOLD_MS	4000U

/* How long after the desktop starts a daemon not heard from yet is waited for rather than called missing (BUG-176). */
#define NETWORK_START_GRACE_MS	30000U

/* The depressed Shift and the locked Caps Lock in the seat's modifier masks. */
#define NETWORK_SHIFT		0x1U
#define NETWORK_CAPS		0x2U

/* The shortest and the longest WPA passphrase, in characters. */
#define NETWORK_KEY_MIN		8U
#define NETWORK_KEY_MAX		63U

/*
 * The Disconnect button on the row of the network it is on (BUG-148), an
 * icon (ws089-p021: the user's "Disconnectボタンは、アイコンにしたい"):
 * its width, its inset from the row's top and bottom, and the icon's size.
 */
#define NETWORK_DISCONNECT_WIDTH	26
#define NETWORK_DISCONNECT_ICON		12
#define NETWORK_DISCONNECT_INSET	4

/* The menu's width, its padding, its rows' height, and its corner radius. */
#define NETWORK_MENU_WIDTH	300
#define NETWORK_MENU_PADDING	8
#define NETWORK_ROW_HEIGHT	30
#define NETWORK_NOTE_HEIGHT	22
#define NETWORK_SEPARATOR	9
#define NETWORK_MENU_RADIUS	12.0f

/* The details' row height, their title's, and how often they are read again while they show. */
#define NETWORK_INFO_ROW		24
#define NETWORK_INFO_TITLE		34
#define NETWORK_INFO_REFRESH_MS		1000U

/* The most rows the menu has: the switch, the state, the networks, the key field's four, the wired line, disconnect, a message. */
#define NETWORK_ROWS_MAX	(KL_BACKEND_NETWORK_SCAN_MAX + 12)

/* The kinds of row. */
enum network_row_kind {
	NETWORK_ROW_SWITCH,
	NETWORK_ROW_NOTE,
	NETWORK_ROW_SEPARATOR,
	NETWORK_ROW_AP,
	NETWORK_ROW_WIRED,
	NETWORK_ROW_CURRENT,
	NETWORK_ROW_KEY
};

/*
 * One row of the open menu: what it is, its text, its place (from the
 * menu's top), and for a network the scan's entry it shows.
 */
struct network_row {
	enum network_row_kind kind;
	char text[160];
	int32_t y;
	int32_t height;
	unsigned ap;
};

/*
 * The network's side of the system bar: the watch libkeiland-backend keeps (NULL
 * until the first tick of the desktop, and while it cannot be made), the
 * state and scan last read from it, whether the menu is open, where and
 * on which output's bar it opened, its rows, where the icon was last drawn
 * on each output's bar (the system bar's and each head's, ws113-p015) and
 * its size, the network last asked to be joined, and the text of the last
 * failed request.
 *
 * The key field: key_open while it is shown, for key_ssid, with the
 * characters typed so far in key (key_length of them, never more than
 * NETWORK_KEY_MAX, wiped whenever the field closes or the key is handed
 * to the system extension, which saves it on its thread, tells the daemon
 * and joins; the join's answer comes here as the bar's own).
 *
 * The saved networks (saved, saved_count) as the system extension's thread
 * last read them, asked for each time the menu opens (WS131 p011: nothing
 * here reads the user's store on the event loop); before the first
 * reading a network that asks for a key is joined, and a join without a
 * saved key opens the key field.
 *
 * The waiting slot: pending_request (KL_BACKEND_NETWORK_REQUEST_NONE when
 * empty) and pending_ssid, sent when the outstanding request is answered.
 *
 * connecting is the network the user chose to join, from the choice (a
 * join sent, waiting in the slot, or behind its key being saved) until the
 * join is answered or given up; empty otherwise.  The menu shows it as
 * being connected to (BUG-154).
 *
 * opened_ms is when the watch was made and heard is 1 once the daemon's
 * state has said it is reachable: until then, for NETWORK_START_GRACE_MS,
 * a daemon still starting (or busy joining the network it knows) is said
 * to be starting, not unavailable (BUG-176); grace_over is 1 once that
 * time has passed and the menu was drawn again.
 *
 * scan_holders counts who asks for scans (ws089-p021): the open menu and
 * each network object of the system extension that asked (system.c); while
 * it is not 0 the watch is asked to keep the radios scanning, and its
 * going from 0 to 1 and back is what the watch is told.  scan_fresh is 1
 * once a scan arrived since the menu opened, so that an empty list says
 * "Looking for networks..." until the first one comes.
 *
 * The details (ws099-p032): info_open while they show, when they were
 * last asked to be read (info_asked_ms, kwl_milliseconds' clock), their
 * height, their rows as last worked out (info_rows, info_count), and the
 * sample of the last reading for the rates (info_sample).  They take the
 * menu's place (menu_x, menu_y).
 *
 * It lives as long as the compositor; the menu's rows are laid out again each
 * time the menu is drawn, so they always show the state last read.
 */
struct network_view {
	struct kl_backend_network *watch;
	unsigned opened;
	uint64_t opened_ms;
	unsigned heard;
	unsigned grace_over;
	struct kl_backend_network_state state;
	struct kl_backend_network_ap scan[KL_BACKEND_NETWORK_SCAN_MAX];
	size_t scan_count;
	unsigned open;
	int32_t menu_x;
	int32_t menu_y;
	int32_t menu_height;
	struct network_row rows[NETWORK_ROWS_MAX];
	unsigned row_count;
	uint64_t logged_layout;
	unsigned output;
	struct kwl_plane_places icons;
	int32_t icon_width;
	int32_t icon_height;
	unsigned icon_logged;
	char failure[192];
	unsigned switch_wanted;
	uint64_t switch_until;
	char joining[KL_BACKEND_NETWORK_SSID_MAX];
	unsigned key_open;
	char key_ssid[KL_BACKEND_NETWORK_SSID_MAX];
	char key[NETWORK_KEY_MAX + 1U];
	size_t key_length;
	char saved[KL_BACKEND_NETWORK_SCAN_MAX][KL_BACKEND_NETWORK_SSID_MAX];
	size_t saved_count;
	unsigned pending_request;
	char pending_ssid[KL_BACKEND_NETWORK_SSID_MAX];
	char connecting[KL_BACKEND_NETWORK_SSID_MAX];
	unsigned scan_holders;
	unsigned scan_fresh;
	unsigned info_open;
	uint64_t info_asked_ms;
	int32_t info_height;
	struct kwl_network_info_row info_rows[KWL_NETWORK_INFO_ROWS];
	unsigned info_count;
	struct kwl_network_info_sample info_sample;
};

/*
 * The one view of the network.  Only the event loop's thread touches it
 * (the ticks, the drawing, the input).
 */
static struct network_view network_view;

/*
 * The characters each key types into the key field, without and with Shift
 * (US layout, as the greeter's password field); 0 for none.
 */
static const char network_plain[NETWORK_KEYS] = {
	0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
	'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
	'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' '
};
static const char network_shifted[NETWORK_KEYS] = {
	0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
	'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
	'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' '
};

static void network_open_menu(struct kwl_server *server);
static void network_place_menu(struct kwl_server *server);
static void network_close_menu(struct kwl_server *server, const char *via);
static void network_layout(struct kwl_server *server);
static unsigned network_layout_rows(struct kwl_server *server, unsigned limit);
static int network_is_current(const struct network_row *row);
static int network_in_disconnect(const struct network_row *row, int32_t x, int32_t y);
static void network_add_row(enum network_row_kind kind, const char *text, int32_t height, unsigned ap);
static void network_add_key_rows(void);
static void network_state_text(const struct kl_backend_network_state *state, char *text, size_t size);
static void network_act(struct kwl_server *server, const struct network_row *row);
static void network_request(struct kwl_server *server, unsigned request, const char *ssid);
static const struct network_row *network_row_at(int32_t x, int32_t y, int32_t *top);
static int network_in_icon(unsigned slot, int32_t x, int32_t y);
static void network_log_state(void);
static unsigned network_switch_on(void);
static void network_switch_settle(struct kwl_server *server);
static void network_log_layout(void);
static void network_draw_bars(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t bottom, unsigned lit, const float *ink, float faint);
static void network_draw_fan(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t middle, unsigned lit, const float *ink, float faint);
static void network_draw_wired(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t middle, const float *ink);
static void network_draw_row(struct kwl_server *server, VkCommandBuffer command, const struct network_row *row, int32_t top, unsigned over);
static void network_draw_row_in(struct kwl_server *server, VkCommandBuffer command, const struct network_row *row, int32_t top, unsigned over, unsigned *kept);
static void network_draw_check(struct kwl_server *server, VkCommandBuffer command, int32_t left, int32_t middle, int32_t baseline, const float *ink);
static void network_draw_disconnect(struct kwl_server *server, VkCommandBuffer command, const struct network_row *row, int32_t top, int on_button);
static void network_draw_switch(struct kwl_server *server, VkCommandBuffer command, int32_t right, int32_t middle, unsigned on);
static void network_draw_lock(struct kwl_server *server, VkCommandBuffer command, int32_t x, int32_t middle, const float *ink);
static unsigned network_strength(int rssi);
static const char *network_request_name(unsigned request);
static const char *network_request_phrase(unsigned request);
static const char *network_wifi_name(unsigned wifi);
static void network_choose_ap(struct kwl_server *server, unsigned ap);
static int network_key_saved(const char *ssid);
static void network_key_open(struct kwl_server *server, const char *ssid);
static void network_key_close(void);
static void network_key_type(struct kwl_server *server, uint32_t key);
static void network_key_submit(struct kwl_server *server);
static void network_notify_failure(struct kwl_server *server, const char *ssid);
static void network_key_wipe(void);
static void network_finished(struct kwl_server *server, unsigned request, int error);
static void network_send_waiting(struct kwl_server *server);
static void network_connecting(const char *ssid);
static void network_info_open(struct kwl_server *server);
static void network_info_close(struct kwl_server *server, const char *via);
static void network_info_place(struct kwl_server *server);
static void network_info_draw(struct kwl_server *server, VkCommandBuffer command);

/*
 * Reads what the network watch has brought since the last tick, and makes
 * the watch on the desktop's first tick.
 */
void
kwl_network_tick(
	struct kwl_server *server)
{
	unsigned changed;
	unsigned request;
	uint64_t now;
	int owned;
	int error;

	/* The watch, once (libkeiland-backend connects to the daemon when it can); holders before it are told to it. */
	if (!network_view.opened) {
		network_view.opened = 1;
		network_view.opened_ms = kwl_milliseconds();
		network_view.watch = kl_backend_network_open();
		if (network_view.watch != NULL && network_view.scan_holders != 0U)
			(void)kl_backend_network_set_scanning(network_view.watch, 1U);
	}

	/* The details, read again each second while they show. */
	if (network_view.info_open) {
		now = kwl_milliseconds();
		if (now - network_view.info_asked_ms >= NETWORK_INFO_REFRESH_MS) {
			network_view.info_asked_ms = now;
			kwl_system_bar_saved(server);
		}
	}

	/* The start's grace ran out with the daemon still silent: the menu says it is unavailable now. */
	if (!network_view.heard && !network_view.grace_over) {
		now = kwl_milliseconds();
		if (now - network_view.opened_ms >= NETWORK_START_GRACE_MS) {
			network_view.grace_over = 1U;
			server->dirty = 1;
		}
	}

	/* No watch could be made (no memory): the icon stays pale. */
	if (network_view.watch == NULL)
		return;

	/* What arrived; with nothing, the switch may still have waited long enough. */
	(void)kl_backend_network_update(network_view.watch, &changed);
	if (changed == 0) {
		network_switch_settle(server);
		return;
	}

	/* A new state redraws the icon (and the menu). */
	if ((changed & KL_BACKEND_NETWORK_CHANGED_STATE) != 0) {
		kl_backend_network_get_state(network_view.watch, &network_view.state);
		if (network_view.state.reachable)
			network_view.heard = 1U;
		network_log_state();
	}

	/* The switch follows the new state. */
	network_switch_settle(server);

	/* A new scan redraws the menu's networks. */
	if ((changed & KL_BACKEND_NETWORK_CHANGED_SCAN) != 0) {
		network_view.scan_count = kl_backend_network_get_scan(network_view.watch, network_view.scan, KL_BACKEND_NETWORK_SCAN_MAX);
		if (network_view.scan_count > KL_BACKEND_NETWORK_SCAN_MAX)
			network_view.scan_count = KL_BACKEND_NETWORK_SCAN_MAX;
		printf("KWL NETWORK scan count=%u\n", (unsigned)network_view.scan_count);
		network_view.scan_fresh = 1U;
	}

	/* The system extension's network objects hear the new state and scan (system.c). */
	kwl_system_network_changed(server, changed);

	/*
	 * A request that finished: the extension's goes to it (and what waits in
	 * the slot is sent), the system bar's says its failure and sends what
	 * waited for it.
	 */
	if ((changed & KL_BACKEND_NETWORK_CHANGED_DONE) != 0) {
		request = kl_backend_network_get_request(network_view.watch, &error);
		printf("KWL NETWORK done request=%s error=%d\n", network_request_name(request), error);
		owned = kwl_system_network_done(server, request, error);
		if (owned) {
			network_send_waiting(server);
		} else {
			network_finished(server, request, error);
		}
	}

	/* Something shown has changed. */
	server->dirty = 1;
}

/*
 * Draws the network's icon at x (its left edge) in a bar whose top is at
 * top, in the bar's ink: Wi-Fi bars or the wired tree.  The bar is the
 * system bar, or a head's on the output the pass draws (ws113-p015).
 */
void
kwl_network_draw_icon(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t top,
	const float *ink)
{
	float blue[4];
	const struct kl_backend_network_state *state;
	unsigned lit;
	unsigned index;
	int32_t middle;
	int differs;

	/* Where a click on this output's bar opens the menu (a little larger than the drawing). */
	kwl_plane_place(&network_view.icons, server->view_output, x - 5, top);
	network_view.icon_width = 30;
	network_view.icon_height = KWL_GLASS_BAR - 6;
	if (!network_view.icon_logged && server->view_output == KWL_PLANE_ANCHOR) {
		network_view.icon_logged = 1;
		printf("KWL NETWORK icon x=%d y=%d width=%d height=%d\n", x - 5, top + 3, network_view.icon_width, network_view.icon_height);
	}

	/* While the menu is open, the icon on the bar it opened from has a pale back of the accent the user chose (the bar keeps its colours). */
	middle = top + KWL_GLASS_BAR_MIDDLE;
	if (network_view.open && network_view.output == server->view_output) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.28f, blue);
		glass_draw_solid(server, command, (float)(x - 5), (float)(middle - 14), (float)network_view.icon_width, 28.0f, 7.0f, blue);
	}

	/* A wired connection is the tree. */
	state = &network_view.state;
	if (state->connected && state->kind == KL_BACKEND_NETWORK_WIRED) {
		network_draw_wired(server, command, x, middle, ink);
		return;
	}

	/* A connected Wi-Fi's fan is lit as far as its signal is strong (all without a scan of it). */
	if (state->connected && state->kind == KL_BACKEND_NETWORK_WIFI) {
		lit = 4;
		for (index = 0; index < network_view.scan_count; index++) {
			differs = strcmp(network_view.scan[index].ssid, state->ssid);
			if (differs == 0)
				lit = network_strength(network_view.scan[index].rssi);
		}

		/* The fan. */
		network_draw_fan(server, command, x, middle, lit, ink, 0.30f);
		return;
	}

	/* Anything else is a pale fan; Wi-Fi that is off is struck through. */
	network_draw_fan(server, command, x, middle, 0, ink, 0.30f);
	if (state->reachable && state->wifi == KL_BACKEND_WIFI_OFF)
		glass_draw_solid(server, command, (float)(x - 1), (float)(middle - 1), 22.0f, 2.0f, 1.0f, ink);
}

/*
 * Draws the open menu under the icon: its shadow, its glass and its rows,
 * the row under the pointer lit.
 */
void
kwl_network_draw_menu(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	struct glass_shape shape;
	const struct network_row *over;
	unsigned index;
	int32_t top;

	/* Only on the output whose bar opened the menu or the details. */
	if (network_view.output != server->view_output)
		return;

	/* The details, when they show instead. */
	if (network_view.info_open) {
		network_info_draw(server, command);
		return;
	}

	/* Only an open menu. */
	if (!network_view.open)
		return;

	/* The rows for the state last read. */
	network_layout(server);

	/* The shadow. */
	glass_shape_init(&shape, (float)network_view.menu_x, (float)network_view.menu_y + 6.0f, (float)NETWORK_MENU_WIDTH, (float)network_view.menu_height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = NETWORK_MENU_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, as white as the System Menu's popups. */
	glass_shape_init(&shape, (float)network_view.menu_x, (float)network_view.menu_y, (float)NETWORK_MENU_WIDTH, (float)network_view.menu_height);
	shape.mode = MODE_GLASS;
	shape.radius = NETWORK_MENU_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* Each row, the one under the pointer lit when it can be chosen. */
	over = network_row_at(server->pointer_x, server->pointer_y, &top);
	for (index = 0; index < network_view.row_count; index++) {
		top = network_view.menu_y + network_view.rows[index].y;
		network_draw_row(server, command, &network_view.rows[index], top, over == &network_view.rows[index]);
	}
}

/*
 * Handles a pointer button for the network: a press on the icon opens or
 * closes the menu; while it is open, a press on a row acts on it and a
 * press elsewhere closes the menu.  Returns 1 when the button was the
 * network's.
 */
int
kwl_network_button(
	struct kwl_server *server,
	uint32_t button,
	uint32_t state)
{
	const struct network_row *row;
	int32_t top;
	int inside;
	int alt;

	/* While the details show, every press closes them and goes no further; releases are theirs too. */
	if (network_view.info_open) {
		if (state != 0)
			network_info_close(server, "click");
		return 1;
	}

	/* With the menu closed, only a left press on the icon. */
	if (!network_view.open) {
		/* A release, or another button, goes on. */
		if (state == 0 || button != KWL_BUTTON_LEFT)
			return 0;

		/* A press off the icon on the bar the pointer is on goes on; on it, what opens opens under it. */
		inside = network_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
		if (!inside)
			return 0;
		network_view.output = server->pointer_output;

		/* With Alt held, the details open (ws099-p032); otherwise the menu. */
		alt = kwl_input_alt_held(server);
		if (alt) {
			network_info_open(server);
			return 1;
		}

		/* The menu. */
		network_open_menu(server);
		return 1;
	}

	/* While it is open, releases are the menu's. */
	if (state == 0)
		return 1;

	/* A press on the icon, on any output's bar, closes it. */
	inside = network_in_icon(server->pointer_output, server->pointer_x, server->pointer_y);
	if (inside) {
		network_close_menu(server, "icon");
		return 1;
	}

	/* A press outside the menu closes it and goes no further. */
	inside = 0;
	if (server->pointer_x >= network_view.menu_x &&
	    server->pointer_x < network_view.menu_x + NETWORK_MENU_WIDTH &&
	    server->pointer_y >= network_view.menu_y &&
	    server->pointer_y < network_view.menu_y + network_view.menu_height)
		inside = 1;
	if (!inside) {
		network_close_menu(server, "outside");
		return 1;
	}

	/* A left press on a row acts on it. */
	row = network_row_at(server->pointer_x, server->pointer_y, &top);
	if (row != NULL && button == KWL_BUTTON_LEFT)
		network_act(server, row);

	/* Succeeded: the press was the menu's. */
	return 1;
}

/*
 * Handles a key while the menu is open: Esc closes it, and the others are
 * the menu's too.  Returns 1 when the key was the network's.
 */
int
kwl_network_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	/* While the details show, Esc closes them, and every key is theirs. */
	if (network_view.info_open) {
		if (key == NETWORK_KEY_ESC && state != 0)
			network_info_close(server, "key");
		return 1;
	}

	/* A closed menu takes no key. */
	if (!network_view.open)
		return 0;

	/* With the key field open, the keys type into it; Esc closes the field. */
	if (network_view.key_open) {
		if (state != 0)
			network_key_type(server, key);
		return 1;
	}

	/* Esc, pressed, closes it. */
	if (key == NETWORK_KEY_ESC && state != 0)
		network_close_menu(server, "key");

	/* Succeeded: the key was the menu's. */
	return 1;
}

/*
 * Follows the pointer while the menu is open (the row under it is lit).
 * Returns 1 when the motion was the menu's.
 */
int
kwl_network_motion(
	struct kwl_server *server)
{
	/* A closed menu does not follow the pointer. */
	if (!network_view.open)
		return 0;

	/* The lit row may have changed. */
	server->dirty = 1;

	/* Succeeded: the motion was the menu's. */
	return 1;
}

/*
 * Gives the network watch for the system extension (system.c), NULL before
 * the desktop's first tick or when it could not be made.
 */
struct kl_backend_network *
kwl_network_watch(
	void)
{
	/* The watch, shared. */
	return network_view.watch;
}

/*
 * Copies the network's state as the watch last reported it (all zero
 * before the first report: the daemon not reached).
 */
void
kwl_network_state(
	struct kl_backend_network_state *state)
{
	/* The state last read. */
	*state = network_view.state;
}

/*
 * Copies up to capacity networks of the last scan and returns how many
 * were copied.
 */
size_t
kwl_network_scan(
	struct kl_backend_network_ap *aps,
	size_t capacity)
{
	size_t count;

	/* As many as there are and fit. */
	count = network_view.scan_count;
	if (count > capacity)
		count = capacity;
	memcpy(aps, network_view.scan, count * sizeof(aps[0]));

	/* Succeeded: the networks are copied. */
	return count;
}

/*
 * Says that a key the key field handed to the system extension could not
 * be saved, or its network not told or joined, and gives up the join.
 */
void
kwl_network_key_failed(
	struct kwl_server *server,
	const char *ssid,
	int error)
{
	/* The failure in the menu, and no network shown as being joined. */
	(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("Could not join {1} ({2})"), ssid, strerror(error), (const char *)NULL);
	network_connecting(NULL);
	network_notify_failure(server, ssid);
	server->dirty = 1;
	printf("KWL NETWORK key failed ssid=%s error=%d\n", ssid, error);
}

/*
 * Keeps the saved networks the system extension's thread read.
 */
void
kwl_network_saved(
	struct kwl_server *server,
	char (*ssids)[KL_BACKEND_NETWORK_SSID_MAX],
	size_t count)
{
	UNUSED_PARAMETER(server);

	/* As many as are kept. */
	if (count > KL_BACKEND_NETWORK_SCAN_MAX)
		count = KL_BACKEND_NETWORK_SCAN_MAX;
	memcpy(network_view.saved, ssids, count * sizeof(network_view.saved[0]));
	network_view.saved_count = count;
	printf("KWL NETWORK saved count=%u\n", (unsigned)count);
}

/*
 * Tells whether the menu is open (the look is not still while it is).
 */
int
kwl_network_is_open(
	void)
{
	/* Open or not, the menu or the details. */
	if (network_view.info_open)
		return 1;
	return (int)network_view.open;
}

/*
 * Counts one more holder of the scans (on 1) or one fewer (on 0): the
 * system bar's open menu, or a network object that asked (system.c).  The
 * watch is asked to keep the radios scanning while any holder is there.
 */
void
kwl_network_scan_hold(
	unsigned on)
{
	/* One more: the first holder starts the asking. */
	if (on != 0U) {
		network_view.scan_holders++;
		printf("KWL NETWORK scan holders=%u\n", network_view.scan_holders);
		if (network_view.scan_holders == 1U && network_view.watch != NULL)
			(void)kl_backend_network_set_scanning(network_view.watch, 1U);
		return;
	}

	/* One fewer than none is a holder counted twice: nothing changes. */
	if (network_view.scan_holders == 0U)
		return;

	/* One fewer: the last holder ends the asking. */
	network_view.scan_holders--;
	printf("KWL NETWORK scan holders=%u\n", network_view.scan_holders);
	if (network_view.scan_holders == 0U && network_view.watch != NULL)
		(void)kl_backend_network_set_scanning(network_view.watch, 0U);
}

/*
 * Takes a reading of the interfaces and the DNS servers the system
 * extension's thread made (system.c), and works out the details' rows from
 * it while they show.
 */
void
kwl_network_details(
	struct kwl_server *server,
	const struct kl_backend_network_link *links,
	size_t link_count,
	const char (*dns)[KL_BACKEND_NETWORK_ADDRESS_MAX],
	size_t dns_count)
{
	struct kwl_network_info_input input;
	unsigned index;

	/* Only while the details show. */
	if (!network_view.info_open)
		return;

	/* The reading's counts are of what there is; only what was copied is read. */
	if (link_count > KL_BACKEND_NETWORK_LINKS_MAX)
		link_count = KL_BACKEND_NETWORK_LINKS_MAX;
	if (dns_count > KL_BACKEND_NETWORK_DNS_MAX)
		dns_count = KL_BACKEND_NETWORK_DNS_MAX;

	/* The rows, from the watch's state and scan and this reading. */
	memset(&input, 0, sizeof(input));
	input.state = &network_view.state;
	input.scan = network_view.scan;
	input.scan_count = network_view.scan_count;
	input.links = links;
	input.link_count = link_count;
	input.dns = dns;
	input.dns_count = dns_count;
	input.now_ms = kwl_milliseconds();
	network_view.info_count = kwl_network_info_build(&input, &network_view.info_sample, network_view.info_rows, KWL_NETWORK_INFO_ROWS);

	/* Shown, and logged for the tests. */
	network_info_place(server);
	server->dirty = 1;
	printf("KWL NETWORK info rows=%u\n", network_view.info_count);
	for (index = 0; index < network_view.info_count; index++)
		printf("KWL NETWORK info row label=%s value=%s\n", network_view.info_rows[index].label, network_view.info_rows[index].value);
}

/* Opens the menu, which holds the radios scanning while it is open. */
static void
network_open_menu(
	struct kwl_server *server)
{
	/* The menu; a failure stays until the user's next request (BUG-187). */
	network_view.open = 1;
	network_view.logged_layout = 0;
	server->dirty = 1;
	printf("KWL NETWORK open\n");

	/* The saved networks, read again by the system extension's thread. */
	kwl_system_bar_saved(server);

	/* The menu holds the radios scanning until it closes (the daemon scans only a radio that is on). */
	network_view.scan_fresh = 0U;
	kwl_network_scan_hold(1U);
}

/* Closes the menu. */
static void
network_close_menu(
	struct kwl_server *server,
	const char *via)
{
	/* A key half typed goes with the menu. */
	network_key_close();

	/* The menu goes, and with it its holding of the scans; the icon's back goes too. */
	if (network_view.open)
		kwl_network_scan_hold(0U);
	network_view.open = 0;
	server->dirty = 1;
	printf("KWL NETWORK close via=%s\n", via);
}

/*
 * Lays out the rows from the state and scan last read: the switch and the
 * state, the networks (the one it is on first), the wired connection and
 * a failure.  A list too long for the screen keeps the networks that fit.
 */
static void
network_layout(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	unsigned limit;
	unsigned listed;
	int32_t room;

	/* Every network first. */
	limit = KL_BACKEND_NETWORK_SCAN_MAX;
	listed = network_layout_rows(server, limit);

	/* The menu's place: under the icon it opened from, on its output. */
	network_place_menu(server);
	(void)kwl_output_rect(server, network_view.output, &output);

	/*
	 * A menu taller than the screen under it lists fewer networks: as many
	 * rows fewer as it overflows, and one more for the note that names the
	 * rest.
	 */
	room = output.y + (int32_t)output.height - network_view.menu_y - 8;
	if (network_view.menu_height > room && listed > 0U) {
		limit = listed;
		while (limit > 0U && network_view.menu_height > room) {
			limit--;
			(void)network_layout_rows(server, limit);
		}
	}

	/* A new layout is logged for the tests that click the rows. */
	network_log_layout();
}

/*
 * Places the menu (or the details, which take its place) under the icon
 * of the output's bar it opened from, its right edge a little in from that
 * output's.
 */
static void
network_place_menu(
	struct kwl_server *server)
{
	struct kwl_plane_rect output;
	int32_t icon_x;
	int32_t top;
	int placed;

	/* The output, and the icon's place on its bar (the output's top left for one never drawn there). */
	(void)kwl_output_rect(server, network_view.output, &output);
	placed = kwl_plane_placed(&network_view.icons, network_view.output, &icon_x, &top);
	if (!placed) {
		icon_x = output.x;
		top = output.y;
	}

	/* Under the icon, inside the output. */
	network_view.menu_x = icon_x + network_view.icon_width - NETWORK_MENU_WIDTH + 60;
	if (network_view.menu_x + NETWORK_MENU_WIDTH > output.x + (int32_t)output.width - 8)
		network_view.menu_x = output.x + (int32_t)output.width - 8 - NETWORK_MENU_WIDTH;
	if (network_view.menu_x < output.x + 8)
		network_view.menu_x = output.x + 8;
	network_view.menu_y = top + KWL_GLASS_BAR + 6;
}

/*
 * Makes the rows with at most limit networks besides the one it is on and
 * the one whose key is typed (the strongest first, as the scan has them).
 * Returns how many networks besides those two the scan had to list.
 */
static unsigned
network_layout_rows(
	struct kwl_server *server,
	unsigned limit)
{
	const struct kl_backend_network_state *state;
	char text[160];
	char number[16];
	unsigned index;
	unsigned key_shown;
	unsigned others;
	unsigned shown;
	int current;
	int32_t y;
	int differs;

	(void)server;

	/* No rows yet, and no key field placed. */
	state = &network_view.state;
	network_view.row_count = 0;
	key_shown = 0;
	others = 0;
	shown = 0;

	/* The Wi-Fi's switch, with its state under it (the network being joined while the user waits for it). */
	if (state->wifi != KL_BACKEND_WIFI_ABSENT && state->reachable) {
		network_add_row(NETWORK_ROW_SWITCH, kl_tr("Wi-Fi"), NETWORK_ROW_HEIGHT, 0);
		network_state_text(state, text, sizeof(text));
		if (network_view.connecting[0] != '\0')
			(void)kl_tr_format(text, sizeof(text), kl_tr("Connecting to {1}..."), network_view.connecting, (const char *)NULL);
		network_add_row(NETWORK_ROW_NOTE, text, NETWORK_NOTE_HEIGHT, 0);
	} else {
		network_state_text(state, text, sizeof(text));
		network_add_row(NETWORK_ROW_NOTE, text, NETWORK_NOTE_HEIGHT, 0);
	}

	/* The networks, while the Wi-Fi is on. */
	if (state->reachable && state->wifi != KL_BACKEND_WIFI_ABSENT && state->wifi != KL_BACKEND_WIFI_OFF) {
		network_add_row(NETWORK_ROW_SEPARATOR, "", NETWORK_SEPARATOR, 0);

		/* No scan since the menu opened yet, or one that found nothing, says so. */
		if (!network_view.scan_fresh && network_view.scan_count == 0) {
			network_add_row(NETWORK_ROW_NOTE, kl_tr("Looking for networks..."), NETWORK_NOTE_HEIGHT, 0);
		} else if (network_view.scan_count == 0 && state->wifi != KL_BACKEND_WIFI_CONNECTED) {
			network_add_row(NETWORK_ROW_NOTE, kl_tr("No networks found"), NETWORK_NOTE_HEIGHT, 0);
		}

		/* The network it is on comes first (its own row when the scan has not found it). */
		if (state->wifi == KL_BACKEND_WIFI_CONNECTED && state->ssid[0] != '\0') {
			current = 0;
			for (index = 0; index < network_view.scan_count; index++) {
				differs = strcmp(network_view.scan[index].ssid, state->ssid);
				if (differs != 0)
					continue;
				network_add_row(NETWORK_ROW_AP, network_view.scan[index].ssid, NETWORK_ROW_HEIGHT, index);
				current = 1;
				break;
			}
			if (!current)
				network_add_row(NETWORK_ROW_CURRENT, state->ssid, NETWORK_ROW_HEIGHT, 0);
		}

		/*
		 * Every other network the scan found, as many as the limit lets;
		 * the key field of the one chosen stands right under its row
		 * (BUG-160), and that one is always listed.
		 */
		for (index = 0; index < network_view.scan_count; index++) {
			/* The network it is on is already first. */
			if (state->wifi == KL_BACKEND_WIFI_CONNECTED) {
				differs = strcmp(network_view.scan[index].ssid, state->ssid);
				if (differs == 0)
					continue;
			}

			/* The network the key is asked for is always listed; the others while the limit lets. */
			differs = 1;
			if (network_view.key_open)
				differs = strcmp(network_view.scan[index].ssid, network_view.key_ssid);
			if (differs != 0) {
				others++;
				if (shown >= limit)
					continue;
				shown++;
			}
			network_add_row(NETWORK_ROW_AP, network_view.scan[index].ssid, NETWORK_ROW_HEIGHT, index);

			/* The key field under the network it is asked for. */
			if (differs != 0)
				continue;
			network_add_key_rows();
			key_shown = 1;
		}

		/* The networks the screen had no room for are named, and found in Settings. */
		if (shown < others) {
			(void)snprintf(number, sizeof(number), "%u", others - shown);
			(void)kl_tr_format(text, sizeof(text), kl_tr("{1} more in Settings > Wi-Fi"), number, (const char *)NULL);
			network_add_row(NETWORK_ROW_NOTE, text, NETWORK_NOTE_HEIGHT, 0);
		}
	}

	/* A network the list does not show (no longer scanned, or the Wi-Fi off) keeps its field under the list. */
	if (network_view.key_open && !key_shown) {
		network_add_row(NETWORK_ROW_SEPARATOR, "", NETWORK_SEPARATOR, 0);
		network_add_key_rows();
	}

	/* The wired connection's line, after a separator. */
	network_add_row(NETWORK_ROW_SEPARATOR, "", NETWORK_SEPARATOR, 0);
	if (state->wired[0] != '\0') {
		(void)kl_tr_format(text, sizeof(text), kl_tr("Wired ({1}): connected"), state->wired, (const char *)NULL);
	} else {
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Wired: not connected"));
	}

	/* The line. */
	network_add_row(NETWORK_ROW_WIRED, text, NETWORK_ROW_HEIGHT, 0);

	/* The last failure (under the key field instead while it is open). */
	if (network_view.failure[0] != '\0' && !network_view.key_open)
		network_add_row(NETWORK_ROW_NOTE, network_view.failure, NETWORK_NOTE_HEIGHT, 0);

	/* The rows' places from the top, and the menu's height. */
	y = NETWORK_MENU_PADDING;
	for (index = 0; index < network_view.row_count; index++) {
		network_view.rows[index].y = y;
		y += network_view.rows[index].height;
	}

	/* The menu ends with its padding under the last row. */
	network_view.menu_height = y + NETWORK_MENU_PADDING;

	/* Succeeded: how many networks wanted a row of their own. */
	return others;
}

/* Adds one row (while there is room). */
static void
network_add_row(
	enum network_row_kind kind,
	const char *text,
	int32_t height,
	unsigned ap)
{
	struct network_row *row;

	/* A full menu takes no more rows. */
	if (network_view.row_count >= NETWORK_ROWS_MAX)
		return;

	/* The row. */
	row = &network_view.rows[network_view.row_count];
	memset(row, 0, sizeof(*row));
	row->kind = kind;
	(void)snprintf(row->text, sizeof(row->text), "%s", text);
	row->height = height;
	row->ap = ap;
	network_view.row_count++;
}

/* Adds the key field's rows: the field and how to finish (the network is the row above it, or named when it is not). */
static void
network_add_key_rows(
	void)
{
	char text[160];

	/* What the field is for. */
	(void)kl_tr_format(text, sizeof(text), kl_tr("Key for {1}"), network_view.key_ssid, (const char *)NULL);
	network_add_row(NETWORK_ROW_NOTE, text, NETWORK_NOTE_HEIGHT, 0);

	/* The field, and the keys that finish it. */
	network_add_row(NETWORK_ROW_KEY, "", NETWORK_ROW_HEIGHT, 0);
	network_add_row(NETWORK_ROW_NOTE, kl_tr("Enter: join   Esc: cancel"), NETWORK_NOTE_HEIGHT, 0);

	/* A key refused before it was sent (too short, not saved) is said under the field it is typed in. */
	if (network_view.failure[0] != '\0')
		network_add_row(NETWORK_ROW_NOTE, network_view.failure, NETWORK_NOTE_HEIGHT, 0);
}

/* Writes the line under the switch: what the Wi-Fi is doing, or why there is none. */
static void
network_state_text(
	const struct kl_backend_network_state *state,
	char *text,
	size_t size)
{
	/* The daemon not heard from yet just after the start: it is starting (BUG-176). */
	if (!state->reachable && !network_view.heard && !network_view.grace_over) {
		(void)snprintf(text, size, "%s", kl_tr("Starting the network service..."));
		return;
	}

	/* The daemon cannot be reached. */
	if (!state->reachable) {
		(void)snprintf(text, size, "%s", kl_tr("Network service not available"));
		return;
	}

	/* The Wi-Fi's own state. */
	switch (state->wifi) {
	case KL_BACKEND_WIFI_ABSENT:
		(void)snprintf(text, size, "%s", kl_tr("No Wi-Fi hardware"));
		break;
	case KL_BACKEND_WIFI_OFF:
		(void)snprintf(text, size, "%s", kl_tr("Wi-Fi is off"));
		break;
	case KL_BACKEND_WIFI_SEARCHING:
		(void)snprintf(text, size, "%s", kl_tr("Searching for a known network"));
		break;
	case KL_BACKEND_WIFI_CONNECTING:
		(void)kl_tr_format(text, size, kl_tr("Connecting to {1}..."), state->ssid, (const char *)NULL);
		break;
	case KL_BACKEND_WIFI_CONNECTED:
		(void)kl_tr_format(text, size, kl_tr("Connected to {1}"), state->ssid, (const char *)NULL);
		break;
	default:
		(void)snprintf(text, size, "%s", kl_tr("Not connected"));
		break;
	}
}

/* Acts on a row: the switch turns the Wi-Fi on or off, a network is joined, disconnect leaves. */
static void
network_act(
	struct kwl_server *server,
	const struct network_row *row)
{
	unsigned wanted;
	unsigned on;
	int current;
	int inside;

	/* What the row does. */
	switch (row->kind) {
	case NETWORK_ROW_SWITCH:
		/*
		 * Off turns on, anything else turns off; the switch shows the new
		 * position at once and keeps it until the state agrees (BUG-183:
		 * turning the radio on takes a second).
		 */
		on = network_switch_on();
		wanted = KL_BACKEND_NETWORK_REQUEST_WIFI_OFF;
		network_view.switch_wanted = 1U;
		if (on == 0U) {
			wanted = KL_BACKEND_NETWORK_REQUEST_WIFI_ON;
			network_view.switch_wanted = 2U;
		}

		/* Held until the answer, then a few seconds more. */
		network_view.switch_until = 0U;
		network_request(server, wanted, NULL);
		break;
	case NETWORK_ROW_AP:
	case NETWORK_ROW_CURRENT:
		/* The network it is on: its Disconnect button leaves it, the rest of its row does nothing. */
		current = network_is_current(row);
		if (current) {
			inside = network_in_disconnect(row, server->pointer_x, server->pointer_y);
			if (inside)
				network_request(server, KL_BACKEND_NETWORK_REQUEST_DISCONNECT, NULL);
			break;
		}

		/* Any other network is joined. */
		if (row->kind == NETWORK_ROW_AP)
			network_choose_ap(server, row->ap);
		break;
	default:
		/* The notes, the separators and the wired line only show. */
		break;
	}
}

/* Sends a request through libkeiland-backend, and says in the menu when it cannot be sent. */
static void
network_request(
	struct kwl_server *server,
	unsigned request,
	const char *ssid)
{
	int error;

	/* No watch, no request. */
	if (network_view.watch == NULL)
		return;

	/* The user acts again: the failure of the last action goes (BUG-187). */
	network_view.failure[0] = '\0';

	/* A join's network is kept for the failure line. */
	network_view.joining[0] = '\0';
	if (ssid != NULL)
		(void)snprintf(network_view.joining, sizeof(network_view.joining), "%s", ssid);

	/*
	 * A join shows its network as being connected to; leaving the Wi-Fi
	 * (disconnect, the switch off) ends any join the user was waiting for.
	 */
	if (request == KL_BACKEND_NETWORK_REQUEST_JOIN)
		network_connecting(network_view.joining);
	if (request == KL_BACKEND_NETWORK_REQUEST_DISCONNECT || request == KL_BACKEND_NETWORK_REQUEST_WIFI_OFF)
		network_connecting(NULL);

	/* The request; the answer comes through the ticks. */
	error = kl_backend_network_request(network_view.watch, request, ssid);
	printf("KWL NETWORK request %s ssid=%s error=%d\n", network_request_name(request), network_view.joining, error);
	server->dirty = 1;

	/* A request behind another one waits for its answer (a scan is not kept). */
	if (error == EBUSY && request != KL_BACKEND_NETWORK_REQUEST_SCAN) {
		network_view.pending_request = request;
		(void)snprintf(network_view.pending_ssid, sizeof(network_view.pending_ssid), "%s", network_view.joining);
		return;
	}

	/* A request that could not even be sent is said in the menu, and a join that was not sent is not waited for. */
	if (error != 0 && request != KL_BACKEND_NETWORK_REQUEST_SCAN) {
		(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("Could not {1} ({2})"), network_request_phrase(request), strerror(error), (const char *)NULL);
		if (request == KL_BACKEND_NETWORK_REQUEST_JOIN || request == KL_BACKEND_NETWORK_REQUEST_PROFILES)
			network_connecting(NULL);
	}
}

/* Finds the row that can be chosen under a point of the open menu, with its top. */
static const struct network_row *
network_row_at(
	int32_t x,
	int32_t y,
	int32_t *top)
{
	const struct network_row *row;
	unsigned index;

	/* Outside the menu's width, nothing. */
	if (!network_view.open)
		return NULL;
	if (x < network_view.menu_x || x >= network_view.menu_x + NETWORK_MENU_WIDTH)
		return NULL;

	/* The row the point is in, when it acts. */
	for (index = 0; index < network_view.row_count; index++) {
		row = &network_view.rows[index];
		*top = network_view.menu_y + row->y;
		if (y < *top || y >= *top + row->height)
			continue;

		/* Notes, separators and the wired line do nothing. */
		if (row->kind == NETWORK_ROW_SWITCH || row->kind == NETWORK_ROW_AP || row->kind == NETWORK_ROW_CURRENT)
			return row;
		return NULL;
	}

	/* The padding. */
	return NULL;
}

/* Tells whether a point is on the icon as last drawn on an output's bar. */
static int
network_in_icon(
	unsigned slot,
	int32_t x,
	int32_t y)
{
	int32_t icon_x;
	int32_t top;
	int placed;

	/* An icon never drawn on that output's bar has no place there. */
	placed = kwl_plane_placed(&network_view.icons, slot, &icon_x, &top);
	if (!placed)
		return 0;

	/* Its rectangle, a little in from the bar's top and bottom. */
	if (x < icon_x || x >= icon_x + network_view.icon_width)
		return 0;
	if (y < top + 3 || y >= top + 3 + network_view.icon_height)
		return 0;

	/* On it. */
	return 1;
}

/* Logs the state for the tests. */
static void
network_log_state(
	void)
{
	const struct kl_backend_network_state *state;
	const char *kind;

	/* What carries the connection. */
	state = &network_view.state;
	kind = "none";
	if (state->kind == KL_BACKEND_NETWORK_WIRED)
		kind = "wired";
	if (state->kind == KL_BACKEND_NETWORK_WIFI)
		kind = "wifi";

	/* One line. */
	printf("KWL NETWORK state reachable=%u connected=%u kind=%s interface=%s wifi=%s ssid=%s\n",
	    state->reachable, state->connected, kind, state->interface, network_wifi_name(state->wifi), state->ssid);
}

/* Logs the rows' places when they change, for the tests that click them. */
static void
network_log_layout(
	void)
{
	const struct network_row *row;
	uint64_t checksum;
	unsigned index;
	size_t at;
	int current;

	/* A checksum of the rows' kinds, places and texts, and of the Wi-Fi's state (the Disconnect button comes with it). */
	checksum = 1469598103934665603ULL;
	for (index = 0; index < network_view.row_count; index++) {
		row = &network_view.rows[index];
		checksum = (checksum ^ (uint64_t)row->kind) * 1099511628211ULL;
		checksum = (checksum ^ (uint64_t)(uint32_t)row->y) * 1099511628211ULL;
		for (at = 0; row->text[at] != '\0'; at++)
			checksum = (checksum ^ (unsigned char)row->text[at]) * 1099511628211ULL;
	}

	/* And the menu's place and the Wi-Fi's state. */
	checksum = (checksum ^ (uint64_t)(uint32_t)network_view.menu_x) * 1099511628211ULL;
	checksum = (checksum ^ (uint64_t)network_view.state.wifi) * 1099511628211ULL;

	/* Only a layout not logged yet. */
	if (checksum == network_view.logged_layout)
		return;
	network_view.logged_layout = checksum;

	/* Each row that acts, with its rectangle. */
	printf("KWL NETWORK menu x=%d y=%d width=%d height=%d rows=%u\n", network_view.menu_x, network_view.menu_y, NETWORK_MENU_WIDTH, network_view.menu_height, network_view.row_count);
	for (index = 0; index < network_view.row_count; index++) {
		row = &network_view.rows[index];
		printf("KWL NETWORK row index=%u kind=%d x=%d y=%d width=%d height=%d text=%s\n", index, (int)row->kind,
		    network_view.menu_x, network_view.menu_y + row->y, NETWORK_MENU_WIDTH, row->height, row->text);

		/* The Disconnect button of the network it is on, for the tests that click it. */
		current = network_is_current(row);
		if (current) {
			printf("KWL NETWORK disconnect x=%d y=%d width=%d height=%d ssid=%s\n",
			    network_view.menu_x + NETWORK_MENU_WIDTH - 10 - NETWORK_DISCONNECT_WIDTH,
			    network_view.menu_y + row->y + NETWORK_DISCONNECT_INSET, NETWORK_DISCONNECT_WIDTH,
			    row->height - 2 * NETWORK_DISCONNECT_INSET, row->text);
		}
	}
}

/*
 * Draws the bar's Wi-Fi fan (ws099-p034) in a 20-pixel square from x: the
 * whole fan faint, then its first lit parts (1: the dot, up to 4: the dot
 * and its three arcs) in the ink over it.  The dot and the arcs share one
 * centre in icons.c, so the dot is under the middle of the arcs.
 */
static void
network_draw_fan(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t middle,
	unsigned lit,
	const float *ink,
	float faint)
{
	float color[4];

	/* The whole fan, faint. */
	memcpy(color, ink, sizeof(color));
	color[3] = ink[3] * faint;
	glass_draw_icon(server, command, GLASS_ICON_WIFI_4, x, middle - 10, 20U, color);

	/* Nothing lit. */
	if (lit == 0U)
		return;

	/* The lit parts over it. */
	if (lit > 4U)
		lit = 4U;
	glass_draw_icon(server, command, GLASS_ICON_WIFI_1 + lit - 1U, x, middle - 10, 20U, ink);
}

/* Draws four rising bars from x, the first lit ones in the ink and the rest faint. */
static void
network_draw_bars(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t bottom,
	unsigned lit,
	const float *ink,
	float faint)
{
	float color[4];
	unsigned step;
	int32_t height;

	/* Each bar, taller to the right. */
	for (step = 0; step < 4; step++) {
		memcpy(color, ink, sizeof(color));
		if (step >= lit)
			color[3] = ink[3] * faint;
		height = 4 + (int32_t)step * 3;
		glass_draw_solid(server, command, (float)(x + (int32_t)step * 5), (float)(bottom - height), 3.0f, (float)height, 1.0f, color);
	}
}

/* Draws the wired connection's icon: a box above two, joined by lines. */
static void
network_draw_wired(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t middle,
	const float *ink)
{
	/* The upper box, and the stem from it. */
	glass_draw_solid(server, command, (float)(x + 5), (float)(middle - 9), 8.0f, 6.0f, 1.5f, ink);
	glass_draw_solid(server, command, (float)(x + 8), (float)(middle - 3), 2.0f, 3.0f, 0.0f, ink);

	/* The bar across, and the legs down. */
	glass_draw_solid(server, command, (float)(x + 2), (float)middle, 14.0f, 2.0f, 0.0f, ink);
	glass_draw_solid(server, command, (float)(x + 2), (float)middle, 2.0f, 3.0f, 0.0f, ink);
	glass_draw_solid(server, command, (float)(x + 14), (float)middle, 2.0f, 3.0f, 0.0f, ink);

	/* The two lower boxes. */
	glass_draw_solid(server, command, (float)(x - 1), (float)(middle + 3), 8.0f, 6.0f, 1.5f, ink);
	glass_draw_solid(server, command, (float)(x + 11), (float)(middle + 3), 8.0f, 6.0f, 1.5f, ink);
}

/*
 * Draws one row at top: its band when lit, and what the row shows.  A lit
 * row is drawn in the accent's colours as they are (ws179-p001): the dark
 * appearance's mapping is left out from its band on, and comes back after
 * the row.
 */
static void
network_draw_row(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct network_row *row,
	int32_t top,
	unsigned over)
{
	unsigned kept;

	/* The row, which may draw its band and ink as they are, then the mapping as it was. */
	kept = server->keep_colours;
	network_draw_row_in(server, command, row, top, over, &kept);
	kwl_accent_done(server, kept);
}

/* Draws one row's band and content for network_draw_row (kept is the mapping to come back to). */
static void
network_draw_row_in(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct network_row *row,
	int32_t top,
	unsigned over,
	unsigned *kept)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	static const float line[4] = { 0.12f, 0.16f, 0.24f, 0.16f };
	static const float field[4] = { 1.0f, 1.0f, 1.0f, 0.95f };
	float blue[4];
	float frame[4];
	const struct kl_backend_network_ap *ap;
	const char *connecting;
	char stars[NETWORK_KEY_MAX + 2U];
	size_t count;
	float ink[4];
	int32_t left;
	int32_t right;
	int32_t middle;
	int32_t baseline;
	int32_t width;
	int differs;
	int joining;
	int current;
	int on_button;
	unsigned framed;

	/* The row's edges, its middle and the text's baseline. */
	left = network_view.menu_x;
	right = network_view.menu_x + NETWORK_MENU_WIDTH - 14;
	middle = top + row->height / 2;
	baseline = middle + 5;

	/* A separator is a thin line. */
	if (row->kind == NETWORK_ROW_SEPARATOR) {
		glass_draw_solid(server, command, (float)(left + 12), (float)(top + row->height / 2), (float)(NETWORK_MENU_WIDTH - 24), 1.0f, 0.0f, line);
		return;
	}

	/* A note is soft text. */
	if (row->kind == NETWORK_ROW_NOTE) {
		glass_draw_text(server, command, SIZE_BAR, left + 14, baseline, row->text, NETWORK_MENU_WIDTH - 28, soft);
		return;
	}

	/* The lit row is a band of the accent with its ink, as they are (not the network it is on: only its button acts). */
	memcpy(ink, dark, sizeof(ink));
	current = network_is_current(row);
	if (current && network_view.connecting[0] == '\0')
		over = 0;
	if (over) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, blue);
		kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, ink);
		*kept = kwl_accent_as_is(server);
		glass_draw_solid(server, command, (float)(left + 5), (float)(top + 1), (float)(NETWORK_MENU_WIDTH - 10), (float)(row->height - 2), 6.0f, blue);
	}

	/* The switch's row: its label, then the switch at the right in the popup's colours. */
	if (row->kind == NETWORK_ROW_SWITCH) {
		glass_draw_text(server, command, SIZE_TITLE, left + 14, baseline + 1, row->text, 160, ink);
		kwl_accent_done(server, *kept);
		network_draw_switch(server, command, right, middle, network_switch_on());
		return;
	}

	/*
	 * The network it is on (BUG-148): the check, its SSID and a Disconnect
	 * button at the right of its row, lit while the pointer is on it.
	 */
	if (current && network_view.connecting[0] == '\0') {
		network_draw_check(server, command, left, middle, baseline, ink);
		glass_draw_text(server, command, SIZE_BAR, left + 32, baseline, row->text, NETWORK_MENU_WIDTH - 32 - NETWORK_DISCONNECT_WIDTH - 20, ink);
		on_button = network_in_disconnect(row, server->pointer_x, server->pointer_y);
		network_draw_disconnect(server, command, row, top, on_button);
		return;
	}

	/*
	 * A network: its SSID, a padlock and its signal; while one is being
	 * joined, it says so in place of the padlock and the signal.
	 */
	if (row->kind == NETWORK_ROW_AP) {
		ap = &network_view.scan[row->ap];
		joining = 0;
		if (network_view.connecting[0] != '\0') {
			/* The network the user chose and waits for. */
			differs = strcmp(ap->ssid, network_view.connecting);
			if (differs == 0)
				joining = 1;
		} else if (network_view.state.wifi == KL_BACKEND_WIFI_CONNECTING) {
			/* The network the daemon is joining on its own (a saved one found again). */
			differs = strcmp(ap->ssid, network_view.state.ssid);
			if (differs == 0)
				joining = 1;
		}

		/*
		 * The row of the network being joined: its SSID and "Connecting..."
		 * at the right, soft on the glass and white on the lit row's blue
		 * (soft there could hardly be read).
		 */
		if (joining) {
			connecting = kl_tr("Connecting...");
			width = glass_text_width(server, SIZE_BAR, connecting);
			glass_draw_text(server, command, SIZE_BAR, left + 32, baseline, row->text, NETWORK_MENU_WIDTH - 32 - 24 - width, ink);
			if (over) {
				glass_draw_text(server, command, SIZE_BAR, right - width, baseline, connecting, width + 2, ink);
			} else {
				glass_draw_text(server, command, SIZE_BAR, right - width, baseline, connecting, width + 2, soft);
			}
			return;
		}

		/* The SSID, the padlock of a network that asks for a key, and the signal. */
		glass_draw_text(server, command, SIZE_BAR, left + 32, baseline, row->text, NETWORK_MENU_WIDTH - 32 - 64, ink);
		if (ap->secured)
			network_draw_lock(server, command, right - 42, middle, ink);
		network_draw_bars(server, command, right - 18, middle + 7, network_strength(ap->rssi), ink, 0.25f);
		return;
	}

	/* The key field: a pale box with a star for each character typed and the cursor. */
	if (row->kind == NETWORK_ROW_KEY) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 0.70f, frame);
		framed = kwl_accent_as_is(server);
		glass_draw_solid(server, command, (float)(left + 11), (float)(top + 2), (float)(NETWORK_MENU_WIDTH - 22), (float)(row->height - 4), 6.0f, frame);
		kwl_accent_done(server, framed);
		glass_draw_solid(server, command, (float)(left + 12), (float)(top + 3), (float)(NETWORK_MENU_WIDTH - 24), (float)(row->height - 6), 5.0f, field);

		/* The key is never drawn, only its length. */
		for (count = 0U; count < network_view.key_length; count++)
			stars[count] = '*';
		stars[count] = '|';
		stars[count + 1U] = '\0';
		glass_draw_text(server, command, SIZE_BAR, left + 20, baseline, stars, NETWORK_MENU_WIDTH - 40, dark);
		return;
	}

	/* The wired line and disconnect are plain text. */
	glass_draw_text(server, command, SIZE_BAR, left + 14, baseline, row->text, NETWORK_MENU_WIDTH - 28, ink);
}

/* Draws the Wi-Fi's switch ending at right: a pill, blue when on, with its knob. */
static void
network_draw_switch(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t right,
	int32_t middle,
	unsigned on)
{
	static const float grey[4] = { 0.62f, 0.66f, 0.72f, 1.0f };
	static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	float blue[4];
	unsigned kept;
	int32_t x;

	/* Off: a grey pill with its knob at the left, in the popup's colours. */
	x = right - 36;
	if (!on) {
		glass_draw_solid(server, command, (float)x, (float)(middle - 10), 36.0f, 20.0f, 10.0f, grey);
		glass_draw_solid(server, command, (float)(x + 2), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
		return;
	}

	/* On: a pill of the accent the user chose with a white knob at the right, as they are (ws179-p001). */
	kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, blue);
	kept = kwl_accent_as_is(server);
	glass_draw_solid(server, command, (float)x, (float)(middle - 10), 36.0f, 20.0f, 10.0f, blue);
	glass_draw_solid(server, command, (float)(x + 18), (float)(middle - 8), 16.0f, 16.0f, 8.0f, white);
	kwl_accent_done(server, kept);
}

/* Draws a small padlock: a body and its shackle. */
static void
network_draw_lock(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t x,
	int32_t middle,
	const float *ink)
{
	struct glass_shape shape;

	/* The shackle, a ring over the body. */
	glass_shape_init(&shape, (float)(x + 2), (float)(middle - 7), 6.0f, 8.0f);
	shape.quad[0] -= 1.0f;
	shape.quad[1] -= 1.0f;
	shape.quad[2] += 2.0f;
	shape.quad[3] += 2.0f;
	shape.mode = MODE_RING;
	shape.radius = 3.0f;
	shape.soft = 1.2f;
	memcpy(shape.color, ink, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);

	/* The body. */
	glass_draw_solid(server, command, (float)x, (float)(middle - 2), 10.0f, 8.0f, 1.5f, ink);
}

/* Tells how many of four bars a signal lights. */
static unsigned
network_strength(
	int rssi)
{
	/* Strong, good, fair, weak. */
	if (rssi >= -55)
		return 4;
	if (rssi >= -67)
		return 3;
	if (rssi >= -78)
		return 2;

	/* Anything weaker still shows one. */
	return 1;
}

/* Lets the switch show the state again once it agrees with what was asked, or a while after the answer (BUG-183). */
static void
network_switch_settle(
	struct kwl_server *server)
{
	uint64_t now;
	int agrees;

	/* Nothing asked. */
	if (network_view.switch_wanted == 0U)
		return;

	/* The state agrees, or the answer came long enough ago. */
	agrees = 0;
	if ((network_view.switch_wanted == 2U) == (network_view.state.wifi != KL_BACKEND_WIFI_OFF))
		agrees = 1;
	now = kwl_milliseconds();
	if (network_view.switch_until != 0U && now >= network_view.switch_until)
		agrees = 1;
	if (!agrees)
		return;

	/* The state again. */
	network_view.switch_wanted = 0U;
	network_view.switch_until = 0U;
	server->dirty = 1;
	printf("KWL NETWORK switch settled wifi=%u\n", network_view.state.wifi);
}

/* Tells whether the switch shows on: what was asked while it waits (BUG-183), else the state. */
static unsigned
network_switch_on(
	void)
{
	/* Asked on, or off. */
	if (network_view.switch_wanted == 2U)
		return 1U;
	if (network_view.switch_wanted == 1U)
		return 0U;

	/* The state. */
	if (network_view.state.wifi != KL_BACKEND_WIFI_OFF)
		return 1U;
	return 0U;
}

/* Names a request, for the log and the failure line. */
static const char *
network_request_name(
	unsigned request)
{
	/* Each request's verb. */
	switch (request) {
	case KL_BACKEND_NETWORK_REQUEST_SCAN:
		return "scan";
	case KL_BACKEND_NETWORK_REQUEST_JOIN:
		return "join";
	case KL_BACKEND_NETWORK_REQUEST_DISCONNECT:
		return "disconnect";
	case KL_BACKEND_NETWORK_REQUEST_WIFI_ON:
		return "turn Wi-Fi on";
	case KL_BACKEND_NETWORK_REQUEST_WIFI_OFF:
		return "turn Wi-Fi off";
	case KL_BACKEND_NETWORK_REQUEST_PROFILES:
		return "save the key";
	case KL_BACKEND_NETWORK_REQUEST_WIRED:
		return "configure wired";
	default:
		break;
	}

	/* No request. */
	return "none";
}

/* Says what a request was meant to do, in the desktop's language, for the failure line ("Could not ..."). */
static const char *
network_request_phrase(
	unsigned request)
{
	/* Each request's words. */
	switch (request) {
	case KL_BACKEND_NETWORK_REQUEST_SCAN:
		return kl_trc("network request", "scan");
	case KL_BACKEND_NETWORK_REQUEST_JOIN:
		return kl_trc("network request", "join");
	case KL_BACKEND_NETWORK_REQUEST_DISCONNECT:
		return kl_trc("network request", "disconnect");
	case KL_BACKEND_NETWORK_REQUEST_WIFI_ON:
		return kl_trc("network request", "turn Wi-Fi on");
	case KL_BACKEND_NETWORK_REQUEST_WIFI_OFF:
		return kl_trc("network request", "turn Wi-Fi off");
	case KL_BACKEND_NETWORK_REQUEST_PROFILES:
		return kl_trc("network request", "save the key");
	case KL_BACKEND_NETWORK_REQUEST_WIRED:
		return kl_trc("network request", "configure wired");
	default:
		break;
	}

	/* No request. */
	return kl_trc("network request", "do that");
}

/* Names a Wi-Fi state, for the log. */
static const char *
network_wifi_name(
	unsigned wifi)
{
	/* Each state's name. */
	switch (wifi) {
	case KL_BACKEND_WIFI_OFF:
		return "off";
	case KL_BACKEND_WIFI_SEARCHING:
		return "searching";
	case KL_BACKEND_WIFI_CONNECTING:
		return "connecting";
	case KL_BACKEND_WIFI_CONNECTED:
		return "connected";
	case KL_BACKEND_WIFI_DISCONNECTED:
		return "disconnected";
	default:
		break;
	}

	/* No radio. */
	return "absent";
}

/*
 * Acts on a network chosen in the menu: one that asks for a key and has
 * none saved opens the key field; any other is joined with its saved
 * profile.
 */
static void
network_choose_ap(
	struct kwl_server *server,
	unsigned ap)
{
	const struct kl_backend_network_ap *chosen;
	int saved;

	/* The network as the scan last reported it. */
	chosen = &network_view.scan[ap];

	/* A network with a saved key, or one that asks for none, is joined. */
	saved = network_key_saved(chosen->ssid);
	if (saved || !chosen->secured) {
		network_request(server, KL_BACKEND_NETWORK_REQUEST_JOIN, chosen->ssid);
		return;
	}

	/* A key is asked for. */
	network_key_open(server, chosen->ssid);
}

/* Tells whether the user's store had a key for a network when the system extension's thread last read it. */
static int
network_key_saved(
	const char *ssid)
{
	size_t index;
	int differs;

	/* Looks for this one among the saved networks. */
	for (index = 0; index < network_view.saved_count; index++) {
		differs = strcmp(network_view.saved[index], ssid);
		if (differs == 0)
			return 1;
	}

	/* Not saved, or not read yet. */
	return 0;
}

/* Opens the key field for a network, empty. */
static void
network_key_open(
	struct kwl_server *server,
	const char *ssid)
{
	/* The field, for this network, with nothing typed and no failure from before. */
	network_key_wipe();
	(void)snprintf(network_view.key_ssid, sizeof(network_view.key_ssid), "%s", ssid);
	network_view.key_open = 1;
	network_view.failure[0] = '\0';
	server->dirty = 1;
	printf("KWL NETWORK key open ssid=%s\n", network_view.key_ssid);
}

/* Closes the key field, wiping what was typed. */
static void
network_key_close(
	void)
{
	/* Nothing of the key is kept. */
	network_key_wipe();
	network_view.key_open = 0;
}

/* Types one key into the field: a character, Backspace, Enter or Esc. */
static void
network_key_type(
	struct kwl_server *server,
	uint32_t key)
{
	char character;
	int shift;

	/* Esc closes the field and leaves the menu open. */
	if (key == NETWORK_KEY_ESC) {
		network_key_close();
		server->dirty = 1;
		printf("KWL NETWORK key cancel\n");
		return;
	}

	/* Enter saves the key and joins. */
	if (key == NETWORK_KEY_ENTER || key == NETWORK_KEY_KPENTER) {
		network_key_submit(server);
		return;
	}

	/* Backspace takes back the last character. */
	if (key == NETWORK_KEY_BACKSPACE) {
		/* An empty field has nothing to take back. */
		if (network_view.key_length != 0U) {
			network_view.key_length--;
			network_view.key[network_view.key_length] = '\0';
		}
		server->dirty = 1;
		return;
	}

	/* Keys beyond the table type nothing. */
	if (key >= NETWORK_KEYS)
		return;

	/* Shift, and Caps Lock for the letters. */
	shift = 0;
	if ((server->modifiers & NETWORK_SHIFT) != 0U)
		shift = 1;
	character = network_plain[key];
	if ((server->locked_modifiers & NETWORK_CAPS) != 0U && character >= 'a' && character <= 'z')
		shift = !shift;
	if (shift)
		character = network_shifted[key];
	if (character == 0)
		return;

	/* A full field takes no more. */
	if (network_view.key_length >= NETWORK_KEY_MAX)
		return;

	/* The character. */
	network_view.key[network_view.key_length] = character;
	network_view.key_length++;
	network_view.key[network_view.key_length] = '\0';
	server->dirty = 1;
}

/*
 * Saves the key typed in the user's store, wipes it, and tells the daemon
 * the saved networks changed; the join follows that answer.
 */
static void
network_key_submit(
	struct kwl_server *server)
{
	int error;

	/* A WPA key is 8 to 63 characters; a shorter one is said and kept to finish. */
	if (network_view.key_length < NETWORK_KEY_MIN) {
		(void)snprintf(network_view.failure, sizeof(network_view.failure), "%s", kl_tr("The key must be 8 to 63 characters"));
		server->dirty = 1;
		return;
	}

	/* The key, handed to the system extension's thread, which saves it, tells the daemon and joins. */
	error = kwl_system_bar_save_key(server, network_view.key_ssid, network_view.key);
	network_key_wipe();
	if (error != 0) {
		(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("Could not save the key ({1})"), strerror(error), (const char *)NULL);
		printf("KWL NETWORK key save error=%d\n", error);
		server->dirty = 1;
		return;
	}

	/* The log line the tests read (never the key). */
	printf("KWL NETWORK key handed ssid=%s\n", network_view.key_ssid);

	/* The field closes; the menu says it is connecting from now, and the join's answer comes as the bar's. */
	network_view.key_open = 0;
	(void)snprintf(network_view.joining, sizeof(network_view.joining), "%s", network_view.key_ssid);
	network_connecting(network_view.key_ssid);
	server->dirty = 1;
}

/*
 * Tells a join that failed by a notification of the compositor's own
 * (WS156, plan/ws156/phase001/phase.md section 6): "Could not join NAME",
 * with the menu's failure line as its body.
 */
static void
network_notify_failure(
	struct kwl_server *server,
	const char *ssid)
{
	char title[KWL_NOTIFY_TITLE_MAX + 1U];

	/* The title, then the notification. */
	(void)kl_tr_format(title, sizeof(title), kl_tr("Could not join {1}"), ssid, (const char *)NULL);
	(void)kwl_notify_system_post(server, title, network_view.failure, 0U, NULL);
}

/* Wipes the key field's characters, every byte of it. */
static void
network_key_wipe(
	void)
{
	volatile char *byte;
	size_t index;

	/* Through a volatile pointer, so the stores are not left out as dead. */
	byte = network_view.key;
	for (index = 0; index < sizeof(network_view.key); index++)
		byte[index] = '\0';
	network_view.key_length = 0U;
}

/*
 * Follows a request that finished: says its failure in the menu, sends the
 * join that waited for a saved key to reach the daemon, opens the key field
 * for a network the daemon has no key for, and sends the request that
 * waited in the slot.
 */
static void
network_finished(
	struct kwl_server *server,
	unsigned request,
	int error)
{
	unsigned index;
	int differs;

	/* No failure from before. */
	network_view.failure[0] = '\0';

	/* The switch's answer: a failure puts it back at once, a success leaves the state a few seconds to agree. */
	if (request == KL_BACKEND_NETWORK_REQUEST_WIFI_ON || request == KL_BACKEND_NETWORK_REQUEST_WIFI_OFF) {
		network_view.switch_until = kwl_milliseconds() + NETWORK_SWITCH_HOLD_MS;
		if (error != 0)
			network_view.switch_wanted = 0U;
		server->dirty = 1;
	}

	/* A join answered, joined or not, is no longer waited for (its state, or its failure, shows now). */
	if (request == KL_BACKEND_NETWORK_REQUEST_JOIN)
		network_connecting(NULL);

	/* A join of a network that asks for a key, without one, asks for it. */
	if (error == ENOENT && request == KL_BACKEND_NETWORK_REQUEST_JOIN) {
		for (index = 0; index < network_view.scan_count; index++) {
			/* The scan's entry of the network, when it asks for a key. */
			differs = strcmp(network_view.scan[index].ssid, network_view.joining);
			if (differs == 0 && network_view.scan[index].secured)
				network_key_open(server, network_view.joining);
		}

		/* A network that asks for no key, or is no longer seen, says so. */
		if (!network_view.key_open)
			(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("Could not join {1}: no saved key"), network_view.joining, (const char *)NULL);
	} else if (error == EPERM &&
	    request == KL_BACKEND_NETWORK_REQUEST_JOIN &&
	    network_view.state.wifi == KL_BACKEND_WIFI_OFF) {
		/* networkd refuses a join while Wi-Fi is off. */
		(void)snprintf(network_view.failure, sizeof(network_view.failure), "%s", kl_tr("Wi-Fi is off; turn it on to join"));
	} else if (error == EPERM) {
		/* Only root and the network group may control Wi-Fi (2026-10-02, ws005-p019). */
		(void)snprintf(network_view.failure, sizeof(network_view.failure), "%s", kl_tr("This account may not control Wi-Fi"));
	} else if (error == EACCES && request == KL_BACKEND_NETWORK_REQUEST_JOIN) {
		/* The network refused the key in its handshake (BUG-157, BUG-187): said until the user acts again. */
		(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("{1} did not accept the key"), network_view.joining, (const char *)NULL);
	} else if (error != 0 && request != KL_BACKEND_NETWORK_REQUEST_SCAN) {
		/* Anything else that failed says the errno's text. */
		(void)kl_tr_format(network_view.failure, sizeof(network_view.failure), kl_tr("Could not {1} ({2})"), network_request_phrase(request), strerror(error), (const char *)NULL);
	}

	/* A join that failed is told by a notification too, the menu's words its body (WS156); one that asks for a key has not failed. */
	if (request == KL_BACKEND_NETWORK_REQUEST_JOIN &&
	    error != 0 &&
	    !network_view.key_open &&
	    network_view.failure[0] != '\0')
		network_notify_failure(server, network_view.joining);

	/* What waited in the slot is sent now. */
	network_send_waiting(server);
}

/* Sends the request that waited in the slot, if one did. */
static void
network_send_waiting(
	struct kwl_server *server)
{
	unsigned waiting;

	/* Nothing waits. */
	waiting = network_view.pending_request;
	if (waiting == KL_BACKEND_NETWORK_REQUEST_NONE)
		return;
	network_view.pending_request = KL_BACKEND_NETWORK_REQUEST_NONE;

	/* A join names its network; the other requests take none. */
	if (network_view.pending_ssid[0] != '\0') {
		network_request(server, waiting, network_view.pending_ssid);
	} else {
		network_request(server, waiting, NULL);
	}
}

/*
 * Sets the network the menu shows as being connected to (NULL: none), and
 * logs each change for the tests.
 */
static void
network_connecting(
	const char *ssid)
{
	int differs;

	/* None, when none was shown: nothing changes. */
	if (ssid == NULL && network_view.connecting[0] == '\0')
		return;

	/* The same network again: nothing changes. */
	if (ssid != NULL) {
		differs = strcmp(ssid, network_view.connecting);
		if (differs == 0)
			return;
	}

	/* The network, or none. */
	network_view.connecting[0] = '\0';
	if (ssid != NULL)
		(void)snprintf(network_view.connecting, sizeof(network_view.connecting), "%s", ssid);

	/* The log line the tests read. */
	printf("KWL NETWORK connecting ssid=%s\n", network_view.connecting);
}

/* Tells whether a row is the network the Wi-Fi is on (a network row of its SSID, or its own row). */
static int
network_is_current(
	const struct network_row *row)
{
	const struct kl_backend_network_ap *ap;
	int differs;

	/* Only while connected. */
	if (network_view.state.wifi != KL_BACKEND_WIFI_CONNECTED)
		return 0;

	/* The row made for it when the scan did not find it. */
	if (row->kind == NETWORK_ROW_CURRENT)
		return 1;

	/* A network row of the same SSID. */
	if (row->kind != NETWORK_ROW_AP)
		return 0;
	ap = &network_view.scan[row->ap];
	differs = strcmp(ap->ssid, network_view.state.ssid);
	if (differs != 0)
		return 0;

	/* Succeeded: the row is the network it is on. */
	return 1;
}

/* Tells whether a point of the screen is on the Disconnect button of a row (its right end). */
static int
network_in_disconnect(
	const struct network_row *row,
	int32_t x,
	int32_t y)
{
	int32_t right;
	int32_t top;

	/* The button's rectangle. */
	right = network_view.menu_x + NETWORK_MENU_WIDTH - 10;
	top = network_view.menu_y + row->y + NETWORK_DISCONNECT_INSET;

	/* Outside it. */
	if (x < right - NETWORK_DISCONNECT_WIDTH || x >= right)
		return 0;
	if (y < top || y >= top + row->height - 2 * NETWORK_DISCONNECT_INSET)
		return 0;

	/* Succeeded: on the button. */
	return 1;
}

/* Draws the check of the network it is on, or a small square without the glyph. */
static void
network_draw_check(
	struct kwl_server *server,
	VkCommandBuffer command,
	int32_t left,
	int32_t middle,
	int32_t baseline,
	const float *ink)
{
	int present;

	/* The font's check mark, when it has one. */
	present = glass_glyph_advance(server, SIZE_BAR, GLASS_CHECK_GLYPH);
	if (present > 0) {
		glass_draw_glyph(server, command, SIZE_BAR, GLASS_CHECK_GLYPH, left + 12, baseline, ink);
	} else {
		glass_draw_solid(server, command, (float)(left + 13), (float)(middle - 4), 8.0f, 8.0f, 2.0f, ink);
	}
}

/* Draws the Disconnect button at the right of the row of the network it is on: an outline, filled blue under the pointer. */
static void
network_draw_disconnect(
	struct kwl_server *server,
	VkCommandBuffer command,
	const struct network_row *row,
	int32_t top,
	int on_button)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float edge[4] = { 0.12f, 0.16f, 0.24f, 0.28f };
	static const float face[4] = { 1.0f, 1.0f, 1.0f, 0.92f };
	float blue[4];
	float on_blue[4];
	const float *ink;
	unsigned kept;
	int32_t x;
	int32_t y;
	int32_t height;

	/* The button's rectangle. */
	x = network_view.menu_x + NETWORK_MENU_WIDTH - 10 - NETWORK_DISCONNECT_WIDTH;
	y = top + NETWORK_DISCONNECT_INSET;
	height = row->height - 2 * NETWORK_DISCONNECT_INSET;

	/* Its face: the accent the user chose under the pointer (as it is), else white within a thin edge. */
	ink = dark;
	kept = server->keep_colours;
	if (on_button) {
		kwl_accent_colour(server, server->dark, KWL_ACCENT_FILL, 1.0f, blue);
		kwl_accent_colour(server, server->dark, KWL_ACCENT_INK, 1.0f, on_blue);
		kept = kwl_accent_as_is(server);
		glass_draw_solid(server, command, (float)x, (float)y, (float)NETWORK_DISCONNECT_WIDTH, (float)height, 6.0f, blue);
		ink = on_blue;
	} else {
		glass_draw_solid(server, command, (float)x, (float)y, (float)NETWORK_DISCONNECT_WIDTH, (float)height, 6.0f, edge);
		glass_draw_solid(server, command, (float)(x + 1), (float)(y + 1), (float)(NETWORK_DISCONNECT_WIDTH - 2), (float)(height - 2), 5.0f, face);
	}

	/* Its icon, the cross of leaving, in the middle (the tests name the button Disconnect). */
	glass_draw_icon(server, command, GLASS_ICON_CLOSE, x + (NETWORK_DISCONNECT_WIDTH - NETWORK_DISCONNECT_ICON) / 2, y + (height - NETWORK_DISCONNECT_ICON) / 2, NETWORK_DISCONNECT_ICON, ink);
	kwl_accent_done(server, kept);
}

/*
 * Opens the details: the title until the first reading comes, which is
 * asked for at once.
 */
static void
network_info_open(
	struct kwl_server *server)
{
	/* No reading yet: the title alone, and no rate from an earlier showing. */
	network_view.info_open = 1U;
	memset(&network_view.info_sample, 0, sizeof(network_view.info_sample));
	network_view.info_count = 1U;
	(void)snprintf(network_view.info_rows[0].label, sizeof(network_view.info_rows[0].label), "%s", "");
	(void)snprintf(network_view.info_rows[0].value, sizeof(network_view.info_rows[0].value), "%s", "Network");
	network_info_place(server);
	server->dirty = 1;
	printf("KWL NETWORK info open\n");

	/* The first reading. */
	network_view.info_asked_ms = kwl_milliseconds();
	kwl_system_bar_saved(server);
}

/* Closes the details. */
static void
network_info_close(
	struct kwl_server *server,
	const char *via)
{
	/* Gone. */
	network_view.info_open = 0U;
	server->dirty = 1;
	printf("KWL NETWORK info close via=%s\n", via);
}

/* Places the details where the menu goes, as tall as their rows. */
static void
network_info_place(
	struct kwl_server *server)
{
	/* Under the icon, where the menu goes. */
	network_place_menu(server);

	/* The title, then the rows. */
	network_view.info_height = NETWORK_MENU_PADDING + NETWORK_INFO_TITLE + NETWORK_MENU_PADDING;
	if (network_view.info_count > 1U)
		network_view.info_height += (int32_t)(network_view.info_count - 1U) * NETWORK_INFO_ROW;
}

/*
 * Draws the details: the shadow and the glass of the menu, the title, and
 * each row with its label on the left and its value on the right.
 */
static void
network_info_draw(
	struct kwl_server *server,
	VkCommandBuffer command)
{
	static const float dark[4] = { 0.12f, 0.16f, 0.24f, 1.0f };
	static const float soft[4] = { 0.40f, 0.46f, 0.56f, 1.0f };
	static const float line[4] = { 0.12f, 0.16f, 0.24f, 0.16f };
	const struct kwl_network_info_row *row;
	const char *value;
	struct glass_shape shape;
	unsigned index;
	int32_t left;
	int32_t right;
	int32_t width;
	int32_t baseline;
	int32_t value_x;

	/* The shadow, as the menu's. */
	glass_shape_init(&shape, (float)network_view.menu_x, (float)network_view.menu_y + 6.0f, (float)NETWORK_MENU_WIDTH, (float)network_view.info_height);
	shape.quad[0] -= 40.0f;
	shape.quad[1] -= 40.0f;
	shape.quad[2] += 80.0f;
	shape.quad[3] += 80.0f;
	shape.mode = MODE_SHADOW;
	shape.radius = NETWORK_MENU_RADIUS;
	shape.soft = 18.0f;
	shape.color[0] = 0.10f;
	shape.color[1] = 0.18f;
	shape.color[2] = 0.35f;
	shape.color[3] = 0.24f;
	glass_shape_draw(server, command, &shape);

	/* The glass, as the menu's. */
	glass_shape_init(&shape, (float)network_view.menu_x, (float)network_view.menu_y, (float)NETWORK_MENU_WIDTH, (float)network_view.info_height);
	shape.mode = MODE_GLASS;
	shape.radius = NETWORK_MENU_RADIUS;
	shape.color[0] = 1.0f;
	shape.color[1] = 1.0f;
	shape.color[2] = 1.0f;
	shape.color[3] = 0.86f;
	shape.edge = 0.85f;
	glass_shape_draw(server, command, &shape);

	/* The title, and a line under it. */
	left = network_view.menu_x + 14;
	right = network_view.menu_x + NETWORK_MENU_WIDTH - 14;
	baseline = network_view.menu_y + NETWORK_MENU_PADDING + NETWORK_INFO_TITLE / 2 + 6;
	if (network_view.info_count > 0U)
		glass_draw_text(server, command, SIZE_TITLE, left, baseline, kl_tr(network_view.info_rows[0].value), NETWORK_MENU_WIDTH - 28, dark);
	glass_draw_solid(server, command, (float)(left - 2), (float)(network_view.menu_y + NETWORK_MENU_PADDING + NETWORK_INFO_TITLE - 1),
			 (float)(NETWORK_MENU_WIDTH - 24), 1.0f, 0.0f, line);

	/* Each row: the label soft at the left, the value dark at the right (cut at the label's column when too long). */
	baseline = network_view.menu_y + NETWORK_MENU_PADDING + NETWORK_INFO_TITLE + NETWORK_INFO_ROW / 2 + 5;
	for (index = 1; index < network_view.info_count; index++) {
		row = &network_view.info_rows[index];
		glass_draw_text(server, command, SIZE_BAR, left, baseline, kl_tr(row->label), 104, soft);
		value = kl_tr(row->value);
		width = glass_text_width(server, SIZE_BAR, value);
		value_x = right - width;
		if (value_x < left + 108)
			value_x = left + 108;
		glass_draw_text(server, command, SIZE_BAR, value_x, baseline, value, right - value_x, dark);
		baseline += NETWORK_INFO_ROW;
	}
}
