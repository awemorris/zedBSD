/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Printers page (ws145-p004, plan/ws145/design.md section 6): the
 * user's printers through the desktop (kl_system_printers_*), never the
 * settings file itself.
 *
 *   Printers      each printer's name, address, protocol and path, the
 *                 default marked; Edit, Make Default and Remove for each.
 *   Edit a Printer its name and its IPP path or LPD queue, while one is
 *                 edited (ws177-p025).
 *   Add a Printer the protocol (IPP or LPD), the address, the port (the
 *                 protocol's usual one when empty) and, as a detail, the
 *                 IPP path or the LPD queue (the usual one when empty).
 *   Print Jobs    each job's title, printer and state, Cancel for those
 *                 not ended.
 *
 * Each change's answer is a line under the cards.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The controls (hit indices): the protocol's buttons, the fields, Add, and each printer's and job's buttons. */
#define PRINTERS_IPP		1
#define PRINTERS_LPD		2
#define PRINTERS_FIELD_FIRST	3
#define PRINTERS_ADD		10
#define PRINTERS_SAVE		11
#define PRINTERS_CLOSE		12
#define PRINTERS_EDIT_FIELD_FIRST	20
#define PRINTERS_DEFAULT_FIRST	100
#define PRINTERS_REMOVE_FIRST	200
#define PRINTERS_CANCEL_FIRST	300
#define PRINTERS_EDIT_FIRST	400

/* The fields: the addition's, then the edit's (as the focus counts them). */
#define PRINTERS_ADDRESS	0
#define PRINTERS_PORT		1
#define PRINTERS_PATH		2
#define PRINTERS_EDIT_NAME	3
#define PRINTERS_EDIT_PATH	4

/* The rows' heights, the fields' place, and the text sizes. */
#define PRINTERS_ROW		56
#define PRINTERS_FIELD_ROW	52
#define PRINTERS_JOB_ROW	44
#define PRINTERS_FIELD_X	170
#define PRINTERS_TEXT_ROW	14U
#define PRINTERS_TEXT_SUB	12U

/* The fields' labels: the addition's and the edit's. */
static const char *const printers_labels[SE_PRINTER_FIELDS + SE_PRINTER_EDIT_FIELDS] = { "Address", "Port", "Path or queue", "Name", "Path or queue" };

static int printers_available(const struct se_app *app);
static int printers_list(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int printers_form(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static int printers_jobs(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void printers_field_draw(struct se_app *app, struct kl_canvas *canvas, int index, int x, int y, int width);
static void printers_add(struct se_app *app);
static int printers_editor(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void printers_edit_start(struct se_app *app, const struct kl_printer *printer);
static void printers_edit_save(struct se_app *app);
static struct kl_field *printers_field(struct se_printers *printers, int index);
static void printers_asked(struct se_app *app, int error, const char *doing);
static const char *printers_state_name(const struct kl_print_job *job);
static const char *printers_printer_name(const struct se_app *app, uint32_t printer);

/*
 * Draws the Printers page's cards from a top edge; returns the edge below
 * them.
 */
int
se_printers_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_printers *printers;
	kl_color ink;
	int available;
	int y;

	/* Without printing, a card that says so. */
	printers = &app->printers;
	available = printers_available(app);
	if (!available) {
		y = se_card_begin(app, canvas, x, top, width, 64 + 50, "Printers", NULL);
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 24, "This desktop cannot print.", PRINTERS_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + 64 + 50;
	}

	/* The printers, the printer being edited, the form, the jobs. */
	y = printers_list(app, canvas, x, top, width);
	if (printers->editing != 0U)
		y = printers_editor(app, canvas, x, y + 16, width);
	y = printers_form(app, canvas, x, y + 16, width);

	/* The last answer. */
	if (printers->message[0] != '\0') {
		ink = SE_COLOR_GOOD;
		if (printers->message_bad)
			ink = SE_COLOR_BAD;
		(void)kl_text_draw_fit(app->text, canvas, x + 2, y + 18, printers->message, PRINTERS_TEXT_SUB, 0, width, ink);
		y += 30;
	}

	/* The jobs. */
	y = printers_jobs(app, canvas, x, y + 16, width);
	return y;
}

/*
 * Carries out a click on a control of the page.
 */
void
se_printers_press(
	struct se_app *app,
	int index)
{
	struct kl_printer list[KL_PRINTERS_MAX];
	struct kl_print_job jobs[KL_PRINT_JOBS_MAX];
	struct se_printers *printers;
	size_t count;
	int available;
	int error;

	/* Nothing without printing, or while an answer is awaited. */
	printers = &app->printers;
	available = printers_available(app);
	if (!available || printers->request != 0U)
		return;
	app->dirty = 1;

	/* The protocol. */
	if (index == PRINTERS_IPP || index == PRINTERS_LPD) {
		printers->protocol = KL_PRINTER_IPP;
		if (index == PRINTERS_LPD)
			printers->protocol = KL_PRINTER_LPD;
		return;
	}

	/* A field takes the keyboard. */
	if (index >= PRINTERS_FIELD_FIRST && index < PRINTERS_FIELD_FIRST + SE_PRINTER_FIELDS) {
		printers->focus = index - PRINTERS_FIELD_FIRST;
		printers->typing = 1;
		return;
	}

	/* An edit's field takes the keyboard. */
	if (index >= PRINTERS_EDIT_FIELD_FIRST && index < PRINTERS_EDIT_FIELD_FIRST + SE_PRINTER_EDIT_FIELDS) {
		printers->focus = PRINTERS_EDIT_NAME + index - PRINTERS_EDIT_FIELD_FIRST;
		printers->typing = 1;
		return;
	}

	/* Add Printer. */
	if (index == PRINTERS_ADD) {
		printers_add(app);
		return;
	}

	/* The edit saved. */
	if (index == PRINTERS_SAVE) {
		printers_edit_save(app);
		return;
	}

	/* The edit left: nothing changes. */
	if (index == PRINTERS_CLOSE) {
		printers->editing = 0U;
		if (printers->focus >= PRINTERS_EDIT_NAME)
			printers->typing = 0;
		return;
	}

	/* A printer's Edit, Make Default or Remove. */
	count = kl_system_printers_get(app->system, list, KL_PRINTERS_MAX);
	if (index >= PRINTERS_EDIT_FIRST && index < PRINTERS_EDIT_FIRST + (int)count) {
		printers_edit_start(app, &list[index - PRINTERS_EDIT_FIRST]);
		return;
	}

	/* Make Default. */
	if (index >= PRINTERS_DEFAULT_FIRST && index < PRINTERS_DEFAULT_FIRST + (int)count) {
		error = kl_system_printers_set_default(app->system, list[index - PRINTERS_DEFAULT_FIRST].id, &printers->request);
		printers_asked(app, error, "default");
		return;
	}

	/* Remove. */
	if (index >= PRINTERS_REMOVE_FIRST && index < PRINTERS_REMOVE_FIRST + (int)count) {
		error = kl_system_printers_remove(app->system, list[index - PRINTERS_REMOVE_FIRST].id, &printers->request);
		printers_asked(app, error, "remove");
		return;
	}

	/* A job's Cancel. */
	count = kl_system_print_jobs_get(app->system, jobs, KL_PRINT_JOBS_MAX);
	if (index >= PRINTERS_CANCEL_FIRST && index < PRINTERS_CANCEL_FIRST + (int)count) {
		error = kl_system_print_cancel(app->system, jobs[index - PRINTERS_CANCEL_FIRST].job, &printers->request);
		printers_asked(app, error, "cancel");
	}
}

/*
 * Takes a key while a field has the keyboard: Tab moves between the
 * fields of its card, Enter adds the printer (or saves the edit), Esc
 * gives the keyboard back, the others type.  Returns 1 when the key was
 * used.
 */
int
se_printers_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_printers *printers;
	int used;

	/* No field has the keyboard. */
	printers = &app->printers;
	if (!printers->typing)
		return 0;
	app->dirty = 1;

	/* Tab and Shift+Tab, within the edit's two fields. */
	if (event->key == SE_KEY_TAB && printers->focus >= PRINTERS_EDIT_NAME) {
		if (printers->focus == PRINTERS_EDIT_NAME)
			printers->focus = PRINTERS_EDIT_PATH;
		else
			printers->focus = PRINTERS_EDIT_NAME;
		return 1;
	}

	/* Tab and Shift+Tab, within the addition's fields. */
	if (event->key == SE_KEY_TAB) {
		if ((event->modifiers & SE_MOD_SHIFT) != 0U)
			printers->focus = (printers->focus + SE_PRINTER_FIELDS - 1) % SE_PRINTER_FIELDS;
		else
			printers->focus = (printers->focus + 1) % SE_PRINTER_FIELDS;
		return 1;
	}

	/* Enter: the edit saved, or the addition. */
	if (event->key == SE_KEY_ENTER) {
		if (printers->focus >= PRINTERS_EDIT_NAME)
			printers_edit_save(app);
		else
			printers_add(app);
		return 1;
	}

	/* Esc: the keyboard goes back. */
	if (event->key == SE_KEY_ESC) {
		printers->typing = 0;
		return 1;
	}

	/* Anything else types into the field. */
	used = se_field_key(printers_field(printers, printers->focus), event);
	return used;
}

/*
 * Follows the printers: a change of the printers or the jobs draws the
 * page again.
 */
void
se_printers_poll(
	struct se_app *app)
{
	/* A change. */
	if ((app->system_changed & KL_SYSTEM_CHANGED_PRINTERS) != 0U) {
		app->dirty = 1;
		se_log("PRINTERS changed");
	}
}

/*
 * Takes the answer of the page's request.  Returns 1 when it was the
 * page's.
 */
int
se_printers_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_printers *printers;
	int edited;
	int added;

	/* Only the request the page asked. */
	printers = &app->printers;
	if (printers->request == 0U || request != printers->request)
		return 0;
	printers->request = 0U;
	se_log("PRINTERS result kind=%s errno=%d", printers->doing, error);
	app->dirty = 1;

	/* What it says. */
	printers->message_bad = 1;
	switch (error) {
	case 0:
		printers->message_bad = 0;
		(void)snprintf(printers->message, sizeof(printers->message), "%s", "Done.");
		edited = strcmp(printers->doing, "edit");
		if (edited == 0) {
			(void)snprintf(printers->message, sizeof(printers->message), "%s", "The printer is changed.");
			printers->editing = 0U;
		}

		/* An addition empties the form. */
		added = strcmp(printers->doing, "add");
		if (added == 0) {
			(void)snprintf(printers->message, sizeof(printers->message), "%s", "The printer is added.");
			se_field_clear(&printers->fields[PRINTERS_ADDRESS]);
			se_field_clear(&printers->fields[PRINTERS_PORT]);
			se_field_clear(&printers->fields[PRINTERS_PATH]);
		}

		/* Its line. */
		break;
	case EINVAL:
		(void)snprintf(printers->message, sizeof(printers->message), "%s", "That printer is already added, or is not known.");
		break;
	case EBUSY:
		(void)snprintf(printers->message, sizeof(printers->message), "%s", "There are too many printers or jobs.");
		break;
	default:
		(void)snprintf(printers->message, sizeof(printers->message), "%s", "The printers could not be changed.");
		break;
	}

	/* Succeeded: the answer was the page's. */
	return 1;
}

/* Tells whether the desktop prints. */
static int
printers_available(
	const struct se_app *app)
{
	unsigned capabilities;

	/* The desktop's printers. */
	if (app->system == NULL)
		return 0;
	capabilities = kl_system_capabilities(app->system);
	return (capabilities & KL_SYSTEM_HAS_PRINTERS) != 0U;
}

/* Draws the printers' card; returns the edge below it. */
static int
printers_list(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_printer list[KL_PRINTERS_MAX];
	char line[256];
	const char *protocol;
	size_t count;
	size_t index;
	int enabled;
	int button;
	int right;
	int height;
	int y;

	/* The card. */
	count = kl_system_printers_get(app->system, list, KL_PRINTERS_MAX);
	height = 64 + 40;
	if (count > 0U)
		height = 64 + (int)count * PRINTERS_ROW + 8;
	y = se_card_begin(app, canvas, x, top, width, height, "Printers", "The printers this account prints to.");
	if (count == 0U) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, "No printer is added yet.", PRINTERS_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* Each printer: its name, its address and protocol, Default or Make Default, Remove. */
	right = x + width - 20;
	enabled = app->printers.request == 0U;
	for (index = 0; index < count; index++) {
		protocol = "IPP";
		if (list[index].protocol == KL_PRINTER_LPD)
			protocol = "LPD";
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, list[index].name, PRINTERS_TEXT_ROW, 1, width / 2, SE_COLOR_TEXT);
		(void)snprintf(line, sizeof(line), "%s:%u  %s  %s", list[index].host, list[index].port, protocol, list[index].path);
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 42, line, PRINTERS_TEXT_SUB, 0, width / 2, SE_COLOR_TEXT_SECONDARY);
		button = se_button_width(app, "Remove");
		(void)se_button_draw(app, canvas, right - button, y + 10, "Remove", 0, enabled, PRINTERS_REMOVE_FIRST + (int)index);
		right -= button + 10;
		button = se_button_width(app, "Edit");
		(void)se_button_draw(app, canvas, right - button, y + 10, "Edit", 0, enabled, PRINTERS_EDIT_FIRST + (int)index);
		right -= button + 10;
		if ((list[index].flags & KL_PRINTER_DEFAULT) != 0U) {
			button = kl_text_width(app->text, "Default", strlen("Default"), PRINTERS_TEXT_SUB, 1);
			(void)kl_text_draw(app->text, canvas, right - button, y + 32, "Default", strlen("Default"), PRINTERS_TEXT_SUB, 1, SE_COLOR_GOOD);
		} else {
			button = se_button_width(app, "Make Default");
			(void)se_button_draw(app, canvas, right - button, y + 10, "Make Default", 0, enabled, PRINTERS_DEFAULT_FIRST + (int)index);
		}

		/* The next row. */
		right = x + width - 20;
		y += PRINTERS_ROW;
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws the card that adds a printer; returns the edge below it. */
static int
printers_form(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_printers *printers;
	int enabled;
	int button;
	int height;
	int index;
	int left;
	int y;

	/* The card. */
	printers = &app->printers;
	height = 64 + 48 + SE_PRINTER_FIELDS * PRINTERS_FIELD_ROW + 56;
	y = se_card_begin(app, canvas, x, top, width, height, "Add a Printer", "Its address on the network, and how it takes documents.");

	/* The protocol: IPP or LPD. */
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(PRINTERS_TEXT_ROW, y + 6, 36), "Protocol", PRINTERS_TEXT_ROW, 0, PRINTERS_FIELD_X - 30, SE_COLOR_TEXT);
	enabled = printers->request == 0U;
	left = x + PRINTERS_FIELD_X;
	button = se_button_draw(app, canvas, left, y + 6, "IPP", printers->protocol != KL_PRINTER_LPD, enabled, PRINTERS_IPP);
	(void)se_button_draw(app, canvas, left + button + 8, y + 6, "LPD", printers->protocol == KL_PRINTER_LPD, enabled, PRINTERS_LPD);
	y += 48;

	/* The fields. */
	for (index = 0; index < SE_PRINTER_FIELDS; index++) {
		printers_field_draw(app, canvas, index, x, y, width);
		y += PRINTERS_FIELD_ROW;
	}

	/* Add Printer at the right, when an address is typed. */
	enabled = printers->request == 0U && printers->fields[PRINTERS_ADDRESS].length > 0U;
	button = se_button_width(app, "Add Printer");
	(void)se_button_draw(app, canvas, x + width - 20 - button, y + 10, "Add Printer", 1, enabled, PRINTERS_ADD);

	/* The edge below the card. */
	return top + height;
}

/* Draws the print jobs' card; returns the edge below it. */
static int
printers_jobs(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_print_job jobs[KL_PRINT_JOBS_MAX];
	char line[256];
	size_t count;
	size_t index;
	int enabled;
	int button;
	int height;
	int y;

	/* The card. */
	count = kl_system_print_jobs_get(app->system, jobs, KL_PRINT_JOBS_MAX);
	height = 64 + 40;
	if (count > 0U)
		height = 64 + (int)count * PRINTERS_JOB_ROW + 8;
	y = se_card_begin(app, canvas, x, top, width, height, "Print Jobs", "What was sent to the printers lately.");
	if (count == 0U) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, "Nothing was printed yet.", PRINTERS_TEXT_ROW, 0, width - 40, SE_COLOR_TEXT_SECONDARY);
		return top + height;
	}

	/* Each job, the newest first: its title, its printer and state, Cancel while it is not ended. */
	enabled = app->printers.request == 0U;
	for (index = count; index > 0U; index--) {
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 18, jobs[index - 1U].title, PRINTERS_TEXT_ROW, 0, width / 2, SE_COLOR_TEXT);
		(void)snprintf(line, sizeof(line), "%s  %s", printers_printer_name(app, jobs[index - 1U].printer), printers_state_name(&jobs[index - 1U]));
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 36, line, PRINTERS_TEXT_SUB, 0, width - 160, SE_COLOR_TEXT_SECONDARY);
		if (jobs[index - 1U].state < KL_PRINT_DONE) {
			button = se_button_width(app, "Cancel");
			(void)se_button_draw(app, canvas, x + width - 20 - button, y + 6, "Cancel", 0, enabled, PRINTERS_CANCEL_FIRST + (int)index - 1);
		}

		/* The next row. */
		y += PRINTERS_JOB_ROW;
	}

	/* The edge below the card. */
	return top + height;
}

