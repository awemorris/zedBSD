/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

static int activation_retry(struct wlan_wpa2_engine *,
	enum wlan_wpa2_state, uint64_t);

static int
bytes_zero(const uint8_t *bytes, size_t length)
{
	uint8_t combined = 0U;
	size_t index;

	for (index = 0U; index < length; index++)
		combined |= bytes[index];
	return combined == 0U;
}

static int
address_valid(const uint8_t address[WLAN_WPA2_MAC_LENGTH])
{
	return address != NULL && (address[0] & 1U) == 0U &&
	    !bytes_zero(address, WLAN_WPA2_MAC_LENGTH);
}

static int
address_equal(const uint8_t left[WLAN_WPA2_MAC_LENGTH],
	const uint8_t right[WLAN_WPA2_MAC_LENGTH])
{
	return wlan_crypto_equal(left, right, WLAN_WPA2_MAC_LENGTH);
}

static int
expected_management_response(const struct wlan_wpa2_engine *engine,
	const uint8_t *frame, size_t length, uint16_t expected_frame_control)
{
	uint16_t frame_control;

	if (length < WPA2_MANAGEMENT_HEADER_LENGTH)
		return 0;
	frame_control = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
	if ((frame_control & (uint16_t)~WPA2_FC_RETRY) !=
	    expected_frame_control)
		return 0;
	return address_equal(frame + 4U, engine->profile.station) &&
	    address_equal(frame + 10U, engine->profile.bssid) &&
	    address_equal(frame + 16U, engine->profile.bssid);
}

static void
retire_implicitly_completed_tx(struct wlan_wpa2_engine *engine)
{
	/* A peer response for the exact transaction is stronger evidence than a
	 * delayed firmware TX report.  Its cookie must no longer be able to advance
	 * a later state; a report which arrives afterwards is deliberately stale. */
	engine->tx_cookie_active = 0U;
}

static uint64_t
bounded_deadline(const struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	uint64_t deadline;

	if (UINT64_MAX - now_ticks < engine->profile.transition_timeout_ticks)
		deadline = UINT64_MAX;
	else
		deadline = now_ticks + engine->profile.transition_timeout_ticks;
	/* The 30-second connect budget ends at authorization.  An exact M3
	 * retransmission afterwards still needs one bounded M4 response. */
	if (!engine->authorized &&
	    deadline > engine->profile.total_deadline_ticks)
		deadline = engine->profile.total_deadline_ticks;
	return deadline;
}

static int
active_time_valid(const struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	return engine->authorized ||
	    now_ticks < engine->profile.total_deadline_ticks;
}

static uint64_t
next_cookie(struct wlan_wpa2_engine *engine)
{
	engine->tx_cookie_next++;
	if (engine->tx_cookie_next == 0U)
		engine->tx_cookie_next++;
	return engine->tx_cookie_next;
}

static int
next_key_generation(struct wlan_wpa2_engine *engine, uint64_t *result)
{
	if (engine->next_key_generation == UINT64_MAX)
		return EOVERFLOW;
	engine->next_key_generation++;
	if (engine->next_key_generation == 0U)
		return EOVERFLOW;
	*result = engine->next_key_generation;
	return 0;
}

