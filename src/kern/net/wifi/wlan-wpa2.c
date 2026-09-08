/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WPA2-PSK supplicant engine.
 *
 * The engine drives one station through authentication, association,
 * the four-way handshake, and later pairwise and group rekeys as a
 * state machine fed by frames, transmit reports, and a timer.  Every
 * hardware effect goes through checked callbacks whose failure leaves
 * the engine in a state cleanup() can always drive back to idle, and
 * every secret is erased as soon as it is no longer needed.
 */

#include "wlan-wpa2.h"

#include <errno.h>
#include <string.h>

#define WPA2_EAPOL_MIC_OFFSET 81U
#define WPA2_EAPOL_MIC_LENGTH WLAN_WPA2_KEY_MIC_LENGTH
#define WPA2_PTK_DATA_LENGTH \
	(2U * WLAN_WPA2_MAC_LENGTH + 2U * WLAN_WPA2_NONCE_LENGTH)
#define WPA2_CAPABILITY_ESS     0x0001U
#define WPA2_CAPABILITY_IBSS    0x0002U
#define WPA2_CAPABILITY_PRIVACY 0x0010U
#define WPA2_MANAGEMENT_HEADER_LENGTH 24U
#define WPA2_FC_ASSOC_RESPONSE 0x0010U
#define WPA2_FC_AUTH_RESPONSE  0x00b0U
#define WPA2_FC_RETRY          0x0800U

static const uint8_t ptk_label[] = "Pairwise key expansion";

static int bytes_zero(const uint8_t *bytes, size_t length);
static int address_valid(const uint8_t address[WLAN_WPA2_MAC_LENGTH]);
static int address_equal(const uint8_t left[WLAN_WPA2_MAC_LENGTH], const uint8_t right[WLAN_WPA2_MAC_LENGTH]);
static int expected_management_response(const struct wlan_wpa2_engine *engine, const uint8_t *frame, size_t length, uint16_t expected_frame_control);
static void retire_implicitly_completed_tx(struct wlan_wpa2_engine *engine);
static uint64_t bounded_deadline(const struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static int active_time_valid(const struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static uint64_t next_cookie(struct wlan_wpa2_engine *engine);
static int next_key_generation(struct wlan_wpa2_engine *engine, uint64_t *result);
static void erase_secrets(struct wlan_wpa2_engine *engine);
static void erase_handshake_secrets(struct wlan_wpa2_engine *engine);
static int cleanup(struct wlan_wpa2_engine *engine);
static int fail(struct wlan_wpa2_engine *engine, int error);
static int ops_valid(const struct wlan_wpa2_ops *ops);
static int profile_valid(const struct wlan_wpa2_profile *profile, uint64_t now_ticks);
static int submit_current(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state pending_state, uint64_t now_ticks);
static int cache_and_submit(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state pending_state, enum wlan_wpa2_tx_kind kind, const uint8_t destination[WLAN_WPA2_MAC_LENGTH], size_t length, uint64_t now_ticks);
static int retry_current(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state pending_state, uint64_t now_ticks);
static int build_authentication(struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static int build_association(struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static int eapol_mic_calculate(const uint8_t kck[WLAN_WPA2_KCK_LENGTH], const uint8_t *frame, size_t length, uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH]);
static int eapol_mic_valid(const struct wlan_wpa2_engine *engine, const uint8_t *frame, size_t length, const uint8_t expected[WLAN_WPA2_KEY_MIC_LENGTH]);
static void ordered_copy(uint8_t *output, const uint8_t *left, const uint8_t *right, size_t length);
static int derive_ptk(struct wlan_wpa2_engine *engine);
static int build_message_2(struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static int build_message_4(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state pending_state, uint64_t now_ticks);
static int build_group_message_2(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state pending_state, uint64_t now_ticks);
static uint64_t key_rsc_packet_number(const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH]);
static int program_pending_pairwise_keys(struct wlan_wpa2_engine *engine);
static int program_pending_group_key(struct wlan_wpa2_engine *engine);
static int install_keys(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_gtk *gtk, const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH]);
static int message_1_first(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks);
static int message_1_retransmit(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks);
static int message_3_digest(const uint8_t *frame, size_t length, uint8_t digest[WLAN_SHA1_DIGEST_SIZE]);
static uint64_t recovery_deadline(const struct wlan_wpa2_engine *engine, uint64_t now_ticks);
static int pairwise_rekey_begin(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks);
static int group_message_1(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, const uint8_t *frame, size_t length, uint64_t now_ticks);
static int message_3_retransmit(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, const uint8_t *frame, size_t length, uint64_t now_ticks);
static int message_3_first(struct wlan_wpa2_engine *engine, const struct wlan_wpa2_eapol_key *key, const uint8_t *frame, size_t length, uint64_t now_ticks);
static enum wlan_wpa2_state pending_retry_state(enum wlan_wpa2_state state);
static int group_rekey_commit(struct wlan_wpa2_engine *engine);
static int pairwise_rekey_commit(struct wlan_wpa2_engine *engine);
static int activation_retry(struct wlan_wpa2_engine *engine, enum wlan_wpa2_state state, uint64_t now_ticks);
static int pairwise_rekey_authorize(struct wlan_wpa2_engine *engine);

/*
 * Initializes an idle engine bound to a set of callbacks.
 */
int
wlan_wpa2_engine_init(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_ops *ops,
	void *callback_context)
{
	/* Rejects a missing engine or an incomplete callback set. */
	if (engine == NULL || !ops_valid(ops))
		return EINVAL;

	/* Starts idle with no secrets. */
	memset(engine, 0, sizeof(*engine));
	engine->ops = ops;
	engine->callback_context = callback_context;
	engine->state = WLAN_WPA2_STATE_IDLE;

	/* Reports the initialized engine. */
	return 0;
}

/*
 * Starts a connection attempt for a profile.
 *
 * The PMK is derived from the passphrase, which is not retained, the
 * radio is started, and the authentication request is transmitted.
 */
int
wlan_wpa2_engine_start(
	struct wlan_wpa2_engine *engine,
	uint64_t generation,
	const struct wlan_wpa2_profile *profile,
	uint64_t now_ticks)
{
	int error;

	/* Rejects a missing engine, a zero generation, or an invalid profile. */
	if (engine == NULL ||
	    !ops_valid(engine->ops) ||
	    generation == 0U ||
	    !profile_valid(profile, now_ticks))
		return EINVAL;

	/* Only an idle or failed engine can start; a failed one is cleaned first. */
	if (engine->state != WLAN_WPA2_STATE_IDLE &&
	    engine->state != WLAN_WPA2_STATE_FAILED)
		return EBUSY;
	if (engine->state == WLAN_WPA2_STATE_FAILED) {
		error = cleanup(engine);
		if (error != 0) {
			engine->last_error = error;
			return error;
		}
	}

	/* Copies the profile without the passphrase and resets the counters. */
	engine->profile = *profile;
	engine->profile.passphrase = NULL;
	engine->profile.passphrase_length = 0U;
	engine->generation = generation;
	engine->key_generation = generation;
	engine->group_key_generation = 0U;
	engine->next_key_generation = generation;
	engine->pending_group_key_generation = 0U;
	engine->next_sequence = profile->initial_sequence;
	engine->last_error = 0;
	engine->connected_lifetime = 0U;
	engine->pairwise_rekey = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	engine->state = WLAN_WPA2_STATE_IDLE;

	/* Derives the PMK. */
	error = wlan_pbkdf2_hmac_sha1(profile->passphrase,
	    profile->passphrase_length, profile->ssid, profile->ssid_length,
	    4096U, engine->pmk, sizeof(engine->pmk));
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/*
	 * radio_start() may have configured hardware even when it reports a
	 * failure.  Preserve that uncertainty until radio_stop() confirms the
	 * generation has been retired.
	 */
	engine->configured = 1U;
	error = engine->ops->radio_start(engine->callback_context, generation,
	    engine->profile.bssid, engine->profile.channel,
	    engine->profile.total_deadline_ticks, &now_ticks);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}
	if (!active_time_valid(engine, now_ticks)) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* Sends the authentication request. */
	error = build_authentication(engine, now_ticks);
	return error;
}

/*
 * Feeds a received management frame to the engine.
 *
 * Only the authentication or association response for the current
 * transaction advances the state; every other frame reports ESTALE.
 */
int
wlan_wpa2_engine_receive_management(
	struct wlan_wpa2_engine *engine,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	uint64_t now_ticks)
{
	struct wlan_wpa2_assoc_response response;
	uint16_t status;
	int error;

	/* Rejects a missing engine or frame, or a stale generation. */
	if (engine == NULL || frame == NULL)
		return EINVAL;
	if (generation == 0U || generation != engine->generation)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks)) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* An authentication response leads to the association request. */
	if (engine->state == WLAN_WPA2_STATE_AUTH_TX ||
	    engine->state == WLAN_WPA2_STATE_AUTH_RESPONSE) {
		/*
		 * Beacons, action frames, frames for another station, and a
		 * delayed response from an older exchange are normal traffic.
		 * Only a response for this exact transaction may fail the
		 * connection on malformed body.
		 */
		if (!expected_management_response(engine, frame, length,
		    WPA2_FC_AUTH_RESPONSE))
			return ESTALE;
		error = wlan_wpa2_auth_response_parse(frame, length,
		    engine->profile.station, engine->profile.bssid, &status);
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		if (status != 0U) {
			error = fail(engine, ECONNREFUSED);
			return error;
		}
		retire_implicitly_completed_tx(engine);
		error = build_association(engine, now_ticks);
		return error;
	}

	/* An association response records the AID and awaits message 1. */
	if (engine->state == WLAN_WPA2_STATE_ASSOC_TX ||
	    engine->state == WLAN_WPA2_STATE_ASSOC_RESPONSE) {
		if (!expected_management_response(engine, frame, length,
		    WPA2_FC_ASSOC_RESPONSE))
			return ESTALE;
		error = wlan_wpa2_assoc_response_parse(frame, length,
		    engine->profile.station, engine->profile.bssid, &response);
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		if (response.status != 0U) {
			error = fail(engine, ECONNREFUSED);
			return error;
		}
		retire_implicitly_completed_tx(engine);

		/*
		 * As with key programming, failure does not prove that the
		 * hardware rejected the request before changing state.
		 */
		engine->associated = 1U;
		engine->aid = response.aid;
		error = engine->ops->association_set(engine->callback_context,
		    engine->generation, engine->profile.bssid, response.aid);
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->state = WLAN_WPA2_STATE_MESSAGE_1;
		engine->retry_count = 0U;
		engine->step_deadline_ticks = bounded_deadline(engine, now_ticks);
		return 0;
	}

	/* Any other state has no management response outstanding. */
	return ESTALE;
}

