/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/wlan-wpa2.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

static const uint8_t station[WLAN_WPA2_MAC_LENGTH] = {
	0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
};
static const uint8_t bssid[WLAN_WPA2_MAC_LENGTH] = {
	0x02U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU
};
static const uint8_t ssid[] = { 'q', '0', '5', '8', '-', 'a', 'p' };
static const uint8_t passphrase[] = "correct horse battery staple";
static const uint8_t rates[] = {
	0x82U, 0x84U, 0x8bU, 0x96U, 0x0cU, 0x12U
};
static const uint8_t selected_rsn[WLAN_WPA2_RSN_IE_LENGTH] = {
	48U, 20U, 1U, 0U, 0U, 15U, 172U, 4U,
	1U, 0U, 0U, 15U, 172U, 4U, 1U, 0U,
	0U, 15U, 172U, 2U, 0U, 0U
};

struct fake_radio {
	uint64_t generation;
	uint64_t cookie;
	uint64_t deadline;
	enum wlan_wpa2_tx_kind tx_kind;
	uint8_t destination[WLAN_WPA2_MAC_LENGTH];
	uint8_t frame[WLAN_WPA2_FRAME_MAX];
	size_t frame_length;
	unsigned entropy_calls;
	unsigned radio_start_calls;
	unsigned tx_calls;
	unsigned management_tx_calls;
	unsigned eapol_tx_calls;
	unsigned association_set_calls;
	unsigned association_clear_calls;
	unsigned pair_install_calls;
	unsigned group_install_calls;
	unsigned pn_advance_calls;
	unsigned pair_delete_calls;
	unsigned group_delete_calls;
	unsigned activate_calls;
	unsigned authorize_on_calls;
	unsigned authorize_off_calls;
	unsigned radio_stop_calls;
	int fail_entropy;
	int fail_radio_start;
	int fail_association_set;
	int fail_pair_install;
	int fail_group_install;
	int fail_pn_advance;
	int fail_pair_delete;
	int fail_group_delete;
	int fail_activate;
	unsigned pair_delete_busy_remaining;
	unsigned group_delete_busy_remaining;
	unsigned activate_busy_remaining;
	unsigned pair_install_busy_remaining;
	unsigned group_install_busy_remaining;
	int fail_authorize;
	int fail_radio_stop;
	uint8_t pairwise_key[WLAN_WPA2_TK_LENGTH];
	uint8_t group_key[WLAN_WPA2_TK_LENGTH];
	uint8_t group_index;
	uint64_t group_receive_pn;
	uint64_t pairwise_key_generation;
	uint64_t group_key_generation;
	uint64_t activated_pairwise_generation;
	uint64_t activated_group_generation;
};

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_be16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

static void
put_be64(uint8_t *bytes, uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++) {
		bytes[7U - index] = (uint8_t)value;
		value >>= 8;
	}
}

static void
fill_bytes(uint8_t *bytes, size_t length, uint8_t seed)
{
	size_t index;

	for (index = 0U; index < length; index++)
		bytes[index] = (uint8_t)(seed + index * 3U);
}

static int
fake_entropy(void *context, void *buffer, size_t length)
{
	struct fake_radio *fake = context;

	fake->entropy_calls++;
	if (fake->fail_entropy)
		return EIO;
	fill_bytes(buffer, length, 0x71U);
	return 0;
}

static int
fake_radio_start(void *context, uint64_t generation,
	const uint8_t expected_bssid[WLAN_WPA2_MAC_LENGTH], uint32_t channel,
	uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(memcmp(expected_bssid, bssid, sizeof(bssid)) == 0);
	assert(channel == 6U && deadline != 0U);
	fake->generation = generation;
	fake->radio_start_calls++;
	return fake->fail_radio_start ? EIO : 0;
}

static int
fake_transmit(void *context, uint64_t generation, uint64_t cookie,
	enum wlan_wpa2_tx_kind kind,
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH], const uint8_t *frame,
	size_t length, uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation && cookie != 0U);
	assert(kind == WLAN_WPA2_TX_MANAGEMENT ||
	    kind == WLAN_WPA2_TX_EAPOL);
	assert(memcmp(destination, bssid, sizeof(bssid)) == 0);
	assert(frame != NULL && length != 0U && length <= sizeof(fake->frame));
	fake->cookie = cookie;
	fake->deadline = deadline;
	fake->tx_kind = kind;
	memcpy(fake->destination, destination, sizeof(fake->destination));
	memcpy(fake->frame, frame, length);
	fake->frame_length = length;
	fake->tx_calls++;
	if (kind == WLAN_WPA2_TX_MANAGEMENT)
		fake->management_tx_calls++;
	else
		fake->eapol_tx_calls++;
	return 0;
}

static int
fake_association_set(void *context, uint64_t generation,
	const uint8_t expected_bssid[WLAN_WPA2_MAC_LENGTH], uint16_t aid)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation);
	assert(memcmp(expected_bssid, bssid, sizeof(bssid)) == 0);
	assert(aid == 42U);
	fake->association_set_calls++;
	return fake->fail_association_set ? EIO : 0;
}

static int
fake_association_clear(void *context, uint64_t generation)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation);
	fake->association_clear_calls++;
	return 0;
}

static int
fake_key_install(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	const uint8_t key[WLAN_WPA2_TK_LENGTH], uint64_t key_generation,
	uint64_t receive_packet_number)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation && key_generation != 0U);
	if (kind == WLAN_WPA2_KEY_PAIRWISE) {
		assert(key_index == 0U && receive_packet_number == 0U);
		fake->pair_install_calls++;
		if (fake->fail_pair_install)
			return EIO;
		if (fake->pair_install_busy_remaining != 0U) {
			fake->pair_install_busy_remaining--;
			return EBUSY;
		}
		memcpy(fake->pairwise_key, key, sizeof(fake->pairwise_key));
		fake->pairwise_key_generation = key_generation;
		return 0;
	}
	assert(kind == WLAN_WPA2_KEY_GROUP && key_index > 0U &&
	    key_index <= 3U);
	fake->group_install_calls++;
	if (fake->fail_group_install)
		return EIO;
	if (fake->group_install_busy_remaining != 0U) {
		fake->group_install_busy_remaining--;
		return EBUSY;
	}
	memcpy(fake->group_key, key, sizeof(fake->group_key));
	fake->group_index = key_index;
	fake->group_receive_pn = receive_packet_number;
	fake->group_key_generation = key_generation;
	return 0;
}

static int
fake_key_delete(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	uint64_t key_generation)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation && key_generation != 0U);
	if (kind == WLAN_WPA2_KEY_PAIRWISE) {
		assert(key_index == 0U);
		fake->pair_delete_calls++;
		if (fake->fail_pair_delete)
			return EBUSY;
		if (fake->pair_delete_busy_remaining != 0U) {
			fake->pair_delete_busy_remaining--;
			return EBUSY;
		}
	} else {
		assert(kind == WLAN_WPA2_KEY_GROUP && key_index > 0U &&
		    key_index <= 3U);
		fake->group_delete_calls++;
		if (fake->fail_group_delete)
			return EBUSY;
		if (fake->group_delete_busy_remaining != 0U) {
			fake->group_delete_busy_remaining--;
			return EBUSY;
		}
	}
	return 0;
}

