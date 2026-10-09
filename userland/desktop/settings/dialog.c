/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The popup of Settings (ws199-p001 section 3.2): a card in the middle of
 * the page with the rest of the window darkened under it, which takes the
 * pointer and the keyboard while it is open.  It has a title, the step
 * ("Step 2 of 5"), a text of a few lines, up to two fields, a line for
 * what went wrong, a small link, and the buttons Back, Cancel and the
 * step's own.  While the desktop works it is busy: a turning ring and a
 * line instead of the fields, the buttons grey, Cancel alone pressable
 * when the work can be cancelled.
 *
 * The popup only shows and takes input; its owner (a wizard, as the
 * Security Keys page's) decides the steps: it sets them with the
 * se_dialog_* calls and hears the buttons, Enter, Esc and the idle time
 * through its act function.  Its ready function says whether the step's
 * button may be pressed.  The fields' text is wiped when they are set
 * again, when the popup closes and at the window's end (se_dialog_close).
 */

#include "settings.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The popup's controls: its fields, then Back, Cancel, the step's button and the link. */
#define DIALOG_FIELD_FIRST	9000
#define DIALOG_BACK		9010
#define DIALOG_CANCEL		9011
#define DIALOG_PRIMARY		9012
#define DIALOG_LINK		9013

/* The card's widest, its margin from the page's edges, its inner margin and its corner. */
#define DIALOG_WIDTH		460
#define DIALOG_EDGE		24
#define DIALOG_PAD		24
#define DIALOG_RADIUS		14.0f

/* The text sizes: the title, the step and the small lines, and the text. */
#define DIALOG_TEXT_TITLE	18U
#define DIALOG_TEXT_SMALL	13U
#define DIALOG_TEXT_BODY	14U

/* The rows: a line of the text, a field's label and its box, the busy line, the error, the link and the buttons. */
#define DIALOG_LINE		20
#define DIALOG_BODY_LINES	5
#define DIALOG_ERROR_LINES	2
#define DIALOG_LABEL		22
#define DIALOG_FIELD		36
#define DIALOG_FIELD_GAP	12
#define DIALOG_BUSY		44
#define DIALOG_SMALL_ROW	24
#define DIALOG_BUTTONS		32
#define DIALOG_GAP		16

/* The busy ring: its radius, its line, and how long a turn of its arc takes. */
#define DIALOG_RING		10.0f
#define DIALOG_RING_LINE	3.0f
#define DIALOG_RING_MS		1200U

/* How often the busy ring is drawn again, and how long the popup waits for input before its owner hears it is idle. */
#define DIALOG_FRAME_MS		80
#define DIALOG_IDLE_MS		120000U

static int dialog_lines(struct se_app *app, const char *text, unsigned pixels, int width, int most);
static int dialog_lines_draw(struct se_app *app, struct kl_canvas *canvas, const char *text, int x, int top, int width, unsigned pixels, int step, int most, kl_color color);
static void dialog_act(struct se_app *app, unsigned action);
static void dialog_place(struct se_app *app, struct kl_rect *card);

/*
 * Opens the popup for an owner, empty: the owner then sets its first step.
 * act hears the buttons and the keys (SE_DIALOG_*); ready says whether the
 * step's button may be pressed (NULL: always).
 */
void
se_dialog_open(
	struct se_app *app,
	void (*act)(struct se_app *app, unsigned action),
	int (*ready)(const struct se_app *app))
{
	struct se_dialog *dialog;

	/* Whatever an earlier popup held goes first. */
	se_dialog_close(app);

	/* Open, its owner's, and new input now. */
	dialog = &app->dialog;
	dialog->open = 1;
	dialog->act = act;
	dialog->ready = ready;
	dialog->input_ms = app->now;
	app->dirty = 1;
}

/* Closes the popup and wipes its fields (also at the window's end). */
void
se_dialog_close(
	struct se_app *app)
{
	struct se_dialog *dialog;
	int index;

	/* The fields' text, overwritten. */
	dialog = &app->dialog;
	for (index = 0; index < SE_DIALOG_FIELDS; index++)
		se_field_clear(&dialog->fields[index]);

	/* Nothing open. */
	memset(dialog, 0, sizeof(*dialog));
	app->dirty = 1;
}

/* Cancels the popup as its Cancel would (going to another page). */
void
se_dialog_dismiss(
	struct se_app *app)
{
	/* The owner hears Cancel; without one the popup just closes. */
	if (app->dialog.act != NULL) {
		app->dialog.act(app, SE_DIALOG_CANCEL);
		return;
	}

	/* No owner: closed. */
	se_dialog_close(app);
}

/*
 * Shows a step: its title, its place among the steps (0 steps: none
 * shown), its text and its button (NULL: none); whether Back is offered.
 * The fields, the error, the link and the busy state are cleared.
 */
void
se_dialog_step(
	struct se_app *app,
	const char *title,
	unsigned step,
	unsigned steps,
	const char *body,
	const char *primary,
	int can_back)
{
	struct se_dialog *dialog;
	int index;

	/* No field, no error, no link, not busy. */
	dialog = &app->dialog;
	for (index = 0; index < SE_DIALOG_FIELDS; index++)
		se_field_clear(&dialog->fields[index]);
	dialog->field_count = 0U;
	dialog->focus = 0;
	dialog->error[0] = '\0';
	dialog->link[0] = '\0';
	dialog->busy = 0;
	dialog->cancellable = 0;
	dialog->final = 0;

	/* The step. */
	(void)snprintf(dialog->title, sizeof(dialog->title), "%s", title);
	(void)snprintf(dialog->body, sizeof(dialog->body), "%s", body);
	dialog->primary[0] = '\0';
	if (primary != NULL)
		(void)snprintf(dialog->primary, sizeof(dialog->primary), "%s", primary);
	dialog->step = step;
	dialog->steps = steps;
	dialog->can_back = can_back;
	dialog->input_ms = app->now;
	app->dirty = 1;
}

/*
 * Adds a field to the step: its label, its placeholder, its kind
 * (SE_FIELD_*) and the most bytes it takes (0: the field's own).  The
 * first field has the keyboard.
 */
void
se_dialog_field(
	struct se_app *app,
	const char *label,
	const char *placeholder,
	unsigned kind,
	size_t limit)
{
	struct se_dialog *dialog;
	unsigned index;

	/* No room. */
	dialog = &app->dialog;
	if (dialog->field_count == SE_DIALOG_FIELDS)
		return;

	/* The field, empty. */
	index = dialog->field_count;
	(void)snprintf(dialog->labels[index], sizeof(dialog->labels[index]), "%s", label);
	(void)snprintf(dialog->placeholders[index], sizeof(dialog->placeholders[index]), "%s", placeholder);
	dialog->kinds[index] = kind;
	dialog->digits[index] = 0U;
	se_field_clear(&dialog->fields[index]);
	if (limit != 0U)
		kl_field_set_limit(&dialog->fields[index], limit);
	dialog->field_count++;
	app->dirty = 1;
}

/* Makes a field of the step take digits only, at most so many (a PIN of digits). */
void
se_dialog_digits(
	struct se_app *app,
	unsigned index,
	size_t digits)
{
	/* Only a field the step has. */
	if (index >= app->dialog.field_count)
		return;

	/* Digits, so many. */
	app->dialog.digits[index] = digits;
}

/* Puts a text into a field of the step (a name the user may edit). */
void
se_dialog_set_text(
	struct se_app *app,
	unsigned index,
	const char *text)
{
	/* Only a field the step has. */
	if (index >= app->dialog.field_count)
		return;

	/* The text, the caret after it. */
	kl_field_set(&app->dialog.fields[index], text);
	app->dirty = 1;
}

/* Gives a field's text (empty for a field the step has not). */
const char *
se_dialog_text(
	const struct se_app *app,
	unsigned index)
{
	/* Only a field the step has. */
	if (index >= app->dialog.field_count)
		return "";

	/* Its text. */
	return app->dialog.fields[index].text;
}

/* Gives a field's length in bytes (0 for a field the step has not). */
size_t
se_dialog_length(
	const struct se_app *app,
	unsigned index)
{
	/* Only a field the step has. */
	if (index >= app->dialog.field_count)
		return 0U;

	/* Its length. */
	return app->dialog.fields[index].length;
}

/* Gives the keyboard to a field of the step. */
void
se_dialog_focus(
	struct se_app *app,
	unsigned index)
{
	/* Only a field the step has. */
	if (index >= app->dialog.field_count)
		return;

	/* The field. */
	app->dialog.focus = (int)index;
	app->dirty = 1;
}

/* Shows a small link under the fields (NULL: none); its press is SE_DIALOG_LINK. */
void
se_dialog_link(
	struct se_app *app,
	const char *text)
{
	/* None. */
	app->dialog.link[0] = '\0';
	if (text != NULL)
		(void)snprintf(app->dialog.link, sizeof(app->dialog.link), "%s", text);
	app->dirty = 1;
}

/* Shows what went wrong, in red under the fields (NULL: nothing). */
void
se_dialog_error(
	struct se_app *app,
	const char *text)
{
	/* Nothing. */
	app->dialog.error[0] = '\0';
	if (text != NULL)
		(void)snprintf(app->dialog.error, sizeof(app->dialog.error), "%s", text);
	app->dirty = 1;
}

/*
 * Makes the popup busy with a line (the fields' text wiped: a busy popup
 * has sent them), Cancel pressable when the work can be cancelled.
 */
void
se_dialog_busy(
	struct se_app *app,
	const char *text,
	int cancellable)
{
	struct se_dialog *dialog;
	int index;

	/* The fields were sent: wiped. */
	dialog = &app->dialog;
	for (index = 0; index < SE_DIALOG_FIELDS; index++)
		se_field_clear(&dialog->fields[index]);
	dialog->field_count = 0U;

	/* Busy. */
	dialog->busy = 1;
	dialog->cancellable = cancellable;
	(void)snprintf(dialog->busy_text, sizeof(dialog->busy_text), "%s", text);
	dialog->error[0] = '\0';
	app->dirty = 1;
}

/* Makes the step shown the wizard's end: its button alone closes it (no Cancel). */
void
se_dialog_final(
	struct se_app *app)
{
	/* The end. */
	app->dialog.final = 1;
	app->dirty = 1;
}

/*
 * Draws the popup over the window when it is open: the veil over
 * everything (which takes the clicks), then the card and its controls.
 */
void
se_dialog_draw(
	struct se_app *app,
	struct kl_canvas *canvas)
{
	struct se_dialog *dialog;
	struct kl_rect whole;
	struct kl_rect card;
	struct kl_rect box;
	char steps[32];
	const char *cancel;
	kl_color veil;
	float fraction;
	int enabled;
	int inner;
	int right;
	int button;
	int index;
	int x;
	int y;

	/* Nothing open. */
	dialog = &app->dialog;
	if (!dialog->open)
		return;

	/* The veil over the window: what is under it is not clickable. */
	whole.x = 0;
	whole.y = 0;
	whole.width = app->width;
	whole.height = app->height;
	veil = KL_RGBA(0x000000U, 0x66U);
	kl_canvas_fill(canvas, &whole, veil);
	se_ui_hit(app, &whole, SE_HIT_PAGE, -1);

	/* The card in the middle of the page. */
	dialog_place(app, &card);
	kl_canvas_shadow(canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, DIALOG_RADIUS, 18.0f, KL_RGBA(0x000000U, 0x50U));
	kl_canvas_round(canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, DIALOG_RADIUS, KL_RGB(SE_COLOR_CARD));
	kl_canvas_round_border(canvas, (float)card.x, (float)card.y, (float)card.width, (float)card.height, DIALOG_RADIUS, 1.0f, SE_COLOR_CARD_EDGE);
	x = card.x + DIALOG_PAD;
	inner = card.width - 2 * DIALOG_PAD;
	y = card.y + DIALOG_PAD;

	/* The step among the steps, then the title. */
	if (dialog->steps > 1U) {
		(void)snprintf(steps, sizeof(steps), "Step %u of %u", dialog->step, dialog->steps);
		(void)kl_text_draw_fit(app->text, canvas, x, y + 12, steps, DIALOG_TEXT_SMALL, 0, inner, SE_COLOR_TEXT_SECONDARY);
		y += DIALOG_SMALL_ROW;
	}

	/* The title. */
	(void)kl_text_draw_fit(app->text, canvas, x, y + 18, dialog->title, DIALOG_TEXT_TITLE, 1, inner, SE_COLOR_TITLE);
	y += 32;

	/* The text, and a gap after it. */
	if (dialog->body[0] != '\0')
		y = dialog_lines_draw(app, canvas, dialog->body, x, y, inner, DIALOG_TEXT_BODY, DIALOG_LINE, DIALOG_BODY_LINES, SE_COLOR_TEXT) + DIALOG_GAP / 2;

	/* Busy: the turning ring and the line. */
	if (dialog->busy) {
		fraction = (float)(app->now % DIALOG_RING_MS) / (float)DIALOG_RING_MS;
		kl_canvas_ring(canvas, (float)x + DIALOG_RING, (float)y + DIALOG_BUSY / 2.0f, DIALOG_RING, DIALOG_RING_LINE, 0.15f + 0.7f * fraction, SE_COLOR_ACCENT);
		(void)kl_text_draw_fit(app->text, canvas, x + 32, kl_text_center(DIALOG_TEXT_BODY, y, DIALOG_BUSY), dialog->busy_text, DIALOG_TEXT_BODY, 0, inner - 32, SE_COLOR_TEXT);
		y += DIALOG_BUSY;
	}

	/* Each field under its label; the one with the keyboard has the focus. */
	for (index = 0; index < (int)dialog->field_count; index++) {
		(void)kl_text_draw_fit(app->text, canvas, x, y + 15, dialog->labels[index], DIALOG_TEXT_SMALL, 0, inner, SE_COLOR_TEXT_SECONDARY);
		y += DIALOG_LABEL;
		box.x = x;
		box.y = y;
		box.width = inner;
		box.height = DIALOG_FIELD;
		se_ui_hit(app, &box, SE_HIT_CONTROL, DIALOG_FIELD_FIRST + index);
		(void)se_field_draw(app, canvas, &dialog->fields[index], &box, dialog->placeholders[index], dialog->kinds[index], dialog->focus == index);
		y += DIALOG_FIELD + DIALOG_FIELD_GAP;
	}

	/* What went wrong, in up to two lines. */
	if (dialog->error[0] != '\0')
		y = dialog_lines_draw(app, canvas, dialog->error, x, y, inner, DIALOG_TEXT_SMALL, DIALOG_SMALL_ROW - 6, DIALOG_ERROR_LINES, SE_COLOR_BAD) + 6;

	/* The link, when not busy. */
	if (dialog->link[0] != '\0' && !dialog->busy) {
		box.x = x;
		box.y = y;
		box.width = kl_text_width(app->text, dialog->link, strlen(dialog->link), DIALOG_TEXT_SMALL, 0);
		box.height = DIALOG_SMALL_ROW;
		(void)kl_text_draw_fit(app->text, canvas, x, y + 15, dialog->link, DIALOG_TEXT_SMALL, 0, inner, SE_COLOR_ACCENT);
		se_ui_hit(app, &box, SE_HIT_CONTROL, DIALOG_LINK);
		y += DIALOG_SMALL_ROW;
	}

	/* The buttons at the right: the step's, Cancel left of it, Back left of that. */
	y += DIALOG_GAP / 2;
	right = card.x + card.width - DIALOG_PAD;
	if (dialog->primary[0] != '\0') {
		enabled = !dialog->busy;
		if (enabled && dialog->ready != NULL)
			enabled = dialog->ready(app);
		button = se_button_width(app, dialog->primary);
		right -= button;
		(void)se_button_draw(app, canvas, right, y, dialog->primary, 1, enabled, DIALOG_PRIMARY);
		right -= 8;
	}

	/* Cancel (Close at the end, which has no step's button), pressable while busy only when the work can be cancelled. */
	cancel = "Cancel";
	if (dialog->primary[0] == '\0')
		cancel = "Close";
	if (!dialog->final) {
		button = se_button_width(app, cancel);
		right -= button;
		enabled = !dialog->busy || dialog->cancellable;
		(void)se_button_draw(app, canvas, right, y, cancel, 0, enabled, DIALOG_CANCEL);
	}

	/* Back, on the left, when the step offers it. */
	if (dialog->can_back) {
		button = se_button_width(app, "Back");
		(void)se_button_draw(app, canvas, card.x + DIALOG_PAD, y, "Back", 0, !dialog->busy, DIALOG_BACK);
	}
}

/*
 * Carries out a click while the popup is open: its own controls, and
 * nothing else (the veil took the rest).  Returns 1 when the popup is
 * open.
 */
int
se_dialog_press(
	struct se_app *app,
	int index)
{
	struct se_dialog *dialog;

	/* Not open: the page's. */
	dialog = &app->dialog;
	if (!dialog->open)
		return 0;
	dialog->input_ms = app->now;

	/* A field takes the keyboard. */
	if (index >= DIALOG_FIELD_FIRST && index < DIALOG_FIELD_FIRST + (int)dialog->field_count) {
		dialog->focus = index - DIALOG_FIELD_FIRST;
		return 1;
	}

	/* The buttons and the link. */
	switch (index) {
	case DIALOG_PRIMARY:
		dialog_act(app, SE_DIALOG_PRIMARY);
		break;
	case DIALOG_CANCEL:
		dialog_act(app, SE_DIALOG_CANCEL);
		break;
	case DIALOG_BACK:
		dialog_act(app, SE_DIALOG_BACK);
		break;
	case DIALOG_LINK:
		dialog_act(app, SE_DIALOG_LINK);
		break;
	default:
		break;
	}

	/* Taken, whatever it was. */
	return 1;
}

/*
 * Takes a key while the popup is open: Tab moves between the fields,
 * Enter is the step's button (or the next field), Esc is Cancel when not
 * busy, and the others type.  Returns 1 when the popup is open (it takes
 * every key then).
 */
int
se_dialog_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_dialog *dialog;
	struct kl_field *field;
	uint32_t character;
	size_t digits;
	int ready;
	int count;

	/* Not open: the page's. */
	dialog = &app->dialog;
	if (!dialog->open)
		return 0;
	dialog->input_ms = app->now;
	count = (int)dialog->field_count;

	/* Esc: Cancel, unless busy with work that cannot be cancelled. */
	if (event->key == SE_KEY_ESC) {
		if (!dialog->busy || dialog->cancellable)
			dialog_act(app, SE_DIALOG_CANCEL);
		return 1;
	}

	/* Busy: nothing else. */
	if (dialog->busy)
		return 1;

	/* Tab and Shift+Tab between the fields. */
	if (event->key == SE_KEY_TAB) {
		if (count == 0)
			return 1;
		if ((event->modifiers & SE_MOD_SHIFT) != 0U) {
			dialog->focus = (dialog->focus + count - 1) % count;
		} else {
			dialog->focus = (dialog->focus + 1) % count;
		}

		/* Taken. */
		return 1;
	}

	/* Enter: the step's button when it may be pressed, else the next field. */
	if (event->key == SE_KEY_ENTER) {
		ready = dialog->primary[0] != '\0';
		if (ready && dialog->ready != NULL)
			ready = dialog->ready(app);
		if (ready) {
			dialog_act(app, SE_DIALOG_PRIMARY);
		} else if (count != 0) {
			dialog->focus = (dialog->focus + 1) % count;
		}

		/* Taken. */
		return 1;
	}

	/* No field: nothing to type into. */
	if (count == 0)
		return 1;

	/*
	 * A field of digits keeps digits only, so many at most: a character
	 * that is not a digit, or one more (without a selection it would
	 * replace), is taken and dropped before the field sees it (BUG-257).
	 */
	field = &dialog->fields[dialog->focus];
	digits = dialog->digits[dialog->focus];
	character = kl_key_character(event->key, event->modifiers);
	if (digits != 0U && character != 0U && (event->modifiers & SE_MOD_CTRL) == 0U) {
		if (character < '0' || character > '9')
			return 1;
		if (field->length >= digits && field->caret == field->anchor)
			return 1;
	}

	/* The others type into the field with the keyboard; a new character takes the error away. */
	(void)se_field_key(field, event);
	dialog->error[0] = '\0';
	return 1;
}