/* Draws one field's row: its label, the field, the keyboard's cursor in the focused one. */
static void
printers_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int index,
	int x,
	int y,
	int width)
{
	struct se_printers *printers;
	struct kl_rect box;
	const char *placeholder;
	int focused;
	int lpd;

	/* The label. */
	printers = &app->printers;
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(PRINTERS_TEXT_ROW, y + 8, 36), printers_labels[index], PRINTERS_TEXT_ROW, 0, PRINTERS_FIELD_X - 30, SE_COLOR_TEXT);

	/* The field's place; a click gives it the keyboard. */
	box.x = x + PRINTERS_FIELD_X;
	box.y = y + 8;
	box.width = width - PRINTERS_FIELD_X - 20;
	box.height = 36;
	if (index >= PRINTERS_EDIT_NAME)
		se_ui_hit(app, &box, SE_HIT_CONTROL, PRINTERS_EDIT_FIELD_FIRST + index - PRINTERS_EDIT_NAME);
	else
		se_ui_hit(app, &box, SE_HIT_CONTROL, PRINTERS_FIELD_FIRST + index);

	/* What an empty field means: the protocol's usual port and path. */
	placeholder = "192.168.1.20";
	lpd = printers->protocol == KL_PRINTER_LPD;
	if (index == PRINTERS_PORT && lpd)
		placeholder = "515";
	else if (index == PRINTERS_PORT)
		placeholder = "631";
	if (index == PRINTERS_PATH && lpd)
		placeholder = "lp";
	else if (index == PRINTERS_PATH)
		placeholder = "/ipp/print";
	if (index == PRINTERS_EDIT_NAME)
		placeholder = "Office Printer";
	else if (index == PRINTERS_EDIT_PATH)
		placeholder = "/ipp/print";
	focused = printers->typing && printers->focus == index;
	(void)se_field_draw(app, canvas, printers_field(printers, index), &box, placeholder, SE_FIELD_PLAIN, focused);
}

