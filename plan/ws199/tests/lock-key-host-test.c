/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws199-p003 (i05): the host test of the security key's mode of the login
 * and the lock screen (userland/desktop/wayland/lock-key.c) and of the
 * PIN's keypad (lock-keypad.c): the steps the owner's options choose, the
 * half second of "Checking", a key that goes back to the password, a card
 * that comes and goes, nothing while the lock screen's card is closed,
 * KEYOWNER once a second, "Try again", a sleep; the keypad's keys, its
 * letters, Shift and its digits alone.
 */

#include "userland/desktop/wayland/lock-key.h"
#include "userland/desktop/wayland/lock-keypad.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed. */
static int failures;

static void check(int condition, const char *what);
static struct kwl_key_owner owner_found(const char *user, unsigned key_pin, unsigned key_touch, unsigned card);
static struct kwl_key_owner owner_reason(const char *reason);
static void test_login(void);
static void test_lock(void);
static void test_keys_moved(void);
static void test_refusals(void);
static void test_keypad(void);

int
main(void)
{
	/* Each part. */
	test_login();
	test_lock();
	test_keys_moved();
	test_refusals();
	test_keypad();

	/* The result. */
	if (failures != 0) {
		printf("lock-key-host-test: %d failures\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("lock-key-host-test: ok\n");
	return 0;
}

/* Notes a check that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Only a failure is said. */
	if (!condition) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

/* Makes KEYOWNER's answer naming an owner. */
static struct kwl_key_owner
owner_found(
	const char *user,
	unsigned key_pin,
	unsigned key_touch,
	unsigned card)
{
	struct kwl_key_owner owner;

	/* Found, with the key's options. */
	memset(&owner, 0, sizeof(owner));
	owner.found = 1U;
	owner.user = user;
	owner.key_pin = key_pin;
	owner.key_touch = key_touch;
	owner.card = card;
	return owner;
}

/* Makes KEYOWNER's answer with a reason. */
static struct kwl_key_owner
owner_reason(
	const char *reason)
{
	struct kwl_key_owner owner;

	/* Nobody's. */
	memset(&owner, 0, sizeof(owner));
	owner.reason = reason;
	return owner;
}

/* The login screen: the owner's PIN and touch, the touch alone, the same owner while the PIN is typed, KEYOWNER once a second. */
static void
test_login(void)
{
	struct kwl_key_mode mode;
	struct kwl_key_owner owner;
	enum kwl_key_action action;

	/* The screen shows: a key already there is asked about at once. */
	kwl_key_reset(&mode, 0U);
	check(!kwl_key_owner_due(&mode, 0U), "nothing asked before the screen listens");
	kwl_key_listen(&mode);
	check(kwl_key_owner_due(&mode, 5000U), "asked once the screen listens");
	kwl_key_owner_sent(&mode, 5000U, 0);
	check(!kwl_key_owner_due(&mode, 5000U), "not asked twice");

	/* The owner whose key asks the PIN: the PIN. */
	owner = owner_found("kei", 1U, 1U, 0U);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_PIN, "the PIN asked: the PIN step");
	check(strcmp(mode.user, "kei") == 0, "the owner kept");

	/* A key that moves while the PIN is typed: asked again within the second's bound; the same owner keeps what is typed. */
	action = kwl_key_keys_changed(&mode, 1U);
	check(action == KWL_KEY_NOTHING && mode.owner_wanted, "a key moved: asked again");
	check(!kwl_key_owner_due(&mode, 5500U), "not within the second");
	check(kwl_key_owner_due(&mode, 6000U), "after the second");
	kwl_key_owner_sent(&mode, 6000U, 0);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_NOTHING && mode.step == KWL_KEY_PIN, "the same owner while the PIN is typed: nothing changes");

	/* The PIN sent: the touch; granted at once. */
	kwl_key_started(&mode, 7000U);
	check(mode.step == KWL_KEY_TOUCH && kwl_key_attempting(&mode), "the PIN sent: the touch");
	check(!kwl_key_owner_due(&mode, 9000U), "nothing asked during an attempt");
	action = kwl_key_answer(&mode, 0, NULL, 7100U);
	check(action == KWL_KEY_GRANTED && mode.step == KWL_KEY_OFF, "granted");

	/* An owner whose key asks no PIN nor touch: the login screen asks the touch still. */
	kwl_key_reset(&mode, 0U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	owner = owner_found("ana", 0U, 0U, 0U);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_TOUCH, "the login always asks the touch");

	/* An owner the screen does not offer. */
	kwl_key_reset(&mode, 0U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	action = kwl_key_owner_answer(&mode, &owner, 0U);
	check(action == KWL_KEY_NOTHING && mode.step == KWL_KEY_OFF, "an owner not shown: nothing");

	/* sessiond busy: asked again after the second. */
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 2000U, 0);
	owner.error = EBUSY;
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_NOTHING && mode.owner_wanted && !kwl_key_owner_due(&mode, 2500U) && kwl_key_owner_due(&mode, 3000U),
	    "busy: asked again after the second");

	/* A backend busy with another request: still wanted. */
	kwl_key_owner_sent(&mode, 3000U, EBUSY);
	check(mode.owner_wanted && !mode.owner_asked, "a busy backend: still wanted");

	/* A key no account has here. */
	kwl_key_owner_sent(&mode, 4000U, 0);
	owner = owner_reason("none");
	action = kwl_key_owner_answer(&mode, &owner, 0U);
	check(action == KWL_KEY_NOT_HERE, "none: not registered here");

	/* Two keys: nothing. */
	owner = owner_reason("many-keys");
	action = kwl_key_owner_answer(&mode, &owner, 0U);
	check(action == KWL_KEY_NOTHING, "two keys: nothing");

	/* The style chosen: the known owner enters its step, another is asked about. */
	owner = owner_found("kei", 0U, 1U, 0U);
	(void)kwl_key_owner_answer(&mode, &owner, 1U);
	kwl_key_leave(&mode);
	check(mode.step == KWL_KEY_OFF, "the password chosen leaves the mode");
	action = kwl_key_choose(&mode, "kei");
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_TOUCH, "the key chosen again: the owner's step");
	kwl_key_leave(&mode);
	mode.owner_wanted = 0U;
	action = kwl_key_choose(&mode, "ana");
	check(action == KWL_KEY_NOTHING && mode.owner_wanted, "the key chosen for another user: asked");
}

