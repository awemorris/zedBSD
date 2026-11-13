/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The server side of the Titlebar Presentation protocol (WS070 p008,
 * plan/ws070/titlebar-design.md section 2): kl_titlebar_manager_v1 and
 * kl_titlebar_v1.
 *
 * A client gives one window's titlebar a mode (menu, controls or tabs) and
 * the models of controls and tabs, in transactions as it builds menus:
 * begin_update copies the state shown, the changes go to the copy, and
 * commit shows the copy at once.  Both models may exist whatever the mode;
 * only the mode's is drawn (titlebar-shell.c).  What the user does with a
 * control or a tab comes back as kl_titlebar_v1's events.
 */

#include "extras.h"
#include "titlebar.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The highest role before version 6 and since, priority, mode, tab flags,
 * tab strip options and focus mode.
 */
#define TITLEBAR_ROLE_LAST		16U
#define TITLEBAR_ROLE_LAST_RELOAD	KWL_CONTROL_RELOAD
#define TITLEBAR_PRIORITY_LAST		2U
#define TITLEBAR_MODE_LAST		3U
#define TITLEBAR_FLAGS_ALL		7U
#define TITLEBAR_OPTIONS_ALL		1U
#define TITLEBAR_FOCUS_EDIT		1U

/* A progress control's value: thousandths, and the one that says the share is not known. */
#define TITLEBAR_VALUE_UNKNOWN		1001U

/* The longest event: a control, a text of the longest, and a word after it. */
#define TITLEBAR_EVENT_MAX		(12U + KWL_TITLEBAR_TEXT_MAX + 4U)

/* kl_titlebar_manager_v1's requests and error. */
#define MANAGER_DESTROY			0U
#define MANAGER_GET_TITLEBAR		1U
#define MANAGER_ERROR_ALREADY_EXISTS	0U

/* kl_titlebar_v1's requests. */
#define REQUEST_DESTROY			0U
#define REQUEST_BEGIN_UPDATE		1U
#define REQUEST_COMMIT			2U
#define REQUEST_SET_MODE		3U
#define REQUEST_ADD_CONTROL		4U
#define REQUEST_REMOVE_CONTROL		5U
#define REQUEST_SET_CONTROL_LABEL	6U
#define REQUEST_SET_CONTROL_STATE	7U
#define REQUEST_SET_CONTROL_VALUE	8U
#define REQUEST_SET_CONTROL_TEXT	9U
#define REQUEST_SET_BREADCRUMB		10U
#define REQUEST_ADD_TAB			11U
#define REQUEST_REMOVE_TAB		12U
#define REQUEST_SET_TAB			13U
#define REQUEST_SET_TABS_OPTIONS	14U
#define REQUEST_FOCUS_CONTROL		15U
#define REQUEST_SET_SUGGESTIONS		16U

/* kl_titlebar_v1's errors. */
#define ERROR_INVALID_ID		0U
#define ERROR_INVALID_VALUE		1U
#define ERROR_NOT_UPDATING		2U
#define ERROR_ALREADY_UPDATING		3U
#define ERROR_BAD_SERIAL		4U
#define ERROR_TOO_LARGE			5U

/* kl_titlebar_v1's events. */
#define EVENT_CONTROL_ACTIVATED		0U
#define EVENT_TEXT_CHANGED		1U
#define EVENT_TEXT_DONE			2U
#define EVENT_TAB_ACTIVATED		3U
#define EVENT_TAB_CLOSE_REQUESTED	4U
#define EVENT_NEW_TAB_REQUESTED		5U
#define EVENT_OVERFLOW_MENU_OPENED	6U
#define EVENT_DROP_TARGET		7U

/* The version that has drop_target. */
#define TITLEBAR_DROP_VERSION		2U

/* The version that has set_suggestions (ws127-p010). */
#define TITLEBAR_SUGGEST_VERSION	4U

/* The version whose breadcrumb takes a value: KWL_BREADCRUMB_BOXED (ws074-p229, the browser's address). */
#define TITLEBAR_BOXED_VERSION		5U

/* The version that has the reload role (KWL_CONTROL_RELOAD). */
#define TITLEBAR_RELOAD_VERSION		6U

/* The version whose box takes its load: KWL_BREADCRUMB_PROGRESS_MASK. */
#define TITLEBAR_PROGRESS_VERSION	7U

static uint32_t titlebar_word(const unsigned char *bytes, size_t offset);
static int titlebar_string(const unsigned char *bytes, size_t size, size_t offset, const char **text, size_t *next);
static int manager_request(struct kwl_object *manager, uint32_t opcode, const unsigned char *bytes, size_t size);
static int titlebar_request(struct kwl_object *titlebar, uint32_t opcode, const unsigned char *bytes, size_t size);
static int titlebar_begin(struct kwl_object *titlebar, uint32_t serial);
static int titlebar_commit(struct kwl_object *titlebar, uint32_t serial);
static int titlebar_edit(struct kwl_object *titlebar, uint32_t opcode, const unsigned char *bytes, size_t size);
static int titlebar_edit_control(struct kwl_object *titlebar, uint32_t opcode, const unsigned char *bytes, size_t size);
static int titlebar_edit_tab(struct kwl_object *titlebar, uint32_t opcode, const unsigned char *bytes, size_t size);
static int titlebar_add_control(struct kwl_object *titlebar, const unsigned char *bytes, size_t size);
static int titlebar_set_text(struct kwl_object *titlebar, const unsigned char *bytes, size_t size);
static int titlebar_set_breadcrumb(struct kwl_object *titlebar, const unsigned char *bytes, size_t size);
static int titlebar_focus(struct kwl_object *titlebar, const unsigned char *bytes, size_t size);
static int titlebar_suggest(struct kwl_object *titlebar, const unsigned char *bytes, size_t size);
static int titlebar_fail(struct kwl_object *titlebar, uint32_t code, const char *reason);
static struct kwl_titlebar_control *titlebar_find_control(struct kwl_titlebar_state *state, uint32_t id, unsigned *index);
static struct kwl_titlebar_tab *titlebar_find_tab(struct kwl_titlebar_state *state, uint32_t id, unsigned *index);
static int titlebar_replace(char **field, const char *text);
static int titlebar_state_copy(struct kwl_titlebar_state *target, const struct kwl_titlebar_state *source);
static void titlebar_state_free(struct kwl_titlebar_state *state);
static void titlebar_control_free(struct kwl_titlebar_control *control);
static char *titlebar_text_copy(const char *text);
static size_t titlebar_put_string(unsigned char *payload, size_t offset, const char *text);
static uint32_t titlebar_seat(struct kwl_object *titlebar);
static int titlebar_live(struct kwl_object *titlebar);
static void titlebar_emit(struct kwl_object *titlebar, uint32_t opcode, const void *payload, size_t size);

