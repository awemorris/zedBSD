/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RANDR (version 1.5) over the monitors the backend tells the core, and
 * the root window's size.
 *
 * The backend tells the core the root window's size and the system's
 * monitors on it (x11server_backend_screen); a backend that never tells
 * has one monitor, the whole root window.  Each monitor is shown as one
 * connected output with a CRTC of its own and one mode of its size, in
 * ids no client chooses.  The clients read the layout (the versions 1.0
 * and 1.1 requests, the screen resources, outputs, CRTCs, monitors), but
 * the system owns it: a request that would change it is refused, and one
 * that asks for the layout as it stands succeeds.  When the layout or the
 * root window's size changes, the root window follows (its pixels too on
 * a rooted server), and the clients that asked hear RRScreenChangeNotify
 * and RRNotify.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The version the core speaks. */
#define RANDR_VERSION_MAJOR		1U
#define RANDR_VERSION_MINOR		5U

/* The requests, by their minor opcodes. */
#define RANDR_QUERY_VERSION		0U
#define RANDR_SET_SCREEN_CONFIG		2U
#define RANDR_SELECT_INPUT		4U
#define RANDR_GET_SCREEN_INFO		5U
#define RANDR_GET_SCREEN_SIZE_RANGE	6U
#define RANDR_SET_SCREEN_SIZE		7U
#define RANDR_GET_SCREEN_RESOURCES	8U
#define RANDR_GET_OUTPUT_INFO		9U
#define RANDR_LIST_OUTPUT_PROPERTIES	10U
#define RANDR_QUERY_OUTPUT_PROPERTY	11U
#define RANDR_CONFIGURE_OUTPUT_PROPERTY	12U
#define RANDR_CHANGE_OUTPUT_PROPERTY	13U
#define RANDR_DELETE_OUTPUT_PROPERTY	14U
#define RANDR_GET_OUTPUT_PROPERTY	15U
#define RANDR_CREATE_MODE		16U
#define RANDR_DESTROY_MODE		17U
#define RANDR_ADD_OUTPUT_MODE		18U
#define RANDR_DELETE_OUTPUT_MODE	19U
#define RANDR_GET_CRTC_INFO		20U
#define RANDR_SET_CRTC_CONFIG		21U
#define RANDR_GET_CRTC_GAMMA_SIZE	22U
#define RANDR_GET_CRTC_GAMMA		23U
#define RANDR_SET_CRTC_GAMMA		24U
#define RANDR_GET_SCREEN_RESOURCES_CURRENT 25U
#define RANDR_SET_CRTC_TRANSFORM	26U
#define RANDR_GET_CRTC_TRANSFORM	27U
#define RANDR_GET_PANNING		28U
#define RANDR_SET_PANNING		29U
#define RANDR_SET_OUTPUT_PRIMARY	30U
#define RANDR_GET_OUTPUT_PRIMARY	31U
#define RANDR_GET_PROVIDERS		32U
#define RANDR_GET_PROVIDER_INFO		33U
#define RANDR_GET_PROVIDER_PROPERTY	41U
#define RANDR_GET_MONITORS		42U
#define RANDR_SET_MONITOR		43U
#define RANDR_DELETE_MONITOR		44U

/* The events a client may select, and all of them together. */
#define RANDR_SCREEN_CHANGE_MASK	0x0001U
#define RANDR_CRTC_CHANGE_MASK		0x0002U
#define RANDR_OUTPUT_CHANGE_MASK	0x0004U
#define RANDR_RESOURCE_CHANGE_MASK	0x0040U
#define RANDR_ALL_MASKS			0x00ffU

/* RRNotify's kinds: a CRTC, an output, and the resources changed. */
#define RANDR_NOTIFY_CRTC		0U
#define RANDR_NOTIFY_OUTPUT		1U
#define RANDR_NOTIFY_RESOURCE		5U

/* The errors after the extension's first: an unknown output, CRTC, mode and provider. */
#define RANDR_BAD_OUTPUT		(X11_RANDR_ERROR + 0U)
#define RANDR_BAD_CRTC			(X11_RANDR_ERROR + 1U)
#define RANDR_BAD_MODE			(X11_RANDR_ERROR + 2U)
#define RANDR_BAD_PROVIDER		(X11_RANDR_ERROR + 3U)

/* What a request to set the layout answers: it was done, the client's view was out of date, or it cannot be done. */
#define RANDR_SET_SUCCESS		0U
#define RANDR_SET_INVALID_CONFIG_TIME	1U
#define RANDR_SET_FAILED		3U

/* The one rotation (none), the output's connection, and its unknown sub-pixel order. */
#define RANDR_ROTATE_0			1U
#define RANDR_CONNECTED			0U
#define RANDR_SUBPIXEL_UNKNOWN		0U

/* A mode's flags: positive horizontal and vertical sync. */
#define RANDR_MODE_FLAGS		0x00000005U

/* The refresh rate of a monitor the backend gives none (hertz), and the highest taken. */
#define RANDR_REFRESH_DEFAULT		60U
#define RANDR_REFRESH_MAX		1000U

/* The largest root window, monitor and place on it (X's 16-bit sizes and places). */
#define RANDR_SIDE_MAX			32767U
#define RANDR_PLACE_MIN			(-32768)
#define RANDR_PLACE_MAX			32767

/* The name of the one monitor of a backend that names none. */
#define RANDR_DEFAULT_NAME		"screen"

/*
 * The ids the monitors' modes are given: each new size of a monitor takes
 * the next, from the first to the last and round again, so that a client
 * that kept a mode's id does not read another size under it.
 */
#define RANDR_MODE_FIRST		X11_RANDR_MODE_XID
#define RANDR_MODE_LAST			0x003fffffU

/* The most bytes the longest reply takes (the screen resources of every monitor). */
#define RANDR_REPLY_MAX			2048U

/* The bytes of one mode in a reply, of one monitor in GetMonitors, and of a transform. */
#define RANDR_MODE_INFO_SIZE		32U
#define RANDR_MONITOR_INFO_SIZE		24U
#define RANDR_TRANSFORM_SIZE		36U

/* The value 1 as RANDR's fixed-point numbers (16.16) write it. */
#define RANDR_FIXED_ONE			0x00010000U

/*
 * One client's selection of RANDR's events on one window: the window, the
 * client's slot and the events it asked for (never 0: a selection of none
 * is removed).
 */
struct randr_select {
	uint32_t window;
	unsigned client;
	uint16_t mask;
};

/*
 * RANDR's selections of events, made when a client first selects any:
 * they go with the client and with the window.
 */
struct x11_randr {
	struct randr_select *selects;
	unsigned count;
	unsigned capacity;
};

