/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the compositor's sleep (ws052-p012, the design of
 * plan/ws052/phase007/phase.md sections 0 and 3 to 5): when a sleep may
 * begin, when its request is sent, what an answer leaves behind (the
 * growing pauses after failures, a sleep that cannot happen at all, the
 * rest after a wake that was not the user's), which presses and keys are
 * not the user's, and what an answer's reason is in the user's words.
 *
 * It knows nothing of the server: sleep.c hands it the time, the frames
 * drawn and the answers, and carries out what it decides.  So the host
 * tests run it alone.
 */

#ifndef KWL_SLEEP_RULES_H
#define KWL_SLEEP_RULES_H

#include <stddef.h>
#include <stdint.h>

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

/* How long the lock screen (or black) has to show its two frames before the request is sent (ms). */
#define KWL_SLEEP_LOCK_MS		500U
/* How long a request refused for another request of sessiond's is asked again (ms). */
#define KWL_SLEEP_SEND_MS		3000U
/* How long after an answer a sleep button's press is not the user's (ms; the press that woke the machine). */
#define KWL_SLEEP_BUTTON_QUIET_MS	1000U
/* How long after an answer a key is not given to the lock screen (ms; the key that woke the machine). */
#define KWL_SLEEP_KEY_QUIET_MS		500U
/* How long without input the machine sleeps again after a wake that was not the user's (ms). */
#define KWL_SLEEP_REST_MS		60000U
/* A wake within this of the sleep is a short one (ms); KWL_SLEEP_SHORT_COUNT in a row pause like a failure. */
#define KWL_SLEEP_SHORT_MS		10000U
#define KWL_SLEEP_SHORT_COUNT		3U
/* The pauses after failures: 30 seconds, 2 minutes, then 10 minutes each time (ms). */
#define KWL_SLEEP_PAUSE_FIRST_MS	30000U
#define KWL_SLEEP_PAUSE_SECOND_MS	120000U
#define KWL_SLEEP_PAUSE_LAST_MS		600000U
/* The time without input before the machine sleeps, on the power adapter and on battery (minutes, 0 never: never unless chosen). */
#define KWL_SLEEP_AC_MINUTES		0
#define KWL_SLEEP_BATTERY_MINUTES	0

/* Where a sleep is in its course. */
enum kwl_sleep_state {
	/* No sleep is asked for. */
	KWL_SLEEP_IDLE,
	/* The lock screen (or black) is put up and waited for; the request is not sent yet. */
	KWL_SLEEP_PENDING,
	/* The request is sent; no frame is drawn until its answer. */
	KWL_SLEEP_WAITING
};

/* What began a sleep. */
enum kwl_sleep_via {
	KWL_SLEEP_VIA_NONE,
	/* The lid is closed (looked at each tick). */
	KWL_SLEEP_VIA_LID,
	/* The sleep button was pressed. */
	KWL_SLEEP_VIA_BUTTON,
	/* No input for the time the settings give. */
	KWL_SLEEP_VIA_IDLE,
	/* An application asked (kl_system_power_v1's SUSPEND). */
	KWL_SLEEP_VIA_APP,
	/* No input for KWL_SLEEP_REST_MS after a wake that was not the user's. */
	KWL_SLEEP_VIA_REST
};

/* What the pending sleep does next (kwl_sleep_pending_step). */
enum kwl_sleep_step {
	/* Waits for the frames. */
	KWL_SLEEP_STEP_WAIT,
	/* Sends the request. */
	KWL_SLEEP_STEP_SEND,
	/* The frames did not come in time: the sleep fails. */
	KWL_SLEEP_STEP_GIVE_UP
};

/* Why a sleep did not happen, or what went wrong after it, in the words the user is shown. */
enum kwl_sleep_reason {
	/* Nothing to say (slept, busy, cancelled). */
	KWL_SLEEP_REASON_NONE,
	/* The machine cannot sleep at all. */
	KWL_SLEEP_REASON_UNSUPPORTED,
	/* Wi-Fi could not be turned off. */
	KWL_SLEEP_REASON_WIFI,
	/* The disk was busy. */
	KWL_SLEEP_REASON_DISK,
	/* A USB device was busy. */
	KWL_SLEEP_REASON_USB,
	/* The display was busy. */
	KWL_SLEEP_REASON_DISPLAY,
	/* Another driver (named) was busy. */
	KWL_SLEEP_REASON_DRIVER_BUSY,
	/* A driver (named) cannot sleep yet. */
	KWL_SLEEP_REASON_DRIVER_CANNOT,
	/* Another part (named, with the error) refused. */
	KWL_SLEEP_REASON_DEVICE,
	/* A device did not come back after the sleep. */
	KWL_SLEEP_REASON_RESUME,
	/* A network change waits to be confirmed. */
	KWL_SLEEP_REASON_CONFIRMED,
	/* Wi-Fi was busy with the user's own change. */
	KWL_SLEEP_REASON_WIFI_BUSY,
	/* sessiond could not carry the sleep out. */
	KWL_SLEEP_REASON_ERROR,
	KWL_SLEEP_REASON_COUNT
};

/* What kwl_sleep_answered asks the caller to do, as bits. */
#define KWL_SLEEP_DO_SAY	0x1U	/* show the reason on the lock or login screen */
#define KWL_SLEEP_DO_NOTIFY	0x2U	/* and leave it as a notification (the first time of this reason) */
#define KWL_SLEEP_DO_WOKE	0x4U	/* the machine slept and woke: the input clock starts again */

/*
 * The compositor's sleep: where it is, what began it, when its steps
 * happened, the pause after failures, and what was learnt (the machine
 * cannot sleep; a failed idle sleep waits for new input).  Zeroed, it is
 * idle with nothing learnt.  It lives as long as the compositor.
 */
struct kwl_sleep {
	enum kwl_sleep_state state;
	enum kwl_sleep_via via;
	/* When the pending sleep began, whether its frames are counted yet, and the frame count then. */
	uint64_t pending_ms;
	unsigned framed;
	uint64_t pending_frame;
	/* When a refused request was first asked again (0: not refused). */
	uint64_t refused_ms;
	/* When the request was sent, and when its answer came (0: none yet). */
	uint64_t waiting_ms;
	uint64_t answered_ms;
	/* The failures in a row (0 to 3) and the time before which no sleep begins after one. */
	unsigned failures;
	uint64_t pause_until_ms;
	/* A failed idle sleep is not tried again before input newer than idle_blocked_ms. */
	unsigned idle_blocked;
	uint64_t idle_blocked_ms;
	/* The machine answered that it cannot sleep: no sleep is asked for again. */
	unsigned unsupported;
	/* A cancel was sent for the sleep waiting (the lid opened). */
	unsigned cancel_sent;
	/* After a wake that was not the user's, the machine sleeps again after KWL_SLEEP_REST_MS without input. */
	unsigned resting;
	/* Short wakes in a row. */
	unsigned short_wakes;
	/* The reasons already left as notifications (bit per enum kwl_sleep_reason). */
	uint32_t notified;
	/* The last input the rules saw (kwl_sleep_input). */
	uint64_t seen_input_ms;
	/* The last tick a fullscreen window was in front: the time without input counts from it too (N7). */
	uint64_t inhibited_ms;
};

uint64_t kwl_sleep_idle_limit_ms(unsigned source, int ac_minutes, int battery_minutes);
int kwl_sleep_may_begin(const struct kwl_sleep *sleep, uint64_t now_ms);
void kwl_sleep_begin(struct kwl_sleep *sleep, enum kwl_sleep_via via, uint64_t now_ms);
void kwl_sleep_framed(struct kwl_sleep *sleep, uint64_t frame);
enum kwl_sleep_step kwl_sleep_pending_step(const struct kwl_sleep *sleep, uint64_t frame, uint64_t now_ms);
int kwl_sleep_sent(struct kwl_sleep *sleep, int error, uint64_t now_ms);
void kwl_sleep_dropped(struct kwl_sleep *sleep);
void kwl_sleep_failed(struct kwl_sleep *sleep, uint64_t now_ms, uint64_t input_ms);
unsigned kwl_sleep_answered(struct kwl_sleep *sleep, const struct kl_backend_power_outcome *outcome, int lid_open, uint64_t now_ms, uint64_t input_ms);
void kwl_sleep_input(struct kwl_sleep *sleep, uint64_t input_ms);
int kwl_sleep_idle_allowed(const struct kwl_sleep *sleep, uint64_t input_ms);
int kwl_sleep_rest_due(const struct kwl_sleep *sleep, uint64_t now_ms, uint64_t input_ms);
int kwl_sleep_button_ignored(const struct kwl_sleep *sleep, uint64_t now_ms);
int kwl_sleep_keys_held(const struct kwl_sleep *sleep, uint64_t now_ms);
enum kwl_sleep_reason kwl_sleep_reason_of(const struct kl_backend_power_outcome *outcome, char *name, size_t size);
const char *kwl_sleep_via_name(enum kwl_sleep_via via);

#endif