/*
 * Carries out a request of a Titlebar Presentation object.
 *
 * Returns 0, or a nonzero value after sending the protocol error (the
 * dispatcher then ends the client).
 */
int
kwl_titlebar_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* Each interface has its own requests. */
	switch (object->kind) {
	case KWL_TITLEBAR_MANAGER:
		error = manager_request(object, opcode, bytes, size);
		break;
	case KWL_TITLEBAR:
		error = titlebar_request(object, opcode, bytes, size);
		break;
	default:
		error = EPROTO;
		break;
	}

	/* Reports a request that was refused. */
	if (error != 0)
		return error;

	/* Succeeded: the request was carried out. */
	return 0;
}

/*
 * Lets the Titlebar Presentation's objects stop naming one that goes.
 *
 * A toplevel may go before its titlebar (the titlebar is then left without
 * a window and ignores what it is told); a titlebar that goes takes its
 * model with it and its window shows its menu again.
 */
void
kwl_titlebar_object_gone(
	struct kwl_object *object)
{
	struct kwl_titlebar_model *model;
	int error;

	/* The presentation forgets it: a press, a field and its places (titlebar-shell.c). */
	kwl_titlebar_forget(object->client->server, object);

	/* A window's titlebar is left without the window. */
	if (object->kind == KWL_TOPLEVEL) {
		if (object->titlebar != NULL)
			object->titlebar->top = NULL;
		object->titlebar = NULL;
		return;
	}

	/* Nothing else is tied to anything but a titlebar. */
	if (object->kind != KWL_TITLEBAR)
		return;

	/* The window stops naming it. */
	if (object->top != NULL) {
		object->top->titlebar = NULL;

		/* Withdraws native decoration ownership with the surviving surface's next commit. */
		error = kwl_decoration_native_changed(object->top);
		if (error != 0) {
			/* Defers connection teardown until the dispatcher can report failure. */
			kwl_client_fail(object->client);
		}
	}

	/* Removes the retiring titlebar's reciprocal toplevel link. */
	object->top = NULL;

	/* The model goes with its object, and the windows are drawn without it. */
	model = object->titlebar_model;
	if (model != NULL) {
		titlebar_state_free(&model->shown);
		titlebar_state_free(&model->pending);
		free(model);
	}

	/* The object keeps no model. */
	object->titlebar_model = NULL;
	object->client->server->dirty = 1;
}

/*
 * Finds the titlebar model a window's surface shows, and the
 * kl_titlebar_v1 its events go to; NULL when the window has none.
 */
struct kwl_titlebar_model *
kwl_titlebar_of_surface(
	struct kwl_object *surface,
	struct kwl_object **titlebar)
{
	struct kwl_object *toplevel;

	/* Nothing is found until the whole chain is. */
	*titlebar = NULL;

	/* The surface's xdg_surface and its toplevel. */
	if (surface == NULL || surface->dead)
		return NULL;
	if (surface->role == NULL)
		return NULL;
	toplevel = surface->role->top;
	if (toplevel == NULL || toplevel->dead)
		return NULL;

	/* The toplevel's titlebar and its model. */
	if (toplevel->titlebar == NULL || toplevel->titlebar->dead)
		return NULL;
	if (toplevel->titlebar->titlebar_model == NULL)
		return NULL;

	/* Succeeded: the model and where its events go. */
	*titlebar = toplevel->titlebar;
	return toplevel->titlebar->titlebar_model;
}

/*
 * Finds a control of a state by its ID; NULL when the state has none.
 */
const struct kwl_titlebar_control *
kwl_titlebar_control(
	const struct kwl_titlebar_state *state,
	uint32_t id)
{
	unsigned index;

	/* A state has at most KWL_TITLEBAR_CONTROLS_MAX controls, so a search is cheap. */
	for (index = 0; index < state->control_count; index++) {
		if (state->controls[index].id == id)
			return &state->controls[index];
	}

	/* No control has the ID. */
	return NULL;
}

/*
 * Tells a window's client that the user chose a control: its ID, a detail
 * (a breadcrumb's part, 0 otherwise), the client's seat and a new serial.
 */
void
kwl_titlebar_send_activated(
	struct kwl_object *titlebar,
	uint32_t id,
	uint32_t detail,
	const char *via)
{
	uint32_t words[4];
	int live;

	/* A titlebar whose client has failed hears nothing. */
	live = titlebar_live(titlebar);
	if (live == 0)
		return;

	/* The control, the detail, the seat (or none) and the serial. */
	words[0] = id;
	words[1] = detail;
	words[2] = titlebar_seat(titlebar);
	words[3] = kwl_next_serial(titlebar->client->server);
	titlebar_emit(titlebar, EVENT_CONTROL_ACTIVATED, words, sizeof(words));

	/* The log line the tests read. */
	printf("KWL TITLEBAR activate client=%llu id=%u detail=%u serial=%u via=%s\n",
	       (unsigned long long)titlebar->client->number, id, detail, words[3], via);
}

/*
 * Tells a window's client that a text control's text changed (done zero),
 * or that its editing ended and how (done nonzero).
 */
