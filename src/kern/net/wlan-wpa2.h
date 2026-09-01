/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_WLAN_WPA2_H
#define ZEDBSD_KERN_NET_WLAN_WPA2_H

#include <stddef.h>
#include <stdint.h>

#include "wlan-crypto.h"
#include "wlan-wpa2-codec.h"

#define WLAN_WPA2_PMK_LENGTH 32U
#define WLAN_WPA2_PTK_LENGTH 64U
#define WLAN_WPA2_KCK_LENGTH 16U
#define WLAN_WPA2_KEK_LENGTH 16U
#define WLAN_WPA2_TK_LENGTH  16U
#define WLAN_WPA2_SSID_MAX   32U
#define WLAN_WPA2_PASSPHRASE_MIN 8U
#define WLAN_WPA2_PASSPHRASE_MAX 63U
#define WLAN_WPA2_FRAME_MAX 2304U
#define WLAN_WPA2_EAPOL_FRAME_MAX \
	(99U + WLAN_WPA2_EAPOL_KEY_DATA_MAX)
#define WLAN_WPA2_RETRY_MAX 5U

enum wlan_wpa2_state {
	WLAN_WPA2_STATE_IDLE = 0,
	WLAN_WPA2_STATE_AUTH_TX,
	WLAN_WPA2_STATE_AUTH_RESPONSE,
	WLAN_WPA2_STATE_ASSOC_TX,
	WLAN_WPA2_STATE_ASSOC_RESPONSE,
	WLAN_WPA2_STATE_MESSAGE_1,
	WLAN_WPA2_STATE_MESSAGE_2_TX,
	WLAN_WPA2_STATE_MESSAGE_3,
	WLAN_WPA2_STATE_MESSAGE_4_TX,
	WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX,
	WLAN_WPA2_STATE_AUTHORIZED,
	WLAN_WPA2_STATE_FAILED
};

enum wlan_wpa2_tx_kind {
	/* A complete, unencrypted IEEE 802.11 management MPDU. */
	WLAN_WPA2_TX_MANAGEMENT = 1,
	/* An EAPOL payload beginning at the 802.1X protocol-version octet. */
	WLAN_WPA2_TX_EAPOL = 2
};

enum wlan_wpa2_key_kind {
	WLAN_WPA2_KEY_PAIRWISE = 1,
	WLAN_WPA2_KEY_GROUP = 2
};

struct wlan_wpa2_profile {
	uint8_t station[WLAN_WPA2_MAC_LENGTH];
	uint8_t bssid[WLAN_WPA2_MAC_LENGTH];
	uint8_t ssid[WLAN_WPA2_SSID_MAX];
	size_t ssid_length;
	uint8_t rates[WLAN_WPA2_RATE_MAX];
	size_t rate_count;
	uint32_t channel;
	uint16_t capability;
	uint16_t listen_interval;
	uint16_t initial_sequence;
	const uint8_t *passphrase;
	size_t passphrase_length;
	/* Both values use the caller's one monotonic tick domain. */
	uint64_t total_deadline_ticks;
	uint64_t transition_timeout_ticks;
};

/*
 * transmit() only queues a private copy and returns.  Success does not mean
 * that the peer acknowledged the frame.  The driver must later report the
 * exact cookie through wlan_wpa2_engine_report_tx().  It may retain neither
 * the frame nor destination pointer after returning, and it must not report a
 * completion reentrantly from inside transmit().  All callbacks run in the
 * caller's serialized WLAN control context, never from a hard-IRQ handler.
 *
 * Every mutating callback is a checked barrier.  A nonzero return may leave
 * the requested hardware state uncertain, so the corresponding inverse
 * callback must be idempotent and must return zero only once that state is
 * known to be absent.  The engine retains the generation and key material
 * until those inverse barriers have succeeded.
 */