/*
 * Follows the time: the busy ring turns, and an open popup left alone
 * too long tells its owner (which wipes what it kept).
 */
void
se_dialog_tick(
	struct se_app *app,
	uint64_t now)
{
	struct se_dialog *dialog;

	/* Nothing open. */
	dialog = &app->dialog;
	if (!dialog->open)
		return;

	/* The ring turns. */
	if (dialog->busy) {
		app->dirty = 1;
		return;
	}

	/* Idle: the owner hears it once, and the time starts again. */
	if (now - dialog->input_ms >= DIALOG_IDLE_MS) {
		dialog->input_ms = now;
		dialog_act(app, SE_DIALOG_IDLE);
	}
}

/* Reports how long the main loop may sleep for the popup: a frame of the ring while busy, the idle time otherwise (-1: no limit). */
int
se_dialog_wait(
	const struct se_app *app)
{
	uint64_t passed;

	/* Nothing open. */
	if (!app->dialog.open)
		return -1;

	/* The ring's next frame. */
	if (app->dialog.busy)
		return DIALOG_FRAME_MS;

	/* The rest of the idle time. */
	passed = app->now - app->dialog.input_ms;
	if (passed >= DIALOG_IDLE_MS)
		return 0;
	return (int)(DIALOG_IDLE_MS - passed);
}

