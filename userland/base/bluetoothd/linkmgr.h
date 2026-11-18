/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's manager of the BR/EDR links' controller-wide settings
 * (ws197-p002, plan/ws197/phase002/phase.md section 6): the page scan
 * that lets bonded devices connect, which the HID host and the phone link
 * both want, and the one BR/EDR page (Create Connection) that may be out
 * at a time, which the HID host, the pairing and the phone link take in
 * turn.
 *
 * Write Scan Enable is sent by the manager only: page scan is on while
 * anybody wants it and off when nobody does, written when that changes
 * (and again after a refusal, at the next want or tick).  A page is begun
 * by its caller before its Create Connection and ended by the caller on
 * every way out of it, by the router on the Connection Complete of its
 * device (success or failure), or by the tick when it has run past
 * BTD_LINKMGR_PAGE_MS (a caller that forgot to end it).  LE's connections
 * are not the manager's: the phone link uses BR/EDR only.
 */

#ifndef BLUETOOTHD_LINKMGR_H
#define BLUETOOTHD_LINKMGR_H

#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>

/* Who wants the page scan or pages: the HID host, the phone link, the pairing. */
#define BTD_LINKMGR_HID		0x01U
#define BTD_LINKMGR_PHONE	0x02U
#define BTD_LINKMGR_PAIR	0x04U

/* The longest a page may stay begun before the tick ends it, and how soon a refused page scan write is tried again (milliseconds). */
#define BTD_LINKMGR_PAGE_MS	15000U
#define BTD_LINKMGR_RETRY_MS	1000U

/*
 * The manager of a controller's session.  It lives in the daemon for the
 * daemon's life; btd_linkmgr_reset forgets what the controller was told
 * when a controller starts.
 *
 * scan_wanted holds who wants page scan; scan_written what the controller
 * was last told (-1: not known, so the next want writes it), scan_retry
 * that a write failed and is tried again.  paging says a page is out, of
 * page_owner to page_address since page_started_ms.  The counts are for
 * the log and the tests.
 */
struct btd_linkmgr {
	struct btd_session *session;
	unsigned scan_wanted;
	int scan_written;
	int scan_retry;
	int paging;
	unsigned page_owner;
	uint8_t page_address[BTD_ADDRESS_BYTES];
	uint64_t page_started_ms;
	uint8_t expired_address[BTD_ADDRESS_BYTES];
	unsigned scan_writes;
	unsigned scan_failures;
	unsigned pages_refused;
	unsigned pages_expired;
};

void btd_linkmgr_init(struct btd_linkmgr *linkmgr, struct btd_session *session);
void btd_linkmgr_reset(struct btd_linkmgr *linkmgr);
int btd_linkmgr_want_scan(struct btd_linkmgr *linkmgr, unsigned who, int on);
int btd_linkmgr_page_begin(struct btd_linkmgr *linkmgr, unsigned who, const uint8_t *address, uint64_t now_ms);
void btd_linkmgr_page_end(struct btd_linkmgr *linkmgr, unsigned who, const uint8_t *address);
void btd_linkmgr_connected(struct btd_linkmgr *linkmgr, const uint8_t *address, uint8_t status);
int btd_linkmgr_paging(const struct btd_linkmgr *linkmgr);
int btd_linkmgr_tick(struct btd_linkmgr *linkmgr, uint64_t now_ms);
uint64_t btd_linkmgr_deadline(const struct btd_linkmgr *linkmgr, uint64_t now_ms);

#endif
