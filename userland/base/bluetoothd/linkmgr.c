/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's manager of the BR/EDR page scan and page (ws197-p002, see
 * linkmgr.h).
 */

#include "userland/base/bluetoothd/linkmgr.h"

#include <errno.h>
#include <string.h>

/* Marks a parameter a function takes for its callers' sake and does not read. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* Write Scan Enable (Core 5.4 Vol 4 Part E 7.3.18) and its values: no scan, page scan only. */
#define LINKMGR_WRITE_SCAN_ENABLE	0x0c1aU
#define LINKMGR_SCAN_NONE		0x00U
#define LINKMGR_SCAN_PAGE		0x02U

static int linkmgr_write_scan(struct btd_linkmgr *linkmgr);
static int linkmgr_page_is(const struct btd_linkmgr *linkmgr, const uint8_t *address);

/*
 * Prepares the manager of a session: nobody wants page scan, no page is
 * out, and what the controller was told is not known.
 */
void
btd_linkmgr_init(
	struct btd_linkmgr *linkmgr,
	struct btd_session *session)
{
	/* Nothing wanted or begun. */
	memset(linkmgr, 0, sizeof(*linkmgr));
	linkmgr->session = session;
	linkmgr->scan_written = -1;
}

/*
 * Forgets what the controller was told and any page out (a controller
 * started or came back): the next want writes the page scan again.  Who
 * wants page scan is kept.
 */
void
btd_linkmgr_reset(
	struct btd_linkmgr *linkmgr)
{
	/* The controller's settings are not known; no page of the old controller is out. */
	linkmgr->scan_written = -1;
	linkmgr->scan_retry = 0;
	linkmgr->paging = 0;
	linkmgr->page_owner = 0U;
}

/*
 * Says whether one party wants page scan, and writes the controller's
 * setting when what anybody wants differs from what it was told.  Returns
 * 0, or the failed command's error (tried again at the next want or tick).
 */
int
btd_linkmgr_want_scan(
	struct btd_linkmgr *linkmgr,
	unsigned who,
	int on)
{
	int error;

	/* The party's want. */
	if (on)
		linkmgr->scan_wanted |= who;
	else
		linkmgr->scan_wanted &= ~who;

	/* The controller told, when it needs telling. */
	error = linkmgr_write_scan(linkmgr);
	if (error != 0)
		return error;

	/* Succeeded: the controller scans as wanted. */
	return 0;
}

/*
 * Begins a party's page of a device, before its Create Connection.
 * Returns 0 (the caller sends Create Connection now), or EBUSY when a page
 * is out already (the caller tries again later).
 */
int
btd_linkmgr_page_begin(
	struct btd_linkmgr *linkmgr,
	unsigned who,
	const uint8_t *address,
	uint64_t now_ms)
{
	/* One page at a time. */
	if (linkmgr->paging) {
		linkmgr->pages_refused++;
		return EBUSY;
	}

	/* Succeeded: the page is this party's. */
	linkmgr->paging = 1;
	linkmgr->page_owner = who;
	memcpy(linkmgr->page_address, address, BTD_ADDRESS_BYTES);
	linkmgr->page_started_ms = now_ms;
	return 0;
}

/*
 * Ends a party's page of a device (its Create Connection failed, it gave
 * up, it was cancelled).  The end of a page that is not out, or of another
 * device, is passed over: a page may be ended both by its caller and by
 * the router.
 */
void
btd_linkmgr_page_end(
	struct btd_linkmgr *linkmgr,
	unsigned who,
	const uint8_t *address)
{
	int same;

	UNUSED_PARAMETER(who);

	/* The page out, of that device (whoever began it: one Connection Complete ends any page of the device). */
	same = linkmgr_page_is(linkmgr, address);
	if (!same)
		return;

	/* Succeeded: no page out. */
	linkmgr->paging = 0;
	linkmgr->page_owner = 0U;
}

/*
 * Takes a BR/EDR ACL Connection Complete the router saw (success or
 * failure): a page of that device is over.
 */