/* Hands an action to the popup's owner. */
static void
dialog_act(
	struct se_app *app,
	unsigned action)
{
	/* The owner, when it has one. */
	if (app->dialog.act != NULL)
		app->dialog.act(app, action);
	app->dirty = 1;
}

/* Works out the card's place: as wide as it may be in the page, as tall as its step, in the page's middle. */
static void
dialog_place(
	struct se_app *app,
	struct kl_rect *card)
{
	const struct kl_rect *page;
	const struct se_dialog *dialog;
	int height;
	int width;
	int lines;

	/* The width the page leaves. */
	page = &app->layout.page;
	dialog = &app->dialog;
	width = DIALOG_WIDTH;
	if (width > page->width - 2 * DIALOG_EDGE)
		width = page->width - 2 * DIALOG_EDGE;

	/* The height of what the step shows. */
	height = 2 * DIALOG_PAD + 32;
	if (dialog->steps > 1U)
		height += DIALOG_SMALL_ROW;
	lines = dialog_lines(app, dialog->body, DIALOG_TEXT_BODY, width - 2 * DIALOG_PAD, DIALOG_BODY_LINES);
	height += lines * DIALOG_LINE;
	if (lines != 0)
		height += DIALOG_GAP / 2;
	if (dialog->busy)
		height += DIALOG_BUSY;
	height += (int)dialog->field_count * (DIALOG_LABEL + DIALOG_FIELD + DIALOG_FIELD_GAP);
	if (dialog->error[0] != '\0') {
		lines = dialog_lines(app, dialog->error, DIALOG_TEXT_SMALL, width - 2 * DIALOG_PAD, DIALOG_ERROR_LINES);
		height += lines * (DIALOG_SMALL_ROW - 6) + 6;
	}