/* The lock screen: nothing before the card, the half second of "Checking", the touch, a sleep. */
static void
test_lock(void)
{
	struct kwl_key_mode mode;
	struct kwl_key_owner owner;
	enum kwl_key_action action;

	/* Before the card: a key that comes does nothing. */
	kwl_key_reset(&mode, 1U);
	action = kwl_key_keys_changed(&mode, 0U);
	check(action == KWL_KEY_NOTHING && !mode.owner_wanted, "the lock before its card: nothing");

	/* The card: asked; neither the PIN nor the touch: Checking. */
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 10000U, 0);
	owner = owner_found("kei", 0U, 0U, 0U);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_CHECKING, "neither asked: Checking");

	/* Granted after 200 ms: held back until the half second. */
	kwl_key_started(&mode, 10000U);
	action = kwl_key_answer(&mode, 0, NULL, 10200U);
	check(action == KWL_KEY_WAIT, "granted early: wait");
	check(kwl_key_tick(&mode, 10499U) == KWL_KEY_NOTHING, "not before the half second");
	check(kwl_key_tick(&mode, 10500U) == KWL_KEY_GRANTED && mode.step == KWL_KEY_OFF, "unlocked at the half second");
	check(kwl_key_tick(&mode, 10600U) == KWL_KEY_NOTHING, "once");

	/* Granted after the half second: at once. */
	(void)kwl_key_choose(&mode, "kei");
	kwl_key_started(&mode, 20000U);
	action = kwl_key_answer(&mode, 0, NULL, 20600U);
	check(action == KWL_KEY_GRANTED, "granted late: at once");

	/* The touch asked: the touch on the lock too. */
	kwl_key_reset(&mode, 1U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	owner = owner_found("kei", 0U, 1U, 0U);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_TOUCH, "the touch asked: the touch");

	/* A sleep during the attempt: cancelled, nothing more asked. */
	kwl_key_started(&mode, 2000U);
	check(kwl_key_sleep(&mode) == 1 && mode.step == KWL_KEY_OFF && !mode.owner_wanted, "a sleep cancels the attempt");
	check(kwl_key_sleep(&mode) == 0, "a sleep without an attempt");
	action = kwl_key_answer(&mode, EACCES, "timeout", 3000U);
	check(action == KWL_KEY_NOTHING, "the cancelled attempt's answer is not the mode's");
}