/* Asks for the printer the form describes. */
static void
printers_add(
	struct se_app *app)
{
	struct se_printers *printers;
	unsigned protocol;
	unsigned port;
	int error;

	/* An address, and a port (the usual one when empty). */
	printers = &app->printers;
	if (printers->request != 0U || printers->fields[PRINTERS_ADDRESS].length == 0U)
		return;
	protocol = KL_PRINTER_IPP;
	if (printers->protocol == KL_PRINTER_LPD)
		protocol = KL_PRINTER_LPD;
	port = 631U;
	if (protocol == KL_PRINTER_LPD)
		port = 515U;
	if (printers->fields[PRINTERS_PORT].length > 0U)
		port = (unsigned)strtoul(printers->fields[PRINTERS_PORT].text, NULL, 10);

	/* Asked of the desktop. */
	error = kl_system_printers_add(app->system, protocol, printers->fields[PRINTERS_ADDRESS].text, port, printers->fields[PRINTERS_PATH].text,
	    &printers->request);
	printers_asked(app, error, "add");
	printers->typing = 0;
}

/*
 * Draws the card that edits a printer's name and its IPP path or LPD
 * queue (ws177-p025); returns the edge below it.  A printer gone meanwhile
 * ends the edit.
 */
static int
printers_editor(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_printer list[KL_PRINTERS_MAX];
	struct se_printers *printers;
	const char *title;
	size_t count;
	size_t index;
	int enabled;
	int button;
	int height;
	int field;
	int y;

	/* The printer edited, still there. */
	printers = &app->printers;
	count = kl_system_printers_get(app->system, list, KL_PRINTERS_MAX);
	title = NULL;
	for (index = 0; index < count; index++) {
		if (list[index].id == printers->editing)
			title = list[index].name;
	}

	/* Gone: nothing to edit. */
	if (title == NULL) {
		printers->editing = 0U;
		return top - 16;
	}

	/* The card. */
	height = 64 + SE_PRINTER_EDIT_FIELDS * PRINTERS_FIELD_ROW + 56;
	y = se_card_begin(app, canvas, x, top, width, height, "Edit a Printer", title);

	/* The fields: the name, the path or queue. */
	for (field = PRINTERS_EDIT_NAME; field <= PRINTERS_EDIT_PATH; field++) {
		printers_field_draw(app, canvas, field, x, y, width);
		y += PRINTERS_FIELD_ROW;
	}

	/* Save at the right, Cancel beside it. */
	enabled = printers->request == 0U;
	button = se_button_width(app, "Save");
	(void)se_button_draw(app, canvas, x + width - 20 - button, y + 10, "Save", 1, enabled, PRINTERS_SAVE);
	(void)se_button_draw(app, canvas, x + width - 20 - button - 10 - se_button_width(app, "Cancel"), y + 10, "Cancel", 0, 1, PRINTERS_CLOSE);

	/* The edge below the card. */
	return top + height;
}