void
kwl_titlebar_send_text(
	struct kwl_object *titlebar,
	uint32_t id,
	const char *text,
	int done,
	uint32_t how)
{
	unsigned char payload[TITLEBAR_EVENT_MAX];
	size_t offset;
	uint32_t opcode;
	int live;

	/* A titlebar whose client has failed hears nothing. */
	live = titlebar_live(titlebar);
	if (live == 0)
		return;

	/* The control, then the text. */
	memcpy(payload, &id, sizeof(id));
	offset = titlebar_put_string(payload, 4U, text);

	/* text_done ends with how; text_changed ends there. */
	opcode = EVENT_TEXT_CHANGED;
	if (done != 0) {
		opcode = EVENT_TEXT_DONE;
		memcpy(payload + offset, &how, sizeof(how));
		offset += 4U;
	}

	/* The event. */
	titlebar_emit(titlebar, opcode, payload, offset);

	/* The log line the tests read (only the length: the text may be anything the user typed). */
	printf("KWL TITLEBAR text client=%llu id=%u done=%d how=%u length=%lu\n",
	       (unsigned long long)titlebar->client->number, id, done, how, (unsigned long)strlen(text));
}

/*
 * Tells a window's client what the user did to a tab (KWL_TAB_EVENT_*):
 * chose it, pressed its close button, or pressed the new-tab button.
 */
void
kwl_titlebar_send_tab(
	struct kwl_object *titlebar,
	uint32_t id,
	unsigned event)
{
	static const char *const names[] = { "activated", "close", "new" };
	uint32_t words[2];
	int live;

	/* A titlebar whose client has failed hears nothing. */
	live = titlebar_live(titlebar);
	if (live == 0)
		return;

	/* Each event has its own words: a tab and a serial, a tab, or a serial. */
	if (event == KWL_TAB_EVENT_ACTIVATED) {
		words[0] = id;
		words[1] = kwl_next_serial(titlebar->client->server);
		titlebar_emit(titlebar, EVENT_TAB_ACTIVATED, words, sizeof(words));
	} else if (event == KWL_TAB_EVENT_CLOSE) {
		words[0] = id;
		titlebar_emit(titlebar, EVENT_TAB_CLOSE_REQUESTED, words, sizeof(words[0]));
	} else {
		words[0] = kwl_next_serial(titlebar->client->server);
		titlebar_emit(titlebar, EVENT_NEW_TAB_REQUESTED, words, sizeof(words[0]));
	}

	/* The log line the tests read. */
	printf("KWL TITLEBAR tab client=%llu id=%u event=%s\n", (unsigned long long)titlebar->client->number, id, names[event]);
}

/*
 * Tells a window's client that the overflow popup opened (a chance to bring
 * its menu up to date).
 */
void
kwl_titlebar_send_overflow(
	struct kwl_object *titlebar)
{
	int live;

	/* A titlebar whose client has failed hears nothing. */
	live = titlebar_live(titlebar);
	if (live == 0)
		return;

	/* The event has no arguments. */
	titlebar_emit(titlebar, EVENT_OVERFLOW_MENU_OPENED, NULL, 0U);
}

/*
 * Tells a window's client the part of a breadcrumb a drag and drop is over
 * (id 0: none now), before the drag's enter or motion (data.c).  A
 * titlebar before version 2 is not told.
 */
void
kwl_titlebar_send_drop_target(
	struct kwl_object *titlebar,
	uint32_t id,
	uint32_t detail)
{
	uint32_t words[2];
	int live;

	/* A titlebar whose client has failed, or that is too old for it, hears nothing. */
	live = titlebar_live(titlebar);
	if (live == 0 || titlebar->version < TITLEBAR_DROP_VERSION)
		return;

	/* The control and its part. */
	words[0] = id;
	words[1] = detail;
	titlebar_emit(titlebar, EVENT_DROP_TARGET, words, sizeof(words));

	/* The log line the tests read. */
	printf("KWL TITLEBAR drop_target client=%llu id=%u detail=%u\n", (unsigned long long)titlebar->client->number, id, detail);
}

/* Reads one native-endian protocol word the caller has checked is there. */
static uint32_t
titlebar_word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* The payload need not be aligned. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: the word. */
	return word;
}

/* Reads one non-null string argument and where the next argument starts; EPROTO when it is malformed. */
static int
titlebar_string(
	const unsigned char *bytes,
	size_t size,
	size_t offset,
	const char **text,
	size_t *next)
{
	uint32_t length;
	size_t aligned;
	size_t actual;

	/* The length word must be in the payload. */
	if (offset > size || size - offset < 4U)
		return EPROTO;

	/* A non-null string has at least its terminator, within the payload. */
	length = titlebar_word(bytes, offset);
	if (length == 0U || length > size - offset - 4U)
		return EPROTO;

	/* Its storage is padded to a word, which must be in the payload too. */
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned > size - offset - 4U)
		return EPROTO;

	/* It ends with its terminator. */
	*text = (const char *)bytes + offset + 4U;
	if ((*text)[length - 1U] != '\0')
		return EPROTO;

	/* And has no other NUL, which could hide bytes. */
	actual = strlen(*text);
	if (actual + 1U != length)
		return EPROTO;

	/* Succeeded: the next argument follows the padded string. */
	*next = offset + 4U + aligned;
	return 0;
}

