/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The displays on the wire (ws113-p005; the design is
 * plan/ws113/phase005/phase.md): kl_system_displays_v1, made by the system
 * manager's get_displays (since its version 18) for a client of the
 * compositor's own user, and the built-in panel's light.
 *
 * Every displays object hears a snapshot when it is made and after every
 * change: an output event for each display connected (the desktop's
 * anchor, the heads of heads.c, and those shown nowhere: held back by the
 * limit of the displays shown at once, or kept off under a closed lid),
 * then done with the snapshot's serial and the mode.  An apply names the
 * serial it was made against; one that is not the last is stale and
 * changes nothing.  The choice itself is heads.c's kwl_displays_apply.
 *
 * The light of the built-in panel is libkeiland-backend's backlight (WS113
 * p013), opened the first time it is needed and shared with the lid's
 * (backend-host.c): while the lid holds the panel out, a light set is kept
 * for the opening instead.  The light chosen is kept in displays.conf
 * (D-STORE) and set again when a session starts (D-BRIGHT-BOOT); the
 * light keys move it by DISPLAYS_KEY_STEP (D-BRIGHT-KEY).  A session that
 * is not active (the login screen, a lock) may not change anything
 * (D-AUTH2).
 *
 * A request's result says whether its choice was written to displays.conf;
 * the file I/O thread writes it (heads.c), so the result is held until the
 * writing that holds the choice is over, and, for a move of the output to
 * another display, until the move's first frame proves it.
 */

#include "kwl.h"
#include "compose.h"
#include "displays.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Marks a parameter a function does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The light keys (the evdev KEY_BRIGHTNESSDOWN and KEY_BRIGHTNESSUP), and how far one moves the light. */
#define DISPLAYS_KEY_DOWN	224U
#define DISPLAYS_KEY_UP		225U
#define DISPLAYS_KEY_STEP	5U

/* How long a result waits for the first frame of the output's move (ms) before it is answered as applied (BUG-266). */
#define DISPLAYS_PENDING_MS	3000U

/* The longest output event: seven words, two strings and their lengths. */
#define DISPLAYS_EVENT_MAX	(7U * 4U + 4U + KL_SYSTEM_DISPLAY_KEY_MAX + 4U + KL_SYSTEM_DISPLAY_LABEL_MAX + 8U)

/*
 * The displays' own state: the snapshot's serial (moved at every change),
 * and whether the light of the start was set.
 */
static struct {
	uint32_t serial;
	unsigned boot_light_done;
} displays_state;

/*
 * A result held back (in the order held): the object (its going forgets
 * the result), its request, how it ended (applied), and whether the
 * choice was written; ticket is the writing of displays.conf it waits for
 * (0: none, written is final), moving says it waits for the first frame
 * of the output's move, since when.  Only the latest move is waited for:
 * a newer one answers the older as applied.
 */
struct displays_held {
	struct displays_held *next;
	struct kwl_object *object;
	uint32_t request;
	uint32_t applied;
	uint32_t written;
	uint64_t ticket;
	unsigned moving;
	uint64_t since_ms;
};

/* The results held back (struct displays_held), the event loop's thread's alone. */
static struct displays_held *displays_held_list;

static void displays_snapshot(struct kwl_object *object);
static void displays_output(struct kwl_object *object, struct kwl_server *server, unsigned index);
static void displays_label(const char *key, char *label, size_t size);
static int displays_light_get(struct kwl_server *server, unsigned *percent);
static int displays_light_set(struct kwl_server *server, unsigned percent, int *saved);
static int displays_apply_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_brightness_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_shown_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_rotation_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_touch_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static void displays_rotation(struct kwl_object *object, struct kwl_server *server, const char *key, unsigned anchor);
static int displays_places(const char *text, struct kwl_display_config *wanted);
static void displays_result(struct kwl_object *object, uint32_t request, uint32_t applied, uint32_t saved);
static void displays_answer(struct kwl_object *object, VkDisplayKHR before, uint32_t request, uint32_t applied, int saved);
static void displays_hold(struct kwl_object *object, uint32_t request, uint32_t applied, int saved, unsigned moving);
static void displays_held_settle(void);
static int displays_active(struct kwl_server *server);
static int displays_string(const unsigned char *bytes, size_t size, size_t offset, size_t bound, const char **text, size_t *next);
static size_t displays_put_word(unsigned char *payload, size_t offset, uint32_t word);
static size_t displays_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t displays_word(const unsigned char *bytes, size_t offset);

/*
 * Makes a displays object for a manager's get_displays (new id), and sends
 * it the snapshot.  Returns 0, or EPROTO.
 */
