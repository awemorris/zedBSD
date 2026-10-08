/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws169-p005: the host test of the browser's sign-in codes of mail
 * (userland/desktop/browser/shell/mail.c) with fakes of libkeiland's
 * system, the titlebar and the view's keys: the browser listens as
 * "browser", a message with a code is offered as the titlebar's control
 * ("Code 482913") and a notification with an action, the notification's
 * number is kept, its click types the code into the page key by key (and
 * takes the control away), the titlebar's control does the same, a
 * message without a code is not offered, an offer that ran out is taken
 * back, and the code is never in the log.  ws177-p014: the notification
 * names the page, a code with letters is typed with "KeyX" (Shift for a
 * capital), a page without a field that takes text gets nothing but the
 * clipboard and a notification without the code, and the main loop is
 * told when the offer runs out.
 *
 * Prints "PASS name" or "FAIL name ..." for each check; exits with 1 when
 * one failed.
 */

#include "userland/desktop/browser/shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What the fakes saw and hold. */
static struct {
	char listened[32];
	struct kl_mail_event mails[4];
	unsigned mail_count;
	struct kl_notify_event notes[4];
	unsigned note_count;
	char title[256];
	char body[256];
	unsigned flags;
	uint32_t next_request;
	uint32_t withdrawn;
	char typed[32];
	unsigned releases;
	int focused;
	char label[32];
	unsigned offers;
	int text_target;
	char copied[32];
	unsigned shifted;
	int bad_key;
} fake;

/* The checks that failed. */
static int test_failures;

/* A system to hand out (its contents are never read). */
static int test_system_token;

int main(void);
static void test_check(const char *name, int passed, const char *detail);

/* libkeiland's fakes. */
struct kl_system *
kl_app_system(
	struct kl_app *app)
{
	UNUSED_PARAMETER(app);
	return (struct kl_system *)&test_system_token;
}

unsigned
kl_system_capabilities(
	const struct kl_system *system)
{
	UNUSED_PARAMETER(system);
	return KL_SYSTEM_HAS_NOTIFY | KL_SYSTEM_HAS_MAIL;
}

int
kl_system_mail_listen(
	struct kl_system *system,
	const char *app,
	uint32_t *request)
{
	UNUSED_PARAMETER(system);
	UNUSED_PARAMETER(request);
	(void)snprintf(fake.listened, sizeof(fake.listened), "%s", app);
	return 0;
}

int
kl_system_dispatch(
	struct kl_system *system,
	unsigned *changed)
{
	UNUSED_PARAMETER(system);
	*changed = 0;
	return 0;
}

int
kl_system_take_mail_event(
	struct kl_system *system,
	struct kl_mail_event *event)
{
	UNUSED_PARAMETER(system);
	if (fake.mail_count == 0U)
		return 0;
	*event = fake.mails[0];
	memmove(&fake.mails[0], &fake.mails[1], sizeof(fake.mails[0]) * 3U);
	fake.mail_count--;
	return 1;
}

int
kl_system_take_notify_event(
	struct kl_system *system,
	struct kl_notify_event *event)
{
	UNUSED_PARAMETER(system);
	if (fake.note_count == 0U)
		return 0;
	*event = fake.notes[0];
	memmove(&fake.notes[0], &fake.notes[1], sizeof(fake.notes[0]) * 3U);
	fake.note_count--;
	return 1;
}

int
kl_system_notify(
	struct kl_system *system,
	const struct kl_notification *notification,
	uint32_t *request)
{
	UNUSED_PARAMETER(system);
	(void)snprintf(fake.title, sizeof(fake.title), "%s", notification->title);
	(void)snprintf(fake.body, sizeof(fake.body), "%s", notification->body);
	fake.flags = notification->flags;
	fake.next_request++;
	*request = fake.next_request;
	return 0;
}

int
kl_system_notify_withdraw(
	struct kl_system *system,
	uint32_t id,
	uint32_t *request)
{
	UNUSED_PARAMETER(system);
	UNUSED_PARAMETER(request);
	fake.withdrawn = id;
	return 0;
}

/* The titlebar's fake (titlebar.c is not linked). */
int
shell_titlebar_offer_code(
	struct shell_titlebar *titlebar,
	const char *label)
{
	UNUSED_PARAMETER(titlebar);
	fake.offers++;
	(void)snprintf(fake.label, sizeof(fake.label), "%s", label != NULL ? label : "");
	return 0;
}

/* The window's clipboard. */
void
kl_window_copy(
	struct kl_window *window,
	const char *text,
	size_t length)
{
	UNUSED_PARAMETER(window);
	(void)snprintf(fake.copied, sizeof(fake.copied), "%.*s", (int)length, text);
}