/* Carries out a request of kl_titlebar_manager_v1. */
static int
manager_request(
	struct kwl_object *manager,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *created;
	struct kwl_object *toplevel;
	uint32_t id;
	int error;

	/* The binding goes; its titlebars stay. */
	if (opcode == MANAGER_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(manager);
		return 0;
	}

	/* Only get_titlebar is left. */
	if (opcode != MANAGER_GET_TITLEBAR || size != 8U)
		return EPROTO;

	/* The window must be one of the client's toplevels. */
	toplevel = kwl_find(manager->client, titlebar_word(bytes, 4U));
	if (toplevel == NULL || toplevel->kind != KWL_TOPLEVEL)
		return EPROTO;

	/* A window has one titlebar at a time. */
	if (toplevel->titlebar != NULL) {
		error = kwl_error_code(manager->client, manager->id, MANAGER_ERROR_ALREADY_EXISTS, "the toplevel already has a kl_titlebar_v1");
		return error;
	}

	/* The titlebar object. */
	id = titlebar_word(bytes, 0U);
	created = kwl_create(manager->client, id, KWL_TITLEBAR, manager->version);
	if (created == NULL)
		return EPROTO;

	/* Its model, empty (menu mode, no controls, no tabs). */
	created->titlebar_model = calloc(1, sizeof(*created->titlebar_model));
	if (created->titlebar_model == NULL) {
		kwl_object_destroy(created);
		return EPROTO;
	}

	/* Tied to the window from both ends. */
	created->top = toplevel;
	toplevel->titlebar = created;
	printf("KWL TITLEBAR create client=%llu titlebar=%u toplevel=%u\n", (unsigned long long)manager->client->number, id, toplevel->id);

	/* Native titlebar creation explicitly requests compositor decoration ownership. */
	error = kwl_decoration_native_changed(toplevel);
	if (error != 0)
		return error;

	/* Succeeded: the window has a titlebar presentation. */
	return 0;
}

/* Carries out a request of kl_titlebar_v1. */
static int
titlebar_request(
	struct kwl_object *titlebar,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_model *model;
	int error;

	/* The titlebar goes; its window shows its menu again (kwl_titlebar_object_gone). */
	if (opcode == REQUEST_DESTROY) {
		if (size != 0U)
			return EPROTO;
		kwl_object_destroy(titlebar);
		return 0;
	}

	/* A transaction starts. */
	if (opcode == REQUEST_BEGIN_UPDATE) {
		if (size != 4U)
			return EPROTO;
		error = titlebar_begin(titlebar, titlebar_word(bytes, 0U));
		return error;
	}

	/* A transaction is shown. */
	if (opcode == REQUEST_COMMIT) {
		if (size != 4U)
			return EPROTO;
		error = titlebar_commit(titlebar, titlebar_word(bytes, 0U));
		return error;
	}

	/* The keyboard asked for a control, outside the transactions. */
	if (opcode == REQUEST_FOCUS_CONTROL) {
		error = titlebar_focus(titlebar, bytes, size);
		return error;
	}

	/* A text field's suggestions, outside the transactions (since version 4, ws127-p010). */
	if (opcode == REQUEST_SET_SUGGESTIONS) {
		if (titlebar->version < TITLEBAR_SUGGEST_VERSION)
			return EPROTO;
		error = titlebar_suggest(titlebar, bytes, size);
		return error;
	}

	/* Every other request changes the model, which only a transaction may. */
	if (opcode > REQUEST_FOCUS_CONTROL)
		return EPROTO;
	model = titlebar->titlebar_model;
	if (model->updating == 0U) {
		error = titlebar_fail(titlebar, ERROR_NOT_UPDATING, "a change outside begin_update and commit");
		return error;
	}

	/* The change itself. */
	error = titlebar_edit(titlebar, opcode, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the change waits for the commit. */
	return 0;
}

/* Opens a transaction: the state shown is copied, and the changes go to the copy. */
static int
titlebar_begin(
	struct kwl_object *titlebar,
	uint32_t serial)
{
	struct kwl_titlebar_model *model;
	int error;

	/* One transaction at a time. */
	model = titlebar->titlebar_model;
	if (model->updating != 0U) {
		error = titlebar_fail(titlebar, ERROR_ALREADY_UPDATING, "begin_update inside a transaction");
		return error;
	}

	/* The copy the changes go to. */
	error = titlebar_state_copy(&model->pending, &model->shown);
	if (error != 0)
		return EPROTO;

	/* The transaction is open under its serial. */
	model->updating = 1;
	model->update_serial = serial;

	/* Succeeded: the changes that follow wait for the commit. */
	return 0;
}

/* Shows a transaction: the copy takes the place of the state shown. */
static int
titlebar_commit(
	struct kwl_object *titlebar,
	uint32_t serial)
{
	struct kwl_titlebar_model *model;
	int error;

	/* Only an open transaction, under the serial it was opened with. */
	model = titlebar->titlebar_model;
	if (model->updating == 0U) {
		error = titlebar_fail(titlebar, ERROR_NOT_UPDATING, "commit without begin_update");
		return error;
	}

	/* The serial pairs the commit with its begin_update. */
	if (serial != model->update_serial) {
		error = titlebar_fail(titlebar, ERROR_BAD_SERIAL, "commit names another serial than begin_update");
		return error;
	}

	/* The old state goes and the copy takes its place (the copy's strings move with it). */
	titlebar_state_free(&model->shown);
	model->shown = model->pending;
	memset(&model->pending, 0, sizeof(model->pending));

	/*
	 * The generation tells the presentation that the model changed; the
	 * titlebar is drawn again from it on the next pass.
	 */
	model->updating = 0;
	model->generation++;
	titlebar->client->server->dirty = 1;

	/* Succeeded: the log line the tests read. */
	printf("KWL TITLEBAR commit client=%llu titlebar=%u serial=%u mode=%u controls=%u tabs=%u generation=%llu\n",
	       (unsigned long long)titlebar->client->number, titlebar->id, serial, model->shown.mode,
	       model->shown.control_count, model->shown.tab_count, (unsigned long long)model->generation);
	return 0;
}

/* Decodes one change in a transaction and applies it to the pending state. */
static int
titlebar_edit(
	struct kwl_object *titlebar,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_state *state;
	uint32_t value;
	int error;

	/* The mode and the tab strip's options are one word each. */
	state = &titlebar->titlebar_model->pending;
	if (opcode == REQUEST_SET_MODE || opcode == REQUEST_SET_TABS_OPTIONS) {
		if (size != 4U)
			return EPROTO;
		value = titlebar_word(bytes, 0U);

		/* The mode, one of the four (the sheet's, ws090-p014, from version 3). */
		if (opcode == REQUEST_SET_MODE) {
			if (value > TITLEBAR_MODE_LAST) {
				error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "an unknown mode");
				return error;
			}

			/* The mode the next commit shows. */
			state->mode = value;
			return 0;
		}

		/* The options, of the bits known. */
		if ((value & ~TITLEBAR_OPTIONS_ALL) != 0U) {
			error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "unknown tab strip options");
			return error;
		}

		/* The options the next commit shows. */
		state->options = value;
		return 0;
	}

	/* The tabs' requests. */
	if (opcode >= REQUEST_ADD_TAB && opcode <= REQUEST_SET_TAB) {
		error = titlebar_edit_tab(titlebar, opcode, bytes, size);
		return error;
	}

	/* The rest are the controls'. */
	error = titlebar_edit_control(titlebar, opcode, bytes, size);
	if (error != 0)
		return error;

	/* Succeeded: the change is in the pending state. */
	return 0;
}

