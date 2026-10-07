/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of wl_surface.enter and leave (ws177-p001,
 * userland/desktop/wayland/surface-outputs.c), with stand-ins for the
 * wire, the plane's outputs and a window's output: a window mapped on the
 * anchor enters it once; moved to a head it enters the head before it
 * leaves the anchor; its sub-surface and its popup follow it; a window of
 * an output not shown is the anchor's; the mirror puts it on every display
 * shown; an unmapped window, a cursor, a surface of no role and one with
 * no image are on no output; a binding made later hears the surfaces
 * already on its display; a display that closes is left while its
 * bindings still name it; a dead binding, a binding of a closed display
 * and another client's binding hear nothing; a client gone fatal is passed
 * over.
 */

#include "userland/desktop/wayland/kwl.h"
#include "userland/desktop/wayland/compose.h"
#include "userland/desktop/wayland/displays.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The most events one step may send. */
#define TEST_EVENTS_MAX	32U

/* One event the compositor sent: to which client, on which object, which event, naming which binding. */
struct test_event {
	struct kwl_client *client;
	uint32_t object;
	uint32_t opcode;
	uint32_t binding;
};

/* The events sent since the last step was checked. */
static struct test_event test_events[TEST_EVENTS_MAX];
static unsigned test_event_count;

/* The outputs of the plane the stand-in reports: the anchor and the heads shown in the extended mode. */
static struct kwl_plane_rect test_outputs[KWL_PLANE_SLOTS];

/* The window a popup hangs from, as popup.c would find it. */
static struct kwl_object *test_popup_window;

/* The number of failed checks. */
static int test_failures;

static void check(int condition, const char *what);
static void test_reset(void);
static struct kwl_object *test_object(struct kwl_client *client, uint32_t id, enum kwl_kind kind);
static int test_sent(unsigned index, struct kwl_client *client, uint32_t object, uint32_t opcode, uint32_t binding);

/* Records an event instead of writing it to the client. */
int
kwl_emit(
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	struct test_event *event;

	/* The events here carry one object id. */
	check(size == sizeof(uint32_t), "an enter or a leave carries one word");
	if (test_event_count == TEST_EVENTS_MAX)
		return 0;

	/* Kept in order. */
	event = &test_events[test_event_count];
	event->client = client;
	event->object = object;
	event->opcode = opcode;
	memcpy(&event->binding, payload, sizeof(event->binding));
	test_event_count++;

	/* Succeeded: recorded. */
	return 0;
}

/* Stands in for heads.c: the test's outputs. */
unsigned
kwl_outputs(
	struct kwl_server *server,
	struct kwl_plane_rect *outputs)
{
	(void)server;

	/* The test's rectangles. */
	memcpy(outputs, test_outputs, sizeof(test_outputs));

	/* Succeeded: every slot. */
	return KWL_PLANE_SLOTS;
}

/* Stands in for heads.c: a sub-surface's root, a popup's window, and its output. */
unsigned
kwl_window_output(
	struct kwl_object *surface)
{
	struct kwl_object *root;

	/* Up the sub-surfaces. */
	root = surface;
	while (root->sub_parent != NULL)
		root = root->sub_parent;

	/* A popup is its window's. */
	if (root->role != NULL &&
	    root->role->top != NULL &&
	    root->role->top->kind == KWL_POPUP)
		root = test_popup_window;
	if (root == NULL)
		return KWL_PLANE_ANCHOR;

	/* Succeeded: the window's output. */
	return root->output;
}

/*
 * Runs the steps: a two-display extended desktop with one client that
 * binds both displays' wl_output, and a second client that binds the
 * anchor's.
 */