static int
fake_key_receive_pn_advance(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t receive_packet_number)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation && key_generation != 0U &&
	    kind == WLAN_WPA2_KEY_GROUP && key_index > 0U && key_index <= 3U &&
	    receive_packet_number > fake->group_receive_pn);
	fake->pn_advance_calls++;
	if (fake->fail_pn_advance)
		return EIO;
	fake->group_receive_pn = receive_packet_number;
	return 0;
}

static int
fake_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_key_generation, uint64_t group_key_generation)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation &&
	    pairwise_key_generation != 0U && group_key_generation != 0U);
	fake->activate_calls++;
	if (fake->fail_activate)
		return EIO;
	if (fake->activate_busy_remaining != 0U) {
		fake->activate_busy_remaining--;
		return EBUSY;
	}
	fake->activated_pairwise_generation = pairwise_key_generation;
	fake->activated_group_generation = group_key_generation;
	return 0;
}

static int
fake_authorized_set(void *context, uint64_t generation, int authorized)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation);
	if (authorized) {
		fake->authorize_on_calls++;
		if (fake->fail_authorize)
			return EIO;
	} else {
		fake->authorize_off_calls++;
	}
	return 0;
}

static int
fake_radio_stop(void *context, uint64_t generation)
{
	struct fake_radio *fake = context;

	assert(generation == fake->generation);
	fake->radio_stop_calls++;
	return fake->fail_radio_stop ? EBUSY : 0;
}

static const struct wlan_wpa2_ops fake_ops = {
	.entropy_fill = fake_entropy,
	.radio_start = fake_radio_start,
	.transmit = fake_transmit,
	.association_set = fake_association_set,
	.association_clear = fake_association_clear,
	.key_install = fake_key_install,
	.key_receive_pn_advance = fake_key_receive_pn_advance,
	.key_delete = fake_key_delete,
	.keys_activate = fake_keys_activate,
	.authorized_set = fake_authorized_set,
	.radio_stop = fake_radio_stop
};

static struct wlan_wpa2_profile
test_profile(uint64_t deadline)
{
	struct wlan_wpa2_profile profile;

	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station, station, sizeof(station));
	memcpy(profile.bssid, bssid, sizeof(bssid));
	memcpy(profile.ssid, ssid, sizeof(ssid));
	profile.ssid_length = sizeof(ssid);
	memcpy(profile.rates, rates, sizeof(rates));
	profile.rate_count = ARRAY_COUNT(rates);
	profile.channel = 6U;
	profile.capability = 0x0431U;
	profile.listen_interval = 10U;
	profile.initial_sequence = 0x100U;
	profile.passphrase = passphrase;
	profile.passphrase_length = sizeof(passphrase) - 1U;
	profile.total_deadline_ticks = deadline;
	profile.transition_timeout_ticks = 10U;
	profile.recovery_timeout_ticks = 300U;
	return profile;
}

static uint64_t
captured_cookie(const struct fake_radio *fake)
{
	assert(fake->cookie != 0U);
	return fake->cookie;
}

static int
report_captured(struct wlan_wpa2_engine *engine, struct fake_radio *fake,
	int acknowledged, int error, uint64_t now)
{
	uint64_t cookie = captured_cookie(fake);

	return wlan_wpa2_engine_report_tx(engine, fake->generation, cookie,
	    acknowledged, error, now);
}

static size_t
authentication_response(uint8_t frame[64], uint16_t status)
{
	memset(frame, 0, 64U);
	put_le16(frame, 0x00b0U);
	memcpy(frame + 4U, station, sizeof(station));
	memcpy(frame + 10U, bssid, sizeof(bssid));
	memcpy(frame + 16U, bssid, sizeof(bssid));
	put_le16(frame + 24U, 0U);
	put_le16(frame + 26U, 2U);
	put_le16(frame + 28U, status);
	return 30U;
}

static size_t
association_response(uint8_t frame[64], uint16_t status)
{
	static const uint8_t response_rates[] = {
		0x82U, 0x84U, 0x8bU, 0x96U
	};
	size_t offset = 30U;

	memset(frame, 0, 64U);
	put_le16(frame, 0x0010U);
	memcpy(frame + 4U, station, sizeof(station));
	memcpy(frame + 10U, bssid, sizeof(bssid));
	memcpy(frame + 16U, bssid, sizeof(bssid));
	put_le16(frame + 24U, 0x0431U);
	put_le16(frame + 26U, status);
	put_le16(frame + 28U, status == 0U ? 0xc02aU : 0U);
	if (status == 0U) {
		frame[offset++] = 1U;
		frame[offset++] = sizeof(response_rates);
		memcpy(frame + offset, response_rates, sizeof(response_rates));
		offset += sizeof(response_rates);
	}
	return offset;
}

static size_t
message_1(uint8_t frame[WLAN_WPA2_EAPOL_FRAME_MAX], uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH])
{
	/* Independently encode descriptor version 2, pairwise+ACK M1. */
	memset(frame, 0, 99U);
	frame[0] = 2U;
	frame[1] = 3U;
	put_be16(frame + 2U, 95U);
	frame[4] = 2U;
	put_be16(frame + 5U, 0x008aU);
	put_be16(frame + 7U, WLAN_WPA2_TK_LENGTH);
	put_be64(frame + 9U, replay);
	memcpy(frame + 17U, anonce, WLAN_WPA2_NONCE_LENGTH);
	put_be16(frame + 97U, 0U);
	return 99U;
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

static void
derive_test_ptk(const uint8_t snonce[WLAN_WPA2_NONCE_LENGTH],
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH],
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH])
{
	static const uint8_t label[] = "Pairwise key expansion";
	uint8_t pmk[WLAN_WPA2_PMK_LENGTH];
	uint8_t data[2U * WLAN_WPA2_MAC_LENGTH +
	    2U * WLAN_WPA2_NONCE_LENGTH];

	assert(wlan_pbkdf2_hmac_sha1(passphrase, sizeof(passphrase) - 1U,
	    ssid, sizeof(ssid), 4096U, pmk, sizeof(pmk)) == 0);
	ordered_copy(data, bssid, station, sizeof(station));
	ordered_copy(data + 2U * sizeof(station), anonce, snonce,
	    WLAN_WPA2_NONCE_LENGTH);
	assert(wlan_crypto_prf_sha1(pmk, sizeof(pmk), label,
	    sizeof(label) - 1U, data, sizeof(data), ptk,
	    WLAN_WPA2_PTK_LENGTH) == 0);
	wlan_crypto_erase(pmk, sizeof(pmk));
	wlan_crypto_erase(data, sizeof(data));
}