struct wlan_wpa2_ops {
	int (*entropy_fill)(void *context, void *buffer, size_t length);
	int (*radio_start)(void *context, uint64_t generation,
		const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint32_t channel,
		uint64_t deadline_ticks);
	int (*transmit)(void *context, uint64_t generation, uint64_t cookie,
		enum wlan_wpa2_tx_kind kind,
		const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
		const uint8_t *frame, size_t length, uint64_t deadline_ticks);
	int (*association_set)(void *context, uint64_t generation,
		const uint8_t bssid[WLAN_WPA2_MAC_LENGTH], uint16_t aid);
	int (*association_clear)(void *context, uint64_t generation);
	int (*key_install)(void *context, uint64_t generation,
		enum wlan_wpa2_key_kind kind, uint8_t key_index,
		const uint8_t key[WLAN_WPA2_TK_LENGTH],
		uint64_t key_generation, uint64_t receive_packet_number);
	/* Monotonically raises the existing slot's software/hardware receive
	 * floor.  It must never rewrite the key or lower/reset a live PN. */
	int (*key_receive_pn_advance)(void *context, uint64_t generation,
		enum wlan_wpa2_key_kind kind, uint8_t key_index,
		uint64_t key_generation, uint64_t receive_packet_number);
	int (*key_delete)(void *context, uint64_t generation,
		enum wlan_wpa2_key_kind kind, uint8_t key_index,
		uint64_t key_generation);
	int (*authorized_set)(void *context, uint64_t generation,
		int authorized);
	int (*radio_stop)(void *context, uint64_t generation);
};

/* This is a private kernel object, deliberately caller-allocated. */
struct wlan_wpa2_engine {
	const struct wlan_wpa2_ops *ops;
	void *callback_context;
	struct wlan_wpa2_profile profile;
	enum wlan_wpa2_state state;
	enum wlan_wpa2_tx_kind tx_kind;
	uint64_t generation;
	uint64_t key_generation;
	uint64_t step_deadline_ticks;
	uint64_t tx_cookie_next;
	uint64_t tx_cookie_active;
	uint64_t message_1_replay_counter;
	uint64_t message_3_replay_counter;
	uint64_t group_receive_packet_number;
	uint16_t next_sequence;
	uint16_t aid;
	uint8_t retry_count;
	uint8_t protocol_version;
	uint8_t gtk_index;
	uint8_t configured;
	uint8_t associated;
	uint8_t pairwise_installed;
	uint8_t group_installed;
	uint8_t authorized;
	uint8_t message_3_accepted;
	int last_error;
	size_t tx_length;
	uint8_t tx_destination[WLAN_WPA2_MAC_LENGTH];
	uint8_t tx_frame[WLAN_WPA2_FRAME_MAX];
	uint8_t pmk[WLAN_WPA2_PMK_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t snonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t message_3_digest[WLAN_SHA1_DIGEST_SIZE];
};

int wlan_wpa2_engine_init(struct wlan_wpa2_engine *engine,
	const struct wlan_wpa2_ops *ops, void *callback_context);
int wlan_wpa2_engine_start(struct wlan_wpa2_engine *engine,
	uint64_t generation, const struct wlan_wpa2_profile *profile,
	uint64_t now_ticks);
int wlan_wpa2_engine_receive_management(struct wlan_wpa2_engine *engine,
	uint64_t generation, const uint8_t *frame, size_t length,
	uint64_t now_ticks);
int wlan_wpa2_engine_receive_eapol(struct wlan_wpa2_engine *engine,
	uint64_t generation,
	const uint8_t source[WLAN_WPA2_MAC_LENGTH],
	const uint8_t destination[WLAN_WPA2_MAC_LENGTH],
	const uint8_t *frame, size_t length, uint64_t now_ticks);
int wlan_wpa2_engine_report_tx(struct wlan_wpa2_engine *engine,
	uint64_t generation, uint64_t cookie, int acknowledged, int error,
	uint64_t now_ticks);
int wlan_wpa2_engine_timer(struct wlan_wpa2_engine *engine,
	uint64_t now_ticks);
int wlan_wpa2_engine_stop(struct wlan_wpa2_engine *engine);

enum wlan_wpa2_state wlan_wpa2_engine_state(
	const struct wlan_wpa2_engine *engine);
int wlan_wpa2_engine_last_error(const struct wlan_wpa2_engine *engine);
uint64_t wlan_wpa2_engine_next_deadline(
	const struct wlan_wpa2_engine *engine);

#ifdef WLAN_WPA2_TESTING
int wlan_wpa2_engine_test_secrets_clear(
	const struct wlan_wpa2_engine *engine);
#endif

#endif