/* Applies a change of a control to the pending state. */
static int
titlebar_edit_control(
	struct kwl_object *titlebar,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_control *control;
	struct kwl_titlebar_state *state;
	const char *text;
	unsigned index;
	size_t length;
	size_t next;
	uint32_t enabled;
	uint32_t checked;
	uint32_t value;
	int error;

	/* Adding, changing text and a breadcrumb's parts have their own decoding. */
	if (opcode == REQUEST_ADD_CONTROL) {
		error = titlebar_add_control(titlebar, bytes, size);
		return error;
	}

	/* A text control's texts. */
	if (opcode == REQUEST_SET_CONTROL_TEXT) {
		error = titlebar_set_text(titlebar, bytes, size);
		return error;
	}

	/* A breadcrumb's parts. */
	if (opcode == REQUEST_SET_BREADCRUMB) {
		error = titlebar_set_breadcrumb(titlebar, bytes, size);
		return error;
	}

	/* The others start with the control, which must be there. */
	if (size < 4U)
		return EPROTO;
	state = &titlebar->titlebar_model->pending;
	control = titlebar_find_control(state, titlebar_word(bytes, 0U), &index);
	if (control == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no control has the ID");
		return error;
	}

	/* Each change. */
	switch (opcode) {
	case REQUEST_REMOVE_CONTROL:
		/* The control goes, and the ones after it close up. */
		if (size != 4U)
			return EPROTO;
		titlebar_control_free(control);
		memmove(&state->controls[index], &state->controls[index + 1U], (state->control_count - index - 1U) * sizeof(state->controls[0]));
		state->control_count--;
		return 0;
	case REQUEST_SET_CONTROL_LABEL:
		/* The label, not longer than a string may be. */
		error = titlebar_string(bytes, size, 4U, &text, &next);
		if (error != 0 || next != size)
			return EPROTO;
		length = strlen(text);
		if (length > KWL_TITLEBAR_TEXT_MAX) {
			error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "a label longer than 1023 bytes");
			return error;
		}

		/* The new label. */
		error = titlebar_replace(&control->label, text);
		return error;
	case REQUEST_SET_CONTROL_STATE:
		/* Enabled and checked, each 0 or 1. */
		if (size != 12U)
			return EPROTO;
		enabled = titlebar_word(bytes, 4U);
		checked = titlebar_word(bytes, 8U);
		if (enabled > 1U || checked > 1U) {
			error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "a state that is not 0 or 1");
			return error;
		}

		/* The new state. */
		control->enabled = enabled;
		control->checked = checked;
		return 0;
	case REQUEST_SET_CONTROL_VALUE:
		/* A progress control's thousandths, or the unknown share. */
		if (size != 8U)
			return EPROTO;
		value = titlebar_word(bytes, 4U);

		/* A breadcrumb's look since version 5: 0, or always a box (KWL_BREADCRUMB_BOXED), with its load since version 7. */
		if (control->role == KWL_CONTROL_BREADCRUMB && titlebar->version >= TITLEBAR_BOXED_VERSION) {
			if (value > KWL_BREADCRUMB_BOXED &&
			    (titlebar->version < TITLEBAR_PROGRESS_VERSION ||
			     (value & ~(KWL_BREADCRUMB_BOXED | KWL_BREADCRUMB_PROGRESS_MASK)) != 0U ||
			     (value & KWL_BREADCRUMB_PROGRESS_MASK) > (1001U << KWL_BREADCRUMB_PROGRESS_SHIFT))) {
				error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "a breadcrumb's value that is not 0, 1 or a box's load");
				return error;
			}

			/* The breadcrumb's new look. */
			control->value = value;
			return 0;
		}

		/* Otherwise only a progress control has a value. */
		if (control->role != KWL_CONTROL_PROGRESS || value > TITLEBAR_VALUE_UNKNOWN) {
			error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "a value for a control that is not progress, or above 1001");
			return error;
		}

		/* The new value. */
		control->value = value;
		return 0;
	default:
		break;
	}

	/* No other request is a control's. */
	return EPROTO;
}

