/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What Settings shows of the computer, asked of the desktop (ws188-p002,
 * plan/ws188/phase001/phase.md section D6): About's names, the file
 * systems of Home's and Storage's tiles, the users of the Users page and
 * the login screen's language of the Languages page.  Settings reads no
 * system file and no account database itself: libkeiland's
 * kl_system_machine_* asks the compositor, which reads them on a thread
 * of its own (Guardrail "app と設定").
 *
 * Each part has one slot (struct se_machine): a part a page wants is
 * asked once until it is known (the file systems again every two seconds
 * while Home or Storage shows them); a failed query is asked again after
 * two seconds; a query without an answer for fifteen seconds is given up
 * and asked again.  An answer is copied part by part, only when the part's
 * serial changed, so that a part's log line comes once an answer.
 */

#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* How long a query may wait for its answer, and how long after a failure the part is asked again. */
#define MACHINE_EXPIRE_MS	15000U
#define MACHINE_RETRY_MS	2000U

/* How often the file systems are read again while a page shows them, and how long a page's wanting lasts. */
#define MACHINE_REFRESH_MS	2000U
#define MACHINE_WANTED_MS	3000U

/* The parts by their slots, as kl_system_machine_query names them. */
static const unsigned machine_bits[SE_MACHINE_PARTS] = { KL_MACHINE_ABOUT, KL_MACHINE_FILESYSTEMS, KL_MACHINE_USERS, KL_MACHINE_LOGIN_LANGUAGE };

static unsigned machine_due(const struct se_app *app, uint64_t now);
static uint32_t machine_query(struct se_app *app, unsigned parts, uint64_t now);
static void machine_copy_about(struct se_app *app);
static void machine_copy_filesystems(struct se_app *app);

/*
 * Asks for what Settings shows from its start: About's names, the users
 * (the Welcome's and Sharing's name, the administrator's cards) and the
 * login screen's language.
 */
void
se_machine_open(
	struct se_app *app)
{
	/* The parts every page may need, asked at once. */
	se_machine_want(app, KL_MACHINE_ABOUT | KL_MACHINE_USERS | KL_MACHINE_LOGIN_LANGUAGE);
}

/*
 * Says a page wants parts known (the file systems: fresh), and asks for
 * those that are due now.
 */
void
se_machine_want(
	struct se_app *app,
	unsigned parts)
{
	unsigned due;

	/* Wanted from now; the file systems for a while after the page last showed them. */
	app->machine.wanted |= parts;
	if ((parts & KL_MACHINE_FILESYSTEMS) != 0U)
		app->machine.filesystems_ms = app->now;

	/* What is due goes now. */
	due = machine_due(app, app->now);
	if (due != 0U)
		(void)machine_query(app, due, app->now);
}

/*
 * Asks for parts at once, even while an earlier query of them waits (a
 * change of the accounts was made, and the list is to show it).  Returns
 * the query's request, or 0 when it could not be asked.
 */
uint32_t
se_machine_ask_now(
	struct se_app *app,
	unsigned parts)
{
	uint32_t request;
	int offered;

	/* Not offered: nothing is asked. */
	offered = se_machine_offered(app);
	if (!offered)
		return 0;

	/* The query, whatever waits. */
	request = machine_query(app, parts, app->now);

	/* Succeeded: the number the answer comes with (0 when asking failed). */
	return request;
}

/*
 * Tells whether a part (one KL_MACHINE_* bit) was copied from an answer
 * at least once.
 */
int
se_machine_known(
	const struct se_app *app,
	unsigned part)
{
	unsigned slot;

	/* The part's slot. */
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		/* Copied once: its serial is not 0. */
		if (machine_bits[slot] == part && app->machine.serials[slot] != 0U)
			return 1;
	}

	/* Not known yet. */
	return 0;
}

/*
 * Tells whether a part (one KL_MACHINE_* bit) is being read: a query of it
 * waits for its answer.
 */
int
se_machine_reading(
	const struct se_app *app,
	unsigned part)
{
	unsigned slot;

	/* The part's slot. */
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		/* The part asked and not answered. */
		if (machine_bits[slot] == part && app->machine.asked[slot] != 0U)
			return 1;
	}

	/* Not being read. */
	return 0;
}

/*
 * Copies each part an answer changed (its serial grew): About's names, the
 * file systems, the users and the login screen's language.
 */
void
se_machine_follow(
	struct se_app *app)
{
	uint32_t serial;
	unsigned slot;

	/* Nothing without the system. */
	if (app->system == NULL)
		return;

	/* Each part whose serial changed. */
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		serial = kl_system_machine_serial(app->system, machine_bits[slot]);
		if (serial == app->machine.serials[slot])
			continue;

