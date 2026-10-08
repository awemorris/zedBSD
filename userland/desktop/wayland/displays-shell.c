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
 */

#include "kwl.h"
#include "compose.h"
#include "displays.h"

#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
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
 * A result held back until the move of the output it caused is proven by
 * the move's first frame (BUG-266): the object (NULL when none waits), its
 * request, whether the choice was written, and since when it waits.  The
 * object's going clears it (kwl_displays_object_gone); a newer held result
 * answers the older one first.
 */
static struct {
	struct kwl_object *object;
	uint32_t request;
	uint32_t written;
	uint64_t since_ms;
} displays_pending;

static void displays_snapshot(struct kwl_object *object);
static void displays_output(struct kwl_object *object, struct kwl_server *server, unsigned index);
static void displays_label(const char *key, char *label, size_t size);
static int displays_light_open(struct kwl_server *server);
static int displays_light_get(struct kwl_server *server, unsigned *percent);
static int displays_light_set(struct kwl_server *server, unsigned percent, int *saved);
static int displays_apply_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_brightness_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_shown_request(struct kwl_object *object, const unsigned char *bytes, size_t size);
static int displays_places(const char *text, struct kwl_display_config *wanted);
static void displays_result(struct kwl_object *object, uint32_t request, uint32_t applied, uint32_t saved);
static void displays_answer(struct kwl_object *object, VkDisplayKHR before, uint32_t request, uint32_t applied, uint32_t written);
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

	/* No other request. */
	return EPROTO;
}

/*
 * Answers the result held back for a move of the output (BUG-266), once
 * the move's first frame was presented or refused: a refused one is
 * FAILED, the choice not holding.
 */
void
kwl_displays_move_settled(
	struct kwl_server *server,
	int failed)
{
	uint32_t applied;

	UNUSED_PARAMETER(server);

	/* Nothing held back. */
	if (displays_pending.object == NULL)
		return;

	/* How the move ended. */
	applied = KL_SYSTEM_RESULT_OK;
	if (failed)
		applied = KL_SYSTEM_RESULT_FAILED;

	/* The answer, and nothing held back any more. */
	printf("KWL DISPLAYS result held request=%u applied=%u\n", displays_pending.request, applied);
	displays_result(displays_pending.object, displays_pending.request, applied, displays_pending.written);
	displays_pending.object = NULL;
}

/*
 * Answers a result held back too long (the move's first frame never came:
 * nothing was drawn, or the output was moved again), as applied.
 */
void
kwl_displays_pending_tick(
	struct kwl_server *server,
	uint64_t now)
{
	/* Nothing held back, or not long yet. */
	if (displays_pending.object == NULL)
		return;
	if (now - displays_pending.since_ms < DISPLAYS_PENDING_MS)
		return;

	/* Answered as the choice applied. */
	kwl_displays_move_settled(server, 0);
}

/*
 * Forgets a held result of a displays object that goes.
 */
void
kwl_displays_object_gone(
	struct kwl_object *object)
{
	/* Only the object that waits. */
	if (displays_pending.object != object)
		return;

	/* Nobody to answer. */
	displays_pending.object = NULL;
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
 * Sets the light displays.conf keeps once a session's first frame is up
 * (D-BRIGHT-BOOT: the panel is not lit by the driver before it); a light
 * not kept stays as the firmware left it.
 */
void
kwl_displays_tick(
	struct kwl_server *server)
{
	struct kwl_compose *compose;
	int error;

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

	/* The panel's light, where the machine has one the compositor may set. */
	error = displays_light_open(server);
	if (error != 0)
		return;
	error = kl_backend_backlight_set(server->backlight, compose->config.brightness);
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

/* Opens the panel's light the first time it is needed (shared with the lid's); returns 0 or the backend's error. */
static int
displays_light_open(
	struct kwl_server *server)
{
	int error;

	/* Open already. */
	if (server->backlight != NULL)
		return 0;

	/* Opened now. */
	error = kl_backend_backlight_open(&server->backlight);
	if (error != 0) {
		server->backlight = NULL;
		return error;
	}

	/* Succeeded: the light is open. */
	return 0;
}

/* Reads the panel's light: the one kept for the opening while the lid holds the panel out. */
static int
displays_light_get(
	struct kwl_server *server,
	unsigned *percent)
{
	int error;

	/* The light. */
	error = displays_light_open(server);
	if (error != 0)
		return error;

	/* Kept while the lid holds it out. */
	if (server->backlight_out) {
		*percent = server->backlight_saved;
		return 0;
	}

	/* The driver's. */
	error = kl_backend_backlight_get(server->backlight, percent);
	if (error != 0)
		return error;

	/* Succeeded: the light. */
	return 0;
}

/*
 * Sets the panel's light (kept for the opening while the lid holds it
 * out) and keeps it in displays.conf; *saved is 0 or the error of writing
 * it.  Returns 0 or the backend's error.
 */
static int
displays_light_set(
	struct kwl_server *server,
	unsigned percent,
	int *saved)
{
	struct kwl_display_config wanted;
	int error;

	/* The light. */
	*saved = 0;
	error = displays_light_open(server);
	if (error != 0)
		return error;

	/* Kept for the opening, or set now. */
	if (server->backlight_out) {
		server->backlight_saved = percent;
	} else {
		error = kl_backend_backlight_set(server->backlight, percent);
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
	uint32_t written;
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

	/* Succeeded: applied, and whether it was written (held back while the output's move is not proven). */
	written = 0U;
	if (saved == 0)
		written = 1U;
	displays_answer(object, before, request, KL_SYSTEM_RESULT_OK, written);
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
	uint32_t written;
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

	/* Succeeded: set, and whether it was written (the snapshot went with the apply). */
	written = 0U;
	if (saved == 0)
		written = 1U;
	displays_result(object, request, KL_SYSTEM_RESULT_OK, written);
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
	uint32_t written;
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

	/* Succeeded: answered, with whether it was written. */
	written = 0U;
	if (error == 0 && saved == 0)
		written = 1U;
	displays_answer(object, before, request, applied, written);
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
 * Answers a request that changed the displays: at once, or, when it moved
 * the output to another display (from `before`), once the move's first
 * frame tells whether the move holds (BUG-266).
 */
static void
displays_answer(
	struct kwl_object *object,
	VkDisplayKHR before,
	uint32_t request,
	uint32_t applied,
	uint32_t written)
{
	struct kwl_compose *compose;
	struct kwl_server *server;

	/* A refusal, or a change that did not move the output, is answered at once. */
	server = object->client->server;
	compose = server->compose;
	if (applied != KL_SYSTEM_RESULT_OK ||
	    compose == NULL ||
	    !compose->switch_proving ||
	    compose->display == before) {
		displays_result(object, request, applied, written);
		return;
	}

	/* An older result held back is answered first, as applied. */
	kwl_displays_move_settled(server, 0);

	/* Held back until the move's first frame. */
	displays_pending.object = object;
	displays_pending.request = request;
	displays_pending.written = written;
	displays_pending.since_ms = kwl_milliseconds();
	printf("KWL DISPLAYS result held request=%u until the move's first frame\n", request);
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