static void
eapol_sign(uint8_t *frame, size_t length,
	const uint8_t kck[WLAN_WPA2_KCK_LENGTH])
{
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];

	assert(length >= 97U && length <= WLAN_WPA2_EAPOL_FRAME_MAX);
	memset(frame + 81U, 0, WLAN_WPA2_KEY_MIC_LENGTH);
	assert(wlan_hmac_sha1(kck, WLAN_WPA2_KCK_LENGTH, frame, length,
	    digest) == 0);
	memcpy(frame + 81U, digest, WLAN_WPA2_KEY_MIC_LENGTH);
	wlan_crypto_erase(digest, sizeof(digest));
}

static int
eapol_mic_matches(const uint8_t *frame, size_t length,
	const uint8_t kck[WLAN_WPA2_KCK_LENGTH])
{
	uint8_t copy[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t expected[WLAN_WPA2_KEY_MIC_LENGTH];
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];
	int result;

	assert(length <= sizeof(copy));
	memcpy(copy, frame, length);
	memcpy(expected, copy + 81U, sizeof(expected));
	memset(copy + 81U, 0, sizeof(expected));
	assert(wlan_hmac_sha1(kck, WLAN_WPA2_KCK_LENGTH, copy, length,
	    digest) == 0);
	result = wlan_crypto_equal(expected, digest, sizeof(expected));
	wlan_crypto_erase(copy, sizeof(copy));
	wlan_crypto_erase(expected, sizeof(expected));
	wlan_crypto_erase(digest, sizeof(digest));
	return result;
}

static void
xor_t(uint8_t accumulator[8], uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++) {
		accumulator[7U - index] ^= (uint8_t)value;
		value >>= 8;
	}
}

static size_t
rfc3394_wrap(const uint8_t kek[WLAN_WPA2_KEK_LENGTH],
	const uint8_t *plaintext, size_t plaintext_length, uint8_t *wrapped,
	size_t capacity)
{
	uint8_t accumulator[8] = {
		0xa6U, 0xa6U, 0xa6U, 0xa6U,
		0xa6U, 0xa6U, 0xa6U, 0xa6U
	};
	uint8_t block[16];
	uint8_t encrypted[16];
	size_t n;
	size_t index;
	unsigned round;

	assert(plaintext_length >= 16U && plaintext_length % 8U == 0U);
	assert(capacity >= plaintext_length + 8U);
	n = plaintext_length / 8U;
	memcpy(wrapped + 8U, plaintext, plaintext_length);
	for (round = 0U; round < 6U; round++) {
		for (index = 1U; index <= n; index++) {
			memcpy(block, accumulator, 8U);
			memcpy(block + 8U, wrapped + index * 8U, 8U);
			assert(wlan_aes128_encrypt_block(kek, block, encrypted) == 0);
			memcpy(accumulator, encrypted, 8U);
			xor_t(accumulator,
			    (uint64_t)round * (uint64_t)n + (uint64_t)index);
			memcpy(wrapped + index * 8U, encrypted + 8U, 8U);
		}
	}
	memcpy(wrapped, accumulator, 8U);
	wlan_crypto_erase(accumulator, sizeof(accumulator));
	wlan_crypto_erase(block, sizeof(block));
	wlan_crypto_erase(encrypted, sizeof(encrypted));
	return plaintext_length + 8U;
}

static size_t
message_3(uint8_t frame[WLAN_WPA2_EAPOL_FRAME_MAX], uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH], const uint8_t *ptk,
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH], uint8_t gtk_index,
	uint64_t receive_pn)
{
	uint8_t plaintext[64];
	uint8_t wrapped[72];
	size_t wrapped_length;
	size_t length;
	unsigned index;

	/* Independently encode the selected RSN IE, one GTK KDE, and padding. */
	assert(gtk_index > 0U && gtk_index <= 3U);
	memcpy(plaintext, selected_rsn, sizeof(selected_rsn));
	plaintext[22] = 221U;
	plaintext[23] = 22U;
	plaintext[24] = 0U;
	plaintext[25] = 15U;
	plaintext[26] = 172U;
	plaintext[27] = 1U;
	plaintext[28] = gtk_index;
	plaintext[29] = 0U;
	memcpy(plaintext + 30U, gtk, WLAN_WPA2_GTK_LENGTH);
	plaintext[46] = 221U;
	plaintext[47] = 0U;
	wrapped_length = rfc3394_wrap(ptk + WLAN_WPA2_KCK_LENGTH,
	    plaintext, 48U, wrapped, sizeof(wrapped));
	length = 99U + wrapped_length;
	memset(frame, 0, length);
	frame[0] = 2U;
	frame[1] = 3U;
	put_be16(frame + 2U, (uint16_t)(95U + wrapped_length));
	frame[4] = 2U;
	put_be16(frame + 5U, 0x13caU);
	put_be16(frame + 7U, WLAN_WPA2_TK_LENGTH);
	put_be64(frame + 9U, replay);
	memcpy(frame + 17U, anonce, WLAN_WPA2_NONCE_LENGTH);
	for (index = 0U; index < 6U; index++) {
		frame[65U + index] = (uint8_t)receive_pn;
		receive_pn >>= 8;
	}
	put_be16(frame + 97U, (uint16_t)wrapped_length);
	memcpy(frame + 99U, wrapped, wrapped_length);
	eapol_sign(frame, length, ptk);
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(wrapped, sizeof(wrapped));
	return length;
}

static size_t
group_message_1(uint8_t frame[WLAN_WPA2_EAPOL_FRAME_MAX], uint64_t replay,
	const uint8_t ptk[WLAN_WPA2_PTK_LENGTH],
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH], uint8_t gtk_index,
	uint64_t receive_pn)
{
	struct wlan_wpa2_eapol_key key;
	uint8_t plaintext[32];
	uint8_t wrapped[40];
	size_t plaintext_length;
	size_t wrapped_length;
	size_t length;
	unsigned index;

	assert(wlan_wpa2_group_plaintext_build(plaintext, sizeof(plaintext),
	    gtk_index, gtk, &plaintext_length) == 0);
	wrapped_length = rfc3394_wrap(ptk + WLAN_WPA2_KCK_LENGTH, plaintext,
	    plaintext_length, wrapped, sizeof(wrapped));
	memset(&key, 0, sizeof(key));
	key.message = WLAN_WPA2_EAPOL_GROUP_MESSAGE_1;
	key.protocol_version = 2U;
	key.key_length = WLAN_WPA2_TK_LENGTH;
	key.replay_counter = replay;
	for (index = 0U; index < 6U; index++) {
		key.rsc[index] = (uint8_t)receive_pn;
		receive_pn >>= 8;
	}
	key.key_data = wrapped;
	key.key_data_length = wrapped_length;
	assert(wlan_wpa2_eapol_key_build(frame, WLAN_WPA2_EAPOL_FRAME_MAX,
	    &key, &length) == 0);
	eapol_sign(frame, length, ptk);
	wlan_crypto_erase(&key, sizeof(key));
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(wrapped, sizeof(wrapped));
	return length;
}

