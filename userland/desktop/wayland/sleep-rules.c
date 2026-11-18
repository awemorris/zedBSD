/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rules of the compositor's sleep (sleep-rules.h says what and why).
 */

#include "sleep-rules.h"

#include <errno.h>
#include <string.h>

static uint64_t sleep_pause_ms(unsigned failures);
static int sleep_wake_is_user(const char *wake);
static int sleep_device_driver(const char *device, char *name, size_t size);

/*
 * Gives how long without input the machine sleeps after: the battery's
 * time on battery, the adapter's otherwise (a source not known counts as
 * the adapter).  0 is never.
 */
uint64_t
kwl_sleep_idle_limit_ms(
	unsigned source,
	int ac_minutes,
	int battery_minutes)
{
	int minutes;

	/* The time the source has. */
	minutes = ac_minutes;
	if (source == KL_BACKEND_POWER_SOURCE_BATTERY)
		minutes = battery_minutes;

	/* Zero or less is never. */
	if (minutes <= 0)
		return 0U;

	/* Succeeded: the time in milliseconds. */
	return (uint64_t)minutes * 60U * 1000U;
}

/*
 * Tells whether a sleep may begin now: none is under way, the machine has
 * not said it cannot sleep, and the pause after a failure is over.
 */
int
kwl_sleep_may_begin(
	const struct kwl_sleep *sleep,
	uint64_t now_ms)
{
	/* One sleep at a time. */
	if (sleep->state != KWL_SLEEP_IDLE)
		return 0;

	/* A machine that cannot sleep is not asked again. */
	if (sleep->unsupported)
		return 0;

	/* The pause after a failure. */
	if (now_ms < sleep->pause_until_ms)
		return 0;

	/* Succeeded: a sleep may begin. */
	return 1;
}

/*
 * Begins a sleep: the caller puts up the lock screen (or black) and the
 * request waits for its frames.
 */
void
kwl_sleep_begin(
	struct kwl_sleep *sleep,
	enum kwl_sleep_via via,
	uint64_t now_ms)
{
	/* Pending from now, with no frame counted and nothing refused or cancelled yet. */
	sleep->state = KWL_SLEEP_PENDING;
	sleep->via = via;
	sleep->pending_ms = now_ms;
	sleep->framed = 0U;
	sleep->pending_frame = 0U;
	sleep->refused_ms = 0U;
	sleep->cancel_sent = 0U;
}

/*
 * Notes the frame count at the moment the lock screen (or black) is in
 * place: the request goes once two more frames were drawn.
 */
void
kwl_sleep_framed(
	struct kwl_sleep *sleep,
	uint64_t frame)
{
	/* Only the first time. */
	if (sleep->framed)
		return;

	/* The frames are counted from here. */
	sleep->framed = 1U;
	sleep->pending_frame = frame;
}

/*
 * Says what the pending sleep does next: send its request once two frames
 * were drawn after the lock screen (or black) was put up, or give up when
 * they did not come within KWL_SLEEP_LOCK_MS.
 */
enum kwl_sleep_step
kwl_sleep_pending_step(
	const struct kwl_sleep *sleep,
	uint64_t frame,
	uint64_t now_ms)
{
	uint64_t elapsed;

	/* Two frames drawn since the lock screen was put up: the user wakes to it. */
	if (sleep->framed && frame >= sleep->pending_frame + 2U)
		return KWL_SLEEP_STEP_SEND;

	/* Not in time (or a clock that went back): the sleep fails. */
	if (now_ms < sleep->pending_ms)
		return KWL_SLEEP_STEP_GIVE_UP;
	elapsed = now_ms - sleep->pending_ms;
	if (elapsed > KWL_SLEEP_LOCK_MS)
		return KWL_SLEEP_STEP_GIVE_UP;

	/* Succeeded: the frames are still awaited. */
	return KWL_SLEEP_STEP_WAIT;
}

