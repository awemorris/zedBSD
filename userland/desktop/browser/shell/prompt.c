/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The question about a page that does not respond, as other browsers ask
 * it: when a task of the page's scripts has run for ten seconds
 * (unresponsive.c's rule over browser_view_script_busy), a card in the
 * middle of the window says so and offers Wait (the main button: Enter
 * and Esc choose it too) and Stop script (browser_view_stop_script, which
 * ends that task and leaves the page).  A task that ends takes its card
 * away; Wait asks again after ten seconds more of the same task.
 *
 * Everything here is the window's thread's and none of it is the view's
 * work: the card is drawn with libkeiland's canvas, text and widgets into
 * memory of its own, which the drawing thread copies over the page's
 * frame (shell_render_overlay).  While the card shows, the pointer's input
 * over the content goes to it (the rest is dropped, as under a dialog) and
 * the keys but Enter and Esc to the shell's shortcuts only; the titlebar,
 * the window's size and the drawing go on.
 *
 * The words are English keys of libkeiland's translations (domain
 * "browser"), read in the desktop's language; where the translations are
 * not there (Windows) the English shows.
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The domain of the browser's translations. */
#define PROMPT_DOMAIN		"browser"

/* The card's widest, narrowest, and its room from the window's sides, in pixels. */
#define PROMPT_WIDTH_MAX	460
#define PROMPT_WIDTH_MIN	200
#define PROMPT_MARGIN		16

/* The card's padding, the gap between its parts, and the room between its buttons. */
#define PROMPT_PAD		20
#define PROMPT_GAP		10
#define PROMPT_BUTTON_GAP	8

/* The title's and the explanation's sizes, the explanation's line and its most lines. */
#define PROMPT_TITLE_PX		17U
#define PROMPT_BODY_PX		14U
#define PROMPT_BODY_LINE	20
#define PROMPT_LINES		4

/* The ids of the card's buttons for libkeiland's widgets. */
#define PROMPT_ID_WAIT		1U
#define PROMPT_ID_STOP		2U

/* How soon a card whose buttons still move is drawn again, in milliseconds. */
#define PROMPT_MOVING_MS	16

/* The longest page's name the explanation takes, in bytes. */
#define PROMPT_NAME_MAX		96U

/* The texts of the card, in English, which are the translations' keys (locale/browser.keys lists them). */
enum prompt_text {
	PROMPT_TEXT_TITLE,
	PROMPT_TEXT_BODY,
	PROMPT_TEXT_WAIT,
	PROMPT_TEXT_STOP,
	PROMPT_TEXT_THIS_PAGE,
	PROMPT_TEXTS
};

/* The English of each text of the card ({1} the page's name, {2} the seconds). */
static const char *const prompt_english[PROMPT_TEXTS] = {
	"This page isn't responding",
	"A script on {1} has been running for {2} seconds. You can wait for it or stop it.",
	"Wait",
	"Stop script",
	"this page"
};

/* What a drawing of the card says was chosen: nothing, Wait, or Stop script. */
enum prompt_choice {
	PROMPT_NONE,
	PROMPT_WAIT,
	PROMPT_STOP
};

static void prompt_show(struct shell_prompt *prompt, struct browser_view *view, struct shell_render *render, uint32_t milliseconds,
    uint64_t now_ms);
static void prompt_hide(struct shell_prompt *prompt, struct shell_render *render, const char *reason);
static void prompt_answer(struct shell_prompt *prompt, struct browser_view *view, struct shell_render *render, enum prompt_choice choice);
static void prompt_compose(struct shell_prompt *prompt);
static void prompt_layout(struct shell_prompt *prompt);
static enum prompt_choice prompt_draw(struct shell_prompt *prompt, struct shell_render *render, uint64_t now_ms);
static void prompt_name(struct browser_view *view, char *name, size_t size);
static const char *prompt_tr(enum prompt_text text);
static void prompt_fill(char *out, size_t size, const char *pattern, const char *first, const char *second);
static int prompt_inside(const struct shell_prompt *prompt, int x, int y);