		/* Copied now. */
		app->machine.serials[slot] = serial;
		app->machine.copied_ms[slot] = app->now;
		app->dirty = 1;

		/* The part to the page that shows it. */
		switch (machine_bits[slot]) {
		case KL_MACHINE_ABOUT:
			machine_copy_about(app);
			break;
		case KL_MACHINE_FILESYSTEMS:
			machine_copy_filesystems(app);
			break;
		case KL_MACHINE_USERS:
			se_users_copy(app);
			break;
		default:
			se_languages_copy(app);
			break;
		}
	}
}

/*
 * Takes the answer of a query of the computer.  Returns 1 when the
 * request was one.
 */
int
se_machine_result(
	struct se_app *app,
	uint32_t request,
	int error)
{
	unsigned slot;
	unsigned index;
	int ours;

	/* Each part this query asked is answered (a failure is asked again later). */
	ours = 0;
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		if (app->machine.asked[slot] != request || request == 0U)
			continue;

		/* The slot is free again, and remembers a failure. */
		app->machine.asked[slot] = 0U;
		app->machine.failed_ms[slot] = 0U;
		if (error != 0)
			app->machine.failed_ms[slot] = app->now;
		ours = 1;
	}

	/* A query a newer one replaced is still the computer's. */
	for (index = 0; index < SE_MACHINE_RECENT; index++) {
		if (app->machine.recent[index] == request && request != 0U)
			ours = 1;
	}

	/* The Languages page's reading after its change (ws158-p004). */
	if (ours && request == app->languages.reload_request)
		se_languages_reloaded(app, error);

	/* Not a query of the computer. */
	if (!ours)
		return 0;

	/* Succeeded: the answer was the computer's (logged for the tests). */
	se_log("MACHINE result request=%u errno=%d", request, error);
	return 1;
}

/*
 * Gives up the queries without an answer for too long, and asks for the
 * parts that are due.
 */
void
se_machine_poll(
	struct se_app *app,
	uint64_t now)
{
	unsigned slot;
	unsigned due;

	/* A query that took too long counts as failed. */
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		if (app->machine.asked[slot] == 0U)
			continue;
		if (now - app->machine.asked_ms[slot] < MACHINE_EXPIRE_MS)
			continue;

		/* Given up: asked again after the retry's wait. */
		se_log("MACHINE expired request=%u part=%u", app->machine.asked[slot], machine_bits[slot]);
		app->machine.asked[slot] = 0U;
		app->machine.failed_ms[slot] = now;
	}

	/* The parts due now. */
	due = machine_due(app, now);
	if (due != 0U)
		(void)machine_query(app, due, now);
}

/*
 * Reports how long the main loop may sleep before a query is due or gives
 * up, in milliseconds, or -1 when nothing is waited for.
 */
int
se_machine_wait(
	const struct se_app *app)
{
	const struct se_machine *machine;
	uint64_t earliest;
	uint64_t at;
	unsigned slot;
	int offered;

	/* Nothing to ask without the computer's readings. */
	offered = se_machine_offered(app);
	if (!offered)
		return -1;

	/* The earliest moment something is due. */
	machine = &app->machine;
	earliest = UINT64_MAX;
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		/* A query waiting: when it gives up. */
		if (machine->asked[slot] != 0U) {
			at = machine->asked_ms[slot] + MACHINE_EXPIRE_MS;
			if (at < earliest)
				earliest = at;
			continue;
		}

		/* A part not wanted is never due. */
		if ((machine->wanted & machine_bits[slot]) == 0U)
			continue;

		/* A failed part: when it is asked again. */
		if (machine->failed_ms[slot] != 0U) {
			at = machine->failed_ms[slot] + MACHINE_RETRY_MS;
			if (at < earliest)
				earliest = at;
			continue;
		}

		/* The file systems while a page shows them: their next reading. */
		if (machine_bits[slot] == KL_MACHINE_FILESYSTEMS && app->now - machine->filesystems_ms < MACHINE_WANTED_MS) {
			at = machine->copied_ms[slot] + MACHINE_REFRESH_MS;
			if (at < earliest)
				earliest = at;
		}
	}

	/* Nothing is due. */
	if (earliest == UINT64_MAX)
		return -1;

	/* Due already. */
	if (earliest <= app->now)
		return 0;

	/* Succeeded: the wait, at most a minute. */
	if (earliest - app->now > 60000U)
		return 60000;
	return (int)(earliest - app->now);
}

/*
 * Tells whether the desktop offers the computer's readings.
 */