int
main(void)
{
	struct kwl_server server;
	struct kwl_compose *compose;
	struct kwl_client first;
	struct kwl_client second;
	struct kwl_object *anchor_binding;
	struct kwl_object *head_binding;
	struct kwl_object *late_binding;
	struct kwl_object *other_binding;
	struct kwl_object *window;
	struct kwl_object *window_role;
	struct kwl_object *window_top;
	struct kwl_object *child;
	struct kwl_object *popup;
	struct kwl_object *popup_role;
	struct kwl_object *popup_top;
	struct kwl_object *cursor;
	struct kwl_object *bare;
	struct kwl_object *other_window;
	struct kwl_object *other_role;
	struct kwl_object *other_top;
	struct kwl_object buffer;

	/* The server: an anchor of 1280x800 and head 1 of 1024x768 right of it, extended. */
	memset(&server, 0, sizeof(server));
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	memset(&buffer, 0, sizeof(buffer));
	compose = calloc(1, sizeof(*compose));
	if (compose == NULL)
		return 2;
	server.compose = compose;
	compose->display_mode = KWL_DISPLAYS_EXTENDED;
	compose->heads[0].open = 1U;
	compose->heads[0].global = 100U;
	test_outputs[0].width = 1280U;
	test_outputs[0].height = 800U;
	test_outputs[1].x = 1280;
	test_outputs[1].width = 1024U;
	test_outputs[1].height = 768U;

	/* Two clients. */
	server.clients = &first;
	first.next = &second;
	first.server = &server;
	first.number = 1U;
	second.server = &server;
	second.number = 2U;

	/* The first client's bindings: the anchor's (10) and the head's (11). */
	anchor_binding = test_object(&first, 10U, KWL_OUTPUT);
	anchor_binding->output_head = 0U;
	head_binding = test_object(&first, 11U, KWL_OUTPUT);
	head_binding->output_head = 1U;

	/* Its window (20), a sub-surface of it (21), a popup (22), a cursor (23) and a surface of no role (24). */
	window = test_object(&first, 20U, KWL_SURFACE);
	window_role = test_object(&first, 120U, KWL_XDG_SURFACE);
	window_top = test_object(&first, 220U, KWL_TOPLEVEL);
	window->role = window_role;
	window_role->top = window_top;
	child = test_object(&first, 21U, KWL_SURFACE);
	child->sub_parent = window;
	popup = test_object(&first, 22U, KWL_SURFACE);
	popup_role = test_object(&first, 122U, KWL_XDG_SURFACE);
	popup_top = test_object(&first, 222U, KWL_POPUP);
	popup->role = popup_role;
	popup_role->top = popup_top;
	test_popup_window = window;
	cursor = test_object(&first, 23U, KWL_SURFACE);
	cursor->cursor_role = 1U;
	cursor->current = &buffer;
	bare = test_object(&first, 24U, KWL_SURFACE);
	bare->current = &buffer;

	/* The second client: an anchor binding (10 of its own) and a window (30). */
	other_binding = test_object(&second, 10U, KWL_OUTPUT);
	other_binding->output_head = 0U;
	other_window = test_object(&second, 30U, KWL_SURFACE);
	other_role = test_object(&second, 130U, KWL_XDG_SURFACE);
	other_top = test_object(&second, 230U, KWL_TOPLEVEL);
	other_window->role = other_role;
	other_role->top = other_top;

	/* 1. Nothing shown: nothing told. */
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 0U, "no surface shown, no event");

	/* 2. The window mapped on the anchor enters it, the cursor and the bare surface stay out. */
	window->current = &buffer;
	window->mapped = 1U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 1U, "the mapped window is told once");
	check(test_sent(0U, &first, 20U, 0U, 10U), "it enters the anchor by its own client's anchor binding");
	check(window->outputs_entered == 1U, "it is on the anchor");
	check(cursor->outputs_entered == 0U, "a cursor is on no output");
	check(bare->outputs_entered == 0U, "a surface of no role is on no output");

	/* 3. Nothing changed: nothing told again. */
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 0U, "an unchanged pass tells nothing");

	/* 4. Its sub-surface and its popup, given images, follow it. */
	child->current = &buffer;
	popup->current = &buffer;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 2U, "the sub-surface and the popup are told");
	check(test_sent(0U, &first, 22U, 0U, 10U), "the popup enters the anchor");
	check(test_sent(1U, &first, 21U, 0U, 10U), "the sub-surface enters the anchor");

	/* 5. Moved to the head: each surface enters it before it leaves the anchor. */
	window->output = 1U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 6U, "three surfaces move");
	check(test_sent(0U, &first, 22U, 0U, 11U), "the popup enters the head");
	check(test_sent(1U, &first, 22U, 1U, 10U), "then it leaves the anchor");
	check(test_sent(2U, &first, 21U, 0U, 11U), "the sub-surface enters the head");
	check(test_sent(3U, &first, 21U, 1U, 10U), "then it leaves the anchor");
	check(test_sent(4U, &first, 20U, 0U, 11U), "the window enters the head");
	check(test_sent(5U, &first, 20U, 1U, 10U), "then it leaves the anchor");

	/* 6. A binding of the head made now hears of the surfaces on it, alone. */
	late_binding = test_object(&first, 12U, KWL_OUTPUT);
	late_binding->output_head = 1U;
	test_reset();
	kwl_surface_outputs_bound(late_binding);
	check(test_event_count == 3U, "the late binding hears the three surfaces");
	check(test_sent(0U, &first, 22U, 0U, 12U), "the popup names the late binding");
	check(test_sent(1U, &first, 21U, 0U, 12U), "the sub-surface names the late binding");
	check(test_sent(2U, &first, 20U, 0U, 12U), "the window names the late binding");
	test_reset();
	kwl_surface_outputs_bound(anchor_binding);
	check(test_event_count == 0U, "a binding of a display no surface is on hears nothing");

	/* 7. The head not shown (between its loss and the retreat): the window is the anchor's. */
	test_outputs[1].width = 0U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 9U, "three surfaces come back to the anchor");
	check(test_sent(6U, &first, 20U, 0U, 10U), "a window of an output not shown enters the anchor");
	check(test_sent(7U, &first, 20U, 1U, 12U), "and leaves the head by the late binding");
	check(test_sent(8U, &first, 20U, 1U, 11U), "and by the first binding");
	check(window->outputs_entered == 1U, "it is on the anchor");
	test_outputs[1].width = 1024U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(window->outputs_entered == 2U, "shown again, it is on the head again");

	/* 8. The head closes: evacuated to the anchor, its surfaces leave it while its bindings name it. */
	window->output = 0U;
	test_reset();
	kwl_surface_outputs_gone(&server, 1U);
	check(test_event_count == 9U, "three surfaces enter the anchor and leave both head bindings");
	check(test_sent(0U, &first, 22U, 0U, 10U), "the popup enters the anchor first");
	check(test_sent(1U, &first, 22U, 1U, 12U), "then leaves by the late binding");
	check(test_sent(2U, &first, 22U, 1U, 11U), "and by the first binding");
	check(window->outputs_entered == 1U, "the window is on the anchor");
	head_binding->output_head = KWL_OUTPUT_GONE;
	late_binding->output_head = KWL_OUTPUT_GONE;
	test_outputs[1].width = 0U;
	compose->heads[0].open = 0U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 0U, "nothing more after the close");

	/* 9. The mirror with a head open: every surface is on both displays. */
	compose->heads[0].open = 1U;
	compose->display_mode = KWL_DISPLAYS_MIRROR;
	head_binding->output_head = 1U;
	late_binding->dead = 1U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 3U, "the mirror adds the head, a dead binding hears nothing");
	check(test_sent(2U, &first, 20U, 0U, 11U), "the window enters the mirrored head");
	check(window->outputs_entered == 3U, "the window is on both displays");
	compose->heads[0].lost = 1U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_sent(2U, &first, 20U, 1U, 11U), "a lost head is left");
	compose->heads[0].lost = 0U;
	compose->display_mode = KWL_DISPLAYS_EXTENDED;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 0U, "back to the extended mode on the anchor: nothing told");

	/* 10. The second client's window: told by its own binding alone. */
	other_window->current = &buffer;
	other_window->mapped = 1U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 1U, "the second client's window is told once");
	check(test_sent(0U, &second, 30U, 0U, 10U), "by the second client's binding");

	/* 11. Unmapped: it leaves; the sub-surface goes with it; the popup's image goes too. */
	window->current = NULL;
	window->mapped = 0U;
	popup->current = NULL;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 3U, "the unmapped window, its sub-surface and the popup leave");
	check(test_sent(0U, &first, 22U, 1U, 10U), "the popup leaves");
	check(test_sent(1U, &first, 21U, 1U, 10U), "the sub-surface leaves with its window");
	check(test_sent(2U, &first, 20U, 1U, 10U), "the window leaves the anchor");

	/* 12. A client gone fatal is passed over. */
	second.fatal = 1U;
	other_window->current = NULL;
	other_window->mapped = 0U;
	test_reset();
	kwl_surface_outputs_sync(&server);
	check(test_event_count == 0U, "a fatal client hears nothing");

	/* Freed. */
	while (first.objects != NULL) {
		window = first.objects;
		first.objects = window->next;
		free(window);
	}
	while (second.objects != NULL) {
		window = second.objects;
		second.objects = window->next;
		free(window);
	}

	/* The compositor's state. */
	free(compose);

	/* The verdict. */
	if (test_failures != 0) {
		printf("host-surface-outputs: %d FAILED\n", test_failures);
		return 1;
	}

	/* Succeeded: every step held. */
	printf("host-surface-outputs: PASS\n");
	return 0;
}