static void
init_and_reach_message_1(struct wlan_wpa2_engine *engine,
	struct fake_radio *fake, uint64_t generation)
{
	struct wlan_wpa2_profile profile = test_profile(1000U);
	uint8_t response[64];
	size_t length;
	uint64_t original_cookie;

	memset(fake, 0, sizeof(*fake));
	assert(wlan_wpa2_engine_init(engine, &fake_ops, fake) == 0);
	assert(wlan_wpa2_engine_start(engine, generation, &profile, 1U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_AUTH_TX);
	assert(fake->radio_start_calls == 1U && fake->management_tx_calls == 1U);
	assert(fake->authorize_on_calls == 0U);
	original_cookie = captured_cookie(fake);
	assert(wlan_wpa2_engine_report_tx(engine, generation,
	    original_cookie + 1U, 1, 0, 2U) == ESTALE);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_AUTH_TX);
	assert(report_captured(engine, fake, 1, 0, 2U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_AUTH_RESPONSE);
	length = authentication_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(engine, generation, response,
	    length, 3U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_ASSOC_TX);
	assert(report_captured(engine, fake, 1, 0, 4U) == 0);
	assert(wlan_wpa2_engine_state(engine) ==
	    WLAN_WPA2_STATE_ASSOC_RESPONSE);
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(engine, generation, response,
	    length, 5U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_MESSAGE_1);
	assert(fake->association_set_calls == 1U);
}

static size_t
reach_message_3(struct wlan_wpa2_engine *engine, struct fake_radio *fake,
	uint64_t generation, uint8_t anonce[WLAN_WPA2_NONCE_LENGTH],
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH],
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX])
{
	struct wlan_wpa2_eapol_key parsed;
	size_t m1_length;

	init_and_reach_message_1(engine, fake, generation);
	fill_bytes(anonce, WLAN_WPA2_NONCE_LENGTH, 0x21U);
	m1_length = message_1(m1, 17U, anonce);
	assert(wlan_wpa2_engine_receive_eapol(engine, generation, bssid,
	    station, m1, m1_length, 6U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_MESSAGE_2_TX);
	assert(fake->entropy_calls == 1U && fake->eapol_tx_calls == 1U);
	/* A duplicate M1 while that M2 awaits C2H completion is absorbed. */
	assert(wlan_wpa2_engine_receive_eapol(engine, generation, bssid,
	    station, m1, m1_length, 6U) == EALREADY);
	assert(fake->entropy_calls == 1U && fake->eapol_tx_calls == 1U);
	assert(wlan_wpa2_eapol_key_parse(fake->frame, fake->frame_length,
	    &parsed) == 0);
	assert(parsed.message == WLAN_WPA2_EAPOL_MESSAGE_2 &&
	    parsed.replay_counter == 17U);
	derive_test_ptk(parsed.nonce, anonce, ptk);
	assert(eapol_mic_matches(fake->frame, fake->frame_length, ptk));
	assert(report_captured(engine, fake, 1, 0, 7U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_MESSAGE_3);
	return m1_length;
}

static void
reach_authorized(struct wlan_wpa2_engine *engine, struct fake_radio *fake,
	uint64_t generation, uint8_t anonce[WLAN_WPA2_NONCE_LENGTH],
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH],
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH], uint8_t gtk_index)
{
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;

	(void)reach_message_3(engine, fake, generation, anonce, ptk, m1);
	fill_bytes(gtk, WLAN_WPA2_GTK_LENGTH, 0x91U);
	length = message_3(m3, 18U, anonce, ptk, gtk, gtk_index, 7U);
	assert(wlan_wpa2_engine_receive_eapol(engine, generation, bssid,
	    station, m3, length, 8U) == 0);
	assert(report_captured(engine, fake, 1, 0, 9U) == 0);
	assert(wlan_wpa2_engine_state(engine) == WLAN_WPA2_STATE_AUTHORIZED &&
	    engine->reconnectable && engine->authorized);
}

