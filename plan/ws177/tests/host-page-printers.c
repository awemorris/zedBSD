/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p025: the host test of Settings' Printers page
 * (userland/desktop/settings/page-printers.c), what it draws and asks:
 * the widgets it draws are recorded by stand-ins (cards, buttons with
 * their hit indices, fields with their texts), the desktop's printers are
 * a table the test fills, and the requests it asks are recorded.  Edit on
 * a printer's row opens the Edit a Printer card with its name and path,
 * Save asks kl_system_printers_edit, the answer closes the card; Tab,
 * Enter and Esc in its fields; a printer gone meanwhile ends the edit.
 *
 *     host-page-printers
 *
 * Prints "PASS name" or "FAIL name ..." and exits with 1 when one failed.
 */

#include "userland/desktop/settings/settings.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What a frame drew. */
#define DRAWN_MAX	128U

/* One thing drawn: a card (its title and subtitle), a button (its label, index, enabled) or a field (its text). */
struct drawn {
	char kind;
	char label[128];
	char sub[128];
	int index;
	int enabled;
	int focused;
};

/* The checks that failed, the frame drawn, the desktop's printers and the requests asked. */
static int failures;
static struct drawn frame[DRAWN_MAX];
static size_t frame_count;
static struct kl_printer printers[KL_PRINTERS_MAX];
static size_t printer_count;
static uint32_t next_request = 1;
static uint32_t edit_printer;
static char edit_name[128];
static char edit_path[64];
static int edit_calls;

/* The appearance's colours, all zero: the test does not look at colours. */
static const struct se_palette test_palette;
const struct se_palette *se_palette = &test_palette;

int main(void);
static void check(const char *name, int passed, const char *detail);
static void draw(struct se_app *app);
static const struct drawn *find(char kind, const char *label);
static const struct drawn *find_index(int index);

/* The widgets the page draws, recorded. */
int
se_card_begin(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width,
	int height,
	const char *title,
	const char *subtitle)
{
	/* A card. */
	(void)app;
	(void)canvas;
	(void)x;
	(void)width;
	(void)height;
	if (frame_count < DRAWN_MAX) {
		memset(&frame[frame_count], 0, sizeof(frame[0]));
		frame[frame_count].kind = 'c';
		(void)snprintf(frame[frame_count].label, sizeof(frame[0].label), "%s", title);
		if (subtitle != NULL)
			(void)snprintf(frame[frame_count].sub, sizeof(frame[0].sub), "%s", subtitle);
		frame_count++;
	}
	return top + 64;
}

int
se_button_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	const char *label,
	int primary,
	int enabled,
	int index)
{
	/* A button. */
	(void)app;
	(void)canvas;
	(void)x;
	(void)y;
	(void)primary;
	if (frame_count < DRAWN_MAX) {
		memset(&frame[frame_count], 0, sizeof(frame[0]));
		frame[frame_count].kind = 'b';
		(void)snprintf(frame[frame_count].label, sizeof(frame[0].label), "%s", label);
		frame[frame_count].index = index;
		frame[frame_count].enabled = enabled;
		frame_count++;
	}
	return 80;
}

int
se_button_width(
	struct se_app *app,
	const char *label)
{
	/* Wide enough. */
	(void)app;
	return (int)strlen(label) * 8 + 24;
}

unsigned
se_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	struct kl_field *field,
	const struct kl_rect *rect,
	const char *placeholder,
	unsigned kind,
	int focused)
{
	/* A field and its text. */
	(void)app;
	(void)canvas;
	(void)rect;
	(void)placeholder;
	(void)kind;
	if (frame_count < DRAWN_MAX) {
		memset(&frame[frame_count], 0, sizeof(frame[0]));
		frame[frame_count].kind = 'f';
		(void)snprintf(frame[frame_count].label, sizeof(frame[0].label), "%.127s", field->text);
		frame[frame_count].focused = focused;
		frame_count++;
	}
	return 0;
}

void
se_ui_hit(
	struct se_app *app,
	const struct kl_rect *rect,
	unsigned kind,
	int index)
{
	/* The fields' places, not needed here. */
	(void)app;
	(void)rect;
	(void)kind;
	(void)index;
}

int
se_field_key(
	struct kl_field *field,
	const struct se_event *event)
{
	/* A typed letter is appended. */
	if (field->length + 1U < sizeof(field->text) && event->key >= 'a' && event->key <= 'z') {
		field->text[field->length] = (char)event->key;
		field->length++;
		field->text[field->length] = '\0';
		return 1;
	}
	return 0;
}