int
kwl_displays_create(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	uint32_t id;

	/* The new object's ID. */
	if (size != 4U)
		return EPROTO;
	id = displays_word(bytes, 0U);

	/* The object, under the ID the client chose. */
	created = kwl_create(manager->client, id, KWL_SYSTEM_DISPLAYS, manager->version);
	if (created == NULL)
		return EPROTO;

	/* Its first snapshot. */
	displays_snapshot(created);
	printf("KWL DISPLAYS object client=%llu id=%u\n", (unsigned long long)manager->client->number, id);
	return 0;
}

/* Carries out a request of a displays object.  Returns 0, or EPROTO for a malformed one. */
int
kwl_displays_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The object goes. */
	if (opcode == KL_SYSTEM_DISPLAYS_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A choice applied. */
	if (opcode == KL_SYSTEM_DISPLAYS_APPLY) {
		error = displays_apply_request(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A light set. */
	if (opcode == KL_SYSTEM_DISPLAYS_SET_BRIGHTNESS) {
		error = displays_brightness_request(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A display turned off or on (since 19, ws113-p014). */
	if (opcode == KL_SYSTEM_DISPLAYS_SET_SHOWN && object->version >= KL_SYSTEM_SINCE_SHOWN) {
		error = displays_shown_request(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A display's angle chosen (since 34, ws217-p004). */
	if (opcode == KL_SYSTEM_DISPLAYS_SET_ROTATION && object->version >= KL_SYSTEM_SINCE_ROTATION) {
		error = displays_rotation_request(object, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* A touch or pen device bound to a display, or the next touch listened for (since 34; ws217-p006 carries them out). */
	if (opcode == KL_SYSTEM_DISPLAYS_BIND_TOUCH || opcode == KL_SYSTEM_DISPLAYS_LISTEN_TOUCH) {
		if (object->version < KL_SYSTEM_SINCE_ROTATION)
			return EPROTO;
		error = displays_touch_request(object, opcode, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* No other request. */
	return EPROTO;
}

/*
 * Settles the result held back for a move of the output, once the move's
 * first frame was presented or refused: a refused one is FAILED, the
 * choice not holding.  It is answered once its writing is over too.
 */
void
kwl_displays_move_settled(
	struct kwl_server *server,
	int failed)
{
	struct displays_held *held;

	UNUSED_PARAMETER(server);

	/* Each result that waits for the move (one at most). */
	for (held = displays_held_list; held != NULL; held = held->next) {
		if (!held->moving)
			continue;

		/* How the move ended. */
		held->moving = 0U;
		if (failed)
			held->applied = KL_SYSTEM_RESULT_FAILED;
		printf("KWL DISPLAYS result held request=%u applied=%u\n", held->request, held->applied);
	}

	/* What waits for nothing more is answered. */
	displays_held_settle();
}

/*
 * Settles a move held back too long (the move's first frame never came:
 * nothing was drawn, or the output was moved again), as applied.
 */
void
kwl_displays_pending_tick(
	struct kwl_server *server,
	uint64_t now)
{
	struct displays_held *held;

	/* Each result that waits for the move, long enough. */
	for (held = displays_held_list; held != NULL; held = held->next) {
		if (!held->moving || now - held->since_ms < DISPLAYS_PENDING_MS)
			continue;

		/* Settled as the choice applied. */
		kwl_displays_move_settled(server, 0);
		return;
	}
}

/*
 * Settles the results that waited for the writings of displays.conf up to
 * a ticket (heads.c, once the file I/O thread wrote it): written, or not
 * when the writing failed.
 */
void
kwl_displays_written(
	struct kwl_server *server,
	uint64_t ticket,
	int error)
{
	struct displays_held *held;

	UNUSED_PARAMETER(server);

	/* Each result whose choice this writing holds. */
	for (held = displays_held_list; held != NULL; held = held->next) {
		if (held->ticket == 0U || held->ticket > ticket)
			continue;

		/* Its writing is over. */
		held->ticket = 0U;
		held->written = 0U;
		if (error == 0)
			held->written = 1U;
	}

	/* What waits for nothing more is answered. */
	displays_held_settle();
}

/*
 * Forgets the held results of a displays object that goes.
 */
void
kwl_displays_object_gone(
	struct kwl_object *object)
{
	struct displays_held **link;
	struct displays_held *held;

	/* Each result of the object leaves the list. */
	link = &displays_held_list;
	while (*link != NULL) {
		held = *link;
		if (held->object != object) {
			link = &held->next;
			continue;
		}

		/* Nobody to answer. */
		*link = held->next;
		free(held);
	}
}

/* Sends every displays object a new snapshot: the displays changed. */
void
kwl_displays_tell(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;

	/* A new serial for the new snapshot. */
	displays_state.serial++;

	/* Each displays object of every live client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->kind != KWL_SYSTEM_DISPLAYS || object->dead)
				continue;
			displays_snapshot(object);
		}
	}
}

/*
 * Takes a light key (D-BRIGHT-KEY): the built-in panel's light down or up
 * by a step, kept in displays.conf, and every displays object told.
 * Returns 1 when the key was a light key, 0 otherwise.
 */
int
kwl_displays_key(
	struct kwl_server *server,
	uint32_t key,
	uint32_t state)
{
	unsigned percent;
	int saved;
	int error;

	/* Only the light keys, on their press. */
	if (key != DISPLAYS_KEY_DOWN && key != DISPLAYS_KEY_UP)
		return 0;
	if (state == 0U)
		return 1;

	/* The light now (or the one kept while the lid holds it out). */
	error = displays_light_get(server, &percent);
	if (error != 0) {
		printf("KWL DISPLAYS key light none error=%d\n", error);
		return 1;
	}

	/* A step down or up, within 0 to 100. */
	if (key == DISPLAYS_KEY_DOWN) {
		if (percent < DISPLAYS_KEY_STEP) {
			percent = 0U;
		} else {
			percent -= DISPLAYS_KEY_STEP;
		}
	} else {
		percent += DISPLAYS_KEY_STEP;
		if (percent > 100U)
			percent = 100U;
	}

	/* Set, kept and told. */
	error = displays_light_set(server, percent, &saved);
	printf("KWL DISPLAYS key light percent=%u error=%d saved=%d\n", percent, error, saved);
	if (error == 0)
		kwl_displays_tell(server);
	return 1;
}

/*
 * Answers a displays result held back too long (BUG-266), and sets the
 * light displays.conf keeps once a session's first frame is up
 * (D-BRIGHT-BOOT: the panel is not lit by the driver before it); a light
 * not kept stays as the firmware left it.
 */
void
kwl_displays_tick(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	uint64_t now;
	int error;

	/* A result whose move's first frame never came is settled after its time. */
	now = kwl_milliseconds();
	kwl_displays_pending_tick(server, now);

	/* Once, in a session, after its first frame. */
	if (displays_state.boot_light_done || server->greeter)
		return;
	compose = server->compose;
	if (compose == NULL || !compose->config_loaded || server->frame == 0U)
		return;
	displays_state.boot_light_done = 1U;

	/* Only a light that was chosen. */
	if (!compose->config.has_brightness)
		return;

	/* The panel's light, where the machine has one the compositor may set (by the display thread). */
	error = kwl_backlight_set(server, compose->config.brightness);
	printf("KWL DISPLAYS boot light percent=%u error=%d\n", compose->config.brightness, error);
}

/* Sends an object the snapshot: an output event for each display connected, then done. */
static void
displays_snapshot(
	struct kwl_object *object)
{
	struct kwl_server *server;
	struct kwl_compose *compose;
	uint32_t words[2];
	unsigned index;

	/* Each display of the last enumeration. */
	server = object->client->server;
	compose = server->compose;
	if (compose != NULL) {
		for (index = 0U; index < compose->display_count; index++)
			displays_output(object, server, index);
	}

	/* The serial and the mode close the snapshot. */
	words[0] = displays_state.serial;
	words[1] = KL_SYSTEM_DISPLAYS_EXTENDED;
	if (compose != NULL && compose->display_mode == KWL_DISPLAYS_MIRROR)
		words[1] = KL_SYSTEM_DISPLAYS_MIRROR;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_DISPLAYS_EVENT_DONE, words, sizeof(words));
}

/* Sends an object one display's output event. */
static void
displays_output(
	struct kwl_object *object,
	struct kwl_server *server,
	unsigned index)
{
	struct kwl_compose *compose;
	const struct kwl_head *head;
	unsigned char payload[DISPLAYS_EVENT_MAX];
	char label[KL_SYSTEM_DISPLAY_LABEL_MAX];
	const char *key;
	VkDisplayKHR display;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint32_t flags;
	unsigned percent;
	unsigned slot;
	int32_t x;
	int32_t y;
	size_t offset;
	unsigned anchor;
	int internal;
	int place;
	int off;
	int error;

	/* The display, its key and its label. */
	compose = server->compose;
	display = compose->displays[index];
	key = compose->display_names[index];
	displays_label(key, label, sizeof(label));
	flags = 0U;
	internal = kwl_output_display_internal(server, display);
	if (internal)
		flags |= KL_SYSTEM_DISPLAY_INTERNAL;
	if ((compose->limited & ((uint32_t)1U << index)) != 0U)
		flags |= KL_SYSTEM_DISPLAY_LIMITED;
	off = kwl_displays_is_off(&compose->config, key);
	if (off)
		flags |= KL_SYSTEM_DISPLAY_OFF;

	/* Where it is and what it shows: the anchor's, a head's, or its own mode and the place kept for it. */
	x = 0;
	y = 0;
	width = 0U;
	height = 0U;
	refresh = 0U;
	head = NULL;
	for (slot = 0U; slot < KWL_HEADS; slot++) {
		if (compose->heads[slot].open && compose->heads[slot].display == display)
			head = &compose->heads[slot];
	}

	/* The anchor, a head, or a display shown nowhere. */
	if (display == compose->display && compose->output_open) {
		flags |= KL_SYSTEM_DISPLAY_ANCHOR | KL_SYSTEM_DISPLAY_SHOWN;
		x = compose->output_x;
		y = compose->output_y;
		width = server->width;
		height = server->height;
		refresh = server->refresh;
	} else if (head != NULL) {
		flags |= KL_SYSTEM_DISPLAY_SHOWN;
		x = head->x;
		y = head->y;
		width = head->width;
		height = head->height;
		refresh = head->refresh;
	} else {
		/* Shown nowhere: its own mode, at the place kept for it. */
		(void)kwl_compose_display_read(server, display, &width, &height, &refresh, NULL, 0U);
		place = kwl_displays_find(&compose->config, key);
		if (place >= 0) {
			x = compose->config.places[place].x;
			y = compose->config.places[place].y;
		}
	}

	/* The built-in panel's light, where the compositor can set it. */
	percent = 0U;
	if (internal) {
		error = displays_light_get(server, &percent);
		if (error == 0) {
			flags |= KL_SYSTEM_DISPLAY_BACKLIGHT;
		} else {
			percent = 0U;
		}
	}

	/* The event: key, label, place, size, refresh, flags and light. */
	offset = displays_put_string(payload, 0U, key);
	offset = displays_put_string(payload, offset, label);
	offset = displays_put_word(payload, offset, (uint32_t)x);
	offset = displays_put_word(payload, offset, (uint32_t)y);
	offset = displays_put_word(payload, offset, width);
	offset = displays_put_word(payload, offset, height);
	offset = displays_put_word(payload, offset, refresh);
	offset = displays_put_word(payload, offset, flags);
	offset = displays_put_word(payload, offset, percent);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_DISPLAYS_EVENT_OUTPUT, payload, offset);

	/* Its angle after it, for an object that hears the angles (since 34, ws217-p004). */
	anchor = 0U;
	if ((flags & KL_SYSTEM_DISPLAY_ANCHOR) != 0U)
		anchor = 1U;
	if (object->version >= KL_SYSTEM_SINCE_ROTATION)
		displays_rotation(object, server, key, anchor);
}

/*
 * Sends an object a display's rotation event (the angle the user chose)
 * and its rotations event (the angles it may take now, and why not).
 */
static void
displays_rotation(
	struct kwl_object *object,
	struct kwl_server *server,
	const char *key,
	unsigned anchor)
{
	unsigned char payload[DISPLAYS_EVENT_MAX];
	unsigned rotation;
	unsigned mask;
	unsigned why;
	size_t offset;

	/* What the compositor tells of the display's angle. */
	kwl_output_rotation_report(server, anchor, &rotation, &mask, &why);

	/* The angle chosen. */
	offset = displays_put_string(payload, 0U, key);
	offset = displays_put_word(payload, offset, rotation);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_DISPLAYS_EVENT_ROTATION, payload, offset);

	/* The angles it may take, and why not. */
	offset = displays_put_string(payload, 0U, key);
	offset = displays_put_word(payload, offset, mask);
	offset = displays_put_word(payload, offset, why);
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_DISPLAYS_EVENT_ROTATIONS, payload, offset);
}

/*
 * Makes a display's label from its key (D-ID A2, "...:hdmi:B"): its
 * connector's kind and port ("HDMI B", "DisplayPort D"), "Built-in
 * display" for the panel, or the key itself (a virtual adapter's name).
 */
static void
displays_label(
	const char *key,
	char *label,
	size_t size)
{
	const char *port;
	const char *found;

	/* The port: the key's last part. */
	port = strrchr(key, ':');
	if (port == NULL) {
		(void)snprintf(label, size, "%s", key);
		return;
	}

	/* After its colon. */
	port++;

	/* The built-in panel. */
	found = strstr(key, ":edp:");
	if (found != NULL) {
		(void)snprintf(label, size, "Built-in display");
		return;
	}

	/* An HDMI or a DisplayPort connector. */
	found = strstr(key, ":hdmi:");
	if (found != NULL) {
		(void)snprintf(label, size, "HDMI %s", port);
		return;
	}

	/* A DisplayPort connector. */
	found = strstr(key, ":dp:");
	if (found != NULL) {
		(void)snprintf(label, size, "DisplayPort %s", port);
		return;
	}

	/* Succeeded: the key itself. */
	(void)snprintf(label, size, "%s", key);
}

/* Reads the panel's light (the display thread's last reading): the one kept for the opening while the lid holds the panel out. */
static int
displays_light_get(
	struct kwl_server *server,
	unsigned *percent)
{
	unsigned now;
	int error;

	/* The light, as the display thread last read it. */
	error = kwl_backlight_get(server, &now);
	if (error != 0)
		return error;

	/* Kept while the lid holds it out. */
	if (server->backlight_out) {
		*percent = server->backlight_saved;
		return 0;
	}

	/* Succeeded: the light. */
	*percent = now;
	return 0;
}

/*
 * Sets the panel's light (kept for the opening while the lid holds it
 * out; else set by the display thread, whose failure is logged) and keeps
 * it in displays.conf; *saved is 0 or the error of writing it.  Returns 0
 * or the error of the light's open (none).
 */
static int
displays_light_set(
	struct kwl_server *server,
	unsigned percent,
	int *saved)
{
	struct kwl_display_config wanted;
	int error;

	/* Kept for the opening, or set by the display thread (a failure is logged there). */
	*saved = 0;
	if (server->backlight_out) {
		if (server->backlight_state == KWL_BACKLIGHT_NONE)
			return server->backlight_error;
		server->backlight_saved = percent;
	} else {
		error = kwl_backlight_set(server, percent);
		if (error != 0)
			return error;
	}

	/* Kept in the choice and written, with the rest of the choice unchanged. */
	if (server->compose != NULL) {
		wanted = server->compose->config;
		wanted.mode = server->compose->display_mode;
		wanted.has_brightness = 1U;
		wanted.brightness = percent;
		server->compose->config.has_brightness = 1U;
		server->compose->config.brightness = percent;
		error = kwl_displays_apply(server, &wanted, saved);
		if (error != 0)
			*saved = error;
	}

	/* Succeeded: the light is set. */
	return 0;
}

/* Carries out an apply(request, serial, mode, places). */
static int
displays_apply_request(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	VkDisplayKHR before;
	struct kwl_display_config wanted;
	const char *places;
	uint32_t request;
	uint32_t serial;
	uint32_t mode;
	size_t next;
	int active;
	int saved;
	int error;

	/* The request's words and places. */
	if (size < 16U)
		return EPROTO;
	request = displays_word(bytes, 0U);
	serial = displays_word(bytes, 4U);
	mode = displays_word(bytes, 8U);
	error = displays_string(bytes, size, 12U, KL_SYSTEM_DISPLAY_PLACES_MAX, &places, &next);
	if (error != 0 || next != size)
		return EPROTO;

	/* Only an active session changes the displays (D-AUTH2). */
	server = object->client->server;
	active = displays_active(server);
	if (!active) {
		displays_result(object, request, KL_SYSTEM_RESULT_DENIED, 0U);
		return 0;
	}

	/* A choice made against the last snapshot. */
	if (server->compose == NULL) {
		displays_result(object, request, KL_SYSTEM_RESULT_UNAVAILABLE, 0U);
		return 0;
	}

	/* A snapshot that is no longer the last changes nothing. */
	if (serial != displays_state.serial) {
		printf("KWL DISPLAYS apply stale serial=%u now=%u\n", serial, displays_state.serial);
		displays_result(object, request, KL_SYSTEM_RESULT_STALE, 0U);
		return 0;
	}

	/* The mode of the two, and the places it names. */
	if (mode != KL_SYSTEM_DISPLAYS_EXTENDED && mode != KL_SYSTEM_DISPLAYS_MIRROR) {
		displays_result(object, request, KL_SYSTEM_RESULT_INVALID, 0U);
		return 0;
	}

	/* The choice now, with the mode and the places asked for. */
	wanted = server->compose->config;
	wanted.mode = KWL_DISPLAYS_EXTENDED;
	if (mode == KL_SYSTEM_DISPLAYS_MIRROR)
		wanted.mode = KWL_DISPLAYS_MIRROR;
	error = displays_places(places, &wanted);
	if (error != 0) {
		displays_result(object, request, KL_SYSTEM_RESULT_INVALID, 0U);
		return 0;
	}

	/* Applied (heads.c tells every object the new snapshot), or refused with nothing changed. */
	before = server->compose->display;
	error = kwl_displays_apply(server, &wanted, &saved);
	printf("KWL DISPLAYS apply client=%llu mode=%u error=%d saved=%d\n", (unsigned long long)object->client->number, mode, error, saved);
	if (error != 0) {
		displays_result(object, request, KL_SYSTEM_RESULT_INVALID, 0U);
		return 0;
	}

	/* Succeeded: applied, answered once written (and once the output's move is proven). */
	displays_answer(object, before, request, KL_SYSTEM_RESULT_OK, saved);
	return 0;
}

/* Carries out a set_brightness(request, key, percent). */
static int
displays_brightness_request(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_compose *compose;
	const char *key;
	uint32_t request;
	uint32_t percent;
	unsigned index;
	size_t next;
	int internal;
	int active;
	int differs;
	int saved;
	int error;

	/* The request's words and key. */
	if (size < 12U)
		return EPROTO;
	request = displays_word(bytes, 0U);
	error = displays_string(bytes, size, 4U, KL_SYSTEM_DISPLAY_KEY_MAX, &key, &next);
	if (error != 0 || next + 4U != size)
		return EPROTO;
	percent = displays_word(bytes, next);

	/* Only an active session changes the light (D-AUTH2). */
	server = object->client->server;
	active = displays_active(server);
	if (!active) {
		displays_result(object, request, KL_SYSTEM_RESULT_DENIED, 0U);
		return 0;
	}

	/* A light within 0 to 100. */
	if (percent > 100U) {
		displays_result(object, request, KL_SYSTEM_RESULT_INVALID, 0U);
		return 0;
	}

	/* The display named must be the built-in panel. */
	compose = server->compose;
	internal = 0;
	if (compose != NULL) {
		for (index = 0U; index < compose->display_count; index++) {
			differs = strcmp(compose->display_names[index], key);
			if (differs != 0)
				continue;
			internal = kwl_output_display_internal(server, compose->displays[index]);
			break;
		}
	}

	/* Another display has no light the compositor sets. */
	if (!internal) {
		displays_result(object, request, KL_SYSTEM_RESULT_UNSUPPORTED, 0U);
		return 0;
	}

	/* Set and kept; a machine without a light the compositor may set does not have it. */
	error = displays_light_set(server, percent, &saved);
	printf("KWL DISPLAYS light client=%llu percent=%u error=%d saved=%d\n", (unsigned long long)object->client->number, percent, error, saved);
	if (error != 0) {
		displays_result(object, request, KL_SYSTEM_RESULT_UNSUPPORTED, 0U);
		return 0;
	}

	/* Succeeded: set, answered once written (the snapshot went with the apply). */
	displays_hold(object, request, KL_SYSTEM_RESULT_OK, saved, 0U);
	return 0;
}

/* Carries out a set_shown(request, key, shown) (ws113-p014). */
static int
displays_shown_request(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	VkDisplayKHR before;
	const char *key;
	uint32_t request;
	uint32_t shown;
	uint32_t applied;
	size_t next;
	int active;
	int saved;
	int error;

	/* The request's words and key. */
	if (size < 12U)
		return EPROTO;
	request = displays_word(bytes, 0U);
	error = displays_string(bytes, size, 4U, KL_SYSTEM_DISPLAY_KEY_MAX, &key, &next);
	if (error != 0 || next + 4U != size)
		return EPROTO;
	shown = displays_word(bytes, next);

	/* Only an active session turns a display off or on (D-AUTH2). */
	server = object->client->server;
	active = displays_active(server);
	if (!active) {
		displays_result(object, request, KL_SYSTEM_RESULT_DENIED, 0U);
		return 0;
	}

	/* Turned off or on (heads.c), and the answer. */
	before = VK_NULL_HANDLE;
	if (server->compose != NULL)
		before = server->compose->display;
	error = kwl_displays_set_shown(server, key, shown != 0U, &saved);
	printf("KWL DISPLAYS shown client=%llu key=%s shown=%u error=%d saved=%d\n", (unsigned long long)object->client->number, key, shown, error, saved);
	switch (error) {
	case 0:
		applied = KL_SYSTEM_RESULT_OK;
		break;
	case EINVAL:
		applied = KL_SYSTEM_RESULT_INVALID;
		break;
	case ENOENT:
		applied = KL_SYSTEM_RESULT_UNAVAILABLE;
		break;
	default:
		applied = KL_SYSTEM_RESULT_FAILED;
		break;
	}

	/* Succeeded: answered, once written when it changed the choice. */
	if (error != 0)
		saved = error;
	displays_answer(object, before, request, applied, saved);
	return 0;
}

/*
 * Carries out a set_rotation(request, key, rotation) (ws217-p004): the
 * angle of the desktop's anchor chosen, shown at once and kept
 * (output-rotate.c); another display connected is not turned until a
 * head's angle (ws217-p005).
 */
static int
displays_rotation_request(
	struct kwl_object *object,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_compose *compose;
	const char *key;
	uint32_t request;
	uint32_t rotation;
	uint32_t applied;
	unsigned shown;
	unsigned index;
	size_t next;
	int connected;
	int differs;
	int active;
	int saved;
	int error;

	/* The request's words and key. */
	if (size < 12U)
		return EPROTO;
	request = displays_word(bytes, 0U);
	error = displays_string(bytes, size, 4U, KL_SYSTEM_DISPLAY_KEY_MAX, &key, &next);
	if (error != 0 || next + 4U != size)
		return EPROTO;
	rotation = displays_word(bytes, next);

	/* Only an active session turns a display (D-AUTH2). */
	server = object->client->server;
	active = displays_active(server);
	if (!active) {
		displays_result(object, request, KL_SYSTEM_RESULT_DENIED, 0U);
		return 0;
	}

	/* Whether the key names a display connected. */
	compose = server->compose;
	connected = 0;
	if (compose != NULL) {
		for (index = 0U; index < compose->display_count; index++) {
			differs = strcmp(compose->display_names[index], key);
			if (differs == 0)
				connected = 1;
		}
	}

	/* The anchor's angle chosen; another display connected cannot be turned yet; a key not connected is not found. */
	saved = -1;
	shown = 0U;
	error = ENOENT;
	if (connected) {
		error = ENOTSUP;
		differs = strcmp(compose->display_name, key);
		if (differs == 0)
			error = kwl_output_select_rotation(server, KWL_PLANE_ANCHOR, rotation, &shown, &saved);
	}
	printf("KWL DISPLAYS rotation client=%llu key=%s rotation=%u error=%d applied=%u saved=%d\n", (unsigned long long)object->client->number, key,
	    rotation, error, shown, saved);

	/* The answer for each outcome. */
	switch (error) {
	case 0:
		applied = KL_SYSTEM_RESULT_OK;
		break;
	case EINVAL:
		applied = KL_SYSTEM_RESULT_INVALID;
		break;
	case ENOTSUP:
		applied = KL_SYSTEM_RESULT_UNSUPPORTED;
		break;
	case ENOENT:
		applied = KL_SYSTEM_RESULT_UNAVAILABLE;
		break;
	default:
		applied = KL_SYSTEM_RESULT_FAILED;
		break;
	}

	/* Succeeded: answered, once written when the choice was given to the file. */
	if (error != 0)
		saved = error;
	displays_hold(object, request, applied, saved, 0U);
	return 0;
}

/*
 * Answers a bind_touch(request, identity, key) or a listen_touch(request,
 * key) as unsupported: the binding of the touch and pen devices to the
 * displays is ws217-p006's.
 */
static int
displays_touch_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const char *identity;
	const char *key;
	uint32_t request;
	size_t next;
	int error;

	/* The request's number. */
	if (size < 8U)
		return EPROTO;
	request = displays_word(bytes, 0U);

	/* A bind's identity, then the key both name. */
	next = 4U;
	identity = "";
	if (opcode == KL_SYSTEM_DISPLAYS_BIND_TOUCH) {
		error = displays_string(bytes, size, next, KL_SYSTEM_TOUCH_IDENTITY_MAX, &identity, &next);
		if (error != 0)
			return EPROTO;
	}
	error = displays_string(bytes, size, next, KL_SYSTEM_DISPLAY_KEY_MAX, &key, &next);
	if (error != 0 || next != size)
		return EPROTO;

	/* Succeeded: not in this compositor yet. */
	printf("KWL DISPLAYS touch client=%llu opcode=%u identity=%s key=%s unsupported\n", (unsigned long long)object->client->number, opcode, identity,
	    key);
	displays_result(object, request, KL_SYSTEM_RESULT_UNSUPPORTED, 0U);
	return 0;
}

/* Reads an apply's places ("KEY X Y" lines) into the choice; returns 0, or EINVAL for a line that is not one. */
static int
displays_places(
	const char *text,
	struct kwl_display_config *wanted)
{
	char line[KL_SYSTEM_DISPLAY_KEY_MAX + 32U];
	char key[KWL_DISPLAYS_KEY];
	const char *end;
	size_t length;
	int32_t x;
	int32_t y;
	int error;

	/* Each line in turn. */
	while (*text != '\0') {
		/* The line, without its end. */
		end = strchr(text, '\n');
		if (end == NULL)
			end = text + strlen(text);
		length = (size_t)(end - text);

		/* An empty line says nothing; a line too long is not a place. */
		if (length != 0U) {
			if (length >= sizeof(line))
				return EINVAL;
			memcpy(line, text, length);
			line[length] = '\0';
			error = kwl_displays_parse_place(line, key, sizeof(key), &x, &y);
			if (error != 0)
				return error;
			error = kwl_displays_set(wanted, key, x, y);
			if (error != 0)
				return error;
		}

		/* The next line. */
		text = end;
		if (*text == '\n')
			text++;
	}

	/* Succeeded: the places are in the choice. */
	return 0;
}

/*
 * Answers a request that changed the displays (saved: 0 when its choice
 * was given to displays.conf, else why not): once its writing is over,
 * and, when it moved the output to another display (from `before`), once
 * the move's first frame tells whether the move holds.
 */
static void
displays_answer(
	struct kwl_object *object,
	VkDisplayKHR before,
	uint32_t request,
	uint32_t applied,
	int saved)
{
	struct kwl_compose *compose;
	struct kwl_server *server;
	unsigned moving;

	/* Only an applied change that moved the output waits for the move's first frame. */
	server = object->client->server;
	compose = server->compose;
	moving = 0U;
	if (applied == KL_SYSTEM_RESULT_OK && compose != NULL && compose->switch_proving && compose->display != before)
		moving = 1U;

	/* Answered, or held back. */
	displays_hold(object, request, applied, saved, moving);
}

/*
 * Holds a result back until what it waits for is over: the writing of
 * displays.conf that holds its choice (saved 0; any other saved is not
 * written) and the output's move (moving).  A result that waits for
 * neither is answered at once.
 */
static void
displays_hold(
	struct kwl_object *object,
	uint32_t request,
	uint32_t applied,
	int saved,
	unsigned moving)
{
	struct displays_held **link;
	struct displays_held *held;
	uint64_t ticket;
	uint32_t written;
	int settled;
	int error;

	/* The writing it waits for, unless it is over already or none was given. */
	ticket = 0U;
	written = 0U;
	if (saved == 0) {
		ticket = kwl_heads_config_ticket();
		settled = kwl_heads_config_settled(ticket, &error);
		if (settled) {
			ticket = 0U;
			if (error == 0)
				written = 1U;
		}
	}

	/* Waiting for nothing: answered now. */
	if (ticket == 0U && !moving) {
		displays_result(object, request, applied, written);
		return;
	}

	/* An older result held for a move is settled first, as applied. */
	if (moving)
		kwl_displays_move_settled(object->client->server, 0);

	/* The record; without memory the result is answered now, not written yet. */
	held = calloc(1U, sizeof(*held));
	if (held == NULL) {
		printf("KWL DISPLAYS result request=%u answered unheld errno=%d\n", request, ENOMEM);
		displays_result(object, request, applied, written);
		return;
	}

	/* What it waits for. */
	held->object = object;
	held->request = request;
	held->applied = applied;
	held->written = written;
	held->ticket = ticket;
	held->moving = moving;
	held->since_ms = kwl_milliseconds();

	/* Kept after the results held before. */
	link = &displays_held_list;
	while (*link != NULL)
		link = &(*link)->next;
	*link = held;

	/* Logged for the tests. */
	printf("KWL DISPLAYS result held request=%u ticket=%llu move=%u\n", request, (unsigned long long)ticket, moving);
}

/* Answers, in the order held, every held result that waits for nothing more. */
static void
displays_held_settle(void)
{
	struct displays_held **link;
	struct displays_held *held;

	/* Each held result in its order. */
	link = &displays_held_list;
	while (*link != NULL) {
		held = *link;
		if (held->ticket != 0U || held->moving) {
			link = &held->next;
			continue;
		}

		/* Answered, and gone from the list. */
		*link = held->next;
		displays_result(held->object, held->request, held->applied, held->written);
		free(held);
	}
}

/* Sends an object a request's result. */
static void
displays_result(
	struct kwl_object *object,
	uint32_t request,
	uint32_t applied,
	uint32_t saved)
{
	uint32_t words[3];

	/* The request's number, how it ended, whether it was written. */
	words[0] = request;
	words[1] = applied;
	words[2] = saved;
	(void)kwl_emit(object->client, object->id, KL_SYSTEM_DISPLAYS_EVENT_RESULT, words, sizeof(words));
}

/* Tells whether the session is active: not the login screen, not locked (D-AUTH2). */
static int
displays_active(
	struct kwl_server *server)
{
	/* The login screen and a lock change nothing. */
	if (server->greeter)
		return 0;
	if (server->locked)
		return 0;

	/* Succeeded: the session is active. */
	return 1;
}

/* Reads a string argument of at most `bound` bytes with its NUL at `offset`; returns 0 or EPROTO. */
static int
displays_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	size_t bound,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t padded;
	size_t actual;

	/* Its length word. */
	if (offset + 4U > size)
		return EPROTO;
	length = displays_word(bytes, offset);
	if (length == 0U || length > bound)
		return EPROTO;

	/* Its bytes, padded to a word, ending with its NUL. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded > size)
		return EPROTO;
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;
	*text = (const char *)bytes + offset + 4U;
	actual = strlen(*text);
	if (actual + 1U != length)
		return EPROTO;

	/* Succeeded: the text and what follows it. */
	*next = offset + 4U + padded;
	return 0;
}

/* Puts a word in a payload; returns the offset after it. */
static size_t
displays_put_word(
	unsigned char *payload,
	size_t offset,
	uint32_t word)
{
	/* The word, as the wire carries it. */
	memcpy(payload + offset, &word, sizeof(word));
	return offset + sizeof(word);
}

/* Puts a string (its length, its bytes and NUL, padded to a word) in a payload; returns the offset after it. */
static size_t
displays_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* The length with the NUL, the bytes, and zeros to the word. */
	length = (uint32_t)strlen(text) + 1U;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, sizeof(length));
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length);
	return offset + 4U + padded;
}

/* Reads a word of a request. */
static uint32_t
displays_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The bytes may not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
