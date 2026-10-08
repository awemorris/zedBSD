/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws177-p015: the host test of Mail's view (userland/desktop/mailer/
 * view.c with store.c and libkeiland's widgets): a folder of 600 messages
 * is listed whole (Up and Down reach its newest and its oldest), Edit
 * Account fills the form with the account, Remove Account asks first and
 * a confirmed removal is a request with the account's index, a
 * certificate's question gives the answer as a request (Esc: not trusted,
 * Enter: trusted), and sixteen accounts' folders are taller than the
 * sidebar (they scroll).
 *
 *     host-mail-view FONT FALLBACK
 *
 * Prints "PASS name" or "FAIL name [detail]" for each check; exits with 1
 * when one failed.
 */

#include "userland/desktop/mailer/mailer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The window's size. */
#define TEST_WIDTH		1180
#define TEST_HEIGHT		740

/* How many messages the folder has (more than the 512 the list showed before). */
#define TEST_MESSAGES		600

/* The checks that failed. */
static int test_failures;

/* The frame's time, moved on between frames. */
static uint64_t test_now = 1000000U;

int main(int argc, char **argv);
unsigned kl_appearance_get(const struct kl_appearance *appearance);
static void test_check(const char *name, int passed, const char *detail);
static void test_frame(struct ml_view *view, struct kl_ui *ui, const struct kl_style *style);
static void test_answer(struct ml_view *view, struct kl_ui *dialog, const struct kl_style *style, uint32_t key);
static int test_request(struct ml_view *view, unsigned action, long *message);
static void test_account(struct ml_account_config *account, const char *name);

/*
 * Draws and drives the view and checks what it did.
 */