/*
 * Makes the question ready: the widgets' input, the text in the fonts the
 * pages use (or the bundled data's), and the translations in the
 * desktop's language.  Without a font the question is never shown (a line
 * says so).
 */
void
shell_prompt_open(
	struct shell_prompt *prompt,
	const struct browser_fonts *fonts,
	const char *language)
{
	char directory[1024];
	char primary[1100];
	char fallback[1100];
	const char *sans;
	const char *other;
	int error;

	/* Nothing shows, no task is watched. */
	memset(prompt, 0, sizeof(*prompt));
	shell_unresponsive_init(&prompt->rule);

	/* The fonts: the pages' sans and fallback, or the bundled data's. */
	sans = NULL;
	other = NULL;
	if (fonts != NULL) {
		sans = fonts->sans;
		other = fonts->fallback;
	}
	if (sans == NULL) {
		error = kl_data_directory(directory, sizeof(directory));
		if (error == 0) {
			(void)snprintf(primary, sizeof(primary), "%s/fonts/keiland.ttf", directory);
			(void)snprintf(fallback, sizeof(fallback), "%s/fonts/keiland-fallback.ttf", directory);
			sans = primary;
			if (other == NULL)
				other = fallback;
		}
	}

	/* The text the card is written in. */
	error = ENOENT;
	if (sans != NULL)
		error = kl_text_open(&prompt->text, sans, other);
	if (error != 0) {
		printf("ZBROWSER ERROR prompt-font error=%d\n", error);
		fflush(stdout);
		return;
	}
	prompt->text_open = 1;

	/* The widgets' input. */
	prompt->ui = kl_ui_create();
	if (prompt->ui == NULL) {
		printf("ZBROWSER ERROR prompt-ui error=%d\n", ENOMEM);
		fflush(stdout);
		return;
	}

	/* The words in the desktop's language. */
	shell_prompt_language(prompt, NULL, language);
}

/*
 * Looks at the page's running task, on the window's thread: shows the
 * question when the task has run long enough, takes it away when the task
 * ended, and draws a card whose buttons still move.  Returns in how many
 * milliseconds to look again (-1: no need).
 */
int
shell_prompt_round(
	struct shell_prompt *prompt,
	struct browser_view *view,
	struct shell_render *render,
	uint64_t now_ms)
{
	enum shell_unresponsive_change change;
	enum prompt_choice choice;
	uint32_t milliseconds;
	uint32_t task;
	int timeout;
	int busy;

	/* A question that cannot be drawn is never asked. */
	if (prompt->ui == NULL)
		return -1;

	/* The task that runs now, and what the rule makes of it. */
	busy = browser_view_script_busy(view, &task, &milliseconds);
	change = shell_unresponsive_step(&prompt->rule, busy, task, milliseconds, &timeout);
	if (change == SHELL_UNRESPONSIVE_HIDE)
		prompt_hide(prompt, render, "ended");
	if (change == SHELL_UNRESPONSIVE_SHOW) {
		if (prompt->shown)
			prompt_hide(prompt, render, "ended");
		prompt->task = task;
		prompt_show(prompt, view, render, milliseconds, now_ms);
	}

	/* A card whose buttons still move (their light), drawn again soon. */
	if (prompt->shown && prompt->moving) {
		choice = prompt_draw(prompt, render, now_ms);
		if (choice != PROMPT_NONE)
			prompt_answer(prompt, view, render, choice);
		if (prompt->shown && prompt->moving && timeout > PROMPT_MOVING_MS)
			timeout = PROMPT_MOVING_MS;
	}

	/* Succeeded: when to look again. */
	return timeout;
}