static unsigned randr_query_version(struct x11server *server, struct x11_client *client, size_t length);
static unsigned randr_set_screen_config(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_select_input(struct x11server *server, unsigned index, const uint8_t *request, size_t length);
static unsigned randr_get_screen_info(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_screen_size_range(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_set_screen_size(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_screen_resources(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_output_info(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_output_property(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_output_property(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_mode_change(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_crtc_info(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_set_crtc_config(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_crtc_gamma(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_crtc_transform(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_panning(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_set_output_primary(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_output_primary(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_providers(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_get_monitors(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned randr_set_monitor(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static int randr_window_known(struct x11server *server, struct x11_client *client, const uint8_t *request);
static unsigned randr_crtc_of(struct x11server *server, uint32_t id);
static unsigned randr_output_of(struct x11server *server, uint32_t id);
static unsigned randr_mode_of(struct x11server *server, uint32_t id);
static unsigned randr_screen_rate(struct x11server *server);
static unsigned randr_millimetres(unsigned pixels);
static unsigned randr_refresh(const struct x11_monitor *monitor);
static void randr_mode_info(struct x11server *server, unsigned monitor, uint8_t *info, int msb);
static size_t randr_mode_name(struct x11server *server, unsigned monitor, char *name, size_t size);
static void randr_monitors_take(struct x11server *server, unsigned *width, unsigned *height, const struct x11_monitor *monitors, unsigned count);
static void randr_monitor_clean(struct x11_monitor *monitor);
static uint32_t randr_mode_next(struct x11server *server);
static void randr_root_resize(struct x11server *server, unsigned width, unsigned height);
static void randr_tell(struct x11server *server, const struct x11_monitor *old, unsigned old_count, int resources);
static void randr_tell_screen(struct x11server *server, struct x11_client *client, uint32_t window);
static void randr_tell_crtc(struct x11server *server, struct x11_client *client, uint32_t window, unsigned monitor);
static void randr_tell_output(struct x11server *server, struct x11_client *client, uint32_t window, unsigned monitor);
static void randr_tell_resources(struct x11server *server, struct x11_client *client, uint32_t window);
static void randr_event_send(struct x11server *server, struct x11_client *client, uint8_t *event);
static int randr_monitor_same(const struct x11_monitor *one, const struct x11_monitor *other);
static int randr_mode_same(const struct x11_monitor *one, const struct x11_monitor *other);

/*
 * Gives a new server its one monitor, the whole root window, until the
 * backend tells it the system's.
 */
void
x11_randr_init(
	struct x11server *server)
{
	/* The whole root window, named as X.Org's virtual screens name theirs, the main one of none. */
	memset(server->monitors, 0, sizeof(server->monitors));
	server->monitors[0].width = server->width;
	server->monitors[0].height = server->height;
	(void)snprintf(server->monitors[0].name, sizeof(server->monitors[0].name), "%s", RANDR_DEFAULT_NAME);
	randr_monitor_clean(&server->monitors[0]);
	server->monitor_count = 1U;
	server->monitor_primary = X11_NO_MONITOR;

	/* Its mode, and the layout as of now. */
	server->monitor_mode_serial = RANDR_MODE_FIRST;
	server->monitor_modes[0] = randr_mode_next(server);
	server->monitor_config_time = server->now;
	server->monitor_set_time = server->now;
}

/*
 * Answers a RANDR request.  Returns 0, or the error to send.
 */
unsigned
x11_randr_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	unsigned code;

	/* The client, and each request by its minor opcode. */
	client = &server->clients[index];
	switch (request[1]) {
	case RANDR_QUERY_VERSION:
		code = randr_query_version(server, client, length);
		break;
	case RANDR_SET_SCREEN_CONFIG:
		code = randr_set_screen_config(server, client, request, length);
		break;
	case RANDR_SELECT_INPUT:
		code = randr_select_input(server, index, request, length);
		break;
	case RANDR_GET_SCREEN_INFO:
		code = randr_get_screen_info(server, client, request, length);
		break;
	case RANDR_GET_SCREEN_SIZE_RANGE:
		code = randr_get_screen_size_range(server, client, request, length);
		break;
	case RANDR_SET_SCREEN_SIZE:
		code = randr_set_screen_size(server, client, request, length);
		break;
	case RANDR_GET_SCREEN_RESOURCES:
	case RANDR_GET_SCREEN_RESOURCES_CURRENT:
		code = randr_get_screen_resources(server, client, request, length);
		break;
	case RANDR_GET_OUTPUT_INFO:
		code = randr_get_output_info(server, client, request, length);
		break;
	case RANDR_LIST_OUTPUT_PROPERTIES:
	case RANDR_QUERY_OUTPUT_PROPERTY:
	case RANDR_CONFIGURE_OUTPUT_PROPERTY:
	case RANDR_CHANGE_OUTPUT_PROPERTY:
	case RANDR_DELETE_OUTPUT_PROPERTY:
		code = randr_output_property(server, client, request, length);
		break;
	case RANDR_GET_OUTPUT_PROPERTY:
		code = randr_get_output_property(server, client, request, length);
		break;
	case RANDR_CREATE_MODE:
	case RANDR_DESTROY_MODE:
	case RANDR_ADD_OUTPUT_MODE:
	case RANDR_DELETE_OUTPUT_MODE:
		code = randr_mode_change(server, client, request, length);
		break;
	case RANDR_GET_CRTC_INFO:
		code = randr_get_crtc_info(server, client, request, length);
		break;
	case RANDR_SET_CRTC_CONFIG:
		code = randr_set_crtc_config(server, client, request, length);
		break;
	case RANDR_GET_CRTC_GAMMA_SIZE:
	case RANDR_GET_CRTC_GAMMA:
	case RANDR_SET_CRTC_GAMMA:
		code = randr_crtc_gamma(server, client, request, length);
		break;
	case RANDR_SET_CRTC_TRANSFORM:
	case RANDR_GET_CRTC_TRANSFORM:
		code = randr_get_crtc_transform(server, client, request, length);
		break;
	case RANDR_GET_PANNING:
	case RANDR_SET_PANNING:
		code = randr_panning(server, client, request, length);
		break;
	case RANDR_SET_OUTPUT_PRIMARY:
		code = randr_set_output_primary(server, client, request, length);
		break;
	case RANDR_GET_OUTPUT_PRIMARY:
		code = randr_get_output_primary(server, client, request, length);
		break;
	case RANDR_GET_PROVIDERS:
		code = randr_get_providers(server, client, request, length);
		break;
	case RANDR_GET_MONITORS:
		code = randr_get_monitors(server, client, request, length);
		break;
	case RANDR_SET_MONITOR:
	case RANDR_DELETE_MONITOR:
		code = randr_set_monitor(server, client, request, length);
		break;
	default:
		/* The providers' requests name a provider, and there is none; the rest are not known. */
		code = X11_BAD_REQUEST;
		if (request[1] >= RANDR_GET_PROVIDER_INFO && request[1] <= RANDR_GET_PROVIDER_PROPERTY)
			code = RANDR_BAD_PROVIDER;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Takes the root window's size and the system's monitors on it from the
 * backend (count 0: one monitor, the whole root window).  When either
 * changed, the root window follows and the clients that asked are told.
 */
void
x11server_backend_screen(
	struct x11server *server,
	unsigned width,
	unsigned height,
	const struct x11_monitor *monitors,
	unsigned count)
{
	struct x11_monitor old[X11_MONITORS_MAX];
	unsigned old_count;
	unsigned index;
	int resized;
	int same;

	/* The layout as it was. */
	memcpy(old, server->monitors, sizeof(old));
	old_count = server->monitor_count;

	/* The new layout, cleaned, and whether the root window's size changes. */
	randr_monitors_take(server, &width, &height, monitors, count);
	resized = 0;
	if (width != server->width || height != server->height)
		resized = 1;

	/* Nothing changed: nobody is told. */
	same = 0;
	if (!resized && server->monitor_count == old_count)
		same = 1;
	for (index = 0U; same && index < old_count; index++) {
		same = randr_monitor_same(&old[index], &server->monitors[index]);
	}
	if (same)
		return;

	/* A monitor that changed its mode (its size or rate) has a new mode's id, one that kept it its old one. */
	for (index = 0U; index < server->monitor_count; index++) {
		same = 0;
		if (index < old_count)
			same = randr_mode_same(&old[index], &server->monitors[index]);
		if (same)
			continue;
		server->monitor_modes[index] = randr_mode_next(server);
	}

	/* The layout of now, as RANDR's times count it. */
	server->monitor_config_time = server->now;
	server->monitor_set_time = server->now;
	x11_log(server, "X11SERVER SCREEN %ux%u monitors=%u", width, height, server->monitor_count);

	/* The root window takes the new size. */
	if (resized)
		randr_root_resize(server, width, height);

	/* Succeeded: the clients that asked are told what changed. */
	randr_tell(server, old, old_count, server->monitor_count != old_count);
}

/*
 * Forgets a closed client's selections of RANDR's events.
 */
void
x11_randr_forget_client(
	struct x11server *server,
	unsigned index)
{
	struct x11_randr *randr;
	unsigned at;

	/* Nothing was selected. */
	randr = server->randr;
	if (randr == NULL)
		return;

	/* Each of the client's, the last one taking its place. */
	at = 0U;
	while (at < randr->count) {
		if (randr->selects[at].client != index) {
			at++;
			continue;
		}
		randr->count--;
		randr->selects[at] = randr->selects[randr->count];
	}
}

/*
 * Forgets the selections of RANDR's events on a window going away.
 */
void
x11_randr_forget_window(
	struct x11server *server,
	uint32_t window)
{
	struct x11_randr *randr;
	unsigned at;

	/* Nothing was selected. */
	randr = server->randr;
	if (randr == NULL)
		return;

	/* Each on the window, the last one taking its place. */
	at = 0U;
	while (at < randr->count) {
		if (randr->selects[at].window != window) {
			at++;
			continue;
		}
		randr->count--;
		randr->selects[at] = randr->selects[randr->count];
	}
}

/*
 * Frees RANDR's selections when the server ends.
 */
void
x11_randr_fini(
	struct x11server *server)
{
	/* Nothing was selected. */
	if (server->randr == NULL)
		return;

	/* Succeeded: the selections go. */
	free(server->randr->selects);
	free(server->randr);
	server->randr = NULL;
}

/* QueryVersion: the version the core speaks (1.5), whatever the client asks for. */
static unsigned
randr_query_version(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The client's version, two words. */
	if (length != 12U)
		return X11_BAD_LENGTH;

	/* Succeeded: the core's version. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, RANDR_VERSION_MAJOR, client->order);
	x11_write32(reply + 12, RANDR_VERSION_MINOR, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetScreenConfig (versions 1.0 and 1.1): the one size, unrotated, at its
 * rate, succeeds; a client whose view of the layout is old is told so.
 */
static unsigned
randr_set_screen_config(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint32_t config_time;
	uint16_t size;
	uint16_t rotation;
	uint16_t rate;
	unsigned status;
	unsigned shown_rate;
	int known;

	/* A drawable, the times, the size, the rotation and (version 1.1) the rate. */
	if (length != 20U && length != 24U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_DRAWABLE;
	config_time = x11_read32(request + 12, client->order);
	size = x11_read16(request + 16, client->order);
	rotation = x11_read16(request + 18, client->order);
	rate = 0U;
	if (length == 24U)
		rate = x11_read16(request + 20, client->order);

	/* The one size, unrotated, at its own rate or any. */
	shown_rate = randr_screen_rate(server);
	if (size != 0U)
		return X11_BAD_VALUE;
	if (rotation != RANDR_ROTATE_0)
		return X11_BAD_VALUE;
	if (rate != 0U && rate != shown_rate)
		return X11_BAD_VALUE;

	/* A client that read the layout before it last changed is told; else the layout it asks for is already there. */
	status = RANDR_SET_SUCCESS;
	if (config_time != server->monitor_config_time)
		status = RANDR_SET_INVALID_CONFIG_TIME;

	/* Succeeded: the status, the times, the root window and its sub-pixel order. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_write32(reply + 12, server->monitor_config_time, client->order);
	x11_write32(reply + 16, X11_ROOT_XID, client->order);
	x11_write16(reply + 20, RANDR_SUBPIXEL_UNKNOWN, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* SelectInput: the RANDR events a client asks for on a window (none: its selection goes). */
static unsigned
randr_select_input(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	struct x11_randr *randr;
	struct randr_select *grown;
	struct x11_window *window;
	unsigned capacity;
	unsigned at;
	uint16_t mask;

	/* The window and the events. */
	client = &server->clients[index];
	if (length != 12U)
		return X11_BAD_LENGTH;
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return X11_BAD_WINDOW;
	mask = x11_read16(request + 8, client->order);
	if ((mask & ~RANDR_ALL_MASKS) != 0U)
		return X11_BAD_VALUE;

	/* The selections, made with the first. */
	randr = server->randr;
	if (randr == NULL) {
		randr = calloc(1, sizeof(*randr));
		if (randr == NULL)
			return X11_BAD_ALLOC;
		server->randr = randr;
	}

	/* The client's selection on the window: changed, or gone when it asks for none. */
	for (at = 0U; at < randr->count; at++) {
		if (randr->selects[at].window != window->id || randr->selects[at].client != index)
			continue;
		if (mask == 0U) {
			randr->count--;
			randr->selects[at] = randr->selects[randr->count];
			return 0U;
		}
		randr->selects[at].mask = mask;
		return 0U;
	}

	/* None, and none asked for: nothing to keep. */
	if (mask == 0U)
		return 0U;

	/* Room for one more. */
	if (randr->count == randr->capacity) {
		capacity = randr->capacity * 2U;
		if (capacity == 0U)
			capacity = 8U;
		grown = realloc(randr->selects, capacity * sizeof(*grown));
		if (grown == NULL)
			return X11_BAD_ALLOC;
		randr->selects = grown;
		randr->capacity = capacity;
	}

	/* Succeeded: kept. */
	randr->selects[randr->count].window = window->id;
	randr->selects[randr->count].client = index;
	randr->selects[randr->count].mask = mask;
	randr->count++;
	return 0U;
}

/*
 * GetScreenInfo (versions 1.0 and 1.1): one size, the root window's,
 * unrotated, at the main monitor's rate.
 */
static unsigned
randr_get_screen_info(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32U + 8U + 4U];
	unsigned rate;
	int known;

	/* A window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* The rotations there are, the root window, the times, one size (the one shown) and its rate. */
	rate = randr_screen_rate(server);
	memset(reply, 0, sizeof(reply));
	reply[1] = RANDR_ROTATE_0;
	x11_write32(reply + 8, X11_ROOT_XID, client->order);
	x11_write32(reply + 12, server->monitor_set_time, client->order);
	x11_write32(reply + 16, server->monitor_config_time, client->order);
	x11_write16(reply + 20, 1U, client->order);
	x11_write16(reply + 22, 0U, client->order);
	x11_write16(reply + 24, RANDR_ROTATE_0, client->order);
	x11_write16(reply + 26, (uint16_t)rate, client->order);
	x11_write16(reply + 28, 2U, client->order);

	/* The size: the root window's, in pixels and millimetres. */
	x11_write16(reply + 32, (uint16_t)server->width, client->order);
	x11_write16(reply + 34, (uint16_t)server->height, client->order);
	x11_write16(reply + 36, (uint16_t)randr_millimetres(server->width), client->order);
	x11_write16(reply + 38, (uint16_t)randr_millimetres(server->height), client->order);

	/* Succeeded: its rates, one. */
	x11_write16(reply + 40, 1U, client->order);
	x11_write16(reply + 42, (uint16_t)rate, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetScreenSizeRange: the root window's size, the only one (the system sets it). */
static unsigned
randr_get_screen_size_range(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	int known;

	/* A window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* Succeeded: the smallest and the largest, both the size it has. */
	memset(reply, 0, sizeof(reply));
	x11_write16(reply + 8, (uint16_t)server->width, client->order);
	x11_write16(reply + 10, (uint16_t)server->height, client->order);
	x11_write16(reply + 12, (uint16_t)server->width, client->order);
	x11_write16(reply + 14, (uint16_t)server->height, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* SetScreenSize: the size the root window has succeeds; any other is outside the range. */
static unsigned
randr_set_screen_size(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint16_t width;
	uint16_t height;
	int known;

	/* A window, the size in pixels and in millimetres. */
	if (length != 20U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;
	width = x11_read16(request + 8, client->order);
	height = x11_read16(request + 10, client->order);

	/* Another size than the root window's is outside the range. */
	if (width != server->width || height != server->height)
		return X11_BAD_VALUE;

	/* Succeeded: it already has that size. */
	return 0U;
}

/*
 * GetScreenResources and GetScreenResourcesCurrent: the times, each
 * monitor's CRTC, output and mode, and the modes' names.
 */
static unsigned
randr_get_screen_resources(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[RANDR_REPLY_MAX];
	char name[X11_MONITOR_NAME_MAX];
	size_t name_length;
	size_t names;
	size_t used;
	unsigned count;
	unsigned index;
	int known;

	/* A window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* The times, and how many CRTCs, outputs and modes. */
	count = server->monitor_count;
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_write32(reply + 12, server->monitor_config_time, client->order);
	x11_write16(reply + 16, (uint16_t)count, client->order);
	x11_write16(reply + 18, (uint16_t)count, client->order);
	x11_write16(reply + 20, (uint16_t)count, client->order);

	/* The CRTCs, then the outputs. */
	used = 32U;
	for (index = 0U; index < count; index++) {
		x11_write32(reply + used, X11_RANDR_CRTC_XID + index, client->order);
		used += 4U;
	}
	for (index = 0U; index < count; index++) {
		x11_write32(reply + used, X11_RANDR_OUTPUT_XID + index, client->order);
		used += 4U;
	}

	/* The modes. */
	for (index = 0U; index < count; index++) {
		randr_mode_info(server, index, reply + used, client->order);
		used += RANDR_MODE_INFO_SIZE;
	}

	/* The modes' names, one after another. */
	names = 0U;
	for (index = 0U; index < count; index++) {
		name_length = randr_mode_name(server, index, name, sizeof(name));
		memcpy(reply + used, name, name_length);
		used += name_length;
		names += name_length;
	}

	/* Succeeded: the names' bytes, and the reply padded to words. */
	x11_write16(reply + 22, (uint16_t)names, client->order);
	used = (used + 3U) & ~(size_t)3U;
	x11_reply(server, client, reply, used);
	return 0U;
}

/*
 * GetOutputInfo: a monitor's output, connected, on its own CRTC, with
 * its one mode (preferred) and its name.
 */
static unsigned
randr_get_output_info(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct x11_monitor *monitor;
	uint8_t reply[36U + 8U + X11_MONITOR_NAME_MAX + 4U];
	size_t name_length;
	size_t used;
	unsigned index;

	/* The output, and the layout's time the client read it at. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	index = randr_output_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_OUTPUT;
	monitor = &server->monitors[index];
	name_length = strlen(monitor->name);

	/* Its CRTC, physical size, connection, one CRTC, one mode (preferred), no clones and its name's length. */
	memset(reply, 0, sizeof(reply));
	reply[1] = RANDR_SET_SUCCESS;
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_write32(reply + 12, X11_RANDR_CRTC_XID + index, client->order);
	x11_write32(reply + 16, monitor->width_mm, client->order);
	x11_write32(reply + 20, monitor->height_mm, client->order);
	reply[24] = RANDR_CONNECTED;
	reply[25] = RANDR_SUBPIXEL_UNKNOWN;
	x11_write16(reply + 26, 1U, client->order);
	x11_write16(reply + 28, 1U, client->order);
	x11_write16(reply + 30, 1U, client->order);
	x11_write16(reply + 32, 0U, client->order);
	x11_write16(reply + 34, (uint16_t)name_length, client->order);

	/* Succeeded: the CRTC, the mode and the name, padded to words. */
	used = 36U;
	x11_write32(reply + used, X11_RANDR_CRTC_XID + index, client->order);
	used += 4U;
	x11_write32(reply + used, server->monitor_modes[index], client->order);
	used += 4U;
	memcpy(reply + used, monitor->name, name_length);
	used += name_length;
	used = (used + 3U) & ~(size_t)3U;
	x11_reply(server, client, reply, used);
	return 0U;
}

/*
 * An output's properties: it has none.  ListOutputProperties names none,
 * QueryOutputProperty finds none, ConfigureOutputProperty and
 * ChangeOutputProperty are refused (the system owns the outputs), and
 * DeleteOutputProperty has nothing to delete.
 */
static unsigned
randr_output_property(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	unsigned index;
	int valid;

	/* An output, and (but for the list) a property. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	index = randr_output_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_OUTPUT;

	/* ListOutputProperties: none. */
	if (request[1] == RANDR_LIST_OUTPUT_PROPERTIES) {
		if (length != 8U)
			return X11_BAD_LENGTH;
		memset(reply, 0, sizeof(reply));
		x11_reply(server, client, reply, sizeof(reply));
		return 0U;
	}

	/* The property, an atom. */
	if (length < 12U)
		return X11_BAD_LENGTH;
	valid = x11_atom_valid(server, x11_read32(request + 8, client->order));
	if (!valid)
		return X11_BAD_ATOM;

	/* QueryOutputProperty: no such property. */
	if (request[1] == RANDR_QUERY_OUTPUT_PROPERTY) {
		if (length != 12U)
			return X11_BAD_LENGTH;
		return X11_BAD_NAME;
	}

	/* DeleteOutputProperty: nothing there, nothing done. */
	if (request[1] == RANDR_DELETE_OUTPUT_PROPERTY) {
		if (length != 12U)
			return X11_BAD_LENGTH;
		return 0U;
	}

	/* Succeeded in refusing: ConfigureOutputProperty and ChangeOutputProperty. */
	return X11_BAD_ACCESS;
}

/* GetOutputProperty: the property is not there (type None). */
static unsigned
randr_get_output_property(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	unsigned index;
	int valid;

	/* An output, the property and its type, the part asked for, delete and pending. */
	if (length != 28U)
		return X11_BAD_LENGTH;
	index = randr_output_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_OUTPUT;
	valid = x11_atom_valid(server, x11_read32(request + 8, client->order));
	if (!valid)
		return X11_BAD_ATOM;
	if (request[24] > 1U || request[25] > 1U)
		return X11_BAD_VALUE;

	/* Succeeded: format 0, type None, nothing after and no items. */
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * CreateMode, DestroyMode, AddOutputMode and DeleteOutputMode: the modes
 * are the system's, so a mode is neither made nor given to an output.
 */
static unsigned
randr_mode_change(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	unsigned index;
	uint32_t mode;
	int known;

	/* CreateMode: a window, the mode's timings and its name. */
	if (request[1] == RANDR_CREATE_MODE) {
		if (length < 40U)
			return X11_BAD_LENGTH;
		known = randr_window_known(server, client, request);
		if (!known)
			return X11_BAD_WINDOW;
		return X11_BAD_ACCESS;
	}

	/* DestroyMode: a mode. */
	if (request[1] == RANDR_DESTROY_MODE) {
		if (length != 8U)
			return X11_BAD_LENGTH;
		index = randr_mode_of(server, x11_read32(request + 4, client->order));
		if (index == X11_NO_MONITOR)
			return RANDR_BAD_MODE;
		return X11_BAD_ACCESS;
	}

	/* AddOutputMode and DeleteOutputMode: an output and a mode. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	index = randr_output_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_OUTPUT;
	mode = x11_read32(request + 8, client->order);
	index = randr_mode_of(server, mode);
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_MODE;

	/* Succeeded in refusing: the system's mode stays with its output. */
	return X11_BAD_ACCESS;
}

/*
 * GetCrtcInfo: a monitor's CRTC, its place and size on the root window,
 * its mode, unrotated, showing its one output.
 */
static unsigned
randr_get_crtc_info(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct x11_monitor *monitor;
	uint8_t reply[32U + 8U];
	unsigned index;

	/* The CRTC, and the layout's time the client read it at. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	index = randr_crtc_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_CRTC;
	monitor = &server->monitors[index];

	/* The time, the place and size, the mode, the rotation and the rotations there are, and one output each way. */
	memset(reply, 0, sizeof(reply));
	reply[1] = RANDR_SET_SUCCESS;
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_write16(reply + 12, (uint16_t)monitor->x, client->order);
	x11_write16(reply + 14, (uint16_t)monitor->y, client->order);
	x11_write16(reply + 16, (uint16_t)monitor->width, client->order);
	x11_write16(reply + 18, (uint16_t)monitor->height, client->order);
	x11_write32(reply + 20, server->monitor_modes[index], client->order);
	x11_write16(reply + 24, RANDR_ROTATE_0, client->order);
	x11_write16(reply + 26, RANDR_ROTATE_0, client->order);
	x11_write16(reply + 28, 1U, client->order);
	x11_write16(reply + 30, 1U, client->order);

	/* Succeeded: the output it shows, and the one it may show. */
	x11_write32(reply + 32, X11_RANDR_OUTPUT_XID + index, client->order);
	x11_write32(reply + 36, X11_RANDR_OUTPUT_XID + index, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetCrtcConfig: the CRTC as it is (its place, mode, no rotation and its
 * output) succeeds; any other configuration fails (the system sets it).
 */
static unsigned
randr_set_crtc_config(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct x11_monitor *monitor;
	uint8_t reply[32];
	uint32_t config_time;
	uint32_t mode;
	uint32_t output;
	uint32_t first_output;
	size_t outputs;
	size_t at;
	unsigned index;
	unsigned found;
	unsigned status;
	uint16_t rotation;
	int16_t x;
	int16_t y;

	/* The CRTC, the times, the place, the mode, the rotation and the outputs. */
	if (length < 28U || (length % 4U) != 0U)
		return X11_BAD_LENGTH;
	index = randr_crtc_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_CRTC;
	monitor = &server->monitors[index];
	config_time = x11_read32(request + 12, client->order);
	x = (int16_t)x11_read16(request + 16, client->order);
	y = (int16_t)x11_read16(request + 18, client->order);
	mode = x11_read32(request + 20, client->order);
	rotation = x11_read16(request + 24, client->order);
	outputs = (length - 28U) / 4U;

	/* A mode there is (or None). */
	if (mode != 0U) {
		found = randr_mode_of(server, mode);
		if (found == X11_NO_MONITOR)
			return RANDR_BAD_MODE;
	}

	/* One rotation. */
	if (rotation == 0U || (rotation & (uint16_t)(rotation - 1U)) != 0U)
		return X11_BAD_VALUE;

	/* Outputs there are. */
	first_output = 0U;
	for (at = 0U; at < outputs; at++) {
		output = x11_read32(request + 28U + at * 4U, client->order);
		found = randr_output_of(server, output);
		if (found == X11_NO_MONITOR)
			return RANDR_BAD_OUTPUT;
		if (at == 0U)
			first_output = output;
	}

	/* The CRTC as it stands succeeds, a client's old view is told, anything else fails. */
	status = RANDR_SET_FAILED;
	if (mode == server->monitor_modes[index] &&
	    x == monitor->x &&
	    y == monitor->y &&
	    rotation == RANDR_ROTATE_0 &&
	    outputs == 1U &&
	    first_output == X11_RANDR_OUTPUT_XID + index)
		status = RANDR_SET_SUCCESS;
	if (config_time != server->monitor_config_time)
		status = RANDR_SET_INVALID_CONFIG_TIME;

	/* Succeeded: the status and the time. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)status;
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * The CRTCs' gamma: none to read or set (size 0), as for a virtual screen.
 * SetCrtcGamma of any other size than 0 is refused.
 */
static unsigned
randr_crtc_gamma(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	unsigned index;
	uint16_t size;

	/* A CRTC. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	index = randr_crtc_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_CRTC;

	/* SetCrtcGamma: the size and its three ramps, which must be none. */
	if (request[1] == RANDR_SET_CRTC_GAMMA) {
		if (length < 12U)
			return X11_BAD_LENGTH;
		size = x11_read16(request + 8, client->order);
		if (size != 0U)
			return X11_BAD_VALUE;
		if (length != 12U)
			return X11_BAD_LENGTH;
		return 0U;
	}

	/* Succeeded: GetCrtcGammaSize and GetCrtcGamma, size 0 and no ramps. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * The CRTCs' transforms: GetCrtcTransform answers the identity, pending
 * and current, without filters, and that the CRTC takes no transform;
 * SetCrtcTransform is refused.
 */
static unsigned
randr_get_crtc_transform(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[96];
	unsigned index;

	/* A CRTC. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	index = randr_crtc_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_CRTC;

	/* SetCrtcTransform: the CRTC takes none. */
	if (request[1] == RANDR_SET_CRTC_TRANSFORM) {
		if (length < 48U)
			return X11_BAD_LENGTH;
		return X11_BAD_MATCH;
	}

	/* The pending transform, the identity: 1 on the diagonal. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, RANDR_FIXED_ONE, client->order);
	x11_write32(reply + 8 + 16, RANDR_FIXED_ONE, client->order);
	x11_write32(reply + 8 + 32, RANDR_FIXED_ONE, client->order);

	/* Succeeded: no transforms taken, and the current transform the same. */
	reply[44] = 0U;
	x11_write32(reply + 48, RANDR_FIXED_ONE, client->order);
	x11_write32(reply + 48 + 16, RANDR_FIXED_ONE, client->order);
	x11_write32(reply + 48 + 32, RANDR_FIXED_ONE, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * The CRTCs' panning: GetPanning answers none (all zero), SetPanning
 * fails.
 */
static unsigned
randr_panning(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[36];
	unsigned index;

	/* A CRTC. */
	if (length < 8U)
		return X11_BAD_LENGTH;
	index = randr_crtc_of(server, x11_read32(request + 4, client->order));
	if (index == X11_NO_MONITOR)
		return RANDR_BAD_CRTC;

	/* SetPanning: the time, the area, the tracking area and the borders; it cannot be done. */
	if (request[1] == RANDR_SET_PANNING) {
		if (length != 36U)
			return X11_BAD_LENGTH;
		memset(reply, 0, sizeof(reply));
		reply[1] = RANDR_SET_FAILED;
		x11_write32(reply + 8, server->monitor_set_time, client->order);
		x11_reply(server, client, reply, 32U);
		return 0U;
	}

	/* Succeeded: GetPanning, no panning. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	memset(reply, 0, sizeof(reply));
	reply[1] = RANDR_SET_SUCCESS;
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetOutputPrimary: a client makes an output the main one (None: none);
 * the clients that asked hear the outputs that changed.
 */
static unsigned
randr_set_output_primary(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct x11_monitor old[X11_MONITORS_MAX];
	uint32_t output;
	unsigned index;
	unsigned at;
	int known;

	/* A window and an output (or None). */
	if (length != 12U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;
	output = x11_read32(request + 8, client->order);
	index = X11_NO_MONITOR;
	if (output != 0U) {
		index = randr_output_of(server, output);
		if (index == X11_NO_MONITOR)
			return RANDR_BAD_OUTPUT;
	}

	/* The main one already: nothing changes. */
	if (index == server->monitor_primary)
		return 0U;

	/* The new main one, marked on the monitors. */
	memcpy(old, server->monitors, sizeof(old));
	server->monitor_primary = index;
	for (at = 0U; at < server->monitor_count; at++) {
		server->monitors[at].primary = 0;
		if (at == index)
			server->monitors[at].primary = 1;
	}

	/* Succeeded: the layout's set time moves, and the clients that asked are told. */
	server->monitor_set_time = server->now;
	randr_tell(server, old, server->monitor_count, 0);
	return 0U;
}

/* GetOutputPrimary: the main output, or None. */
static unsigned
randr_get_output_primary(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	int known;

	/* A window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* Succeeded: the main monitor's output, or None. */
	memset(reply, 0, sizeof(reply));
	if (server->monitor_primary != X11_NO_MONITOR)
		x11_write32(reply + 8, X11_RANDR_OUTPUT_XID + server->monitor_primary, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetProviders: the time, and no providers (one device draws every monitor). */
static unsigned
randr_get_providers(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	int known;

	/* A window. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* Succeeded: the time and none. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetMonitors: each monitor, named after its output, with its place and
 * size, its physical size and its one output, the main one (if any)
 * marked.
 */
static unsigned
randr_get_monitors(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	const struct x11_monitor *monitor;
	uint8_t reply[32U + X11_MONITORS_MAX * (RANDR_MONITOR_INFO_SIZE + 4U)];
	uint8_t *info;
	uint32_t name;
	size_t used;
	unsigned index;
	int known;

	/* A window, and whether only the active monitors are asked for (all of them are). */
	if (length != 12U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* The time, and how many monitors and outputs. */
	memset(reply, 0, sizeof(reply));
	x11_write32(reply + 8, server->monitor_set_time, client->order);
	x11_write32(reply + 12, server->monitor_count, client->order);
	x11_write32(reply + 16, server->monitor_count, client->order);

	/* Each monitor and its output. */
	used = 32U;
	for (index = 0U; index < server->monitor_count; index++) {
		monitor = &server->monitors[index];
		name = x11_atom_intern(server, monitor->name, strlen(monitor->name), 0);
		info = reply + used;
		x11_write32(info, name, client->order);
		if (index == server->monitor_primary)
			info[4] = 1U;
		info[5] = 1U;
		x11_write16(info + 6, 1U, client->order);
		x11_write16(info + 8, (uint16_t)monitor->x, client->order);
		x11_write16(info + 10, (uint16_t)monitor->y, client->order);
		x11_write16(info + 12, (uint16_t)monitor->width, client->order);
		x11_write16(info + 14, (uint16_t)monitor->height, client->order);
		x11_write32(info + 16, monitor->width_mm, client->order);
		x11_write32(info + 20, monitor->height_mm, client->order);
		x11_write32(info + 24, X11_RANDR_OUTPUT_XID + index, client->order);
		used += RANDR_MONITOR_INFO_SIZE + 4U;
	}

	/* Succeeded: the monitors. */
	x11_reply(server, client, reply, used);
	return 0U;
}

/*
 * SetMonitor and DeleteMonitor: the monitors are the system's, so a
 * client neither defines one nor takes one away.
 */
static unsigned
randr_set_monitor(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	int known;

	/* A window, and a monitor's description or name. */
	if (length < 12U)
		return X11_BAD_LENGTH;
	known = randr_window_known(server, client, request);
	if (!known)
		return X11_BAD_WINDOW;

	/* Succeeded in refusing. */
	return X11_BAD_ACCESS;
}

/* Tells whether the window a request names first (at its fifth byte) is one there is. */
static int
randr_window_known(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request)
{
	struct x11_window *window;

	/* The window by its id. */
	window = x11_window_find(server, x11_read32(request + 4, client->order));
	if (window == NULL)
		return 0;

	/* Succeeded: there. */
	return 1;
}

/* Returns the monitor of a CRTC's id, or X11_NO_MONITOR. */
static unsigned
randr_crtc_of(
	struct x11server *server,
	uint32_t id)
{
	/* An id below the CRTCs' or past the last monitor's. */
	if (id < X11_RANDR_CRTC_XID)
		return X11_NO_MONITOR;
	if (id - X11_RANDR_CRTC_XID >= server->monitor_count)
		return X11_NO_MONITOR;

	/* Succeeded: its monitor. */
	return id - X11_RANDR_CRTC_XID;
}

/* Returns the monitor of an output's id, or X11_NO_MONITOR. */
static unsigned
randr_output_of(
	struct x11server *server,
	uint32_t id)
{
	/* An id below the outputs' or past the last monitor's. */
	if (id < X11_RANDR_OUTPUT_XID)
		return X11_NO_MONITOR;
	if (id - X11_RANDR_OUTPUT_XID >= server->monitor_count)
		return X11_NO_MONITOR;

	/* Succeeded: its monitor. */
	return id - X11_RANDR_OUTPUT_XID;
}

/* Returns the monitor whose mode an id is, or X11_NO_MONITOR. */
static unsigned
randr_mode_of(
	struct x11server *server,
	uint32_t id)
{
	unsigned index;

	/* Each monitor's mode. */
	for (index = 0U; index < server->monitor_count; index++) {
		if (server->monitor_modes[index] == id)
			return index;
	}

	/* None has it. */
	return X11_NO_MONITOR;
}

/* Returns the screen's refresh rate (versions 1.0 and 1.1): the main monitor's, or the first's. */
static unsigned
randr_screen_rate(
	struct x11server *server)
{
	unsigned monitor;
	unsigned rate;

	/* The main monitor, or the first. */
	monitor = server->monitor_primary;
	if (monitor == X11_NO_MONITOR)
		monitor = 0U;

	/* Succeeded: its rate. */
	rate = randr_refresh(&server->monitors[monitor]);
	return rate;
}

/* Gives a length in pixels in millimetres at 96 pixels an inch (as the setup counts the screen's). */
static unsigned
randr_millimetres(
	unsigned pixels)
{
	/* Succeeded: 25.4 millimetres an inch. */
	return pixels * 254U / 96U / 10U;
}

/* Gives a monitor's refresh rate in hertz. */
static unsigned
randr_refresh(
	const struct x11_monitor *monitor)
{
	/* None given: the default. */
	if (monitor->refresh == 0U)
		return RANDR_REFRESH_DEFAULT;

	/* Succeeded: the monitor's. */
	return monitor->refresh;
}

/*
 * Writes a monitor's mode (32 bytes): its id and size, and timings with no
 * blanking whose dot clock gives the monitor's refresh rate.
 */
static void
randr_mode_info(
	struct x11server *server,
	unsigned monitor,
	uint8_t *info,
	int msb)
{
	const struct x11_monitor *shown;
	char name[X11_MONITOR_NAME_MAX];
	uint64_t clock;
	size_t name_length;

	/* The dot clock of the size at the rate, as large as it may be. */
	shown = &server->monitors[monitor];
	clock = (uint64_t)shown->width * (uint64_t)shown->height * (uint64_t)randr_refresh(shown);
	if (clock > 0xffffffffU)
		clock = 0xffffffffU;
	name_length = randr_mode_name(server, monitor, name, sizeof(name));

	/* The id, the size and the dot clock. */
	memset(info, 0, RANDR_MODE_INFO_SIZE);
	x11_write32(info, server->monitor_modes[monitor], msb);
	x11_write16(info + 4, (uint16_t)shown->width, msb);
	x11_write16(info + 6, (uint16_t)shown->height, msb);
	x11_write32(info + 8, (uint32_t)clock, msb);

	/* The horizontal timings: sync at the edge, the total the width. */
	x11_write16(info + 12, (uint16_t)shown->width, msb);
	x11_write16(info + 14, (uint16_t)shown->width, msb);
	x11_write16(info + 16, (uint16_t)shown->width, msb);

	/* The vertical timings: sync at the edge, the total the height. */
	x11_write16(info + 20, (uint16_t)shown->height, msb);
	x11_write16(info + 22, (uint16_t)shown->height, msb);
	x11_write16(info + 24, (uint16_t)shown->height, msb);

	/* The name's length and the flags. */
	x11_write16(info + 26, (uint16_t)name_length, msb);
	x11_write32(info + 28, RANDR_MODE_FLAGS, msb);
}

/* Writes a monitor's mode's name ("WIDTHxHEIGHT") and returns its length. */
static size_t
randr_mode_name(
	struct x11server *server,
	unsigned monitor,
	char *name,
	size_t size)
{
	int written;

	/* The size as text. */
	written = snprintf(name, size, "%ux%u", server->monitors[monitor].width, server->monitors[monitor].height);
	if (written < 0)
		return 0U;

	/* Succeeded: its length (it always fits). */
	return (size_t)written;
}

/*
 * Takes the monitors as the backend gave them, each within X's sizes and
 * places, and brings the root window's size (*width, *height) within them
 * too: count 0 (or no monitors) is one monitor, the whole root window.
 * The main one is the first the backend marked, or none.
 */
static void
randr_monitors_take(
	struct x11server *server,
	unsigned *width,
	unsigned *height,
	const struct x11_monitor *monitors,
	unsigned count)
{
	unsigned limit;
	unsigned index;

	/* The root window's size: at least a pixel each way, at most X's largest (a rooted server's pixels' largest). */
	limit = RANDR_SIDE_MAX;
	if (server->mode == X11SERVER_ROOTED)
		limit = X11_IMAGE_SIDE_MAX;
	if (*width == 0U)
		*width = 1U;
	if (*width > limit)
		*width = limit;
	if (*height == 0U)
		*height = 1U;
	if (*height > limit)
		*height = limit;

	/* The monitors, as many as are kept. */
	if (monitors == NULL)
		count = 0U;
	if (count > X11_MONITORS_MAX)
		count = X11_MONITORS_MAX;
	memset(server->monitors, 0, sizeof(server->monitors));
	if (count == 0U) {
		server->monitors[0].width = *width;
		server->monitors[0].height = *height;
		(void)snprintf(server->monitors[0].name, sizeof(server->monitors[0].name), "%s", RANDR_DEFAULT_NAME);
		count = 1U;
	} else {
		memcpy(server->monitors, monitors, count * sizeof(*monitors));
	}
	server->monitor_count = count;

	/* Each one cleaned and named, the first marked the main one. */
	server->monitor_primary = X11_NO_MONITOR;
	for (index = 0U; index < count; index++) {
		randr_monitor_clean(&server->monitors[index]);
		if (server->monitors[index].name[0] == '\0') {
			(void)snprintf(server->monitors[index].name, sizeof(server->monitors[index].name), "%s-%u",
			    RANDR_DEFAULT_NAME, index);
		}
		if (server->monitors[index].primary == 0)
			continue;
		server->monitors[index].primary = 0;
		if (server->monitor_primary == X11_NO_MONITOR) {
			server->monitor_primary = index;
			server->monitors[index].primary = 1;
		}
	}
}

/*
 * Brings a monitor within X's sizes and places: a size of at least a
 * pixel, a place in 16 bits, a physical size (at 96 pixels an inch when
 * none was given), a rate, and a name of printable ASCII ended by a NUL.
 */
static void
randr_monitor_clean(
	struct x11_monitor *monitor)
{
	size_t index;
	char letter;

	/* The size. */
	if (monitor->width == 0U)
		monitor->width = 1U;
	if (monitor->width > RANDR_SIDE_MAX)
		monitor->width = RANDR_SIDE_MAX;
	if (monitor->height == 0U)
		monitor->height = 1U;
	if (monitor->height > RANDR_SIDE_MAX)
		monitor->height = RANDR_SIDE_MAX;

	/* The place. */
	if (monitor->x < RANDR_PLACE_MIN)
		monitor->x = RANDR_PLACE_MIN;
	if (monitor->x > RANDR_PLACE_MAX)
		monitor->x = RANDR_PLACE_MAX;
	if (monitor->y < RANDR_PLACE_MIN)
		monitor->y = RANDR_PLACE_MIN;
	if (monitor->y > RANDR_PLACE_MAX)
		monitor->y = RANDR_PLACE_MAX;

	/* The physical size and the rate. */
	if (monitor->width_mm == 0U)
		monitor->width_mm = randr_millimetres(monitor->width);
	if (monitor->height_mm == 0U)
		monitor->height_mm = randr_millimetres(monitor->height);
	if (monitor->refresh > RANDR_REFRESH_MAX)
		monitor->refresh = RANDR_REFRESH_MAX;

	/* The name: ended, and each letter printable (others become '-'). */
	monitor->name[X11_MONITOR_NAME_MAX - 1U] = '\0';
	for (index = 0U; monitor->name[index] != '\0'; index++) {
		letter = monitor->name[index];
		if (letter < '!' || letter > '~')
			monitor->name[index] = '-';
	}
}

/* Gives the id of the next mode, going round from the first after the last. */
static uint32_t
randr_mode_next(
	struct x11server *server)
{
	uint32_t mode;

	/* The next id, and the one after it. */
	mode = server->monitor_mode_serial;
	server->monitor_mode_serial++;
	if (server->monitor_mode_serial > RANDR_MODE_LAST)
		server->monitor_mode_serial = RANDR_MODE_FIRST;

	/* Succeeded: the id. */
	return mode;
}

/*
 * Gives the root window a new size: a rooted server's pixels follow (what
 * is still there kept, the rest painted and exposed), the pointer stays
 * on it, and the root window's clients hear ConfigureNotify.
 */
static void
randr_root_resize(
	struct x11server *server,
	unsigned width,
	unsigned height)
{
	struct x11_window *root;
	uint8_t event[32];

	/* The size, with a rooted server's pixels following it. */
	root = server->root;
	if (server->mode == X11SERVER_ROOTED)
		x11_draw_begin(server, root, root);
	server->width = width;
	server->height = height;
	root->width = (uint16_t)width;
	root->height = (uint16_t)height;
	if (server->mode == X11SERVER_ROOTED) {
		x11_draw_end(server);
		x11_mark_dirty(server, 0, 0, (int)width, (int)height);
	}

	/* The pointer on the root window still. */
	if (server->pointer_x >= (int)width)
		server->pointer_x = (int)width - 1;
	if (server->pointer_y >= (int)height)
		server->pointer_y = (int)height - 1;

	/* Succeeded: ConfigureNotify, the root window's place, size and no border. */
	memset(event, 0, sizeof(event));
	event[0] = X11_EVENT_CONFIGURE_NOTIFY;
	x11_write32(event + 8, root->id, 0);
	x11_write16(event + 20, (uint16_t)width, 0);
	x11_write16(event + 22, (uint16_t)height, 0);
	x11_event_structure(server, root, event);
}

/*
 * Tells the clients that asked what changed from an old layout: the
 * screen, each CRTC and output that is new or changed, and (when
 * monitors came or went) the resources.
 */
static void
randr_tell(
	struct x11server *server,
	const struct x11_monitor *old,
	unsigned old_count,
	int resources)
{
	struct x11_randr *randr;
	struct x11_client *client;
	struct randr_select select;
	unsigned at;
	unsigned index;
	int same;

	/* Nobody asked. */
	randr = server->randr;
	if (randr == NULL)
		return;

	/* Each selection, its client told what it asked for. */
	for (at = 0U; at < randr->count; at++) {
		select = randr->selects[at];
		client = x11_client_of(server, select.client);
		if (client == NULL)
			continue;
		if ((select.mask & RANDR_SCREEN_CHANGE_MASK) != 0U)
			randr_tell_screen(server, client, select.window);
		for (index = 0U; index < server->monitor_count; index++) {
			same = 0;
			if (index < old_count)
				same = randr_monitor_same(&old[index], &server->monitors[index]);
			if (same)
				continue;
			if ((select.mask & RANDR_CRTC_CHANGE_MASK) != 0U)
				randr_tell_crtc(server, client, select.window, index);
			if ((select.mask & RANDR_OUTPUT_CHANGE_MASK) != 0U)
				randr_tell_output(server, client, select.window, index);
		}
		if (resources && (select.mask & RANDR_RESOURCE_CHANGE_MASK) != 0U)
			randr_tell_resources(server, client, select.window);
	}
}

/* Sends a client RRScreenChangeNotify: the root window's size and the layout's times. */
static void
randr_tell_screen(
	struct x11server *server,
	struct x11_client *client,
	uint32_t window)
{
	uint8_t event[32];
	int msb;

	/* The rotation, the times, the root window and the window selected on, the size and the sub-pixel order. */
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = (uint8_t)X11_RANDR_EVENT;
	event[1] = RANDR_ROTATE_0;
	x11_write32(event + 4, server->monitor_set_time, msb);
	x11_write32(event + 8, server->monitor_config_time, msb);
	x11_write32(event + 12, X11_ROOT_XID, msb);
	x11_write32(event + 16, window, msb);
	x11_write16(event + 20, 0U, msb);
	x11_write16(event + 22, RANDR_SUBPIXEL_UNKNOWN, msb);
	x11_write16(event + 24, (uint16_t)server->width, msb);
	x11_write16(event + 26, (uint16_t)server->height, msb);
	x11_write16(event + 28, (uint16_t)randr_millimetres(server->width), msb);
	x11_write16(event + 30, (uint16_t)randr_millimetres(server->height), msb);

	/* Succeeded: sent. */
	randr_event_send(server, client, event);
}

/* Sends a client RRNotify of a monitor's CRTC: its mode, place and size. */
static void
randr_tell_crtc(
	struct x11server *server,
	struct x11_client *client,
	uint32_t window,
	unsigned monitor)
{
	const struct x11_monitor *shown;
	uint8_t event[32];
	int msb;

	/* The time, the window, the CRTC, its mode and rotation, its place and size. */
	shown = &server->monitors[monitor];
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = (uint8_t)(X11_RANDR_EVENT + 1U);
	event[1] = RANDR_NOTIFY_CRTC;
	x11_write32(event + 4, server->monitor_set_time, msb);
	x11_write32(event + 8, window, msb);
	x11_write32(event + 12, X11_RANDR_CRTC_XID + monitor, msb);
	x11_write32(event + 16, server->monitor_modes[monitor], msb);
	x11_write16(event + 20, RANDR_ROTATE_0, msb);
	x11_write16(event + 24, (uint16_t)shown->x, msb);
	x11_write16(event + 26, (uint16_t)shown->y, msb);
	x11_write16(event + 28, (uint16_t)shown->width, msb);
	x11_write16(event + 30, (uint16_t)shown->height, msb);

	/* Succeeded: sent. */
	randr_event_send(server, client, event);
}

/* Sends a client RRNotify of a monitor's output: its CRTC, mode and connection. */
static void
randr_tell_output(
	struct x11server *server,
	struct x11_client *client,
	uint32_t window,
	unsigned monitor)
{
	uint8_t event[32];
	int msb;

	/* The times, the window, the output, its CRTC and mode, rotation, connection and sub-pixel order. */
	msb = client->order;
	memset(event, 0, sizeof(event));
	event[0] = (uint8_t)(X11_RANDR_EVENT + 1U);
	event[1] = RANDR_NOTIFY_OUTPUT;
	x11_write32(event + 4, server->monitor_set_time, msb);
	x11_write32(event + 8, server->monitor_config_time, msb);
	x11_write32(event + 12, window, msb);
	x11_write32(event + 16, X11_RANDR_OUTPUT_XID + monitor, msb);
	x11_write32(event + 20, X11_RANDR_CRTC_XID + monitor, msb);
	x11_write32(event + 24, server->monitor_modes[monitor], msb);
	x11_write16(event + 28, RANDR_ROTATE_0, msb);
	event[30] = RANDR_CONNECTED;
	event[31] = RANDR_SUBPIXEL_UNKNOWN;

	/* Succeeded: sent. */
	randr_event_send(server, client, event);
}

/* Sends a client RRNotify that monitors (CRTCs and outputs) came or went. */
static void
randr_tell_resources(
	struct x11server *server,
	struct x11_client *client,
	uint32_t window)
{
	uint8_t event[32];

	/* The time and the window. */
	memset(event, 0, sizeof(event));
	event[0] = (uint8_t)(X11_RANDR_EVENT + 1U);
	event[1] = RANDR_NOTIFY_RESOURCE;
	x11_write32(event + 4, server->monitor_set_time, client->order);
	x11_write32(event + 8, window, client->order);

	/* Succeeded: sent. */
	randr_event_send(server, client, event);
}

/* Sends one of RANDR's events (already in the client's order) with the client's last sequence number. */
static void
randr_event_send(
	struct x11server *server,
	struct x11_client *client,
	uint8_t *event)
{
	/* The sequence number of the last request the client sent. */
	x11_write16(event + 2, client->sequence, client->order);

	/* Succeeded: queued. */
	x11_client_send(server, client, event, 32U);
}

/* Tells whether two monitors are shown alike (place, size, physical size, rate, main or not, name). */
static int
randr_monitor_same(
	const struct x11_monitor *one,
	const struct x11_monitor *other)
{
	int differs;

	/* The place and the size. */
	if (one->x != other->x || one->y != other->y)
		return 0;
	if (one->width != other->width || one->height != other->height)
		return 0;

	/* The physical size, the rate and whether it is the main one. */
	if (one->width_mm != other->width_mm || one->height_mm != other->height_mm)
		return 0;
	if (one->refresh != other->refresh)
		return 0;
	if (one->primary != other->primary)
		return 0;

	/* The name. */
	differs = strcmp(one->name, other->name);
	if (differs != 0)
		return 0;

	/* Succeeded: alike. */
	return 1;
}

/* Tells whether two monitors show the same mode (size and rate). */
static int
randr_mode_same(
	const struct x11_monitor *one,
	const struct x11_monitor *other)
{
	unsigned one_rate;
	unsigned other_rate;

	/* The size. */
	if (one->width != other->width || one->height != other->height)
		return 0;

	/* The rate. */
	one_rate = randr_refresh(one);
	other_rate = randr_refresh(other);
	if (one_rate != other_rate)
		return 0;

	/* Succeeded: the same mode. */
	return 1;
}