static void
test_peer_response_precedes_tx_report(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_profile profile = test_profile(1000U);
	struct wlan_wpa2_eapol_key parsed;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t eapol[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t response[64];
	size_t length;
	uint64_t auth_cookie;
	uint64_t assoc_cookie;
	uint64_t m2_cookie;
	uint64_t m4_cookie;

	memset(&fake, 0, sizeof(fake));
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	assert(wlan_wpa2_engine_start(&engine, 114U, &profile, 1U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTH_TX);
	auth_cookie = captured_cookie(&fake);

	/* An unrelated response cannot retire the in-flight authentication. */
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 114U, response,
	    length, 2U) == ESTALE);
	assert(engine.tx_cookie_active == auth_cookie);

	/* The exact authentication response proves that the AP received the TX,
	 * even when its firmware completion has not reached the host yet. */
	length = authentication_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 114U, response,
	    length, 2U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_ASSOC_TX);
	assoc_cookie = captured_cookie(&fake);
	assert(assoc_cookie != auth_cookie &&
	    engine.tx_cookie_active == assoc_cookie);
	assert(wlan_wpa2_engine_report_tx(&engine, 114U, auth_cookie, 1, 0,
	    2U) == ESTALE);

	/* Association has the same ordering rule.  Its response leaves no active
	 * association-request cookie while the engine waits for M1. */
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 114U, response,
	    length, 3U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_1 &&
	    engine.tx_cookie_active == 0U && fake.association_set_calls == 1U);
	assert(wlan_wpa2_engine_report_tx(&engine, 114U, assoc_cookie, 1, 0,
	    3U) == ESTALE);

	fill_bytes(anonce, sizeof(anonce), 0x21U);
	length = message_1(eapol, 17U, anonce);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 114U, bssid, station,
	    eapol, length, 4U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_2_TX);
	m2_cookie = captured_cookie(&fake);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 && parsed.message == WLAN_WPA2_EAPOL_MESSAGE_2);
	derive_test_ptk(parsed.nonce, anonce, ptk);

	/* A valid M3 authenticates receipt of M2.  It may therefore replace that
	 * pending report with M4, but M4 itself still requires TX completion. */
	fill_bytes(gtk, sizeof(gtk), 0x91U);
	length = message_3(eapol, 18U, anonce, ptk, gtk, 2U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 114U, bssid, station,
	    eapol, length, 5U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_4_TX);
	m4_cookie = captured_cookie(&fake);
	assert(m4_cookie != m2_cookie && engine.tx_cookie_active == m4_cookie &&
	    fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.authorize_on_calls == 0U);
	assert(wlan_wpa2_engine_report_tx(&engine, 114U, m2_cookie, 1, 0,
	    5U) == ESTALE);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_4_TX &&
	    fake.authorize_on_calls == 0U);
	assert(wlan_wpa2_engine_report_tx(&engine, 114U, m4_cookie, 1, 0,
	    6U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED &&
	    fake.authorize_on_calls == 1U);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_complete_handshake_and_retransmission(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_eapol_key parsed;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t saved_m2[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t saved_m2_length;
	size_t m1_length;
	size_t m3_length;
	uint64_t superseded_cookie;
	unsigned eapol_before;

	m1_length = reach_message_3(&engine, &fake, 91U, anonce, ptk, m1);
	/* An exact M1 reuses SNonce/PTK and resends the byte-identical M2. */
	saved_m2_length = fake.frame_length;
	memcpy(saved_m2, fake.frame, saved_m2_length);
	eapol_before = fake.eapol_tx_calls;
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m1, m1_length, 8U) == 0);
	assert(fake.entropy_calls == 1U &&
	    fake.eapol_tx_calls == eapol_before + 1U);
	assert(fake.frame_length == saved_m2_length &&
	    memcmp(fake.frame, saved_m2, saved_m2_length) == 0);
	/* A higher replay on an M1 retry replaces the pending M2 but keeps
	 * SNonce/PTK.  A late C2H report for the replaced M2 is stale. */
	superseded_cookie = captured_cookie(&fake);
	m1_length = message_1(m1, 18U, anonce);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m1, m1_length, 9U) == 0);
	assert(fake.entropy_calls == 1U &&
	    fake.eapol_tx_calls == eapol_before + 2U &&
	    captured_cookie(&fake) != superseded_cookie);
	assert(wlan_wpa2_engine_report_tx(&engine, 91U, superseded_cookie,
	    1, 0, 9U) == ESTALE);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 && parsed.message == WLAN_WPA2_EAPOL_MESSAGE_2 &&
	    parsed.replay_counter == 18U &&
	    eapol_mic_matches(fake.frame, fake.frame_length, ptk));
	assert(report_captured(&engine, &fake, 1, 0, 10U) == 0);

	fill_bytes(gtk, sizeof(gtk), 0x91U);
	m3_length = message_3(m3, 19U, anonce, ptk, gtk, 2U,
	    UINT64_C(0x00000504030201));
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m3, m3_length, 11U) == 0);
	assert(wlan_wpa2_engine_state(&engine) ==
	    WLAN_WPA2_STATE_MESSAGE_4_TX);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U);
	assert(memcmp(fake.pairwise_key, ptk + 32U, 16U) == 0);
	assert(memcmp(fake.group_key, gtk, sizeof(gtk)) == 0 &&
	    fake.group_index == 2U &&
	    fake.group_receive_pn == UINT64_C(0x00000504030201));
	assert(fake.authorize_on_calls == 0U);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 && parsed.message == WLAN_WPA2_EAPOL_MESSAGE_4 &&
	    parsed.replay_counter == 19U &&
	    eapol_mic_matches(fake.frame, fake.frame_length, ptk));
	/* M3 while its original M4 is pending neither reinstalls nor resubmits. */
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m3, m3_length, 12U) == EALREADY);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U);
	assert(fake.authorize_on_calls == 0U);
	/* A higher M3 replay is semantically revalidated and replaces M4,
	 * without rewriting either hardware key or receive PN. */
	superseded_cookie = captured_cookie(&fake);
	m3_length = message_3(m3, 20U, anonce, ptk, gtk, 2U,
	    UINT64_C(0x00000504030211));
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m3, m3_length, 13U) == 0);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.pn_advance_calls == 1U &&
	    fake.group_receive_pn == UINT64_C(0x00000504030211));
	assert(wlan_wpa2_engine_report_tx(&engine, 91U, superseded_cookie,
	    1, 0, 13U) == ESTALE);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 && parsed.message == WLAN_WPA2_EAPOL_MESSAGE_4 &&
	    parsed.replay_counter == 20U &&
	    eapol_mic_matches(fake.frame, fake.frame_length, ptk));
	assert(report_captured(&engine, &fake, 1, 0, 14U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED);
	assert(fake.authorize_on_calls == 1U);

	/* A peer retransmission after authorization gets the same M4, no keys. */
	eapol_before = fake.eapol_tx_calls;
	assert(wlan_wpa2_engine_receive_eapol(&engine, 91U, bssid, station,
	    m3, m3_length, 1001U) == 0);
	assert(wlan_wpa2_engine_state(&engine) ==
	    WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX);
	assert(fake.eapol_tx_calls == eapol_before + 1U &&
	    fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.authorize_on_calls == 1U);
	assert(report_captured(&engine, &fake, 1, 0, 1002U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED);
	assert(fake.authorize_on_calls == 1U);

	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(fake.authorize_off_calls == 1U && fake.pair_delete_calls == 1U &&
	    fake.group_delete_calls == 1U && fake.association_clear_calls == 1U &&
	    fake.radio_stop_calls == 1U);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_address_entropy_and_nonce_failures(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_profile profile;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t wrong[WLAN_WPA2_MAC_LENGTH];
	uint8_t response[64];
	size_t length;

	init_and_reach_message_1(&engine, &fake, 92U);
	fill_bytes(anonce, sizeof(anonce), 0x31U);
	length = message_1(m1, 1U, anonce);
	memcpy(wrong, bssid, sizeof(wrong));
	wrong[5] ^= 1U;
	assert(wlan_wpa2_engine_receive_eapol(&engine, 92U, wrong, station,
	    m1, length, 6U) == EACCES);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));

	memset(&fake, 0, sizeof(fake));
	fake.fail_entropy = 1;
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	profile = test_profile(1000U);
	assert(wlan_wpa2_engine_start(&engine, 93U, &profile, 1U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 2U) == 0);
	length = authentication_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 93U, response,
	    length, 3U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 4U) == 0);
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 93U, response,
	    length, 5U) == 0);
	length = message_1(m1, 1U, anonce);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 93U, bssid, station,
	    m1, length, 6U) == EIO);
	assert(fake.entropy_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
}

static void
test_unrelated_management_is_benign(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t response[64];
	size_t length;

	memset(&fake, 0, sizeof(fake));
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	{
		struct wlan_wpa2_profile profile = test_profile(1000U);

		assert(wlan_wpa2_engine_start(&engine, 94U, &profile, 1U) == 0);
	}
	assert(report_captured(&engine, &fake, 1, 0, 2U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTH_RESPONSE);

	/* A valid frame of the wrong subtype, an action frame, and a response for
	 * another station must not consume the transaction or run cleanup. */
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 3U) == ESTALE);
	response[0U] = 0xd0U;
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 3U) == ESTALE);
	length = authentication_response(response, 0U);
	response[4U] ^= 2U;
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 3U) == ESTALE);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTH_RESPONSE);
	assert(fake.radio_stop_calls == 0U && fake.management_tx_calls == 1U);

	response[4U] ^= 2U;
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 3U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 4U) == 0);
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 5U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_1);
	length = authentication_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 94U, response,
	    length, 6U) == ESTALE);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_1);
	assert(fake.radio_stop_calls == 0U);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
}