/* The view's fakes. */
const char *
browser_view_title(
	const struct browser_view *view)
{
	UNUSED_PARAMETER(view);
	return "Example Bank - Sign in";
}

int
browser_view_text_target(
	struct browser_view *view,
	float caret[4])
{
	UNUSED_PARAMETER(view);
	caret[0] = 0.0f;
	caret[1] = 0.0f;
	caret[2] = 0.0f;
	caret[3] = 0.0f;
	return fake.text_target;
}

int
browser_view_focus(
	struct browser_view *view,
	int focused)
{
	UNUSED_PARAMETER(view);
	fake.focused = focused;
	return 0;
}

int
browser_view_key(
	struct browser_view *view,
	const char *key,
	const char *code,
	const char *text,
	int pressed,
	int repeat,
	uint32_t modifiers)
{
	size_t length;

	UNUSED_PARAMETER(view);
	UNUSED_PARAMETER(repeat);
	if (!pressed) {
		fake.releases++;
		return 0;
	}
	if (strcmp(key, text) != 0)
		return EINVAL;
	if (key[0] >= '0' && key[0] <= '9' && (strncmp(code, "Digit", 5U) != 0 || code[5] != key[0] || modifiers != 0U))
		fake.bad_key = 1;
	if (key[0] >= 'A' && key[0] <= 'Z' && (strncmp(code, "Key", 3U) != 0 || code[3] != key[0] || modifiers != BROWSER_MOD_SHIFT))
		fake.bad_key = 1;
	if (key[0] >= 'A' && key[0] <= 'Z')
		fake.shifted++;
	length = strlen(fake.typed);
	if (length + 1U < sizeof(fake.typed)) {
		fake.typed[length] = text[0];
		fake.typed[length + 1U] = '\0';
	}
	return 0;
}

/*
 * Drives mail.c and checks what it did.
 */