/*
 * Takes what sending the request gave: sent (or one sent already) waits
 * for the answer, another request of sessiond's awaited is asked again on
 * a later tick for KWL_SLEEP_SEND_MS.  Returns 1 when the sleep now waits
 * for its answer, 0 when it is still pending, -1 when it failed (the
 * caller calls kwl_sleep_failed) and -2 when it cannot be asked at all
 * (kwl_sleep_dropped was done).
 */
int
kwl_sleep_sent(
	struct kwl_sleep *sleep,
	int error,
	uint64_t now_ms)
{
	/* Sent, or a sleep already asked for whose answer this one takes. */
	if (error == 0 || error == EALREADY) {
		sleep->state = KWL_SLEEP_WAITING;
		sleep->waiting_ms = now_ms;
		return 1;
	}

	/* Another request awaits its answer: asked again, for a while. */
	if (error == EBUSY) {
		if (sleep->refused_ms == 0U)
			sleep->refused_ms = now_ms;
		if (now_ms >= sleep->refused_ms && now_ms - sleep->refused_ms <= KWL_SLEEP_SEND_MS)
			return 0;
		return -1;
	}

	/* No sleep can be asked (the machine cannot sleep, or no session manager): nothing more happens. */
	if (error == ENOTSUP) {
		kwl_sleep_dropped(sleep);
		return -2;
	}

	/* Any other error of sending is a failure. */
	return -1;
}

/*
 * Drops the pending sleep without counting a failure (the lid opened
 * before the request went, or nothing can be asked).
 */
void
kwl_sleep_dropped(
	struct kwl_sleep *sleep)
{
	/* Nothing is under way. */
	sleep->state = KWL_SLEEP_IDLE;
	sleep->via = KWL_SLEEP_VIA_NONE;
	sleep->framed = 0U;
	sleep->refused_ms = 0U;
}

/*
 * Counts a failed sleep: the next one waits 30 seconds, then 2 minutes,
 * then 10 minutes; an idle sleep is not tried again before new input.
 */
void
kwl_sleep_failed(
	struct kwl_sleep *sleep,
	uint64_t now_ms,
	uint64_t input_ms)
{
	/* An idle sleep that failed waits for input newer than the one it counted from. */
	if (sleep->via == KWL_SLEEP_VIA_IDLE || sleep->via == KWL_SLEEP_VIA_REST) {
		sleep->idle_blocked = 1U;
		sleep->idle_blocked_ms = input_ms;
	}

	/* The failures in a row, which lengthen the pause up to its last length. */
	if (sleep->failures < 3U)
		sleep->failures++;
	sleep->pause_until_ms = now_ms + sleep_pause_ms(sleep->failures);

	/* Nothing is under way. */
	kwl_sleep_dropped(sleep);
}

/*
 * Takes the answer of the sleep that waited: what it came to (outcome),
 * whether the lid is open now, the time, and the last input.  Returns the
 * KWL_SLEEP_DO_* the caller carries out.
 */
