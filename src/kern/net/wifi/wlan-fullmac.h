/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The FullMAC connection engine.
 *
 * A FullMAC radio's firmware runs the association and the WPA2 supplicant
 * itself.  This engine is the common core's record of one such connection:
 * it derives the PMK, hands the connection to the radio, maps the firmware's
 * ASSOCIATED and AUTHORIZED events into its own state, enforces the overall
 * connection deadline, and undoes the controlled port and the radio
 * connection as checked barriers.  It has the same shape as the WPA2 engine
 * so that the station drives either one the same way, and like that engine it
 * never blocks or touches a device itself.
 */

#ifndef KERN_KERN_NET_WLAN_FULLMAC_H
#define KERN_KERN_NET_WLAN_FULLMAC_H

#include <stddef.h>
#include <stdint.h>

#include "kern/net/wlan.h"

enum wlan_fullmac_state {
	WLAN_FULLMAC_STATE_IDLE = 0,
	/* The connection request is with the driver and its firmware. */
	WLAN_FULLMAC_STATE_CONNECTING,
	/* The firmware has joined; its supplicant runs the handshakes. */
	WLAN_FULLMAC_STATE_ASSOCIATED,
	/* The firmware has the keys, and the controlled port is open. */
	WLAN_FULLMAC_STATE_AUTHORIZED,
	WLAN_FULLMAC_STATE_FAILED
};

/*
 * One connection the station asks the engine to make.
 *
 * The passphrase is only read during wlan_fullmac_engine_start; the caller
 * keeps and erases its own copy.
 */
struct wlan_fullmac_profile {
	struct wlan_bss_record bss;
	const uint8_t *passphrase;
	size_t passphrase_length;
	/* The overall connection deadline, in the caller's tick domain. */
	uint64_t total_deadline_ticks;
};

/*
 * The station's side of the engine.
 *
 * All three run in the caller's serialized WLAN control context.  connect
 * hands the request to the radio and returns at once; the request is erased
 * when it returns.  authorized_set opens or closes the controlled port, and
 * radio_stop retires the radio connection.  The two inverse callbacks
 * (authorized_set with 0, and radio_stop) are checked barriers: they must be
 * idempotent and return zero only once the state is known to be absent.
 */
struct wlan_fullmac_ops {
	int (*connect)(void *context,
		       uint64_t generation,
		       const struct wlan_fullmac_connect_request *request);

	int (*authorized_set)(void *context,
			      uint64_t generation,
			      int authorized);

	int (*radio_stop)(void *context,
			  uint64_t generation);
};

/*
 * The record of one station's FullMAC connection.
 *
 * It is a private kernel object, deliberately caller-allocated inside the
 * station.  It holds no secret after wlan_fullmac_engine_start returns: the
 * request scratch below carries the PMK only across the connect callback.
 */
struct wlan_fullmac_engine {
	const struct wlan_fullmac_ops *ops;
	void *callback_context;
	enum wlan_fullmac_state state;
	uint64_t generation;
	uint64_t total_deadline_ticks;
	int last_error;
	/* connect was called, so radio_stop is owed even if connect failed. */
	uint8_t configured;
	/* authorized_set(1) was called, so authorized_set(0) is owed. */
	uint8_t authorized;
	uint8_t reserved[6];
	/* Holds the request only while connect runs; zero at every other time. */
	struct wlan_fullmac_connect_request request;
};

int
wlan_fullmac_engine_init(
	struct wlan_fullmac_engine *engine,
	const struct wlan_fullmac_ops *ops,
	void *callback_context);

int
wlan_fullmac_engine_start(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	const struct wlan_fullmac_profile *profile,
	uint64_t now_ticks);

int
wlan_fullmac_engine_report(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	enum wlan_fullmac_event event,
	uint64_t now_ticks);

int
wlan_fullmac_engine_fail(
	struct wlan_fullmac_engine *engine,
	uint64_t generation,
	int error);

int
wlan_fullmac_engine_timer(
	struct wlan_fullmac_engine *engine,
	uint64_t now_ticks);

int
wlan_fullmac_engine_stop(
	struct wlan_fullmac_engine *engine);

enum wlan_fullmac_state
wlan_fullmac_engine_state(
	const struct wlan_fullmac_engine *engine);

int
wlan_fullmac_engine_last_error(
	const struct wlan_fullmac_engine *engine);

uint64_t
wlan_fullmac_engine_next_deadline(
	const struct wlan_fullmac_engine *engine);

int
wlan_fullmac_engine_secrets_clear(
	const struct wlan_fullmac_engine *engine);

#endif