/* A USB key that goes during an attempt, and a card that comes and goes. */
static void
test_keys_moved(void)
{
	struct kwl_key_mode mode;
	struct kwl_key_owner owner;
	enum kwl_key_action action;

	/* A USB key's attempt; the key goes: cancelled, asked again after the answer, the password. */
	kwl_key_reset(&mode, 0U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	owner = owner_found("kei", 0U, 1U, 0U);
	(void)kwl_key_owner_answer(&mode, &owner, 1U);
	kwl_key_started(&mode, 1100U);
	action = kwl_key_keys_changed(&mode, 1U);
	check(action == KWL_KEY_CANCEL, "a USB key moved during the attempt: cancel");
	action = kwl_key_answer(&mode, EACCES, "timeout", 1200U);
	check(action == KWL_KEY_NOTHING && mode.step == KWL_KEY_AGAIN && mode.owner_wanted, "its answer: the owner asked again");
	kwl_key_owner_sent(&mode, 2000U, 0);
	owner = owner_reason("no-key");
	action = kwl_key_owner_answer(&mode, &owner, 0U);
	check(action == KWL_KEY_LEAVE && mode.step == KWL_KEY_OFF, "the key went: the password");

	/* Plugged in again: the owner's step afresh. */
	action = kwl_key_keys_changed(&mode, 1U);
	kwl_key_owner_sent(&mode, 3000U, 0);
	owner = owner_found("kei", 0U, 1U, 0U);
	action = kwl_key_owner_answer(&mode, &owner, 1U);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_TOUCH, "plugged in again: the touch again");

	/* A card's attempt: cards coming and going do not stop it. */
	kwl_key_reset(&mode, 0U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	owner = owner_found("kei", 1U, 1U, 1U);
	(void)kwl_key_owner_answer(&mode, &owner, 1U);
	check(mode.step == KWL_KEY_PIN, "a card asking the PIN: the PIN");

	/* The card lifted while the PIN is typed: the mode stays. */
	(void)kwl_key_keys_changed(&mode, 1U);
	kwl_key_owner_sent(&mode, 2000U, 0);
	owner = owner_reason("no-key");
	action = kwl_key_owner_answer(&mode, &owner, 0U);
	check(action == KWL_KEY_NOTHING && mode.step == KWL_KEY_PIN, "a card lifted: the PIN stays");

	/* The PIN sent, the card held again: the attempt goes on. */
	kwl_key_started(&mode, 3000U);
	action = kwl_key_keys_changed(&mode, 1U);
	check(action == KWL_KEY_NOTHING && kwl_key_attempting(&mode), "a card held during the attempt: it goes on");
}

/* The refusals: the time, the PIN asked by the key, a wrong PIN, "Try again". */
static void
test_refusals(void)
{
	struct kwl_key_mode mode;
	struct kwl_key_owner owner;
	enum kwl_key_action action;

	/* The touch's time ran out: "Try again", and not again by itself. */
	kwl_key_reset(&mode, 0U);
	kwl_key_listen(&mode);
	kwl_key_owner_sent(&mode, 1000U, 0);
	owner = owner_found("kei", 0U, 1U, 0U);
	(void)kwl_key_owner_answer(&mode, &owner, 1U);
	kwl_key_started(&mode, 1000U);
	action = kwl_key_answer(&mode, EACCES, "timeout", 31000U);
	check(action == KWL_KEY_REFUSED && mode.step == KWL_KEY_AGAIN && !mode.owner_wanted, "the time ran out: Try again");
	action = kwl_key_try_again(&mode);
	check(action == KWL_KEY_ENTER && mode.step == KWL_KEY_TOUCH, "Try again: the touch again");
	check(kwl_key_try_again(&mode) == KWL_KEY_NOTHING, "nothing to try again during the attempt");

	/* A key that asks its PIN (alwaysUv): the PIN from now on. */
	kwl_key_started(&mode, 40000U);
	action = kwl_key_answer(&mode, EACCES, "pin-required", 40100U);
	check(action == KWL_KEY_REFUSED && mode.step == KWL_KEY_PIN && mode.key_pin == 1U, "pin-required: the PIN");

	/* A wrong key PIN: the PIN again. */
	kwl_key_started(&mode, 41000U);
	action = kwl_key_answer(&mode, EACCES, "bad-secret", 41100U);
	check(action == KWL_KEY_REFUSED && mode.step == KWL_KEY_PIN, "a wrong key PIN: the PIN again");

	/* A refusal without the PIN asked: Try again. */
	mode.key_pin = 0U;
	(void)kwl_key_choose(&mode, "kei");
	kwl_key_started(&mode, 42000U);
	action = kwl_key_answer(&mode, EACCES, "cloned", 42100U);
	check(action == KWL_KEY_REFUSED && mode.step == KWL_KEY_AGAIN, "another refusal: Try again");
}

/* The keypad: its digits, its letters, Shift, the digits alone. */
static void
test_keypad(void)
{
	struct kwl_keypad pad;
	struct kwl_keypad_key keys[KWL_KEYPAD_KEYS];
	enum kwl_keypad_action action;
	size_t count;
	size_t index;
	int32_t height;
	char character;
	int hit;
	int inside;

	/* Four rows of keys and three gaps. */
	check(kwl_keypad_height() == 4 * KWL_KEYPAD_KEY + 3 * KWL_KEYPAD_GAP, "the keypad's height");

	/* The digits: twelve keys inside the width; 5 in the middle. */
	kwl_keypad_reset(&pad, 0U);
	count = kwl_keypad_layout(&pad, 100, 200, 332, keys, KWL_KEYPAD_KEYS);
	check(count == 12U, "twelve digits' keys");
	inside = 1;
	height = kwl_keypad_height();
	for (index = 0U; index < count; index++) {
		if (keys[index].rect[0] < 100 || keys[index].rect[0] + keys[index].rect[2] > 432)
			inside = 0;
		if (keys[index].rect[1] < 200 || keys[index].rect[1] + keys[index].rect[3] > 200 + height)
			inside = 0;
	}

	/* None outside. */
	check(inside, "the digits inside the keypad");
	hit = kwl_keypad_hit(keys, count, 100 + 166, 200 + KWL_KEYPAD_KEY + KWL_KEYPAD_GAP + 10);
	check(hit >= 0 && keys[hit].character == '5', "the middle is 5");
	action = kwl_keypad_press(&pad, &keys[hit], &character);
	check(action == KWL_KEYPAD_CHARACTER && character == '5', "5 typed");
	check(kwl_keypad_hit(keys, count, 99, 200) == -1, "nothing left of it");
	check(keys[9].action == KWL_KEYPAD_LETTERS && keys[11].action == KWL_KEYPAD_BACKSPACE, "ABC and Backspace in the last row");

	/* ABC: the letters, thirty keys inside the width. */
	action = kwl_keypad_press(&pad, &keys[9], &character);
	check(action == KWL_KEYPAD_LETTERS && pad.letters, "ABC: the letters");
	count = kwl_keypad_layout(&pad, 100, 200, 332, keys, KWL_KEYPAD_KEYS);
	check(count == 30U && keys[0].character == 'q', "thirty letters' keys from q");
	inside = 1;
	for (index = 0U; index < count; index++) {
		if (keys[index].rect[0] < 100 || keys[index].rect[0] + keys[index].rect[2] > 432)
			inside = 0;
	}

	/* None outside. */
	check(inside, "the letters inside the keypad");

	/* Shift once: one capital. */
	check(keys[19].action == KWL_KEYPAD_SHIFT && keys[27].action == KWL_KEYPAD_BACKSPACE, "Shift and Backspace in the third row");
	(void)kwl_keypad_press(&pad, &keys[19], &character);
	(void)kwl_keypad_press(&pad, &keys[0], &character);
	check(character == 'Q', "Shift: a capital");
	(void)kwl_keypad_press(&pad, &keys[0], &character);
	check(character == 'q', "then small again");

	/* Shift twice: capitals until it is pressed again. */
	(void)kwl_keypad_press(&pad, &keys[19], &character);
	(void)kwl_keypad_press(&pad, &keys[19], &character);
	(void)kwl_keypad_press(&pad, &keys[1], &character);
	check(character == 'W', "Shift held: a capital");
	(void)kwl_keypad_press(&pad, &keys[1], &character);
	check(character == 'W', "Shift held: still");
	(void)kwl_keypad_press(&pad, &keys[19], &character);
	(void)kwl_keypad_press(&pad, &keys[1], &character);
	check(character == 'w', "Shift let go");

	/* 123 and OK. */
	check(keys[28].action == KWL_KEYPAD_DIGITS && keys[29].action == KWL_KEYPAD_ENTER, "123 and OK");
	(void)kwl_keypad_press(&pad, &keys[28], &character);
	check(!pad.letters, "123: the digits");

	/* The six digits of the PIN: the digits alone, OK for ABC. */
	kwl_keypad_reset(&pad, 1U);
	pad.letters = 1U;
	count = kwl_keypad_layout(&pad, 0, 0, 300, keys, KWL_KEYPAD_KEYS);
	check(count == 12U && keys[9].action == KWL_KEYPAD_ENTER, "the digits alone: OK, no letters");

	/* No room: as many as fit. */
	kwl_keypad_reset(&pad, 0U);
	count = kwl_keypad_layout(&pad, 0, 0, 300, keys, 5U);
	check(count == 5U, "as many keys as there is room for");
}