/*
 * Takes an input of the window while the question shows: the pointer over
 * the card to its buttons (over the rest of the content it is dropped),
 * and Enter and Esc to Wait.  Returns 1 when the input was the question's
 * (taken or dropped), 0 when the window goes on with it (a key the
 * shell's shortcuts may take, the focus, a later start's location).
 */
int
shell_prompt_input(
	struct shell_prompt *prompt,
	struct browser_view *view,
	struct shell_render *render,
	const struct shell_event *event,
	uint64_t now_ms)
{
	enum prompt_choice choice;
	int changed;
	int pressed;
	int inside;

	/* No question, nothing taken. */
	if (!prompt->shown)
		return 0;

	/* Each kind of input. */
	changed = 0;
	choice = PROMPT_NONE;
	switch (event->type) {
	case SHELL_EVENT_MOTION:
	case SHELL_EVENT_BUTTON:
		/* The pointer over the card is the widgets', in the card's own place. */
		inside = prompt_inside(prompt, event->x, event->y);
		if (inside) {
			changed = kl_ui_pointer_motion(prompt->ui, (double)(event->x - prompt->x), (double)(event->y - prompt->y));
		} else {
			changed = kl_ui_pointer_leave(prompt->ui);
		}

		/* A left button over the card presses or lets go of what is under it. */
		if (event->type == SHELL_EVENT_BUTTON && inside && event->button == SHELL_BUTTON_LEFT) {
			pressed = kl_ui_pointer_button(prompt->ui, event->pressed, now_ms * 1000U);
			if (pressed)
				changed = 1;
		}
		break;
	case SHELL_EVENT_LEAVE:
		/* The pointer left the window. */
		changed = kl_ui_pointer_leave(prompt->ui);
		break;
	case SHELL_EVENT_SCROLL:
	case SHELL_EVENT_TEXT_COMMIT:
	case SHELL_EVENT_TEXT_PREEDIT:
	case SHELL_EVENT_TEXT_DELETE:
		/* The page's other input is dropped while the card shows (as under a dialog). */
		break;
	case SHELL_EVENT_KEY:
		/* Enter and Esc choose Wait; the other keys go to the shell's shortcuts. */
		if (event->key != KL_KEY_ENTER && event->key != KL_KEY_ESC)
			return 0;
		if (event->pressed)
			choice = PROMPT_WAIT;
		break;
	default:
		/* The focus and a later start's location are the window's. */
		return 0;
	}

	/* The card drawn again with the change, which tells what was clicked. */
	if (changed && choice == PROMPT_NONE)
		choice = prompt_draw(prompt, render, now_ms);
	if (choice != PROMPT_NONE)
		prompt_answer(prompt, view, render, choice);

	/* Succeeded: the input was the question's. */
	return 1;
}

/*
 * Places the card in the window's new size, and draws it again while it
 * shows.
 */
void
shell_prompt_resize(
	struct shell_prompt *prompt,
	struct shell_render *render,
	uint32_t width,
	uint32_t height,
	uint64_t now_ms)
{
	/* The size the card is placed in. */
	prompt->window_width = width;
	prompt->window_height = height;
	if (!prompt->shown)
		return;

	/* The card laid out and drawn at it. */
	prompt_layout(prompt);
	(void)prompt_draw(prompt, render, now_ms);
}

/*
 * Draws the card again (the desktop's appearance changed), while it shows.
 */
void
shell_prompt_redraw(
	struct shell_prompt *prompt,
	struct shell_render *render,
	uint64_t now_ms)
{
	/* Nothing shows. */
	if (!prompt->shown)
		return;

	/* The card in the theme now. */
	(void)prompt_draw(prompt, render, now_ms);
}

/*
 * Reads the browser's translations in a language ("en", "ja", ...; NULL
 * for English), and words a card that shows in it (render NULL: none can
 * show yet).
 */
