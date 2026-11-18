/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * networkd's sleep (ws052-p010, plan/ws052/phase007/phase.md section 2).
 *
 * Before the machine sleeps in S0 idle, sessiond asks networkd to turn the
 * Wi-Fi radios off (SLEEP_PREPARE): a radio that is on stops the sleep
 * (the Wi-Fi driver refuses its suspend).  networkd records the Wi-Fi
 * policy's state, retires the connection and stops the radios without
 * changing the policy, and stays asleep until SLEEP_END, when it takes the
 * recorded policy up again; it ends its sleep by itself after
 * NETWORKD_SLEEP_SAFETY_SECONDS if no SLEEP_END comes (sessiond's helper
 * died).  While asleep it refuses the Wi-Fi changes and the confirmed
 * transactions, and serves the rest.
 *
 * This part knows only the record and its rules, nothing of the radios or
 * the sockets, so the host tests run it alone.
 */

#ifndef KERN_NETWORKD_SLEEP_STATE_H
#define KERN_NETWORKD_SLEEP_STATE_H

#include "userland/base/networkd/managed-wlan-state.h"

#include <stdint.h>

/* How long networkd stays asleep without SLEEP_END (seconds). */
#define NETWORKD_SLEEP_SAFETY_SECONDS	(10U * 60U)

/* What SLEEP_END takes up again. */
enum networkd_sleep_resume {
	/* Nothing: the radios stay down (Wi-Fi was off, or networkd was not asleep). */
	NETWORKD_SLEEP_RESUME_NONE,
	/* The radios up and the automatic search for the saved networks. */
	NETWORKD_SLEEP_RESUME_SEARCH,
	/* The radios up, and no connection of networkd's own. */
	NETWORKD_SLEEP_RESUME_MANUAL
};

/*
 * The sleep: whether networkd is asleep, the Wi-Fi policy's state when it
 * fell asleep, and when it ends its sleep by itself (CLOCK_MONOTONIC
 * microseconds).
 */
struct networkd_sleep {
	unsigned asleep;
	enum networkd_managed_wlan_state recorded;
	uint64_t deadline_us;
};

void networkd_sleep_init(struct networkd_sleep *record);
int networkd_sleep_begin(struct networkd_sleep *record, enum networkd_managed_wlan_state state, enum networkd_managed_wlan_state retire_target, uint64_t now_us);
enum networkd_sleep_resume networkd_sleep_end(struct networkd_sleep *record);
int networkd_sleep_due(const struct networkd_sleep *record, uint64_t now_us);
int networkd_sleep_poll_timeout(const struct networkd_sleep *record, uint64_t now_us);
int networkd_sleep_admits(const struct networkd_sleep *record, uint32_t opcode);

#endif