static void
erase_secrets(struct wlan_wpa2_engine *engine, int preserve_pmk)
{
	if (!preserve_pmk)
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

static void
erase_handshake_secrets(struct wlan_wpa2_engine *engine)
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

static int
cleanup(struct wlan_wpa2_engine *engine, int preserve_pmk)
{
	int error;

	engine->tx_cookie_active = 0U;
	engine->step_deadline_ticks = 0U;
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
	/* No installed hardware key can now reference this material.  Erase it
	 * even when a later association/radio barrier needs a checked retry. */
	erase_secrets(engine, preserve_pmk);
	engine->key_generation = 0U;
	engine->group_key_generation = 0U;
	engine->pending_pairwise_key_generation = 0U;
	engine->pending_group_key_generation = 0U;
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
	engine->retry_count = 0U;
	engine->pairwise_rekey = 0U;
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	if (!preserve_pmk) {
		engine->reconnectable = 0U;
		engine->next_key_generation = 0U;
		engine->key_generation = 0U;
		engine->group_key_generation = 0U;
		engine->pending_group_key_generation = 0U;
		engine->pending_pairwise_key_generation = 0U;
	}
	return 0;
}

static int
fail(struct wlan_wpa2_engine *engine, int error)
{
	if (error == 0)
		error = EIO;
	(void)cleanup(engine, engine->reconnectable != 0U);
	engine->last_error = error;
	engine->state = WLAN_WPA2_STATE_FAILED;
	return error;
}

static int
ops_valid(const struct wlan_wpa2_ops *ops)
{
	return ops != NULL && ops->entropy_fill != NULL &&
	    ops->radio_start != NULL && ops->transmit != NULL &&
	    ops->association_set != NULL && ops->association_clear != NULL &&
	    ops->key_install != NULL && ops->key_receive_pn_advance != NULL &&
	    ops->key_delete != NULL &&
	    ops->keys_activate != NULL &&
	    ops->authorized_set != NULL && ops->radio_stop != NULL;
}

static int
profile_valid(const struct wlan_wpa2_profile *profile, uint64_t now_ticks)
{
	size_t index;

	if (profile == NULL || !address_valid(profile->station) ||
	    !address_valid(profile->bssid) ||
	    address_equal(profile->station, profile->bssid) ||
	    profile->ssid_length == 0U ||
	    profile->ssid_length > WLAN_WPA2_SSID_MAX ||
	    profile->rate_count == 0U ||
	    profile->rate_count > WLAN_WPA2_RATE_MAX ||
	    profile->channel == 0U || profile->channel > 11U ||
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
	for (index = 0U; index < profile->rate_count; index++) {
		if ((profile->rates[index] & 0x7fU) == 0U)
			return 0;
	}
	return 1;
}

static int
submit_current(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state, uint64_t now_ticks)
{
	uint64_t cookie;
	int error;

	if (!active_time_valid(engine, now_ticks))
		return fail(engine, ETIMEDOUT);
	engine->state = pending_state;
	engine->step_deadline_ticks = bounded_deadline(engine, now_ticks);
	cookie = next_cookie(engine);
	engine->tx_cookie_active = cookie;
	error = engine->ops->transmit(engine->callback_context,
	    engine->generation, cookie, engine->tx_kind,
	    engine->tx_destination, engine->tx_frame, engine->tx_length,
	    engine->step_deadline_ticks);
	if (error != 0)
		return fail(engine, error);
	return 0;
}

static int
cache_and_submit(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state, enum wlan_wpa2_tx_kind kind,
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH], size_t length,
	uint64_t now_ticks)
{
	if (length == 0U || length > sizeof(engine->tx_frame))
		return fail(engine, EMSGSIZE);
	engine->tx_kind = kind;
	memcpy(engine->tx_destination, destination,
	    sizeof(engine->tx_destination));
	engine->tx_length = length;
	engine->retry_count = 0U;
	return submit_current(engine, pending_state, now_ticks);
}

static int
retry_current(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state, uint64_t now_ticks)
{
	if (engine->retry_count >= WLAN_WPA2_RETRY_MAX)
		return fail(engine, ETIMEDOUT);
	engine->retry_count++;
	return submit_current(engine, pending_state, now_ticks);
}

static int
build_authentication(struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	size_t length;
	int error;

	error = wlan_wpa2_auth_request_build(engine->tx_frame,
	    sizeof(engine->tx_frame), engine->profile.station,
	    engine->profile.bssid, engine->next_sequence, &length);
	if (error != 0)
		return fail(engine, error);
	engine->next_sequence = (uint16_t)((engine->next_sequence + 1U) &
	    0x0fffU);
	return cache_and_submit(engine, WLAN_WPA2_STATE_AUTH_TX,
	    WLAN_WPA2_TX_MANAGEMENT, engine->profile.bssid, length, now_ticks);
}

static int
build_association(struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	size_t length;
	int error;

	error = wlan_wpa2_assoc_request_build(engine->tx_frame,
	    sizeof(engine->tx_frame), engine->profile.station,
	    engine->profile.bssid, engine->next_sequence,
	    engine->profile.capability, engine->profile.listen_interval,
	    engine->profile.ssid, engine->profile.ssid_length,
	    engine->profile.rates, engine->profile.rate_count, &length);
	if (error != 0)
		return fail(engine, error);
	engine->next_sequence = (uint16_t)((engine->next_sequence + 1U) &
	    0x0fffU);
	return cache_and_submit(engine, WLAN_WPA2_STATE_ASSOC_TX,
	    WLAN_WPA2_TX_MANAGEMENT, engine->profile.bssid, length, now_ticks);
}

static int
eapol_mic_calculate(const uint8_t kck[WLAN_WPA2_KCK_LENGTH],
	const uint8_t *frame, size_t length,
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH])
{
	uint8_t copy[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	int error;

	if (frame == NULL || mic == NULL || length <
	    WPA2_EAPOL_MIC_OFFSET + WPA2_EAPOL_MIC_LENGTH ||
	    length > sizeof(copy))
		return EINVAL;
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

static int
eapol_mic_valid(const struct wlan_wpa2_engine *engine,
	const uint8_t *frame, size_t length,
	const uint8_t expected[WLAN_WPA2_KEY_MIC_LENGTH])
{
	uint8_t calculated[WLAN_WPA2_KEY_MIC_LENGTH];
	int error;
	int equal;

	error = eapol_mic_calculate(engine->ptk, frame, length, calculated);
	if (error != 0)
		return error;
	equal = wlan_crypto_equal(calculated, expected, sizeof(calculated));
	wlan_crypto_erase(calculated, sizeof(calculated));
	return equal ? 0 : EACCES;
}

static void
ordered_copy(uint8_t *output, const uint8_t *left, const uint8_t *right,
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

static int
derive_ptk(struct wlan_wpa2_engine *engine)
{
	uint8_t data[WPA2_PTK_DATA_LENGTH];
	int error;

	ordered_copy(data, engine->profile.bssid, engine->profile.station,
	    WLAN_WPA2_MAC_LENGTH);
	ordered_copy(data + 2U * WLAN_WPA2_MAC_LENGTH, engine->anonce,
	    engine->snonce, WLAN_WPA2_NONCE_LENGTH);
	error = wlan_crypto_prf_sha1(engine->pmk, sizeof(engine->pmk),
	    ptk_label, sizeof(ptk_label) - 1U, data, sizeof(data), engine->ptk,
	    sizeof(engine->ptk));
	wlan_crypto_erase(data, sizeof(data));
	return error;
}

static int
build_message_2(struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t rsn[WLAN_WPA2_RSN_IE_LENGTH];
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t rsn_length;
	size_t length;
	int error;

	memset(&key, 0, sizeof(key));
	error = wlan_wpa2_rsn_build_ccmp_psk(rsn, sizeof(rsn), &rsn_length);
	if (error != 0)
		return fail(engine, error);
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
	if (error != 0)
		return fail(engine, error);
	return cache_and_submit(engine, WLAN_WPA2_STATE_MESSAGE_2_TX,
	    WLAN_WPA2_TX_EAPOL, engine->profile.bssid, length, now_ticks);
}

static int
build_message_4(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state, uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t length;
	int error;

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
	if (error != 0)
		return fail(engine, error);
	return cache_and_submit(engine, pending_state, WLAN_WPA2_TX_EAPOL,
	    engine->profile.bssid, length, now_ticks);
}

static int
build_group_message_2(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state pending_state, uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t mic[WLAN_WPA2_KEY_MIC_LENGTH];
	size_t length;
	int error;

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
	if (error != 0)
		return fail(engine, error);
	return cache_and_submit(engine, pending_state, WLAN_WPA2_TX_EAPOL,
	    engine->profile.bssid, length, now_ticks);
}

static uint64_t
key_rsc_packet_number(const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH])
{
	return (uint64_t)rsc[0] | ((uint64_t)rsc[1] << 8) |
	    ((uint64_t)rsc[2] << 16) | ((uint64_t)rsc[3] << 24) |
	    ((uint64_t)rsc[4] << 32) | ((uint64_t)rsc[5] << 40);
}

static int
program_pending_pairwise_keys(struct wlan_wpa2_engine *engine)
{
	int error;

	if (!engine->pending_pairwise_programmed) {
		error = engine->ops->key_install(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
		    engine->ptk + WLAN_WPA2_KCK_LENGTH + WLAN_WPA2_KEK_LENGTH,
		    engine->pending_pairwise_key_generation, 0U);
		if (error != 0)
			return error;
		engine->pending_pairwise_programmed = 1U;
	}
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

static int
program_pending_group_key(struct wlan_wpa2_engine *engine)
{
	int error;

	if (engine->pending_group_programmed)
		return 0;
	error = engine->ops->key_install(engine->callback_context,
	    engine->generation, WLAN_WPA2_KEY_GROUP,
	    engine->pending_gtk_index, engine->pending_gtk,
	    engine->pending_group_key_generation,
	    engine->pending_group_receive_packet_number);
	if (error == 0)
		engine->pending_group_programmed = 1U;
	return error;
}

static int
install_keys(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_gtk *gtk,
	const uint8_t rsc[WLAN_WPA2_KEY_RSC_LENGTH])
{
	uint64_t pairwise_generation;
	uint64_t group_generation;
	uint64_t receive_packet_number;
	int error;

	if (rsc[6] != 0U || rsc[7] != 0U)
		return EINVAL;
	/* A failed programming request may have reached hardware.  Mark each
	 * slot before crossing the callback barrier so fail()->cleanup() proves
	 * it absent before any secret is erased or a lower layer is retired. */
	pairwise_generation = engine->pairwise_rekey ?
	    engine->pending_pairwise_key_generation : engine->key_generation;
	group_generation = pairwise_generation;
	receive_packet_number = key_rsc_packet_number(rsc);
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
		return program_pending_pairwise_keys(engine);
	} else {
		engine->pairwise_installed = 1U;
	}
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

static int
message_1_first(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks)
{
	int error;

	engine->protocol_version = key->protocol_version;
	engine->message_1_replay_counter = key->replay_counter;
	memcpy(engine->anonce, key->nonce, sizeof(engine->anonce));
	error = engine->ops->entropy_fill(engine->callback_context,
	    engine->snonce, sizeof(engine->snonce));
	if (error != 0 || bytes_zero(engine->snonce, sizeof(engine->snonce)))
		return fail(engine, error != 0 ? error : EIO);
	error = derive_ptk(engine);
	if (error != 0)
		return fail(engine, error);
	return build_message_2(engine, now_ticks);
}

static int
message_1_retransmit(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks)
{
	if (key->protocol_version != engine->protocol_version ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce)))
		return fail(engine, EACCES);
	if (key->replay_counter < engine->message_1_replay_counter)
		return fail(engine, EACCES);
	if (key->replay_counter > engine->message_1_replay_counter) {
		/* Authenticators may increment Replay Counter on an M1 retry.
		 * Reuse the original SNonce/PTK, but authenticate a newly encoded
		 * M2 carrying that counter.  Replacing tx_cookie_active makes a
		 * late report for the superseded M2 harmless. */
		engine->message_1_replay_counter = key->replay_counter;
		return build_message_2(engine, now_ticks);
	}
	if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX)
		return EALREADY;
	return retry_current(engine, WLAN_WPA2_STATE_MESSAGE_2_TX, now_ticks);
}

