/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth page (ws143-p006, plan/ws143/phase006/phase.md section 7):
 * Bluetooth through the desktop (kl_system_bluetooth_*), never the service
 * itself.
 *
 *   Bluetooth      the switch (when the service can turn the controller
 *                  off), the state in words, and why there is none (no
 *                  service, no adapter, its firmware missing).
 *   My Devices     the paired devices: name, kind, connected, battery, a
 *                  note on a pairing of the old way; Connect or Disconnect
 *                  (when the service can) and Remove.
 *   Other Devices  while the page shows and the controller is on, the
 *                  devices a scan finds, with Pair.  A pairing's question
 *                  is the desktop's own window's, not this page's.
 *
 * While the page shows, the state is watched and the scan is asked again
 * each SE_BLUETOOTH_RENEW_MS (the desktop lets an asking go after a
 * minute).  Each request's answer is a line under the cards.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The controls (hit indices): the switch, and each device's buttons by its place in the drawn list. */
#define BLUETOOTH_SWITCH	1
#define BLUETOOTH_PAIR_FIRST	100
#define BLUETOOTH_FORGET_FIRST	200
#define BLUETOOTH_CONNECT_FIRST	300

/* The rows' heights, the padding and the text sizes. */
#define BLUETOOTH_ROW		56
#define BLUETOOTH_PAD		20
#define BLUETOOTH_TEXT_ROW	14U
#define BLUETOOTH_TEXT_SUB	12U

/* How often the scan is asked again while the page shows (the desktop holds an asking a minute). */
#define SE_BLUETOOTH_RENEW_MS	30000U

static int bluetooth_available(const struct se_app *app);
static int bluetooth_power_card(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, const struct kl_bluetooth_state *state);
static int bluetooth_list(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width, int paired);
static void bluetooth_follow(struct se_app *app, int shown);
static void bluetooth_ask(struct se_app *app, unsigned action, size_t drawn);
static void bluetooth_asked(struct se_app *app, int error, const char *doing);
static const char *bluetooth_state_words(const struct kl_bluetooth_state *state);
static const char *bluetooth_kind_words(unsigned kind);

/*
 * Draws the Bluetooth page's cards from a top edge; returns the edge below
 * them.
 */
int
se_bluetooth_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_bluetooth_state state;
	struct se_bluetooth *bluetooth;
	kl_color ink;
	int available;
	int y;

	/* Without the desktop's Bluetooth, a card that says so. */
	bluetooth = &app->bluetooth;
	bluetooth->drawn_count = 0U;
	available = bluetooth_available(app);
	if (!available) {
		y = se_card_begin(app, canvas, x, top, width, 64 + 50, "Bluetooth", NULL);
		(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 24, "This desktop has no Bluetooth.", BLUETOOTH_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + 64 + 50;
	}

	/* The switch and the state. */
	(void)kl_system_bluetooth_state(app->system, &state);
	y = bluetooth_power_card(app, canvas, x, top, width, &state);

	/* The last answer. */
	if (bluetooth->message[0] != '\0') {
		ink = SE_COLOR_GOOD;
		if (bluetooth->message_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, bluetooth->message, BLUETOOTH_TEXT_SUB, 0, width, ink);
		y += 30;
	}

	/* Nothing more without a controller that answers. */
	if (!state.reachable || state.state == KL_BLUETOOTH_ABSENT || state.state == KL_BLUETOOTH_NONE)
		return y;

	/* The paired devices, then (while on) the others around. */
	y = bluetooth_list(app, canvas, x, y + 16, width, 1);
	if (state.state == KL_BLUETOOTH_ON)
		y = bluetooth_list(app, canvas, x, y + 16, width, 0);
	return y;
}

/*
 * Carries out a click on a control of the page.
 */