void
shell_prompt_language(
	struct shell_prompt *prompt,
	struct shell_render *render,
	const char *language)
{
	int error;

	/* The catalogs; English needs none, and a system without them keeps the English. */
	if (language == NULL)
		language = "en";
	error = kl_tr_open(PROMPT_DOMAIN, language);
	if (error != 0 && error != ENOTSUP) {
		printf("ZBROWSER ERROR prompt-language language=%s error=%d\n", language, error);
		fflush(stdout);
	}

	/* A card that shows, in the new words. */
	if (render == NULL || !prompt->shown)
		return;
	prompt_compose(prompt);
	prompt_layout(prompt);
	(void)prompt_draw(prompt, render, shell_clock());
}

/*
 * Releases the question (the drawing thread's copy of the card is the
 * drawing thread's).
 */
void
shell_prompt_close(
	struct shell_prompt *prompt)
{
	/* The widgets, the text and the card's memory. */
	if (prompt->ui != NULL)
		kl_ui_destroy(prompt->ui);
	if (prompt->text_open)
		kl_text_close(&prompt->text);
	free(prompt->pixels);

	/* Nothing is held any more. */
	memset(prompt, 0, sizeof(*prompt));
}

/* Shows the question for the task the rule watches, which has run so long. */
static void
prompt_show(
	struct shell_prompt *prompt,
	struct browser_view *view,
	struct shell_render *render,
	uint32_t milliseconds,
	uint64_t now_ms)
{
	/* The page's name and the seconds, worded, and the card laid out. */
	prompt->seconds = milliseconds / 1000U;
	prompt_name(view, prompt->name, sizeof(prompt->name));
	prompt_compose(prompt);
	prompt_layout(prompt);

	/* The card, drawn with nothing under the pointer yet, and shown. */
	(void)kl_ui_pointer_leave(prompt->ui);
	prompt->shown = 1;
	(void)prompt_draw(prompt, render, now_ms);

	/* The line the tests read: the card and its buttons in the window. */
	printf("ZBROWSER PROMPT shown task=%u seconds=%u x=%d y=%d width=%d height=%d wait=%d,%d,%d,%d stop=%d,%d,%d,%d\n",
	    (unsigned)prompt->task, (unsigned)prompt->seconds, prompt->x, prompt->y, prompt->width, prompt->height,
	    prompt->x + prompt->wait.x, prompt->y + prompt->wait.y, prompt->wait.width, prompt->wait.height,
	    prompt->x + prompt->stop.x, prompt->y + prompt->stop.y, prompt->stop.width, prompt->stop.height);
	fflush(stdout);
}

/* Takes the card away, and says why in the line the tests read. */
static void
prompt_hide(
	struct shell_prompt *prompt,
	struct shell_render *render,
	const char *reason)
{
	/* The drawing thread draws the page alone again. */
	if (!prompt->shown)
		return;
	prompt->shown = 0;
	prompt->moving = 0;
	shell_render_overlay(render, NULL, 0U, 0U);

	/* The line. */
	printf("ZBROWSER PROMPT hidden task=%u reason=%s\n", (unsigned)prompt->task, reason);
	fflush(stdout);
}

/* Carries out the button chosen: Wait asks again later, Stop script ends the task; either takes the card away. */
static void
prompt_answer(
	struct shell_prompt *prompt,
	struct browser_view *view,
	struct shell_render *render,
	enum prompt_choice choice)
{
	uint32_t milliseconds;
	uint32_t task;
	int busy;
	int error;

	/* How long the task has run now (another task, or none: the rule starts again at the next look). */
	busy = browser_view_script_busy(view, &task, &milliseconds);
	if (!busy || task != prompt->task)
		milliseconds = 0U;

	/* Stop script: the task asked to end (the page stays). */
	if (choice == PROMPT_STOP) {
		error = browser_view_stop_script(view, prompt->task);
		printf("ZBROWSER PROMPT stop task=%u error=%d\n", (unsigned)prompt->task, error);
		fflush(stdout);
	} else {
		printf("ZBROWSER PROMPT wait task=%u\n", (unsigned)prompt->task);
		fflush(stdout);
	}

