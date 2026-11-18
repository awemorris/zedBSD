/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xdg_activation_v1 (ws089-p016): a program asks the compositor for a
 * token, hands it to another program, and that program brings one of its
 * windows to the front with it.  A second start of a program that keeps
 * one window (Settings) hands its request and its token to the first,
 * which activates its window with the token.
 *
 * A token is text the compositor makes from random bytes, good for one
 * activation within ACTIVATION_LIFE_MS.  It is granted -- an activation
 * with it brings the window -- when the client that asked for it has the
 * keyboard's focus (the user is working in it), or when the client is a
 * program that has just started and shows no window yet: such a program
 * could show a window of its own on top anyway, so handing that right to
 * another program's window grants nothing new.  A token not granted is
 * still given (a client cannot tell), and its activation does nothing.
 * The launcher (kwl_spawn) gives each program it starts a granted token in
 * XDG_ACTIVATION_TOKEN.
 */

#include "activation.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The requests of xdg_activation_v1. */
#define ACTIVATION_DESTROY		0U
#define ACTIVATION_GET_TOKEN		1U
#define ACTIVATION_ACTIVATE		2U

/* The requests of xdg_activation_token_v1, its event, and its error. */
#define TOKEN_SET_SERIAL		0U
#define TOKEN_SET_APP_ID		1U
#define TOKEN_SET_SURFACE		2U
#define TOKEN_COMMIT			3U
#define TOKEN_DESTROY			4U
#define TOKEN_EVENT_DONE		0U
#define TOKEN_ERROR_ALREADY_USED	0U

/* How many tokens wait for their activation at once (a new one takes the oldest one's place). */
#define ACTIVATION_TOKENS		16U

/* How long a token is good for, in milliseconds (a program's start on a slow machine fits). */
#define ACTIVATION_LIFE_MS		30000U

/* How long after its connection a client without a window counts as a program that has just started, in milliseconds. */
#define ACTIVATION_FRESH_MS		5000U

/* The random bytes behind a token's text (two hexadecimal digits each). */
#define ACTIVATION_RANDOM_BYTES		16U

/*
 * One token the compositor gave: its text, the application it was asked
 * for (for the log), when it was given, whether it still waits for its
 * one activation, and whether that activation may bring a window.
 */
struct activation_token {
	char text[KWL_ACTIVATION_TOKEN_SIZE];
	char app_id[64];
	uint64_t issued_ms;
	unsigned live;
	unsigned granted;
};

/*
 * The tokens given and not used yet, for the compositor's life.  A slot
 * is in use while live is set; an activation, or a new token when every
 * slot is in use (the oldest gives way), clears it.  Only the event loop's
 * thread touches it.
 */
static struct activation_token activation_tokens[ACTIVATION_TOKENS];

