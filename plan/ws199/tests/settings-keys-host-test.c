/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the Security Keys page's wizards (ws199-p004, design
 * section 7): page-users-keys.c is included here so that its static
 * steps are reached, and linked with the rest of Settings' host build
 * (plan/ws089/tests/host-build.sh) and its stand-in desktop
 * (host-kl-system.c: two keys listed, one key there with a PIN unless
 * HOST_KEY_NO_PIN, the keys' own operations with HOST_KEY_OPS).
 *
 * Checked: a key's name (what is refused, the default one: the key's own
 * without its colon, else "Security Key" or the first free numbered one),
 * the Add Key steps from the password to the touch and the end (the
 * password kept and wiped at the end), the page refusing a second wizard
 * while one is open and the popup refusing Enter while busy, the idle time
 * (two minutes) wiping the password kept and asking it again (not while
 * busy), the new PIN typed twice (the same, long enough), and the warning
 * before a weaker way to sign in (none for the way now).
 */

#include "page-users-keys.c"

#include <stdlib.h>

/* The stand-in desktop's request numbers: what the keys are, and an addition. */
#define TEST_INFO_REQUEST	80U
#define TEST_ADD_REQUEST	77U

/* The popup's idle time (dialog.c's DIALOG_IDLE_MS, the two minutes of design section 3.2). */
#define TEST_IDLE_MS		120000U

/* The checks that failed. */
static unsigned test_failures;

static void test_check(int condition, const char *what);
static void test_enter(struct se_app *app);
static void test_names(struct se_app *app);
static void test_add(struct se_app *app);
static void test_idle(struct se_app *app);
static void test_pins(struct se_app *app);
static void test_options(struct se_app *app);

/*
 * Runs the checks; the exit status says whether they all held.
 */
