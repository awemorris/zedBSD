/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop surface (kl_desktop_v1, ws094-p002, plan/ws094/design.md
 * §3): one client's surface that lies over the wallpaper and under every
 * window, on every virtual desktop, where Files shows the icons of
 * ~/Desktop.
 *
 * kl_desktop_manager_v1.get_desktop_surface(id, surface, token) gives
 * a surface the role, once, to the client that shows the token the
 * compositor gave the program it started (KEILAND_DESKTOP_TOKEN); the new
 * kl_desktop_surface_v1 hears configure(serial, x, y, width, height):
 * where on the output it is (the work area under the system bar), and the
 * client acknowledges it before it draws.  The surface has no window: it
 * is not in the windows' list, Wiseview, the system bar or the focus
 * cycle.  Its image is laid over the wallpaper by its alpha, moves with
 * the desktop layer while App Home opens, and is under the windows' glass.
 *
 * A press where no window is (and none of the compositor's own places)
 * goes to the desktop surface, which then has the keyboard until a window
 * is pressed, raised or mapped.  A finger does the same through the
 * pointer's press, and a drag and drop over no window is the desktop's.
 *
 * A login session starts the desktop program (/bin/files --desktop, ws094-p003)
 * with a new token unless /etc/keiland/desktop says "off", and so does any
 * compositor that --desktop-client names a program for; it is started
 * again two seconds after it ends, at most four times a minute.
 *
 * ws164-p002: a login session whose account has not taken the Welcome
 * (the setting welcome.done is 0) starts Settings' Welcome once, after the
 * desktop program (settings --welcome; it is not started again when it
 * ends, and the login screen and --testing never start it).
 *
 * The installer's image (the one with the installer's index) starts the
 * installer in its place, once a session, and no Welcome: the image is
 * for installing, and the installed system has its own Welcome.
 */

#include "desktop.h"
#include "extras.h"
#include "menu.h"
#include "popup.h"

#include "userland/desktop/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Marks a parameter a function's signature requires but it does not use. */
#define UNUSED_PARAMETER(name)	((void)(name))

/* The requests of kl_desktop_manager_v1 and of kl_desktop_surface_v1, and the surface's one event. */
#define DESKTOP_MANAGER_DESTROY		0U
#define DESKTOP_MANAGER_GET		1U
#define DESKTOP_SURFACE_DESTROY		0U
#define DESKTOP_SURFACE_ACK		1U
#define DESKTOP_SURFACE_DRAWN		2U
#define DESKTOP_SURFACE_CONFIGURE	0U

/* The most rectangles set_drawn tells (version 2, ws270-p007); a request of more is kept as the box around them. */
#define DESKTOP_PARTS_MAX		32U

/* The manager's errors: the role taken or the surface not free for it; a token that is not the one given. */
#define DESKTOP_ERROR_ROLE		0U
#define DESKTOP_ERROR_TOKEN		1U

/* The program a login session starts, and the file that turns it off. */
#define DESKTOP_COMMAND			KEILAND_BINDIR "/files --desktop"

/* Settings' Welcome, started once at a session's start while welcome.done is 0 (ws164-p002). */
#define DESKTOP_WELCOME			KEILAND_BINDIR "/settings --welcome"
#define DESKTOP_SWITCH			KEILAND_SYSCONFDIR "/keiland/desktop"

/* The installer, and the index only the installer's image has. */
#define DESKTOP_INSTALLER		KEILAND_BINDIR "/installer"
#define DESKTOP_INSTALLER_INDEX		"/opt/installer/index"

/* How long after it ends the program is started again, and how many starts a minute are allowed. */
#define DESKTOP_RESTART_MS		2000U
#define DESKTOP_STARTS			4U
#define DESKTOP_STARTS_MS		60000U

/* The token's random bytes (written in hex, twice as many characters), and the longest token taken. */
#define DESKTOP_TOKEN_BYTES		16U
#define DESKTOP_TOKEN_MAX		64U

/* The longest command line the desktop program is started with. */
#define DESKTOP_LINE_MAX		512U