unsigned
kwl_sleep_answered(
	struct kwl_sleep *sleep,
	const struct kl_backend_power_outcome *outcome,
	int lid_open,
	uint64_t now_ms,
	uint64_t input_ms)
{
	enum kwl_sleep_reason reason;
	char name[KL_BACKEND_POWER_DEVICE_MAX];
	unsigned actions;
	uint32_t bit;
	int user;

	/* The answer came; the presses and keys around it are not the user's for a moment. */
	sleep->answered_ms = now_ms;
	actions = 0U;

	/* The reason to show, once as a notification. */
	reason = kwl_sleep_reason_of(outcome, name, sizeof(name));
	if (reason != KWL_SLEEP_REASON_NONE) {
		actions |= KWL_SLEEP_DO_SAY;
		bit = (uint32_t)1U << (unsigned)reason;
		if ((sleep->notified & bit) == 0U) {
			sleep->notified |= bit;
			actions |= KWL_SLEEP_DO_NOTIFY;
		}
	}

	/* What the kind of the answer leaves behind. */
	switch (outcome->kind) {
	case KL_BACKEND_SLEEP_SLEPT:
		/* Slept and woke: the failures end, and the input clock starts again. */
		actions |= KWL_SLEEP_DO_WOKE;
		sleep->failures = 0U;
		sleep->pause_until_ms = 0U;
		sleep->idle_blocked = 0U;

		/* A wake soon after the sleep is a short one; several in a row pause like a failure. */
		if (now_ms >= sleep->waiting_ms && now_ms - sleep->waiting_ms < KWL_SLEEP_SHORT_MS) {
			sleep->short_wakes++;
		} else {
			sleep->short_wakes = 0U;
		}

		/* Several short wakes in a row pause like a failure. */
		if (sleep->short_wakes >= KWL_SLEEP_SHORT_COUNT) {
			sleep->short_wakes = 0U;
			sleep->failures = 1U;
			sleep->pause_until_ms = now_ms + sleep_pause_ms(sleep->failures);
		}

		/* An open lid and a wake that was not the user's: the machine sleeps again after a short rest. */
		user = sleep_wake_is_user(outcome->wake);
		sleep->resting = 0U;
		if (lid_open && !user)
			sleep->resting = 1U;
		break;
	case KL_BACKEND_SLEEP_UNSUPPORTED:
		/* The machine cannot sleep: none is asked again. */
		sleep->unsupported = 1U;
		break;
	case KL_BACKEND_SLEEP_DEVICE:
		/* A driver without the part to sleep waits the longest pause; another refusal counts as a failure. */
		kwl_sleep_failed(sleep, now_ms, input_ms);
		if (outcome->error == EOPNOTSUPP) {
			sleep->failures = 3U;
			sleep->pause_until_ms = now_ms + sleep_pause_ms(sleep->failures);
		}

		/* The refusal is counted. */
		break;
	case KL_BACKEND_SLEEP_NETWORK:
	case KL_BACKEND_SLEEP_ERROR:
		/* A failure. */
		kwl_sleep_failed(sleep, now_ms, input_ms);
		break;
	default:
		/* Cancelled, or busy with another sleep: nothing is counted. */
		break;
	}

	/* Nothing is under way any more (kwl_sleep_failed may have said so already). */
	kwl_sleep_dropped(sleep);
	sleep->cancel_sent = 0U;

	/* Succeeded: the caller carries out the actions. */
	return actions;
}

/*
 * Takes new input: the pauses start again from their first length, a
 * failed idle sleep may be tried again, and the rest after a wake ends.
 */
void
kwl_sleep_input(
	struct kwl_sleep *sleep,
	uint64_t input_ms)
{
	/* The same input as before is not new. */
	if (input_ms == sleep->seen_input_ms)
		return;
	sleep->seen_input_ms = input_ms;

	/* The user is here: the pauses and the rest end. */
	sleep->failures = 0U;
	sleep->pause_until_ms = 0U;
	sleep->resting = 0U;

	/* Input after a failed idle sleep lets the next one be tried. */
	if (sleep->idle_blocked && input_ms > sleep->idle_blocked_ms)
		sleep->idle_blocked = 0U;
}

/*
 * Tells whether an idle sleep may be tried: not after a failed one until
 * new input.
 */
int
kwl_sleep_idle_allowed(
	const struct kwl_sleep *sleep,
	uint64_t input_ms)
{
	/* A failed idle sleep waits for input newer than the one it counted from. */
	if (sleep->idle_blocked && input_ms <= sleep->idle_blocked_ms)
		return 0;

	/* Succeeded: the idle sleep may be tried. */
	return 1;
}

/*
 * Tells whether the rest after a wake that was not the user's is over:
 * KWL_SLEEP_REST_MS without input.
 */