/* Counts and reports a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* A failure is named. */
	if (!condition) {
		printf("FAIL: %s\n", what);
		test_failures++;
	}
}

/* Forgets the events of the last step. */
static void
test_reset(void)
{
	/* No event yet. */
	memset(test_events, 0, sizeof(test_events));
	test_event_count = 0U;
}

/* Makes a client's object, first in its list as kwl_create puts it. */
static struct kwl_object *
test_object(
	struct kwl_client *client,
	uint32_t id,
	enum kwl_kind kind)
{
	struct kwl_object *object;

	/* A zeroed object. */
	object = calloc(1, sizeof(*object));
	if (object == NULL) {
		printf("FAIL: out of memory\n");
		exit(2);
	}

	/* Its identity, at the head of the list. */
	object->client = client;
	object->id = id;
	object->kind = kind;
	object->version = 4U;
	object->next = client->objects;
	client->objects = object;

	/* Succeeded: the object. */
	return object;
}

/* Tells whether the event at an index is the one expected. */
static int
test_sent(
	unsigned index,
	struct kwl_client *client,
	uint32_t object,
	uint32_t opcode,
	uint32_t binding)
{
	const struct test_event *event;

	/* An index past the events is not sent. */
	if (index >= test_event_count)
		return 0;
	event = &test_events[index];

	/* Each field must match. */
	if (event->client != client)
		return 0;
	if (event->object != object)
		return 0;
	if (event->opcode != opcode)
		return 0;
	if (event->binding != binding)
		return 0;

	/* Succeeded: the event is the one expected. */
	return 1;
}