	/* The card goes; a task that goes on is asked about again later. */
	shell_unresponsive_answer(&prompt->rule, milliseconds);
	prompt_hide(prompt, render, "answered");
}

/* Words the card's title, explanation and buttons in the language read. */
static void
prompt_compose(
	struct shell_prompt *prompt)
{
	char seconds[16];
	const char *name;

	/* The explanation, with the page's name (this page, when it has none) and the seconds. */
	name = prompt->name;
	if (name[0] == '\0')
		name = prompt_tr(PROMPT_TEXT_THIS_PAGE);
	(void)snprintf(seconds, sizeof(seconds), "%u", (unsigned)prompt->seconds);
	prompt_fill(prompt->body, sizeof(prompt->body), prompt_tr(PROMPT_TEXT_BODY), name, seconds);

	/* The title and the buttons. */
	(void)snprintf(prompt->title, sizeof(prompt->title), "%s", prompt_tr(PROMPT_TEXT_TITLE));
	(void)snprintf(prompt->wait_label, sizeof(prompt->wait_label), "%s", prompt_tr(PROMPT_TEXT_WAIT));
	(void)snprintf(prompt->stop_label, sizeof(prompt->stop_label), "%s", prompt_tr(PROMPT_TEXT_STOP));
}

/* Lays the card out in the window: its size by its words, its place in the middle, its buttons at its bottom right. */
static void
prompt_layout(
	struct shell_prompt *prompt)
{
	struct kl_text_line title;
	const struct kl_theme *theme;
	struct kl_style style;
	size_t done;
	size_t length;
	int lines;
	int width;

	/* The width: the most, within the window's sides. */
	width = PROMPT_WIDTH_MAX;
	if (width > (int)prompt->window_width - 2 * PROMPT_MARGIN)
		width = (int)prompt->window_width - 2 * PROMPT_MARGIN;
	if (width < PROMPT_WIDTH_MIN)
		width = PROMPT_WIDTH_MIN;

	/* How many lines the explanation takes at that width. */
	done = 0U;
	for (lines = 0; prompt->body[done] != '\0' && lines < PROMPT_LINES; lines++) {
		length = kl_text_break(&prompt->text, prompt->body + done, PROMPT_BODY_PX, 0, width - 2 * PROMPT_PAD);
		if (length == 0U)
			break;
		done += length;
		while (prompt->body[done] == ' ')
			done++;
	}
	prompt->lines = lines;

	/* The height: the title, the lines and the buttons, padded. */
	theme = kl_theme_default();
	kl_text_metrics(&prompt->text, PROMPT_TITLE_PX, &title);
	prompt->width = width;
	prompt->height = PROMPT_PAD + title.height + PROMPT_GAP + lines * PROMPT_BODY_LINE + 2 * PROMPT_GAP + theme->control_height +
	    PROMPT_PAD;

	/* Its place, in the middle of the window (the drawing thread puts it there in its frame too). */
	prompt->x = ((int)prompt->window_width - prompt->width) / 2;
	prompt->y = ((int)prompt->window_height - prompt->height) / 2;
	if (prompt->x < 0)
		prompt->x = 0;
	if (prompt->y < 0)
		prompt->y = 0;

	/* The buttons from the right: Wait, the main one, then Stop script. */
	memset(&style, 0, sizeof(style));
	style.text = &prompt->text;
	style.theme = theme;
	prompt->wait.width = kl_button_width(&style, prompt->wait_label);
	prompt->wait.height = theme->control_height;
	prompt->wait.x = prompt->width - PROMPT_PAD - prompt->wait.width;
	prompt->wait.y = prompt->height - PROMPT_PAD - prompt->wait.height;
	prompt->stop.width = kl_button_width(&style, prompt->stop_label);
	prompt->stop.height = theme->control_height;
	prompt->stop.x = prompt->wait.x - PROMPT_BUTTON_GAP - prompt->stop.width;
	prompt->stop.y = prompt->wait.y;
}