int
main(void)
{
	static struct se_app app;

	/* The stand-in desktop: two keys listed, one key there with a PIN, the keys' own operations. */
	(void)setenv("HOST_KEYS", "1", 1);
	(void)setenv("HOST_KEY_OPS", "1", 1);
	memset(&app, 0, sizeof(app));
	app.system = (struct kl_system *)&app;
	app.now = 1000U;

	/* Each part. */
	test_names(&app);
	test_add(&app);
	test_idle(&app);
	test_pins(&app);
	test_options(&app);

	/* The verdict. */
	if (test_failures != 0U) {
		printf("settings-keys-host-test: FAIL (%u)\n", test_failures);
		return 1;
	}

	/* Every check held. */
	printf("settings-keys-host-test: PASS\n");
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

/* A key's name: refused ones, and the default one. */
static void
test_names(
	struct se_app *app)
{
	char name[KL_SYSTEM_KEY_LABEL_MAX + 1U];
	char long_name[KL_SYSTEM_KEY_LABEL_MAX + 2U];

	/* What a name may not be. */
	memset(long_name, 'k', sizeof(long_name) - 1U);
	long_name[sizeof(long_name) - 1U] = '\0';
	test_check(!keys_name_valid("", 0U), "an empty name is refused");
	test_check(!keys_name_valid("   ", 3U), "spaces alone are refused");
	test_check(!keys_name_valid("a:b", 3U), "a colon is refused");
	test_check(!keys_name_valid("a\tb", 3U), "a control character is refused");
	test_check(!keys_name_valid(long_name, strlen(long_name)), "a name of 33 bytes is refused");
	test_check(keys_name_valid(long_name, KL_SYSTEM_KEY_LABEL_MAX), "a name of 32 bytes is taken");
	test_check(keys_name_valid(" Work key ", 10U), "a name with spaces around it is taken");

	/* No key told: "Security Key", free among the two listed. */
	app->keys.info_known = 0;
	keys_default_name(app, name, sizeof(name));
	test_check(strcmp(name, "Security Key") == 0, "the default name without the key's own");

	/* The key's own name, already listed: "Security Key". */
	app->keys.info_known = 1;
	(void)snprintf(app->keys.info.name, sizeof(app->keys.info.name), "%s", "YubiKey 5 NFC");
	keys_default_name(app, name, sizeof(name));
	test_check(strcmp(name, "Security Key") == 0, "a listed own name gives way to Security Key");

	/* The key's own name, its colon made a space. */
	(void)snprintf(app->keys.info.name, sizeof(app->keys.info.name), "%s", "Token:2");
	keys_default_name(app, name, sizeof(name));
	test_check(strcmp(name, "Token 2") == 0, "the key's own name without its colon");
	app->keys.info_known = 0;
}

/* Add Key from the password to the end. */
static void
test_add(
	struct se_app *app)
{
	unsigned flow;

	/* Add Key opens the password's step, not ready while empty. */
	se_keys_press(app, KEYS_ADD);
	test_check(app->dialog.open && app->keys.flow == SE_KEYS_FLOW_ADD, "Add Key opens the wizard");
	test_check(app->keys.step == KEYS_STEP_PASSWORD, "Add Key starts at the password");
	test_check(!keys_ready(app), "the password's step waits for the password");

	/* The password typed: on to the key, busy while it is asked about. */
	se_dialog_set_text(app, 0U, "secret");
	test_check(keys_ready(app), "the password's step is ready with one");
	test_enter(app);
	test_check(app->keys.step == KEYS_STEP_INSERT && app->keys.info_asked, "the key's step asks what is there");
	test_check(app->dialog.busy, "the popup is busy while the key is asked about");
	test_check(strcmp(app->keys.password.text, "secret") == 0, "the password is kept for the addition");

	/* Busy: Enter does nothing, and the page opens no second wizard. */
	test_enter(app);
	test_check(app->keys.step == KEYS_STEP_INSERT, "Enter does nothing while busy");
	flow = app->keys.flow;
	se_keys_press(app, KEYS_RESET);
	test_check(app->keys.flow == flow, "no second wizard while one is open");

	/* One key there: its name, the default one. */
	(void)se_keys_result(app, TEST_INFO_REQUEST, 0);
	test_check(app->keys.step == KEYS_STEP_NAME, "one key there: its name");
	test_check(strcmp(se_dialog_text(app, 0U), "Security Key") == 0, "the name's field holds the default name");

	/* A name with spaces around it: kept without them, then the key's PIN. */
	se_dialog_set_text(app, 0U, "  Work key ");
	test_enter(app);
	test_check(strcmp(app->keys.name, "Work key") == 0, "the name is kept without its spaces");
	test_check(app->keys.step == KEYS_STEP_PIN, "a key with a PIN: its PIN");

	/* The key's PIN: four characters at least. */
	se_dialog_set_text(app, 0U, "123");
	test_check(!keys_ready(app), "a PIN of three is not enough");
	se_dialog_set_text(app, 0U, "1234");
	test_check(keys_ready(app), "a PIN of four is enough");

	/* Sent: the touch, busy and cancellable. */
	test_enter(app);
	test_check(app->keys.step == KEYS_STEP_TOUCH && app->keys.asked, "the addition is asked, the touch shown");
	test_check(app->dialog.busy && app->dialog.cancellable, "the touch is busy and cancellable");

	/* Its answer: the end, the password wiped. */
	(void)se_keys_result(app, TEST_ADD_REQUEST, 0);
	test_check(app->keys.step == KEYS_STEP_DONE && !app->keys.asked, "the addition's answer ends the wizard");
	test_check(app->keys.password.length == 0U, "the password is wiped at the end");

	/* Done closes. */
	test_enter(app);
	test_check(!app->dialog.open, "Done closes the popup");
}

/* The idle time wipes the password kept, not while busy. */
static void
test_idle(
	struct se_app *app)
{
	/* An addition at the key's step, busy: the idle time does nothing. */
	se_keys_press(app, KEYS_ADD);
	se_dialog_set_text(app, 0U, "secret");
	test_enter(app);
	app->now += 2U * TEST_IDLE_MS;
	se_dialog_tick(app, app->now);
	test_check(app->keys.step == KEYS_STEP_INSERT && app->keys.password.length != 0U, "busy: the idle time keeps the password");

	/* At the name, left alone a minute: nothing yet. */
	(void)se_keys_result(app, TEST_INFO_REQUEST, 0);
	app->dialog.input_ms = app->now;
	app->now += TEST_IDLE_MS / 2U;
	se_dialog_tick(app, app->now);
	test_check(app->keys.step == KEYS_STEP_NAME && app->keys.password.length != 0U, "a minute alone keeps the password");

	/* Two minutes alone: the password wiped and asked again. */
	app->now += TEST_IDLE_MS / 2U;
	se_dialog_tick(app, app->now);
	test_check(app->keys.step == KEYS_STEP_PASSWORD, "two minutes alone: the password's step again");
	test_check(app->keys.password.length == 0U, "two minutes alone: the password wiped");
	test_check(app->dialog.error[0] != '\0', "two minutes alone: said why");

	/* Closed. */
	se_keys_end(app);
}

/* The new PIN typed twice. */
static void
test_pins(
	struct se_app *app)
{
	/* Change PIN on a key without one: its first PIN. */
	(void)setenv("HOST_KEY_NO_PIN", "1", 1);
	se_keys_press(app, KEYS_CHANGE_PIN);
	test_check(app->keys.flow == SE_KEYS_FLOW_KEY_PIN && app->keys.step == KEYS_STEP_INSERT, "Change PIN starts at the key");
	(void)se_keys_result(app, TEST_INFO_REQUEST, 0);
	test_check(app->keys.step == KEYS_STEP_SET_PIN, "a key without a PIN: its first PIN");

	/* Two different PINs, a short one twice, the same twice. */
	se_dialog_set_text(app, 0U, "1234");
	se_dialog_set_text(app, 1U, "1235");
	test_check(!keys_ready(app), "two different PINs are not ready");
	se_dialog_set_text(app, 0U, "123");
	se_dialog_set_text(app, 1U, "123");
	test_check(!keys_ready(app), "a short PIN twice is not ready");
	se_dialog_set_text(app, 0U, "1234");
	se_dialog_set_text(app, 1U, "1234");
	test_check(keys_ready(app), "the same PIN twice is ready");
	se_keys_end(app);

	/* A key with a PIN: the PIN now and a new one twice. */
	(void)unsetenv("HOST_KEY_NO_PIN");
	se_keys_press(app, KEYS_CHANGE_PIN);
	(void)se_keys_result(app, TEST_INFO_REQUEST, 0);
	test_check(app->keys.step == KEYS_STEP_CHANGE, "a key with a PIN: the PIN now and a new one");
	se_dialog_set_text(app, 1U, "567890");
	se_dialog_set_text(app, 2U, "567890");
	test_check(!keys_ready(app), "without the PIN now it is not ready");
	se_dialog_set_text(app, 0U, "1234");
	test_check(keys_ready(app), "the PIN now and the new one twice are ready");
	se_keys_end(app);
}

/* The warning before a weaker way to sign in. */
static void
test_options(
	struct se_app *app)
{
	/* The way now (the PIN and the touch): nothing opens. */
	se_keys_press(app, KEYS_OPTION_FIRST);
	test_check(!app->dialog.open, "the way now opens nothing");

	/* Neither: the warning first, naming the plugged-in key. */
	se_keys_press(app, KEYS_OPTION_FIRST + 2);
	test_check(app->keys.flow == SE_KEYS_FLOW_OPTIONS && app->keys.weaker, "a weaker way opens its wizard");
	test_check(app->keys.step == KEYS_STEP_WARNING, "a weaker way warns first");
	test_check(strstr(app->dialog.body, "swipe") != NULL, "no touch to unlock warns of the key left there");

	/* Read: the password, and Back to the warning. */
	test_enter(app);
	test_check(app->keys.step == KEYS_STEP_PASSWORD, "the warning read: the password");
	keys_back(app);
	test_check(app->keys.step == KEYS_STEP_WARNING, "Back from the password: the warning");
	se_keys_end(app);

	/* Touch only, from neither (stronger): the password alone. */
	(void)setenv("HOST_KEY_OPTION", "2", 1);
	se_keys_press(app, KEYS_OPTION_FIRST + 1);
	test_check(!app->keys.weaker && app->keys.step == KEYS_STEP_PASSWORD, "a stronger way asks the password alone");
	se_keys_end(app);
	(void)unsetenv("HOST_KEY_OPTION");
}
