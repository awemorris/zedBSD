/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The FullMAC connection engine.
 *
 * The firmware of a FullMAC radio associates and runs the WPA2 supplicant by
 * itself.  The engine keeps the common core's view of that connection: it
 * derives the PMK the firmware needs, follows the firmware's ASSOCIATED and
 * AUTHORIZED events in order, fails the attempt when the overall deadline
 * passes first, and undoes the controlled port and the radio connection as
 * checked barriers that a later stop retries.
 */

#include "wlan-fullmac.h"
#include <kern/kcrt.h>

#include "wlan-crypto.h"

#include <uapi/errno.h>

/* The PBKDF2 iteration count IEEE 802.11 fixes for a passphrase PMK. */
#define WLAN_FULLMAC_PBKDF2_ITERATIONS 4096U

static int ops_valid(const struct wlan_fullmac_ops *ops);
static int profile_valid(const struct wlan_fullmac_profile *profile);
static int deadline_passed(uint64_t now_ticks, uint64_t deadline_ticks);
static int attempt_live(const struct wlan_fullmac_engine *engine);
static void record_failure(struct wlan_fullmac_engine *engine, int error);

/*
 * Initializes an idle engine bound to the station's callbacks.
 */
int
wlan_fullmac_engine_init(
	struct wlan_fullmac_engine *engine,
	const struct wlan_fullmac_ops *ops,
	void *callback_context)
{
	int valid;

	/* Rejects a missing engine. */
	if (engine == NULL)
		return EINVAL;

	/* Rejects an incomplete callback set. */
	valid = ops_valid(ops);
	if (!valid)
		return EINVAL;

	/* Starts idle, bound to the callbacks, with no request held. */
	kern_memset(engine, 0, sizeof(*engine));
	engine->ops = ops;
	engine->callback_context = callback_context;
	engine->state = WLAN_FULLMAC_STATE_IDLE;

	/* Succeeded: the engine waits for its first connection. */
	return 0;
}

/*
 * Starts one connection attempt and hands it to the radio.
 *
 * The PMK is derived from the passphrase into the request, the request is
 * given to the station's connect callback, and the request is erased as soon
 * as the callback returns, so the engine keeps no secret.  Every failure after
 * the attempt is accepted leaves the engine FAILED with that error.
 */
int
wlan_fullmac_engine_start(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	const struct wlan_fullmac_profile *profile,
	uint64_t now_ticks)
{
	int valid;
	int expired;
	int error;

	/* Rejects a missing engine or a zero generation. */
	if (engine == NULL || generation == 0U)
		return EINVAL;

	/* Rejects an engine that was never bound to callbacks. */
	valid = ops_valid(engine->ops);
	if (!valid)
		return EINVAL;

	/* Rejects a profile that cannot describe a WPA2-PSK connection. */
	valid = profile_valid(profile);
	if (!valid)
		return EINVAL;

	/* Only an idle engine starts; the station stops an earlier attempt first. */
	if (engine->state != WLAN_FULLMAC_STATE_IDLE)
		return EBUSY;

	/* Accepts the attempt: from here on a failure is recorded as FAILED. */
	engine->generation = generation;
	engine->total_deadline_ticks = profile->total_deadline_ticks;
	engine->last_error = 0;
	engine->configured = 0U;
	engine->authorized = 0U;

	/* An attempt whose whole budget is already spent never reaches the radio. */
	expired = deadline_passed(now_ticks, engine->total_deadline_ticks);
	if (expired) {
		record_failure(engine, ETIMEDOUT);
		return ETIMEDOUT;
	}

	/* Derives the PMK the firmware supplicant will use, salted with the SSID. */
	kern_memset(&engine->request, 0, sizeof(engine->request));
	error = wlan_pbkdf2_hmac_sha1(profile->passphrase,
	    profile->passphrase_length, profile->bss.ssid,
	    profile->bss.ssid_length, WLAN_FULLMAC_PBKDF2_ITERATIONS,
	    engine->request.pmk, sizeof(engine->request.pmk));
	if (error != 0) {
		wlan_crypto_erase(&engine->request, sizeof(engine->request));
		record_failure(engine, error);
		return error;
	}

	/* Describes the connection the radio is to make. */
	engine->request.generation = generation;
	engine->request.deadline_ticks = engine->total_deadline_ticks;
	engine->request.bss = profile->bss;

	/*
	 * The radio may have taken the request even when it reports a failure,
	 * so configured is set before the call: radio_stop is owed from now on.
	 */
	engine->configured = 1U;

	/* Hands the request to the radio, then erases the PMK at once. */
	error = engine->ops->connect(engine->callback_context, generation,
	    &engine->request);
	wlan_crypto_erase(&engine->request, sizeof(engine->request));

	/* Reports why the radio did not take the connection. */
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* The firmware now owns the association and the handshakes. */
	engine->state = WLAN_FULLMAC_STATE_CONNECTING;

	/* Succeeded: the attempt waits for the firmware's events. */
	return 0;
}

