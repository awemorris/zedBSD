/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Users page's popups (ws200-p001):
 * page-users-password.c is included here so that its static steps are
 * reached, and linked with the rest of Settings' host build
 * (plan/ws089/tests/host-build.sh) and its stand-in desktop
 * (host-kl-system.c: HOST_ACCOUNT_RESULT answers a change, HOST_METHODS
 * gives the methods' bits, HOST_ENROLLED "PIN KEYS" what is set up,
 * HOST_REFUSAL a refusal's word).
 *
 * Checked: Change Password from the current password to the new one twice
 * and Done (the current one kept and wiped), the new one that differs, is
 * short or is the current one said in the popup, a wrong current password
 * and a refused new one going back to the first step, the idle time wiping
 * the kept password; the methods' switches (one not set up, the last of
 * the password and a key kept on, the PIN always), the password turned
 * off warned of first, a method turned on or off asking the password and
 * sending the methods with the one flipped, and a wrong password said.
 */

#include "page-users-password.c"

#include <stdlib.h>

/* The popup's idle time (dialog.c's DIALOG_IDLE_MS). */
#define TEST_IDLE_MS		120000U

/* The checks that failed. */
static unsigned test_failures;

static void test_check(int condition, const char *what);
static void test_enter(struct se_app *app);
static void test_change(struct se_app *app);
static void test_change_refused(struct se_app *app);
static void test_switches(struct se_app *app);
static void test_methods(struct se_app *app);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(void)
{
	static struct se_app app;

	/* The stand-in desktop: the account, its changes answered with success. */
	(void)setenv("HOST_ACCOUNT_RESULT", "0", 1);
	memset(&app, 0, sizeof(app));
	app.system = (struct kl_system *)&app;
	app.now = 1000U;

	/* Each part. */
	test_change(&app);
	test_change_refused(&app);
	test_switches(&app);
	test_methods(&app);

	/* The verdict. */
	if (test_failures != 0U) {
		printf("settings-password-host-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Every check held. */
	printf("settings-password-host-test: PASS\n");
	return 0;
}

/* Counts and reports a check that does not hold. */
static void
test_check(
	int condition,
	const char *what)
{
	/* A check that holds says nothing. */
	if (condition)
		return;
	test_failures++;
	printf("FAIL: %s\n", what);
}

/* Presses Enter in the popup. */
static void
test_enter(
	struct se_app *app)
{
	struct se_event event;

	/* The key, as the window gives it. */
	memset(&event, 0, sizeof(event));
	event.pressed = 1;
	event.key = SE_KEY_ENTER;
	(void)se_dialog_key(app, &event);
}

/* Change Password from the first step to Done. */
static void
test_change(
	struct se_app *app)
{
	/* The button opens the current password's step, not ready while empty. */
	(void)se_password_press(app, PASSWORD_CHANGE);
	test_check(app->dialog.open && app->password.flow == SE_PASSWORD_FLOW_CHANGE, "Change Password opens the popup");
	test_check(app->password.step == PASSWORD_STEP_CURRENT && app->dialog.step == 1U && app->dialog.steps == 2U, "step 1 of 2: the current password");
	test_check(!password_ready(app), "the current password's step waits for it");

	/* The current password: kept, then the new one's step. */
	se_dialog_set_text(app, 0U, "old-pass-1");
	test_enter(app);
	test_check(app->password.step == PASSWORD_STEP_NEW && app->dialog.step == 2U, "step 2 of 2: the new password");
	test_check(strcmp(app->password.current.text, "old-pass-1") == 0, "the current password is kept");

	/* Two different, a short one, the current one: said in the popup, the step kept. */
	se_dialog_set_text(app, 0U, "new-pass-1");
	se_dialog_set_text(app, 1U, "new-pass-2");
	test_enter(app);
	test_check(app->password.step == PASSWORD_STEP_NEW && strstr(app->dialog.error, "differ") != NULL, "a repeat that differs is said");
	se_dialog_set_text(app, 0U, "short");
	se_dialog_set_text(app, 1U, "short");
	test_enter(app);
	test_check(!app->password.asked && strstr(app->dialog.error, "8 characters") != NULL, "a short password is said");
	se_dialog_set_text(app, 0U, "old-pass-1");
	se_dialog_set_text(app, 1U, "old-pass-1");
	test_enter(app);
	test_check(!app->password.asked && strstr(app->dialog.error, "other than") != NULL, "the current password again is said");

	/* A good one: asked, busy, the kept password wiped. */
	se_dialog_set_text(app, 0U, "new-pass-1");
	se_dialog_set_text(app, 1U, "new-pass-1");
	test_enter(app);
	test_check(app->password.asked && app->dialog.busy && !app->dialog.cancellable, "the change is asked, busy, not cancellable");
	test_check(app->password.current.length == 0U, "the kept password is wiped once asked");

	/* Busy: no second change, and Enter does nothing. */
	test_check(se_password_press(app, PASSWORD_CHANGE) == 1 && app->password.asked, "no second change while one is asked");

	/* The answer: Done, then closed. */
	(void)se_password_result(app, app->password.request, 0);
	test_check(app->password.step == PASSWORD_STEP_DONE && !app->password.asked, "the answer ends the popup");
	test_check(strstr(app->dialog.body, "changed") != NULL, "the end says the password changed");
	test_enter(app);
	test_check(!app->dialog.open, "Done closes the popup");
}

/* A wrong current password, a refused new one, and the idle time. */
static void
test_change_refused(
	struct se_app *app)
{
	/* A wrong current password: back to the first step, said. */
	(void)se_password_press(app, PASSWORD_CHANGE);
	se_dialog_set_text(app, 0U, "wrong-pass");
	test_enter(app);
	se_dialog_set_text(app, 0U, "new-pass-1");
	se_dialog_set_text(app, 1U, "new-pass-1");
	test_enter(app);
	(void)se_password_result(app, app->password.request, EPERM);
	test_check(app->password.step == PASSWORD_STEP_CURRENT && strstr(app->dialog.error, "current password is wrong") != NULL, "a wrong current password goes back, said");

	/* A refused new one: back to the first step too (the current one was wiped), said. */
	se_dialog_set_text(app, 0U, "old-pass-1");
	test_enter(app);
	se_dialog_set_text(app, 0U, "new-pass-1");
	se_dialog_set_text(app, 1U, "new-pass-1");
	test_enter(app);
	(void)se_password_result(app, app->password.request, EINVAL);
	test_check(app->password.step == PASSWORD_STEP_CURRENT && strstr(app->dialog.error, "not accepted") != NULL, "a refused new password is said");

	/* Left alone two minutes at the new one's step: the kept password wiped, asked again. */
	se_dialog_set_text(app, 0U, "old-pass-1");
	test_enter(app);
	app->dialog.input_ms = app->now;
	app->now += TEST_IDLE_MS;
	se_dialog_tick(app, app->now);
	test_check(app->password.step == PASSWORD_STEP_CURRENT && app->password.current.length == 0U, "two minutes alone wipe the kept password");

	/* Cancel closes. */
	se_password_end(app);
	test_check(!app->dialog.open && app->password.flow == SE_PASSWORD_FLOW_NONE, "Cancel closes the popup");
}

/* Which switches may be flipped. */
static void
test_switches(
	struct se_app *app)
{
	unsigned on;

	/* Without the methods: no card. */
	test_check(!password_methods_available(app), "no methods without KL_SYSTEM_HAS_METHODS");

	/* Every method, a PIN and a key set up: each switch on and free. */
	(void)setenv("HOST_METHODS", "7", 1);
	(void)setenv("HOST_ENROLLED", "1 1", 1);
	test_check(password_methods_available(app), "the methods with KL_SYSTEM_HAS_METHODS");
	on = password_methods_on(app);
	test_check(on == 7U, "every method on");
	test_check(password_switch_enabled(app, KL_SYSTEM_METHOD_PASSWORD, on), "the password may go off while a key is on");
	test_check(password_switch_enabled(app, KL_SYSTEM_METHOD_KEY, on), "a key may go off while the password is on");
	test_check(password_switch_enabled(app, KL_SYSTEM_METHOD_PIN, on), "the PIN may go off");

	/* A key alone on: it must stay on; the password may come on again. */
	(void)setenv("HOST_METHODS", "4", 1);
	on = password_methods_on(app);
	test_check(on == 4U, "the key alone on");
	test_check(!password_switch_enabled(app, KL_SYSTEM_METHOD_KEY, on), "the last first sign-in stays on");
	test_check(password_switch_enabled(app, KL_SYSTEM_METHOD_PASSWORD, on), "the password may come on");

	/* Nothing set up but the password: the others off and fixed, the password fixed on. */
	(void)setenv("HOST_METHODS", "7", 1);
	(void)setenv("HOST_ENROLLED", "0 0", 1);
	on = password_methods_on(app);
	test_check(on == KL_SYSTEM_METHOD_PASSWORD, "methods not set up are shown off");
	test_check(!password_switch_enabled(app, KL_SYSTEM_METHOD_PIN, on), "a PIN not set up cannot come on");
	test_check(!password_switch_enabled(app, KL_SYSTEM_METHOD_KEY, on), "a key not set up cannot come on");
	test_check(!password_switch_enabled(app, KL_SYSTEM_METHOD_PASSWORD, on), "the password alone stays on");
}

/* A method turned off and on through the popup. */
static void
test_methods(
	struct se_app *app)
{
	(void)setenv("HOST_METHODS", "7", 1);
	(void)setenv("HOST_ENROLLED", "1 1", 1);

	/* The password off: warned first (step 1 of 2), then the password (step 2 of 2), Back to the warning. */
	(void)se_password_press(app, PASSWORD_METHOD_FIRST);
	test_check(app->password.flow == SE_PASSWORD_FLOW_METHODS && app->password.methods == 6U, "the password off asks for the PIN and the key");
	test_check(app->password.step == PASSWORD_STEP_WARNING && app->dialog.steps == 2U, "the password off is warned of first");
	test_check(strstr(app->dialog.body, "console") != NULL, "the warning names the console");
	test_enter(app);
	test_check(app->password.step == PASSWORD_STEP_CONFIRM && app->dialog.step == 2U, "then the password");
	test_check(!password_ready(app), "the password's step waits for it");
	password_act(app, SE_DIALOG_BACK);
	test_check(app->password.step == PASSWORD_STEP_WARNING, "Back goes to the warning");
	test_enter(app);

	/* Sent: busy, then Done. */
	se_dialog_set_text(app, 0U, "kei");
	test_enter(app);
	test_check(app->password.asked && app->dialog.busy, "the methods are asked, busy");
	(void)se_password_result(app, app->password.request, 0);
	test_check(app->password.step == PASSWORD_STEP_DONE && strstr(app->dialog.body, "console") != NULL, "Done says the console keeps the password");
	se_password_end(app);

	/* The PIN off: the password at once (step 1 of 1), the PIN's bit cleared. */
	(void)se_password_press(app, PASSWORD_METHOD_FIRST + 1);
	test_check(app->password.step == PASSWORD_STEP_CONFIRM && app->dialog.steps == 1U, "the PIN off asks the password at once");
	test_check(app->password.methods == 5U, "the PIN's bit cleared");

	/* A wrong password: said at the same step. */
	(void)setenv("HOST_REFUSAL", "bad-secret", 1);
	se_dialog_set_text(app, 0U, "wrong");
	test_enter(app);
	(void)se_password_result(app, app->password.request, EPERM);
	test_check(app->password.step == PASSWORD_STEP_CONFIRM && strstr(app->dialog.error, "password is wrong") != NULL, "a wrong password is said");
	(void)unsetenv("HOST_REFUSAL");
	se_password_end(app);

	/* The key on again from the password alone: the key's bit set. */
	(void)setenv("HOST_METHODS", "3", 1);
	(void)se_password_press(app, PASSWORD_METHOD_FIRST + 2);
	test_check(app->password.methods == 7U && app->password.step == PASSWORD_STEP_CONFIRM, "a key on asks the password");
	test_check(strstr(app->dialog.body, "will take your Security Key") != NULL, "turning on is said");
	se_password_end(app);
}