int
main(void)
{
	static char log_text[4096];
	struct shell_mail mail;
	struct shell_titlebar titlebar;
	struct browser_view *view;
	char label_first[32];
	char label_after_fill[32];
	char typed_by_notification[32];
	char body_first[256];
	char title_first[256];
	unsigned flags_first;
	char typed_before_copy[32];
	char typed_by_titlebar[32];
	unsigned releases_by_titlebar;
	int due_offered;
	int due_none;
	int due_over;
	FILE *log_file;
	size_t length;
	int saved;

	/* The log (standard output) kept in a file to look for the code in it. */
	fflush(stdout);
	saved = dup(1);
	log_file = tmpfile();
	if (log_file == NULL || saved < 0)
		return 2;
	(void)dup2(fileno(log_file), 1);
	view = (struct browser_view *)&test_system_token;
	memset(&titlebar, 0, sizeof(titlebar));
	titlebar.kui = (struct kl_window *)&test_system_token;
	fake.text_target = 1;

	/* Opened: listening as the browser. */
	shell_mail_open(&mail, NULL);

	/* A message without a code, then one with a code. */
	(void)snprintf(fake.mails[0].from, sizeof(fake.mails[0].from), "News");
	(void)snprintf(fake.mails[1].from, sizeof(fake.mails[1].from), "Example Bank");
	(void)snprintf(fake.mails[1].code, sizeof(fake.mails[1].code), "482913");
	fake.mail_count = 2;
	shell_mail_round(&mail, view, &titlebar, 1000U);
	(void)snprintf(label_first, sizeof(label_first), "%s", fake.label);
	(void)snprintf(body_first, sizeof(body_first), "%s", fake.body);
	(void)snprintf(title_first, sizeof(title_first), "%s", fake.title);
	flags_first = fake.flags;
	due_offered = shell_mail_timeout(&mail, 1000U + 30U * 1000U);
	due_over = shell_mail_timeout(&mail, 1000U + 3U * 60U * 1000U);

	/* The notification's number, then its click. */
	fake.notes[0].kind = KL_NOTIFY_POSTED;
	fake.notes[0].request = fake.next_request;
	fake.notes[0].id = 77;
	fake.notes[1].kind = KL_NOTIFY_ACTIVATED;
	fake.notes[1].id = 77;
	fake.note_count = 2;
	shell_mail_round(&mail, view, &titlebar, 2000U);
	(void)snprintf(label_after_fill, sizeof(label_after_fill), "%s", fake.label);
	(void)snprintf(typed_by_notification, sizeof(typed_by_notification), "%s", fake.typed);

	/* Another code, which runs out unclicked. */
	(void)snprintf(fake.mails[0].from, sizeof(fake.mails[0].from), "Shop");
	(void)snprintf(fake.mails[0].code, sizeof(fake.mails[0].code), "7351");
	fake.mail_count = 1;
	shell_mail_round(&mail, view, &titlebar, 3000U);
	fake.notes[0].kind = KL_NOTIFY_POSTED;
	fake.notes[0].request = fake.next_request;
	fake.notes[0].id = 78;
	fake.note_count = 1;
	shell_mail_round(&mail, view, &titlebar, 4000U);
	shell_mail_round(&mail, view, &titlebar, 4000U + 2U * 60U * 1000U);
	due_none = shell_mail_timeout(&mail, 5000U + 2U * 60U * 1000U);

	/* A third code, typed by the titlebar's control. */
	(void)snprintf(fake.mails[0].from, sizeof(fake.mails[0].from), "Club");
	(void)snprintf(fake.mails[0].code, sizeof(fake.mails[0].code), "123456");
	fake.mail_count = 1;
	shell_mail_round(&mail, view, &titlebar, 200000U);
	fake.typed[0] = '\0';
	shell_mail_fill(&mail, view, &titlebar);
	(void)snprintf(typed_by_titlebar, sizeof(typed_by_titlebar), "%s", fake.typed);
	releases_by_titlebar = fake.releases;

	/* A code with letters, typed by the titlebar's control. */
	(void)snprintf(fake.mails[0].from, sizeof(fake.mails[0].from), "Cloud");
	(void)snprintf(fake.mails[0].code, sizeof(fake.mails[0].code), "X7K2PQ");
	fake.mail_count = 1;
	shell_mail_round(&mail, view, &titlebar, 300000U);
	fake.typed[0] = '\0';
	shell_mail_fill(&mail, view, &titlebar);
	(void)snprintf(typed_before_copy, sizeof(typed_before_copy), "%s", fake.typed);

	/* A page without a field: the clipboard, nothing typed. */
	(void)snprintf(fake.mails[0].from, sizeof(fake.mails[0].from), "Club");
	(void)snprintf(fake.mails[0].code, sizeof(fake.mails[0].code), "246810");
	fake.mail_count = 1;
	shell_mail_round(&mail, view, &titlebar, 400000U);
	fake.text_target = 0;
	fake.typed[0] = '\0';
	shell_mail_fill(&mail, view, &titlebar);
	shell_mail_close(&mail, &titlebar);

	/* The log back on standard output, read for the checks. */
	fflush(stdout);
	(void)dup2(saved, 1);
	rewind(log_file);
	length = fread(log_text, 1U, sizeof(log_text) - 1U, log_file);
	log_text[length] = '\0';
	fclose(log_file);
	printf("%s", log_text);

	/* The checks. */
	test_check("listen", strcmp(fake.listened, "browser") == 0, fake.listened);
	test_check("offered-once-each", fake.next_request == 6U, "");
	test_check("offer-names-page", strstr(body_first, "on Example Bank - Sign in.") != NULL, body_first);
	test_check("timeout", due_offered == 90 * 1000 && due_over == 0 && due_none == -1, "");
	test_check("offer-titlebar", strcmp(label_first, "Code 482913") == 0, label_first);
	test_check("offer-action", flags_first == KL_NOTIFY_ACTION && strcmp(title_first, "Sign-in code from Example Bank") == 0, title_first);
	test_check("lettered", strcmp(typed_before_copy, "X7K2PQ") == 0 && fake.shifted == 4U && !fake.bad_key, typed_before_copy);
	test_check("no-field-copied", strcmp(fake.copied, "246810") == 0 && fake.typed[0] == '\0' && fake.label[0] == '\0', fake.copied);
	test_check("no-field-told", fake.flags == 0U && strcmp(fake.title, "Sign-in code copied") == 0 && strstr(fake.body, "246810") == NULL, fake.title);
	test_check("typed-by-notification", strcmp(typed_by_notification, "482913") == 0 && fake.focused == 1 && label_after_fill[0] == '\0', typed_by_notification);
	test_check("ran-out", fake.withdrawn == 78U, "");
	test_check("typed-by-titlebar", strcmp(typed_by_titlebar, "123456") == 0 && releases_by_titlebar == 12U, typed_by_titlebar);
	test_check("no-code-in-log", strstr(log_text, "482913") == NULL && strstr(log_text, "7351") == NULL && strstr(log_text, "123456") == NULL &&
	    strstr(log_text, "X7K2PQ") == NULL && strstr(log_text, "246810") == NULL, "");
	test_check("log-lengths", strstr(log_text, "ZBROWSER MAIL fill length=6 error=0") != NULL &&
	    strstr(log_text, "ZBROWSER MAIL copied length=6 clipboard=1 notified=1") != NULL, "");

	/* The outcome. */
	if (test_failures != 0) {
		printf("host-browser-mail: %d FAILED\n", test_failures);
		return 1;
	}
	printf("host-browser-mail: PASS\n");
	return 0;
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
