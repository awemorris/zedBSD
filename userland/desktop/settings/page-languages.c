/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Languages page (WS154 p002, ws158-p004):
 *
 *   Input method      which input method the keyboard types through:
 *                     Japanese (Kei's), SKK, or none (English); one is
 *                     chosen at a time, and the desktop starts the input
 *                     method again with it at once (ime.method).
 *   Display language  the language of the desktop's words (ui.language,
 *                     WS158): English or Japanese, each named in its own
 *                     language; the desktop and its programs follow at once.
 *   Login screen      for an administrator only: the language of the login
 *                     screen (the system's, which the desktop reads,
 *                     ws188-p002: libkeiland's
 *                     kl_system_machine_login_language), changed through
 *                     the desktop's account administration (account-admin's
 *                     system-language) with the administrator's password.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The three choices' switches (hit indices), in ime.method's order plus one. */
#define LANGUAGES_NONE		1
#define LANGUAGES_JAPANESE	2
#define LANGUAGES_SKK		3

/* The display language's switches, the login screen's, its password's field and its Apply button. */
#define LANGUAGES_SHOW_EN	4
#define LANGUAGES_SHOW_JA	5
#define LANGUAGES_SYSTEM_EN	6
#define LANGUAGES_SYSTEM_JA	7
#define LANGUAGES_FIELD		8
#define LANGUAGES_APPLY		9

/* The card's margin, the space between cards, a choice's height and the text sizes. */
#define LANGUAGES_PAD		18
#define LANGUAGES_GAP		16
#define LANGUAGES_ROW		60
#define LANGUAGES_SHORT_ROW	48
#define LANGUAGES_TEXT_TITLE	15U
#define LANGUAGES_TEXT_SMALL	13U

/* Where the password's field starts in the card, and its height. */
#define LANGUAGES_FIELD_X	200
#define LANGUAGES_FIELD_HEIGHT	36

/* The languages, by ui.language's number: their codes, and their names in their own language (never translated). */
#define LANGUAGES_COUNT		2

/* One choice: its switch, the method it chooses, its name and what it does. */
struct languages_choice {
	int index;
	int method;
	const char *name;
	const char *line;
};

/* The choices, in the order shown. */
static const struct languages_choice languages_choices[] = {
	{ LANGUAGES_JAPANESE, 1, "Japanese", "Kei's input method: romaji to kana, converted a phrase at a time." },
	{ LANGUAGES_SKK, 2, "SKK", "Emacs's SKK: an upper-case letter starts a word, Space converts it." },
	{ LANGUAGES_NONE, 0, "None (English)", "The keys type as they are; no input method." },
};