static int activation_create_token(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int activation_activate(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int activation_token_request(struct kwl_object *token, uint32_t opcode, const unsigned char *bytes, size_t size);
static int activation_commit(struct kwl_object *token);
static int activation_grant(struct kwl_client *client, const char **reason);
static int activation_shows_window(const struct kwl_client *client);
static int activation_window(const struct kwl_object *surface);
static struct activation_token *activation_slot(uint64_t now);
static int activation_text(char *text, size_t size);
static int activation_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static uint32_t activation_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of xdg_activation_v1 or of one of its tokens.
 */
int
kwl_activation_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* A token's own requests. */
	if (object->kind == KWL_ACTIVATION_TOKEN) {
		error = activation_token_request(object, opcode, bytes, size);
		if (error != 0)
			return error;
		return 0;
	}

	/* The global goes; its tokens stay. */
	if (opcode == ACTIVATION_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* A new token, or an activation with one. */
	if (opcode == ACTIVATION_GET_TOKEN) {
		error = activation_create_token(object, bytes, size);
	} else if (opcode == ACTIVATION_ACTIVATE) {
		error = activation_activate(object, bytes, size);
	} else {
		error = EPROTO;
	}

	/* Reports a request that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Gives a granted token for a program the compositor starts (the launcher
 * puts it in the program's XDG_ACTIVATION_TOKEN).  Returns 0 with the
 * token's text in token, or the error that kept it from being made.
 */
int
kwl_activation_issue(
	struct kwl_server *server,
	const char *app_id,
	const char *via,
	char *token,
	size_t size)
{
	struct activation_token *slot;
	uint64_t now;
	int error;

	/* Room for the text. */
	if (size < KWL_ACTIVATION_TOKEN_SIZE)
		return ENOSPC;

	/* A slot for it. */
	(void)server;
	now = kwl_milliseconds();
	slot = activation_slot(now);

	/* Its text, from random bytes. */
	error = activation_text(slot->text, sizeof(slot->text));
	if (error != 0)
		return error;

	/* Granted: the program was started by the user's choice in the compositor. */
	snprintf(slot->app_id, sizeof(slot->app_id), "%s", app_id);
	slot->issued_ms = now;
	slot->live = 1;
	slot->granted = 1;
	snprintf(token, size, "%s", slot->text);
	printf("KWL ACTIVATION token client=0 app=%s granted=1 reason=%s at_ms=%llu\n", slot->app_id, via, (unsigned long long)now);

	/* Succeeded: the caller holds the token's text. */
	return 0;
}

/* Makes a token object for the client (get_activation_token: its new ID). */
static int
activation_create_token(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;

	/* The new ID alone. */
	if (size != 4U)
		return EPROTO;

	/* The token object, with nothing set yet and not committed. */
	created = kwl_create(manager->client, activation_word(bytes, 0U), KWL_ACTIVATION_TOKEN, manager->version);
	if (created == NULL)
		return EPROTO;
	created->app_id[0] = '\0';
	created->activation_surface = 0U;
	created->activation_committed = 0;

	/* Succeeded: the client can describe the token and commit it. */
	return 0;
}

/*
 * Brings a window of the client to the front with a token (activate: the
 * token's text and the surface).  A token unknown, used, too old or not
 * granted, a surface that is not a shown window, and a locked screen leave
 * the windows as they are; the request itself is not an error.
 */
static int
activation_activate(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct activation_token *found;
	struct kwl_server *server;
	struct kwl_object *surface;
	const char *reason;
	const char *text;
	uint64_t now;
	size_t offset;
	unsigned index;
	int window;
	int error;
	int same;

	/* The token's text, then the surface, and nothing after them. */
	error = activation_string(bytes, size, 0U, &text, &offset);
	if (error != 0)
		return error;
	if (offset + 4U != size)
		return EPROTO;

	/* One of the client's surfaces. */
	surface = kwl_find(manager->client, activation_word(bytes, offset));
	if (surface == NULL || surface->kind != KWL_SURFACE)
		return EPROTO;

	/* The token, which this activation uses up whatever comes of it. */
	server = manager->client->server;
	now = kwl_milliseconds();
	found = NULL;
	for (index = 0; index < ACTIVATION_TOKENS; index++) {
		if (!activation_tokens[index].live)
			continue;
		same = strcmp(activation_tokens[index].text, text);
		if (same == 0) {
			found = &activation_tokens[index];
			found->live = 0;
			break;
		}
	}

	/* Whether the window comes to the front: a known, fresh, granted token, a shown window, an unlocked screen. */
	window = activation_window(surface);
	reason = NULL;
	if (found == NULL) {
		reason = "unknown-token";
	} else if (now - found->issued_ms > ACTIVATION_LIFE_MS) {
		reason = "expired";
	} else if (!found->granted) {
		reason = "not-granted";
	} else if (!window) {
		reason = "not-window";
	} else if (server->locked || server->greeter) {
		reason = "locked";
	}

	/* Refused: the windows stay as they are. */
	if (reason != NULL) {
		printf("KWL ACTIVATION activate client=%llu surface=%u result=refused reason=%s at_ms=%llu\n", (unsigned long long)manager->client->number, surface->id, reason, (unsigned long long)now);
		return 0;
	}

	/* The window, on its desktop, in front with the keyboard's focus. */
	printf("KWL ACTIVATION activate client=%llu surface=%u app=%s result=activated token_app=%s at_ms=%llu\n", (unsigned long long)manager->client->number, surface->id, surface->app_id, found->app_id, (unsigned long long)now);
	kwl_glass_activate(server, surface, "activation");

	/* Succeeded: the window is in front. */
	return 0;
}

/* Carries out a request of a token object: its description, its commit, or its end. */
static int
activation_token_request(
	struct kwl_object *token,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const char *text;
	size_t offset;
	int error;

	/* The token object goes (the token it gave stays good). */
	if (opcode == TOKEN_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(token);
		return 0;
	}

	/* After the commit nothing else may be asked of it. */
	if (token->activation_committed) {
		(void)kwl_error_code(token->client, token->id, TOKEN_ERROR_ALREADY_USED, "the token was already committed");
		return EPROTO;
	}

	/* Each description, then the commit. */
	switch (opcode) {
	case TOKEN_SET_SERIAL:
		/* The input event behind the request (the focus decides instead): a serial and a seat. */
		if (size != 8U)
			return EPROTO;
		break;
	case TOKEN_SET_APP_ID:
		/* The application the token is for, kept for the log (cut to what the record holds). */
		error = activation_string(bytes, size, 0U, &text, &offset);
		if (error != 0)
			return error;
		if (offset != size)
			return EPROTO;
		snprintf(token->app_id, sizeof(token->app_id), "%s", text);
		break;
	case TOKEN_SET_SURFACE:
		/* The surface the request came from (the focus decides instead). */
		if (size != 4U)
			return EPROTO;
		token->activation_surface = activation_word(bytes, 0U);
		break;
	case TOKEN_COMMIT:
		/* The token is made and sent. */
		if (size != 0U)
			return EPROTO;
		error = activation_commit(token);
		if (error != 0)
			return error;
		break;
	default:
		return EPROTO;
	}

	/* Succeeded: the request was carried out. */
	return 0;
}

/* Makes the token a committed token object stands for, and sends its text (done). */
static int
activation_commit(
	struct kwl_object *token)
{
	unsigned char payload[4U + KWL_ACTIVATION_TOKEN_SIZE + 3U];
	struct activation_token *slot;
	const char *reason;
	uint32_t length;
	uint64_t now;
	size_t aligned;
	int granted;
	int error;

	/* Committed: the object takes no more descriptions. */
	token->activation_committed = 1;

	/* A slot, and the token's text from random bytes. */
	now = kwl_milliseconds();
	slot = activation_slot(now);
	error = activation_text(slot->text, sizeof(slot->text));
	if (error != 0)
		return error;

	/* Whether its activation may bring a window. */
	granted = activation_grant(token->client, &reason);
	snprintf(slot->app_id, sizeof(slot->app_id), "%s", token->app_id);
	slot->issued_ms = now;
	slot->live = 1;
	slot->granted = (unsigned)granted;
	printf("KWL ACTIVATION token client=%llu app=%s granted=%d reason=%s at_ms=%llu\n", (unsigned long long)token->client->number, slot->app_id, granted, reason, (unsigned long long)now);

	/* The done event: the text as a string argument (its length with the NUL, the bytes, the padding). */
	length = (uint32_t)strlen(slot->text) + 1U;
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	memset(payload, 0, sizeof(payload));
	memcpy(payload, &length, sizeof(length));
	memcpy(payload + 4U, slot->text, (size_t)length - 1U);
	error = kwl_emit(token->client, token->id, TOKEN_EVENT_DONE, payload, 4U + aligned);
	if (error != 0)
		return error;

	/* Succeeded: the client has its token. */
	return 0;
}

/*
 * Decides whether a client's token may bring a window: the client has the
 * keyboard's focus, or it has just started and shows no window.  Returns
 * 1 or 0, with the reason for the log.
 */
static int
activation_grant(
	struct kwl_client *client,
	const char **reason)
{
	struct kwl_server *server;
	uint64_t now;
	int shows;

	/* The user is working in the client. */
	server = client->server;
	if (server->focus != NULL &&
	    !server->focus->dead &&
	    server->focus->client == client) {
		*reason = "focus";
		return 1;
	}

	/* A program that has just started and shows no window yet. */
	now = kwl_milliseconds();
	shows = activation_shows_window(client);
	if (!shows && now - client->connected_ms <= ACTIVATION_FRESH_MS) {
		*reason = "new-program";
		return 1;
	}

	/* Otherwise the token brings nothing. */
	*reason = "no-focus";
	return 0;
}

/* Tells whether a client shows a window (a mapped surface of its own). */
static int
activation_shows_window(
	const struct kwl_client *client)
{
	const struct kwl_object *object;

	/* Each of its surfaces. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->kind == KWL_SURFACE && !object->dead && object->mapped)
			return 1;
	}

	/* None is shown. */
	return 0;
}

/* Tells whether a surface is a shown window: a mapped toplevel. */
static int
activation_window(
	const struct kwl_object *surface)
{
	/* Shown. */
	if (surface->dead || !surface->mapped)
		return 0;

	/* A toplevel's surface (not a popup's, a cursor's or the desktop's icons). */
	if (surface->role == NULL ||
	    surface->role->top == NULL ||
	    surface->role->top->kind != KWL_TOPLEVEL)
		return 0;

	/* Succeeded: it is a window. */
	return 1;
}

/* Finds the slot for a new token: a free one, else the oldest. */
static struct activation_token *
activation_slot(
	uint64_t now)
{
	struct activation_token *oldest;
	unsigned index;

	/* A free slot, or one whose token can no longer be used; otherwise the oldest gives way. */
	oldest = &activation_tokens[0];
	for (index = 0; index < ACTIVATION_TOKENS; index++) {
		if (!activation_tokens[index].live)
			return &activation_tokens[index];
		if (now - activation_tokens[index].issued_ms > ACTIVATION_LIFE_MS)
			return &activation_tokens[index];
		if (activation_tokens[index].issued_ms < oldest->issued_ms)
			oldest = &activation_tokens[index];
	}

	/* Every slot holds a token still good: the oldest. */
	return oldest;
}

/* Writes a token's text: random bytes in hexadecimal. */
static int
activation_text(
	char *text,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	unsigned char random[ACTIVATION_RANDOM_BYTES];
	unsigned index;
	int status;

	/* Room for two digits a byte and the NUL. */
	if (size < ACTIVATION_RANDOM_BYTES * 2U + 1U)
		return ENOSPC;

	/* The random bytes, which nobody can guess. */
	status = getentropy(random, sizeof(random));
	if (status != 0)
		return errno;

	/* Each byte as two digits. */
	for (index = 0; index < ACTIVATION_RANDOM_BYTES; index++) {
		text[index * 2U] = digits[random[index] >> 4];
		text[index * 2U + 1U] = digits[random[index] & 0x0fU];
	}

	/* The end of the text. */
	text[ACTIVATION_RANDOM_BYTES * 2U] = '\0';

	/* Succeeded: the text is written. */
	return 0;
}

/* Validates one non-null string argument and returns where the next argument starts. */
static int
activation_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t aligned;
	size_t actual;

	/* The length word fits. */
	if (offset > size || size - offset < 4U)
		return EPROTO;

	/* A non-null string, its NUL within the bytes there are. */
	length = activation_word(bytes, offset);
	if (length == 0U || length > size - offset - 4U)
		return EPROTO;

	/* Its padded storage fits too. */
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned > size - offset - 4U)
		return EPROTO;

	/* It ends at its NUL, with none before. */
	*text = (const char *)bytes + offset + 4U;
	if ((*text)[length - 1U] != '\0')
		return EPROTO;
	actual = strlen(*text);
	if (actual + 1U != length)
		return EPROTO;

	/* Succeeded: the next argument follows the padding. */
	*next = offset + 4U + aligned;
	return 0;
}

/* Reads one possibly unaligned protocol word. */
static uint32_t
activation_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The word, without assuming its alignment. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}