static int
message_3_digest(const uint8_t *frame, size_t length,
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE])
{
	return wlan_sha1(frame, length, digest);
}

static uint64_t
recovery_deadline(const struct wlan_wpa2_engine *engine, uint64_t now_ticks)
{
	if (UINT64_MAX - now_ticks < engine->profile.recovery_timeout_ticks)
		return UINT64_MAX;
	return now_ticks + engine->profile.recovery_timeout_ticks;
}

static int
pairwise_rekey_begin(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, uint64_t now_ticks)
{
	uint64_t previous_replay = engine->message_3_replay_counter;
	uint64_t key_generation;
	int error;

	if (engine->group_replay_counter > previous_replay)
		previous_replay = engine->group_replay_counter;
	if (!engine->reconnectable || !engine->authorized ||
	    !engine->pairwise_installed || !engine->group_installed ||
	    key->replay_counter <= previous_replay)
		return fail(engine, EACCES);
	/* Close the controlled port before staging a replacement generation.  The
	 * old generation remains active only for receiving the rekey exchange;
	 * keys_activate() performs the checked atomic switch after M4 is ACKed. */
	error = engine->ops->authorized_set(engine->callback_context,
	    engine->generation, 0);
	if (error != 0)
		return fail(engine, error);
	engine->authorized = 0U;
	erase_handshake_secrets(engine);
	error = next_key_generation(engine, &key_generation);
	if (error != 0)
		return fail(engine, error);
	engine->pending_pairwise_key_generation = key_generation;
	engine->profile.total_deadline_ticks = recovery_deadline(engine,
	    now_ticks);
	engine->pairwise_rekey = 1U;
	engine->activation_complete = 0U;
	engine->old_group_retired = 0U;
	engine->old_pairwise_retired = 0U;
	return message_1_first(engine, key, now_ticks);
}