/*
 * Feeds a received EAPOL-Key frame to the engine.
 *
 * The frame is dispatched by message type and state to the handshake
 * or rekey handlers; an unexpected message fails the connection.
 */
int
wlan_wpa2_engine_receive_eapol(
	struct wlan_wpa2_engine *engine,
	uint64_t generation,
	const uint8_t source[WLAN_WPA2_MAC_LENGTH],
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	const uint8_t *frame,
	size_t length,
	uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	int error;

	/* Rejects a missing operand, a stale generation, or an expired attempt. */
	if (engine == NULL ||
	    source == NULL ||
	    destination == NULL ||
	    frame == NULL)
		return EINVAL;
	if (generation == 0U || generation != engine->generation)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks)) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* The frame must come from the access point to this station. */
	if (!address_equal(source, engine->profile.bssid) ||
	    !address_equal(destination, engine->profile.station)) {
		error = fail(engine, EACCES);
		return error;
	}
	error = wlan_wpa2_eapol_key_parse(frame, length, &key);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Message 1 starts the handshake, repeats it, or begins a pairwise rekey. */
	if (key.message == WLAN_WPA2_EAPOL_MESSAGE_1) {
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_1) {
			error = message_1_first(engine, &key, now_ticks);
			return error;
		}
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_3) {
			error = message_1_retransmit(engine, &key, now_ticks);
			return error;
		}
		if (engine->state == WLAN_WPA2_STATE_AUTHORIZED) {
			error = pairwise_rekey_begin(engine, &key, now_ticks);
			return error;
		}
		return EBUSY;
	}

	/* Message 3 installs the keys or repeats. */
	if (key.message == WLAN_WPA2_EAPOL_MESSAGE_3) {
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_3) {
			error = message_3_first(engine, &key, frame, length,
			    now_ticks);
			return error;
		}
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_4_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX ||
		    engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE ||
		    engine->state == WLAN_WPA2_STATE_AUTHORIZED) {
			error = message_3_retransmit(engine, &key, frame, length,
			    now_ticks);
			return error;
		}
		return EBUSY;
	}

	/* Group message 1 rekeys the group key of an authorized station. */
	if (key.message == WLAN_WPA2_EAPOL_GROUP_MESSAGE_1 &&
	    (engine->state == WLAN_WPA2_STATE_AUTHORIZED ||
	    engine->state == WLAN_WPA2_STATE_GROUP_STAGE ||
	    engine->state == WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX ||
	    engine->state ==
	    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX)) {
		error = group_message_1(engine, &key, frame, length, now_ticks);
		return error;
	}

	/* Anything else is a protocol violation. */
	error = fail(engine, EACCES);
	return error;
}

/*
 * Reports the outcome of a transmission the engine requested.
 *
 * An acknowledged frame advances the state; a lost one retries, except
 * that management frames wait for the step deadline instead.
 */
int
wlan_wpa2_engine_report_tx(
	struct wlan_wpa2_engine *engine,
	uint64_t generation,
	uint64_t cookie,
	int acknowledged,
	int error,
	uint64_t now_ticks)
{
	enum wlan_wpa2_state pending;
	int callback_error;
	int result;

	/* Rejects a missing engine or an inconsistent report. */
	if (engine == NULL ||
	    (acknowledged != 0 && acknowledged != 1) ||
	    error < 0 ||
	    (acknowledged && error != 0))
		return EINVAL;

	/* Only the report for the active transmission of this generation counts. */
	if (generation == 0U ||
	    generation != engine->generation ||
	    cookie == 0U ||
	    cookie != engine->tx_cookie_active)
		return ESTALE;
	pending = pending_retry_state(engine->state);
	if (pending == WLAN_WPA2_STATE_IDLE || pending != engine->state)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks)) {
		result = fail(engine, ETIMEDOUT);
		return result;
	}

	/* A lost frame is retried. */
	if (!acknowledged || error != 0) {
		/*
		 * The q070 physical target reported six unacknowledged
		 * authentication frames within tens of milliseconds, so an
		 * immediate resubmission converts the bounded retry budget
		 * into one sub-second burst.  Management attempts retire this
		 * report's cookie and let the existing step deadline drive the
		 * next spaced retransmission; EAPOL keeps its immediate
		 * policy because the authenticator paces that exchange.
		 */
		if (pending == WLAN_WPA2_STATE_AUTH_TX ||
		    pending == WLAN_WPA2_STATE_ASSOC_TX) {
			engine->tx_cookie_active = 0U;
			return 0;
		}
		result = retry_current(engine, pending, now_ticks);
		return result;
	}

	/* An acknowledged frame moves to the next state. */
	engine->tx_cookie_active = 0U;
	engine->step_deadline_ticks = bounded_deadline(engine, now_ticks);
	switch (engine->state) {
	case WLAN_WPA2_STATE_AUTH_TX:
		engine->state = WLAN_WPA2_STATE_AUTH_RESPONSE;
		return 0;
	case WLAN_WPA2_STATE_ASSOC_TX:
		engine->state = WLAN_WPA2_STATE_ASSOC_RESPONSE;
		return 0;
	case WLAN_WPA2_STATE_MESSAGE_2_TX:
		engine->state = WLAN_WPA2_STATE_MESSAGE_3;
		return 0;
	case WLAN_WPA2_STATE_MESSAGE_4_TX:
	case WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX:
		/* A rekey switches to the staged keys before authorizing. */
		if (engine->pairwise_rekey) {
			callback_error = pairwise_rekey_commit(engine);
			if (callback_error == EBUSY) {
				result = activation_retry(engine,
				    WLAN_WPA2_STATE_PAIRWISE_ACTIVATE,
				    now_ticks);
				return result;
			}
			if (callback_error != 0)
				return callback_error;
		}

		/*
		 * A post-commit duplicate M3 has no staged replacement and the
		 * controlled port is already open.  Otherwise every acknowledged
		 * M4, including a retransmission, crosses the same authorization
		 * barrier.
		 */
		if (engine->authorized) {
			engine->state = WLAN_WPA2_STATE_AUTHORIZED;
			engine->step_deadline_ticks = 0U;
			return 0;
		}
		result = pairwise_rekey_authorize(engine);
		return result;
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX:
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX:
		/* A group rekey with nothing staged was a plain retransmission. */
		if (!engine->pending_group_installed) {
			engine->state = WLAN_WPA2_STATE_AUTHORIZED;
			engine->step_deadline_ticks = 0U;
			return 0;
		}
		callback_error = group_rekey_commit(engine);
		if (callback_error == EBUSY) {
			result = activation_retry(engine,
			    WLAN_WPA2_STATE_GROUP_ACTIVATE, now_ticks);
			return result;
		}
		return callback_error;
	default:
		return ESTALE;
	}
}

/*
 * Runs the engine's timer.
 *
 * An expired step deadline retries the pending transmission, or
 * resumes staged key programming and activation that reported EBUSY.
 */
