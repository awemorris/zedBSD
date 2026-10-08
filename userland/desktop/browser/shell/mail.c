/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sign-in codes of mail (WS169 p005, plan/ws169/phase001/phase.md
 * section 4): the browser listens to the compositor's arrivals of mail
 * (kl_system_mail_listen under the name "browser"; the compositor tells
 * them only while the user allows it, the desktop's mail.codes.browser).
 * A message with a sign-in code is offered as a control at the front of
 * the titlebar ("Code 482913") and as a notification ("Sign-in code from
 * ..."); a click of either types the code into the page's focused field,
 * as the keyboard would.  An offer lasts two minutes, and a newer code
 * replaces it.
 *
 * ws177-p014: a page whose focus is in no field that takes text (the
 * click went outside the field, or the page has none) is not typed into,
 * so that its own keys (its shortcuts) do not fire.  ws177-p018: the
 * code then goes into the page's field for one-time codes
 * (autocomplete="one-time-code", browser_view_focus_field), and only a
 * page without one gets nothing: the code goes to the clipboard instead,
 * and a notification says to paste it.  A code with
 * letters is typed with the letters' DOM names ("KeyK", Shift for a
 * capital).  Each browser window hears the arrival and offers the code,
 * and the notification names the window's page, so the user chooses the
 * window by clicking its control or its notification.  The main loop
 * wakes when an offer runs out (shell_mail_timeout), so that the offer is
 * taken back on time.
 *
 * The page's drawing and engine are not touched: the code goes in through
 * browser_view_key.  The code is never written to the log, only its
 * length.
 */

#include "shell/internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How long a code is offered, in ms. */
#define SHELL_MAIL_OFFER_MS	(2U * 60U * 1000U)

/* The name the browser listens under (the setting is mail.codes.browser). */
#define SHELL_MAIL_READER	"browser"

/* The most bytes of the page's title the notification names. */
#define SHELL_MAIL_TITLE_MAX	120

static void shell_mail_offer(struct shell_mail *mail, const struct kl_mail_event *arrival, struct browser_view *view, struct shell_titlebar *titlebar, uint64_t now_ms);
static void shell_mail_withdraw(struct shell_mail *mail, struct shell_titlebar *titlebar);
static int shell_mail_type(const char *code, struct browser_view *view);
static void shell_mail_copy(struct shell_mail *mail, struct shell_titlebar *titlebar);

/*
 * Starts listening to the arrivals of mail through the application's
 * system.  A compositor without them leaves the browser without codes.
 */
void
shell_mail_open(
	struct shell_mail *mail,
	struct kl_app *app)
{
	unsigned capabilities;
	int error;

	/* Nothing yet. */
	memset(mail, 0, sizeof(*mail));

	/* The application's system. */
	mail->system = kl_app_system(app);
	if (mail->system == NULL) {
		printf("ZBROWSER MAIL none error=%d\n", errno);
		fflush(stdout);
		return;
	}

	/* A compositor without the arrivals of mail. */
	capabilities = kl_system_capabilities(mail->system);
	if ((capabilities & KL_SYSTEM_HAS_MAIL) == 0U) {
		printf("ZBROWSER MAIL none error=%d\n", ENOTSUP);
		fflush(stdout);
		mail->system = NULL;
		return;
	}

	/* Listening (the compositor answers INVALID only for a name it does not know). */
	error = kl_system_mail_listen(mail->system, SHELL_MAIL_READER, NULL);
	printf("ZBROWSER MAIL listen error=%d\n", error);
	fflush(stdout);
	if (error != 0)
		mail->system = NULL;
}

/*
 * Takes what the compositor told since the last round: a code arrived
 * (offered), the notification of the offer clicked (the code typed into
 * the page), and an offer that ran out (taken back).  Returns 1 when the
 * titlebar's controls were declared again (the caller shows the browser's
 * state on them), else 0.
 */
int
shell_mail_round(
	struct shell_mail *mail,
	struct browser_view *view,
	struct shell_titlebar *titlebar,
	uint64_t now_ms)
{
	struct kl_mail_event arrival;
	struct kl_notify_event event;
	unsigned changed;
	int offered;
	int taken;
	int status;

	/* No arrivals to hear. */
	if (mail->system == NULL)
		return 0;

	/* The compositor's events read by the window's dispatch. */
	status = kl_system_dispatch(mail->system, &changed);
	if (status != 0) {
		mail->system = NULL;
		return 0;
	}

	/* Whether an offer was made or taken away this round. */
	offered = 0;

	/* Each message that arrived with a code. */
	for (;;) {
		taken = kl_system_take_mail_event(mail->system, &arrival);
		if (!taken)
			break;

		/* A message without a code is not the browser's. */
		if (arrival.code[0] == '\0')
			continue;
		shell_mail_offer(mail, &arrival, view, titlebar, now_ms);
		offered = 1;
	}

	/* The notifications' events: the offer's number, its click, its close. */
	for (;;) {
		taken = kl_system_take_notify_event(mail->system, &event);
		if (!taken)
			break;

		/* The number of the offer posted. */
		if (event.kind == KL_NOTIFY_POSTED && event.request == mail->request) {
			mail->notification = event.id;
			continue;
		}

		/* The offer clicked while it lasts: the code typed. */
		if (event.kind == KL_NOTIFY_ACTIVATED && mail->notification != 0U && event.id == mail->notification) {
			shell_mail_fill(mail, view, titlebar);
			offered = 1;
			continue;
		}

		/* The notification closed (dismissed): the titlebar's control stays until it is used or runs out. */
		if (event.kind == KL_NOTIFY_CLOSED && event.id == mail->notification)
			mail->notification = 0;
	}

	/* An offer that ran out is taken back. */
	if (mail->code[0] != '\0' && now_ms >= mail->until_ms) {
		shell_mail_withdraw(mail, titlebar);
		offered = 1;
	}

	/* Whether the titlebar was declared again. */
	return offered;
}

/*
 * Tells in how many milliseconds the offer runs out, for the main loop's
 * wait: -1 without an offer, 0 when it has run out already.
 */
int
shell_mail_timeout(
	const struct shell_mail *mail,
	uint64_t now_ms)
{
	uint64_t left;

	/* No offer waits for nothing. */
	if (mail->system == NULL)
		return -1;
	if (mail->code[0] == '\0')
		return -1;

	/* An offer that ran out is due now. */
	if (now_ms >= mail->until_ms)
		return 0;

	/* The time left, which an offer of two minutes keeps within an int. */
	left = mail->until_ms - now_ms;
	if (left > SHELL_MAIL_OFFER_MS)
		left = SHELL_MAIL_OFFER_MS;

	/* Succeeded: the milliseconds until the offer is taken back. */
	return (int)left;
}

/*
 * Stops listening: an offer still shown is taken back.
 */
void
shell_mail_close(
	struct shell_mail *mail,
	struct shell_titlebar *titlebar)
{
	/* The offer. */
	if (mail->system != NULL && mail->code[0] != '\0')
		shell_mail_withdraw(mail, titlebar);

	/* The code forgotten (the system is the application's and goes with it). */
	memset(mail, 0, sizeof(*mail));
}

/*
 * Types the offered code into the page's focused field, key by key, and
 * forgets it (the titlebar's control and the notification go).  A page
 * whose focus is in no field that takes text gets nothing: the code goes
 * to the clipboard instead (ws177-p014).
 */
void
shell_mail_fill(
	struct shell_mail *mail,
	struct browser_view *view,
	struct shell_titlebar *titlebar)
{
	float caret[4];
	size_t length;
	int target;
	int error;

	/* Nothing offered. */
	length = strlen(mail->code);
	if (length == 0U)
		return;

	/* The page has the keyboard again (the click went to the notification or the titlebar). */
	(void)browser_view_focus(view, 1);

	/* No field with the focus: the page's field for one-time codes, else the clipboard (keys would be the page's shortcuts). */
	target = browser_view_text_target(view, caret);
	if (target != 1) {
		error = browser_view_focus_field(view, "one-time-code");
		printf("ZBROWSER MAIL one-time-code-field error=%d\n", error);
		fflush(stdout);
		if (error != 0) {
			shell_mail_copy(mail, titlebar);
			return;
		}
	}

	/* The code typed into the field. */
	error = shell_mail_type(mail->code, view);

	/* The log the tests read (its length only), and the code forgotten. */
	printf("ZBROWSER MAIL fill length=%lu error=%d\n", (unsigned long)length, error);
	fflush(stdout);
	shell_mail_withdraw(mail, titlebar);
}

/* Offers a code as the titlebar's control and as a notification naming the window's page, in place of an earlier one. */
static void
shell_mail_offer(
	struct shell_mail *mail,
	const struct kl_mail_event *arrival,
	struct browser_view *view,
	struct shell_titlebar *titlebar,
	uint64_t now_ms)
{
	struct kl_notification notification;
	char title[KL_MAIL_TEXT_MAX + 32U];
	char body[KL_MAIL_CODE_MAX + SHELL_MAIL_TITLE_MAX + 64U];
	const char *page;
	int shown;
	int error;

	/* The code kept for the click, for two minutes. */
	(void)snprintf(mail->code, sizeof(mail->code), "%s", arrival->code);
	mail->until_ms = now_ms + SHELL_MAIL_OFFER_MS;

	/* The titlebar's control, labelled with the code. */
	(void)snprintf(mail->label, sizeof(mail->label), "Code %s", arrival->code);
	shown = shell_titlebar_offer_code(titlebar, mail->label);

	/* The page's title, which tells this window's notification from another window's. */
	page = browser_view_title(view);
	if (page == NULL)
		page = "";

	/* The notification's words. */
	(void)snprintf(title, sizeof(title), "Sign-in code from %s", arrival->from);
	if (page[0] != '\0') {
		(void)snprintf(body, sizeof(body), "Click to fill in %s on %.*s.", arrival->code, SHELL_MAIL_TITLE_MAX, page);
	} else {
		(void)snprintf(body, sizeof(body), "Click to fill in %s in Browser.", arrival->code);
	}

	/* Posted with an action, in place of the earlier offer. */
	memset(&notification, 0, sizeof(notification));
	notification.title = title;
	notification.body = body;
	notification.replaces = mail->notification;
	notification.flags = KL_NOTIFY_ACTION;
	error = kl_system_notify(mail->system, &notification, &mail->request);
	memset(body, 0, sizeof(body));
	printf("ZBROWSER MAIL code length=%lu titlebar=%d notified=%d\n", (unsigned long)strlen(mail->code), shown == 0, error == 0);
	fflush(stdout);
}

/* Takes back the titlebar's control and the notification of the offer, and forgets the code. */
static void
shell_mail_withdraw(
	struct shell_mail *mail,
	struct shell_titlebar *titlebar)
{
	/* The titlebar's control. */
	(void)shell_titlebar_offer_code(titlebar, NULL);

	/* The notification, when its number came. */
	if (mail->notification != 0U)
		(void)kl_system_notify_withdraw(mail->system, mail->notification, NULL);

	/* The code. */
	memset(mail->code, 0, sizeof(mail->code));
	memset(mail->label, 0, sizeof(mail->label));
	mail->notification = 0;
}

/*
 * Presses and lets go of each character of a code, with the DOM's names
 * of its key: a digit's "DigitN", a letter's "KeyX" (with Shift for a
 * capital).  Returns 0, or why the page refused a key.
 */
static int
shell_mail_type(
	const char *code,
	struct browser_view *view)
{
	char key[2];
	char name[8];
	uint32_t modifiers;
	size_t index;
	int error;

	/* Each character, while the page takes them. */
	for (index = 0; code[index] != '\0'; index++) {
		key[0] = code[index];
		key[1] = '\0';
		name[0] = '\0';
		modifiers = 0U;

		/* The key's DOM name: a digit's, a small letter's, or a capital's with Shift held. */
		if (key[0] >= '0' && key[0] <= '9') {
			(void)snprintf(name, sizeof(name), "Digit%c", key[0]);
		} else if (key[0] >= 'a' && key[0] <= 'z') {
			(void)snprintf(name, sizeof(name), "Key%c", key[0] - 'a' + 'A');
		} else if (key[0] >= 'A' && key[0] <= 'Z') {
			(void)snprintf(name, sizeof(name), "Key%c", key[0]);
			modifiers = BROWSER_MOD_SHIFT;
		}

		/* Pressed. */
		error = browser_view_key(view, key, name, key, 1, 0, modifiers);
		if (error != 0)
			return error;

		/* Let go. */
		error = browser_view_key(view, key, name, key, 0, 0, modifiers);
		if (error != 0)
			return error;
	}

	/* Succeeded: the whole code was typed. */
	return 0;
}

/*
 * Puts the offered code on the clipboard for a page without a field to
 * take it, takes the offer back, and says so in a notification without
 * the code.
 */
static void
shell_mail_copy(
	struct shell_mail *mail,
	struct shell_titlebar *titlebar)
{
	struct kl_notification notification;
	uint32_t request;
	size_t length;
	int error;

	/* The code on the clipboard, as a copy from the window would put it. */
	length = strlen(mail->code);
	if (titlebar->kui != NULL)
		kl_window_copy(titlebar->kui, mail->code, length);

	/* The offer taken back and the code forgotten. */
	shell_mail_withdraw(mail, titlebar);

	/* The user told where the code went. */
	memset(&notification, 0, sizeof(notification));
	notification.title = "Sign-in code copied";
	notification.body = "No field on the page takes it: click the field and paste with Ctrl+V.";
	error = kl_system_notify(mail->system, &notification, &request);

	/* The log the tests read (its length only). */
	printf("ZBROWSER MAIL copied length=%lu clipboard=%d notified=%d\n", (unsigned long)length, titlebar->kui != NULL, error == 0);
	fflush(stdout);
}