static int
group_message_1(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, const uint8_t *frame,
	size_t length, uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length = 0U;
	uint64_t receive_packet_number;
	uint64_t key_generation;
	uint64_t current_receive_packet_number;
	uint64_t current_group_generation;
	const uint8_t *current_gtk;
	uint8_t current_gtk_index;
	int staged_new = 0;
	int error;

	memset(&gtk, 0, sizeof(gtk));
	if (!engine->authorized || !engine->pairwise_installed ||
	    !engine->group_installed ||
	    key->protocol_version != engine->protocol_version ||
	    key->replay_counter <= engine->message_3_replay_counter)
		return fail(engine, EACCES);
	error = eapol_mic_valid(engine, frame, length, key->mic);
	if (error == 0)
		error = message_3_digest(frame, length, digest);
	if (error == 0 && engine->group_message_accepted &&
	    key->replay_counter == engine->group_replay_counter) {
		if (!wlan_crypto_equal(digest, engine->group_message_digest,
		    sizeof(digest)))
			error = EACCES;
		wlan_crypto_erase(plaintext, sizeof(plaintext));
		wlan_crypto_erase(&gtk, sizeof(gtk));
		wlan_crypto_erase(digest, sizeof(digest));
		if (error != 0)
			return fail(engine, error);
		if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE ||
		    engine->state == WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX ||
		    engine->state ==
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX)
			return EALREADY;
		return build_group_message_2(engine,
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX,
		    now_ticks);
	}
	if (error == 0 && engine->group_message_accepted &&
	    key->replay_counter < engine->group_replay_counter)
		error = EACCES;
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
	current_gtk = engine->pending_group_installed ? engine->pending_gtk :
	    engine->gtk;
	current_gtk_index = engine->pending_group_installed ?
	    engine->pending_gtk_index : engine->gtk_index;
	current_receive_packet_number = engine->pending_group_installed ?
	    engine->pending_group_receive_packet_number :
	    engine->group_receive_packet_number;
	current_group_generation = engine->pending_group_installed ?
	    engine->pending_group_key_generation : engine->group_key_generation;
	if (error == 0 && engine->group_message_accepted &&
	    gtk.key_index == current_gtk_index &&
	    wlan_crypto_equal(gtk.key, current_gtk, sizeof(engine->gtk))) {
		/* Authenticators may advance Replay Counter on a G1 retry.  Revalidate
		 * the complete KDE and monotonic RSC, but never allocate/reinstall a
		 * generation or reset its PN. */
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
		if (error != 0)
			return fail(engine, error);
		if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE)
			return EBUSY;
		retire_implicitly_completed_tx(engine);
		return build_group_message_2(engine,
		    engine->pending_group_installed ?
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX :
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX,
		    now_ticks);
	}
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
		/* As with initial programming, a failed request may have reached
		 * hardware and therefore owns an idempotent delete barrier. */
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
	if (error == EBUSY && staged_new) {
		retire_implicitly_completed_tx(engine);
		return activation_retry(engine, WLAN_WPA2_STATE_GROUP_STAGE,
		    now_ticks);
	}
	if (error == EBUSY)
		return error;
	if (error != 0)
		return fail(engine, error);
	retire_implicitly_completed_tx(engine);
	return build_group_message_2(engine,
	    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX, now_ticks);
}

static int
message_3_retransmit(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, const uint8_t *frame,
	size_t length, uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length = 0U;
	uint64_t receive_packet_number = 0U;
	uint64_t current_receive_packet_number;
	uint64_t current_group_generation;
	const uint8_t *current_gtk;
	uint8_t current_gtk_index;
	int replay_advanced;
	int error;

	memset(&gtk, 0, sizeof(gtk));
	if (!engine->message_3_accepted ||
	    key->protocol_version != engine->protocol_version ||
	    key->replay_counter < engine->message_3_replay_counter ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce)))
		return fail(engine, EACCES);
	replay_advanced = key->replay_counter >
	    engine->message_3_replay_counter;
	current_gtk = engine->pairwise_rekey ? engine->pending_gtk :
	    engine->gtk;
	current_gtk_index = engine->pairwise_rekey ?
	    engine->pending_gtk_index : engine->gtk_index;
	current_receive_packet_number = engine->pairwise_rekey ?
	    engine->pending_group_receive_packet_number :
	    engine->group_receive_packet_number;
	current_group_generation = engine->pairwise_rekey ?
	    engine->pending_group_key_generation :
	    engine->group_key_generation;
	error = eapol_mic_valid(engine, frame, length, key->mic);
	if (error == 0)
		error = message_3_digest(frame, length, digest);
	if (error == 0 && key->replay_counter ==
	    engine->message_3_replay_counter) {
		if (!wlan_crypto_equal(digest, engine->message_3_digest,
		    sizeof(digest)))
			error = EACCES;
	} else if (error == 0) {
		/* A standards-compliant authenticator may increment Replay
		 * Counter for an M3 retry.  Re-authenticate all semantics while
		 * preserving the installed slots and their receive PN. */
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
	if (error != 0)
		return fail(engine, error);
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE)
		return replay_advanced ? EBUSY : EALREADY;
	if (!replay_advanced &&
	    (engine->state == WLAN_WPA2_STATE_MESSAGE_4_TX ||
	    engine->state == WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX))
		return EALREADY;
	return build_message_4(engine, engine->authorized ?
	    WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX :
	    WLAN_WPA2_STATE_MESSAGE_4_TX, now_ticks);
}