/*
 * The one desktop surface of the compositor and the program that shows it.
 *
 * surface and object are the surface with the role and its
 * kl_desktop_surface_v1 (both NULL when no client has the role); they
 * are cleared when either goes (kwl_desktop_object_gone).  x, y, width and
 * height are the place last configured, serial the configure's.  focused
 * says the desktop has the keyboard, since focus_top was the top window
 * (another top window takes it back).  command is the program to start
 * (NULL: none), token the token it is given (fixed says --desktop-token
 * chose it), pid the program running (0: none), starting that its start
 * is asked of the file I/O thread and its process ID has not come back
 * (spawn.c), gone_ms when it ended,
 * starts and starts_ms how many starts the minute since starts_ms has had.
 * drawn says the program's first image was drawn since its start (logged
 * once with its time, ws094-p009).
 */
struct desktop_state {
	struct kwl_object *surface;
	struct kwl_object *object;
	uint32_t serial;
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int focused;
	struct kwl_object *focus_top;
	const char *command;
	char token[DESKTOP_TOKEN_MAX + 1U];
	int fixed;
	int decided;
	pid_t pid;
	int starting;
	uint64_t gone_ms;
	unsigned starts;
	uint64_t starts_ms;
	int limited;
	int drawn;
	int welcome_decided;
	/*
	 * The parts of the surface its program draws anything in (set_drawn,
	 * version 2, ws270-p007), in the surface's pixels (x, y, width,
	 * height): told, then applied with the surface's commit.  parts_known
	 * is 0 until the first is applied (all of the surface is drawn, as for
	 * a program of version 1); none known after it means nothing is drawn.
	 */
	int32_t pending_parts[DESKTOP_PARTS_MAX][4];
	unsigned pending_part_count;
	unsigned pending_parts_set;
	int32_t parts[DESKTOP_PARTS_MAX][4];
	unsigned part_count;
	unsigned parts_known;
};

/*
 * The desktop surface and its program, for the compositor's life (one
 * compositor has one output and one desktop).  It starts empty; the
 * command-line options and the first display pass fill it.
 */
static struct desktop_state desk;