/*
 * Draws the card into its memory and hands it to the drawing thread.
 * Reports the button the input since the last drawing clicked.
 */
static enum prompt_choice
prompt_draw(
	struct shell_prompt *prompt,
	struct shell_render *render,
	uint64_t now_ms)
{
	const struct kl_theme *theme;
	struct kl_text_line title;
	struct kl_text_line body;
	struct kl_canvas canvas;
	struct kl_style style;
	struct kl_rect rect;
	enum prompt_choice choice;
	uint32_t *pixels;
	size_t count;
	size_t done;
	size_t length;
	int baseline;
	int clicked;
	int line;
	int top;
	int error;

	/* Memory for the card at its size. */
	count = (size_t)prompt->width * (size_t)prompt->height;
	if (count > prompt->pixel_count) {
		pixels = realloc(prompt->pixels, count * sizeof(uint32_t));
		if (pixels == NULL) {
			printf("ZBROWSER ERROR prompt-draw error=%d\n", ENOMEM);
			fflush(stdout);
			return PROMPT_NONE;
		}
		prompt->pixels = pixels;
		prompt->pixel_count = count;
	}

	/* The canvas over it. */
	error = kl_canvas_init(&canvas, prompt->pixels, (size_t)prompt->width, prompt->width, prompt->height);
	if (error != 0) {
		printf("ZBROWSER ERROR prompt-draw error=%d\n", error);
		fflush(stdout);
		return PROMPT_NONE;
	}
	theme = kl_theme_default();
	memset(&style, 0, sizeof(style));
	style.canvas = &canvas;
	style.text = &prompt->text;
	style.theme = theme;

	/* The card: opaque (it is copied over the page), with its edge. */
	rect.x = 0;
	rect.y = 0;
	rect.width = prompt->width;
	rect.height = prompt->height;
	kl_canvas_fill(&canvas, &rect, theme->panel | 0xff000000U);
	kl_canvas_round_border(&canvas, 0.5f, 0.5f, (float)prompt->width - 1.0f, (float)prompt->height - 1.0f, 0.0f, 1.0f,
	    theme->panel_edge);

	/* The title, bold, on its line under the padding. */
	kl_text_metrics(&prompt->text, PROMPT_TITLE_PX, &title);
	kl_text_metrics(&prompt->text, PROMPT_BODY_PX, &body);
	(void)kl_text_draw_fit(&prompt->text, &canvas, PROMPT_PAD, PROMPT_PAD + title.ascent, prompt->title, PROMPT_TITLE_PX, 1,
	    prompt->width - 2 * PROMPT_PAD, theme->text);

	/* The explanation, broken into its lines under the title. */
	done = 0U;
	top = PROMPT_PAD + title.height + PROMPT_GAP;
	for (line = 0; line < prompt->lines; line++) {
		length = kl_text_break(&prompt->text, prompt->body + done, PROMPT_BODY_PX, 0, prompt->width - 2 * PROMPT_PAD);
		if (length == 0U)
			break;
		baseline = top + line * PROMPT_BODY_LINE + (PROMPT_BODY_LINE - body.height) / 2 + body.ascent;
		(void)kl_text_draw(&prompt->text, &canvas, PROMPT_PAD, baseline, prompt->body + done, length, PROMPT_BODY_PX, 0,
		    theme->text_secondary);
		done += length;
		while (prompt->body[done] == ' ')
			done++;
	}

	/* The buttons, which tell what the input since the last drawing clicked. */
	choice = PROMPT_NONE;
	kl_ui_begin(prompt->ui, now_ms * 1000U);
	clicked = kl_button(prompt->ui, &style, PROMPT_ID_STOP, &prompt->stop, prompt->stop_label, 0U);
	if (clicked)
		choice = PROMPT_STOP;
	clicked = kl_button(prompt->ui, &style, PROMPT_ID_WAIT, &prompt->wait, prompt->wait_label, KL_BUTTON_PRIMARY);
	if (clicked)
		choice = PROMPT_WAIT;
	prompt->moving = kl_ui_end(prompt->ui, now_ms * 1000U);
	kl_canvas_release(&canvas);

	/* The card, to the drawing thread. */
	shell_render_overlay(render, prompt->pixels, (uint32_t)prompt->width, (uint32_t)prompt->height);

	/* Succeeded: what was clicked, if anything. */
	return choice;
}