int
kwl_sleep_rest_due(
	const struct kwl_sleep *sleep,
	uint64_t now_ms,
	uint64_t input_ms)
{
	/* No rest is under way. */
	if (!sleep->resting)
		return 0;

	/* Not long enough without input (or a clock that went back). */
	if (now_ms < input_ms)
		return 0;
	if (now_ms - input_ms < KWL_SLEEP_REST_MS)
		return 0;

	/* Succeeded: the machine sleeps again. */
	return 1;
}

/*
 * Tells whether a sleep button's press is not the user's: while a sleep is
 * under way (the press that woke the machine comes before the answer), and
 * for KWL_SLEEP_BUTTON_QUIET_MS after the answer.
 */
int
kwl_sleep_button_ignored(
	const struct kwl_sleep *sleep,
	uint64_t now_ms)
{
	/* While a sleep is under way. */
	if (sleep->state != KWL_SLEEP_IDLE)
		return 1;

	/* Just after an answer. */
	if (sleep->answered_ms != 0U &&
	    now_ms >= sleep->answered_ms &&
	    now_ms - sleep->answered_ms < KWL_SLEEP_BUTTON_QUIET_MS)
		return 1;

	/* Succeeded: the press is the user's. */
	return 0;
}

/*
 * Tells whether keys are kept from the lock screen: while a sleep is
 * under way (nothing is drawn, so the password field would fill unseen),
 * and for KWL_SLEEP_KEY_QUIET_MS after the answer (the key that woke the
 * machine).
 */
int
kwl_sleep_keys_held(
	const struct kwl_sleep *sleep,
	uint64_t now_ms)
{
	/* While a sleep is under way. */
	if (sleep->state != KWL_SLEEP_IDLE)
		return 1;

	/* Just after an answer. */
	if (sleep->answered_ms != 0U &&
	    now_ms >= sleep->answered_ms &&
	    now_ms - sleep->answered_ms < KWL_SLEEP_KEY_QUIET_MS)
		return 1;

	/* Succeeded: keys go to the lock screen. */
	return 0;
}

/*
 * Says why a sleep did not happen, or what went wrong after it, as one of
 * the reasons the user is shown, with the name the words need (the driver,
 * or the part that refused) in name.
 */
enum kwl_sleep_reason
kwl_sleep_reason_of(
	const struct kl_backend_power_outcome *outcome,
	char *name,
	size_t size)
{
	int found;
	int same;

	/* No name unless the reason has one. */
	if (size > 0U)
		name[0] = '\0';

	/* Each kind of answer. */
	switch (outcome->kind) {
	case KL_BACKEND_SLEEP_SLEPT:
		/* Slept: only a device that did not come back is said. */
		if (outcome->resume_error != 0)
			return KWL_SLEEP_REASON_RESUME;
		return KWL_SLEEP_REASON_NONE;
	case KL_BACKEND_SLEEP_UNSUPPORTED:
		/* The machine cannot sleep. */
		return KWL_SLEEP_REASON_UNSUPPORTED;
	case KL_BACKEND_SLEEP_NETWORK:
		/* networkd: a change to confirm, the user's own Wi-Fi change, or the radio that would not go off. */
		if (outcome->network == KL_BACKEND_SLEEP_NETWORK_CONFIRMED)
			return KWL_SLEEP_REASON_CONFIRMED;
		if (outcome->network == KL_BACKEND_SLEEP_NETWORK_BUSY)
			return KWL_SLEEP_REASON_WIFI_BUSY;
		return KWL_SLEEP_REASON_WIFI;
	case KL_BACKEND_SLEEP_DEVICE:
		/* A driver's part: below. */
		break;
	case KL_BACKEND_SLEEP_ERROR:
		/* sessiond could not carry it out. */
		return KWL_SLEEP_REASON_ERROR;
	default:
		/* Busy with another sleep, or cancelled: nothing is said. */
		return KWL_SLEEP_REASON_NONE;
	}

	/* A part that is not a PCI device's driver is named as the kernel names it. */
	found = sleep_device_driver(outcome->device, name, size);
	if (!found) {
		if (size > 0U) {
			(void)strncpy(name, outcome->device, size - 1U);
			name[size - 1U] = '\0';
		}

		/* The part as the kernel names it. */
		return KWL_SLEEP_REASON_DEVICE;
	}

	/* A driver with no part to sleep yet. */
	if (outcome->error == EOPNOTSUPP)
		return KWL_SLEEP_REASON_DRIVER_CANNOT;

	/* Another refusal than busy names the part and the error. */
	if (outcome->error != EBUSY)
		return KWL_SLEEP_REASON_DEVICE;

	/* The drivers whose busy has words of its own. */
	same = strcmp(name, "intel-ax211");
	if (same == 0)
		return KWL_SLEEP_REASON_WIFI;
	same = strcmp(name, "nvme");
	if (same == 0)
		return KWL_SLEEP_REASON_DISK;
	same = strcmp(name, "xhci");
	if (same == 0)
		return KWL_SLEEP_REASON_USB;
	same = strcmp(name, "i915");
	if (same == 0)
		return KWL_SLEEP_REASON_DISPLAY;

	/* Succeeded: another driver was busy. */
	return KWL_SLEEP_REASON_DRIVER_BUSY;
}

