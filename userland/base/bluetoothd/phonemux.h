/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The profiles' share of the phone link (ws197-p005, plan/ws197/
 * phase005/phase.md section 3.1).  The phone link takes one profile
 * (struct btd_phone_profile); the mux is that profile, and hands each of
 * its children -- MAP, PBAP, later HFP -- what is theirs: the link's
 * readiness and end to all of them, an SDP query's answer to the child
 * that asked, and each DLC's opening, data, room and close to the child
 * that opened it (or that accepted the phone's DLC on one of bluetoothd's
 * server channels).
 *
 * A DLC is followed in a row from the moment a child asks for it (or
 * accepts it) until it closes.  A DLC this side asked for that RFCOMM
 * refuses or gives up on is told only as closed, with no opening; the
 * row of its server channel names the child.  A child does not close a
 * DLC of its own that has not opened yet (RFCOMM would tell nobody); it
 * waits for the opening or the close.  Asking for a DLC to a server
 * channel whose earlier DLC is still followed is refused as busy, so
 * that the earlier one's late close is never taken for the new one's.
 *
 * Without system calls; the host tests build it.
 */

#ifndef BLUETOOTHD_PHONEMUX_H
#define BLUETOOTHD_PHONEMUX_H

#include "userland/base/bluetoothd/phone.h"

#include <stddef.h>
#include <stdint.h>

/* How many profiles share the link, and how many DLCs are followed at once. */
#define BTD_PHONEMUX_CHILDREN		3U
#define BTD_PHONEMUX_ROWS		8U

/* No child: no SDP query under way. */
#define BTD_PHONEMUX_NONE		0xffffffffU

/*
 * What the mux asks of the phone link: an SDP query, a DLC to the phone's
 * server channel, the close of a DLC no child owns, and a line to log
 * (NULL: none).  The daemon points them at btd_phone_sdp_query,
 * btd_phone_dlc_open and btd_phone_dlc_close; the host tests at fakes.
 */
struct btd_phonemux_hooks {
	void *context;
	int (*sdp_query)(void *context, uint16_t uuid);
	int (*dlc_open)(void *context, unsigned server_channel);
	void (*dlc_close)(void *context, unsigned dlci);
	void (*log)(void *context, const char *line);
};

/*
 * One DLC followed: its server channel, whether this side opened it, its
 * DLCI once it opened (0 before), and its child.  A row whose used is 0
 * is free.
 */
struct btd_phonemux_row {
	int used;
	unsigned channel;
	int ours;
	unsigned dlci;
	unsigned child;
};

/*
 * The mux: its hooks, its children's profiles in the order added, the
 * child whose SDP query is under way (BTD_PHONEMUX_NONE: none), and the
 * DLCs followed.  It lives in the daemon for the daemon's life.
 */
struct btd_phonemux {
	struct btd_phonemux_hooks hooks;
	struct btd_phone_profile children[BTD_PHONEMUX_CHILDREN];
	unsigned child_count;
	unsigned sdp_child;
	struct btd_phonemux_row rows[BTD_PHONEMUX_ROWS];
};

void btd_phonemux_init(struct btd_phonemux *mux, const struct btd_phonemux_hooks *hooks);
int btd_phonemux_add(struct btd_phonemux *mux, const struct btd_phone_profile *child, unsigned *index);
void btd_phonemux_profile(struct btd_phonemux *mux, struct btd_phone_profile *profile);
int btd_phonemux_sdp_query(struct btd_phonemux *mux, unsigned child, uint16_t uuid);
int btd_phonemux_dlc_open(struct btd_phonemux *mux, unsigned child, unsigned server_channel);

#endif