/* Applies a change of a tab to the pending state. */
static int
titlebar_edit_tab(
	struct kwl_object *titlebar,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_state *state;
	struct kwl_titlebar_tab *tab;
	const char *title;
	unsigned index;
	size_t length;
	size_t next;
	uint32_t flags;
	uint32_t id;
	int error;

	/* Each starts with the tab's ID, which may not be 0. */
	if (size < 4U)
		return EPROTO;
	state = &titlebar->titlebar_model->pending;
	id = titlebar_word(bytes, 0U);
	tab = titlebar_find_tab(state, id, &index);

	/* A new tab: a new ID, a title, and room for it. */
	if (opcode == REQUEST_ADD_TAB) {
		error = titlebar_string(bytes, size, 4U, &title, &next);
		if (error != 0 || next != size)
			return EPROTO;
		if (id == 0U || tab != NULL) {
			error = titlebar_fail(titlebar, ERROR_INVALID_ID, "a tab ID that is 0 or taken");
			return error;
		}

		/* Room for it. */
		length = strlen(title);
		if (state->tab_count == KWL_TITLEBAR_TABS_MAX || length > KWL_TITLEBAR_TEXT_MAX) {
			error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "more than 128 tabs, or a title longer than 1023 bytes");
			return error;
		}

		/* The tab at the end, closable and not active until told. */
		tab = &state->tabs[state->tab_count];
		memset(tab, 0, sizeof(*tab));
		tab->id = id;
		tab->flags = KWL_TAB_CLOSABLE;
		tab->title = titlebar_text_copy(title);
		if (tab->title == NULL)
			return EPROTO;
		state->tab_count++;
		return 0;
	}

	/* The others name a tab that is there. */
	if (tab == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no tab has the ID");
		return error;
	}

	/* A tab goes, and the ones after it close up. */
	if (opcode == REQUEST_REMOVE_TAB) {
		if (size != 4U)
			return EPROTO;
		free(tab->title);
		memmove(&state->tabs[index], &state->tabs[index + 1U], (state->tab_count - index - 1U) * sizeof(state->tabs[0]));
		state->tab_count--;
		return 0;
	}

	/* set_tab: a title and the flags known. */
	error = titlebar_string(bytes, size, 4U, &title, &next);
	if (error != 0 || next + 4U != size)
		return EPROTO;
	flags = titlebar_word(bytes, next);
	if ((flags & ~TITLEBAR_FLAGS_ALL) != 0U) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "unknown tab flags");
		return error;
	}

	/* A title not longer than a string may be. */
	length = strlen(title);
	if (length > KWL_TITLEBAR_TEXT_MAX) {
		error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "a title longer than 1023 bytes");
		return error;
	}

	/* The tab's new title and flags. */
	tab->flags = flags;
	error = titlebar_replace(&tab->title, title);
	if (error != 0)
		return error;

	/* Succeeded: the tab changed. */
	return 0;
}

/* Adds a control at the end of the pending state (add_control: id, role, priority, group, label). */
static int
titlebar_add_control(
	struct kwl_object *titlebar,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_control *control;
	struct kwl_titlebar_state *state;
	const char *label;
	unsigned index;
	size_t length;
	size_t next;
	uint32_t id;
	uint32_t role;
	uint32_t priority;
	uint32_t last;
	int error;

	/* The arguments. */
	if (size < 16U)
		return EPROTO;
	error = titlebar_string(bytes, size, 16U, &label, &next);
	if (error != 0 || next != size)
		return EPROTO;
	id = titlebar_word(bytes, 0U);
	role = titlebar_word(bytes, 4U);
	priority = titlebar_word(bytes, 8U);

	/* A new ID, which may not be 0. */
	state = &titlebar->titlebar_model->pending;
	control = titlebar_find_control(state, id, &index);
	if (id == 0U || control != NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "a control ID that is 0 or taken");
		return error;
	}

	/* A role the client's version knows, and a known priority. */
	last = TITLEBAR_ROLE_LAST;
	if (titlebar->version >= TITLEBAR_RELOAD_VERSION)
		last = TITLEBAR_ROLE_LAST_RELOAD;
	if (role == 0U || role > last || priority > TITLEBAR_PRIORITY_LAST) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "an unknown role or priority");
		return error;
	}

	/* Room for it. */
	length = strlen(label);
	if (state->control_count == KWL_TITLEBAR_CONTROLS_MAX || length > KWL_TITLEBAR_TEXT_MAX) {
		error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "more than 64 controls, or a label longer than 1023 bytes");
		return error;
	}

	/* The control at the end, enabled, unchecked, with empty texts. */
	control = &state->controls[state->control_count];
	memset(control, 0, sizeof(*control));
	control->id = id;
	control->role = role;
	control->priority = priority;
	control->group = titlebar_word(bytes, 12U);
	control->enabled = 1;
	control->label = titlebar_text_copy(label);
	if (control->label == NULL)
		return EPROTO;
	state->control_count++;

	/* Succeeded: the control is in the pending state. */
	return 0;
}

/* Sets a text control's text and placeholder (set_control_text: id, text, placeholder). */
static int
titlebar_set_text(
	struct kwl_object *titlebar,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_control *control;
	const char *placeholder;
	const char *text;
	unsigned index;
	size_t text_length;
	size_t placeholder_length;
	size_t middle;
	size_t next;
	int error;

	/* The arguments. */
	if (size < 4U)
		return EPROTO;
	error = titlebar_string(bytes, size, 4U, &text, &middle);
	if (error != 0)
		return EPROTO;
	error = titlebar_string(bytes, size, middle, &placeholder, &next);
	if (error != 0 || next != size)
		return EPROTO;

	/* A search or a breadcrumb, which are the controls that have text. */
	control = titlebar_find_control(&titlebar->titlebar_model->pending, titlebar_word(bytes, 0U), &index);
	if (control == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no control has the ID");
		return error;
	}

	/* Only those two roles have text. */
	if (control->role != KWL_CONTROL_SEARCH && control->role != KWL_CONTROL_BREADCRUMB) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "text for a control that is not a search or a breadcrumb");
		return error;
	}

	/* Not longer than a string may be. */
	text_length = strlen(text);
	placeholder_length = strlen(placeholder);
	if (text_length > KWL_TITLEBAR_TEXT_MAX || placeholder_length > KWL_TITLEBAR_TEXT_MAX) {
		error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "a text longer than 1023 bytes");
		return error;
	}

	/* The text and the placeholder. */
	error = titlebar_replace(&control->text, text);
	if (error != 0)
		return error;
	error = titlebar_replace(&control->placeholder, placeholder);
	if (error != 0)
		return error;

	/* Succeeded: the texts changed. */
	return 0;
}