int
wlan_wpa2_engine_timer(
	struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	enum wlan_wpa2_state pending;
	int error;

	/* Rejects a missing engine; a settled state needs no timer. */
	if (engine == NULL)
		return EINVAL;
	if (engine->state == WLAN_WPA2_STATE_IDLE ||
	    engine->state == WLAN_WPA2_STATE_FAILED ||
	    engine->state == WLAN_WPA2_STATE_AUTHORIZED)
		return 0;
	if (!active_time_valid(engine, now_ticks)) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}
	if (now_ticks < engine->step_deadline_ticks)
		return 0;

	/* Resumes a staged pairwise rekey. */
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE) {
		error = program_pending_pairwise_keys(engine);
		if (error == EBUSY) {
			error = activation_retry(engine,
			    WLAN_WPA2_STATE_PAIRWISE_STAGE, now_ticks);
			return error;
		}
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		error = build_message_4(engine, WLAN_WPA2_STATE_MESSAGE_4_TX,
		    now_ticks);
		return error;
	}

	/* Resumes a staged group rekey. */
	if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE) {
		error = program_pending_group_key(engine);
		if (error == EBUSY) {
			error = activation_retry(engine, WLAN_WPA2_STATE_GROUP_STAGE,
			    now_ticks);
			return error;
		}
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		error = build_group_message_2(engine,
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX, now_ticks);
		return error;
	}

	/* Resumes a key activation that was busy. */
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_ACTIVATE) {
		error = pairwise_rekey_commit(engine);
		if (error == EBUSY) {
			error = activation_retry(engine,
			    WLAN_WPA2_STATE_PAIRWISE_ACTIVATE, now_ticks);
			return error;
		}
		if (error != 0)
			return error;
		error = pairwise_rekey_authorize(engine);
		return error;
	}
	if (engine->state == WLAN_WPA2_STATE_GROUP_ACTIVATE) {
		error = group_rekey_commit(engine);
		if (error == EBUSY) {
			error = activation_retry(engine,
			    WLAN_WPA2_STATE_GROUP_ACTIVATE, now_ticks);
			return error;
		}
		return error;
	}

	/* Otherwise the pending transmission is retried. */
	pending = pending_retry_state(engine->state);
	if (pending == WLAN_WPA2_STATE_IDLE) {
		error = fail(engine, EINVAL);
		return error;
	}
	error = retry_current(engine, pending, now_ticks);
	return error;
}

/*
 * Stops the engine, releasing every hardware state and secret.
 */
int
wlan_wpa2_engine_stop(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/* Rejects a missing engine or callback set. */
	if (engine == NULL || !ops_valid(engine->ops))
		return EINVAL;

	/* A failed cleanup leaves the engine failed for a later retry. */
	error = cleanup(engine);
	if (error != 0) {
		engine->state = WLAN_WPA2_STATE_FAILED;
		engine->last_error = error;
		return error;
	}

	/* Returns to idle with the profile forgotten. */
	engine->state = WLAN_WPA2_STATE_IDLE;
	engine->last_error = error;
	engine->generation = 0U;
	engine->key_generation = 0U;
	memset(&engine->profile, 0, sizeof(engine->profile));
	return error;
}

/*
 * Reports the engine state.
 */
enum wlan_wpa2_state
wlan_wpa2_engine_state(
	const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL)
		return WLAN_WPA2_STATE_FAILED;
	return engine->state;
}

/*
 * Reports the error that failed the engine.
 */
int
wlan_wpa2_engine_last_error(
	const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL)
		return EINVAL;
	return engine->last_error;
}

/*
 * Reports when the timer must next run, or zero when it need not.
 */
uint64_t
wlan_wpa2_engine_next_deadline(
	const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL ||
	    engine->state == WLAN_WPA2_STATE_IDLE ||
	    engine->state == WLAN_WPA2_STATE_FAILED ||
	    engine->state == WLAN_WPA2_STATE_AUTHORIZED)
		return 0U;
	return engine->step_deadline_ticks;
}

#ifdef WLAN_WPA2_TESTING
/*
 * Tests that every secret and cached frame has been erased.
 */
int
wlan_wpa2_engine_test_secrets_clear(
	const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL)
		return 0;
	if (!bytes_zero(engine->pmk, sizeof(engine->pmk)))
		return 0;
	if (!bytes_zero(engine->ptk, sizeof(engine->ptk)))
		return 0;
	if (!bytes_zero(engine->anonce, sizeof(engine->anonce)))
		return 0;
	if (!bytes_zero(engine->snonce, sizeof(engine->snonce)))
		return 0;
	if (!bytes_zero(engine->gtk, sizeof(engine->gtk)))
		return 0;
	if (!bytes_zero(engine->pending_gtk, sizeof(engine->pending_gtk)))
		return 0;
	if (!bytes_zero(engine->message_3_digest,
	    sizeof(engine->message_3_digest)))
		return 0;
	if (!bytes_zero(engine->group_message_digest,
	    sizeof(engine->group_message_digest)))
		return 0;
	if (!bytes_zero(engine->tx_frame, sizeof(engine->tx_frame)))
		return 0;
	if (engine->group_receive_packet_number != 0U)
		return 0;
	return 1;
}
#endif

/* Tests whether a buffer is all zero, without an early exit. */
static int
bytes_zero(
	const uint8_t *bytes,
	size_t length)
{
	uint8_t combined;
	size_t index;

	/* Ors every byte together in constant time. */
	combined = 0U;
	for (index = 0U; index < length; index++)
		combined |= bytes[index];

	/* Reports whether the whole buffer was zero. */
	if (combined != 0U)
		return 0;
	return 1;
}

/* Tests that an address is a present, unicast, non-zero MAC address. */
static int
address_valid(
	const uint8_t address[WLAN_WPA2_MAC_LENGTH])
{
	if (address == NULL)
		return 0;
	if ((address[0] & 1U) != 0U)
		return 0;
	if (bytes_zero(address, WLAN_WPA2_MAC_LENGTH))
		return 0;
	return 1;
}

/* Compares two MAC addresses in constant time. */
static int
address_equal(
	const uint8_t left[WLAN_WPA2_MAC_LENGTH],
	const uint8_t right[WLAN_WPA2_MAC_LENGTH])
{
	int equal;

	equal = wlan_crypto_equal(left, right, WLAN_WPA2_MAC_LENGTH);
	return equal;
}

/* Tests whether a management frame is the AP's response to this station. */
static int
expected_management_response(
	const struct wlan_wpa2_engine *engine,
	const uint8_t *frame,
	size_t length,
	uint16_t expected_frame_control)
{
	uint16_t frame_control;

	/* The frame control, ignoring the retry bit, must match. */
	if (length < WPA2_MANAGEMENT_HEADER_LENGTH)
		return 0;
	frame_control = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
	if ((frame_control & (uint16_t)~WPA2_FC_RETRY) !=
	    expected_frame_control)
		return 0;

	/* The addresses must be this station and the BSS. */
	if (!address_equal(frame + 4U, engine->profile.station))
		return 0;
	if (!address_equal(frame + 10U, engine->profile.bssid))
		return 0;
	if (!address_equal(frame + 16U, engine->profile.bssid))
		return 0;
	return 1;
}

/* Retires the active transmission once the peer's response proves it arrived. */
static void
retire_implicitly_completed_tx(
	struct wlan_wpa2_engine *engine)
{
	/*
	 * A peer response for the exact transaction is stronger evidence than
	 * a delayed firmware TX report.  Its cookie must no longer be able to
	 * advance a later state; a report which arrives afterwards is
	 * deliberately stale.
	 */
	engine->tx_cookie_active = 0U;
}

/* Computes a step deadline bounded by the connect budget before authorization. */
static uint64_t
bounded_deadline(
	const struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	uint64_t deadline;

	/* Saturates instead of wrapping. */
	if (UINT64_MAX - now_ticks < engine->profile.transition_timeout_ticks)
		deadline = UINT64_MAX;
	else
		deadline = now_ticks + engine->profile.transition_timeout_ticks;

	/*
	 * The 30-second connect budget ends at authorization.  An exact M3
	 * retransmission afterwards still needs one bounded M4 response.
	 */
	if (!engine->authorized &&
	    deadline > engine->profile.total_deadline_ticks)
		deadline = engine->profile.total_deadline_ticks;
	return deadline;
}

/* Tests whether the connect budget still allows activity. */
static int
active_time_valid(
	const struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	if (engine->authorized)
		return 1;
	if (now_ticks < engine->profile.total_deadline_ticks)
		return 1;
	return 0;
}

/* Allocates the next non-zero transmission cookie. */
static uint64_t
next_cookie(
	struct wlan_wpa2_engine *engine)
{
	engine->tx_cookie_next++;
	if (engine->tx_cookie_next == 0U)
		engine->tx_cookie_next++;
	return engine->tx_cookie_next;
}

/* Allocates the next key generation, refusing to wrap. */
static int
next_key_generation(
	struct wlan_wpa2_engine *engine,
	uint64_t *result)
{
	if (engine->next_key_generation == UINT64_MAX)
		return EOVERFLOW;
	engine->next_key_generation++;
	if (engine->next_key_generation == 0U)
		return EOVERFLOW;
	*result = engine->next_key_generation;
	return 0;
}

/* Erases every secret and resets the handshake and group counters. */
static void
erase_secrets(
	struct wlan_wpa2_engine *engine)
{
	wlan_crypto_erase(engine->pmk, sizeof(engine->pmk));
	wlan_crypto_erase(engine->ptk, sizeof(engine->ptk));
	wlan_crypto_erase(engine->anonce, sizeof(engine->anonce));
	wlan_crypto_erase(engine->snonce, sizeof(engine->snonce));
	wlan_crypto_erase(engine->gtk, sizeof(engine->gtk));
	wlan_crypto_erase(engine->pending_gtk, sizeof(engine->pending_gtk));
	wlan_crypto_erase(engine->message_3_digest,
	    sizeof(engine->message_3_digest));
	wlan_crypto_erase(engine->group_message_digest,
	    sizeof(engine->group_message_digest));
	wlan_crypto_erase(engine->tx_frame, sizeof(engine->tx_frame));
	engine->tx_length = 0U;
	engine->gtk_index = 0U;
	engine->pending_gtk_index = 0U;
	engine->message_1_replay_counter = 0U;
	engine->message_3_replay_counter = 0U;
	engine->group_replay_counter = 0U;
	engine->group_receive_packet_number = 0U;
	engine->pending_group_receive_packet_number = 0U;
	engine->message_3_accepted = 0U;
	engine->group_message_accepted = 0U;
	engine->protocol_version = 0U;
}