/* Starts editing a printer: its name and path in the fields, the name with the keyboard. */
static void
printers_edit_start(
	struct se_app *app,
	const struct kl_printer *printer)
{
	struct se_printers *printers;

	/* The printer and its values. */
	printers = &app->printers;
	printers->editing = printer->id;
	kl_field_set(&printers->edit_fields[0], printer->name);
	kl_field_set(&printers->edit_fields[1], printer->path);
	printers->focus = PRINTERS_EDIT_NAME;
	printers->typing = 1;
	printers->message[0] = '\0';
	se_log("PRINTERS edit printer=%u", printer->id);
}

/* Asks for the edit's name and path. */
static void
printers_edit_save(
	struct se_app *app)
{
	struct se_printers *printers;
	int error;

	/* A printer edited, and no request out. */
	printers = &app->printers;
	if (printers->request != 0U || printers->editing == 0U)
		return;

	/* Asked of the desktop. */
	error = kl_system_printers_edit(app->system, printers->editing, printers->edit_fields[0].text, printers->edit_fields[1].text,
	    &printers->request);
	printers_asked(app, error, "edit");
	printers->typing = 0;
}

/* The field of an index the focus counts (0 to 2 the addition's, 3 and 4 the edit's). */
static struct kl_field *
printers_field(
	struct se_printers *printers,
	int index)
{
	struct kl_field *field;

	/* The edit's. */
	if (index >= PRINTERS_EDIT_NAME) {
		field = &printers->edit_fields[index - PRINTERS_EDIT_NAME];
		return field;
	}

	/* Succeeded: the addition's. */
	field = &printers->fields[index];
	return field;
}