	/* The link, and the buttons. */
	if (dialog->link[0] != '\0' && !dialog->busy)
		height += DIALOG_SMALL_ROW;
	height += DIALOG_GAP / 2 + DIALOG_BUTTONS;

	/* In the page's middle. */
	card->width = width;
	card->height = height;
	card->x = page->x + (page->width - width) / 2;
	card->y = page->y + (page->height - height) / 2;
	if (card->y < page->y + DIALOG_EDGE)
		card->y = page->y + DIALOG_EDGE;
}

/* Counts the lines a text takes at a size and a width (at most most). */
static int
dialog_lines(
	struct se_app *app,
	const char *text,
	unsigned pixels,
	int width,
	int most)
{
	size_t done;
	size_t length;
	int lines;

	/* Each line, as much as fits. */
	done = 0U;
	lines = 0;
	while (text[done] != '\0' && lines < most) {
		length = kl_text_break(app->text, text + done, pixels, 0, width);
		lines++;
		if (length == 0U)
			break;

		/* The next line starts after the spaces the break left. */
		done += length;
		while (text[done] == ' ')
			done++;
	}

	/* The lines. */
	return lines;
}

/* Draws a text in lines (at most most) from a top edge, a line every step; returns the edge below the last line. */
static int
dialog_lines_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	const char *text,
	int x,
	int top,
	int width,
	unsigned pixels,
	int step,
	int most,
	kl_color color)
{
	size_t done;
	size_t length;
	int lines;
	int y;

	/* Each line; the last one allowed (or a word wider than the line) takes the rest, cut short. */
	done = 0U;
	lines = 0;
	y = top;
	while (text[done] != '\0' && lines < most) {
		length = kl_text_break(app->text, text + done, pixels, 0, width);
		if (lines + 1 == most || length == 0U) {
			(void)kl_text_draw_fit(app->text, canvas, x, y + 15, text + done, pixels, 0, width, color);
			y += step;
			break;
		}

		/* One line, broken where it fits. */
		(void)kl_text_draw(app->text, canvas, x, y + 15, text + done, length, pixels, 0, color);
		lines++;
		y += step;

		/* The next line starts after the spaces the break left. */
		done += length;
		while (text[done] == ' ')
			done++;
	}

	/* The edge below. */
	return y;
}