static int
message_3_first(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_eapol_key *key, const uint8_t *frame,
	size_t length, uint64_t now_ticks)
{
	uint8_t plaintext[WLAN_WPA2_EAPOL_KEY_DATA_MAX];
	struct wlan_wpa2_gtk gtk;
	size_t plaintext_length = 0U;
	int error;

	memset(&gtk, 0, sizeof(gtk));
	if (key->protocol_version != engine->protocol_version ||
	    key->replay_counter <= engine->message_1_replay_counter ||
	    !wlan_crypto_equal(key->nonce, engine->anonce,
	    sizeof(engine->anonce)))
		return fail(engine, EACCES);
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
	if (error == EBUSY && engine->pairwise_rekey) {
		retire_implicitly_completed_tx(engine);
		return activation_retry(engine, WLAN_WPA2_STATE_PAIRWISE_STAGE,
		    now_ticks);
	}
	if (error != 0)
		return fail(engine, error);
	retire_implicitly_completed_tx(engine);
	return build_message_4(engine, WLAN_WPA2_STATE_MESSAGE_4_TX,
	    now_ticks);
}

int
wlan_wpa2_engine_init(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_ops *ops, void *callback_context)
{
	if (engine == NULL || !ops_valid(ops))
		return EINVAL;
	memset(engine, 0, sizeof(*engine));
	engine->ops = ops;
	engine->callback_context = callback_context;
	engine->state = WLAN_WPA2_STATE_IDLE;
	return 0;
}

int
wlan_wpa2_engine_start(struct wlan_wpa2_engine *engine,
	uint64_t generation, const struct wlan_wpa2_profile *profile,
	uint64_t now_ticks)
{
	int error;

	if (engine == NULL || !ops_valid(engine->ops) || generation == 0U ||
	    !profile_valid(profile, now_ticks))
		return EINVAL;
	if (engine->state != WLAN_WPA2_STATE_IDLE &&
	    engine->state != WLAN_WPA2_STATE_FAILED)
		return EBUSY;
	if (engine->state == WLAN_WPA2_STATE_FAILED) {
		error = cleanup(engine, 0);
		if (error != 0) {
			engine->last_error = error;
			return error;
		}
	}
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
	engine->reconnectable = 0U;
	engine->pairwise_rekey = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	engine->state = WLAN_WPA2_STATE_IDLE;
	error = wlan_pbkdf2_hmac_sha1(profile->passphrase,
	    profile->passphrase_length, profile->ssid, profile->ssid_length,
	    4096U, engine->pmk, sizeof(engine->pmk));
	if (error != 0)
		return fail(engine, error);
	/* radio_start() may have configured hardware even when it reports a
	 * failure.  Preserve that uncertainty until radio_stop() confirms the
	 * generation has been retired. */
	engine->configured = 1U;
	error = engine->ops->radio_start(engine->callback_context, generation,
	    engine->profile.bssid, engine->profile.channel,
	    bounded_deadline(engine, now_ticks));
	if (error != 0)
		return fail(engine, error);
	return build_authentication(engine, now_ticks);
}

int
wlan_wpa2_engine_receive_management(struct wlan_wpa2_engine *engine,
	uint64_t generation, const uint8_t *frame, size_t length,
	uint64_t now_ticks)
{
	struct wlan_wpa2_assoc_response response;
	uint16_t status;
	int error;

	if (engine == NULL || frame == NULL)
		return EINVAL;
	if (generation == 0U || generation != engine->generation)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks))
		return fail(engine, ETIMEDOUT);
	if (engine->state == WLAN_WPA2_STATE_AUTH_TX ||
	    engine->state == WLAN_WPA2_STATE_AUTH_RESPONSE) {
		/* Beacons, action frames, frames for another station, and a delayed
		 * response from an older exchange are normal traffic.  Only a response
		 * for this exact transaction may fail the connection on malformed body. */
		if (!expected_management_response(engine, frame, length,
		    WPA2_FC_AUTH_RESPONSE))
			return ESTALE;
		error = wlan_wpa2_auth_response_parse(frame, length,
		    engine->profile.station, engine->profile.bssid, &status);
		if (error != 0)
			return fail(engine, error);
		if (status != 0U)
			return fail(engine, ECONNREFUSED);
		retire_implicitly_completed_tx(engine);
		return build_association(engine, now_ticks);
	}
	if (engine->state == WLAN_WPA2_STATE_ASSOC_TX ||
	    engine->state == WLAN_WPA2_STATE_ASSOC_RESPONSE) {
		if (!expected_management_response(engine, frame, length,
		    WPA2_FC_ASSOC_RESPONSE))
			return ESTALE;
		error = wlan_wpa2_assoc_response_parse(frame, length,
		    engine->profile.station, engine->profile.bssid, &response);
		if (error != 0)
			return fail(engine, error);
		if (response.status != 0U)
			return fail(engine, ECONNREFUSED);
		retire_implicitly_completed_tx(engine);
		/* As with key programming, failure does not prove that the
		 * hardware rejected the request before changing state. */
		engine->associated = 1U;
		engine->aid = response.aid;
		error = engine->ops->association_set(engine->callback_context,
		    engine->generation, engine->profile.bssid, response.aid);
		if (error != 0)
			return fail(engine, error);
		engine->state = WLAN_WPA2_STATE_MESSAGE_1;
		engine->retry_count = 0U;
		engine->step_deadline_ticks = bounded_deadline(engine, now_ticks);
		return 0;
	}
	return ESTALE;
}

