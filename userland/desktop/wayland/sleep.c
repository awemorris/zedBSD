/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's sleep (ws052-p012; the design is
 * plan/ws052/phase007/phase.md, its fourth version, sections 0 and 3 to
 * 5).  Three causes sleep the machine: the lid closed (looked at each
 * tick, as a level), the sleep button, and the time without input the
 * settings give (power.sleep.ac and power.sleep.battery; the screen goes
 * out at half that time).  An application's SUSPEND comes here too.  The
 * power button's short press does not sleep the machine: it opens the
 * power dialog (WS182, the 2026-10-07 user decision, backend-host.c).
 *
 * A session is locked first, and the request is sent to sessiond once the
 * lock screen (or the lid's black) was drawn twice, so that the user wakes
 * to it; from then until the answer no frame is drawn (sleep_hold).  The
 * answer comes through handoff.c before the lock and login screens could
 * take it for a password's.  A refusal is said on the lock or login
 * screen and once as a notification, and the next sleep waits a growing
 * pause; a machine that cannot sleep is not asked again.
 *
 * The rules are sleep-rules.c, which the host tests run alone; this file
 * carries them out on the server.  Only a backend whose sessiond answers
 * a sleep (zedBSD) takes part: elsewhere the causes do what they did.
 *
 * The lid of a machine showing an external display does not sleep it
 * (kwl_output_lid_matters, N8 R5), and closing the lid with an external
 * display connected moves the desktop to it instead (backend-host.c's
 * kwl_lid_follow, R4; WS113 p004a and p011a); the opening brings it back.
 */

#include "kwl.h"
#include "compose.h"

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int sleep_possible(struct kwl_server *server);
static int sleep_owns_screen(struct kwl_server *server);
static uint64_t sleep_quiet_since(struct kwl_server *server, uint64_t now_ms);
static void sleep_idle(struct kwl_server *server, uint64_t now_ms);
static void sleep_begin(struct kwl_server *server, enum kwl_sleep_via via, uint64_t now_ms);
static void sleep_pending(struct kwl_server *server, uint64_t now_ms);
static void sleep_fail(struct kwl_server *server, const char *why, uint64_t now_ms);
static void sleep_follow_lid(struct kwl_server *server);
static void sleep_say(struct kwl_server *server, const struct kl_backend_power_outcome *outcome, unsigned actions);

/*
 * Looks at the sleep's causes and carries its course on, once each pass of
 * the event loop: the input that came, the screen going out and back with
 * the time without input, the lid closed, the time without input run out,
 * the rest after a wake that was not the user's, and the pending request.
 */
void
kwl_sleep_tick(
	struct kwl_server *server)
{
	struct kwl_sleep *sleep;
	uint64_t now;
	int answers;

	/* Only a backend whose session manager answers a sleep takes part. */
	answers = kwl_sleep_answers(server);
	if (!answers)
		return;

	/* The lid's level, read once from the backend (a machine started with it closed). */
	sleep = &server->sleep;
	now = kwl_milliseconds();
	if (!server->sleep_lid_synced) {
		server->sleep_lid_synced = 1U;
		sleep_follow_lid(server);
	}

	/* New input: the pauses end, and a screen out for the time without input lights. */
	if (server->lock_input_ms != sleep->seen_input_ms) {
		kwl_sleep_input(sleep, server->lock_input_ms);
		if (server->screen_idle_off && !server->lid.closed) {
			kwl_lid_screen_restore(server);
			printf("KWL SLEEP screen on via=input\n");
		}
	}

	/* A pending request goes on; one waiting for its answer has nothing to do. */
	if (sleep->state == KWL_SLEEP_PENDING) {
		sleep_pending(server, now);
		return;
	}

	/* One waiting for its answer has nothing to do until it comes. */
	if (sleep->state == KWL_SLEEP_WAITING)
		return;

	/* The screen out at half the time without input, whether or not the machine can sleep. */
	sleep_idle(server, now);
}

/*
 * Takes a press of the sleep button: a sleep, unless it is the press that
 * woke the machine or one comes too soon after an answer.
 */
void
kwl_sleep_button(
	struct kwl_server *server)
{
	uint64_t now;
	int ignored;
	int possible;
	int may;

	/* Only a backend whose session manager answers a sleep takes part. */
	possible = sleep_possible(server);
	if (!possible) {
		printf("KWL SLEEP skip via=sleep-button reason=cannot\n");
		return;
	}

	/* A press around a sleep is not the user's. */
	now = kwl_milliseconds();
	ignored = kwl_sleep_button_ignored(&server->sleep, now);
	if (ignored) {
		printf("KWL SLEEP ignore button\n");
		return;
	}

	/* Not during the pause after a failure. */
	may = kwl_sleep_may_begin(&server->sleep, now);
	if (!may) {
		printf("KWL SLEEP skip via=sleep-button reason=paused\n");
		return;
	}

	/* Succeeded: the sleep begins. */
	sleep_begin(server, KWL_SLEEP_VIA_BUTTON, now);
}

/*
 * Asks for a sleep from another part (an application's SUSPEND, system.c).
 * Returns 0 when it began, ENOTSUP when the machine cannot sleep or the
 * compositor does not show the screen, or EBUSY while one is under way or
 * paused after a failure.
 */
int
kwl_sleep_request(
	struct kwl_server *server,
	enum kwl_sleep_via via)
{
	uint64_t now;
	int possible;
	int may;

	/* A machine that cannot sleep, or a compositor without the screen. */
	possible = sleep_possible(server);
	if (!possible)
		return ENOTSUP;

	/* One at a time, and not during a pause. */
	now = kwl_milliseconds();
	may = kwl_sleep_may_begin(&server->sleep, now);
	if (!may)
		return EBUSY;

	/* Succeeded: the sleep begins. */
	sleep_begin(server, via, now);
	return 0;
}

/*
 * Tells whether the backend's session manager answers a sleep (zedBSD's
 * sessiond); elsewhere the compositor's sleep takes no part.
 */
int
kwl_sleep_answers(
	struct kwl_server *server)
{
	struct kl_backend_power_outcome outcome;
	int error;

	/* The backend says whether it keeps a sleep's outcome. */
	error = kl_backend_power_outcome(server->backend, &outcome);
	if (error != 0)
		return 0;

	/* Succeeded: sleeps are answered. */
	return 1;
}

/*
 * Tells whether a sleep's request waits for its answer.
 */
int
kwl_sleep_waiting(
	struct kwl_server *server)
{
	/* Waiting from the request until the answer. */
	if (server->sleep.state == KWL_SLEEP_WAITING)
		return 1;

	/* Succeeded: no answer is awaited. */
	return 0;
}

/*
 * Takes the answer to the sleep's request (handoff.c): draws again, says a
 * refusal, follows the lid as it is now, and starts the input clock again
 * after a sleep.
 */
void
kwl_sleep_answer(
	struct kwl_server *server,
	int error)
{
	struct kl_backend_power_outcome outcome;
	unsigned actions;
	uint64_t now;
	int lid_open;
	int status;

	/* What the sleep came to; a backend that cannot say gives an error of its own. */
	memset(&outcome, 0, sizeof(outcome));
	status = kl_backend_power_outcome(server->backend, &outcome);
	if (status != 0) {
		outcome.kind = KL_BACKEND_SLEEP_ERROR;
		outcome.error = error;
	}

	/* The power and the lid as they are now (the adapter, the lid's level, whether the machine can sleep). */
	kwl_power_read(server);
	lid_open = 1;
	if (server->power.lid == 0)
		lid_open = 0;

	/* The rules take the answer. */
	now = kwl_milliseconds();
	actions = kwl_sleep_answered(&server->sleep, &outcome, lid_open, now, server->lock_input_ms);
	printf("KWL SLEEP answer kind=%d error=%d wake=%s device=%s network=%d resume_error=%d\n",
	       (int)outcome.kind, outcome.error, outcome.wake, outcome.device, (int)outcome.network, outcome.resume_error);

	/* Frames are drawn again: the lock screen (or black) the user wakes to. */
	server->sleep_hold = 0U;
	server->dirty = 1;

	/* After a sleep the time without input counts from now (not as new input: the rest after a wake stays). */
	if ((actions & KWL_SLEEP_DO_WOKE) != 0U) {
		server->lock_input_ms = now;
		server->sleep.seen_input_ms = now;
	}

	/* Woken with the lid open, the screen lights (a closed lid keeps it out, section 3.5). */
	if ((actions & KWL_SLEEP_DO_WOKE) != 0U && lid_open)
		kwl_lid_screen_restore(server);

	/* Settings and the bar hear what the machine can do now. */
	kwl_system_power_changed(server);

	/* The lid as it is now: an opening during the sleep is carried out here (sleep_follow_lid). */
	sleep_follow_lid(server);

	/* A refusal said on the lock or login screen, and once as a notification. */
	sleep_say(server, &outcome, actions);
}

/*
 * Tells whether keys are kept from the lock and login screens now (around
 * a sleep, greeter.c).
 */
int
kwl_sleep_keys_held_now(
	struct kwl_server *server)
{
	uint64_t now;
	int held;

	/* The rules decide. */
	now = kwl_milliseconds();
	held = kwl_sleep_keys_held(&server->sleep, now);
	if (held)
		return 1;

	/* Succeeded: keys go to the screen. */
	return 0;
}

/*
 * Takes the lid's opening while a sleep is under way.  Pending, the sleep
 * is dropped and the opening is carried out as usual (returns 0); waiting
 * for its answer, sessiond is asked to cancel it and the opening waits
 * for the answer (returns 1), so that no unlock comes before the sleep.
 */
int
kwl_sleep_lid_opened(
	struct kwl_server *server)
{
	struct kwl_sleep *sleep;
	int error;

	/* Pending: the request never goes (the lid's sleep, or any other: the user is here). */
	sleep = &server->sleep;
	if (sleep->state == KWL_SLEEP_PENDING) {
		kwl_sleep_dropped(sleep);
		printf("KWL SLEEP drop reason=lid-open\n");
		return 0;
	}

	/* Nothing under way: the opening is the lid's own. */
	if (sleep->state != KWL_SLEEP_WAITING)
		return 0;

	/* Waiting: a cancel, once; it is never answered (the sleep's own answer still comes). */
	if (!sleep->cancel_sent) {
		sleep->cancel_sent = 1U;
		error = kl_backend_power_cancel_sleep(server->backend);
		printf("KWL SLEEP cancel error=%d\n", error);
	}

	/* Succeeded: the opening waits for the answer. */
	return 1;
}

/* Tells whether a sleep can be asked: the machine can sleep, sessiond answers, and this compositor shows the screen. */
static int
sleep_possible(
	struct kwl_server *server)
{
	unsigned bit;
	int answers;
	int owns;

	/* sessiond answers sleeps (zedBSD). */
	answers = kwl_sleep_answers(server);
	if (!answers)
		return 0;

	/* The backend offers the sleep (the machine can sleep, and sessiond is there). */
	bit = KL_BACKEND_POWER_ACTION_BIT(KL_BACKEND_POWER_SUSPEND);
	if ((server->power.actions & bit) == 0U)
		return 0;

	/* The machine answered once that it cannot. */
	if (server->sleep.unsupported)
		return 0;

	/* Only the compositor showing the screen (not one handing it over, section 0.5). */
	owns = sleep_owns_screen(server);
	if (!owns)
		return 0;

	/* Succeeded: a sleep can be asked. */
	return 1;
}

/*
 * Tells whether this compositor shows the screen: its output is open, it
 * is not logging out, and a login screen has not handed over to a session.
 */
static int
sleep_owns_screen(
	struct kwl_server *server)
{
	int starting;

	/* The output, open. */
	if (server->compose == NULL || !server->compose->output_open)
		return 0;

	/* A session that asked for Log Out hands the screen to a new login screen. */
	if (server->logout_ms != 0U)
		return 0;

	/* A login screen whose login was accepted hands the screen to the session. */
	if (server->greeter) {
		starting = kwl_greeter_starting();
		if (starting)
			return 0;
	}

	/* Succeeded: the screen is this compositor's. */
	return 1;
}

/*
 * Gives the time the time without input counts from: the last input, or
 * the last tick a fullscreen window was in front of a session (a video or
 * a game shown does not count as no input, N7).
 */
static uint64_t
sleep_quiet_since(
	struct kwl_server *server,
	uint64_t now_ms)
{
	struct kwl_object *top;
	uint64_t since;

	/* A fullscreen window in front of an unlocked session keeps the time from counting. */
	if (!server->greeter && !server->locked) {
		top = kwl_top_window(server);
		if (top != NULL && top->fullscreen)
			server->sleep.inhibited_ms = now_ms;
	}

	/* The later of the last input and the last such tick. */
	since = server->lock_input_ms;
	if (server->sleep.inhibited_ms > since)
		since = server->sleep.inhibited_ms;

	/* Succeeded: the time it counts from. */
	return since;
}

/*
 * Carries out the time without input and the lid closed: the screen out
 * at half the time, a sleep at the whole time, after the rest of a wake
 * that was not the user's, or while the lid is closed.
 */
static void
sleep_idle(
	struct kwl_server *server,
	uint64_t now_ms)
{
	struct kwl_sleep *sleep;
	uint64_t limit;
	uint64_t since;
	uint64_t quiet;
	int possible;
	int may;
	int allowed;
	int rest;
	int matters;

	/* The time without input and the limit the power source has. */
	sleep = &server->sleep;
	since = sleep_quiet_since(server, now_ms);
	quiet = 0U;
	if (now_ms > since)
		quiet = now_ms - since;
	limit = kwl_sleep_idle_limit_ms(server->power.source, server->sleep_ac_minutes, server->sleep_battery_minutes);

	/* Half the time without input puts the screen out (the next input lights it). */
	if (limit != 0U && quiet >= limit / 2U && !server->screen_off) {
		kwl_screen_off(server, "idle");
		server->screen_idle_off = 1U;
		printf("KWL SLEEP screen off via=idle quiet_ms=%llu\n", (unsigned long long)quiet);
	}

	/* The sleeps need a machine that can sleep, and no pause. */
	possible = sleep_possible(server);
	if (!possible)
		return;
	may = kwl_sleep_may_begin(sleep, now_ms);
	if (!may)
		return;

	/* The lid closed, when it matters to the output shown. */
	if (server->lid.closed) {
		matters = kwl_output_lid_matters(server);
		if (matters) {
			sleep_begin(server, KWL_SLEEP_VIA_LID, now_ms);
			return;
		}
	}

	/* The rest after a wake that was not the user's. */
	rest = kwl_sleep_rest_due(sleep, now_ms, since);
	if (rest) {
		sleep_begin(server, KWL_SLEEP_VIA_REST, now_ms);
		return;
	}

	/* The whole time without input, unless a failed idle sleep waits for new input. */
	allowed = kwl_sleep_idle_allowed(sleep, server->lock_input_ms);
	if (limit != 0U && quiet >= limit && allowed)
		sleep_begin(server, KWL_SLEEP_VIA_IDLE, now_ms);
}

/* Begins a sleep: the session locked (and the lid's black kept), the request sent once that was drawn. */
static void
sleep_begin(
	struct kwl_server *server,
	enum kwl_sleep_via via,
	uint64_t now_ms)
{
	/*
	 * Pending from now; a security key's change of Settings, and an
	 * attempt of the login or lock screen's, are stopped first, and the
	 * lock screen's card closes (ws199-p001, R5).
	 */
	kwl_system_keys_cancel(server, "sleep");
	kwl_greeter_sleep(server);
	kwl_sleep_begin(&server->sleep, via, now_ms);
	printf("KWL SLEEP begin via=%s\n", kwl_sleep_via_name(via));

	/* The first step at once (the lock). */
	sleep_pending(server, now_ms);
}

/*
 * Carries the pending sleep on: the session locked, the lock screen (or
 * black) drawn twice, then the request.  A lid opened meanwhile drops it.
 */
static void
sleep_pending(
	struct kwl_server *server,
	uint64_t now_ms)
{
	struct kwl_sleep *sleep;
	enum kwl_sleep_step step;
	const char *via_name;
	char reason[32];
	int locked;
	int error;
	int sent;

	/* The lid's sleep ends when the lid is open again. */
	sleep = &server->sleep;
	if (sleep->via == KWL_SLEEP_VIA_LID && !server->lid.closed) {
		kwl_sleep_dropped(sleep);
		printf("KWL SLEEP drop reason=lid-open\n");
		return;
	}

	/*
	 * A session sleeps only behind its lock screen (section 0.1); a session
	 * that cannot lock does not sleep.  The lock's reason names what began
	 * the sleep: one the user chose (the sleep button, App Home's or an
	 * application's Sleep) always asks for the secret, one of the lid, of
	 * idleness or of the rest after a wake opens on a swipe for a while
	 * (ws187-p002, the 2026-10-08 user decision).
	 */
	if (!server->greeter && !server->locked) {
		via_name = kwl_sleep_via_name(sleep->via);
		snprintf(reason, sizeof(reason), "sleep-%s", via_name);
		locked = kwl_lock(server, reason);
		if (!locked) {
			sleep_fail(server, "not-locked", now_ms);
			return;
		}
	}

	/* The frames are counted from the lock screen (or black) in place; it is drawn each pass until the request. */
	kwl_sleep_framed(sleep, server->frame);
	server->dirty = 1;

	/* Two frames drawn: the request; not in time: a failure. */
	step = kwl_sleep_pending_step(sleep, server->frame, now_ms);
	if (step == KWL_SLEEP_STEP_WAIT)
		return;
	if (step == KWL_SLEEP_STEP_GIVE_UP) {
		sleep_fail(server, "frames", now_ms);
		return;
	}

	/* The request to sessiond. */
	error = kl_backend_power_action(server->backend, KL_BACKEND_POWER_SUSPEND);
	sent = kwl_sleep_sent(sleep, error, now_ms);
	if (sent == 0)
		return;
	if (sent == -1) {
		printf("KWL SLEEP send error=%d\n", error);
		sleep_fail(server, "send", now_ms);
		return;
	}

	/* Nothing can be asked: the machine cannot sleep, or sessiond is gone. */
	if (sent == -2) {
		printf("KWL SLEEP skip via=%s reason=cannot error=%d\n", kwl_sleep_via_name(sleep->via), error);
		return;
	}

	/* Succeeded: no frame is drawn until the answer. */
	server->sleep_hold = 1U;
	printf("KWL SLEEP asked via=%s frame=%llu\n", kwl_sleep_via_name(sleep->via), (unsigned long long)server->frame);
}

/* Counts a failure of the pending sleep, which pauses the next one. */
static void
sleep_fail(
	struct kwl_server *server,
	const char *why,
	uint64_t now_ms)
{
	/* The rules count it. */
	printf("KWL SLEEP fail via=%s reason=%s\n", kwl_sleep_via_name(server->sleep.via), why);
	kwl_sleep_failed(&server->sleep, now_ms, server->lock_input_ms);
}

/*
 * Follows the lid's level the backend reads (at the start, and after a
 * sleep, when an opening during it was held back), as its event would.
 */
static void
sleep_follow_lid(
	struct kwl_server *server)
{
	/* An unknown lid is left as the events say. */
	if (server->power.lid < 0)
		return;

	/* Open now while the compositor has it closed. */
	if (server->power.lid > 0 && server->lid.closed) {
		printf("KWL SLEEP lid level=open\n");
		kwl_lid_follow(server, 1U);
		return;
	}

	/* Closed now while the compositor has it open. */
	if (server->power.lid == 0 && !server->lid.closed) {
		printf("KWL SLEEP lid level=closed\n");
		kwl_lid_follow(server, 0U);
	}
}

/*
 * Says an answer's reason in the user's words: on the lock or login
 * screen's line, and as a notification the first time.
 */
static void
sleep_say(
	struct kwl_server *server,
	const struct kl_backend_power_outcome *outcome,
	unsigned actions)
{
	enum kwl_sleep_reason reason;
	char name[KL_BACKEND_POWER_DEVICE_MAX];
	char number[16];
	char text[160];
	unsigned notify;

	/* Nothing to say. */
	if ((actions & KWL_SLEEP_DO_SAY) == 0U)
		return;

	/* The words of the reason. */
	reason = kwl_sleep_reason_of(outcome, name, sizeof(name));
	(void)snprintf(number, sizeof(number), "%d", outcome->error);
	text[0] = '\0';
	switch (reason) {
	case KWL_SLEEP_REASON_UNSUPPORTED:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("This computer cannot sleep."));
		break;
	case KWL_SLEEP_REASON_WIFI:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: Wi-Fi could not be turned off."));
		break;
	case KWL_SLEEP_REASON_DISK:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: the disk was busy."));
		break;
	case KWL_SLEEP_REASON_USB:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: a USB device was busy."));
		break;
	case KWL_SLEEP_REASON_DISPLAY:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: the display was busy."));
		break;
	case KWL_SLEEP_REASON_DRIVER_BUSY:
		(void)kl_tr_format(text, sizeof(text), kl_tr("Sleep was cancelled: {1} was busy."), name, (const char *)NULL);
		break;
	case KWL_SLEEP_REASON_DRIVER_CANNOT:
		(void)kl_tr_format(text, sizeof(text), kl_tr("Sleep was cancelled: {1} cannot sleep yet."), name, (const char *)NULL);
		break;
	case KWL_SLEEP_REASON_DEVICE:
		if (name[0] != '\0') {
			(void)kl_tr_format(text, sizeof(text), kl_tr("Sleep was cancelled ({1}, error {2})."), name, number, (const char *)NULL);
		} else {
			(void)kl_tr_format(text, sizeof(text), kl_tr("Sleep was cancelled (error {1})."), number, (const char *)NULL);
		}

		/* The part is named, or only the error. */
		break;
	case KWL_SLEEP_REASON_RESUME:
		(void)snprintf(number, sizeof(number), "%d", outcome->resume_error);
		(void)kl_tr_format(text, sizeof(text), kl_tr("A device did not come back after sleep (error {1})."), number, (const char *)NULL);
		break;
	case KWL_SLEEP_REASON_CONFIRMED:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: a network change is waiting to be confirmed."));
		break;
	case KWL_SLEEP_REASON_WIFI_BUSY:
		(void)snprintf(text, sizeof(text), "%s", kl_tr("Sleep was cancelled: Wi-Fi was busy."));
		break;
	case KWL_SLEEP_REASON_ERROR:
		(void)kl_tr_format(text, sizeof(text), kl_tr("Sleep was cancelled (error {1})."), number, (const char *)NULL);
		break;
	default:
		break;
	}

	/* No words for the reason. */
	if (text[0] == '\0')
		return;

	/* The log names the reason, whether it is left as a notification, and the words. */
	notify = 0U;
	if ((actions & KWL_SLEEP_DO_NOTIFY) != 0U)
		notify = 1U;
	printf("KWL SLEEP say reason=%d notify=%u text=%s\n", (int)reason, notify, text);

	/* The lock or login screen's line. */
	if (server->greeter || server->locked)
		kwl_greeter_say(server, text);

	/* The notification, once for the reason. */
	if (notify)
		(void)kwl_notify_post_system(server, kl_tr("Sleep"), text, 0U);
}