void
se_bluetooth_press(
	struct se_app *app,
	int index)
{
	struct kl_bluetooth_state state;
	struct se_bluetooth *bluetooth;
	const char *doing;
	unsigned on;
	int available;
	int error;
	size_t drawn;

	/* Nothing without Bluetooth, or while an answer is awaited. */
	bluetooth = &app->bluetooth;
	available = bluetooth_available(app);
	if (!available || bluetooth->request != 0U)
		return;
	app->dirty = 1;

	/* The switch: the other position. */
	if (index == BLUETOOTH_SWITCH) {
		(void)kl_system_bluetooth_state(app->system, &state);
		on = 1U;
		doing = "power-on";
		if ((state.flags & KL_BLUETOOTH_POWERED) != 0U) {
			on = 0U;
			doing = "power-off";
		}

		/* Asked of the desktop. */
		error = kl_system_bluetooth_power(app->system, on, &bluetooth->request);
		bluetooth_asked(app, error, doing);
		return;
	}

	/* A device's button, by its place in the list drawn. */
	if (index >= BLUETOOTH_CONNECT_FIRST) {
		drawn = (size_t)(index - BLUETOOTH_CONNECT_FIRST);
		if (drawn < bluetooth->drawn_count && (bluetooth->drawn[drawn].flags & KL_BLUETOOTH_CONNECTED) != 0U)
			bluetooth_ask(app, KL_BLUETOOTH_DISCONNECT, drawn);
		else
			bluetooth_ask(app, KL_BLUETOOTH_CONNECT, drawn);
		return;
	}

	/* Remove. */
	if (index >= BLUETOOTH_FORGET_FIRST) {
		bluetooth_ask(app, KL_BLUETOOTH_FORGET, (size_t)(index - BLUETOOTH_FORGET_FIRST));
		return;
	}

	/* Pair. */
	if (index >= BLUETOOTH_PAIR_FIRST)
		bluetooth_ask(app, KL_BLUETOOTH_PAIR, (size_t)(index - BLUETOOTH_PAIR_FIRST));
}

/*
 * Follows Bluetooth: a change draws the page again; the state is watched
 * and the devices around looked for while the page shows.
 */
void
se_bluetooth_poll(
	struct se_app *app)
{
	struct kl_bluetooth_state state;
	int available;

	/* Only with the desktop's Bluetooth. */
	available = bluetooth_available(app);
	if (!available)
		return;

	/* A change, or the state not logged yet (a service that is not running sends no change: T1-438). */
	if ((app->system_changed & KL_SYSTEM_CHANGED_BLUETOOTH) != 0U || !app->bluetooth.state_logged) {
		app->bluetooth.state_logged = 1;
		(void)kl_system_bluetooth_state(app->system, &state);
		se_log("BLUETOOTH state reachable=%u state=%u flags=%u features=%u", state.reachable, state.state, state.flags, state.features);
		if (app->page == SE_PAGE_BLUETOOTH)
			app->dirty = 1;
	}

	/* Watched and scanned while the page shows. */
	bluetooth_follow(app, app->page == SE_PAGE_BLUETOOTH);
}

/*
 * Tells how long the main loop may sleep for Bluetooth: until the scan is
 * asked again while the page shows; -1 when nothing is due.
 */
int
se_bluetooth_wait(
	const struct se_app *app)
{
	uint64_t elapsed;

	/* Nothing while it does not scan. */
	if (app->bluetooth.scan_at == 0U)
		return -1;

	/* The time left to the next asking. */
	elapsed = app->now - app->bluetooth.scan_at;
	if (elapsed >= SE_BLUETOOTH_RENEW_MS)
		return 0;
	return (int)(SE_BLUETOOTH_RENEW_MS - elapsed);
}

/* Stops following Bluetooth as Settings closes. */
void
se_bluetooth_close(
	struct se_app *app)
{
	int available;

	/* No longer watched or scanned. */
	available = bluetooth_available(app);
	if (available)
		bluetooth_follow(app, 0);
}

/*
 * Takes the answer of the page's request.  Returns 1 when it was the
 * page's.
 */