int
wlan_wpa2_engine_receive_eapol(struct wlan_wpa2_engine *engine,
	uint64_t generation,
	const uint8_t source[WLAN_WPA2_MAC_LENGTH],
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	const uint8_t *frame, size_t length, uint64_t now_ticks)
{
	struct wlan_wpa2_eapol_key key;
	int error;

	if (engine == NULL || source == NULL || destination == NULL ||
	    frame == NULL)
		return EINVAL;
	if (generation == 0U || generation != engine->generation)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks))
		return fail(engine, ETIMEDOUT);
	if (!address_equal(source, engine->profile.bssid) ||
	    !address_equal(destination, engine->profile.station))
		return fail(engine, EACCES);
	error = wlan_wpa2_eapol_key_parse(frame, length, &key);
	if (error != 0)
		return fail(engine, error);
	if (key.message == WLAN_WPA2_EAPOL_MESSAGE_1) {
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_1)
			return message_1_first(engine, &key, now_ticks);
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_3)
			return message_1_retransmit(engine, &key, now_ticks);
		if (engine->state == WLAN_WPA2_STATE_AUTHORIZED)
			return pairwise_rekey_begin(engine, &key, now_ticks);
		return EBUSY;
	}
	if (key.message == WLAN_WPA2_EAPOL_MESSAGE_3) {
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_2_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_3)
			return message_3_first(engine, &key, frame, length,
			    now_ticks);
		if (engine->state == WLAN_WPA2_STATE_MESSAGE_4_TX ||
		    engine->state == WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX ||
		    engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE ||
		    engine->state == WLAN_WPA2_STATE_AUTHORIZED)
			return message_3_retransmit(engine, &key, frame, length,
			    now_ticks);
		return EBUSY;
	}
	if (key.message == WLAN_WPA2_EAPOL_GROUP_MESSAGE_1 &&
	    (engine->state == WLAN_WPA2_STATE_AUTHORIZED ||
	    engine->state == WLAN_WPA2_STATE_GROUP_STAGE ||
	    engine->state == WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX ||
	    engine->state ==
	    WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX))
		return group_message_1(engine, &key, frame, length, now_ticks);
	return fail(engine, EACCES);
}

static enum wlan_wpa2_state
pending_retry_state(enum wlan_wpa2_state state)
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

static int
group_rekey_commit(struct wlan_wpa2_engine *engine)
{
	int error;

	if (!engine->pending_group_installed || !engine->group_installed)
		return fail(engine, EINVAL);
	if (!engine->activation_complete) {
		error = engine->ops->keys_activate(engine->callback_context,
		    engine->generation, engine->key_generation,
		    engine->pending_group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0)
			return fail(engine, error);
		engine->activation_complete = 1U;
	}
	if (!engine->old_group_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP, engine->gtk_index,
		    engine->group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0)
			return fail(engine, error);
		engine->old_group_retired = 1U;
	}
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

static int
pairwise_rekey_commit(struct wlan_wpa2_engine *engine)
{
	uint64_t old_pairwise_generation = engine->key_generation;
	uint64_t old_group_generation = engine->group_key_generation;
	uint8_t old_gtk_index = engine->gtk_index;
	int error;

	if (!engine->pairwise_rekey || !engine->pairwise_installed ||
	    !engine->group_installed || !engine->pending_pairwise_installed ||
	    !engine->pending_group_installed)
		return fail(engine, EINVAL);
	if (!engine->activation_complete) {
		error = engine->ops->keys_activate(engine->callback_context,
		    engine->generation, engine->pending_pairwise_key_generation,
		    engine->pending_group_key_generation);
		if (error == EBUSY)
			return error;
		if (error != 0)
			return fail(engine, error);
		engine->activation_complete = 1U;
	}
	if (!engine->old_group_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_GROUP, old_gtk_index,
		    old_group_generation);
		if (error == EBUSY)
			return error;
		if (error != 0)
			return fail(engine, error);
		engine->old_group_retired = 1U;
	}
	if (!engine->old_pairwise_retired) {
		error = engine->ops->key_delete(engine->callback_context,
		    engine->generation, WLAN_WPA2_KEY_PAIRWISE, 0U,
		    old_pairwise_generation);
		if (error == EBUSY)
			return error;
		if (error != 0)
			return fail(engine, error);
		engine->old_pairwise_retired = 1U;
	}
	/* Both deletes are idempotent tombstone operations.  Keep the old-key
	 * ownership flags set until the complete retirement barrier succeeds so
	 * an EBUSY after the group delete can retry activate + both deletes
	 * without making the engine's precondition internally inconsistent. */
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