void
se_field_clear(
	struct kl_field *field)
{
	/* Empty. */
	memset(field->text, 0, sizeof(field->text));
	field->length = 0;
}

void
se_log(
	const char *format,
	...)
{
	/* Not kept. */
	(void)format;
}

void
kl_field_set(
	struct kl_field *field,
	const char *text)
{
	/* The text. */
	(void)snprintf(field->text, sizeof(field->text), "%s", text);
	field->length = strlen(field->text);
}

int
kl_text_center(
	unsigned pixels,
	int top,
	int height)
{
	/* The middle. */
	(void)pixels;
	return top + height / 2;
}

int
kl_text_width(
	struct kl_text *text,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold)
{
	/* Eight a character. */
	(void)text;
	(void)string;
	(void)pixels;
	(void)bold;
	return (int)length * 8;
}

int
kl_text_draw(
	struct kl_text *text,
	struct kl_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	size_t length,
	unsigned pixels,
	int bold,
	kl_color color)
{
	/* A text, recorded. */
	(void)text;
	(void)canvas;
	(void)x;
	(void)baseline;
	(void)pixels;
	(void)bold;
	(void)color;
	if (frame_count < DRAWN_MAX) {
		memset(&frame[frame_count], 0, sizeof(frame[0]));
		frame[frame_count].kind = 't';
		(void)snprintf(frame[frame_count].label, sizeof(frame[0].label), "%.*s", (int)length, string);
		frame_count++;
	}
	return 0;
}

int
kl_text_draw_fit(
	struct kl_text *text,
	struct kl_canvas *canvas,
	int x,
	int baseline,
	const char *string,
	unsigned pixels,
	int bold,
	int width,
	kl_color color)
{
	/* A text, recorded. */
	(void)width;
	return kl_text_draw(text, canvas, x, baseline, string, strlen(string), pixels, bold, color);
}

/* The desktop's printers, stood in for. */
unsigned
kl_system_capabilities(
	const struct kl_system *system)
{
	/* Printing. */
	(void)system;
	return KL_SYSTEM_HAS_PRINTERS;
}

size_t
kl_system_printers_get(
	const struct kl_system *system,
	struct kl_printer *list,
	size_t capacity)
{
	size_t count;

	/* The test's table. */
	(void)system;
	count = printer_count < capacity ? printer_count : capacity;
	memcpy(list, printers, count * sizeof(list[0]));
	return count;
}

size_t
kl_system_print_jobs_get(
	const struct kl_system *system,
	struct kl_print_job *jobs,
	size_t capacity)
{
	/* None. */
	(void)system;
	(void)jobs;
	(void)capacity;
	return 0;
}

int
kl_system_printers_add(
	struct kl_system *system,
	unsigned protocol,
	const char *host,
	unsigned port,
	const char *path,
	uint32_t *request)
{
	/* Asked. */
	(void)system;
	(void)protocol;
	(void)host;
	(void)port;
	(void)path;
	*request = next_request++;
	return 0;
}

int
kl_system_printers_remove(
	struct kl_system *system,
	uint32_t printer,
	uint32_t *request)
{
	/* Asked. */
	(void)system;
	(void)printer;
	*request = next_request++;
	return 0;
}

int
kl_system_printers_set_default(
	struct kl_system *system,
	uint32_t printer,
	uint32_t *request)
{
	/* Asked. */
	(void)system;
	(void)printer;
	*request = next_request++;
	return 0;
}

int
kl_system_print_cancel(
	struct kl_system *system,
	uint32_t job,
	uint32_t *request)
{
	/* Asked. */
	(void)system;
	(void)job;
	*request = next_request++;
	return 0;
}

int
kl_system_printers_edit(
	struct kl_system *system,
	uint32_t printer,
	const char *name,
	const char *path,
	uint32_t *request)
{
	/* Asked, recorded. */
	(void)system;
	edit_calls++;
	edit_printer = printer;
	(void)snprintf(edit_name, sizeof(edit_name), "%s", name);
	(void)snprintf(edit_path, sizeof(edit_path), "%s", path);
	*request = next_request++;
	return 0;
}