int
se_machine_offered(
	const struct se_app *app)
{
	unsigned bits;

	/* No system, no readings. */
	if (app->system == NULL)
		return 0;

	/* The desktop's offer. */
	bits = kl_system_capabilities(app->system);
	if ((bits & KL_SYSTEM_HAS_MACHINE) == 0U)
		return 0;

	/* Offered. */
	return 1;
}

/* Works out the parts due to be asked now. */
static unsigned
machine_due(
	const struct se_app *app,
	uint64_t now)
{
	const struct se_machine *machine;
	unsigned slot;
	unsigned due;
	unsigned bit;
	int offered;

	/* Nothing is asked of a desktop that does not offer it. */
	offered = se_machine_offered(app);
	if (!offered)
		return 0U;

	/* Each part wanted and free. */
	machine = &app->machine;
	due = 0U;
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		bit = machine_bits[slot];
		if ((machine->wanted & bit) == 0U || machine->asked[slot] != 0U)
			continue;

		/* A failed part waits a little before it is asked again. */
		if (machine->failed_ms[slot] != 0U && now - machine->failed_ms[slot] < MACHINE_RETRY_MS)
			continue;

		/* The file systems: while a page shows them, read again every two seconds. */
		if (bit == KL_MACHINE_FILESYSTEMS) {
			if (now - machine->filesystems_ms >= MACHINE_WANTED_MS)
				continue;
			if (machine->copied_ms[slot] != 0U && now - machine->copied_ms[slot] < MACHINE_REFRESH_MS)
				continue;
			due |= bit;
			continue;
		}

		/* The other parts: until they are known (a part copied once has a serial). */
		if (machine->serials[slot] == 0U)
			due |= bit;
	}

	/* Succeeded: the parts to ask. */
	return due;
}

/* Asks for parts; returns the request's number, or 0 when it could not be asked. */
static uint32_t
machine_query(
	struct se_app *app,
	unsigned parts,
	uint64_t now)
{
	uint32_t request;
	unsigned slot;
	int error;

	/* The query. */
	error = kl_system_machine_query(app->system, parts, &request);
	if (error != 0) {
		se_log("MACHINE query parts=%u errno=%d", parts, error);
		return 0;
	}

	/* Each part asked waits for this answer (one that waited for an older one now waits for this). */
	for (slot = 0; slot < SE_MACHINE_PARTS; slot++) {
		if ((parts & machine_bits[slot]) == 0U)
			continue;
		app->machine.asked[slot] = request;
		app->machine.asked_ms[slot] = now;
	}

	/* Remembered among the last queries. */
	app->machine.recent[app->machine.recent_next] = request;
	app->machine.recent_next = (app->machine.recent_next + 1U) % SE_MACHINE_RECENT;
	se_log("MACHINE query request=%u parts=%u", request, parts);

	/* Succeeded: the query is asked. */
	return request;
}

/* Copies About's names: only the six the desktop reads (the window's and the monitor's stay). */
static void
machine_copy_about(
	struct se_app *app)
{
	struct kl_machine_about names;
	struct se_about *about;
	int error;

	/* The names of the last answer. */
	error = kl_system_machine_about(app->system, &names);
	if (error != 0)
		return;

	/* Into About's own fields. */
	about = &app->about;
	(void)snprintf(about->system, sizeof(about->system), "%s", names.system);
	(void)snprintf(about->kernel, sizeof(about->kernel), "%s", names.kernel);
	(void)snprintf(about->machine, sizeof(about->machine), "%s", names.architecture);
	(void)snprintf(about->processor, sizeof(about->processor), "%s", names.processor);
	(void)snprintf(about->host, sizeof(about->host), "%s", names.host);
	about->cores = names.cpus;

	/* The log line the tests read. */
	se_log("ABOUT system=%s kernel=%s machine=%s cores=%u host=%s", about->system, about->kernel, about->machine,
	       about->cores, about->host);
}

/* Copies the file systems for Home's and Storage's tiles. */
static void
machine_copy_filesystems(
	struct se_app *app)
{
	struct kl_machine_filesystem list[KL_MACHINE_FILESYSTEMS_MAX];
	struct se_volume *volume;
	size_t count;
	size_t index;

	/* The file systems of the last answer. */
	count = kl_system_machine_filesystems(app->system, list, KL_MACHINE_FILESYSTEMS_MAX);
	if (count > SE_VOLUMES)
		count = SE_VOLUMES;

	/* Each one, as the pages show it. */
	for (index = 0; index < count; index++) {
		volume = &app->look.volumes[index];
		memcpy(volume->path, list[index].path, sizeof(volume->path));
		volume->path[sizeof(volume->path) - 1U] = '\0';
		volume->total = list[index].total;
		volume->available = list[index].available;
		volume->used = list[index].used;
	}

	/* The list's length, now known. */
	app->look.volume_count = (unsigned)count;
}
