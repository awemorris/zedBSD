/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The security key's mode of the login screen and the lock screen
 * (lock-key.c, ws199-p001 sections 3.6 and 3.7): when a key is plugged in
 * or held to a reader, the screen asks sessiond whose it is (KEYOWNER, at
 * most once a second, the last of a burst), selects that user, and asks
 * for the key's PIN (a keypad under the field) and its touch, or for the
 * touch alone; the lock screen of an account that asks neither unlocks
 * after "Checking your security key..." has shown for half a second.
 *
 * It knows nothing of the server, the backend or the fonts: the caller
 * hands it the events, the answers and the times, and acts on what it
 * says (greeter.c).  So the host tests run it alone.
 */

#ifndef KWL_LOCK_KEY_H
#define KWL_LOCK_KEY_H

#include <stdint.h>

/* The fewest milliseconds between two KEYOWNER (sessiond answers one a second, R10). */
#define KWL_KEY_OWNER_MS	1000U

/* How long the lock screen shows "Checking your security key..." at least before it unlocks (milliseconds). */
#define KWL_KEY_CHECKING_MS	500U

/* The longest account name the mode keeps, with its end. */
#define KWL_KEY_USER		33U

/*
 * Where the mode is: none (the field takes what the user chose), the
 * key's PIN being typed, an attempt under way that waits for the touch,
 * the lock's unlock without the PIN or the touch under way, or an attempt
 * that ended without the user (the touch's time ran out): "Try again".
 */
enum kwl_key_step {
	KWL_KEY_OFF,
	KWL_KEY_PIN,
	KWL_KEY_TOUCH,
	KWL_KEY_CHECKING,
	KWL_KEY_AGAIN
};

/* What the caller is to do after an event. */
enum kwl_key_action {
	KWL_KEY_NOTHING,	/* nothing more */
	KWL_KEY_ENTER,		/* select the owner and show the step (an attempt starts for TOUCH and CHECKING) */
	KWL_KEY_START,		/* send the attempt (the PIN typed, or none) */
	KWL_KEY_NOT_HERE,	/* say the key is not registered here */
	KWL_KEY_LEAVE,		/* the key went: back to the password */
	KWL_KEY_CANCEL,		/* cancel the attempt under way (its key went) */
	KWL_KEY_GRANTED,	/* the attempt was granted: log in or unlock now */
	KWL_KEY_WAIT,		/* granted, but "Checking" has not shown long enough yet */
	KWL_KEY_REFUSED		/* refused: say why (the step says what follows) */
};

/*
 * The mode: its step, whether it is the lock screen's, the owner sessiond
 * last named (with its key's options and whether the key is a card), the
 * KEYOWNER to ask (wanted), under way (asked) and when the last went, when
 * "Checking" began and whether its grant waits for it, and whether keys
 * came or went during the attempt (it is to be asked again after it).
 */
struct kwl_key_mode {
	enum kwl_key_step step;
	unsigned lock;
	unsigned known;
	char user[KWL_KEY_USER];
	unsigned key_pin;
	unsigned key_touch;
	unsigned card;
	unsigned owner_wanted;
	unsigned owner_asked;
	uint64_t owner_ms;
	uint64_t checking_ms;
	unsigned granted;
	unsigned keys_moved;
};

/*
 * KEYOWNER's answer as the caller got it: found with the owner's name and
 * options, or the reason (none, no-key, many-owners, ...); error is the
 * backend's (EBUSY: asked again a second later).
 */
struct kwl_key_owner {
	int error;
	unsigned found;
	const char *user;
	unsigned key_pin;
	unsigned key_touch;
	unsigned card;
	const char *reason;
};

void kwl_key_reset(struct kwl_key_mode *mode, unsigned lock);
enum kwl_key_action kwl_key_keys_changed(struct kwl_key_mode *mode, unsigned listening);
void kwl_key_listen(struct kwl_key_mode *mode);
int kwl_key_owner_due(const struct kwl_key_mode *mode, uint64_t now_ms);
void kwl_key_owner_sent(struct kwl_key_mode *mode, uint64_t now_ms, int error);
enum kwl_key_action kwl_key_owner_answer(struct kwl_key_mode *mode, const struct kwl_key_owner *owner, unsigned user_shown);
enum kwl_key_action kwl_key_choose(struct kwl_key_mode *mode, const char *user);
void kwl_key_started(struct kwl_key_mode *mode, uint64_t now_ms);
enum kwl_key_action kwl_key_answer(struct kwl_key_mode *mode, int error, const char *reason, uint64_t now_ms);
enum kwl_key_action kwl_key_tick(struct kwl_key_mode *mode, uint64_t now_ms);
enum kwl_key_action kwl_key_try_again(struct kwl_key_mode *mode);
void kwl_key_leave(struct kwl_key_mode *mode);
int kwl_key_sleep(struct kwl_key_mode *mode);
int kwl_key_attempting(const struct kwl_key_mode *mode);

#endif