static const char *const languages_codes[LANGUAGES_COUNT] = { "en", "ja" };
static const char *const languages_names[LANGUAGES_COUNT] = { "English", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" };

/* The words of account-admin's refusals, and what the page says for each. */
static const char *const languages_words[] = { "not-administrator", "bad-password", "busy" };

static int languages_draw_system(struct se_app *app, struct kl_canvas *canvas, int x, int top, int width);
static void languages_field_draw(struct se_app *app, struct kl_canvas *canvas, int x, int y, int width);
static int languages_ready(const struct se_app *app);
static void languages_apply(struct se_app *app);
static const char *languages_saying(const char *word);

/*
 * Draws the Languages page's cards from a top edge; returns the edge below
 * them.
 */
int
se_languages_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct kl_text_line line;
	const struct languages_choice *choice;
	int height;
	int y;
	size_t i;

	/* The input method's card: one row a choice, a switch at each. */
	height = se_card_height(0, 1) + 3 * LANGUAGES_ROW + 4;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Input method"), kl_tr("What the keyboard types through. The change applies at once."));
	kl_text_metrics(app->text, LANGUAGES_TEXT_TITLE, &line);
	for (i = 0; i < sizeof(languages_choices) / sizeof(languages_choices[0]); i++) {
		/* The name, what it does, and its switch (on for the method chosen). */
		choice = &languages_choices[i];
		(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_PAD + 2, y + 10 + line.ascent, kl_tr(choice->name), LANGUAGES_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
		(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_PAD + 2, y + 32 + line.ascent, kl_tr(choice->line), LANGUAGES_TEXT_SMALL, 0, width - 120, SE_COLOR_TEXT_SECONDARY);
		se_toggle_draw(app, canvas, x + width - LANGUAGES_PAD - 44, y + 16, app->look.ime_method == choice->method, app->look.writable, choice->index);
		y += LANGUAGES_ROW;
	}

	/* The display language's card: one row a language, named in its own language, a switch at each. */
	top += height + LANGUAGES_GAP;
	height = se_card_height(0, 1) + LANGUAGES_COUNT * LANGUAGES_SHORT_ROW + 4;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Display language"), kl_tr("The language of the desktop's words. The change applies at once."));
	for (i = 0; i < LANGUAGES_COUNT; i++) {
		/* The name, and its switch (on for the language shown). */
		(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_PAD + 2, y + 10 + line.ascent, languages_names[i], LANGUAGES_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
		se_toggle_draw(app, canvas, x + width - LANGUAGES_PAD - 44, y + 10, app->look.ui_language == (int)i, app->look.writable, LANGUAGES_SHOW_EN + (int)i);
		y += LANGUAGES_SHORT_ROW;
	}

	/* The login screen's card, for an administrator. */
	top += height;
	top = languages_draw_system(app, canvas, x, top, width);

	/* The edge below the cards. */
	return top;
}

/*
 * Draws the login screen's card from the edge below the cards above, when
 * the user is an administrator of a desktop that administers the accounts;
 * returns the edge below it (the same edge without it).
 */
static int
languages_draw_system(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int top,
	int width)
{
	struct se_languages *languages;
	struct kl_text_line line;
	const char *now;
	kl_color ink;
	int administer;
	int enabled;
	int height;
	int ready;
	int right;
	int apply;
	int y;
	int i;

	/* Whether the user is an administrator (the users asked of the desktop, as the Users page asks them). */
	se_users_load(app);
	administer = se_users_admin_available(app);
	if (!administer)
		return top;

	/* The system's language, asked of the desktop until it is known (and again after a change). */
	languages = &app->languages;
	se_machine_want(app, KL_MACHINE_LOGIN_LANGUAGE);

	/* The card: the language now, one row a language, the password, Apply and the answer. */
	top += LANGUAGES_GAP;
	height = se_card_height(0, 1) + 30 + LANGUAGES_COUNT * LANGUAGES_SHORT_ROW + 56 + 60;
	y = se_card_begin(app, canvas, x, top, width, height, kl_tr("Login screen"), kl_tr("The language of the login screen, for everyone on this computer."));
	now = kl_tr("Not set (English)");
	if (languages->system >= 0 && languages->system < LANGUAGES_COUNT)
		now = languages_names[languages->system];
	(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_PAD + 2, y + 18, kl_tr("Now"), LANGUAGES_TEXT_SMALL, 0, LANGUAGES_FIELD_X - 30, SE_COLOR_TEXT_SECONDARY);
	(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_FIELD_X, y + 18, now, LANGUAGES_TEXT_SMALL, 0, width - LANGUAGES_FIELD_X - 20, SE_COLOR_TEXT);
	y += 30;

	/* The languages to choose, each named in its own language. */
	kl_text_metrics(app->text, LANGUAGES_TEXT_TITLE, &line);
	for (i = 0; i < LANGUAGES_COUNT; i++) {
		/* The name, and its switch (on for the one chosen). */
		(void)kl_text_draw_fit(app->text, canvas, x + LANGUAGES_PAD + 2, y + 10 + line.ascent, languages_names[i], LANGUAGES_TEXT_TITLE, 1, width / 2, SE_COLOR_TEXT);
		se_toggle_draw(app, canvas, x + width - LANGUAGES_PAD - 44, y + 10, languages->chosen == i, 1, LANGUAGES_SYSTEM_EN + i);
		y += LANGUAGES_SHORT_ROW;
	}

	/* The administrator's password. */
	languages_field_draw(app, canvas, x, y, width);
	y += 56;

	/* Apply at the right, when a change and the password are there. */
	right = x + width - 20;
	apply = se_button_width(app, kl_tr("Apply"));
	ready = languages_ready(app);
	enabled = ready;
	(void)se_button_draw(app, canvas, right - apply, y + 4, kl_tr("Apply"), 1, enabled, LANGUAGES_APPLY);

	/* The last answer: green when it was made, red when it was not. */
	ink = SE_COLOR_GOOD;
	if (languages->message_bad)
		ink = SE_COLOR_BAD;
	if (languages->message[0] != '\0')
		(void)kl_text_draw_fit(app->text, canvas, x + 20, y + 22, languages->message, LANGUAGES_TEXT_SMALL, 0, right - apply - x - 40, ink);

	/* The edge below the card. */
	return top + height;
}

/* Draws the password's row: its label at the left, the field at the right with dots, and the cursor when it has the keyboard. */
static void
languages_field_draw(
	struct se_app *app,
	struct kl_canvas *canvas,
	int x,
	int y,
	int width)
{
	struct se_languages *languages;
	struct kl_rect box;

	/* The label. */
	languages = &app->languages;
	(void)kl_text_draw_fit(app->text, canvas, x + 20, kl_text_center(LANGUAGES_TEXT_SMALL, y + 8, LANGUAGES_FIELD_HEIGHT), kl_tr("Your password"), LANGUAGES_TEXT_SMALL, 0, LANGUAGES_FIELD_X - 30, SE_COLOR_TEXT);

	/* The field's place; a click on it gives it the keyboard. */
	box.x = x + LANGUAGES_FIELD_X;
	box.y = y + 8;
	box.width = width - LANGUAGES_FIELD_X - 20;
	box.height = LANGUAGES_FIELD_HEIGHT;
	se_ui_hit(app, &box, SE_HIT_CONTROL, LANGUAGES_FIELD);

	/* libkeiland's field: the password as dots, without an input method (ws090-p007). */
	(void)se_field_draw(app, canvas, &languages->password, &box, kl_tr("Needed for the change"), SE_FIELD_SECRET, languages->focused);
}

/*
 * Carries out a click on the page: an input method's switch (that method
 * is chosen; a click on the one chosen changes nothing), a display
 * language's, or the login screen's switches, field and Apply.
 */
void
se_languages_press(
	struct se_app *app,
	int index)
{
	const struct languages_choice *choice;
	struct se_languages *languages;
	int language;
	int ready;
	size_t i;

	/* The display language's switches: that language at once (a click on the one shown changes nothing). */
	languages = &app->languages;
	if (index == LANGUAGES_SHOW_EN || index == LANGUAGES_SHOW_JA) {
		language = index - LANGUAGES_SHOW_EN;
		if (!app->look.writable || app->look.ui_language == language)
			return;
		app->look.ui_language = language;
		se_look_set_number(app, "ui.language", language, 0);
		se_log("LANGUAGES ui language=%s", languages_codes[language]);
		app->dirty = 1;
		return;
	}

	/* The login screen's switches choose the language to set. */
	if (index == LANGUAGES_SYSTEM_EN || index == LANGUAGES_SYSTEM_JA) {
		languages->chosen = index - LANGUAGES_SYSTEM_EN;
		languages->message[0] = '\0';
		app->dirty = 1;
		return;
	}

	/* The field takes the keyboard. */
	if (index == LANGUAGES_FIELD) {
		languages->focused = 1;
		app->dirty = 1;
		return;
	}

	/* Apply, when a change and the password are there. */
	if (index == LANGUAGES_APPLY) {
		ready = languages_ready(app);
		if (ready)
			languages_apply(app);
		app->dirty = 1;
		return;
	}

	/* The choice of the switch. */
	choice = NULL;
	for (i = 0; i < sizeof(languages_choices) / sizeof(languages_choices[0]); i++) {
		/* The switch clicked. */
		if (languages_choices[i].index == index)
			choice = &languages_choices[i];
	}

	/* Not a choice, a settings file that cannot be written, or the method chosen already. */
	if (choice == NULL || !app->look.writable || app->look.ime_method == choice->method)
		return;

	/* Chosen, and told to the desktop through the settings. */
	app->look.ime_method = choice->method;
	se_look_set_number(app, "ime.method", choice->method, 1);
	se_log("LANGUAGES ime method=%d", choice->method);

	/* The page shows it. */
	app->dirty = 1;
}

/*
 * Takes a key while the login screen's password field has the keyboard:
 * Enter applies the change when it is ready, Esc empties the field (or,
 * when it is empty, gives the keyboard back and is the window's), the
 * others type.  Returns 1 when the key was the page's.
 */
int
se_languages_key(
	struct se_app *app,
	const struct se_event *event)
{
	struct se_languages *languages;
	int ready;
	int used;

	/* Only while the field has the keyboard. */
	languages = &app->languages;
	if (!languages->focused)
		return 0;

	/* Enter: the change, when it is ready. */
	app->dirty = 1;
	if (event->key == SE_KEY_ENTER) {
		ready = languages_ready(app);
		if (ready)
			languages_apply(app);
		return 1;
	}

	/* Esc empties the field; an empty one gives the keyboard back. */
	if (event->key == SE_KEY_ESC) {
		if (languages->password.length == 0) {
			languages->focused = 0;
			return 0;
		}

		/* Emptied. */
		se_field_clear(&languages->password);
		return 1;
	}

	/* Anything else types into the field (or is not the page's). */
	used = se_field_key(&languages->password, event);
	if (used == 0)
		return 0;

	/* A new character takes the last answer away. */
	if (!languages->asked)
		languages->message[0] = '\0';

	/* Succeeded: the field took the key. */
	return 1;
}

/*
 * Takes the answer of the login screen's change when it is the page's: the
 * system's language read again, or the refusal in words.  Returns 1 when
 * the request was the page's.
 */
int
se_languages_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	struct se_languages *languages;
	char word[KL_ACCOUNT_REASON_SIZE];
	const char *saying;
	int refused;

	/* Only the change the page asked. */
	languages = &app->languages;
	if (!languages->asked || request != languages->request)
		return 0;
	languages->asked = 0;
	app->dirty = 1;

	/*
	 * Made: the language is read again by the desktop, and the change's
	 * log line waits for that reading (se_languages_reloaded), so that it
	 * carries the language set (ws188-p002).
	 */
	if (error == 0) {
		(void)snprintf(languages->message, sizeof(languages->message), "%s", kl_tr("Changed. The login screen uses it from the next time it shows."));
		languages->message_bad = 0;
		languages->reload_for = request;
		languages->reload_request = se_machine_ask_now(app, KL_MACHINE_USERS | KL_MACHINE_LOGIN_LANGUAGE);
		if (languages->reload_request == 0U)
			se_languages_reloaded(app, ENOTSUP);
		return 1;
	}

	/* A refusal in words, or a failure (without any password in the log). */
	word[0] = '\0';
	refused = kl_system_account_refusal(app->system, request, word, sizeof(word));
	saying = kl_tr("The change could not be made.");
	if (refused)
		saying = languages_saying(word);
	(void)snprintf(languages->message, sizeof(languages->message), "%s", saying);
	languages->message_bad = 1;
	se_log("LANGUAGES system result request=%u errno=%d reason=%s", request, error, word);

	/* Succeeded: the answer was the page's. */
	return 1;
}

/*
 * Copies the login screen's language of the desktop's last answer ("en",
 * "ja", or not set).  The language to set starts as the system's (English
 * when it is not set) the first time it is known, and follows it when it
 * changes, but not while the administrator's choice is the only change.
 */
void
se_languages_copy(
	struct se_app *app)
{
	struct se_languages *languages;
	char text[8];
	const char *code;
	int previous;
	int error;
	int same;
	int i;

	/* The language's code; one never answered stays as it was. */
	languages = &app->languages;
	error = kl_system_machine_login_language(app->system, text, sizeof(text));
	if (error != 0)
		return;

	/* A language this page knows, else not set. */
	previous = languages->system;
	languages->system = -1;
	for (i = 0; i < LANGUAGES_COUNT; i++) {
		same = strcmp(text, languages_codes[i]);
		if (same == 0)
			languages->system = i;
	}

	/* The one to set: the system's, when first known or when the system's changed. */
	if (!languages->system_read || previous != languages->system) {
		languages->chosen = 0;
		if (languages->system >= 0)
			languages->chosen = languages->system;
	}
	languages->system_read = 1;

	/* The log line the tests read. */
	code = "none";
	if (languages->system >= 0)
		code = languages_codes[languages->system];
	se_log("LANGUAGES system language=%s", code);
}

/*
 * Reports a change of the login screen's language once the desktop read it
 * again (the reading's error; the change itself was made).
 */
void
se_languages_reloaded(
	struct se_app *app,
	int error)
{
	struct se_languages *languages;

	/* The change's line with the language read after it (-1 when it could not be read). */
	languages = &app->languages;
	se_log("LANGUAGES system result request=%u errno=0 system=%d reread=%d", languages->reload_for, languages->system, error);
	languages->reload_request = 0U;
	languages->reload_for = 0U;
	app->dirty = 1;
}

/* Tells whether the login screen's change can be asked: another language chosen, the password typed, no change under way. */
static int
languages_ready(
	const struct se_app *app)
{
	const struct se_languages *languages;
	int system;

	/* A language other than the system's (not set is English). */
	languages = &app->languages;
	system = languages->system;
	if (system < 0)
		system = 0;
	if (languages->chosen == system && languages->system >= 0)
		return 0;

	/* The password, and no answer awaited. */
	if (languages->password.length == 0 || languages->asked)
		return 0;

	/* Ready. */
	return 1;
}

/*
 * Asks the desktop to set the system's language (account-admin's
 * system-language, with the administrator's password), and wipes the field.
 */
static void
languages_apply(
	struct se_app *app)
{
	struct se_languages *languages;
	char operation[64];
	uint32_t request;
	int error;

	/* The operation's lines: its name and the language's code. */
	languages = &app->languages;
	(void)snprintf(operation, sizeof(operation), "system-language\n%s\n", languages_codes[languages->chosen]);

	/* Asked; the password leaves with the next flush, and no copy stays. */
	error = kl_system_account_administer(app->system, languages->password.text, operation, &request);
	se_field_clear(&languages->password);

	/* Not asked: said at once. */
	if (error != 0) {
		(void)snprintf(languages->message, sizeof(languages->message), "%s", kl_tr("The change could not be asked."));
		languages->message_bad = 1;
		se_log("LANGUAGES system errno=%d", error);
		return;
	}

	/* Succeeded: the answer comes as a result. */
	languages->asked = 1;
	languages->request = request;
	(void)snprintf(languages->message, sizeof(languages->message), "%s", kl_tr("Changing..."));
	languages->message_bad = 0;
	se_log("LANGUAGES system request=%u language=%s", request, languages_codes[languages->chosen]);
}

/* Gives what the page says for one of account-admin's refusal words. */
static const char *
languages_saying(
	const char *word)
{
	int same;

	/* Each word the page knows. */
	same = strcmp(word, languages_words[0]);
	if (same == 0)
		return kl_tr("Only an administrator can change the login screen's language.");
	same = strcmp(word, languages_words[1]);
	if (same == 0)
		return kl_tr("Your password is wrong.");
	same = strcmp(word, languages_words[2]);
	if (same == 0)
		return kl_tr("Another change is under way. Try again in a moment.");

	/* Any other. */
	return kl_tr("The change could not be made.");
}