/*
 * Applies one firmware event to the attempt it belongs to.
 *
 * An event of another generation, or of an attempt that already ended, is
 * stale.  A repeated or out-of-order event of the live attempt changes
 * nothing.  AUTHORIZED opens the controlled port through authorized_set; the
 * firmware may report it without a separate ASSOCIATED first.
 */
int
wlan_fullmac_engine_report(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	enum wlan_fullmac_event event,
	uint64_t now_ticks)
{
	int valid;
	int expired;
	int error;

	/* Rejects a missing engine. */
	if (engine == NULL)
		return EINVAL;

	/* Rejects an engine that was never bound to callbacks. */
	valid = ops_valid(engine->ops);
	if (!valid)
		return EINVAL;

	/* Rejects an event the firmware interface does not define. */
	if (event != WLAN_FULLMAC_EVENT_ASSOCIATED &&
	    event != WLAN_FULLMAC_EVENT_AUTHORIZED)
		return EINVAL;

	/* An event of another connection is stale. */
	if (generation == 0U || generation != engine->generation)
		return ESTALE;

	/* An event that arrives after the attempt ended is stale too. */
	if (engine->state == WLAN_FULLMAC_STATE_IDLE ||
	    engine->state == WLAN_FULLMAC_STATE_FAILED)
		return ESTALE;

	/* An authorized connection has nothing left to learn from either event. */
	if (engine->state == WLAN_FULLMAC_STATE_AUTHORIZED)
		return 0;

	/* An event after the overall deadline cannot rescue the attempt. */
	expired = deadline_passed(now_ticks, engine->total_deadline_ticks);
	if (expired) {
		record_failure(engine, ETIMEDOUT);
		return ETIMEDOUT;
	}

	/* The firmware joined: its supplicant now runs the handshakes. */
	if (event == WLAN_FULLMAC_EVENT_ASSOCIATED) {
		engine->state = WLAN_FULLMAC_STATE_ASSOCIATED;
		return 0;
	}

	/*
	 * The firmware has the keys.  authorized is set before the call, as the
	 * port may be partly open even when authorized_set reports a failure,
	 * so authorized_set(0) is owed from now on.
	 */
	engine->authorized = 1U;
	error = engine->ops->authorized_set(engine->callback_context,
	    engine->generation, 1);

	/* Reports why the controlled port could not be opened. */
	if (error != 0) {
		record_failure(engine, error);
		return error;
	}

	/* The connection is authorized and its port is open. */
	engine->state = WLAN_FULLMAC_STATE_AUTHORIZED;

	/* Succeeded: the connection is up. */
	return 0;
}

/*
 * Fails the live attempt after the radio reported the link lost.
 *
 * The first reason is kept when the attempt already failed (BUG-157).  An
 * open controlled port is closed now; a close that fails stays owed to
 * wlan_fullmac_engine_stop, which the caller runs next.
 */