/* Erases the four-way handshake secrets before a pairwise rekey. */
static void
erase_handshake_secrets(
	struct wlan_wpa2_engine *engine)
{
	wlan_crypto_erase(engine->ptk, sizeof(engine->ptk));
	wlan_crypto_erase(engine->anonce, sizeof(engine->anonce));
	wlan_crypto_erase(engine->snonce, sizeof(engine->snonce));
	wlan_crypto_erase(engine->message_3_digest,
	    sizeof(engine->message_3_digest));
	wlan_crypto_erase(engine->tx_frame, sizeof(engine->tx_frame));
	engine->tx_length = 0U;
	engine->tx_cookie_active = 0U;
	engine->message_1_replay_counter = 0U;
	engine->message_3_replay_counter = 0U;
	engine->message_3_accepted = 0U;
	engine->protocol_version = 0U;
}

/* Drives every hardware state back to idle and erases the secrets. */
static int
cleanup(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/*
	 * Inverse callbacks identify hardware state by generation/slot
	 * metadata; none consumes the key bytes.  Scrub all software secret
	 * material before crossing a checked barrier so link loss cannot
	 * preserve a PMK or nonce merely because hardware cleanup must be
	 * retried.
	 */
	wlan_crypto_erase(engine->pmk, sizeof(engine->pmk));
	wlan_crypto_erase(engine->ptk, sizeof(engine->ptk));
	wlan_crypto_erase(engine->anonce, sizeof(engine->anonce));
	wlan_crypto_erase(engine->snonce, sizeof(engine->snonce));
	wlan_crypto_erase(engine->gtk, sizeof(engine->gtk));
	wlan_crypto_erase(engine->pending_gtk, sizeof(engine->pending_gtk));
	wlan_crypto_erase(engine->message_3_digest,
	    sizeof(engine->message_3_digest));
	wlan_crypto_erase(engine->group_message_digest,
	    sizeof(engine->group_message_digest));
	wlan_crypto_erase(engine->tx_frame, sizeof(engine->tx_frame));
	engine->message_1_replay_counter = 0U;
	engine->message_3_replay_counter = 0U;
	engine->group_replay_counter = 0U;
	engine->group_receive_packet_number = 0U;
	engine->pending_group_receive_packet_number = 0U;
	engine->tx_cookie_active = 0U;
	engine->step_deadline_ticks = 0U;

	/* Closes the port and deletes every installed key, checked in turn. */
	if (engine->authorized) {
		error = engine->ops->authorized_set(engine->callback_context,
		    engine->generation, 0);
		if (error != 0)
			return error;
		engine->authorized = 0U;
	}
	if (engine->pending_group_installed) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP,
		    engine->pending_gtk_index,
		    engine->pending_group_key_generation);
		if (error != 0)
			return error;
		engine->pending_group_installed = 0U;
	}
	if (engine->pending_pairwise_installed) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
		    engine->pending_pairwise_key_generation);
		if (error != 0)
			return error;
		engine->pending_pairwise_installed = 0U;
	}
	if (engine->group_installed) {
		if (!engine->old_group_retired) {
			error = engine->ops->key_delete(engine->callback_context,
			    engine->generation, WLAN_WPA2_KEY_GROUP,
			    engine->gtk_index, engine->group_key_generation);
			if (error != 0)
				return error;
		}
		engine->group_installed = 0U;
	}
	if (engine->pairwise_installed) {
		if (!engine->old_pairwise_retired) {
			error = engine->ops->key_delete(engine->callback_context,
			    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
			    engine->key_generation);
			if (error != 0)
				return error;
		}
		engine->pairwise_installed = 0U;
	}

	/*
	 * No installed hardware key can now reference this material.  Erase
	 * it even when a later association/radio barrier needs a checked
	 * retry.
	 */
	erase_secrets(engine);
	engine->key_generation = 0U;
	engine->group_key_generation = 0U;
	engine->pending_pairwise_key_generation = 0U;
	engine->pending_group_key_generation = 0U;

	/* Clears the association and stops the radio. */
	if (engine->associated) {
		error = engine->ops->association_clear(engine->callback_context,
		    engine->generation);
		if (error != 0)
			return error;
		engine->associated = 0U;
		engine->aid = 0U;
	}
	if (engine->configured) {
		error = engine->ops->radio_stop(engine->callback_context,
		    engine->generation);
		if (error != 0)
			return error;
		engine->configured = 0U;
	}
	engine->aid = 0U;

	/*
	 * retry_count deliberately survives this cleanup: it is the public
	 * failure evidence behind the status retries field, and the next
	 * cache_and_submit() resets it before any new transmission.
	 */
	engine->pairwise_rekey = 0U;
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	engine->connected_lifetime = 0U;
	engine->next_key_generation = 0U;
	engine->key_generation = 0U;
	engine->group_key_generation = 0U;
	engine->pending_group_key_generation = 0U;
	engine->pending_pairwise_key_generation = 0U;
	return 0;
}

/* Fails the engine with an error, cleaning up as far as possible. */
static int
fail(
	struct wlan_wpa2_engine *engine,
	int error)
{
	if (error == 0)
		error = EIO;
	(void)cleanup(engine);
	engine->last_error = error;
	engine->state = WLAN_WPA2_STATE_FAILED;
	return error;
}

/* Tests that a callback set is complete. */
static int
ops_valid(
	const struct wlan_wpa2_ops *ops)
{
	if (ops == NULL)
		return 0;
	if (ops->entropy_fill == NULL)
		return 0;
	if (ops->radio_start == NULL)
		return 0;
	if (ops->transmit == NULL)
		return 0;
	if (ops->association_set == NULL)
		return 0;
	if (ops->association_clear == NULL)
		return 0;
	if (ops->key_install == NULL)
		return 0;
	if (ops->key_receive_pn_advance == NULL)
		return 0;
	if (ops->key_delete == NULL)
		return 0;
	if (ops->keys_activate == NULL)
		return 0;
	if (ops->authorized_set == NULL)
		return 0;
	if (ops->radio_stop == NULL)
		return 0;
	return 1;
}

/* Tests that a profile describes a usable WPA2-PSK network. */
static int
profile_valid(
	const struct wlan_wpa2_profile *profile,
	uint64_t now_ticks)
{
	size_t index;
	uint8_t channel;
	int channel_valid;

	/* The channel must be a 2.4 GHz channel or a valid 5 GHz one. */
	if (profile == NULL)
		channel = 0U;
	else
		channel = profile->channel;
	channel_valid = 0;
	if (channel >= 1U && channel <= 14U)
		channel_valid = 1;
	else if (channel >= 36U && channel <= 144U && (channel - 36U) % 4U == 0U)
		channel_valid = 1;
	else if (channel >= 149U && channel <= 181U && (channel - 149U) % 4U == 0U)
		channel_valid = 1;

	/* Checks the addresses, SSID, rates, capabilities, passphrase, and timeouts. */
	if (profile == NULL ||
	    !address_valid(profile->station) ||
	    !address_valid(profile->bssid) ||
	    address_equal(profile->station, profile->bssid) ||
	    profile->ssid_length == 0U ||
	    profile->ssid_length > WLAN_WPA2_SSID_MAX ||
	    profile->rate_count == 0U ||
	    profile->rate_count > WLAN_WPA2_RATE_MAX ||
	    !channel_valid ||
	    (profile->capability & (WPA2_CAPABILITY_ESS |
	    WPA2_CAPABILITY_PRIVACY)) != (WPA2_CAPABILITY_ESS |
	    WPA2_CAPABILITY_PRIVACY) ||
	    (profile->capability & WPA2_CAPABILITY_IBSS) != 0U ||
	    profile->listen_interval == 0U ||
	    profile->initial_sequence > 0x0fffU ||
	    profile->passphrase == NULL ||
	    profile->passphrase_length < WLAN_WPA2_PASSPHRASE_MIN ||
	    profile->passphrase_length > WLAN_WPA2_PASSPHRASE_MAX ||
	    profile->transition_timeout_ticks == 0U ||
	    profile->recovery_timeout_ticks == 0U ||
	    profile->total_deadline_ticks <= now_ticks)
		return 0;

	/* Every rate must be non-zero. */
	for (index = 0U; index < profile->rate_count; index++) {
		if ((profile->rates[index] & 0x7fU) == 0U)
			return 0;
	}
	return 1;
}

/* Transmits the cached frame for a pending state with a new cookie. */
static int
submit_current(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state,
	uint64_t now_ticks)
{
	uint64_t cookie;
	int error;

	/* The connect budget must still allow it. */
	if (!active_time_valid(engine, now_ticks)) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* Enters the pending state and hands the frame to the driver. */
	engine->state = pending_state;
	engine->step_deadline_ticks = bounded_deadline(engine, now_ticks);
	cookie = next_cookie(engine);
	engine->tx_cookie_active = cookie;
	error = engine->ops->transmit(engine->callback_context,
	    engine->generation, cookie, engine->tx_kind,
	    engine->tx_destination, engine->tx_frame, engine->tx_length,
	    engine->step_deadline_ticks);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}
	return 0;
}