/* Writes the page's name for the explanation: its title, else its location's host, else its location (empty: none). */
static void
prompt_name(
	struct browser_view *view,
	char *name,
	size_t size)
{
	const char *title;
	const char *url;
	const char *host;
	const char *end;
	size_t length;

	/* The title, when the page has one of its own (the view gives the location for a page without). */
	name[0] = '\0';
	title = browser_view_title(view);
	url = browser_view_url(view);
	if (title != NULL && title[0] != '\0' && (url == NULL || strcmp(title, url) != 0)) {
		(void)snprintf(name, size, "%s", title);
	} else if (url != NULL) {
		/* The host, between "//" and the path. */
		host = strstr(url, "://");
		end = NULL;
		if (host != NULL) {
			host += 3;
			end = strpbrk(host, "/?#");
		}
		if (host != NULL && end != host && host[0] != '\0') {
			length = strlen(host);
			if (end != NULL)
				length = (size_t)(end - host);
			(void)snprintf(name, size, "%.*s", (int)length, host);
		} else {
			(void)snprintf(name, size, "%s", url);
		}
	}

	/* Cut to its longest, at the start of a character. */
	length = strlen(name);
	if (length <= PROMPT_NAME_MAX)
		return;
	length = PROMPT_NAME_MAX;
	while (length > 0U && ((unsigned char)name[length] & 0xc0U) == 0x80U)
		length--;
	name[length] = '\0';
}

/* Gives a text of the card in the language read (the English where there is no translation). */
static const char *
prompt_tr(
	enum prompt_text text)
{
	const char *translated;

	/* The catalog's text (none where the translations are not there). */
	translated = kl_tr(prompt_english[text]);
	if (translated == NULL)
		return prompt_english[text];

	/* Succeeded: the text. */
	return translated;
}

/* Writes a pattern with its places {1} and {2} filled (as kl_tr_format, which a system without the translations lacks). */
static void
prompt_fill(
	char *out,
	size_t size,
	const char *pattern,
	const char *first,
	const char *second)
{
	const char *with;
	size_t used;
	size_t length;

	/* Each byte of the pattern, or a place's string, while there is room (and one byte for the end). */
	used = 0U;
	while (*pattern != '\0' && used + 1U < size) {
		/* A place: its string, cut to the room left. */
		with = NULL;
		if (pattern[0] == '{' && pattern[1] == '1' && pattern[2] == '}')
			with = first;
		if (pattern[0] == '{' && pattern[1] == '2' && pattern[2] == '}')
			with = second;
		if (with != NULL) {
			length = strlen(with);
			if (length > size - 1U - used)
				length = size - 1U - used;
			memcpy(out + used, with, length);
			used += length;
			pattern += 3;
			continue;
		}

		/* A byte of the pattern. */
		out[used++] = *pattern++;
	}

	/* The end. */
	out[used] = '\0';
}

/* Reports whether a place of the window is on the card. */
static int
prompt_inside(
	const struct shell_prompt *prompt,
	int x,
	int y)
{
	/* Left of it, right of it, above, below. */
	if (x < prompt->x || x >= prompt->x + prompt->width)
		return 0;
	if (y < prompt->y || y >= prompt->y + prompt->height)
		return 0;

	/* Succeeded: on the card. */
	return 1;
}
