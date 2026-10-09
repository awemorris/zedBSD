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
 *   Use as phone   (ws197-p004c, plan/ws197/phase004/phase.md section
 *                  4.3) a phone's button: on pairs it as the user's phone
 *                  when it is not yet (PAIR phone=1), turns its switch on
 *                  (kl_system_phone_link_set) and then the desktop's
 *                  phone.backend to 2; off turns the switch off, then
 *                  phone.backend to 0.  A step that fails stops there.  The
 *                  phone's line tells its messages' state.
 *
 * While the page shows, the state is watched and the scan is asked again
 * each SE_BLUETOOTH_RENEW_MS (the desktop lets an asking go after a
 * minute).  Each request's answer is a line under the cards.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* The controls (hit indices): the switch, and each device's buttons by its place in the drawn list. */
#define BLUETOOTH_SWITCH	1
#define BLUETOOTH_PAIR_FIRST	100
#define BLUETOOTH_FORGET_FIRST	200
#define BLUETOOTH_CONNECT_FIRST	300
#define BLUETOOTH_PHONE_FIRST	400

/* The desktop's setting of the phone's backend: none, and the paired phone (ws197-p004c). */
#define BLUETOOTH_PHONE_SETTING		"phone.backend"
#define BLUETOOTH_PHONE_NONE		0
#define BLUETOOTH_PHONE_BLUETOOTH	2