/* Notes a request asked (or refused at once). */
static void
printers_asked(
	struct se_app *app,
	int error,
	const char *doing)
{
	struct se_printers *printers;

	/* The kind, for the answer's line. */
	printers = &app->printers;
	(void)snprintf(printers->doing, sizeof(printers->doing), "%s", doing);
	se_log("PRINTERS ask kind=%s error=%d", doing, error);
	printers->message[0] = '\0';
	if (error == 0)
		return;

	/* Refused at once. */
	printers->request = 0U;
	printers->message_bad = 1;
	(void)snprintf(printers->message, sizeof(printers->message), "%s", "Check the address and the port.");
}

/* The words of a job's state. */
static const char *
printers_state_name(
	const struct kl_print_job *job)
{
	static char text[64];

	/* By the state. */
	switch (job->state) {
	case KL_PRINT_QUEUED:
		return "Waiting";
	case KL_PRINT_SENDING:
		return "Sending";
	case KL_PRINT_WAITING:
		return "Printing";
	case KL_PRINT_DONE:
		return "Done";
	case KL_PRINT_CANCELLED:
		return "Cancelled";
	default:
		break;
	}

	/* Failed, with its word. */
	if (job->detail[0] == '\0')
		return "Failed";
	(void)snprintf(text, sizeof(text), "Failed (%s)", job->detail);
	return text;
}

/* The name of a job's printer (its number when it is gone). */
static const char *
printers_printer_name(
	const struct se_app *app,
	uint32_t printer)
{
	static struct kl_printer list[KL_PRINTERS_MAX];
	size_t count;
	size_t index;

	/* The printer. */
	count = kl_system_printers_get(app->system, list, KL_PRINTERS_MAX);
	for (index = 0; index < count; index++) {
		if (list[index].id == printer)
			return list[index].name;
	}

	/* Gone. */
	return "A removed printer";
}