/* Records a built frame as the current transmission and submits it. */
static int
cache_and_submit(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state,
	enum wlan_wpa2_tx_kind kind,
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	size_t length,
	uint64_t now_ticks)
{
	int error;

	/* The frame must fit the cache. */
	if (length == 0U || length > sizeof(engine->tx_frame)) {
		error = fail(engine, EMSGSIZE);
		return error;
	}

	/* A new frame starts a fresh retry budget. */
	engine->tx_kind = kind;
	memcpy(engine->tx_destination, destination,
	    sizeof(engine->tx_destination));
	engine->tx_length = length;
	engine->retry_count = 0U;
	error = submit_current(engine, pending_state, now_ticks);
	return error;
}

/* Retransmits the current frame, failing once the retry budget is spent. */
static int
retry_current(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state,
	uint64_t now_ticks)
{
	int error;

	/* Gives up once the exchange has been retried often enough. */
	if (engine->retry_count >= WLAN_WPA2_RETRY_MAX) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* Sends the cached frame again. */
	engine->retry_count++;
	error = submit_current(engine, pending_state, now_ticks);

	/* Reports the retry result. */
	return error;
}

/* Builds and sends the open-system authentication request. */
static int
build_authentication(
	struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	size_t length;
	int error;

	/* Encodes the authentication request. */
	error = wlan_wpa2_auth_request_build(engine->tx_frame,
	    sizeof(engine->tx_frame), engine->profile.station,
	    engine->profile.bssid, engine->next_sequence, &length);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Sends it, keeping a copy for the retries. */
	engine->next_sequence = (uint16_t)((engine->next_sequence + 1U) &
	    0x0fffU);
	error = cache_and_submit(engine, WLAN_WPA2_STATE_AUTH_TX,
	    WLAN_WPA2_TX_MANAGEMENT, engine->profile.bssid, length, now_ticks);

	/* Reports the transmit result. */
	return error;
}

/* Builds and sends the association request. */
static int
build_association(
	struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	size_t length;
	int error;

	/* Encodes the association request with this station's profile. */
	error = wlan_wpa2_assoc_request_build(engine->tx_frame,
	    sizeof(engine->tx_frame), engine->profile.station,
	    engine->profile.bssid, engine->next_sequence,
	    engine->profile.capability, engine->profile.listen_interval,
	    engine->profile.ssid, engine->profile.ssid_length,
	    engine->profile.rates, engine->profile.rate_count, &length);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Sends it, keeping a copy for the retries. */
	engine->next_sequence = (uint16_t)((engine->next_sequence + 1U) &
	    0x0fffU);
	error = cache_and_submit(engine, WLAN_WPA2_STATE_ASSOC_TX,
	    WLAN_WPA2_TX_MANAGEMENT, engine->profile.bssid, length, now_ticks);

	/* Reports the transmit result. */
	return error;
}

/* Computes the MIC of an EAPOL-Key frame with its MIC field zeroed. */
static int
eapol_mic_calculate(
	const uint8_t kck[WLAN_WPA2_KCK_LENGTH],
	const uint8_t *frame,
	size_t length,
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH])
{
	uint8_t copy[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	int error;

	/* The frame must hold a MIC field and fit the working copy. */
	if (frame == NULL ||
	    mic == NULL ||
	    length < WPA2_EAPOL_MIC_OFFSET + WPA2_EAPOL_MIC_LENGTH ||
	    length > sizeof(copy))
		return EINVAL;

	/* Hashes the copy with a zeroed MIC and keeps the leading bytes. */
	memcpy(copy, frame, length);
	memset(copy + WPA2_EAPOL_MIC_OFFSET, 0, WPA2_EAPOL_MIC_LENGTH);
	error = wlan_hmac_sha1(kck, WLAN_WPA2_KCK_LENGTH, copy, length,
	    digest);
	if (error == 0)
		memcpy(mic, digest, WLAN_WPA2_KEY_MIC_LENGTH);
	wlan_crypto_erase(copy, sizeof(copy));
	wlan_crypto_erase(digest, sizeof(digest));
	return error;
}

/* Checks the MIC of a received EAPOL-Key frame against the PTK. */
static int
eapol_mic_valid(
	const struct wlan_wpa2_engine *engine,
	const uint8_t *frame,
	size_t length,
	const uint8_t expected[WLAN_WPA2_KEY_MIC_LENGTH])
{
	uint8_t calculated[WLAN_WPA2_KEY_MIC_LENGTH];
	int error;
	int equal;

	/* Recomputes the MIC over the frame. */
	error = eapol_mic_calculate(engine->ptk, frame, length, calculated);
	if (error != 0)
		return error;

	/* Compares it in constant time and erases the copy. */
	equal = wlan_crypto_equal(calculated, expected, sizeof(calculated));
	wlan_crypto_erase(calculated, sizeof(calculated));

	/* Reports whether the frame carried the expected MIC. */
	if (!equal)
		return EACCES;
	return 0;
}

/* Concatenates two buffers in ascending order, as the PTK derivation requires. */
static void
ordered_copy(
	uint8_t *output,
	const uint8_t *left,
	const uint8_t *right,
	size_t length)
{
	if (memcmp(left, right, length) < 0) {
		memcpy(output, left, length);
		memcpy(output + length, right, length);
	} else {
		memcpy(output, right, length);
		memcpy(output + length, left, length);
	}
}

/* Derives the PTK from the PMK, the addresses, and the nonces. */
static int
derive_ptk(
	struct wlan_wpa2_engine *engine)
{
	uint8_t data[WPA2_PTK_DATA_LENGTH];
	int error;

	/* Builds the PRF input from the ordered addresses and nonces. */
	ordered_copy(data, engine->profile.bssid, engine->profile.station,
	    WLAN_WPA2_MAC_LENGTH);
	ordered_copy(data + 2U * WLAN_WPA2_MAC_LENGTH, engine->anonce,
	    engine->snonce, WLAN_WPA2_NONCE_LENGTH);

	/* Derives the pairwise key and erases the input. */
	error = wlan_crypto_prf_sha1(engine->pmk, sizeof(engine->pmk),
	    ptk_label, sizeof(ptk_label) - 1U, data, sizeof(data), engine->ptk,
	    sizeof(engine->ptk));
	wlan_crypto_erase(data, sizeof(data));

	/* Reports the derivation result. */
	return error;
}

/* Builds and sends handshake message 2 with the SNonce and RSN element. */
static int
build_message_2(
	struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t rsn[WLAN_WPA2_RSN_IE_LENGTH];
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t rsn_length;
	size_t length;
	int error;

	/* Builds the frame and fills in its MIC. */
	memset(&key, 0, sizeof(key));
	error = wlan_wpa2_rsn_build_ccmp_psk(rsn, sizeof(rsn), &rsn_length);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}
	key.message = WLAN_WPA2_EAPOL_MESSAGE_2;
	key.protocol_version = engine->protocol_version;
	key.replay_counter = engine->message_1_replay_counter;
	memcpy(key.nonce, engine->snonce, sizeof(key.nonce));
	key.key_data = rsn;
	key.key_data_length = rsn_length;
	error = wlan_wpa2_eapol_key_build(engine->tx_frame,
	    sizeof(engine->tx_frame), &key, &length);
	if (error == 0)
		error = eapol_mic_calculate(engine->ptk, engine->tx_frame,
		    length, mic);
	if (error == 0)
		memcpy(engine->tx_frame + WPA2_EAPOL_MIC_OFFSET, mic,
		    sizeof(mic));
	wlan_crypto_erase(&key, sizeof(key));
	wlan_crypto_erase(rsn, sizeof(rsn));
	wlan_crypto_erase(mic, sizeof(mic));
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Sends it. */
	error = cache_and_submit(engine, WLAN_WPA2_STATE_MESSAGE_2_TX,
	    WLAN_WPA2_TX_EAPOL, engine->profile.bssid, length, now_ticks);
	return error;
}

/* Builds and sends handshake message 4. */
static int
build_message_4(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state,
	uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t length;
	int error;

	/* Builds the frame and fills in its MIC. */
	memset(&key, 0, sizeof(key));
	key.message = WLAN_WPA2_EAPOL_MESSAGE_4;
	key.protocol_version = engine->protocol_version;
	key.replay_counter = engine->message_3_replay_counter;
	error = wlan_wpa2_eapol_key_build(engine->tx_frame,
	    sizeof(engine->tx_frame), &key, &length);
	if (error == 0)
		error = eapol_mic_calculate(engine->ptk, engine->tx_frame,
		    length, mic);
	if (error == 0)
		memcpy(engine->tx_frame + WPA2_EAPOL_MIC_OFFSET, mic,
		    sizeof(mic));
	wlan_crypto_erase(&key, sizeof(key));
	wlan_crypto_erase(mic, sizeof(mic));
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Sends it. */
	error = cache_and_submit(engine, pending_state, WLAN_WPA2_TX_EAPOL,
	    engine->profile.bssid, length, now_ticks);
	return error;
}

/* Builds and sends group handshake message 2. */
static int
build_group_message_2(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state,
	uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t length;
	int error;

	/* Builds the frame and fills in its MIC. */
	memset(&key, 0, sizeof(key));
	key.message = WLAN_WPA2_EAPOL_GROUP_MESSAGE_2;
	key.protocol_version = engine->protocol_version;
	key.replay_counter = engine->group_replay_counter;
	error = wlan_wpa2_eapol_key_build(engine->tx_frame,
	    sizeof(engine->tx_frame), &key, &length);
	if (error == 0)
		error = eapol_mic_calculate(engine->ptk, engine->tx_frame,
		    length, mic);
	if (error == 0)
		memcpy(engine->tx_frame + WPA2_EAPOL_MIC_OFFSET, mic,
		    sizeof(mic));
	wlan_crypto_erase(&key, sizeof(key));
	wlan_crypto_erase(mic, sizeof(mic));
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Sends it. */
	error = cache_and_submit(engine, pending_state, WLAN_WPA2_TX_EAPOL,
	    engine->profile.bssid, length, now_ticks);
	return error;
}