/* The phone link's messages ready (kl_phone_link's messages). */
#define BLUETOOTH_MESSAGES_CONNECTING	1U
#define BLUETOOTH_MESSAGES_READY	2U
#define BLUETOOTH_MESSAGES_FAILED	3U

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
static int bluetooth_phone_available(const struct se_app *app);
static int bluetooth_phone_used(const struct se_app *app, const struct kl_bluetooth_device *device);
static void bluetooth_phone_press(struct se_app *app, size_t drawn);
static void bluetooth_phone_link(struct se_app *app);
static int bluetooth_phone_result(struct se_app *app, uint32_t request, int error);
static const char *bluetooth_phone_words(const struct se_app *app);

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

	/* A phone's "Use as phone" (ws197-p004c). */
	if (index >= BLUETOOTH_PHONE_FIRST) {
		bluetooth_phone_press(app, (size_t)(index - BLUETOOTH_PHONE_FIRST));
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
	struct kl_phone_event event;
	int available;
	int taken;
	int error;

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

	/* The phone's link as last told (its events are not this page's: they go). */
	if ((app->system_changed & KL_SYSTEM_CHANGED_PHONE) != 0U && app->bluetooth.phone_watching) {
		do {
			taken = kl_system_take_phone_event(app->system, &event);
		} while (taken == 1);
		error = kl_system_phone_link(app->system, &app->bluetooth.phone_link, sizeof(app->bluetooth.phone_link));
		if (error == 0) {
			app->bluetooth.phone_known = 1;
			se_log("BLUETOOTH phone enabled=%u messages=%u owner=%u why=%s", app->bluetooth.phone_link.enabled, app->bluetooth.phone_link.messages,
			    app->bluetooth.phone_link.owner, app->bluetooth.phone_link.why);
			if (app->page == SE_PAGE_BLUETOOTH)
				app->dirty = 1;
		}
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
		{ "pair-phone", "The phone is paired." },
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
	int phone;
	int same;

	/* The phone's switch (ws197-p004c). */
	phone = bluetooth_phone_result(app, request, error);
	if (phone)
		return 1;

	/* Only the request the page asked. */
	bluetooth = &app->bluetooth;
	if (bluetooth->request == 0U || request != bluetooth->request)
		return 0;
	bluetooth->request = 0U;
	se_log("BLUETOOTH result kind=%s errno=%d", bluetooth->doing, error);
	app->dirty = 1;

	/* The pairing of the user's phone: its switch next (ws197-p004c). */
	same = strcmp(bluetooth->doing, "pair-phone");
	if (same == 0 && bluetooth->phone_step == SE_PHONE_STEP_PAIR) {
		if (error == 0) {
			bluetooth_phone_link(app);
			return 1;
		}

		/* Not paired: the switch stops there. */
		bluetooth->phone_step = SE_PHONE_STEP_NONE;
	}

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
	int phone_offered;
	int connectable;
	int enabled;
	int button;
	int used;
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
	phone_offered = bluetooth_phone_available(app);
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

		/* The user's phone: its messages' state (ws197-p004c). */
		used = bluetooth_phone_used(app, device);
		if (used)
			written = snprintf(line, sizeof(line), "%s%s  %s", bluetooth_kind_words(device->kind), connected, bluetooth_phone_words(app));

		/* A line longer than the room is cut (the drawing fits it to its width anyway). */
		if (written < 0)
			line[0] = '\0';
		(void)kl_text_draw_fit(app->text, canvas, x + BLUETOOTH_PAD, y + 42, line, BLUETOOTH_TEXT_SUB, 0, width / 2 + 60, SE_COLOR_TEXT_SECONDARY);

		/* Pair, or Remove and Connect or Disconnect, from the right. */
		right = x + width - BLUETOOTH_PAD;
		if (!paired) {
			button = se_button_width(app, "Pair");
			(void)se_button_draw(app, canvas, right - button, y + 10, "Pair", 1, enabled, BLUETOOTH_PAIR_FIRST + (int)drawn);
			right -= button + 10;

			/* A phone may be paired as the user's at once (ws197-p004c). */
			if (phone_offered && device->kind == KL_BLUETOOTH_KIND_PHONE && device->type == KL_BLUETOOTH_BREDR) {
				button = se_button_width(app, "Use as phone");
				(void)se_button_draw(app, canvas, right - button, y + 10, "Use as phone", 0, enabled, BLUETOOTH_PHONE_FIRST + (int)drawn);
			}
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
				right -= button + 10;
			}

			/* A phone's switch: used for messages or not (ws197-p004c). */
			label = "Use as phone";
			if (used && app->bluetooth.phone_link.enabled)
				label = "Stop using as phone";
			if (phone_offered && (used || device->kind == KL_BLUETOOTH_KIND_PHONE) && device->type == KL_BLUETOOTH_BREDR) {
				button = se_button_width(app, label);
				(void)se_button_draw(app, canvas, right - button, y + 10, label, 0, enabled && bluetooth->phone_step == SE_PHONE_STEP_NONE,
				    BLUETOOTH_PHONE_FIRST + (int)drawn);
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
	int offered;
	int error;

	/* The watching follows the page. */
	bluetooth = &app->bluetooth;
	if (shown != bluetooth->watching) {
		error = kl_system_bluetooth_watch(app->system, (unsigned)shown);
		bluetooth->watching = shown;
		se_log("BLUETOOTH watch on=%d errno=%d", shown, error);
	}

	/* The phone's link too, where the desktop offers it (ws197-p004c). */
	offered = bluetooth_phone_available(app);
	if (offered && shown != bluetooth->phone_watching) {
		error = kl_system_phone_watch_link(app->system, (unsigned)shown);
		bluetooth->phone_watching = shown;
		se_log("BLUETOOTH phone-watch on=%d errno=%d", shown, error);
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

/* Tells whether the desktop offers the phone's switch (KL_SYSTEM_HAS_PHONE_SYNC, ws197-p004c). */
static int
bluetooth_phone_available(
	const struct se_app *app)
{
	unsigned capabilities;

	/* The desktop's phone, with its messages. */
	if (app->system == NULL)
		return 0;
	capabilities = kl_system_capabilities(app->system);
	if ((capabilities & KL_SYSTEM_HAS_PHONE_SYNC) == 0U)
		return 0;

	/* Succeeded: offered. */
	return 1;
}

/* Tells whether a device is the user's phone as the link last told (its address). */
static int
bluetooth_phone_used(
	const struct se_app *app,
	const struct kl_bluetooth_device *device)
{
	int same;

	/* A link told, with an address. */
	if (!app->bluetooth.phone_known || app->bluetooth.phone_link.address[0] == '\0')
		return 0;

	/* The same address (the letters in either case). */
	same = strcasecmp(app->bluetooth.phone_link.address, device->address);
	if (same != 0)
		return 0;

	/* Succeeded: the user's phone. */
	return 1;
}

/*
 * Carries out a phone's "Use as phone" or "Stop using as phone" (section
 * 4.3): on pairs it as the user's first when it is not the user's phone
 * yet, then turns its switch on; off turns the switch off.
 */
static void
bluetooth_phone_press(
	struct se_app *app,
	size_t drawn)
{
	struct se_bluetooth *bluetooth;
	const struct kl_bluetooth_device *device;
	int used;
	int error;

	/* A device drawn, and no switch under way. */
	bluetooth = &app->bluetooth;
	if (drawn >= bluetooth->drawn_count || bluetooth->phone_step != SE_PHONE_STEP_NONE || bluetooth->request != 0U)
		return;
	device = &bluetooth->drawn[drawn];
	used = bluetooth_phone_used(app, device);
	(void)snprintf(bluetooth->phone_address, sizeof(bluetooth->phone_address), "%s", device->address);

	/* Off: its switch. */
	bluetooth->phone_on = 1;
	if (used && bluetooth->phone_link.enabled)
		bluetooth->phone_on = 0;
	if (!bluetooth->phone_on || used) {
		bluetooth_phone_link(app);
		return;
	}

	/* On, a phone not the user's yet: paired as the user's first. */
	bluetooth->phone_step = SE_PHONE_STEP_PAIR;
	error = kl_system_bluetooth_device(app->system, KL_BLUETOOTH_PAIR_PHONE, device->address, KL_BLUETOOTH_BREDR, &bluetooth->request);
	bluetooth_asked(app, error, "pair-phone");
	if (error != 0)
		bluetooth->phone_step = SE_PHONE_STEP_NONE;
}

/* Asks the phone's switch on or off (its answer turns the desktop's phone.backend). */
static void
bluetooth_phone_link(
	struct se_app *app)
{
	struct se_bluetooth *bluetooth;
	int error;

	/* Asked of the desktop. */
	bluetooth = &app->bluetooth;
	bluetooth->phone_step = SE_PHONE_STEP_LINK;
	error = kl_system_phone_link_set(app->system, bluetooth->phone_address, (unsigned)bluetooth->phone_on, KL_PHONE_PROFILE_MESSAGES,
	    &bluetooth->phone_request);
	se_log("BLUETOOTH phone-switch on=%d error=%d", bluetooth->phone_on, error);
	app->dirty = 1;
	if (error == 0)
		return;

	/* Refused at once: stopped there. */
	bluetooth->phone_step = SE_PHONE_STEP_NONE;
	bluetooth->phone_request = 0U;
	bluetooth->message_bad = 1;
	(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "The phone could not be asked.");
}

/*
 * Takes the answer of the phone's switch: on, the desktop's phone.backend
 * becomes the paired phone; off, none.  Returns 1 when it was its answer.
 */
static int
bluetooth_phone_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_bluetooth *bluetooth;

	/* Only the switch asked. */
	bluetooth = &app->bluetooth;
	if (bluetooth->phone_request == 0U || request != bluetooth->phone_request)
		return 0;
	bluetooth->phone_request = 0U;
	bluetooth->phone_step = SE_PHONE_STEP_NONE;
	se_log("BLUETOOTH phone-switch-result on=%d errno=%d", bluetooth->phone_on, error);
	app->dirty = 1;

	/* Failed: stopped there (the link's state shows what is). */
	if (error != 0) {
		bluetooth->message_bad = 1;
		(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "The phone's switch could not be changed.");
		if (error == EACCES)
			(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "The phone is another account's.");
		if (error == ENOTCONN || error == ENOTSUP)
			(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "Bluetooth cannot do that here yet.");
		return 1;
	}

	/* The desktop's phone follows the switch. */
	bluetooth->message_bad = 0;
	if (bluetooth->phone_on) {
		se_look_set_number(app, BLUETOOTH_PHONE_SETTING, BLUETOOTH_PHONE_BLUETOOTH, BLUETOOTH_PHONE_NONE);
		(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "The phone is used for messages.");
	} else {
		se_look_set_number(app, BLUETOOTH_PHONE_SETTING, BLUETOOTH_PHONE_NONE, BLUETOOTH_PHONE_NONE);
		(void)snprintf(bluetooth->message, sizeof(bluetooth->message), "%s", "The phone is no longer used.");
	}

	/* Succeeded: the answer was the switch's. */
	return 1;
}

/* The words of the user's phone's messages, as the link last told. */
static const char *
bluetooth_phone_words(
	const struct se_app *app)
{
	const struct kl_phone_link *link;
	int same;

	/* Not used for messages. */
	link = &app->bluetooth.phone_link;
	if (!link->enabled)
		return "Not used as phone";

	/* Why its messages do not work. */
	same = strcmp(link->why, "permission");
	if (same == 0)
		return "Allow access to messages on the phone";
	same = strcmp(link->why, "no-mas");
	if (same == 0)
		return "The phone does not share its messages";
	same = strcmp(link->why, "not-owner");
	if (same == 0)
		return "Another account's phone";

	/* Each state of its messages. */
	switch (link->messages) {
	case BLUETOOTH_MESSAGES_READY:
		return "Messages connected";
	case BLUETOOTH_MESSAGES_CONNECTING:
		return "Messages connecting...";
	case BLUETOOTH_MESSAGES_FAILED:
		return "Messages not connected";
	default:
		break;
	}

	/* Off: not near, or not yet. */
	if (!link->present)
		return "Used as phone (messages while you are signed in here)";
	return "Used as phone (not connected)";
}
