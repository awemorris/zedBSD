/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's own notifications and what a click on one does
 * (ws156-p003, plan/ws156/phase001/phase.md section 6): a notification
 * posted with an action keeps it here by its number, and a click of its
 * body runs it (a command started, as the bar's buttons start one).  The
 * battery's warnings are here too: an urgent notification once at 10% and
 * once at 5% while on the battery, again only after charging or climbing
 * back above the mark.
 */

#include "kwl.h"
#include "notify.h"

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <keiland/keiland.h>

#include <stdio.h>
#include <string.h>

/* The battery's marks, in percent: low, then critically low. */
#define NOTIFY_BATTERY_LOW	10
#define NOTIFY_BATTERY_CRITICAL	5

/* How many of the compositor's notifications with an action are kept, and the longest command. */
#define NOTIFY_SYSTEM_ACTIONS	16U
#define NOTIFY_SYSTEM_COMMAND	256U

/* One notification's action: its number (0: a free entry) and the command its click starts. */
struct notify_system_action {
	uint32_t id;
	char command[NOTIFY_SYSTEM_COMMAND];
};

/*
 * The actions of the compositor's notifications, the oldest replaced when
 * all are taken.  Only the main loop touches them; an entry stays after
 * its notification went (its number is not given again).
 */
static struct notify_system_action notify_system_actions[NOTIFY_SYSTEM_ACTIONS];
static unsigned notify_system_next;

/*
 * Whether the battery's low and critical warnings were given since it was
 * last charging or above their marks (each is given once a discharge).
 */
static int notify_battery_low;
static int notify_battery_critical;

/*
 * Posts a notification of the compositor's own whose body's click starts
 * a command (NULL: no action).  Returns its number, or 0 when it could not
 * be kept.
 */
uint32_t
kwl_notify_system_post(
	struct kwl_server *server,
	const char *title,
	const char *body,
	unsigned flags,
	const char *command)
{
	struct notify_system_action *action;
	uint32_t id;

	/* A notification with an action, when it has one. */
	if (command != NULL)
		flags |= KWL_NOTIFY_ACTION;
	id = kwl_notify_post_system(server, title, body, flags);
	if (id == 0U || command == NULL)
		return id;

	/* Its action, in the next entry. */
	action = &notify_system_actions[notify_system_next];
	notify_system_next = (notify_system_next + 1U) % NOTIFY_SYSTEM_ACTIONS;
	action->id = id;
	(void)snprintf(action->command, sizeof(action->command), "%s", command);

	/* Succeeded: its number. */
	return id;
}

/*
 * Posts a notification of the compositor's own under an application's
 * name (ws197-p004c), whose body's click starts a command (NULL: no
 * action), with what the lock screen shows of it (NULL: nothing).
 * Returns its number, or 0 when it could not be kept.
 */
uint32_t
kwl_notify_app_post(
	struct kwl_server *server,
	const char *app,
	const char *title,
	const char *body,
	const char *command,
	const char *lock_text)
{
	struct notify_system_action *action;
	unsigned flags;
	uint32_t id;

	/* A notification with an action, when it has one. */
	flags = 0U;
	if (command != NULL)
		flags |= KWL_NOTIFY_ACTION;
	id = kwl_notify_post_as(server, app, title, body, flags, lock_text);
	if (id == 0U || command == NULL)
		return id;

	/* Its action, in the next entry. */
	action = &notify_system_actions[notify_system_next];
	notify_system_next = (notify_system_next + 1U) % NOTIFY_SYSTEM_ACTIONS;
	action->id = id;
	(void)snprintf(action->command, sizeof(action->command), "%s", command);

	/* Succeeded: its number. */
	return id;
}

/*
 * Runs the action of a compositor's notification whose body was clicked
 * (kwl_notify_activate): its command is started.
 */
void
kwl_notify_system_activated(
	struct kwl_server *server,
	uint32_t id)
{
	unsigned index;
	pid_t child;

	/* The notification's action, if it has one. */
	for (index = 0U; index < NOTIFY_SYSTEM_ACTIONS; index++) {
		if (notify_system_actions[index].id != id || id == 0U)
			continue;

		/* Its command, started once. */
		child = kwl_spawn(server, notify_system_actions[index].command);
		printf("KWL NOTIFY system-action id=%u command=\"%s\" pid=%ld\n", id, notify_system_actions[index].command, (long)child);
		notify_system_actions[index].id = 0U;
		return;
	}
}

/*
 * Warns of a low battery from the power state just read (the backend's,
 * kwl_power_read): on the battery, an urgent notification at 10% and
 * another at 5%, each once until the machine charges or the charge is
 * back above its mark.
 */
void
kwl_notify_battery(
	struct kwl_server *server)
{
	char body[KWL_NOTIFY_BODY_MAX + 1U];
	char percent[16];
	int charge;

	/* Charging, or on the mains, or no battery: the warnings may come again later. */
	charge = server->power.percent;
	if (charge < 0 || server->power.charging != 0U || server->power.source != KL_BACKEND_POWER_SOURCE_BATTERY) {
		notify_battery_low = 0;
		notify_battery_critical = 0;
		return;
	}

	/* Back above a mark, its warning may come again. */
	if (charge > NOTIFY_BATTERY_LOW)
		notify_battery_low = 0;
	if (charge > NOTIFY_BATTERY_CRITICAL)
		notify_battery_critical = 0;

	/* The charge left, for the body. */
	(void)snprintf(percent, sizeof(percent), "%d", charge);
	(void)kl_tr_format(body, sizeof(body), kl_tr("{1}% of the battery is left"), percent, (const char *)NULL);

	/* Critically low: once (and the low warning with it). */
	if (charge <= NOTIFY_BATTERY_CRITICAL && !notify_battery_critical) {
		notify_battery_critical = 1;
		notify_battery_low = 1;
		(void)kwl_notify_system_post(server, kl_tr("Battery critically low"), body, KWL_NOTIFY_URGENT, NULL);
		printf("KWL NOTIFY battery percent=%d level=critical\n", charge);
		return;
	}

	/* Low: once. */
	if (charge <= NOTIFY_BATTERY_LOW && !notify_battery_low) {
		notify_battery_low = 1;
		(void)kwl_notify_system_post(server, kl_tr("Battery low"), body, KWL_NOTIFY_URGENT, NULL);
		printf("KWL NOTIFY battery percent=%d level=low\n", charge);
	}
}
