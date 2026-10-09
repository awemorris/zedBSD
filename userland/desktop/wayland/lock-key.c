/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The security key's mode of the login and the lock screen (lock-key.h,
 * ws199-p001 sections 3.6 and 3.7, review-3 R2, R5, R10).
 *
 * The screen listens for keys while it shows its field (the login screen
 * always; the lock screen only while its card shows, so a key plugged in
 * or a sleep's waking counts of the USB bus never unlock it).  A key that
 * comes or goes then asks KEYOWNER once the last one is a second old; the
 * answer names the owner, whose key's options choose the step:
 *
 *   the key's PIN asked          PIN (the keypad), then TOUCH once it is sent
 *   no PIN, the touch asked      TOUCH at once (the login screen always
 *                                asks the touch)
 *   the lock, neither asked      CHECKING at once, unlocked when granted and
 *                                "Checking" has shown KWL_KEY_CHECKING_MS
 *
 * A USB key that goes during an attempt cancels it, and the owner is asked
 * again after its answer (a card on a reader comes and goes as the user
 * holds it, so the attempt goes on).  An attempt that ends without the
 * user (its time, a cancel) is not repeated by itself: "Try again".
 */

#include "lock-key.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void key_step_from_options(struct kwl_key_mode *mode);
static int key_reason_is(const char *reason, const char *word);
static int key_is_owner(const struct kwl_key_mode *mode, const char *user);

/*
 * Starts the mode afresh for a screen: no step, no owner known, nothing
 * asked.  lock is 1 for the lock screen.
 */
void
kwl_key_reset(
	struct kwl_key_mode *mode,
	unsigned lock)
{
	/* Nothing of a former screen stays. */
	memset(mode, 0, sizeof(*mode));
	mode->lock = lock;
}

/*
 * Takes a key that came or went (the backend's keys_changed): with the
 * screen listening, the owner is to be asked; a USB key during an attempt
 * cancels it.  Returns KWL_KEY_CANCEL or KWL_KEY_NOTHING.
 */
enum kwl_key_action
kwl_key_keys_changed(
	struct kwl_key_mode *mode,
	unsigned listening)
{
	int attempting;

	/* A screen that does not listen (the lock screen before its card) does nothing. */
	if (!listening)
		return KWL_KEY_NOTHING;

	/*
	 * During an attempt a card comes and goes as the user holds it; a USB
	 * key that went (or another that came) ends the attempt, and the owner
	 * is asked once its answer came.
	 */
	attempting = kwl_key_attempting(mode);
	if (attempting) {
		if (mode->card)
			return KWL_KEY_NOTHING;
		mode->keys_moved = 1U;
		return KWL_KEY_CANCEL;
	}

	/* Otherwise the owner is asked (the last of a burst). */
	mode->owner_wanted = 1U;
	return KWL_KEY_NOTHING;
}

/*
 * Notes the screen began to listen (the login screen shown, the lock
 * screen's card brought): a key already there is asked about.
 */
void
kwl_key_listen(
	struct kwl_key_mode *mode)
{
	/* Asked once it may be. */
	mode->owner_wanted = 1U;
}

/*
 * Tells whether KEYOWNER may be asked now: wanted, none under way, no
 * attempt under way, and the last one a second old.  The caller also
 * waits while another request of the screen is answered.
 */
int
kwl_key_owner_due(
	const struct kwl_key_mode *mode,
	uint64_t now_ms)
{
	int attempting;

	/* Not wanted, or one under way. */
	if (!mode->owner_wanted || mode->owner_asked)
		return 0;

	/* An attempt is under way: its answer first. */
	attempting = kwl_key_attempting(mode);
	if (attempting)
		return 0;

	/* The last one less than a second ago. */
	if (mode->owner_ms != 0U && now_ms - mode->owner_ms < KWL_KEY_OWNER_MS)
		return 0;

	/* Succeeded: it may be asked. */
	return 1;
}

/*
 * Notes KEYOWNER was asked (error 0), refused for a busy backend (EBUSY:
 * asked again later) or could not be asked (another error: not again
 * until a key moves).
 */
void
kwl_key_owner_sent(
	struct kwl_key_mode *mode,
	uint64_t now_ms,
	int error)
{
	/* The backend was busy: still wanted. */
	if (error == EBUSY)
		return;

	/* Asked, or not to be asked. */
	mode->owner_wanted = 0U;
	if (error != 0)
		return;

	/* Under way since now. */
	mode->owner_asked = 1U;
	mode->owner_ms = now_ms;
}

/*
 * Takes KEYOWNER's answer.  user_shown says whether the screen offers the
 * owner (the login screen's users; the lock screen's own user always).
 * Returns KWL_KEY_ENTER (the owner and its step: an attempt starts for
 * TOUCH and CHECKING), KWL_KEY_NOT_HERE, KWL_KEY_LEAVE (the USB key went),
 * or KWL_KEY_NOTHING.
 */
enum kwl_key_action
kwl_key_owner_answer(
	struct kwl_key_mode *mode,
	const struct kwl_key_owner *owner,
	unsigned user_shown)
{
	int attempting;
	int same;

	/* No longer under way. */
	mode->owner_asked = 0U;

	/* sessiond answers one a second: asked again once it may be. */
	if (owner->error == EBUSY) {
		mode->owner_wanted = 1U;
		return KWL_KEY_NOTHING;
	}

	/* No answer, or an attempt began meanwhile: nothing changes. */
	attempting = kwl_key_attempting(mode);
	if (owner->error != 0 || attempting)
		return KWL_KEY_NOTHING;

	/* An owner the screen offers. */
	if (owner->found) {
		if (!user_shown || owner->user == NULL)
			return KWL_KEY_NOTHING;

		/* The same owner while its PIN is typed: what is typed stays. */
		same = key_is_owner(mode, owner->user);
		if (same && mode->step == KWL_KEY_PIN)
			return KWL_KEY_NOTHING;

		/* The owner, its key's options and its step. */
		mode->known = 1U;
		(void)snprintf(mode->user, sizeof(mode->user), "%s", owner->user);
		mode->key_pin = owner->key_pin;
		mode->key_touch = owner->key_touch;
		mode->card = owner->card;
		key_step_from_options(mode);
		return KWL_KEY_ENTER;
	}

	/* A key no account here has. */
	same = key_reason_is(owner->reason, "none");
	if (same) {
		mode->step = KWL_KEY_OFF;
		mode->known = 0U;
		return KWL_KEY_NOT_HERE;
	}

	/* No key: the USB key went (a card lifted from the reader leaves the mode as it is). */
	same = key_reason_is(owner->reason, "no-key");
	if (same && mode->step != KWL_KEY_OFF && !mode->card) {
		mode->step = KWL_KEY_OFF;
		mode->known = 0U;
		return KWL_KEY_LEAVE;
	}

	/* Two keys, a key of two accounts, or a failure: nothing changes. */
	return KWL_KEY_NOTHING;
}

/*
 * Takes the user's choice of the security key (its style chosen): the
 * owner already known for user enters its step; otherwise the owner is
 * asked, and the field takes the key's PIN meanwhile.
 */
enum kwl_key_action
kwl_key_choose(
	struct kwl_key_mode *mode,
	const char *user)
{
	int same;

	/* The owner known: its step. */
	same = key_is_owner(mode, user);
	if (same) {
		key_step_from_options(mode);
		return KWL_KEY_ENTER;
	}

	/* Otherwise asked. */
	mode->owner_wanted = 1U;
	return KWL_KEY_NOTHING;
}

/* Notes the attempt went (the PIN typed, or none): it waits for the touch, or checks the key. */
void
kwl_key_started(
	struct kwl_key_mode *mode,
	uint64_t now_ms)
{
	/* The PIN typed: the touch next. */
	if (mode->step == KWL_KEY_PIN)
		mode->step = KWL_KEY_TOUCH;

	/* Since now, not granted yet, no key moved. */
	mode->checking_ms = now_ms;
	mode->granted = 0U;
	mode->keys_moved = 0U;
}

/*
 * Takes the answer to the attempt (error 0 granted, else refused with
 * reason).  Returns KWL_KEY_GRANTED, KWL_KEY_WAIT (granted; "Checking" is
 * to show longer), KWL_KEY_REFUSED (the step says what follows: PIN asks
 * it again, AGAIN offers "Try again"), or KWL_KEY_NOTHING (no attempt of
 * the mode, or one ended because its key went: the owner is asked again).
 */
enum kwl_key_action
kwl_key_answer(
	struct kwl_key_mode *mode,
	int error,
	const char *reason,
	uint64_t now_ms)
{
	int attempting;
	int timeout;
	int pin;

	/* Only an attempt of the mode. */
	attempting = kwl_key_attempting(mode);
	if (!attempting)
		return KWL_KEY_NOTHING;

	/* Granted: at once, or once "Checking" has shown long enough. */
	if (error == 0) {
		if (mode->step == KWL_KEY_CHECKING && now_ms - mode->checking_ms < KWL_KEY_CHECKING_MS) {
			mode->granted = 1U;
			return KWL_KEY_WAIT;
		}

		/* Done. */
		mode->step = KWL_KEY_OFF;
		return KWL_KEY_GRANTED;
	}

	/* Ended without the key's judgment: the time, or a cancel. */
	timeout = key_reason_is(reason, "timeout");
	if (!timeout)
		timeout = key_reason_is(reason, "canceled");
	if (timeout && mode->keys_moved) {
		mode->keys_moved = 0U;
		mode->step = KWL_KEY_AGAIN;
		mode->owner_wanted = 1U;
		return KWL_KEY_NOTHING;
	}

	/* The time ran out, or the user cancelled: "Try again". */
	if (timeout) {
		mode->step = KWL_KEY_AGAIN;
		return KWL_KEY_REFUSED;
	}

	/* A key that asks its PIN after all (alwaysUv): the PIN from now on. */
	pin = key_reason_is(reason, "pin-required");
	if (pin) {
		mode->key_pin = 1U;
		mode->step = KWL_KEY_PIN;
		return KWL_KEY_REFUSED;
	}

	/* Another refusal: the PIN again when it is asked, else "Try again". */
	mode->step = KWL_KEY_AGAIN;
	if (mode->key_pin)
		mode->step = KWL_KEY_PIN;
	return KWL_KEY_REFUSED;
}

/* Tells, once "Checking" has shown long enough, that a grant held back is due (KWL_KEY_GRANTED). */
enum kwl_key_action
kwl_key_tick(
	struct kwl_key_mode *mode,
	uint64_t now_ms)
{
	/* Nothing held back, or not yet. */
	if (!mode->granted)
		return KWL_KEY_NOTHING;
	if (now_ms - mode->checking_ms < KWL_KEY_CHECKING_MS)
		return KWL_KEY_NOTHING;

	/* Succeeded: due now. */
	mode->granted = 0U;
	mode->step = KWL_KEY_OFF;
	return KWL_KEY_GRANTED;
}

/* Takes "Try again": the owner's step again (KWL_KEY_ENTER), or KWL_KEY_NOTHING when nothing was to try again. */
enum kwl_key_action
kwl_key_try_again(
	struct kwl_key_mode *mode)
{
	/* Only after an attempt that ended without the user. */
	if (mode->step != KWL_KEY_AGAIN)
		return KWL_KEY_NOTHING;

	/* Succeeded: the owner's step. */
	key_step_from_options(mode);
	return KWL_KEY_ENTER;
}

/* Leaves the mode for another style the user chose (the owner stays known). */
void
kwl_key_leave(
	struct kwl_key_mode *mode)
{
	/* No step, nothing held back. */
	mode->step = KWL_KEY_OFF;
	mode->granted = 0U;
}

/*
 * Ends the mode for a sleep (R5): nothing more is asked.  Returns 1 when
 * an attempt was under way (the caller cancels it), 0 otherwise.
 */
int
kwl_key_sleep(
	struct kwl_key_mode *mode)
{
	int attempting;

	/* Whether an attempt was under way. */
	attempting = kwl_key_attempting(mode);

	/* No step, nothing wanted or held back. */
	mode->step = KWL_KEY_OFF;
	mode->owner_wanted = 0U;
	mode->granted = 0U;
	mode->keys_moved = 0U;
	if (attempting)
		return 1;

	/* Succeeded: nothing was under way. */
	return 0;
}

/* Tells whether an attempt of the mode is under way (TOUCH or CHECKING). */
int
kwl_key_attempting(
	const struct kwl_key_mode *mode)
{
	/* Waiting for the touch, or checking the key. */
	if (mode->step == KWL_KEY_TOUCH || mode->step == KWL_KEY_CHECKING)
		return 1;

	/* No attempt. */
	return 0;
}

/*
 * Chooses the owner's step from its key's options: the PIN when asked;
 * else the lock's unlock without the touch when not asked; else the touch
 * (the login screen always asks it).
 */
static void
key_step_from_options(
	struct kwl_key_mode *mode)
{
	/* Nothing held back from a former attempt. */
	mode->granted = 0U;
	mode->keys_moved = 0U;

	/* The PIN asked. */
	if (mode->key_pin) {
		mode->step = KWL_KEY_PIN;
		return;
	}

	/* The lock's unlock without the touch. */
	if (mode->lock && !mode->key_touch) {
		mode->step = KWL_KEY_CHECKING;
		return;
	}

	/* The touch. */
	mode->step = KWL_KEY_TOUCH;
}

/* Tells whether a reason is the word (no reason is no word). */
static int
key_reason_is(
	const char *reason,
	const char *word)
{
	int same;

	/* No reason. */
	if (reason == NULL)
		return 0;

	/* The word. */
	same = strcmp(reason, word);
	if (same != 0)
		return 0;

	/* Succeeded: it is. */
	return 1;
}

/* Tells whether user is the owner the mode knows. */
static int
key_is_owner(
	const struct kwl_key_mode *mode,
	const char *user)
{
	int same;

	/* No owner known. */
	if (!mode->known)
		return 0;

	/* Another account. */
	same = strcmp(mode->user, user);
	if (same != 0)
		return 0;

	/* Succeeded: the owner. */
	return 1;
}
