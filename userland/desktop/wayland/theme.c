/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * kl_theme_v1 (ws089-p017): the desktop's appearance, light or dark,
 * told to a client when it binds the global and again whenever the
 * setting appearance.dark changes (settings.c calls kwl_theme_changed),
 * so that libkeiland's applications draw in it and redraw.  The
 * compositor's own drawing follows server->dark (glass.c).
 *
 * Version 2 (ws179-p001) tells the accent the user chose
 * (appearance.accent) after the appearance, to an object bound at
 * version 2 or later only.  The compositor's own drawing takes the
 * accent's colours with kwl_accent.
 */

#include "kwl.h"

#include "../artwork/accent.h"

#include <errno.h>
#include <stdio.h>

/* The request and the events of kl_theme_v1 (accent since version 2). */
#define THEME_DESTROY		0U
#define THEME_EVENT_APPEARANCE	0U
#define THEME_EVENT_ACCENT	1U
#define THEME_ACCENT_VERSION	2U

static int theme_send(struct kwl_object *theme);
static void theme_rgba(uint32_t colour, float *rgba);

/*
 * Carries out a request of kl_theme_v1: destroy is the only one.
 */
int
kwl_theme_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	/* destroy, without arguments. */
	(void)bytes;
	if (opcode != THEME_DESTROY || size != 0U)
		return EPROTO;
	kwl_object_destroy(object);

	/* Succeeded. */
	return 0;
}

/*
 * Tells a client that bound the global the appearance now.
 */
int
kwl_theme_bind(
	struct kwl_object *theme)
{
	int error;

	/* The client draws in the appearance from now on (its windows' glass follows it, panels.c). */
	theme->client->theme_bound = 1U;
	theme->client->server->dirty = 1;

	/* The appearance. */
	error = theme_send(theme);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Tells every client that bound the global the appearance, after it
 * changed.
 */
void
kwl_theme_changed(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *object;

	/* Each live object of each client. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (object = client->objects; object != NULL; object = object->next) {
			if (object->kind == KWL_THEME && !object->dead)
				(void)theme_send(object);
		}
	}

	/* Logs the appearance and the accent told, for the tests. */
	printf("KWL THEME appearance=%d accent=%d\n", (int)server->dark, (int)server->accent);
}

/*
 * Writes a part of the accent's colours (KWL_ACCENT_*: the accent, the ink
 * on it, the accent as text) for a surface whose ground is dark
 * (dark_ground) or light, with an opacity, as 0 to 1 RGBA.
 */
void
kwl_accent_colour(
	const struct kwl_server *server,
	int dark_ground,
	unsigned part,
	float alpha,
	float *out)
{
	struct ka_accent values;
	unsigned appearance;
	uint32_t colour;

	/* The table's row for the accent and the ground. */
	appearance = KA_LIGHT;
	if (dark_ground)
		appearance = KA_DARK;
	ka_accent_values((unsigned)server->accent, appearance, &values);

	/* The part asked for, with the opacity asked for. */
	colour = values.accent;
	if (part == KWL_ACCENT_INK)
		colour = values.ink;
	else if (part == KWL_ACCENT_TEXT)
		colour = values.text;
	theme_rgba(colour, out);
	out[3] = alpha;
}

/*
 * Starts drawing in the accent's colours as they are: the dark
 * appearance's mapping of the colours is left out until kwl_accent_done
 * (glass.c reads server->keep_colours).  Returns what kwl_accent_done
 * restores.
 */
unsigned
kwl_accent_as_is(
	struct kwl_server *server)
{
	unsigned previous;

	/* The colours kept from now on. */
	previous = server->keep_colours;
	server->keep_colours = 1U;

	/* What was set before. */
	return previous;
}

/* Ends drawing in the accent's colours as they are: the mapping is what it was. */
void
kwl_accent_done(
	struct kwl_server *server,
	unsigned previous)
{
	/* The setting before kwl_accent_as_is. */
	server->keep_colours = previous;
}

/* Sends one object the appearance (0 light, 1 dark) and, from version 2, the accent. */
static int
theme_send(
	struct kwl_object *theme)
{
	uint32_t appearance;
	uint32_t accent;
	int error;

	/* The appearance as the protocol's value. */
	appearance = 0U;
	if (theme->client->server->dark != 0)
		appearance = 1U;
	error = kwl_emit(theme->client, theme->id, THEME_EVENT_APPEARANCE, &appearance, sizeof(appearance));
	if (error != 0)
		return error;

	/* An object of version 1 knows no accent. */
	if (theme->version < THEME_ACCENT_VERSION)
		return 0;

	/* The accent's index. */
	accent = (uint32_t)theme->client->server->accent;
	error = kwl_emit(theme->client, theme->id, THEME_EVENT_ACCENT, &accent, sizeof(accent));
	if (error != 0)
		return error;

	/* Succeeded: the object knows both. */
	return 0;
}

/* Turns a 0xAARRGGBB colour into 0 to 1 RGBA. */
static void
theme_rgba(
	uint32_t colour,
	float *rgba)
{
	/* Each channel. */
	rgba[0] = (float)((colour >> 16) & 0xffU) / 255.0f;
	rgba[1] = (float)((colour >> 8) & 0xffU) / 255.0f;
	rgba[2] = (float)(colour & 0xffU) / 255.0f;
	rgba[3] = (float)((colour >> 24) & 0xffU) / 255.0f;
}
