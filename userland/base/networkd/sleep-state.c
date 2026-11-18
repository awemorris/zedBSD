/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The record of networkd's sleep and its rules (ws052-p010): what the
 * Wi-Fi policy was when networkd fell asleep, what SLEEP_END takes up
 * again, when the sleep ends by itself, and which requests are served
 * while it lasts.
 */

#include "userland/base/networkd/sleep-state.h"
#include "userland/base/net/protocol.h"

#include <limits.h>
#include <stddef.h>

static enum networkd_sleep_resume sleep_resume_of(enum networkd_managed_wlan_state state);

/*
 * Initializes a sleep record: networkd is awake.
 */
void
networkd_sleep_init(
	struct networkd_sleep *record)
{
	/* Awake, with nothing recorded. */
	record->asleep = 0U;
	record->recorded = NETWORKD_WLAN_DISABLED;
	record->deadline_us = 0U;
}

/*
 * Begins the sleep: records the Wi-Fi policy's state (the state a
 * retirement under way goes to, for one that is retiring) and when the
 * sleep ends by itself.  A second SLEEP_PREPARE while asleep keeps the
 * first record and only moves the end.  Returns 1 when the sleep began
 * (the caller turns the radios off), 0 when networkd was asleep already.
 */
int
networkd_sleep_begin(
	struct networkd_sleep *record,
	enum networkd_managed_wlan_state state,
	enum networkd_managed_wlan_state retire_target,
	uint64_t now_us)
{
	/* Asleep already: the first record stays, the end moves. */
	record->deadline_us = now_us + (uint64_t)NETWORKD_SLEEP_SAFETY_SECONDS * 1000000ULL;
	if (record->asleep)
		return 0;

	/* A retirement under way is recorded as the state it goes to. */
	record->recorded = state;
	if (state == NETWORKD_WLAN_RETIRING)
		record->recorded = retire_target;

	/* Succeeded: networkd is asleep from now on. */
	record->asleep = 1U;
	return 1;
}

/*
 * Ends the sleep and says what to take up again
 * (NETWORKD_SLEEP_RESUME_NONE when networkd was not asleep).
 */
enum networkd_sleep_resume
networkd_sleep_end(
	struct networkd_sleep *record)
{
	enum networkd_sleep_resume resume;

	/* Awake already: nothing to take up. */
	if (!record->asleep)
		return NETWORKD_SLEEP_RESUME_NONE;

	/* Awake from now on, with what the record asks for. */
	resume = sleep_resume_of(record->recorded);
	record->asleep = 0U;
	record->deadline_us = 0U;

	/* Succeeded: the resume the recorded policy asks for. */
	return resume;
}

/*
 * Tells whether the sleep has lasted past its end without SLEEP_END.
 */
int
networkd_sleep_due(
	const struct networkd_sleep *record,
	uint64_t now_us)
{
	/* Only a sleep has an end. */
	if (!record->asleep)
		return 0;

	/* Its end has come. */
	if (now_us >= record->deadline_us)
		return 1;

	/* Succeeded: the sleep goes on. */
	return 0;
}

/*
 * Gives how long the event loop may wait for the sleep's end, in
 * milliseconds (rounded up), or -1 while awake.
 */
int
networkd_sleep_poll_timeout(
	const struct networkd_sleep *record,
	uint64_t now_us)
{
	uint64_t milliseconds;

	/* Awake: nothing to wait for. */
	if (!record->asleep)
		return -1;

	/* The end has come. */
	if (now_us >= record->deadline_us)
		return 0;

	/* The time left, in whole milliseconds, within an int. */
	milliseconds = (record->deadline_us - now_us + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;

	/* Succeeded: the wait. */
	return (int)milliseconds;
}

/*
 * Tells whether a request is served now: while asleep the Wi-Fi changes
 * (on, off, join, disconnect) and the start of a confirmed transaction
 * are refused; everything else (the wired requests, the lists, the
 * scans' leases, the sessions' stores, the sleep's own) is served.
 */
int
networkd_sleep_admits(
	const struct networkd_sleep *record,
	uint32_t opcode)
{
	/* Awake: everything is served. */
	if (!record->asleep)
		return 1;

	/* The Wi-Fi changes and a new confirmed transaction wait for the end. */
	switch (opcode) {
	case NETWORKD_OP_WIFI_ENABLE:
	case NETWORKD_OP_WIFI_DISABLE:
	case NETWORKD_OP_WIFI_CONNECT:
	case NETWORKD_OP_WIFI_DISCONNECT:
	case NETWORKD_OP_CONFIRMED_ARM:
		return 0;
	default:
		break;
	}

	/* Succeeded: the request is served. */
	return 1;
}

/* Gives what SLEEP_END takes up again for the recorded Wi-Fi policy state. */
static enum networkd_sleep_resume
sleep_resume_of(
	enum networkd_managed_wlan_state state)
{
	/* Each recorded state. */
	switch (state) {
	case NETWORKD_WLAN_DISABLED:
		/* Wi-Fi was off: the radios stay down. */
		return NETWORKD_SLEEP_RESUME_NONE;
	case NETWORKD_WLAN_MANUAL_DISCONNECTED:
		/* The user had disconnected: the radios up, no connection. */
		return NETWORKD_SLEEP_RESUME_MANUAL;
	case NETWORKD_WLAN_AUTO_SEARCHING:
	case NETWORKD_WLAN_CONNECTING:
	case NETWORKD_WLAN_CONNECTED:
	case NETWORKD_WLAN_RECONNECTING:
	case NETWORKD_WLAN_RETIRING:
	default:
		/* Searching, connecting or connected: the automatic search for the saved networks. */
		break;
	}

	/* Succeeded: the search. */
	return NETWORKD_SLEEP_RESUME_SEARCH;
}