static void
test_message_3_validation_and_rollback(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t wrong_anonce[WLAN_WPA2_NONCE_LENGTH];
	size_t length;

	fill_bytes(gtk, sizeof(gtk), 0xa1U);
	(void)reach_message_3(&engine, &fake, 94U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	m3[81U] ^= 0x80U;
	assert(wlan_wpa2_engine_receive_eapol(&engine, 94U, bssid, station,
	    m3, length, 8U) == EACCES);
	assert(fake.pair_install_calls == 0U && fake.group_install_calls == 0U &&
	    fake.authorize_on_calls == 0U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 95U, anonce, ptk, m1);
	length = message_3(m3, 17U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 95U, bssid, station,
	    m3, length, 8U) == EACCES);
	assert(fake.pair_install_calls == 0U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 96U, anonce, ptk, m1);
	memcpy(wrong_anonce, anonce, sizeof(wrong_anonce));
	wrong_anonce[0] ^= 1U;
	length = message_3(m3, 18U, wrong_anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 96U, bssid, station,
	    m3, length, 8U) == EACCES);
	assert(fake.pair_install_calls == 0U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 97U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	m3[length - 1U] ^= 1U;
	eapol_sign(m3, length, ptk);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 97U, bssid, station,
	    m3, length, 8U) == EACCES);
	assert(fake.pair_install_calls == 0U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 104U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 104U, bssid, station,
	    m3, length, 8U) == 0);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U);
	m3[length - 1U] ^= 1U;
	eapol_sign(m3, length, ptk);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 104U, bssid, station,
	    m3, length, 9U) == EACCES);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.pair_delete_calls == 1U && fake.group_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 105U, anonce, ptk, m1);
	fake.fail_group_install = 1;
	length = message_3(m3, 18U, anonce, ptk, gtk, 3U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 105U, bssid, station,
	    m3, length, 8U) == EIO);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.pair_delete_calls == 1U && fake.group_delete_calls == 1U &&
	    fake.authorize_on_calls == 0U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	/* A higher replay may monotonically advance RSC without reinstalling;
	 * a later rollback remains forbidden. */
	(void)reach_message_3(&engine, &fake, 112U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 7U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 112U, bssid, station,
	    m3, length, 8U) == 0);
	length = message_3(m3, 19U, anonce, ptk, gtk, 1U, 8U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 112U, bssid, station,
	    m3, length, 9U) == 0);
	assert(fake.pn_advance_calls == 1U && fake.group_receive_pn == 8U &&
	    fake.pair_install_calls == 1U && fake.group_install_calls == 1U);
	length = message_3(m3, 20U, anonce, ptk, gtk, 1U, 7U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 112U, bssid, station,
	    m3, length, 10U) == EACCES);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.pair_delete_calls == 1U && fake.group_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 113U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 7U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 113U, bssid, station,
	    m3, length, 8U) == 0);
	gtk[0] ^= 1U;
	length = message_3(m3, 19U, anonce, ptk, gtk, 1U, 7U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 113U, bssid, station,
	    m3, length, 9U) == EACCES);
	assert(fake.pair_install_calls == 1U && fake.group_install_calls == 1U &&
	    fake.pair_delete_calls == 1U && fake.group_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_m4_ack_gate_nack_and_timeout(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;
	unsigned attempt;

	fill_bytes(gtk, sizeof(gtk), 0xb1U);
	(void)reach_message_3(&engine, &fake, 98U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 98U, bssid, station,
	    m3, length, 8U) == 0);
	for (attempt = 0U; attempt < WLAN_WPA2_RETRY_MAX; attempt++) {
		assert(fake.authorize_on_calls == 0U);
		assert(report_captured(&engine, &fake, 0, EIO,
		    9U + attempt) == 0);
		assert(wlan_wpa2_engine_state(&engine) ==
		    WLAN_WPA2_STATE_MESSAGE_4_TX);
	}
	assert(report_captured(&engine, &fake, 0, EIO, 20U) == ETIMEDOUT);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED &&
	    fake.authorize_on_calls == 0U && fake.pair_delete_calls == 1U &&
	    fake.group_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	init_and_reach_message_1(&engine, &fake, 99U);
	assert(wlan_wpa2_engine_next_deadline(&engine) != 0U);
	assert(wlan_wpa2_engine_timer(&engine, 1000U) == ETIMEDOUT);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED &&
	    wlan_wpa2_engine_last_error(&engine) == ETIMEDOUT &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_authorize_callback_failure(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;

	fill_bytes(gtk, sizeof(gtk), 0xc1U);
	(void)reach_message_3(&engine, &fake, 100U, anonce, ptk, m1);
	fake.fail_authorize = 1;
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 100U, bssid, station,
	    m3, length, 8U) == 0);
	assert(fake.authorize_on_calls == 0U);
	assert(report_captured(&engine, &fake, 1, 0, 9U) == EIO);
	assert(fake.authorize_on_calls == 1U && fake.authorize_off_calls == 1U &&
	    fake.pair_delete_calls == 1U && fake.group_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_checked_cleanup_retry(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;

	/* A radio barrier failure retains the configured generation for retry,
	 * but the already-retired association no longer pins secrets. */
	init_and_reach_message_1(&engine, &fake, 106U);
	fake.fail_radio_stop = 1;
	assert(wlan_wpa2_engine_stop(&engine) == EBUSY);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED &&
	    engine.configured && !engine.associated &&
	    fake.association_clear_calls == 1U && fake.radio_stop_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	fake.fail_radio_stop = 0;
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_IDLE &&
	    fake.association_clear_calls == 1U && fake.radio_stop_calls == 2U);

	/* Never retire the PTK or lower layers while GTK deletion is unproven. */
	fill_bytes(gtk, sizeof(gtk), 0xd1U);
	(void)reach_message_3(&engine, &fake, 107U, anonce, ptk, m1);
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 107U, bssid, station,
	    m3, length, 8U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 9U) == 0);
	fake.fail_group_delete = 1;
	assert(wlan_wpa2_engine_stop(&engine) == EBUSY);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED &&
	    !engine.authorized && engine.group_installed &&
	    engine.pairwise_installed && engine.associated && engine.configured);
	assert(fake.authorize_off_calls == 1U && fake.group_delete_calls == 1U &&
	    fake.pair_delete_calls == 0U && fake.association_clear_calls == 0U &&
	    fake.radio_stop_calls == 0U);
	fake.fail_group_delete = 0;
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(fake.authorize_off_calls == 1U && fake.group_delete_calls == 2U &&
	    fake.pair_delete_calls == 1U && fake.association_clear_calls == 1U &&
	    fake.radio_stop_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_uncertain_callback_rollback(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_profile profile = test_profile(1000U);
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t response[64];
	size_t length;

	/* A start error does not prove that the radio was left untouched. */
	memset(&fake, 0, sizeof(fake));
	fake.fail_radio_start = 1;
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	assert(wlan_wpa2_engine_start(&engine, 108U, &profile, 1U) == EIO);
	assert(fake.radio_start_calls == 1U && fake.radio_stop_calls == 1U &&
	    !engine.configured && wlan_wpa2_engine_test_secrets_clear(&engine));

	/* Likewise, a rejected association is explicitly cleared before the
	 * radio generation is retired. */
	memset(&fake, 0, sizeof(fake));
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	assert(wlan_wpa2_engine_start(&engine, 109U, &profile, 1U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 2U) == 0);
	length = authentication_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 109U, response,
	    length, 3U) == 0);
	assert(report_captured(&engine, &fake, 1, 0, 4U) == 0);
	fake.fail_association_set = 1;
	length = association_response(response, 0U);
	assert(wlan_wpa2_engine_receive_management(&engine, 109U, response,
	    length, 5U) == EIO);
	assert(fake.association_set_calls == 1U &&
	    fake.association_clear_calls == 1U && fake.radio_stop_calls == 1U &&
	    !engine.associated && !engine.configured &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	/* Key-programming errors are uncertain until an idempotent delete
	 * succeeds; no lower cleanup barrier may overtake a retained slot. */
	fill_bytes(gtk, sizeof(gtk), 0xe1U);
	(void)reach_message_3(&engine, &fake, 110U, anonce, ptk, m1);
	fake.fail_pair_install = 1;
	fake.fail_pair_delete = 1;
	length = message_3(m3, 18U, anonce, ptk, gtk, 1U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 110U, bssid, station,
	    m3, length, 8U) == EIO);
	assert(engine.pairwise_installed && engine.associated &&
	    engine.configured && fake.pair_delete_calls == 1U &&
	    fake.association_clear_calls == 0U && fake.radio_stop_calls == 0U &&
	    !wlan_wpa2_engine_test_secrets_clear(&engine));
	fake.fail_pair_delete = 0;
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(fake.pair_delete_calls == 2U &&
	    fake.association_clear_calls == 1U && fake.radio_stop_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));

	(void)reach_message_3(&engine, &fake, 111U, anonce, ptk, m1);
	fake.fail_group_install = 1;
	fake.fail_group_delete = 1;
	length = message_3(m3, 18U, anonce, ptk, gtk, 2U, 0U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 111U, bssid, station,
	    m3, length, 8U) == EIO);
	assert(engine.group_installed && engine.pairwise_installed &&
	    fake.group_delete_calls == 1U && fake.pair_delete_calls == 0U &&
	    !wlan_wpa2_engine_test_secrets_clear(&engine));
	fake.fail_group_delete = 0;
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(fake.group_delete_calls == 2U && fake.pair_delete_calls == 1U &&
	    wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_group_rekey_stage_retry_and_replay(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_eapol_key parsed;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t replacement[WLAN_WPA2_GTK_LENGTH];
	uint8_t g1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t pair_m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;
	uint64_t pairwise_generation;
	uint64_t group_generation;
	unsigned install_calls;
	unsigned activate_calls;
	unsigned delete_calls;

	reach_authorized(&engine, &fake, 200U, anonce, ptk, gtk, 1U);
	pairwise_generation = engine.key_generation;
	group_generation = engine.group_key_generation;
	fill_bytes(replacement, sizeof(replacement), 0x41U);
	fake.group_install_busy_remaining = 1U;
	length = group_message_1(g1, 30U, ptk, replacement, 2U, 10U);
	install_calls = fake.group_install_calls;
	assert(wlan_wpa2_engine_receive_eapol(&engine, 200U, bssid, station,
	    g1, length, 20U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_GROUP_STAGE &&
	    engine.group_key_generation == group_generation &&
	    engine.pending_group_installed &&
	    !engine.pending_group_programmed && engine.authorized);
	assert(fake.group_install_calls == install_calls + 1U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 200U, bssid, station,
	    g1, length, 20U) == EALREADY);
	assert(wlan_wpa2_engine_timer(&engine, 21U) == 0);
	assert(wlan_wpa2_engine_state(&engine) ==
	    WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX &&
	    fake.group_install_calls == install_calls + 2U);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 &&
	    parsed.message == WLAN_WPA2_EAPOL_GROUP_MESSAGE_2);

	/* An authenticator may advance Replay Counter and RSC for the same
	 * staged GTK.  The engine raises the PN floor and rebuilds G2, but does
	 * not allocate or install another generation. */
	length = group_message_1(g1, 31U, ptk, replacement, 2U, 11U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 200U, bssid, station,
	    g1, length, 22U) == 0);
	assert(fake.group_install_calls == install_calls + 2U &&
	    fake.pn_advance_calls == 1U);
	assert(report_captured(&engine, &fake, 1, 0, 23U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED &&
	    engine.key_generation == pairwise_generation &&
	    engine.group_key_generation != group_generation &&
	    engine.group_receive_packet_number == 11U &&
	    fake.activate_calls == 1U && fake.group_delete_calls == 1U);

	/* Same-index GTK replacement is valid and crosses the same stage/activate
	 * contract; it is not rejected merely because the index is reused. */
	memcpy(gtk, replacement, sizeof(gtk));
	fill_bytes(replacement, sizeof(replacement), 0x61U);
	length = group_message_1(g1, 32U, ptk, replacement, 2U, 12U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 200U, bssid, station,
	    g1, length, 24U) == 0);
	activate_calls = fake.activate_calls;
	delete_calls = fake.group_delete_calls;
	assert(report_captured(&engine, &fake, 1, 0, 25U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED &&
	    fake.activate_calls == activate_calls + 1U &&
	    fake.group_delete_calls == delete_calls + 1U);

	/* Pairwise rekey replay must exceed every preceding EAPOL-Key replay,
	 * including the latest group-key exchange. */
	fill_bytes(anonce, sizeof(anonce), 0x31U);
	length = message_1(pair_m1, 32U, anonce);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 200U, bssid, station,
	    pair_m1, length, 26U) == EACCES);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_pairwise_rekey_stage_and_retire_retry(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_eapol_key parsed;
	uint8_t old_anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t new_anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t old_ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t new_ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t replacement[WLAN_WPA2_GTK_LENGTH];
	uint8_t m1[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t m3[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;
	uint64_t old_pairwise_generation;
	uint64_t old_group_generation;
	unsigned activate_calls;
	unsigned group_delete_calls;

	reach_authorized(&engine, &fake, 201U, old_anonce, old_ptk, gtk, 1U);
	old_pairwise_generation = engine.key_generation;
	old_group_generation = engine.group_key_generation;
	fill_bytes(new_anonce, sizeof(new_anonce), 0x51U);
	length = message_1(m1, 40U, new_anonce);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 201U, bssid, station,
	    m1, length, 20U) == 0);
	assert(!engine.authorized && engine.pairwise_rekey &&
	    fake.authorize_off_calls == 1U);
	assert(wlan_wpa2_eapol_key_parse(fake.frame, fake.frame_length,
	    &parsed) == 0 && parsed.message == WLAN_WPA2_EAPOL_MESSAGE_2);
	derive_test_ptk(parsed.nonce, new_anonce, new_ptk);
	assert(report_captured(&engine, &fake, 1, 0, 21U) == 0);

	fill_bytes(replacement, sizeof(replacement), 0x71U);
	fake.pair_install_busy_remaining = 1U;
	length = message_3(m3, 41U, new_anonce, new_ptk, replacement, 1U, 20U);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 201U, bssid, station,
	    m3, length, 22U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_PAIRWISE_STAGE &&
	    engine.key_generation == old_pairwise_generation &&
	    engine.group_key_generation == old_group_generation &&
	    engine.pending_pairwise_installed &&
	    !engine.pending_pairwise_programmed);
	assert(wlan_wpa2_engine_receive_eapol(&engine, 201U, bssid, station,
	    m3, length, 22U) == EALREADY);
	assert(wlan_wpa2_engine_timer(&engine, 23U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_MESSAGE_4_TX &&
	    engine.key_generation == old_pairwise_generation &&
	    engine.pending_pairwise_programmed && engine.pending_group_programmed);

	/* Activation succeeds, old GTK retirement succeeds, then old PTK
	 * retirement reports EBUSY.  The next timer resumes at the one incomplete
	 * step: it must not activate again or delete the old GTK again. */
	fake.pair_delete_busy_remaining = 1U;
	assert(report_captured(&engine, &fake, 1, 0, 24U) == 0);
	assert(wlan_wpa2_engine_state(&engine) ==
	    WLAN_WPA2_STATE_PAIRWISE_ACTIVATE);
	activate_calls = fake.activate_calls;
	group_delete_calls = fake.group_delete_calls;
	assert(activate_calls == 1U && group_delete_calls == 1U &&
	    fake.pair_delete_calls == 1U);
	assert(wlan_wpa2_engine_timer(&engine, 25U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTHORIZED &&
	    fake.activate_calls == activate_calls &&
	    fake.group_delete_calls == group_delete_calls &&
	    fake.pair_delete_calls == 2U && fake.authorize_on_calls == 2U &&
	    engine.key_generation != old_pairwise_generation &&
	    engine.group_key_generation != old_group_generation &&
	    !engine.pending_pairwise_installed &&
	    !engine.pending_group_installed);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(old_ptk, sizeof(old_ptk));
	wlan_crypto_erase(new_ptk, sizeof(new_ptk));
}

static void
test_reconnect_cleanup_retry_and_fresh_generation(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint64_t old_key_generation;

	reach_authorized(&engine, &fake, 202U, anonce, ptk, gtk, 1U);
	old_key_generation = engine.key_generation;
	fake.group_delete_busy_remaining = 1U;
	assert(wlan_wpa2_engine_link_lost(&engine, ECONNRESET) == EBUSY);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_FAILED &&
	    engine.group_installed && wlan_wpa2_engine_can_reconnect(&engine));
	assert(wlan_wpa2_engine_link_lost(&engine, ECONNRESET) == 0);
	assert(wlan_wpa2_engine_state(&engine) ==
	    WLAN_WPA2_STATE_RECONNECT_WAIT && !engine.configured &&
	    !engine.associated && !engine.pairwise_installed &&
	    !engine.group_installed && wlan_wpa2_engine_can_reconnect(&engine));
	assert(wlan_wpa2_engine_reconnect(&engine, 203U, 1000U, 30U) == 0);
	assert(wlan_wpa2_engine_state(&engine) == WLAN_WPA2_STATE_AUTH_TX &&
	    engine.generation == 203U &&
	    engine.key_generation > old_key_generation &&
	    fake.radio_start_calls == 2U);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_group_rekey_100_iterations(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t replacement[WLAN_WPA2_GTK_LENGTH];
	uint8_t g1[WLAN_WPA2_EAPOL_FRAME_MAX];
	size_t length;
	unsigned iteration;

	reach_authorized(&engine, &fake, 204U, anonce, ptk, gtk, 1U);
	for (iteration = 0U; iteration < 100U; iteration++) {
		fill_bytes(replacement, sizeof(replacement),
		    (uint8_t)(0x20U + iteration));
		length = group_message_1(g1, 30U + iteration, ptk, replacement,
		    (uint8_t)(1U + (iteration % 3U)), 20U + iteration);
		assert(wlan_wpa2_engine_receive_eapol(&engine, 204U, bssid,
		    station, g1, length, 20U + iteration * 2U) == 0);
		assert(report_captured(&engine, &fake, 1, 0,
		    21U + iteration * 2U) == 0);
		assert(wlan_wpa2_engine_state(&engine) ==
		    WLAN_WPA2_STATE_AUTHORIZED && engine.authorized &&
		    !engine.pending_group_installed &&
		    !engine.pending_group_programmed &&
		    !engine.activation_complete);
	}
	assert(fake.activate_calls == 100U &&
	    fake.group_delete_calls == 100U);
	assert(wlan_wpa2_engine_stop(&engine) == 0);
	assert(wlan_wpa2_engine_test_secrets_clear(&engine));
	wlan_crypto_erase(ptk, sizeof(ptk));
}

static void
test_argument_contract(void)
{
	struct wlan_wpa2_engine engine;
	struct fake_radio fake;
	struct wlan_wpa2_profile profile = test_profile(1000U);
	struct wlan_wpa2_ops incomplete = fake_ops;

	incomplete.transmit = NULL;
	assert(wlan_wpa2_engine_init(&engine, &incomplete, &fake) == EINVAL);
	assert(wlan_wpa2_engine_init(&engine, &fake_ops, &fake) == 0);
	profile.passphrase_length = 7U;
	assert(wlan_wpa2_engine_start(&engine, 1U, &profile, 1U) == EINVAL);
	profile = test_profile(1U);
	assert(wlan_wpa2_engine_start(&engine, 1U, &profile, 1U) == EINVAL);
	assert(wlan_wpa2_engine_receive_management(NULL, 0U, NULL, 0U, 0U) ==
	    EINVAL);
	assert(wlan_wpa2_engine_report_tx(&engine, 1U, 1U, 2, 0, 1U) ==
	    EINVAL);
	assert(wlan_wpa2_engine_timer(NULL, 0U) == EINVAL);
}

int
main(void)
{
	test_argument_contract();
	test_peer_response_precedes_tx_report();
	test_complete_handshake_and_retransmission();
	test_address_entropy_and_nonce_failures();
	test_unrelated_management_is_benign();
	test_message_3_validation_and_rollback();
	test_m4_ack_gate_nack_and_timeout();
	test_authorize_callback_failure();
	test_checked_cleanup_retry();
	test_uncertain_callback_rollback();
	test_group_rekey_stage_retry_and_replay();
	test_pairwise_rekey_stage_and_retire_retry();
	test_reconnect_cleanup_retry_and_fresh_generation();
	test_group_rekey_100_iterations();
	return 0;
}
