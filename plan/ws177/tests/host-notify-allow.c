/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p026: the host test of the compositor's gate on notifications
 * (userland/desktop/wayland/notify-shell.c with notify.c): an application
 * whose notify.allow.<name> setting is 0 is refused DENIED; one on, or
 * without a setting, is posted; a post without a name is known by its
 * window's app_id.
 *
 *     host-notify-allow
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/libkeiland/system/kl-system-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The checks that failed, the last event sent (opcode and words), and the settings the test sets. */
static int failures;
static uint32_t last_opcode;
static uint32_t last_words[2];
static int allow_mailer = 1;
static int allow_calendar = 0;
static int allow_browser = 0;

int main(void);
static void check(const char *name, int passed, const char *detail);
static int post(struct kwl_object *object, uint32_t request, const char *app);
static size_t put_word(unsigned char *bytes, size_t offset, uint32_t word);
static size_t put_string(unsigned char *bytes, size_t offset, const char *text);

/* The compositor's calls notify-shell.c makes, stood in for. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	/* The last one, kept. */
	(void)client;
	(void)object;
	last_opcode = opcode;
	memset(last_words, 0, sizeof(last_words));
	memcpy(last_words, payload, size < sizeof(last_words) ? size : sizeof(last_words));
	return 0;
}

struct kwl_object *
kwl_create(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind,
	uint32_t version)
{
	struct kwl_object *object;

	/* A fresh object at the list's head. */
	object = calloc(1, sizeof(*object));
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = version;
	object->next = client->objects;
	client->objects = object;
	return object;
}

struct kwl_object *
kwl_find(
	struct kwl_client *client,
	uint32_t id)
{
	struct kwl_object *object;

	/* By its ID. */
	for (object = client->objects; object != NULL; object = object->next) {
		if (object->id == id)
			return object;
	}
	return NULL;
}

void
kwl_object_destroy(
	struct kwl_object *object)
{
	/* Dead. */
	object->dead = 1;
}

uint64_t
kwl_milliseconds(void)
{
	static uint64_t now;

	/* A second apart each time: the rate never refuses. */
	now += 1000U;
	return now;
}

void
kwl_notify_system_activated(
	struct kwl_server *server,
	uint32_t id)
{
	/* Not used here. */
	(void)server;
	(void)id;
}

int
kwl_settings_number(
	struct kwl_server *server,
	const char *name,
	int *number)
{
	int same;

	/* The test's three settings; the others are not set. */
	(void)server;
	same = strcmp(name, "notify.allow.mailer");
	if (same == 0) {
		*number = allow_mailer;
		return 0;
	}
	same = strcmp(name, "notify.allow.calendar");
	if (same == 0) {
		*number = allow_calendar;
		return 0;
	}
	same = strcmp(name, "notify.allow.browser");
	if (same == 0) {
		*number = allow_browser;
		return 0;
	}
	return ENOENT;
}

/* Runs the checks. */
int
main(void)
{
	struct kwl_server *server;
	struct kwl_client *client;
	struct kwl_object manager;
	struct kwl_object *object;
	struct kwl_object *surface;
	unsigned char bytes[8];
	int status;

	/* A server, a client and its notify object. */
	server = calloc(1, sizeof(*server));
	client = calloc(1, sizeof(*client));
	client->server = server;
	client->number = 1;
	server->clients = client;
	memset(&manager, 0, sizeof(manager));
	manager.client = client;
	manager.id = 3;
	manager.kind = KWL_SYSTEM_MANAGER;
	manager.version = 24;
	(void)put_word(bytes, 0, 7U);
	status = kwl_notify_create(&manager, bytes, 4U);
	object = kwl_find(client, 7U);
	check("create", status == 0 && object != NULL, "a notify object");

	/* Mail on: posted. */
	status = post(object, 1U, "mailer");
	check("allowed", status == 0 && last_opcode == KL_SYSTEM_NOTIFY_EVENT_POSTED && last_words[0] == 1U, "posted");

	/* Calendar off: DENIED. */
	status = post(object, 2U, "calendar");
	check("denied", status == 0 && last_opcode == KL_SYSTEM_NOTIFY_EVENT_RESULT && last_words[0] == 2U && last_words[1] == KL_SYSTEM_RESULT_DENIED,
	    "result DENIED");

	/* An application without a setting: posted. */
	status = post(object, 3U, "keiland-notify");
	check("no-setting", status == 0 && last_opcode == KL_SYSTEM_NOTIFY_EVENT_POSTED, "posted");

	/* No name: its window's app_id (browser, off) decides. */
	surface = kwl_create(client, 20U, KWL_SURFACE, 4U);
	surface->mapped = 1;
	surface->role = (void *)surface;
	(void)snprintf(surface->app_id, sizeof(surface->app_id), "browser");
	status = post(object, 4U, "");
	check("window-app-id", status == 0 && last_opcode == KL_SYSTEM_NOTIFY_EVENT_RESULT && last_words[1] == KL_SYSTEM_RESULT_DENIED, "DENIED by app_id");

	/* Turned on again: posted. */
	allow_browser = 1;
	status = post(object, 5U, "");
	check("turned-on", status == 0 && last_opcode == KL_SYSTEM_NOTIFY_EVENT_POSTED, "posted");

	/* The outcome. */
	printf("host-notify-allow: %s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}

/* Prints a check's outcome. */
static void
check(
	const char *name,
	int passed,
	const char *detail)
{
	/* PASS, or FAIL with why. */
	if (passed) {
		printf("PASS %s\n", name);
	} else {
		printf("FAIL %s: %s\n", name, detail);
		failures++;
	}
}

/* Sends post(request, 0, app, "Title", "Body", 0). */
static int
post(
	struct kwl_object *object,
	uint32_t request,
	const char *app)
{
	unsigned char bytes[256];
	size_t length;
	int status;

	/* The bytes. */
	length = put_word(bytes, 0, request);
	length = put_word(bytes, length, 0U);
	length = put_string(bytes, length, app);
	length = put_string(bytes, length, "Title");
	length = put_string(bytes, length, "Body");
	length = put_word(bytes, length, 0U);
	status = kwl_notify_request(object, KL_SYSTEM_NOTIFY_POST, bytes, length);
	return status;
}

/* Puts a word on the wire (the host's order). */
static size_t
put_word(
	unsigned char *bytes,
	size_t offset,
	uint32_t word)
{
	/* Four bytes. */
	memcpy(bytes + offset, &word, 4U);
	return offset + 4U;
}

/* Puts a string on the wire: its length with the NUL, its bytes, padded to a word. */
static size_t
put_string(
	unsigned char *bytes,
	size_t offset,
	const char *text)
{
	size_t length;
	size_t padded;

	/* The length, then the bytes and the padding. */
	length = strlen(text) + 1U;
	offset = put_word(bytes, offset, (uint32_t)length);
	padded = (length + 3U) & ~(size_t)3U;
	memset(bytes + offset, 0, padded);
	memcpy(bytes + offset, text, length);
	return offset + padded;
}