int
wlan_fullmac_engine_fail(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	int error)
{
	int valid;
	int port_error;

	/* Rejects a missing engine or a reason that is not an error. */
	if (engine == NULL || error <= 0)
		return EINVAL;

	/* Rejects an engine that was never bound to callbacks. */
	valid = ops_valid(engine->ops);
	if (!valid)
		return EINVAL;

	/* A loss of another connection is stale. */
	if (generation == 0U || generation != engine->generation)
		return ESTALE;

	/* An idle engine has no connection to lose. */
	if (engine->state == WLAN_FULLMAC_STATE_IDLE)
		return ENOTCONN;

	/* Closes the controlled port; a failed close stays owed to stop. */
	if (engine->authorized) {
		port_error = engine->ops->authorized_set(
		    engine->callback_context, engine->generation, 0);
		if (port_error == 0)
			engine->authorized = 0U;
	}

	/* Records the loss, keeping an earlier reason. */
	record_failure(engine, error);

	/* Succeeded: the attempt is failed and waits for stop. */
	return 0;
}

/*
 * Fails an attempt the firmware did not authorize within its deadline.
 *
 * After authorization nothing is timed here: the firmware watches the
 * beacons and reports a loss by itself.
 */
int
wlan_fullmac_engine_timer(
	struct wlan_fullmac_engine *engine,
	uint64_t now_ticks)
{
	int live;
	int expired;

	/* Rejects a missing engine. */
	if (engine == NULL)
		return EINVAL;

	/* Only an attempt still waiting for the firmware has a deadline. */
	live = attempt_live(engine);
	if (!live)
		return 0;

	/* Fails the attempt once its whole budget is spent. */
	expired = deadline_passed(now_ticks, engine->total_deadline_ticks);
	if (expired) {
		record_failure(engine, ETIMEDOUT);
		return ETIMEDOUT;
	}

	/* Succeeded: the attempt still has time. */
	return 0;
}

/*
 * Retires the attempt: closes the controlled port, then stops the radio.
 *
 * Both are checked barriers.  A barrier that fails leaves the engine FAILED
 * with everything it still owes, and the next stop retries from there.  Only
 * when both are done does the engine return to idle.
 */
int
wlan_fullmac_engine_stop(
	struct wlan_fullmac_engine *engine)
{
	int valid;
	int error;

	/* Rejects a missing engine. */
	if (engine == NULL)
		return EINVAL;

	/* Rejects an engine that was never bound to callbacks. */
	valid = ops_valid(engine->ops);
	if (!valid)
		return EINVAL;

	/* Closes the controlled port before the radio connection goes. */
	if (engine->authorized) {
		error = engine->ops->authorized_set(engine->callback_context,
		    engine->generation, 0);
		if (error != 0) {
			record_failure(engine, error);
			return error;
		}

		engine->authorized = 0U;
	}

	/* Retires the radio connection the attempt handed out. */
	if (engine->configured) {
		error = engine->ops->radio_stop(engine->callback_context,
		    engine->generation);
		if (error != 0) {
			record_failure(engine, error);
			return error;
		}

		engine->configured = 0U;
	}

	/* Forgets the attempt; nothing more is owed for it. */
	engine->state = WLAN_FULLMAC_STATE_IDLE;
	engine->generation = 0U;
	engine->total_deadline_ticks = 0U;
	engine->last_error = 0;
	wlan_crypto_erase(&engine->request, sizeof(engine->request));

	/* Succeeded: the engine is idle again. */
	return 0;
}

/*
 * Reports the engine state.
 */
enum wlan_fullmac_state
wlan_fullmac_engine_state(
	const struct wlan_fullmac_engine *engine)
{
	/* A missing engine reads as failed, never as a usable idle one. */
	if (engine == NULL)
		return WLAN_FULLMAC_STATE_FAILED;

	/* Reports the state of the current attempt. */
	return engine->state;
}

/*
 * Reports the error that failed the attempt, or zero.
 */
int
wlan_fullmac_engine_last_error(
	const struct wlan_fullmac_engine *engine)
{
	/* A missing engine has no attempt to describe. */
	if (engine == NULL)
		return EINVAL;

	/* Reports the first error of the current attempt. */
	return engine->last_error;
}