/* Sets a breadcrumb's parts (set_breadcrumb: id, an array of NUL-terminated parts). */
static int
titlebar_set_breadcrumb(
	struct kwl_object *titlebar,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_titlebar_control *control;
	const char *parts;
	unsigned index;
	uint32_t length;
	size_t aligned;
	size_t at;
	size_t part;
	int error;

	/* The control and the array's length, which the payload must hold (padded to a word). */
	if (size < 8U)
		return EPROTO;
	length = titlebar_word(bytes, 4U);
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned != size - 8U)
		return EPROTO;
	parts = (const char *)bytes + 8U;

	/* A breadcrumb that is there. */
	control = titlebar_find_control(&titlebar->titlebar_model->pending, titlebar_word(bytes, 0U), &index);
	if (control == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no control has the ID");
		return error;
	}

	/* Only a breadcrumb has parts. */
	if (control->role != KWL_CONTROL_BREADCRUMB) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "parts for a control that is not a breadcrumb");
		return error;
	}

	/* A list that is not empty ends with a part's NUL. */
	if (length != 0U && parts[length - 1U] != '\0') {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "breadcrumb parts not ended by NUL");
		return error;
	}

	/* The old parts go. */
	for (part = 0; part < control->segment_count; part++) {
		free(control->segments[part]);
		control->segments[part] = NULL;
	}

	/* None is left. */
	control->segment_count = 0;

	/* Each part, as many as a breadcrumb has at most. */
	at = 0;
	while (at < length) {
		if (control->segment_count == KWL_TITLEBAR_SEGMENTS_MAX) {
			error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "more than 32 breadcrumb parts");
			return error;
		}

		/* The part, copied. */
		control->segments[control->segment_count] = titlebar_text_copy(parts + at);
		if (control->segments[control->segment_count] == NULL)
			return EPROTO;
		at += strlen(parts + at) + 1U;
		control->segment_count++;
	}

	/* Succeeded: the breadcrumb has its parts. */
	return 0;
}

/* Asks for the keyboard for a text control (focus_control: id, mode), outside the transactions. */
static int
titlebar_focus(
	struct kwl_object *titlebar,
	const unsigned char *bytes,
	size_t size)
{
	const struct kwl_titlebar_control *control;
	struct kwl_titlebar_model *model;
	uint32_t mode;
	int error;

	/* The control, which the state shown must have, and how it takes the keyboard. */
	if (size != 8U)
		return EPROTO;
	model = titlebar->titlebar_model;
	control = kwl_titlebar_control(&model->shown, titlebar_word(bytes, 0U));
	mode = titlebar_word(bytes, 4U);
	if (control == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no control shown has the ID");
		return error;
	}

	/* A search takes it as a field; a breadcrumb may also be edited as a path. */
	if (control->role != KWL_CONTROL_SEARCH && control->role != KWL_CONTROL_BREADCRUMB) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "focus for a control that is not a search or a breadcrumb");
		return error;
	}

	/* A known way to take it. */
	if (mode > TITLEBAR_FOCUS_EDIT) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "an unknown focus mode");
		return error;
	}

	/* The presentation takes the request on its next pass. */
	model->focus_id = control->id;
	model->focus_mode = mode;
	titlebar->client->server->dirty = 1;

	/* Succeeded: the control waits for the keyboard. */
	return 0;
}

/*
 * Takes a text field's suggestions (set_suggestions: id, an array of
 * NUL-terminated strings, each suggestion a label and the text it puts in
 * the field), outside the transactions: the presentation shows them under
 * the field while it has the keyboard, until its text changes.  An empty
 * array takes them away.
 */
static int
titlebar_suggest(
	struct kwl_object *titlebar,
	const unsigned char *bytes,
	size_t size)
{
	const struct kwl_titlebar_control *control;
	const char *strings[2U * KWL_TITLEBAR_SUGGESTIONS_MAX];
	const char *items;
	uint32_t length;
	size_t aligned;
	size_t count;
	size_t at;
	size_t text_length;
	int error;

	/* The control and the array's length, which the payload must hold (padded to a word). */
	if (size < 8U)
		return EPROTO;
	length = titlebar_word(bytes, 4U);
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	if (aligned != size - 8U)
		return EPROTO;
	items = (const char *)bytes + 8U;

	/* A search or a breadcrumb that is shown. */
	control = kwl_titlebar_control(&titlebar->titlebar_model->shown, titlebar_word(bytes, 0U));
	if (control == NULL) {
		error = titlebar_fail(titlebar, ERROR_INVALID_ID, "no control shown has the ID");
		return error;
	}

	/* Only a text field has suggestions. */
	if (control->role != KWL_CONTROL_SEARCH && control->role != KWL_CONTROL_BREADCRUMB) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "suggestions for a control that is not a search or a breadcrumb");
		return error;
	}

	/* A list that is not empty ends with a string's NUL. */
	if (length != 0U && items[length - 1U] != '\0') {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "suggestions not ended by NUL");
		return error;
	}

	/* Each string, as many as the suggestions hold, none longer than a text. */
	count = 0;
	at = 0;
	while (at < length) {
		if (count == 2U * KWL_TITLEBAR_SUGGESTIONS_MAX) {
			error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "more than 12 suggestions");
			return error;
		}

		/* The string, which must fit a field. */
		text_length = strlen(items + at);
		if (text_length > KWL_TITLEBAR_TEXT_MAX) {
			error = titlebar_fail(titlebar, ERROR_TOO_LARGE, "a suggestion longer than 1023 bytes");
			return error;
		}
		strings[count] = items + at;
		count++;
		at += text_length + 1U;
	}

	/* Each suggestion is a label and a text: the strings come in pairs. */
	if ((count % 2U) != 0U) {
		error = titlebar_fail(titlebar, ERROR_INVALID_VALUE, "a suggestion without its text");
		return error;
	}

	/* The presentation shows them when the field is this one's. */
	kwl_titlebar_suggestions(titlebar->client->server, titlebar, control->id, strings, count / 2U);

	/* Succeeded: the suggestions are taken (or the field had gone). */
	return 0;
}

/* Sends a kl_titlebar_v1 protocol error. */
static int
titlebar_fail(
	struct kwl_object *titlebar,
	uint32_t code,
	const char *reason)
{
	int error;

	/* The error names the titlebar and the kl_titlebar_v1 error code. */
	error = kwl_error_code(titlebar->client, titlebar->id, code, reason);

	/* Reports the refusal. */
	return error;
}

/* Finds a control of a state that may change, and its index; NULL when there is none. */
static struct kwl_titlebar_control *
titlebar_find_control(
	struct kwl_titlebar_state *state,
	uint32_t id,
	unsigned *index)
{
	unsigned at;

	/* Each control, for the ID. */
	for (at = 0; at < state->control_count; at++) {
		if (state->controls[at].id != id)
			continue;
		*index = at;
		return &state->controls[at];
	}

	/* No control has the ID. */
	return NULL;
}