static int
activation_retry(struct wlan_wpa2_engine *engine,
	enum wlan_wpa2_state state, uint64_t now_ticks)
{
	uint64_t retry;

	if (now_ticks >= engine->profile.total_deadline_ticks)
		return fail(engine, ETIMEDOUT);
	retry = now_ticks == UINT64_MAX ? UINT64_MAX : now_ticks + 1U;
	if (retry > engine->profile.total_deadline_ticks)
		retry = engine->profile.total_deadline_ticks;
	engine->state = state;
	engine->step_deadline_ticks = retry;
	return 0;
}

static int
pairwise_rekey_authorize(struct wlan_wpa2_engine *engine)
{
	int error;

	/* A nonzero return may still mean the port was opened.  Mark the
	 * uncertain state first so cleanup always drives it closed. */
	engine->authorized = 1U;
	error = engine->ops->authorized_set(engine->callback_context,
	    engine->generation, 1);
	if (error != 0)
		return fail(engine, error);
	engine->state = WLAN_WPA2_STATE_AUTHORIZED;
	engine->reconnectable = 1U;
	engine->pairwise_rekey = 0U;
	engine->step_deadline_ticks = 0U;
	return 0;
}

int
wlan_wpa2_engine_report_tx(struct wlan_wpa2_engine *engine,
	uint64_t generation, uint64_t cookie, int acknowledged, int error,
	uint64_t now_ticks)
{
	enum wlan_wpa2_state pending;
	int callback_error;

	if (engine == NULL || (acknowledged != 0 && acknowledged != 1) ||
	    error < 0 || (acknowledged && error != 0))
		return EINVAL;
	if (generation == 0U || generation != engine->generation ||
	    cookie == 0U || cookie != engine->tx_cookie_active)
		return ESTALE;
	pending = pending_retry_state(engine->state);
	if (pending == WLAN_WPA2_STATE_IDLE || pending != engine->state)
		return ESTALE;
	if (!active_time_valid(engine, now_ticks))
		return fail(engine, ETIMEDOUT);
	if (!acknowledged || error != 0)
		return retry_current(engine, pending, now_ticks);
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
		if (engine->pairwise_rekey) {
			callback_error = pairwise_rekey_commit(engine);
			if (callback_error == EBUSY)
				return activation_retry(engine,
				    WLAN_WPA2_STATE_PAIRWISE_ACTIVATE,
				    now_ticks);
			if (callback_error != 0)
				return callback_error;
		}
		/* A post-commit duplicate M3 has no staged replacement and the
		 * controlled port is already open.  Otherwise every acknowledged M4,
		 * including a retransmission, crosses the same authorization barrier. */
		if (engine->authorized) {
			engine->state = WLAN_WPA2_STATE_AUTHORIZED;
			engine->step_deadline_ticks = 0U;
			return 0;
		}
		return pairwise_rekey_authorize(engine);
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX:
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX:
		if (!engine->pending_group_installed) {
			engine->state = WLAN_WPA2_STATE_AUTHORIZED;
			engine->step_deadline_ticks = 0U;
			return 0;
		}
		callback_error = group_rekey_commit(engine);
		if (callback_error == EBUSY)
			return activation_retry(engine,
			    WLAN_WPA2_STATE_GROUP_ACTIVATE, now_ticks);
		return callback_error;
	default:
		return ESTALE;
	}
}

int
wlan_wpa2_engine_timer(struct wlan_wpa2_engine *engine,
	uint64_t now_ticks)
{
	enum wlan_wpa2_state pending;
	int error;

	if (engine == NULL)
		return EINVAL;
	if (engine->state == WLAN_WPA2_STATE_IDLE ||
	    engine->state == WLAN_WPA2_STATE_FAILED ||
	    engine->state == WLAN_WPA2_STATE_RECONNECT_WAIT ||
	    engine->state == WLAN_WPA2_STATE_AUTHORIZED)
		return 0;
	if (!active_time_valid(engine, now_ticks))
		return fail(engine, ETIMEDOUT);
	if (now_ticks < engine->step_deadline_ticks)
		return 0;
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_STAGE) {
		error = program_pending_pairwise_keys(engine);
		if (error == EBUSY)
			return activation_retry(engine,
			    WLAN_WPA2_STATE_PAIRWISE_STAGE, now_ticks);
		if (error != 0)
			return fail(engine, error);
		return build_message_4(engine, WLAN_WPA2_STATE_MESSAGE_4_TX,
		    now_ticks);
	}
	if (engine->state == WLAN_WPA2_STATE_GROUP_STAGE) {
		error = program_pending_group_key(engine);
		if (error == EBUSY)
			return activation_retry(engine, WLAN_WPA2_STATE_GROUP_STAGE,
			    now_ticks);
		if (error != 0)
			return fail(engine, error);
		return build_group_message_2(engine,
		    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX, now_ticks);
	}
	if (engine->state == WLAN_WPA2_STATE_PAIRWISE_ACTIVATE) {
		error = pairwise_rekey_commit(engine);
		if (error == EBUSY)
			return activation_retry(engine,
			    WLAN_WPA2_STATE_PAIRWISE_ACTIVATE, now_ticks);
		if (error != 0)
			return error;
		return pairwise_rekey_authorize(engine);
	}
	if (engine->state == WLAN_WPA2_STATE_GROUP_ACTIVATE) {
		error = group_rekey_commit(engine);
		if (error == EBUSY)
			return activation_retry(engine,
			    WLAN_WPA2_STATE_GROUP_ACTIVATE, now_ticks);
		return error;
	}
	pending = pending_retry_state(engine->state);
	if (pending == WLAN_WPA2_STATE_IDLE)
		return fail(engine, EINVAL);
	return retry_current(engine, pending, now_ticks);
}