/*
 * Reports when the timer must next run, or zero when it need not.
 */
uint64_t
wlan_fullmac_engine_next_deadline(
	const struct wlan_fullmac_engine *engine)
{
	int live;

	/* A missing engine has no deadline. */
	if (engine == NULL)
		return 0U;

	/* Only an attempt still waiting for the firmware is timed. */
	live = attempt_live(engine);
	if (!live)
		return 0U;

	/* Reports the overall connection deadline. */
	return engine->total_deadline_ticks;
}

/*
 * Tests that the engine holds no PMK.
 */
int
wlan_fullmac_engine_secrets_clear(
	const struct wlan_fullmac_engine *engine)
{
	const uint8_t *bytes;
	size_t index;

	/* A missing engine cannot be shown to be clear. */
	if (engine == NULL)
		return 0;

	/* Looks for any byte the request scratch still holds. */
	bytes = (const uint8_t *)&engine->request;
	for (index = 0U; index < sizeof(engine->request); index++) {
		if (bytes[index] != 0U)
			return 0;
	}

	/* Reports that no secret is left. */
	return 1;
}

/* Tests that a callback set is complete. */
static int
ops_valid(
	const struct wlan_fullmac_ops *ops)
{
	/* A missing set or any missing callback is incomplete. */
	if (ops == NULL)
		return 0;
	if (ops->connect == NULL)
		return 0;
	if (ops->authorized_set == NULL)
		return 0;
	if (ops->radio_stop == NULL)
		return 0;

	/* Reports a complete set. */
	return 1;
}

/* Tests that a profile names a network, a WPA2 passphrase, and a deadline. */
static int
profile_valid(
	const struct wlan_fullmac_profile *profile)
{
	/* A missing profile describes nothing. */
	if (profile == NULL)
		return 0;

	/* The SSID salts the PMK, so it must be present and in range. */
	if (profile->bss.ssid_length == 0U ||
	    profile->bss.ssid_length > WLAN_SSID_MAX)
		return 0;

	/* WPA2-PSK takes a passphrase of 8 to 63 characters. */
	if (profile->passphrase == NULL)
		return 0;
	if (profile->passphrase_length < WLAN_PASSPHRASE_MIN ||
	    profile->passphrase_length > WLAN_PASSPHRASE_MAX)
		return 0;

	/* Every attempt is bounded by an overall deadline. */
	if (profile->total_deadline_ticks == 0U)
		return 0;

	/* Reports a usable profile. */
	return 1;
}

/* Tests whether a nonzero deadline has been reached. */
static int
deadline_passed(
	uint64_t now_ticks,
	uint64_t deadline_ticks)
{
	/* A zero deadline is never reached. */
	if (deadline_ticks == 0U)
		return 0;

	/* The deadline is reached at its own tick. */
	if (now_ticks < deadline_ticks)
		return 0;

	/* Reports that the deadline has passed. */
	return 1;
}

/* Tests whether the attempt still waits for the firmware to authorize it. */
static int
attempt_live(
	const struct wlan_fullmac_engine *engine)
{
	/* The radio has the request, or the firmware has joined. */
	if (engine->state == WLAN_FULLMAC_STATE_CONNECTING)
		return 1;
	if (engine->state == WLAN_FULLMAC_STATE_ASSOCIATED)
		return 1;

	/* Reports an attempt that is not waiting. */
	return 0;
}

/* Marks the attempt failed, keeping the first error it failed with. */
static void
record_failure(
	struct wlan_fullmac_engine *engine,
	int error)
{
	/* An unspecified failure is still a failure. */
	if (error <= 0)
		error = EIO;

	/* The first reason is what the station reports (BUG-157). */
	if (engine->state != WLAN_FULLMAC_STATE_FAILED ||
	    engine->last_error == 0)
		engine->last_error = error;

	/* The attempt is over; stop retires whatever it still owes. */
	engine->state = WLAN_FULLMAC_STATE_FAILED;
}