/* Decodes the 48-bit receive packet number of a key RSC field. */
static uint64_t
key_rsc_packet_number(
	const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH])
{
	return (uint64_t)rsc[0] | ((uint64_t)rsc[1] << 8) |
	    ((uint64_t)rsc[2] << 16) | ((uint64_t)rsc[3] << 24) |
	    ((uint64_t)rsc[4] << 32) | ((uint64_t)rsc[5] << 40);
}

/* Programs the staged pairwise and group keys of a rekey, resuming after EBUSY. */
static int
program_pending_pairwise_keys(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/* Installs the pairwise key once. */
	if (!engine->pending_pairwise_programmed) {
		error = engine->ops->key_install(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
		    engine->ptk + WLAN_WPA2_KCK_LENGTH + WLAN_WPA2_KEK_LENGTH,
		    engine->pending_pairwise_key_generation, 0U);
		if (error != 0)
			return error;
		engine->pending_pairwise_programmed = 1U;
	}

	/* Installs the group key once. */
	if (!engine->pending_group_programmed) {
		error = engine->ops->key_install(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP,
		    engine->pending_gtk_index, engine->pending_gtk,
		    engine->pending_group_key_generation,
		    engine->pending_group_receive_packet_number);
		if (error != 0)
			return error;
		engine->pending_group_programmed = 1U;
	}
	return 0;
}

/* Programs the staged group key of a group rekey. */
static int
program_pending_group_key(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/* Installs the group key once. */
	if (engine->pending_group_programmed)
		return 0;
	error = engine->ops->key_install(engine->callback_context,
	    engine->generation, WLAN_WPA2_KEY_GROUP,
	    engine->pending_gtk_index, engine->pending_gtk,
	    engine->pending_group_key_generation,
	    engine->pending_group_receive_packet_number);
	if (error == 0)
		engine->pending_group_programmed = 1U;

	/* Reports the installation result. */
	return error;
}

/* Installs the pairwise and group keys from message 3, staging them during a rekey. */
static int
install_keys(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_gtk *gtk,
	const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH])
{
	uint64_t pairwise_generation;
	uint64_t group_generation;
	uint64_t receive_packet_number;
	int error;

	/* The RSC must fit 48 bits. */
	if (rsc[6] != 0U || rsc[7] != 0U)
		return EINVAL;

	/*
	 * A failed programming request may have reached hardware.  Mark each
	 * slot before crossing the callback barrier so fail()->cleanup()
	 * proves it absent before any secret is erased or a lower layer is
	 * retired.
	 */
	if (engine->pairwise_rekey)
		pairwise_generation = engine->pending_pairwise_key_generation;
	else
		pairwise_generation = engine->key_generation;
	group_generation = pairwise_generation;
	receive_packet_number = key_rsc_packet_number(rsc);

	/* A rekey stages both keys for activation after message 4. */
	if (engine->pairwise_rekey) {
		engine->pending_pairwise_installed = 1U;
		engine->pending_group_installed = 1U;
		engine->pending_pairwise_programmed = 0U;
		engine->pending_group_programmed = 0U;
		engine->pending_gtk_index = gtk->key_index;
		engine->pending_group_receive_packet_number =
		    receive_packet_number;
		engine->pending_group_key_generation = group_generation;
		memcpy(engine->pending_gtk, gtk->key,
		    sizeof(engine->pending_gtk));
		error = program_pending_pairwise_keys(engine);
		return error;
	}

	/* The initial handshake installs both keys directly. */
	engine->pairwise_installed = 1U;
	error = engine->ops->key_install(engine->callback_context,
	    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
	    engine->ptk + WLAN_WPA2_KCK_LENGTH + WLAN_WPA2_KEK_LENGTH,
	    pairwise_generation, 0U);
	if (error != 0)
		return error;
	engine->gtk_index = gtk->key_index;
	engine->group_receive_packet_number = receive_packet_number;
	memcpy(engine->gtk, gtk->key, sizeof(engine->gtk));
	engine->group_key_generation = group_generation;
	engine->group_installed = 1U;
	error = engine->ops->key_install(engine->callback_context,
	    engine->generation, WLAN_WPA2_KEY_GROUP, gtk->key_index, gtk->key,
	    group_generation, receive_packet_number);
	if (error != 0)
		return error;
	return 0;
}

/* Handles the first message 1: draws the SNonce, derives the PTK, and answers. */
static int
message_1_first(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	uint64_t now_ticks)
{
	int error;

	/* Records the authenticator's nonce and draws our own. */
	engine->protocol_version = key->protocol_version;
	engine->message_1_replay_counter = key->replay_counter;
	memcpy(engine->anonce, key->nonce, sizeof(engine->anonce));
	error = engine->ops->entropy_fill(engine->callback_context,
	    engine->snonce, sizeof(engine->snonce));
	if (error != 0 || bytes_zero(engine->snonce, sizeof(engine->snonce))) {
		if (error == 0)
			error = EIO;
		error = fail(engine, error);
		return error;
	}

	/* Derives the PTK and sends message 2. */
	error = derive_ptk(engine);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}
	error = build_message_2(engine, now_ticks);
	return error;
}

/* Handles a repeated message 1 by resending message 2. */
static int
message_1_retransmit(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	uint64_t now_ticks)
{
	int error;

	/* The repeat must carry the same version and nonce and not go back. */
	if (key->protocol_version != engine->protocol_version ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce))) {
		error = fail(engine, EACCES);
		return error;
	}
	if (key->replay_counter < engine->message_1_replay_counter) {
		error = fail(engine, EACCES);
		return error;
	}

	/*
	 * Authenticators may increment Replay Counter on an M1 retry.  Reuse
	 * the original SNonce/PTK, but authenticate a newly encoded M2
	 * carrying that counter.  Replacing tx_cookie_active makes a late
	 * report for the superseded M2 harmless.
	 */
	if (key->replay_counter > engine->message_1_replay_counter) {
		engine->message_1_replay_counter = key->replay_counter;
		error = build_message_2(engine, now_ticks);
		return error;
	}

	/* An identical repeat resends the cached message 2. */
	if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX)
		return EALREADY;
	error = retry_current(engine, WLAN_WPA2_STATE_MESSAGE_2_TX, now_ticks);
	return error;
}

/* Digests a message 3 frame so a repeat can be recognized. */
static int
message_3_digest(
	const uint8_t *frame,
	size_t length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	int error;

	error = wlan_sha1(frame, length, digest);
	return error;
}

/* Computes the deadline for a rekey to recover the connection. */
static uint64_t
recovery_deadline(
	const struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	if (UINT64_MAX - now_ticks < engine->profile.recovery_timeout_ticks)
		return UINT64_MAX;
	return now_ticks + engine->profile.recovery_timeout_ticks;
}

/* Begins a pairwise rekey on a message 1 received while authorized. */
static int
pairwise_rekey_begin(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	uint64_t now_ticks)
{
	uint64_t previous_replay;
	uint64_t key_generation;
	int error;

	/* The replay counter must exceed both previous exchanges. */
	previous_replay = engine->message_3_replay_counter;
	if (engine->group_replay_counter > previous_replay)
		previous_replay = engine->group_replay_counter;
	if (!engine->connected_lifetime ||
	    !engine->authorized ||
	    !engine->pairwise_installed ||
	    !engine->group_installed ||
	    key->replay_counter <= previous_replay) {
		error = fail(engine, EACCES);
		return error;
	}

	/*
	 * Close the controlled port before staging a replacement generation.
	 * The old generation remains active only for receiving the rekey
	 * exchange; keys_activate() performs the checked atomic switch after
	 * M4 is ACKed.
	 */
	error = engine->ops->authorized_set(engine->callback_context,
	    engine->generation, 0);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}
	engine->authorized = 0U;
	erase_handshake_secrets(engine);
	error = next_key_generation(engine, &key_generation);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Runs the handshake again under the recovery deadline. */
	engine->pending_pairwise_key_generation = key_generation;
	engine->profile.total_deadline_ticks = recovery_deadline(engine,
	    now_ticks);
	engine->pairwise_rekey = 1U;
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	error = message_1_first(engine, key, now_ticks);
	return error;
}