int
main(
	int argc,
	char **argv)
{
	struct ml_account_config account;
	struct ml_message message;
	struct kl_canvas canvas;
	struct kl_style style;
	struct kl_text text;
	struct ml_view view;
	struct kl_ui *ui;
	struct kl_ui *dialog;
	uint32_t *pixels;
	char name[32];
	long answer;
	int found;
	int error;
	int i;

	/* The fonts. */
	if (argc != 3) {
		fprintf(stderr, "usage: host-mail-view FONT FALLBACK\n");
		return 2;
	}
	kl_text_companions("userland/desktop/fonts/Mahora-Bold.ttf", "userland/desktop/fonts/JetBrainsMono-Regular.ttf");
	error = kl_text_open(&text, argv[1], argv[2]);
	if (error != 0)
		return 2;

	/* The frame, its canvas, the inputs and the style. */
	pixels = calloc((size_t)TEST_WIDTH * TEST_HEIGHT, sizeof(pixels[0]));
	if (pixels == NULL)
		return 2;
	error = kl_canvas_init(&canvas, pixels, TEST_WIDTH, TEST_WIDTH, TEST_HEIGHT);
	if (error != 0)
		return 2;
	ui = kl_ui_create();
	dialog = kl_ui_create();
	if (ui == NULL || dialog == NULL)
		return 2;
	style.canvas = &canvas;
	style.text = &text;
	style.theme = kl_theme_default();
	style.glass = 0;

	/* Two accounts, the first's inbox with 600 messages, the oldest first. */
	test_account(&account, "Personal");
	(void)ml_store_add_account(&account);
	test_account(&account, "Work");
	(void)ml_store_add_account(&account);
	memset(&message, 0, sizeof(message));
	message.folder = ML_INBOX;
	message.from_name = "Ann";
	message.from_address = "ann@example.org";
	message.subject = "Hello";
	message.date_short = "09:41";
	message.date_long = "Monday";
	message.body = "Words.";
	for (i = 0; i < TEST_MESSAGES; i++) {
		message.uid = (uint32_t)i + 1U;
		message.date = (time_t)(1000000 + i * 60);
		(void)ml_store_insert(&message);
	}

	/* The view on the first account's inbox. */
	error = ml_view_init(&view);
	if (error != 0)
		return 2;
	test_frame(&view, ui, &style);

	/* Up to the newest, then Down to the oldest: the whole folder is listed. */
	for (i = 0; i < TEST_MESSAGES + 10; i++)
		ml_view_key(&view, KL_KEY_UP, 0U, test_now);
	test_check("list-newest", view.selected == TEST_MESSAGES - 1, "");
	for (i = 0; i < TEST_MESSAGES + 10; i++)
		ml_view_key(&view, KL_KEY_DOWN, 0U, test_now);
	test_check("list-oldest", view.selected == 0, "");
	test_frame(&view, ui, &style);

	/* Edit Account: the form with the first account. */
	while (view.request_count != 0U)
		(void)test_request(&view, 0U, NULL);
	ml_view_action(&view, ML_ACTION_EDIT_ACCOUNT, test_now);
	test_frame(&view, ui, &style);
	test_check("edit-form", view.editing == 0 && view.adding && strcmp(view.setup_name.text, "Personal") == 0 &&
	    strcmp(view.setup_imap.text, "imap.personal.example:993") == 0 && strcmp(view.setup_password.text, "secret 1") == 0, view.setup_imap.text);

	/* Remove Account asks; Esc keeps the account, Enter removes it. */
	ml_view_action(&view, ML_ACTION_ASK_REMOVE, test_now);
	test_check("remove-asks", view.question == ML_QUESTION_REMOVE, "");
	test_answer(&view, dialog, &style, KL_KEY_ESC);
	found = test_request(&view, ML_ACTION_REMOVE_ACCOUNT, &answer);
	test_check("remove-cancel", view.question == ML_QUESTION_NONE && !found, "");
	ml_view_action(&view, ML_ACTION_ASK_REMOVE, test_now);
	test_answer(&view, dialog, &style, KL_KEY_ENTER);
	found = test_request(&view, ML_ACTION_REMOVE_ACCOUNT, &answer);
	test_check("remove-confirmed", found && answer == 0, "");

	/* Cancel leaves the form, its fields emptied; Add Account's form is empty. */
	ml_view_action(&view, ML_ACTION_CANCEL, test_now);
	ml_view_action(&view, ML_ACTION_ADD_ACCOUNT, test_now);
	test_check("add-empty", view.editing == -1 && view.setup_name.text[0] == '\0' && view.setup_password.text[0] == '\0', view.setup_name.text);
	ml_view_action(&view, ML_ACTION_CANCEL, test_now);

	/* A certificate: Esc is not trusted, Enter is. */
	ml_view_ask(&view, ML_QUESTION_TRUST, "Trust the certificate of mail.example?", "Mail cannot verify it.");
	test_answer(&view, dialog, &style, KL_KEY_ESC);
	found = test_request(&view, ML_ACTION_TRUST, &answer);
	test_check("trust-no", found && answer == 0, "");
	ml_view_ask(&view, ML_QUESTION_TRUST, "Trust the certificate of mail.example?", "Mail cannot verify it.");
	test_answer(&view, dialog, &style, KL_KEY_ENTER);
	found = test_request(&view, ML_ACTION_TRUST, &answer);
	test_check("trust-yes", found && answer == 1, "");

	/* Sixteen accounts: their folders are taller than the sidebar's room, which scrolls. */
	for (i = 2; i < (int)ML_ACCOUNTS_MAX; i++) {
		(void)snprintf(name, sizeof(name), "Account %d", i);
		test_account(&account, name);
		(void)ml_store_add_account(&account);
	}
	test_frame(&view, ui, &style);
	test_frame(&view, ui, &style);
	test_check("sidebar-scrolls", view.sidebar_height > TEST_HEIGHT * 2, "");

	/* The outcome. */
	ml_view_release(&view);
	ml_store_release();
	kl_ui_destroy(dialog);
	kl_ui_destroy(ui);
	kl_canvas_release(&canvas);
	free(pixels);
	kl_text_close(&text);
	if (test_failures != 0) {
		printf("host-mail-view: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-mail-view: PASS\n");
	return 0;
}

/* The view's log, not checked here. */
void
ml_log(
	const char *format,
	...)
{
	UNUSED_PARAMETER(format);
}

/* The light appearance (libkeiland's theme asks for it). */
unsigned
kl_appearance_get(
	const struct kl_appearance *appearance)
{
	UNUSED_PARAMETER(appearance);
	return KL_APPEARANCE_LIGHT;
}

/* Prints a check's outcome. */
static void
test_check(
	const char *name,
	int passed,
	const char *detail)
{
	/* Passed. */
	if (passed) {
		printf("PASS %s\n", name);
		return;
	}

	/* Failed, with what was seen. */
	printf("FAIL %s [%s]\n", name, detail);
	test_failures++;
}

/* Draws one frame of the view as the window does. */
static void
test_frame(
	struct ml_view *view,
	struct kl_ui *ui,
	const struct kl_style *style)
{
	struct kl_event event;
	int taken;

	/* The frame, a frame's time after the last. */
	test_now += 16000U;
	kl_ui_begin(ui, test_now);
	ml_view_draw(view, ui, style, TEST_WIDTH, TEST_HEIGHT, test_now);
	(void)kl_ui_end(ui, test_now);

	/* What no widget took. */
	for (;;) {
		taken = kl_ui_take(ui, &event);
		if (!taken)
			break;
	}
}

/* Answers the question asked with a key, on the question's own input, a frame before and after it. */
static void
test_answer(
	struct ml_view *view,
	struct kl_ui *dialog,
	const struct kl_style *style,
	uint32_t key)
{
	struct kl_event event;
	int frame;
	int taken;

	/* Three frames of the question: shown, the key pressed, the key let go. */
	for (frame = 0; frame < 3; frame++) {
		if (frame == 1)
			(void)kl_ui_key(dialog, key, 1, 0U);
		if (frame == 2)
			(void)kl_ui_key(dialog, key, 0, 0U);
		test_now += 16000U;
		kl_ui_begin(dialog, test_now);
		ml_view_question(view, dialog, style, TEST_WIDTH, TEST_HEIGHT, test_now);
		(void)kl_ui_end(dialog, test_now);
		for (;;) {
			taken = kl_ui_take(dialog, &event);
			if (!taken)
				break;
		}
	}
}

/* Takes the requests the view queued and tells whether one was of an action (its message given). */
static int
test_request(
	struct ml_view *view,
	unsigned action,
	long *message)
{
	struct ml_request request;
	int found;
	int taken;

	/* Each request. */
	found = 0;
	for (;;) {
		taken = ml_view_take_request(view, &request);
		if (!taken)
			break;
		if (request.action == action && message != NULL) {
			*message = request.message;
			found = 1;
		}
	}

	/* Whether it was there. */
	return found;
}

/* Makes an account of a name, its servers named after it. */
static void
test_account(
	struct ml_account_config *account,
	const char *name)
{
	char server[ML_TEXT_MAX];
	char lower[32];
	size_t index;

	/* The name in small letters for the servers. */
	for (index = 0; name[index] != '\0' && index + 1U < sizeof(lower); index++) {
		lower[index] = name[index];
		if (name[index] >= 'A' && name[index] <= 'Z')
			lower[index] = (char)(name[index] - 'A' + 'a');
		if (name[index] == ' ')
			lower[index] = '-';
	}
	lower[index] = '\0';

	/* The account. */
	memset(account, 0, sizeof(*account));
	(void)snprintf(account->name, sizeof(account->name), "%s", name);
	(void)snprintf(account->address, sizeof(account->address), "kei@%s.example", lower);
	(void)snprintf(account->user, sizeof(account->user), "kei@%s.example", lower);
	(void)snprintf(account->password, sizeof(account->password), "secret 1");
	(void)snprintf(server, sizeof(server), "imap.%s.example:993", lower);
	(void)ml_server_parse(server, 993U, &account->imap);
	(void)snprintf(server, sizeof(server), "smtp.%s.example:465", lower);
	(void)ml_server_parse(server, 465U, &account->smtp);
}
