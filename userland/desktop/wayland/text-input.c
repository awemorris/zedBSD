/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The text input protocol (text-input-unstable-v3, version 1; ws095-p004,
 * plan/ws095/design.md sections 3 and 4.1).
 *
 * An application makes a zwp_text_input_v3 for the seat.  While one of its
 * surfaces has the keyboard, its text inputs are told enter; the one it
 * enables and commits is the text input the input method serves
 * (input-method.c).  Its state (the surrounding text, what changed it, the
 * content type, the cursor's rectangle) waits for the client's commit, as
 * the protocol says, and the commits are counted: the count is the serial
 * of each done the application is sent.  A text input whose content is
 * secret (a password or a PIN, or text marked hidden or sensitive) is never
 * given to the input method, so its keys go straight to the application.
 */

#include "ime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The requests of zwp_text_input_manager_v3. */
#define MANAGER_DESTROY			0U
#define MANAGER_GET_TEXT_INPUT		1U

/* The requests of zwp_text_input_v3. */
#define INPUT_DESTROY			0U
#define INPUT_ENABLE			1U
#define INPUT_DISABLE			2U
#define INPUT_SET_SURROUNDING_TEXT	3U
#define INPUT_SET_TEXT_CHANGE_CAUSE	4U
#define INPUT_SET_CONTENT_TYPE		5U
#define INPUT_SET_CURSOR_RECTANGLE	6U
#define INPUT_COMMIT			7U

/* The events of zwp_text_input_v3. */
#define INPUT_ENTER			0U
#define INPUT_LEAVE			1U
#define INPUT_PREEDIT_STRING		2U
#define INPUT_COMMIT_STRING		3U
#define INPUT_DELETE_SURROUNDING_TEXT	4U
#define INPUT_DONE			5U

/* The content hints and purposes that make a field secret. */
#define INPUT_HINT_HIDDEN_TEXT		0x40U
#define INPUT_HINT_SENSITIVE_DATA	0x80U
#define INPUT_PURPOSE_PASSWORD		8U
#define INPUT_PURPOSE_PIN		9U

/*
 * Every live text input, newest first.
 *
 * A record is added by get_text_input and removed when its object goes
 * (kwl_text_input_object_gone); the compositor has one seat, so one list
 * serves it.
 */
static struct kwl_text_input *text_inputs;

static struct kwl_text_input *input_find(struct kwl_object *object);
static int manager_get_text_input(struct kwl_object *manager, const unsigned char *bytes, size_t size);
static int input_request(struct kwl_object *object, uint32_t opcode, const unsigned char *bytes, size_t size);
static int input_set_surrounding(struct kwl_text_input *input, const unsigned char *bytes, size_t size);
static void input_commit(struct kwl_text_input *input);
static void input_clear(struct kwl_text_input *input);
static int input_secret(const struct kwl_text_input *input);
static void input_send_surface(struct kwl_text_input *input, uint32_t opcode, struct kwl_object *surface);
static void input_emit(struct kwl_object *object, uint32_t opcode, const void *payload, size_t size);
static size_t input_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t input_word(const unsigned char *bytes, size_t offset);

/*
 * Carries out a request of zwp_text_input_manager_v3 or zwp_text_input_v3.
 */
int
kwl_text_input_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* A text input's own requests. */
	if (object->kind == KWL_TEXT_INPUT) {
		error = input_request(object, opcode, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the text input's request is carried out. */
		return 0;
	}

	/* The manager's destroy leaves the text inputs it made. */
	if (opcode == MANAGER_DESTROY && size == 0U) {
		kwl_object_destroy(object);
		return 0;
	}

	/* The manager's get_text_input. */
	if (opcode == MANAGER_GET_TEXT_INPUT) {
		error = manager_get_text_input(object, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the client has its text input. */
		return 0;
	}

	/* No other request exists. */
	return EPROTO;
}

/*
 * Forgets a text input whose object goes.
 */
void
kwl_text_input_object_gone(
	struct kwl_object *object)
{
	struct kwl_text_input **link;
	struct kwl_text_input *input;

	/* Only a text input has a record. */
	if (object->kind != KWL_TEXT_INPUT)
		return;

	/* Finds the record's link. */
	link = &text_inputs;
	while (*link != NULL && (*link)->object != object)
		link = &(*link)->next;

	/* A record that was never made (the allocation failed) leaves nothing. */
	if (*link == NULL)
		return;

	/* The input method stops serving it, then the record goes. */
	input = *link;
	kwl_ime_text_input_gone(object->client->server, input);
	*link = input->next;
	free(input->pending_text);
	free(input->text);
	free(input);
}

/*
 * Follows a change of the keyboard focus: the text inputs of the surface
 * that had it are told leave and lose their state, and those of the
 * client of the surface that has it now are told enter.
 */
void
kwl_text_input_focus(
	struct kwl_server *server,
	struct kwl_object *previous)
{
	struct kwl_text_input *input;

	/* The surface that lost the keyboard: its text inputs hear leave and are disabled. */
	for (input = text_inputs; input != NULL; input = input->next) {
		if (previous == NULL || input->surface != previous)
			continue;

		input_send_surface(input, INPUT_LEAVE, previous);
		input->surface = NULL;
		input_clear(input);
	}

	/* The surface that has the keyboard: its client's text inputs hear enter. */
	if (server->focus == NULL)
		return;

	for (input = text_inputs; input != NULL; input = input->next) {
		if (input->object->dead || input->object->client != server->focus->client)
			continue;

		/* A text input already entered there needs nothing. */
		if (input->surface == server->focus)
			continue;

		input->surface = server->focus;
		input_send_surface(input, INPUT_ENTER, server->focus);
	}
}

/*
 * Finds the text input the input method is to serve now: the enabled one
 * of the focused surface, when its content is not secret and neither the
 * lock screen nor App Home shows.
 *
 * Returns NULL when there is none.
 */
struct kwl_text_input *
kwl_text_input_current(
	struct kwl_server *server)
{
	struct kwl_text_input *input;
	int secret;

	/* Nothing has the keyboard. */
	if (server->focus == NULL)
		return NULL;

	/* The lock screen and the login screen take every key. */
	if (server->locked || server->greeter)
		return NULL;

	/* App Home takes the keys while it shows. */
	if (server->home > 0.0f)
		return NULL;

	/* The first enabled text input on the focused surface. */
	for (input = text_inputs; input != NULL; input = input->next) {
		if (input->object->dead || input->surface != server->focus || !input->enabled)
			continue;

		/* A secret field's keys go straight to the application. */
		secret = input_secret(input);
		if (secret)
			return NULL;

		/* Succeeded: this text input is served. */
		return input;
	}

	/* No text input on the focused surface is enabled. */
	return NULL;
}

/*
 * Sends a text input what the input method made, applied together at the
 * done that follows: the preedit (NULL or empty for none) with its cursor,
 * the text to commit (NULL for none), and the text to delete around the
 * cursor.
 */
void
kwl_text_input_deliver(
	struct kwl_text_input *input,
	const char *preedit,
	int32_t begin,
	int32_t end,
	const char *commit,
	uint32_t before,
	uint32_t after)
{
	unsigned char payload[KWL_IME_TEXT_MAX + 16U];
	size_t offset;
	uint32_t words[2];

	/* The preedit, or none, and its cursor. */
	offset = 0;
	if (preedit != NULL && preedit[0] != '\0') {
		offset = input_put_string(payload, offset, preedit);
	} else {
		offset = input_put_string(payload, offset, NULL);
		begin = 0;
		end = 0;
	}

	memcpy(payload + offset, &begin, 4);
	memcpy(payload + offset + 4U, &end, 4);
	input_emit(input->object, INPUT_PREEDIT_STRING, payload, offset + 8U);

	/* The text to commit, when there is some. */
	if (commit != NULL && commit[0] != '\0') {
		offset = input_put_string(payload, 0, commit);
		input_emit(input->object, INPUT_COMMIT_STRING, payload, offset);
	}

	/* The text to delete, when there is some. */
	if (before != 0U || after != 0U) {
		words[0] = before;
		words[1] = after;
		input_emit(input->object, INPUT_DELETE_SURROUNDING_TEXT, words, sizeof(words));
	}

	/* done applies them; its serial is the number of the client's commits. */
	words[0] = input->commits;
	input_emit(input->object, INPUT_DONE, words, 4U);
}

/*
 * Finds the record of a text input object.
 */
static struct kwl_text_input *
input_find(
	struct kwl_object *object)
{
	struct kwl_text_input *input;

	/* Looks through the live text inputs. */
	for (input = text_inputs; input != NULL; input = input->next) {
		if (input->object == object)
			return input;
	}

	/* The object has no record. */
	return NULL;
}

/*
 * Makes a text input for a client (get_text_input: the new ID, the seat);
 * a client whose surface has the keyboard hears enter at once.
 */
static int
manager_get_text_input(
	struct kwl_object *manager,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_server *server;
	struct kwl_object *object;
	struct kwl_text_input *input;
	uint32_t id;

	/* The request carries the new ID and the seat. */
	if (size != 8U)
		return EPROTO;

	/* The new object. */
	id = input_word(bytes, 0);
	object = kwl_create(manager->client, id, KWL_TEXT_INPUT, manager->version);
	if (object == NULL)
		return EPROTO;

	/* Its record. */
	input = calloc(1, sizeof(*input));
	if (input == NULL)
		return ENOMEM;

	input->object = object;
	input->next = text_inputs;
	text_inputs = input;

	/* A client whose surface has the keyboard hears enter now. */
	server = manager->client->server;
	if (server->focus != NULL && server->focus->client == manager->client) {
		input->surface = server->focus;
		input_send_surface(input, INPUT_ENTER, server->focus);
	}

	/* Succeeded: the text input exists. */
	return 0;
}

/*
 * Carries out a request of a text input.
 */
static int
input_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_text_input *input;
	int error;

	/* destroy retires the object (and the record with it). */
	if (opcode == INPUT_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(object);
		return 0;
	}

	/* The other requests work on the record. */
	input = input_find(object);
	if (input == NULL)
		return ENOMEM;

	/* Each request sets pending state, applied by commit. */
	switch (opcode) {
	case INPUT_ENABLE:
		/* enable asks for the input method; it resets the state, which must follow again. */
		if (size != 0U)
			return EPROTO;
		input->pending_enable = 1;
		input->pending_disable = 0;
		input->pending_text_set = 0;
		input->pending_cause = 0;
		input->pending_hint = 0;
		input->pending_purpose = 0;
		memset(input->pending_rectangle, 0, sizeof(input->pending_rectangle));
		return 0;
	case INPUT_DISABLE:
		/* disable gives the input method up. */
		if (size != 0U)
			return EPROTO;
		input->pending_disable = 1;
		input->pending_enable = 0;
		return 0;
	case INPUT_SET_SURROUNDING_TEXT:
		error = input_set_surrounding(input, bytes, size);
		return error;
	case INPUT_SET_TEXT_CHANGE_CAUSE:
		/* What changed the text last. */
		if (size != 4U)
			return EPROTO;
		input->pending_cause = input_word(bytes, 0);
		return 0;
	case INPUT_SET_CONTENT_TYPE:
		/* The hints and the purpose of the field. */
		if (size != 8U)
			return EPROTO;
		input->pending_hint = input_word(bytes, 0);
		input->pending_purpose = input_word(bytes, 4);
		return 0;
	case INPUT_SET_CURSOR_RECTANGLE:
		/* The cursor's rectangle in the surface. */
		if (size != 16U)
			return EPROTO;
		memcpy(input->pending_rectangle, bytes, 16U);
		return 0;
	case INPUT_COMMIT:
		/* The pending state applies. */
		if (size != 0U)
			return EPROTO;
		input_commit(input);
		return 0;
	default:
		break;
	}

	/* No other request exists. */
	return EPROTO;
}

/*
 * Keeps the surrounding text of set_surrounding_text (the text, the
 * cursor and the anchor) until the commit.
 */
static int
input_set_surrounding(
	struct kwl_text_input *input,
	const unsigned char *bytes,
	size_t size)
{
	uint32_t length;
	size_t padded;
	char *copy;

	/* The text's length, with its NUL. */
	if (size < 4U)
		return EPROTO;

	length = input_word(bytes, 0);
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	if (length == 0U || length > KWL_IME_TEXT_MAX || 4U + padded + 8U != size)
		return EPROTO;

	/* The text must end with its NUL. */
	if (bytes[4U + length - 1U] != '\0')
		return EPROTO;

	/* A copy replaces the pending one. */
	copy = malloc(length);
	if (copy == NULL)
		return ENOMEM;

	memcpy(copy, bytes + 4U, length);
	free(input->pending_text);
	input->pending_text = copy;
	input->pending_text_set = 1;

	/* The cursor and the anchor follow the text. */
	input->pending_cursor = (int32_t)input_word(bytes, 4U + padded);
	input->pending_anchor = (int32_t)input_word(bytes, 4U + padded + 4U);

	/* Succeeded: the surrounding text waits for the commit. */
	return 0;
}

/*
 * Applies a text input's pending state at its commit, counts the commit,
 * and tells the input method.
 */
static void
input_commit(
	struct kwl_text_input *input)
{
	struct kwl_server *server;

	/* The commit's number is the serial of the dones that follow. */
	input->commits++;

	/* enable and disable start from no state; enable turns the input method on (each said in the log, ws090-p025). */
	if (input->pending_enable) {
		input_clear(input);
		input->enabled = 1;
		printf("KWL TEXT enable client=%llu surface=%u\n", (unsigned long long)input->object->client->number,
		    input->surface != NULL ? input->surface->id : 0U);
	} else if (input->pending_disable) {
		input_clear(input);
		printf("KWL TEXT disable client=%llu surface=%u\n", (unsigned long long)input->object->client->number,
		    input->surface != NULL ? input->surface->id : 0U);
	}

	input->pending_enable = 0;
	input->pending_disable = 0;

	/* The state set since the last commit. */
	if (input->pending_text_set) {
		free(input->text);
		input->text = input->pending_text;
		input->pending_text = NULL;
		input->pending_text_set = 0;
		input->cursor = input->pending_cursor;
		input->anchor = input->pending_anchor;
		input->text_commit = input->commits;
	}

	input->cause = input->pending_cause;
	input->hint = input->pending_hint;
	input->purpose = input->pending_purpose;
	memcpy(input->rectangle, input->pending_rectangle, sizeof(input->rectangle));

	/* The input method follows (input-method.c). */
	server = input->object->client->server;
	kwl_ime_update(server, input);
}

/*
 * Empties a text input's state: it is disabled and knows nothing of its field.
 */
static void
input_clear(
	struct kwl_text_input *input)
{
	/* Nothing enabled and no state. */
	input->enabled = 0;
	free(input->text);
	input->text = NULL;
	input->cursor = 0;
	input->anchor = 0;
	input->text_commit = 0;
	input->cause = 0;
	input->hint = 0;
	input->purpose = 0;
	memset(input->rectangle, 0, sizeof(input->rectangle));
}

/*
 * Tells whether a text input's content is secret.
 */
static int
input_secret(
	const struct kwl_text_input *input)
{
	/* A password or a PIN. */
	if (input->purpose == INPUT_PURPOSE_PASSWORD || input->purpose == INPUT_PURPOSE_PIN)
		return 1;

	/* Text the application marks hidden or sensitive. */
	if ((input->hint & (INPUT_HINT_HIDDEN_TEXT | INPUT_HINT_SENSITIVE_DATA)) != 0U)
		return 1;

	/* Ordinary text. */
	return 0;
}

/*
 * Sends a text input an event that names a surface (enter or leave).
 */
static void
input_send_surface(
	struct kwl_text_input *input,
	uint32_t opcode,
	struct kwl_object *surface)
{
	uint32_t word;

	/* The event carries the surface. */
	word = surface->id;
	input_emit(input->object, opcode, &word, sizeof(word));
}

/*
 * Queues an event, failing the client whose queue refuses it (as the seat does).
 */
static void
input_emit(
	struct kwl_object *object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	int error;

	/* A dead object or a failed client hears nothing further. */
	if (object->dead || object->client->fatal)
		return;

	/* A client that stopped reading loses its connection. */
	error = kwl_emit(object->client, object->id, opcode, payload, size);
	if (error != 0) {
		kwl_client_fail(object->client);
	}
}

/*
 * Writes a string argument (NULL for a null string) and gives the offset after it.
 */
static size_t
input_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t padded;

	/* A null string is its length 0 alone. */
	if (text == NULL) {
		length = 0;
		memcpy(payload + offset, &length, 4);
		return offset + 4U;
	}

	/* The length with the NUL, the bytes, and zeros to a four-byte boundary. */
	length = (uint32_t)strlen(text) + 1U;
	if (length > KWL_IME_TEXT_MAX)
		length = KWL_IME_TEXT_MAX;
	padded = ((size_t)length + 3U) & ~(size_t)3U;
	memcpy(payload + offset, &length, 4);
	memset(payload + offset + 4U, 0, padded);
	memcpy(payload + offset + 4U, text, length - 1U);

	/* The offset after the string. */
	return offset + 4U + padded;
}

/*
 * Reads a 32-bit word of a request.
 */
static uint32_t
input_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The wire's native byte order. */
	memcpy(&word, bytes + offset, sizeof(word));
	return word;
}