/*
 * Names what began a sleep, for the log.
 */
const char *
kwl_sleep_via_name(
	enum kwl_sleep_via via)
{
	/* Each cause's word. */
	switch (via) {
	case KWL_SLEEP_VIA_LID:
		return "lid";
	case KWL_SLEEP_VIA_BUTTON:
		return "sleep-button";
	case KWL_SLEEP_VIA_IDLE:
		return "idle";
	case KWL_SLEEP_VIA_APP:
		return "app";
	case KWL_SLEEP_VIA_REST:
		return "rest";
	default:
		break;
	}

	/* Succeeded: no cause. */
	return "none";
}

/* Gives the pause after the failures in a row: 30 seconds, 2 minutes, then 10 minutes. */
static uint64_t
sleep_pause_ms(
	unsigned failures)
{
	/* The first failure's. */
	if (failures <= 1U)
		return KWL_SLEEP_PAUSE_FIRST_MS;

	/* The second's. */
	if (failures == 2U)
		return KWL_SLEEP_PAUSE_SECOND_MS;

	/* Succeeded: the last and longest. */
	return KWL_SLEEP_PAUSE_LAST_MS;
}

/* Tells whether a wake was the user's (the power button, the lid, a key, a USB device). */
static int
sleep_wake_is_user(
	const char *wake)
{
	static const char *const user_wakes[] = { "power-button", "lid", "keyboard", "usb" };
	unsigned index;
	int same;

	/* Each of the user's wakes. */
	for (index = 0U; index < sizeof(user_wakes) / sizeof(user_wakes[0]); index++) {
		same = strcmp(wake, user_wakes[index]);
		if (same == 0)
			return 1;
	}

	/* The devices' mode wakes no one: it is not a wake to rest after. */
	same = strcmp(wake, "none");
	if (same == 0)
		return 1;

	/* Succeeded: the adapter, a timer, or a spurious wake. */
	return 0;
}

/*
 * Finds the driver of a PCI device's name as the kernel gives it ("pci
 * 0000:00:14.3 intel-ax211": the last word).  Returns 1 with the driver in
 * name, 0 for a part that is not a PCI device.
 */
static int
sleep_device_driver(
	const char *device,
	char *name,
	size_t size)
{
	const char *last;
	size_t length;
	int same;

	/* Only a PCI device names a driver. */
	same = strncmp(device, "pci ", 4U);
	if (same != 0)
		return 0;

	/* The last word. */
	last = strrchr(device, ' ');
	if (last == NULL || last[1] == '\0')
		return 0;
	last++;

	/* Copied, cut to the room there is. */
	length = strlen(last);
	if (size == 0U)
		return 0;
	if (length >= size)
		length = size - 1U;
	memcpy(name, last, length);
	name[length] = '\0';

	/* Succeeded: the driver is named. */
	return 1;
}