/* Handles group message 1: stages a new group key or re-acknowledges a repeat. */
static int
group_message_1(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	const uint8_t *frame,
	size_t length,
	uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length;
	uint64_t receive_packet_number;
	uint64_t key_generation;
	uint64_t current_receive_packet_number;
	uint64_t current_group_generation;
	const uint8_t *current_gtk;
	uint8_t current_gtk_index;
	enum wlan_wpa2_state pending_state;
	int staged_new;
	int error;

	plaintext_length = 0U;
	staged_new = 0;

	/* Only an authorized station with keys accepts a fresh replay counter. */
	memset(&gtk, 0, sizeof(gtk));
	if (!engine->authorized ||
	    !engine->pairwise_installed ||
	    !engine->group_installed ||
	    key->protocol_version != engine->protocol_version ||
	    key->replay_counter <= engine->message_3_replay_counter) {
		error = fail(engine, EACCES);
		return error;
	}
	error = eapol_mic_valid(engine, frame, length, key->mic);
	if (error == 0)
		error = message_3_digest(frame, length, digest);

	/* An identical repeat of the accepted message is re-acknowledged. */
	if (error == 0 && engine->group_message_accepted &&
	    key->replay_counter == engine->group_replay_counter) {
		if (!wlan_crypto_equal(digest, engine->group_message_digest,
		    sizeof(digest)))
			error = EACCES;
		wlan_crypto_erase(plaintext, sizeof(plaintext));
		wlan_crypto_erase(&gtk, sizeof(gtk));
		wlan_crypto_erase(digest, sizeof(digest));
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE ||
		    engine->state == WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX ||
		    engine->state ==
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX)
			return EALREADY;
		error = build_group_message_2(engine,
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX,
		    now_ticks);
		return error;
	}
	if (error == 0 && engine->group_message_accepted &&
	    key->replay_counter < engine->group_replay_counter)
		error = EACCES;

	/* Unwraps and parses the new group key. */
	if (error == 0)
		error = wlan_rfc3394_unwrap(engine->ptk +
		    WLAN_WPA2_KCK_LENGTH, key->key_data, key->key_data_length,
		    plaintext, sizeof(plaintext), &plaintext_length);
	if (error == 0)
		error = wlan_wpa2_group_plaintext_parse(plaintext,
		    plaintext_length, &gtk);
	receive_packet_number = key_rsc_packet_number(key->rsc);
	if (error == 0 && (key->rsc[6] != 0U || key->rsc[7] != 0U))
		error = EACCES;

	/* The current group key is the staged one during a group rekey. */
	if (engine->pending_group_installed) {
		current_gtk = engine->pending_gtk;
		current_gtk_index = engine->pending_gtk_index;
		current_receive_packet_number =
		    engine->pending_group_receive_packet_number;
		current_group_generation = engine->pending_group_key_generation;
	} else {
		current_gtk = engine->gtk;
		current_gtk_index = engine->gtk_index;
		current_receive_packet_number =
		    engine->group_receive_packet_number;
		current_group_generation = engine->group_key_generation;
	}

	/*
	 * Authenticators may advance Replay Counter on a G1 retry.  Revalidate
	 * the complete KDE and monotonic RSC, but never allocate/reinstall a
	 * generation or reset its PN.
	 */
	if (error == 0 && engine->group_message_accepted &&
	    gtk.key_index == current_gtk_index &&
	    wlan_crypto_equal(gtk.key, current_gtk, sizeof(engine->gtk))) {
		if (receive_packet_number < current_receive_packet_number) {
			error = EACCES;
		} else if (receive_packet_number >
		    current_receive_packet_number) {
			if (engine->pending_group_installed &&
			    !engine->pending_group_programmed) {
				error = 0;
			} else {
				error = engine->ops->key_receive_pn_advance(
				    engine->callback_context, engine->generation,
				    WLAN_WPA2_KEY_GROUP, current_gtk_index,
				    current_group_generation, receive_packet_number);
			}
			if (error == 0) {
				if (engine->pending_group_installed)
					engine->pending_group_receive_packet_number =
					    receive_packet_number;
				else
					engine->group_receive_packet_number =
					    receive_packet_number;
			}
		}
		if (error == 0) {
			engine->group_replay_counter = key->replay_counter;
			memcpy(engine->group_message_digest, digest,
			    sizeof(engine->group_message_digest));
		}
		wlan_crypto_erase(plaintext, sizeof(plaintext));
		wlan_crypto_erase(&gtk, sizeof(gtk));
		wlan_crypto_erase(digest, sizeof(digest));
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE)
			return EBUSY;
		retire_implicitly_completed_tx(engine);
		if (engine->pending_group_installed)
			pending_state = WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX;
		else
			pending_state =
			    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX;
		error = build_group_message_2(engine, pending_state, now_ticks);
		return error;
	}

	/* Stages the new key under a fresh generation. */
	if (error == 0 && engine->pending_group_installed)
		error = EBUSY;
	if (error == 0)
		error = next_key_generation(engine, &key_generation);
	if (error == 0) {
		engine->profile.total_deadline_ticks = recovery_deadline(engine,
		    now_ticks);
		engine->pending_gtk_index = gtk.key_index;
		engine->pending_group_receive_packet_number =
		    receive_packet_number;
		engine->pending_group_key_generation = key_generation;
		engine->activation_complete = 0U;
		engine->old_group_retired = 0U;
		engine->old_pairwise_retired = 0U;
		engine->pending_group_programmed = 0U;
		memcpy(engine->pending_gtk, gtk.key,
		    sizeof(engine->pending_gtk));

		/*
		 * As with initial programming, a failed request may have
		 * reached hardware and therefore owns an idempotent delete
		 * barrier.
		 */
		engine->pending_group_installed = 1U;
		staged_new = 1;
		error = program_pending_group_key(engine);
	}
	if (error == 0 || (error == EBUSY && staged_new)) {
		engine->group_replay_counter = key->replay_counter;
		memcpy(engine->group_message_digest, digest,
		    sizeof(engine->group_message_digest));
		engine->group_message_accepted = 1U;
	}
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(&gtk, sizeof(gtk));
	wlan_crypto_erase(digest, sizeof(digest));

	/* A busy driver retries the programming from the timer. */
	if (error == EBUSY && staged_new) {
		retire_implicitly_completed_tx(engine);
		error = activation_retry(engine, WLAN_WPA2_STATE_GROUP_STAGE,
		    now_ticks);
		return error;
	}
	if (error == EBUSY)
		return error;
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Answers with group message 2. */
	retire_implicitly_completed_tx(engine);
	error = build_group_message_2(engine,
	    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX, now_ticks);
	return error;
}

/* Handles a repeated message 3 by revalidating it and resending message 4. */
static int
message_3_retransmit(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	const uint8_t *frame,
	size_t length,
	uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length;
	uint64_t receive_packet_number;
	uint64_t current_receive_packet_number;
	uint64_t current_group_generation;
	const uint8_t *current_gtk;
	uint8_t current_gtk_index;
	enum wlan_wpa2_state pending_state;
	int replay_advanced;
	int error;

	plaintext_length = 0U;
	receive_packet_number = 0U;

	/* The repeat must follow an accepted message 3 with the same nonce. */
	memset(&gtk, 0, sizeof(gtk));
	if (!engine->message_3_accepted ||
	    key->protocol_version != engine->protocol_version ||
	    key->replay_counter < engine->message_3_replay_counter ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce))) {
		error = fail(engine, EACCES);
		return error;
	}
	replay_advanced = key->replay_counter >
	    engine->message_3_replay_counter;

	/* The current group key is the staged one during a pairwise rekey. */
	if (engine->pairwise_rekey) {
		current_gtk = engine->pending_gtk;
		current_gtk_index = engine->pending_gtk_index;
		current_receive_packet_number =
		    engine->pending_group_receive_packet_number;
		current_group_generation = engine->pending_group_key_generation;
	} else {
		current_gtk = engine->gtk;
		current_gtk_index = engine->gtk_index;
		current_receive_packet_number =
		    engine->group_receive_packet_number;
		current_group_generation = engine->group_key_generation;
	}
	error = eapol_mic_valid(engine, frame, length, key->mic);
	if (error == 0)
		error = message_3_digest(frame, length, digest);
	if (error == 0 && key->replay_counter ==
	    engine->message_3_replay_counter) {
		/* The same counter must carry the identical frame. */
		if (!wlan_crypto_equal(digest, engine->message_3_digest,
		    sizeof(digest)))
			error = EACCES;
	} else if (error == 0) {
		/*
		 * A standards-compliant authenticator may increment Replay
		 * Counter for an M3 retry.  Re-authenticate all semantics
		 * while preserving the installed slots and their receive PN.
		 */
		error = wlan_rfc3394_unwrap(engine->ptk +
		    WLAN_WPA2_KCK_LENGTH, key->key_data, key->key_data_length,
		    plaintext, sizeof(plaintext), &plaintext_length);
		if (error == 0)
			error = wlan_wpa2_m3_plaintext_parse(plaintext,
			    plaintext_length, &gtk);
		receive_packet_number = key_rsc_packet_number(key->rsc);
		if (error == 0 && (key->rsc[6] != 0U || key->rsc[7] != 0U ||
		    gtk.key_index != current_gtk_index ||
		    receive_packet_number < current_receive_packet_number ||
		    !wlan_crypto_equal(gtk.key, current_gtk,
		    WLAN_WPA2_GTK_LENGTH)))
			error = EACCES;
		if (error == 0 && receive_packet_number >
		    current_receive_packet_number) {
			if (engine->pairwise_rekey &&
			    !engine->pending_group_programmed) {
				error = 0;
			} else {
				error = engine->ops->key_receive_pn_advance(
				    engine->callback_context, engine->generation,
				    WLAN_WPA2_KEY_GROUP, current_gtk_index,
				    current_group_generation, receive_packet_number);
			}
			if (error == 0) {
				if (engine->pairwise_rekey)
					engine->pending_group_receive_packet_number =
					    receive_packet_number;
				else
					engine->group_receive_packet_number =
					    receive_packet_number;
			}
		}
		if (error == 0) {
			engine->message_3_replay_counter = key->replay_counter;
			memcpy(engine->message_3_digest, digest,
			    sizeof(engine->message_3_digest));
		}
	}
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(&gtk, sizeof(gtk));
	wlan_crypto_erase(digest, sizeof(digest));
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Resends message 4 unless one is already on its way. */
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE) {
		if (replay_advanced)
			return EBUSY;
		return EALREADY;
	}
	if (!replay_advanced &&
	    (engine->state == WLAN_WPA2_STATE_MESSAGE_4_TX ||
	    engine->state == WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX))
		return EALREADY;
	if (engine->authorized)
		pending_state = WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX;
	else
		pending_state = WLAN_WPA2_STATE_MESSAGE_4_TX;
	error = build_message_4(engine, pending_state, now_ticks);
	return error;
}