void
btd_linkmgr_connected(
	struct btd_linkmgr *linkmgr,
	const uint8_t *address,
	uint8_t status)
{
	int same;

	UNUSED_PARAMETER(status);

	/* The page out, of that device: over whether it succeeded or failed. */
	same = linkmgr_page_is(linkmgr, address);
	if (!same)
		return;

	/* Succeeded: no page out. */
	linkmgr->paging = 0;
	linkmgr->page_owner = 0U;
}

/*
 * Tells whether a page is out.
 */
int
btd_linkmgr_paging(
	const struct btd_linkmgr *linkmgr)
{
	/* A page out. */
	if (linkmgr->paging)
		return 1;

	/* None. */
	return 0;
}

/*
 * Writes a page scan setting a refusal left unwritten, and ends a page
 * that has run past BTD_LINKMGR_PAGE_MS.  Returns 1 when it ended one (its
 * device in expired_address, for the caller's log), else 0.
 */
int
btd_linkmgr_tick(
	struct btd_linkmgr *linkmgr,
	uint64_t now_ms)
{
	uint64_t elapsed;

	/* A refused write of the page scan, tried again (its failure waits for the next tick). */
	if (linkmgr->scan_retry)
		(void)linkmgr_write_scan(linkmgr);

	/* No page out. */
	if (!linkmgr->paging)
		return 0;

	/* A page within its time (a clock that went back counts as none elapsed). */
	elapsed = 0U;
	if (now_ms > linkmgr->page_started_ms)
		elapsed = now_ms - linkmgr->page_started_ms;
	if (elapsed <= BTD_LINKMGR_PAGE_MS)
		return 0;

	/* Succeeded: the page is ended, and its device kept for the log. */
	memcpy(linkmgr->expired_address, linkmgr->page_address, BTD_ADDRESS_BYTES);
	linkmgr->paging = 0;
	linkmgr->page_owner = 0U;
	linkmgr->pages_expired++;
	return 1;
}

/*
 * Gives when the tick has work (the daemon's loop wakes then): a refused
 * page scan write's next try, a page's end of time; 0 when nothing waits.
 */
uint64_t
btd_linkmgr_deadline(
	const struct btd_linkmgr *linkmgr,
	uint64_t now_ms)
{
	uint64_t earliest;
	uint64_t expiry;

	/* A refused write, tried again soon. */
	earliest = 0U;
	if (linkmgr->scan_retry)
		earliest = now_ms + BTD_LINKMGR_RETRY_MS;

	/* A page out, ended just after its time. */
	if (linkmgr->paging) {
		expiry = linkmgr->page_started_ms + BTD_LINKMGR_PAGE_MS + 1U;
		if (earliest == 0U || expiry < earliest)
			earliest = expiry;
	}

	/* Succeeded: the earliest, or 0. */
	return earliest;
}

/* Writes Write Scan Enable when what anybody wants differs from what the controller was told. */
static int
linkmgr_write_scan(
	struct btd_linkmgr *linkmgr)
{
	uint8_t scan[1];
	int wanted;
	int error;

	/* Page scan while anybody wants it. */
	wanted = 0;
	scan[0] = LINKMGR_SCAN_NONE;
	if (linkmgr->scan_wanted != 0U) {
		wanted = 1;
		scan[0] = LINKMGR_SCAN_PAGE;
	}

	/* Told already. */
	if (linkmgr->scan_written == wanted) {
		linkmgr->scan_retry = 0;
		return 0;
	}

	/* The command; a refusal is tried again. */
	linkmgr->scan_writes++;
	error = btd_session_command(linkmgr->session, LINKMGR_WRITE_SCAN_ENABLE, scan, sizeof(scan));
	if (error != 0) {
		linkmgr->scan_failures++;
		linkmgr->scan_retry = 1;
		return error;
	}

	/* Succeeded: the controller scans as wanted. */
	linkmgr->scan_written = wanted;
	linkmgr->scan_retry = 0;
	return 0;
}

/* Tells whether the page out is of a device. */
static int
linkmgr_page_is(
	const struct btd_linkmgr *linkmgr,
	const uint8_t *address)
{
	int differs;

	/* No page out. */
	if (!linkmgr->paging)
		return 0;

	/* Another device's. */
	differs = memcmp(linkmgr->page_address, address, BTD_ADDRESS_BYTES);
	if (differs != 0)
		return 0;

	/* Succeeded: that device's. */
	return 1;
}