/* Runs the checks. */
int
main(void)
{
	static struct se_app app;
	struct se_event event;
	const struct drawn *found;
	const struct drawn *field;
	uint32_t asked;
	size_t index;
	int handled;
	int fields;

	/* Two printers: an IPP one (the default) and an LPD one. */
	memset(&app, 0, sizeof(app));
	app.system = (struct kl_system *)&app;
	printers[0].id = 1;
	printers[0].protocol = KL_PRINTER_IPP;
	(void)snprintf(printers[0].host, sizeof(printers[0].host), "192.168.1.20");
	printers[0].port = 631;
	(void)snprintf(printers[0].path, sizeof(printers[0].path), "/ipp/print");
	(void)snprintf(printers[0].name, sizeof(printers[0].name), "Office");
	printers[0].flags = KL_PRINTER_DEFAULT;
	printers[1].id = 2;
	printers[1].protocol = KL_PRINTER_LPD;
	(void)snprintf(printers[1].host, sizeof(printers[1].host), "192.168.1.30");
	printers[1].port = 515;
	(void)snprintf(printers[1].path, sizeof(printers[1].path), "lp");
	(void)snprintf(printers[1].name, sizeof(printers[1].name), "Basement (LPD)");
	printer_count = 2;

	/* 1. Each printer's row has Edit; no edit card yet. */
	draw(&app);
	check("edit-buttons", find_index(400) != NULL && find_index(401) != NULL, "Edit at 400 and 401");
	check("no-editor", find('c', "Edit a Printer") == NULL, "no Edit a Printer card");

	/* 2. Edit on the LPD printer: its card with its name, the fields with its name and queue, the name focused. */
	se_printers_press(&app, 401);
	draw(&app);
	found = find('c', "Edit a Printer");
	check("editor-card", found != NULL && strcmp(found->sub, "Basement (LPD)") == 0, found != NULL ? found->sub : "none");
	fields = 0;
	for (index = 0; index < frame_count; index++) {
		if (frame[index].kind != 'f')
			continue;
		if (strcmp(frame[index].label, "Basement (LPD)") == 0 && frame[index].focused)
			fields |= 1;
		if (strcmp(frame[index].label, "lp") == 0)
			fields |= 2;
	}
	check("editor-fields", fields == 3, "name focused, queue lp");

	/* 3. Typing, Tab to the queue, typing there, Enter saves. */
	memset(&event, 0, sizeof(event));
	event.key = 'x';
	handled = se_printers_key(&app, &event);
	event.key = SE_KEY_TAB;
	(void)se_printers_key(&app, &event);
	event.key = 'r';
	(void)se_printers_key(&app, &event);
	event.key = SE_KEY_ENTER;
	(void)se_printers_key(&app, &event);
	check("save-asked", handled && edit_calls == 1 && edit_printer == 2U && strcmp(edit_name, "Basement (LPD)x") == 0 && strcmp(edit_path, "lpr") == 0,
	    edit_name);

	/* 4. While the answer is out, Save is disabled; the answer closes the card with its line. */
	draw(&app);
	found = find_index(11);
	check("save-disabled-while-asked", found != NULL && !found->enabled, "Save disabled");
	asked = app.printers.request;
	handled = se_printers_result(&app, asked, 0);
	draw(&app);
	check("answer-closes", handled && find('c', "Edit a Printer") == NULL && strcmp(app.printers.message, "The printer is changed.") == 0,
	    app.printers.message);

	/* 5. Cancel leaves without asking. */
	se_printers_press(&app, 400);
	se_printers_press(&app, 12);
	draw(&app);
	check("cancel", find('c', "Edit a Printer") == NULL && edit_calls == 1, "no request, no card");

	/* 6. A printer gone while it is edited ends the edit. */
	se_printers_press(&app, 401);
	printer_count = 1;
	draw(&app);
	draw(&app);
	check("printer-gone", find('c', "Edit a Printer") == NULL && app.printers.editing == 0U, "the edit ended");

	/* 7. Only an Edit for the printer left; the addition's form is still there. */
	field = find('c', "Add a Printer");
	check("add-card-stays", field != NULL && find_index(401) == NULL && find_index(400) != NULL, "Add a Printer, Edit 400 only");

	/* The outcome. */
	printf("host-page-printers: %s\n", failures == 0 ? "PASS" : "FAIL");
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

/* Draws the page once, the frame recorded anew. */
static void
draw(
	struct se_app *app)
{
	/* A new frame. */
	frame_count = 0;
	(void)se_printers_draw(app, NULL, 0, 0, 800);
}

/* Finds a thing drawn by its kind and label. */
static const struct drawn *
find(
	char kind,
	const char *label)
{
	size_t index;

	/* Each thing drawn. */
	for (index = 0; index < frame_count; index++) {
		if (frame[index].kind == kind && strcmp(frame[index].label, label) == 0)
			return &frame[index];
	}
	return NULL;
}

/* Finds a button by its index. */
static const struct drawn *
find_index(
	int index)
{
	size_t at;

	/* Each button drawn. */
	for (at = 0; at < frame_count; at++) {
		if (frame[at].kind == 'b' && frame[at].index == index)
			return &frame[at];
	}
	return NULL;
}