static int desktop_get(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int desktop_set_drawn(const unsigned char *bytes, size_t size);
static int desktop_string(const unsigned char *bytes, size_t size, size_t offset, const char **text);
static void desktop_place(struct kwl_server *server, int32_t *x, int32_t *y, int32_t *width, int32_t *height);
static int desktop_configure(struct kwl_server *server);
static void desktop_start(struct kwl_server *server);
static void desktop_watch(struct kwl_server *server);
static void desktop_welcome(struct kwl_server *server);
static void desktop_started(struct kwl_server *server, pid_t pid, int error, void *context);
static void desktop_installer_started(struct kwl_server *server, pid_t pid, int error, void *context);
static void desktop_welcome_started(struct kwl_server *server, pid_t pid, int error, void *context);
static int desktop_switched_off(void);
static void desktop_new_token(void);
static uint32_t desktop_word(const unsigned char *bytes, size_t offset);

/*
 * Takes a command-line option of the desktop: --desktop-client=COMMAND
 * (the program to start, also outside a login session; "none" for no
 * program) or --desktop-token=TOKEN (a fixed token, for tests that start
 * their own client).  Returns 1 when the argument was one, 0 when it was
 * another option, EINVAL for a bad value.
 */
int
kwl_desktop_option(
	struct kwl_server *server,
	const char *argument)
{
	size_t length;
	int match;

	UNUSED_PARAMETER(server);

	/* The program to start. */
	match = strncmp(argument, "--desktop-client=", 17);
	if (match == 0) {
		/* "none" starts no program, even in a login session. */
		desk.command = argument + 17;
		match = strcmp(desk.command, "none");
		if (match == 0)
			desk.command = "";
		desk.decided = 1;
		return 1;
	}

	/* Another option. */
	match = strncmp(argument, "--desktop-token=", 16);
	if (match != 0)
		return 0;

	/* A token that fits, not empty. */
	length = strlen(argument + 16);
	if (length == 0U || length > DESKTOP_TOKEN_MAX)
		return EINVAL;

	/* The fixed token. */
	snprintf(desk.token, sizeof(desk.token), "%s", argument + 16);
	desk.fixed = 1;

	/* Succeeded: the option was the desktop's. */
	return 1;
}

/*
 * Carries out a request of kl_desktop_manager_v1 or of
 * kl_desktop_surface_v1.
 */
int
kwl_desktop_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* The manager: it goes, or it gives a surface the role. */
	if (object->kind == KWL_DESKTOP_MANAGER) {
		/* destroy. */
		if (opcode == DESKTOP_MANAGER_DESTROY && size == 0U) {
			kwl_object_destroy(object);
			return 0;
		}

		/* Only get_desktop_surface is left. */
		if (opcode != DESKTOP_MANAGER_GET)
			return EPROTO;
		error = desktop_get(object, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the surface is the desktop's. */
		return 0;
	}

	/* The desktop surface goes; the surface shows nothing more. */
	if (opcode == DESKTOP_SURFACE_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* set_drawn (version 2): the parts the next commit draws anything in. */
	if (opcode == DESKTOP_SURFACE_DRAWN && object->version >= 2U) {
		error = desktop_set_drawn(bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* ack_configure: the configure the client drew for. */
	if (opcode != DESKTOP_SURFACE_ACK || size != 4U)
		return EPROTO;
	printf("KWL DESKTOP ack serial=%u\n", desktop_word(bytes, 0U));

	/* Succeeded: the acknowledgement is taken. */
	return 0;
}

/*
 * Unties an object that is going from the desktop: the surface with the
 * role or its kl_desktop_surface_v1 ends the role, and the keyboard
 * goes back to the windows.
 */
void
kwl_desktop_object_gone(
	struct kwl_object *object)
{
	struct kwl_server *server;

	/* Only the role's surface and its object end the role. */
	if (object != desk.surface && object != desk.object)
		return;

	/* The role ends; the windows have the keyboard again. */
	server = object->client->server;
	printf("KWL DESKTOP gone client=%llu\n", (unsigned long long)object->client->number);
	kwl_desktop_unfocus(server);
	desk.surface = NULL;
	desk.object = NULL;
	desk.parts_known = 0U;
	desk.pending_parts_set = 0U;
	desk.focus_top = NULL;
	server->dirty = 1;

	/* Succeeded: the role has ended. */
	return;
}

/*
 * Moves the desktop on with the display pass: starts (or starts again) its
 * program, tells the surface a new place when the output changed, and
 * answers its frame callbacks while it is not drawn.
 */
void
kwl_desktop_tick(
	struct kwl_server *server)
{
	int32_t x;
	int32_t y;
	int32_t width;
	int32_t height;
	int error;

	/* The program: started, watched, started again; the Welcome once after it (ws164-p002). */
	desktop_watch(server);
	desktop_start(server);
	desktop_welcome(server);

	/* Without the surface nothing else is to do. */
	if (desk.surface == NULL || desk.surface->dead)
		return;

	/* A new place (the output's size, the look) is configured again. */
	desktop_place(server, &x, &y, &width, &height);
	if (x != desk.x ||
	    y != desk.y ||
	    width != desk.width ||
	    height != desk.height) {
		error = desktop_configure(server);
		if (error != 0)
			printf("KWL DESKTOP configure-failed errno=%d\n", error);
	}

	/* A frame that does not draw the desktop (a fullscreen window, the lock screen) still answers its callbacks. */
	if (!server->windowed ||
	    server->locked ||
	    server->greeter)
		kwl_callbacks_done(&desk.surface->committed_callbacks);

	/* Succeeded: the desktop is in step with the pass. */
	return;
}

/*
 * Draws the desktop surface's image where it is, over the wallpaper and
 * under the windows, by its alpha; in the glass look it moves with the
 * desktop layer (App Home).
 */
void
kwl_desktop_draw(
	struct kwl_server *server,
	struct kwl_list *command)
{
	const struct kwl_import *image;
	struct kwl_import alpha;
	struct glass_shape shape;
	struct glass_shape piece;
	struct kwl_object *surface;
	float rect[4];
	float across;
	float down;
	uint32_t width;
	uint32_t height;
	unsigned index;
	int inside;

	/* Only a surface with an image. */
	surface = kwl_desktop_surface(server);
	if (surface == NULL)
		return;
	image = kwl_compose_surface_image(surface);
	if (image == NULL)
		return;

	/* The first image since the program started, with its time (the tests read it). */
	if (!desk.drawn) {
		desk.drawn = 1;
		printf("KWL DESKTOP drawn at_ms=%llu\n", (unsigned long long)kwl_milliseconds());
	}

	/* Its size (its viewport's, else its image's). */
	kwl_surface_size(surface, &width, &height);
	if (width == 0U || height == 0U) {
		width = image->width;
		height = image->height;
	}

	/* The plain look: a quad by its alpha. */
	if (!server->glass) {
		alpha = *image;
		alpha.draw = KWL_DRAW_ALPHA;
		kwl_compose_surface_quad(server, command, surface, &alpha, surface->x, surface->y);
		return;
	}

	/* The glass look: the image, by its alpha unless it is opaque, with the layer. */
	glass_shape_init(&shape, (float)surface->x, (float)surface->y, (float)width, (float)height);
	kwl_viewport_source(surface, shape.uv);
	shape.mode = MODE_IMAGE;
	shape.set = image->set;
	if (image->draw == KWL_DRAW_OPAQUE)
		shape.opaque = 1.0f;

	/* All of it, unless its program told the parts it draws in (ws270-p007). */
	if (!desk.parts_known) {
		glass_shape_draw(server, command, &shape);
		return;
	}

	/* Only those parts, from the image's pixels to the surface's place and size. */
	across = (float)width / (float)image->width;
	down = (float)height / (float)image->height;
	for (index = 0U; index < desk.part_count; index++) {
		rect[0] = (float)surface->x + (float)desk.parts[index][0] * across;
		rect[1] = (float)surface->y + (float)desk.parts[index][1] * down;
		rect[2] = (float)desk.parts[index][2] * across;
		rect[3] = (float)desk.parts[index][3] * down;
		inside = glass_shape_crop(&shape, rect, &piece);
		if (inside)
			glass_shape_draw(server, command, &piece);
	}

	/* Succeeded: the desktop is drawn. */
	return;
}

/*
 * Applies the parts its program told with a commit of the desktop's
 * surface (set_drawn, ws270-p007); a commit of any other surface, or with
 * nothing told since the last, changes nothing.
 */
void
kwl_desktop_commit(
	struct kwl_object *surface)
{
	/* Only the desktop's surface, after a set_drawn. */
	if (surface != desk.surface || !desk.pending_parts_set)
		return;

	/* The parts told become those drawn. */
	memcpy(desk.parts, desk.pending_parts, (size_t)desk.pending_part_count * sizeof(desk.parts[0]));
	desk.part_count = desk.pending_part_count;
	desk.parts_known = 1U;
	desk.pending_parts_set = 0U;
}

/*
 * Returns the desktop surface when it has an image to show, NULL
 * otherwise.
 */
struct kwl_object *
kwl_desktop_surface(
	struct kwl_server *server)
{
	struct kwl_object *surface;

	UNUSED_PARAMETER(server);

	/* No surface, or one that is going. */
	surface = desk.surface;
	if (surface == NULL)
		return NULL;
	if (surface->dead || surface->client->fatal)
		return NULL;

	/* A surface without an image yet shows nothing. */
	if (surface->current == NULL)
		return NULL;

	/* Succeeded: the surface to show. */
	return surface;
}

/*
 * Tells whether a surface is the desktop's (1) or not (0), whether or not
 * it has an image yet.
 */
int
kwl_desktop_is(
	const struct kwl_object *surface)
{
	/* Only the surface with the role. */
	if (surface == NULL || surface != desk.surface)
		return 0;

	/* It is the desktop's. */
	return 1;
}

/*
 * Chooses the surface that has the keyboard at a display pass: the desktop
 * while it was pressed last and the top window is the same as then,
 * otherwise the top window (which then takes the keyboard back).
 */
struct kwl_object *
kwl_desktop_front(
	struct kwl_server *server,
	struct kwl_object *top)
{
	struct kwl_object *surface;

	/* The desktop was not pressed last. */
	if (!desk.focused)
		return top;

	/* A desktop surface that went, or another top window, gives the keyboard back. */
	surface = kwl_desktop_surface(server);
	if (surface == NULL || top != desk.focus_top) {
		desk.focused = 0;
		printf("KWL DESKTOP unfocus via=window\n");
		return top;
	}

	/* Succeeded: the desktop keeps the keyboard. */
	return surface;
}

/*
 * Takes a press where no window is (after the compositor's own places): the
 * desktop surface under the pointer gets the keyboard and hears the press.
 * Returns 1 when the desktop takes it, 0 when there is no desktop there.
 */
int
kwl_desktop_press(
	struct kwl_server *server)
{
	struct kwl_object *surface;

	/* A desktop surface under the pointer. */
	surface = kwl_desktop_at(server, server->pointer_x, server->pointer_y);
	if (surface == NULL)
		return 0;

	/* It has the keyboard until another window comes on top or is pressed. */
	if (!desk.focused) {
		desk.focused = 1;
		desk.focus_top = kwl_top_window(server);
		printf("KWL DESKTOP focus client=%llu surface=%u\n", (unsigned long long)surface->client->number, surface->id);
	}

	/* The focus follows now, so that the press reaches the desktop. */
	server->front_surface = surface;
	kwl_seat_focus(server);
	server->dirty = 1;

	/* Succeeded: the desktop hears the press. */
	return 1;
}

/*
 * Gives the keyboard back to the top window when the desktop has it (a
 * window was pressed).
 */
void
kwl_desktop_unfocus(
	struct kwl_server *server)
{
	/* Only a desktop with the keyboard gives it back. */
	if (!desk.focused)
		return;

	/* The top window takes it at once, before the press that caused this is delivered. */
	desk.focused = 0;
	server->front_surface = kwl_top_window(server);
	kwl_seat_focus(server);
	printf("KWL DESKTOP unfocus via=press\n");

	/* Succeeded: the top window has the keyboard. */
	return;
}

/*
 * Finds the desktop surface at a point of the output where no window is
 * (for a press, a finger, a drag and drop's target); NULL when a window,
 * its title bar or the system bar is there, or there is no desktop.
 */
struct kwl_object *
kwl_desktop_at(
	struct kwl_server *server,
	int32_t x,
	int32_t y)
{
	struct kwl_object *surface;
	struct kwl_object *window;
	uint32_t width;
	uint32_t height;

	/* A desktop with an image, in the windows' mode. */
	surface = kwl_desktop_surface(server);
	if (surface == NULL || !server->windowed)
		return NULL;

	/* The point must be on it. */
	kwl_surface_size(surface, &width, &height);
	if (x < surface->x || y < surface->y)
		return NULL;
	if (x >= surface->x + (int32_t)width || y >= surface->y + (int32_t)height)
		return NULL;

	/* No window there: in the glass look any window or title bar at the point, in the plain look any window at all. */
	if (server->glass) {
		window = kwl_glass_window_at(server, x, y);
	} else {
		window = kwl_top_window(server);
	}

	/* A window covers the desktop there. */
	if (window != NULL)
		return NULL;

	/* Succeeded: the desktop is at the point. */
	return surface;
}

/* Gives a surface the desktop's role (one surface, for the token given), and configures it. */
static int
desktop_get(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *surface;
	struct kwl_object *created;
	struct kwl_server *server;
	const char *token;
	uint32_t id;
	uint32_t surface_id;
	int differs;
	int error;

	/* The new ID, the surface and the token. */
	server = manager->client->server;
	if (size < 12U)
		return EPROTO;
	id = desktop_word(bytes, 0U);
	surface_id = desktop_word(bytes, 4U);
	surface = kwl_find(manager->client, surface_id);
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;
	error = desktop_string(bytes, size, 8U, &token);
	if (error != 0)
		return error;

	/* The token must be the one given; without one given no client may have the role. */
	differs = 1;
	if (desk.token[0] != '\0')
		differs = strcmp(token, desk.token);
	if (differs != 0) {
		printf("KWL DESKTOP refused client=%llu reason=token\n", (unsigned long long)manager->client->number);
		(void)kwl_error_code(manager->client, manager->id, DESKTOP_ERROR_TOKEN, "not the desktop's token");
		return EPROTO;
	}

	/* One desktop surface, on a surface with no other role. */
	if (desk.surface != NULL ||
	    surface->role != NULL ||
	    surface->sub_role != NULL ||
	    surface->cursor_role) {
		printf("KWL DESKTOP refused client=%llu reason=role\n", (unsigned long long)manager->client->number);
		(void)kwl_error_code(manager->client, manager->id, DESKTOP_ERROR_ROLE, "the desktop has a surface, or the surface has a role");
		return EPROTO;
	}

	/* The desktop surface, tied to its surface. */
	created = kwl_create(manager->client, id, KWL_DESKTOP_SURFACE, manager->version);
	if (created == NULL)
		return EPROTO;
	created->surface = surface;
	desk.surface = surface;
	desk.object = created;
	desk.parts_known = 0U;
	desk.pending_parts_set = 0U;

	/* Where it is, told to the client. */
	error = desktop_configure(server);
	if (error != 0)
		return error;

	/* The log the tests read. */
	printf("KWL DESKTOP role client=%llu surface=%u x=%d y=%d width=%d height=%d\n", (unsigned long long)surface->client->number, surface->id, desk.x, desk.y, desk.width, desk.height);

	/* Succeeded: the surface is the desktop's. */
	return 0;
}

/* Reads a protocol string at an offset (its length with the end, then the bytes padded to a word); EPROTO when it does not fit. */
static int
desktop_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text)
{
	uint32_t length;
	size_t padded;

	/* The length, which counts the terminating zero. */
	if (offset + 4U > size)
		return EPROTO;
	length = desktop_word(bytes, offset);
	if (length == 0U || length > DESKTOP_TOKEN_MAX + 1U)
		return EPROTO;

	/* The bytes, padded to a whole word, must be the rest of the request. */
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (offset + 4U + padded != size)
		return EPROTO;

	/* The string must end where its length says. */
	if (bytes[offset + 4U + length - 1U] != '\0')
		return EPROTO;

	/* Succeeded: the string. */
	*text = (const char *)(bytes + offset + 4U);
	return 0;
}

/* Works out where the desktop is: the output under the system bar in the glass look, the whole output otherwise. */
static void
desktop_place(
	struct kwl_server *server,
	int32_t *x,
	int32_t *y,
	int32_t *width,
	int32_t *height)
{
	int32_t right;
	int32_t bottom;

	/* What the on-screen keyboard's panel takes at the right or the bottom (keyboard.c, ws102-p007). */
	kwl_keyboard_reserved(&right, &bottom);

	/* The left edge, the whole width less the keyboard's column. */
	*x = 0;
	*width = (int32_t)server->width - right;

	/* Under the system bar in the glass look. */
	*y = 0;
	if (server->glass)
		*y = KWL_GLASS_BAR;

	/* The rest of the height, less the keyboard's row. */
	*height = (int32_t)server->height - *y - bottom;

	/* Succeeded: the place is given. */
	return;
}

/* Tells the desktop surface its place (configure) and moves it there. */
static int
desktop_configure(
	struct kwl_server *server)
{
	uint32_t words[5];
	int error;

	/* The place. */
	desktop_place(server, &desk.x, &desk.y, &desk.width, &desk.height);
	desk.surface->x = desk.x;
	desk.surface->y = desk.y;

	/* configure(serial, x, y, width, height). */
	desk.serial = kwl_next_serial(server);
	words[0] = desk.serial;
	words[1] = (uint32_t)desk.x;
	words[2] = (uint32_t)desk.y;
	words[3] = (uint32_t)desk.width;
	words[4] = (uint32_t)desk.height;
	error = kwl_emit(desk.object->client, desk.object->id, DESKTOP_SURFACE_CONFIGURE, words, sizeof(words));
	if (error != 0)
		return error;

	/* Succeeded: the client knows where it is. */
	server->dirty = 1;
	return 0;
}

/*
 * Starts the desktop program when there should be one and none runs: in a
 * login session unless /etc/keiland/desktop is "off", or when
 * --desktop-client named one; two seconds after the last one ended, and
 * not more than DESKTOP_STARTS times a minute.
 */
static void
desktop_start(
	struct kwl_server *server)
{
	char line[DESKTOP_LINE_MAX];
	uint64_t now;
	int error;
	int off;

	/* Whether a program is wanted is decided once: the option, else a login session's switch. */
	if (!desk.decided) {
		desk.decided = 1;
		desk.command = NULL;
		if (server->session) {
			/* A session shows the desktop unless it is switched off. */
			off = desktop_switched_off();
			if (!off)
				desk.command = DESKTOP_COMMAND;
		}
	}

	/* No program wanted, or one running, or too many starts already. */
	if (desk.command == NULL || desk.command[0] == '\0')
		return;
	if (desk.pid > 0 || desk.starting || desk.limited)
		return;

	/* Not before two seconds after the last one ended (the loop comes back then). */
	now = kwl_milliseconds();
	if (desk.gone_ms != 0U && now - desk.gone_ms < DESKTOP_RESTART_MS) {
		kwl_due(server, desk.gone_ms + DESKTOP_RESTART_MS);
		return;
	}

	/* A new minute of starts, or one more in this minute; too many stop the starts. */
	if (desk.starts == 0U || now - desk.starts_ms >= DESKTOP_STARTS_MS) {
		desk.starts = 0U;
		desk.starts_ms = now;
	}

	/* Too many starts in the minute stop the starts for good. */
	if (desk.starts >= DESKTOP_STARTS) {
		desk.limited = 1;
		printf("KWL DESKTOP start-limit starts=%u\n", desk.starts);
		return;
	}

	/* A new token for each start, unless a fixed one was given. */
	if (!desk.fixed)
		desktop_new_token();

	/* The program, its token in its environment only. */
	snprintf(line, sizeof(line), "KEILAND_DESKTOP_TOKEN=%s exec %s", desk.token, desk.command);
	desk.starts++;
	desk.starting = 1;
	error = kwl_spawn(server, line, desktop_started, NULL);
	if (error != 0)
		desktop_started(server, 0, error, NULL);
}

/*
 * Hears the desktop program's start (every start counts toward the
 * minute's limit, a failed one too): it runs, or is tried again after a
 * while.
 */
static void
desktop_started(
	struct kwl_server *server,
	pid_t pid,
	int error,
	void *context)
{
	/* Failed: started again after a while. */
	(void)server;
	(void)context;
	desk.starting = 0;
	if (error != 0) {
		printf("KWL DESKTOP start-failed errno=%d\n", error);
		desk.pid = 0;
		desk.gone_ms = kwl_milliseconds();
		return;
	}

	/* Succeeded: the log the tests read; its first image is logged when drawn. */
	desk.pid = pid;
	desk.drawn = 0;
	printf("KWL DESKTOP start pid=%d command=%s at_ms=%llu\n", (int)desk.pid, desk.command, (unsigned long long)kwl_milliseconds());
}

/*
 * Starts Settings' Welcome once in a login session, after the desktop
 * program was started (or was decided against), while welcome.done is 0
 * (logged either way); on the installer's image, the installer instead.
 */
static void
desktop_welcome(
	struct kwl_server *server)
{
	int installer;
	int done;
	int error;

	/* Once, in a session that is not the login screen, after the desktop program's decision. */
	if (desk.welcome_decided || !desk.decided)
		return;
	if (!server->session || server->greeter)
		return;
	desk.welcome_decided = 1;

	/* The installer's image opens the installer, not the Welcome. */
	installer = access(DESKTOP_INSTALLER_INDEX, R_OK);
	if (installer == 0) {
		/* Starts the installer, which is not started again when it ends. */
		error = kwl_spawn(server, "exec " DESKTOP_INSTALLER, desktop_installer_started, NULL);
		if (error != 0)
			printf("KWL INSTALLER start-failed errno=%d\n", error);
		return;
	}

	/* The account took or skipped it already (a store that cannot tell starts nothing). */
	done = 1;
	error = kwl_settings_number(server, "welcome.done", &done);
	if (error != 0 || done != 0) {
		printf("KWL WELCOME skip done=%d error=%d\n", done, error);
		return;
	}

	/* Settings in the Welcome's mode (its end sets welcome.done). */
	error = kwl_spawn(server, "exec " DESKTOP_WELCOME, desktop_welcome_started, NULL);
	if (error != 0)
		printf("KWL WELCOME start-failed errno=%d\n", error);
}

/* Logs the installer's start (its process ID, or the failure). */
static void
desktop_installer_started(
	struct kwl_server *server,
	pid_t pid,
	int error,
	void *context)
{
	/* The failure. */
	(void)server;
	(void)context;
	if (error != 0) {
		printf("KWL INSTALLER start-failed errno=%d\n", error);
		return;
	}

	/* Succeeded: the log the tests read. */
	printf("KWL INSTALLER start pid=%d\n", (int)pid);
}

/* Logs the Welcome's start (its process ID, or the failure). */
static void
desktop_welcome_started(
	struct kwl_server *server,
	pid_t pid,
	int error,
	void *context)
{
	/* The failure. */
	(void)server;
	(void)context;
	if (error != 0) {
		printf("KWL WELCOME start-failed errno=%d\n", error);
		return;
	}

	/* Succeeded: the log the tests read. */
	printf("KWL WELCOME start pid=%d\n", (int)pid);
}

/* Notices that the desktop program ended (its own wait, or App Home's collecting every child). */
static void
desktop_watch(
	struct kwl_server *server)
{
	pid_t ended;
	int status;

	UNUSED_PARAMETER(server);

	/* No program running. */
	if (desk.pid <= 0)
		return;

	/* Still running: nothing to do. */
	ended = waitpid(desk.pid, &status, WNOHANG);
	if (ended == 0)
		return;

	/* Ended, or collected already (ECHILD); another error leaves it running. */
	if (ended < 0 && errno != ECHILD)
		return;

	/* The program is gone; it is started again after a while. */
	printf("KWL DESKTOP exited pid=%d\n", (int)desk.pid);
	desk.pid = 0;
	desk.gone_ms = kwl_milliseconds();

	/* Succeeded: the end is noted. */
	return;
}

/* Tells whether /etc/keiland/desktop turns the desktop off (its first word is "off"). */
static int
desktop_switched_off(
	void)
{
	char text[8];
	ssize_t count;
	int descriptor;
	int match;

	/* Without the file the desktop is on. */
	descriptor = open(DESKTOP_SWITCH, O_RDONLY);
	if (descriptor < 0)
		return 0;

	/* Its first bytes. */
	memset(text, 0, sizeof(text));
	count = read(descriptor, text, sizeof(text) - 1U);
	if (count < 3) {
		close(descriptor);
		return 0;
	}

	/* The file is not needed any more. */
	close(descriptor);

	/* "off", alone on its line or followed by a space, turns it off. */
	match = strncmp(text, "off", 3U);
	if (match != 0)
		return 0;
	if (text[3] != '\0' &&
	    text[3] != '\n' &&
	    text[3] != ' ')
		return 0;

	/* The desktop is switched off. */
	return 1;
}

/* Makes a new random token, written in hex. */
static void
desktop_new_token(
	void)
{
	static const char digits[] = "0123456789abcdef";
	unsigned char bytes[DESKTOP_TOKEN_BYTES];
	unsigned index;

	/* Random bytes from the kernel's generator. */
	arc4random_buf(bytes, sizeof(bytes));

	/* Each byte as two hex digits. */
	for (index = 0; index < DESKTOP_TOKEN_BYTES; index++) {
		desk.token[index * 2U] = digits[bytes[index] >> 4];
		desk.token[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
	}

	/* The token ends after them. */
	desk.token[DESKTOP_TOKEN_BYTES * 2U] = '\0';

	/* Succeeded: a new token. */
	return;
}

/* Reads one possibly unaligned native-endian protocol word. */
static uint32_t
desktop_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word, copied out byte by byte. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word in the host's order. */
	return word;
}

/*
 * Takes set_drawn's rectangles (an array of x, y, width and height, 32-bit
 * each), applied with the surface's next commit.  More than
 * DESKTOP_PARTS_MAX are kept as the box around them.  Returns 0, or EPROTO
 * for an array that does not fit the request or holds part of a rectangle.
 */
static int
desktop_set_drawn(
	const unsigned char *bytes,
	size_t size)
{
	int32_t rect[4];
	int32_t box[4];
	uint32_t length;
	uint32_t count;
	uint32_t index;
	unsigned side;

	/* The array's length, whole rectangles, within the request (padded to words). */
	if (size < 4U)
		return EPROTO;
	length = desktop_word(bytes, 0U);
	if (length % 16U != 0U || (size_t)length + 4U != size)
		return EPROTO;
	count = length / 16U;

	/* Each rectangle, and the box around them all. */
	box[0] = INT32_MAX;
	box[1] = INT32_MAX;
	box[2] = INT32_MIN;
	box[3] = INT32_MIN;
	for (index = 0U; index < count; index++) {
		for (side = 0U; side < 4U; side++)
			rect[side] = (int32_t)desktop_word(bytes, 4U + (size_t)index * 16U + side * 4U);

		/* Kept while there is room. */
		if (index < DESKTOP_PARTS_MAX)
			memcpy(desk.pending_parts[index], rect, sizeof(rect));

		/* The box's edges. */
		if (rect[0] < box[0])
			box[0] = rect[0];
		if (rect[1] < box[1])
			box[1] = rect[1];
		if (rect[0] + rect[2] > box[2])
			box[2] = rect[0] + rect[2];
		if (rect[1] + rect[3] > box[3])
			box[3] = rect[1] + rect[3];
	}

	/* Too many: the box around them. */
	desk.pending_part_count = count;
	if (count > DESKTOP_PARTS_MAX) {
		desk.pending_parts[0][0] = box[0];
		desk.pending_parts[0][1] = box[1];
		desk.pending_parts[0][2] = box[2] - box[0];
		desk.pending_parts[0][3] = box[3] - box[1];
		desk.pending_part_count = 1U;
	}

	/* Succeeded: applied with the next commit. */
	desk.pending_parts_set = 1U;
	return 0;
}