int
se_bluetooth_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	static const struct {
		const char *doing;
		const char *words;
	} done[] = {
		{ "pair", "The device is paired." },
		{ "forget", "The device is removed." },
		{ "connect", "The device is connected." },
		{ "disconnect", "The device is disconnected." }
	};
	static const struct {
		int error;
		const char *words;
	} failed[] = {
		{ EBUSY, "Bluetooth is busy. Try again in a moment." },
		{ ENETUNREACH, "The device did not answer." },
		{ EACCES, "The pairing was refused." },
		{ ENODEV, "Bluetooth is off or not available." },
		{ EPERM, "This account may not change Bluetooth." },
		{ ENOTSUP, "Bluetooth cannot do that here yet." },
		{ EINVAL, "That device is not known." }
	};
	struct se_bluetooth *bluetooth;
	size_t index;
	int same;

	/* Only the request the page asked. */
	bluetooth = &app->bluetooth;
	if (bluetooth->request == 0U || request != bluetooth->request)
		return 0;
	bluetooth->request = 0U;
	se_log("BLUETOOTH result kind=%s errno=%d", bluetooth->doing, error);
	app->dirty = 1;

	/* Succeeded: a line for a device's change, none for the switch. */
	bluetooth->message[0] = '\0';
	bluetooth->message_bad = 0;
	if (error == 0) {
		for (index = 0; index < sizeof(done) / sizeof(done[0]); index++) {
			same = strcmp(bluetooth->doing, done[index].doing);
			if (same == 0)
				(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", done[index].words);
		}

		/* The page's answer. */
		return 1;
	}

	/* Failed: why. */
	bluetooth->message_bad = 1;
	(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "It did not work.");
	for (index = 0; index < sizeof(failed) / sizeof(failed[0]); index++) {
		if (failed[index].error == error)
			(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", failed[index].words);
	}

	/* Succeeded: the answer was the page's. */
	return 1;
}

/* Tells whether the desktop offers Bluetooth. */
static int
bluetooth_available(
	const struct se_app *app)
{
	unsigned capabilities;

	/* The desktop's Bluetooth. */
	if (app->system == NULL)
		return 0;
	capabilities = kl_system_capabilities(app->system);
	return (capabilities & KL_SYSTEM_HAS_BLUETOOTH) != 0U;
}

/* Draws the card of the switch and the state; returns the edge below it. */
static int
bluetooth_power_card(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	const struct kl_bluetooth_state *state)
{
	char subtitle[128];
	const char *named;
	const char *note;
	int switchable;
	int enabled;
	int height;
	int y;

	/* The card, named with the controller when there is one. */
	named = NULL;
	if (state->name[0] != '\0') {
		(void)snprintf(subtitle, sizeof(subtitle), "%s  %s", state->name, state->address);
		named = subtitle;
	}

	/* The card. */
	height = 64 + 64;
	y = se_card_begin(app, canvas, x, top, width, height, "Bluetooth", named);

	/* The state in words, and the switch when the service can turn the controller off. */
	(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 22, bluetooth_state_words(state), BLUETOOTH_TEXT_ROW, 1, width - 120, SE_COLOR_TEXT);
	note = "";
	if (state->reachable && state->state == KL_BLUETOOTH_ON && (state->flags & KL_BLUETOOTH_ANSWERS) == 0U)
		note = "Another program answers the pairing questions.";
	if ((state->flags & KL_BLUETOOTH_SCANNING) != 0U && note[0] == '\0')
		note = "Looking for devices...";
	(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 44, note, BLUETOOTH_TEXT_SUB, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
	switchable = state->reachable && (state->features & KL_BLUETOOTH_CAN_POWER) != 0U && state->state != KL_BLUETOOTH_NONE;
	if (switchable) {
		enabled = app->bluetooth.request == 0U;
		se_toggle_draw(app, canvas, x + width - BLUETOOTH_PAD - 44, y + 16, (state->flags & KL_BLUETOOTH_POWERED) != 0U, enabled, BLUETOOTH_SWITCH);
	}

	/* The edge below the card. */
	return top + height;
}

/*
 * Draws the paired devices' card (paired 1) or the others' (0), and keeps
 * each device drawn for the clicks; returns the edge below it.
 */
static int
bluetooth_list(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int paired)
{
	struct kl_bluetooth_device devices[KL_BLUETOOTH_DEVICES_MAX];
	struct kl_bluetooth_state state;
	struct se_bluetooth *bluetooth;
	const struct kl_bluetooth_device *device;
	const char *title;
	const char *subtitle;
	const char *empty;
	const char *label;
	const char *connected;
	const char *legacy;
	char line[160];
	char battery[32];
	size_t count;
	size_t index;
	size_t rows;
	size_t drawn;
	int connectable;
	int enabled;
	int button;
	int right;
	int height;
	int written;
	int y;

	/* The devices of the card. */
	bluetooth = &app->bluetooth;
	(void)kl_system_bluetooth_state(app->system, &state);
	count = kl_system_bluetooth_devices(app->system, devices, KL_BLUETOOTH_DEVICES_MAX);
	rows = 0U;
	for (index = 0; index < count; index++) {
		if (((devices[index].flags & KL_BLUETOOTH_PAIRED) != 0U) == (paired != 0))
			rows++;
	}

	/* The card. */
	title = "Other Devices";
	subtitle = "Devices nearby that can be paired.";
	empty = "Looking for devices...";
	if (paired) {
		title = "My Devices";
		subtitle = "The devices paired with this computer.";
		empty = "No device is paired.";
	}

	/* Its height: a row a device, or the line that says there is none. */
	height = 64 + 40;
	if (rows > 0U)
		height = 64 + (int)rows * BLUETOOTH_ROW + 8;
	y = se_card_begin(app, canvas, x, top, width, height, title, subtitle);
	if (rows == 0U) {
		(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 22, empty, BLUETOOTH_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* Each device: its name, what it is and how it is, and its buttons. */
	enabled = bluetooth->request == 0U && (state.flags & KL_BLUETOOTH_PAIRING) == 0U;
	connectable = (state.features & KL_BLUETOOTH_CAN_CONNECT) != 0U && state.state == KL_BLUETOOTH_ON;
	for (index = 0; index < count; index++) {
		device = &devices[index];
		if (((device->flags & KL_BLUETOOTH_PAIRED) != 0U) != (paired != 0))
			continue;

		/* Kept for the clicks (no more than fit). */
		if (bluetooth->drawn_count >= KL_BLUETOOTH_DEVICES_MAX)
			break;
		drawn = bluetooth->drawn_count++;
		bluetooth->drawn[drawn] = *device;

		/* The name, and the line under it. */
		(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 22, device->name, BLUETOOTH_TEXT_ROW, 1, width / 2, SE_COLOR_TEXT);
		battery[0] = '\0';
		if (device->battery >= 0)
			(void)snprintf(battery, sizeof(battery), "  Battery %d%%", device->battery);
		connected = "";
		if ((device->flags & KL_BLUETOOTH_CONNECTED) != 0U)
			connected = "  Connected";
		legacy = "";
		if ((device->flags & KL_BLUETOOTH_LEGACY) != 0U)
			legacy = "  Paired the old, less safe way";
		written = snprintf(line, sizeof(line), "%s%s%s%s", bluetooth_kind_words(device->kind), connected, battery, legacy);
		if (!paired)
			written = snprintf(line, sizeof(line), "%s  %s", bluetooth_kind_words(device->kind), device->address);

		/* A line longer than the room is cut (the drawing fits it to its width anyway). */
		if (written < 0)
			line[0] = '\0';
		(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 42, line, BLUETOOTH_TEXT_SUB, 0, width / 2 + 60, SE_COLOR_TEXT_SECONDARY);

		/* Pair, or Remove and Connect or Disconnect, from the right. */
		right = x + width - BLUETOOTH_PAD;
		if (!paired) {
			button = se_button_width(app, "Pair");
			(void)se_button_draw(app, canvas, right - button, y + 10, "Pair", 1, enabled, BLUETOOTH_PAIR_FIRST + (int)drawn);
		} else {
			button = se_button_width(app, "Remove");
			(void)se_button_draw(app, canvas, right - button, y + 10, "Remove", 0, enabled, BLUETOOTH_FORGET_FIRST + (int)drawn);
			right -= button + 10;
			label = "Connect";
			if ((device->flags & KL_BLUETOOTH_CONNECTED) != 0U)
				label = "Disconnect";
			if (connectable) {
				button = se_button_width(app, label);
				(void)se_button_draw(app, canvas, right - button, y + 10, label, 0, enabled, BLUETOOTH_CONNECT_FIRST + (int)drawn);
			}
		}

		/* The next row. */
		y += BLUETOOTH_ROW;
	}

	/* The edge below the card. */
	return top + height;
}

/* Watches and scans while the page shows (the scan asked again in time), and no longer when it does not. */
static void
bluetooth_follow(
	struct se_app *app,
	int shown)
{
	struct se_bluetooth *bluetooth;
	int error;

	/* The watching follows the page. */
	bluetooth = &app->bluetooth;
	if (shown != bluetooth->watching) {
		error = kl_system_bluetooth_watch(app->system, (unsigned)shown);
		bluetooth->watching = shown;
		se_log("BLUETOOTH watch on=%d errno=%d", shown, error);
	}

	/* The scan: asked when the page comes and again in time, given up when it goes. */
	if (!shown) {
		if (bluetooth->scan_at != 0U) {
			error = kl_system_bluetooth_scan(app->system, 0U);
			bluetooth->scan_at = 0U;
			se_log("BLUETOOTH scan on=0 errno=%d", error);
		}

		/* Nothing more while it does not show. */
		return;
	}

	/* Not yet due again. */
	if (bluetooth->scan_at != 0U && app->now - bluetooth->scan_at < SE_BLUETOOTH_RENEW_MS)
		return;
	error = kl_system_bluetooth_scan(app->system, 1U);
	bluetooth->scan_at = app->now;
	if (bluetooth->scan_at == 0U)
		bluetooth->scan_at = 1U;
	se_log("BLUETOOTH scan on=1 errno=%d", error);
}

/* Asks for an action on a device drawn. */
static void
bluetooth_ask(
	struct se_app *app,
	unsigned action,
	size_t drawn)
{
	static const char *const names[] = { "", "pair", "forget", "connect", "disconnect" };
	struct se_bluetooth *bluetooth;
	const struct kl_bluetooth_device *device;
	int error;

	/* A device drawn. */
	bluetooth = &app->bluetooth;
	if (drawn >= bluetooth->drawn_count || action >= sizeof(names) / sizeof(names[0]))
		return;
	device = &bluetooth->drawn[drawn];

	/* Asked of the desktop. */
	error = kl_system_bluetooth_device(app->system, action, device->address, device->type, &bluetooth->request);
	bluetooth_asked(app, error, names[action]);
}

/* Notes a request asked (or refused at once). */
static void
bluetooth_asked(
	struct se_app *app,
	int error,
	const char *doing)
{
	struct se_bluetooth *bluetooth;

	/* The kind, for the answer's line. */
	bluetooth = &app->bluetooth;
	(void)snprintf(bluetooth->doing, sizeof(bluetooth->doing), "%s", doing);
	se_log("BLUETOOTH ask kind=%s error=%d", doing, error);
	bluetooth->message[0] = '\0';
	if (error == 0)
		return;

	/* Refused at once. */
	bluetooth->request = 0U;
	bluetooth->message_bad = 1;
	(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "Bluetooth could not be asked.");
}

/* The words of the state. */
static const char *
bluetooth_state_words(
	const struct kl_bluetooth_state *state)
{
	/* No service. */
	if (!state->reachable)
		return "Bluetooth is not available on this computer.";

	/* Each state. */
	switch (state->state) {
	case KL_BLUETOOTH_NONE:
		return "No Bluetooth adapter was found.";
	case KL_BLUETOOTH_STARTING:
		return "Bluetooth is starting...";
	case KL_BLUETOOTH_OFF:
		return "Bluetooth is off.";
	case KL_BLUETOOTH_ON:
		return "Bluetooth is on.";
	case KL_BLUETOOTH_FIRMWARE:
		return "The adapter's firmware is missing.";
	case KL_BLUETOOTH_UNSUPPORTED:
		return "This adapter is not supported.";
	default:
		break;
	}

	/* Any other. */
	return "Bluetooth is not available on this computer.";
}

/* The words of a device's kind. */
static const char *
bluetooth_kind_words(
	unsigned kind)
{
	static const char *const words[] = { "Device", "Keyboard", "Mouse", "Audio", "Phone", "Computer" };

	/* A known kind. */
	if (kind < sizeof(words) / sizeof(words[0]))
		return words[kind];
	return "Device";
}