/* Handles the first message 3: installs the keys and answers with message 4. */
static int
message_3_first(
	struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key,
	const uint8_t *frame,
	size_t length,
	uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length;
	int error;

	plaintext_length = 0U;

	/* The message must continue this handshake with a fresh counter. */
	memset(&gtk, 0, sizeof(gtk));
	if (key->protocol_version != engine->protocol_version ||
	    key->replay_counter <= engine->message_1_replay_counter ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce))) {
		error = fail(engine, EACCES);
		return error;
	}

	/* Verifies the MIC, unwraps the group key, and installs the keys. */
	error = eapol_mic_valid(engine, frame, length, key->mic);
	if (error == 0)
		error = wlan_rfc3394_unwrap(engine->ptk + WLAN_WPA2_KCK_LENGTH,
		    key->key_data, key->key_data_length, plaintext,
		    sizeof(plaintext), &plaintext_length);
	if (error == 0)
		error = wlan_wpa2_m3_plaintext_parse(plaintext,
		    plaintext_length, &gtk);
	if (error == 0)
		error = message_3_digest(frame, length,
		    engine->message_3_digest);
	if (error == 0)
		error = install_keys(engine, &gtk, key->rsc);
	if (error == 0 || (error == EBUSY && engine->pairwise_rekey)) {
		engine->message_3_replay_counter = key->replay_counter;
		engine->message_3_accepted = 1U;
	}
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(&gtk, sizeof(gtk));

	/* A busy driver during a rekey retries the programming from the timer. */
	if (error == EBUSY && engine->pairwise_rekey) {
		retire_implicitly_completed_tx(engine);
		error = activation_retry(engine, WLAN_WPA2_STATE_PAIRWISE_STAGE,
		    now_ticks);
		return error;
	}
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* Answers with message 4. */
	retire_implicitly_completed_tx(engine);
	error = build_message_4(engine, WLAN_WPA2_STATE_MESSAGE_4_TX,
	    now_ticks);
	return error;
}

/* Maps a state to the transmission state its retry re-enters. */
static enum wlan_wpa2_state
pending_retry_state(
	enum wlan_wpa2_state state)
{
	switch (state) {
	case WLAN_WPA2_STATE_AUTH_TX:
	case WLAN_WPA2_STATE_AUTH_RESPONSE:
		return WLAN_WPA2_STATE_AUTH_TX;
	case WLAN_WPA2_STATE_ASSOC_TX:
	case WLAN_WPA2_STATE_ASSOC_RESPONSE:
	case WLAN_WPA2_STATE_MESSAGE_1:
		return WLAN_WPA2_STATE_ASSOC_TX;
	case WLAN_WPA2_STATE_MESSAGE_2_TX:
	case WLAN_WPA2_STATE_MESSAGE_3:
		return WLAN_WPA2_STATE_MESSAGE_2_TX;
	case WLAN_WPA2_STATE_MESSAGE_4_TX:
		return WLAN_WPA2_STATE_MESSAGE_4_TX;
	case WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX:
		return WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX;
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX:
		return WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX;
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX:
		return WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX;
	default:
		return WLAN_WPA2_STATE_IDLE;
	}
}

/* Activates a staged group key and retires the old one, resuming after EBUSY. */
static int
group_rekey_commit(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/* Both the old and the staged key must exist. */
	if (!engine->pending_group_installed || !engine->group_installed) {
		error = fail(engine, EINVAL);
		return error;
	}

	/* Switches the hardware, then deletes the old key. */
	if (!engine->activation_complete) {
		error = engine->ops->keys_activate(engine->callback_context,
		    engine->generation, engine->key_generation,
		    engine->pending_group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->activation_complete = 1U;
	}
	if (!engine->old_group_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP, engine->gtk_index,
		    engine->group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->old_group_retired = 1U;
	}

	/* The staged key becomes the current one. */
	engine->group_installed = 0U;
	engine->gtk_index = engine->pending_gtk_index;
	engine->group_receive_packet_number =
	    engine->pending_group_receive_packet_number;
	engine->group_key_generation = engine->pending_group_key_generation;
	memcpy(engine->gtk, engine->pending_gtk, sizeof(engine->gtk));
	engine->group_installed = 1U;
	engine->pending_group_installed = 0U;
	engine->pending_group_programmed = 0U;
	engine->pending_gtk_index = 0U;
	engine->pending_group_receive_packet_number = 0U;
	engine->pending_group_key_generation = 0U;
	wlan_crypto_erase(engine->pending_gtk, sizeof(engine->pending_gtk));
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	engine->state = WLAN_WPA2_STATE_AUTHORIZED;
	engine->step_deadline_ticks = 0U;
	return 0;
}

/* Activates staged pairwise and group keys and retires the old ones, resuming after EBUSY. */
static int
pairwise_rekey_commit(
	struct wlan_wpa2_engine *engine)
{
	uint64_t old_pairwise_generation;
	uint64_t old_group_generation;
	uint8_t old_gtk_index;
	int error;

	old_pairwise_generation = engine->key_generation;
	old_group_generation = engine->group_key_generation;
	old_gtk_index = engine->gtk_index;

	/* Both old keys and both staged keys must exist. */
	if (!engine->pairwise_rekey ||
	    !engine->pairwise_installed ||
	    !engine->group_installed ||
	    !engine->pending_pairwise_installed ||
	    !engine->pending_group_installed) {
		error = fail(engine, EINVAL);
		return error;
	}

	/* Switches the hardware, then deletes the old keys. */
	if (!engine->activation_complete) {
		error = engine->ops->keys_activate(engine->callback_context,
		    engine->generation, engine->pending_pairwise_key_generation,
		    engine->pending_group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->activation_complete = 1U;
	}
	if (!engine->old_group_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP, old_gtk_index,
		    old_group_generation);
		if (error == EBUSY)
			return error;
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->old_group_retired = 1U;
	}
	if (!engine->old_pairwise_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
		    old_pairwise_generation);
		if (error == EBUSY)
			return error;
		if (error != 0) {
			error = fail(engine, error);
			return error;
		}
		engine->old_pairwise_retired = 1U;
	}

	/*
	 * Both deletes are idempotent tombstone operations.  Keep the old-key
	 * ownership flags set until the complete retirement barrier succeeds
	 * so an EBUSY after the group delete can retry activate + both
	 * deletes without making the engine's precondition internally
	 * inconsistent.
	 */
	engine->group_installed = 0U;
	engine->pairwise_installed = 0U;
	engine->key_generation = engine->pending_pairwise_key_generation;
	engine->group_key_generation = engine->pending_group_key_generation;
	engine->gtk_index = engine->pending_gtk_index;
	engine->group_receive_packet_number =
	    engine->pending_group_receive_packet_number;
	memcpy(engine->gtk, engine->pending_gtk, sizeof(engine->gtk));
	engine->pairwise_installed = 1U;
	engine->group_installed = 1U;
	engine->pending_pairwise_installed = 0U;
	engine->pending_group_installed = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	engine->pending_pairwise_key_generation = 0U;
	engine->pending_group_key_generation = 0U;
	engine->pending_gtk_index = 0U;
	engine->pending_group_receive_packet_number = 0U;
	wlan_crypto_erase(engine->pending_gtk, sizeof(engine->pending_gtk));
	engine->pairwise_rekey = 0U;
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	return 0;
}

/* Schedules a retry of a busy activation on the next tick. */
static int
activation_retry(
	struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state state,
	uint64_t now_ticks)
{
	uint64_t retry;
	int error;

	/* The connect budget must still allow it. */
	if (now_ticks >= engine->profile.total_deadline_ticks) {
		error = fail(engine, ETIMEDOUT);
		return error;
	}

	/* Retries one tick later, within the budget. */
	if (now_ticks == UINT64_MAX)
		retry = UINT64_MAX;
	else
		retry = now_ticks + 1U;
	if (retry > engine->profile.total_deadline_ticks)
		retry = engine->profile.total_deadline_ticks;
	engine->state = state;
	engine->step_deadline_ticks = retry;
	return 0;
}

/* Opens the controlled port after the handshake or a rekey. */
static int
pairwise_rekey_authorize(
	struct wlan_wpa2_engine *engine)
{
	int error;

	/*
	 * A nonzero return may still mean the port was opened.  Mark the
	 * uncertain state first so cleanup always drives it closed.
	 */
	engine->authorized = 1U;
	error = engine->ops->authorized_set(engine->callback_context,
	    engine->generation, 1);
	if (error != 0) {
		error = fail(engine, error);
		return error;
	}

	/* The station is connected. */
	engine->state = WLAN_WPA2_STATE_AUTHORIZED;
	engine->connected_lifetime = 1U;
	engine->pairwise_rekey = 0U;
	engine->step_deadline_ticks = 0U;
	return 0;
}