int
wlan_wpa2_engine_link_lost(struct wlan_wpa2_engine *engine, int error)
{
	int cleanup_error;

	if (engine == NULL || !ops_valid(engine->ops) || error < 0)
		return EINVAL;
	if (!engine->reconnectable)
		return ENOTCONN;
	cleanup_error = cleanup(engine, 1);
	engine->last_error = cleanup_error != 0 ? cleanup_error :
	    (error != 0 ? error : ENETDOWN);
	engine->state = cleanup_error == 0 ?
	    WLAN_WPA2_STATE_RECONNECT_WAIT : WLAN_WPA2_STATE_FAILED;
	return cleanup_error;
}

int
wlan_wpa2_engine_reconnect(struct wlan_wpa2_engine *engine,
	uint64_t generation, uint64_t total_deadline_ticks,
	uint64_t now_ticks)
{
	uint64_t key_generation;
	int error;

	if (engine == NULL || !ops_valid(engine->ops) || generation == 0U ||
	    !engine->reconnectable || total_deadline_ticks <= now_ticks ||
	    (engine->state != WLAN_WPA2_STATE_RECONNECT_WAIT &&
	    engine->state != WLAN_WPA2_STATE_FAILED))
		return EINVAL;
	error = cleanup(engine, 1);
	if (error != 0) {
		engine->last_error = error;
		engine->state = WLAN_WPA2_STATE_FAILED;
		return error;
	}
	error = next_key_generation(engine, &key_generation);
	if (error != 0)
		return fail(engine, error);
	engine->generation = generation;
	engine->key_generation = key_generation;
	engine->group_key_generation = 0U;
	engine->pending_group_key_generation = 0U;
	engine->profile.total_deadline_ticks = total_deadline_ticks;
	engine->last_error = 0;
	engine->state = WLAN_WPA2_STATE_IDLE;
	engine->pairwise_rekey = 0U;
	engine->pending_pairwise_programmed = 0U;
	engine->pending_group_programmed = 0U;
	engine->configured = 1U;
	error = engine->ops->radio_start(engine->callback_context, generation,
	    engine->profile.bssid, engine->profile.channel,
	    bounded_deadline(engine, now_ticks));
	if (error != 0)
		return fail(engine, error);
	return build_authentication(engine, now_ticks);
}

int
wlan_wpa2_engine_can_reconnect(const struct wlan_wpa2_engine *engine)
{
	return engine != NULL && engine->reconnectable != 0U &&
	    !bytes_zero(engine->pmk, sizeof(engine->pmk));
}

int
wlan_wpa2_engine_stop(struct wlan_wpa2_engine *engine)
{
	int error;

	if (engine == NULL || !ops_valid(engine->ops))
		return EINVAL;
	error = cleanup(engine, 0);
	if (error != 0) {
		engine->state = WLAN_WPA2_STATE_FAILED;
		engine->last_error = error;
		return error;
	}
	engine->state = WLAN_WPA2_STATE_IDLE;
	engine->last_error = error;
	engine->generation = 0U;
	engine->key_generation = 0U;
	memset(&engine->profile, 0, sizeof(engine->profile));
	return error;
}

enum wlan_wpa2_state
wlan_wpa2_engine_state(const struct wlan_wpa2_engine *engine)
{
	return engine == NULL ? WLAN_WPA2_STATE_FAILED : engine->state;
}

int
wlan_wpa2_engine_last_error(const struct wlan_wpa2_engine *engine)
{
	return engine == NULL ? EINVAL : engine->last_error;
}

uint64_t
wlan_wpa2_engine_next_deadline(const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL || engine->state == WLAN_WPA2_STATE_IDLE ||
	    engine->state == WLAN_WPA2_STATE_FAILED ||
	    engine->state == WLAN_WPA2_STATE_RECONNECT_WAIT ||
	    engine->state == WLAN_WPA2_STATE_AUTHORIZED)
		return 0U;
	return engine->step_deadline_ticks;
}

#ifdef WLAN_WPA2_TESTING
int
wlan_wpa2_engine_test_secrets_clear(const struct wlan_wpa2_engine *engine)
{
	if (engine == NULL)
		return 0;
	return bytes_zero(engine->pmk, sizeof(engine->pmk)) &&
	    bytes_zero(engine->ptk, sizeof(engine->ptk)) &&
	    bytes_zero(engine->anonce, sizeof(engine->anonce)) &&
	    bytes_zero(engine->snonce, sizeof(engine->snonce)) &&
	    bytes_zero(engine->gtk, sizeof(engine->gtk)) &&
	    bytes_zero(engine->pending_gtk, sizeof(engine->pending_gtk)) &&
	    bytes_zero(engine->message_3_digest,
	    sizeof(engine->message_3_digest)) &&
	    bytes_zero(engine->group_message_digest,
	    sizeof(engine->group_message_digest)) &&
	    bytes_zero(engine->tx_frame, sizeof(engine->tx_frame)) &&
	    engine->group_receive_packet_number == 0U;
}
#endif
