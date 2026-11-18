/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power on Linux: logind's PowerOff, Reboot and Suspend
 * (libkeiland-backend, ws131-p006).
 *
 * Each question and each action opens its own connection to the system
 * bus and closes it after the answer, so the power does not depend on
 * which seat the compositor took (logind's or the direct one).  An action
 * is offered when logind's CanPowerOff, CanReboot or CanSuspend answers
 * "yes" or "challenge" (polkit may still ask or refuse).  The call is not
 * interactive, and logind's answer to it is the action's return value
 * (no session_answer follows on Linux).  The power source is not read.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"
#include "userland/desktop/libkeiland-backend-linux/dbus-linux.h"

#include <errno.h>
#include <string.h>

/* logind's manager. */
#define POWER_LOGIND		"org.freedesktop.login1"
#define POWER_LOGIND_PATH	"/org/freedesktop/login1"
#define POWER_LOGIND_MANAGER	"org.freedesktop.login1.Manager"

static int power_offered(struct linux_dbus *bus, const char *member);
static const char *power_member(unsigned action, int question);

/*
 * Copies the power's state: the source unknown, and the actions logind
 * allows.
 */
int
kl_backend_power_get_state(
	const struct kl_backend *backend,
	struct kl_backend_power_state *state)
{
	struct linux_dbus bus;
	unsigned action;
	int error;

	/* A state needs a backend and somewhere to put it. */
	if (backend == NULL || state == NULL)
		return EINVAL;

	/* No battery interface: the source and the charge are unknown. */
	state->source = KL_BACKEND_POWER_SOURCE_UNKNOWN;
	state->percent = -1;
	state->charging = 0U;
	state->actions = 0U;
	state->lid = -1;
	state->can_sleep = 0U;

	/* Without the system bus no action is offered. */
	memset(&bus, 0, sizeof(bus));
	bus.fd = -1;
	error = dbus_open_system(&bus);
	if (error != 0)
		return 0;

	/* Each action logind allows. */
	for (action = KL_BACKEND_POWER_POWEROFF; action <= KL_BACKEND_POWER_SUSPEND; action++) {
		if (power_offered(&bus, power_member(action, 1)))
			state->actions |= KL_BACKEND_POWER_ACTION_BIT(action);
	}
	dbus_close(&bus);

	/* Succeeded: the state is filled. */
	return 0;
}

/*
 * Asks logind to power the machine off, restart it or suspend it.
 */
int
kl_backend_power_action(
	struct kl_backend *backend,
	unsigned action)
{
	struct linux_dbus bus;
	struct dbus_arg interactive;
	struct dbus_reply *reply;
	const char *member;
	int error;

	/* A known action, one at a time (a suspend may be asked again after the machine wakes). */
	if (backend == NULL)
		return EINVAL;
	member = power_member(action, 0);
	if (member == NULL)
		return ENOTSUP;
	if (backend->power_asked != 0U && backend->power_asked != KL_BACKEND_POWER_SUSPEND)
		return EBUSY;

	/* The call, not interactive: polkit decides without asking. */
	memset(&bus, 0, sizeof(bus));
	bus.fd = -1;
	error = dbus_open_system(&bus);
	if (error != 0)
		return error;
	memset(&interactive, 0, sizeof(interactive));
	interactive.number = 0;
	error = dbus_call(&bus, POWER_LOGIND, POWER_LOGIND_PATH, POWER_LOGIND_MANAGER, member, "b", &interactive, 1, &reply);
	if (error == 0)
		dbus_reply_free(reply);
	dbus_close(&bus);
	if (error != 0)
		return error;

	/* Succeeded: logind ends or suspends the machine. */
	backend->power_asked = action;
	return 0;
}

/* Asks logind whether an action is allowed: "yes" or "challenge". */
static int
power_offered(
	struct linux_dbus *bus,
	const char *member)
{
	struct dbus_reply *reply;
	const char *answer;
	int error;
	int offered;

	/* The question, answered with a string. */
	error = dbus_call(bus, POWER_LOGIND, POWER_LOGIND_PATH, POWER_LOGIND_MANAGER, member, "", NULL, 0, &reply);
	if (error != 0)
		return 0;
	offered = 0;
	if (strcmp(reply->signature, "s") == 0) {
		error = dbus_read_string(reply, &answer);
		if (error == 0 && (strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0))
			offered = 1;
	}
	dbus_reply_free(reply);

	/* 1 when logind allows it. */
	return offered;
}

/* logind's method for an action, or for asking about it; NULL for an unknown action. */
static const char *
power_member(
	unsigned action,
	int question)
{
	/* The three actions. */
	if (action == KL_BACKEND_POWER_POWEROFF)
		return question ? "CanPowerOff" : "PowerOff";
	if (action == KL_BACKEND_POWER_REBOOT)
		return question ? "CanReboot" : "Reboot";
	if (action == KL_BACKEND_POWER_SUSPEND)
		return question ? "CanSuspend" : "Suspend";

	/* Anything else. */
	return NULL;
}

/*
 * Copies what the last sleep came to: sleeps are not answered so here.
 */
int
kl_backend_power_outcome(
	const struct kl_backend *backend,
	struct kl_backend_power_outcome *outcome)
{
	/* An outcome needs a backend and somewhere to put it. */
	if (backend == NULL || outcome == NULL)
		return EINVAL;

	/* No sessiond answers a sleep here (ws052-p011). */
	return ENOTSUP;
}

/*
 * Refuses to cancel a sleep: there is no sessiond to ask.
 */
int
kl_backend_power_cancel_sleep(
	struct kl_backend *backend)
{
	/* A cancel needs a backend. */
	if (backend == NULL)
		return EINVAL;

	/* No sessiond to ask (ws052-p011). */
	return ENOTSUP;
}