/* Finds a tab of a state that may change, and its index; NULL when there is none. */
static struct kwl_titlebar_tab *
titlebar_find_tab(
	struct kwl_titlebar_state *state,
	uint32_t id,
	unsigned *index)
{
	unsigned at;

	/* Each tab, for the ID. */
	for (at = 0; at < state->tab_count; at++) {
		if (state->tabs[at].id != id)
			continue;
		*index = at;
		return &state->tabs[at];
	}

	/* No tab has the ID. */
	return NULL;
}

/* Replaces an allocated string by a copy of a text; EPROTO when there is no memory. */
static int
titlebar_replace(
	char **field,
	const char *text)
{
	char *copy;

	/* The copy, before the old one goes. */
	copy = titlebar_text_copy(text);
	if (copy == NULL)
		return EPROTO;

	/* The old string goes and the copy takes its place. */
	free(*field);
	*field = copy;

	/* Succeeded: the string is replaced. */
	return 0;
}

/* Copies a state deeply (its strings too) into an empty one; ENOMEM leaves the target empty. */
static int
titlebar_state_copy(
	struct kwl_titlebar_state *target,
	const struct kwl_titlebar_state *source)
{
	struct kwl_titlebar_control *control;
	unsigned index;
	unsigned part;

	/* The numbers, and the pointers that are made again below. */
	*target = *source;

	/* Each control's strings. */
	for (index = 0; index < target->control_count; index++) {
		control = &target->controls[index];
		control->label = titlebar_text_copy(source->controls[index].label);
		control->text = titlebar_text_copy(source->controls[index].text);
		control->placeholder = titlebar_text_copy(source->controls[index].placeholder);
		for (part = 0; part < control->segment_count; part++)
			control->segments[part] = titlebar_text_copy(source->controls[index].segments[part]);
	}

	/* Each tab's title. */
	for (index = 0; index < target->tab_count; index++)
		target->tabs[index].title = titlebar_text_copy(source->tabs[index].title);

	/* A copy that ran out of memory is not kept (a NULL where the source has a string). */
	for (index = 0; index < target->control_count; index++) {
		control = &target->controls[index];
		if (control->label == NULL) {
			titlebar_state_free(target);
			return ENOMEM;
		}
	}

	/* Succeeded: the target is a whole copy. */
	return 0;
}

/* Frees a state's strings and leaves it empty. */
static void
titlebar_state_free(
	struct kwl_titlebar_state *state)
{
	unsigned index;

	/* Each control's strings. */
	for (index = 0; index < state->control_count; index++)
		titlebar_control_free(&state->controls[index]);

	/* Each tab's title. */
	for (index = 0; index < state->tab_count; index++)
		free(state->tabs[index].title);

	/* Nothing is left. */
	memset(state, 0, sizeof(*state));
}

/* Frees a control's strings. */
static void
titlebar_control_free(
	struct kwl_titlebar_control *control)
{
	unsigned part;

	/* The label and the texts. */
	free(control->label);
	free(control->text);
	free(control->placeholder);
	control->label = NULL;
	control->text = NULL;
	control->placeholder = NULL;

	/* The breadcrumb's parts. */
	for (part = 0; part < control->segment_count; part++) {
		free(control->segments[part]);
		control->segments[part] = NULL;
	}

	/* None is left. */
	control->segment_count = 0;
}

/* Copies a text into a new allocation; NULL for no text or no memory. */
static char *
titlebar_text_copy(
	const char *text)
{
	char *copy;
	size_t length;

	/* No text, no copy. */
	if (text == NULL)
		return NULL;

	/* The bytes and the terminator. */
	length = strlen(text) + 1U;
	copy = malloc(length);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);

	/* Succeeded: the copy. */
	return copy;
}

/* Writes a string argument (its length with the NUL, its bytes, the padding) at an offset; returns where the next goes. */
static size_t
titlebar_put_string(
	unsigned char *payload,
	size_t offset,
	const char *text)
{
	uint32_t length;
	size_t aligned;

	/* The length, the NUL counted, and the text cut to what a string may hold. */
	length = (uint32_t)strlen(text);
	if (length > KWL_TITLEBAR_TEXT_MAX)
		length = KWL_TITLEBAR_TEXT_MAX;
	length++;
	memcpy(payload + offset, &length, sizeof(length));

	/* The bytes, the NUL and the padding to a word. */
	aligned = ((size_t)length + 3U) & ~(size_t)3U;
	memset(payload + offset + 4U, 0, aligned);
	memcpy(payload + offset + 4U, text, (size_t)length - 1U);

	/* Succeeded: the next argument's place. */
	return offset + 4U + aligned;
}

/* Returns the first live wl_seat of the titlebar's client, or 0 (the null seat). */
static uint32_t
titlebar_seat(
	struct kwl_object *titlebar)
{
	struct kwl_object *object;

	/* The client's objects, for a seat. */
	for (object = titlebar->client->objects; object != NULL; object = object->next) {
		if (object->kind == KWL_SEAT && object->dead == 0U)
			return object->id;
	}

	/* The client has no seat. */
	return 0;
}

/* Tells whether a titlebar can still be told anything (it and its client are alive). */
static int
titlebar_live(
	struct kwl_object *titlebar)
{
	/* A dead titlebar, or a failed client. */
	if (titlebar->dead != 0U)
		return 0;
	if (titlebar->client->fatal != 0U)
		return 0;

	/* It can be told. */
	return 1;
}

/* Sends an event of a titlebar; a client that cannot take it is failed. */
static void
titlebar_emit(
	struct kwl_object *titlebar,
	uint32_t opcode,
	const void *payload,
	size_t size)
{
	int error;

	/* The event. */
	error = kwl_emit(titlebar->client, titlebar->id, opcode, payload, size);
	if (error == 0)
		return;

	/* The client is failed. */
	kwl_client_fail(titlebar->client);
}
