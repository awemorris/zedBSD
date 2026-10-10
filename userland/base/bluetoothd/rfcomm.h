/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's RFCOMM (ws197-p002, plan/ws197/phase002/phase.md section 8):
 * the multiplexer of RFCOMM 1.2 over one L2CAP channel (PSM 0x0003) of one
 * ACL link, a subset of 3GPP TS 27.010 (GSM 07.10).  One session carries
 * the data link connections (DLCs) of both directions: the phone's server
 * channels (MAP's MAS, PBAP's PSE, HFP's AG) that bluetoothd opens, and
 * bluetoothd's own server channels (MAP's MNS, HFP's HF) that the phone
 * opens.  Flow is credit based (RFCOMM 1.2 section 6.5); a peer that does
 * not offer credits is refused.
 *
 * Without system calls: the owner hands in each L2CAP payload and the
 * clock, and sends what the session writes through its send hook; the host
 * tests build it.
 */

#ifndef BLUETOOTHD_RFCOMM_H
#define BLUETOOTHD_RFCOMM_H

#include <stddef.h>
#include <stdint.h>

/* How many DLCs one session holds, and how many server channels may be offered. */
#define BTD_RFCOMM_DLCS_MAX		6U
#define BTD_RFCOMM_SERVERS_MAX		4U

/* The server channels a DLCI can name (RFCOMM 1.2 section 5.4: 1 to 30). */
#define BTD_RFCOMM_CHANNEL_FIRST	1U
#define BTD_RFCOMM_CHANNEL_LAST		30U

/* The frame size of a DLC opened without a PN (RFCOMM 1.2 table 5.1), and the longest bluetoothd takes. */
#define BTD_RFCOMM_N1_DEFAULT		127U
#define BTD_RFCOMM_N1_MAX		1019U

/*
 * The bytes of frames the peer may send on credits bluetoothd gave and it
 * has not used yet, over the whole session: the bound of what can arrive
 * while the daemon waits for a command's answer (phase002 section 3.5).
 */
#define BTD_RFCOMM_CREDIT_BYTES		8192U

/* The most credits offered in a PN (the K field has 3 bits). */
#define BTD_RFCOMM_PN_CREDITS_MAX	7U

/*
 * The timers (RFCOMM 1.2 section 5.3): an answer to SABM or DISC on DLCI 0,
 * to the SABM of a DLC, and to a multiplexer command; and how long after
 * its last DLC closed the session is closed.
 */
#define BTD_RFCOMM_T1_MS		20000U
#define BTD_RFCOMM_T1_DLC_MS		60000U
#define BTD_RFCOMM_T2_MS		20000U
#define BTD_RFCOMM_IDLE_MS		2000U

/* How many malformed frames in a row end the session. */
#define BTD_RFCOMM_MALFORMED_MAX	4U

/* How many control frames wait when the send hook has no room, and the longest. */
#define BTD_RFCOMM_PENDING_MAX		8U
#define BTD_RFCOMM_CONTROL_MAX		24U

/* Why a DLC or the session closed. */
#define BTD_RFCOMM_CLOSED_LOCAL		1
#define BTD_RFCOMM_CLOSED_REMOTE	2
#define BTD_RFCOMM_CLOSED_REFUSED	3
#define BTD_RFCOMM_CLOSED_TIMEOUT	4
#define BTD_RFCOMM_CLOSED_ERROR		5
#define BTD_RFCOMM_CLOSED_LOST		6

/* The session's states: no session, our SABM on DLCI 0 sent, open, our DISC on DLCI 0 sent. */
#define BTD_RFCOMM_SESSION_CLOSED	0U
#define BTD_RFCOMM_SESSION_OPENING	1U
#define BTD_RFCOMM_SESSION_OPEN		2U
#define BTD_RFCOMM_SESSION_CLOSING	3U

/*
 * A DLC's states: free; waiting for the session to open (ours); our PN
 * sent; the peer's PN answered (theirs, before its SABM); our SABM sent;
 * open with the modem status still being exchanged; connected (data may
 * flow); our DISC sent.
 */
#define BTD_RFCOMM_DLC_FREE		0U
#define BTD_RFCOMM_DLC_WAITING		1U
#define BTD_RFCOMM_DLC_NEGOTIATING	2U
#define BTD_RFCOMM_DLC_NEGOTIATED	3U
#define BTD_RFCOMM_DLC_CONNECTING	4U
#define BTD_RFCOMM_DLC_OPEN		5U
#define BTD_RFCOMM_DLC_CONNECTED	6U
#define BTD_RFCOMM_DLC_CLOSING		7U

/*
 * What the session tells its owner, and how it writes.  send writes one
 * L2CAP payload on the session's channel and returns 0, ENOBUFS when it
 * has no room now (the session keeps a control frame to write later and
 * tells a writer of data that nothing more went), or another error.
 * accept says whether a server channel of bluetoothd's is offered now (1)
 * or not (0).  opened, data, writable and closed are a DLC's (named by its
 * DLCI); ended is the session's: the owner closes the L2CAP channel.  data
 * is processed before it returns: its frame's credit goes back then.
 */
struct btd_rfcomm_events {
	void *context;
	int (*send)(void *context, const uint8_t *payload, size_t length);
	int (*accept)(void *context, unsigned server_channel);
	void (*opened)(void *context, unsigned dlci);
	void (*data)(void *context, unsigned dlci, const uint8_t *data, size_t length);
	void (*writable)(void *context, unsigned dlci);
	void (*closed)(void *context, unsigned dlci, int reason);
	void (*ended)(void *context, int reason);
};

/*
 * One data link connection: its DLCI and state, whether bluetoothd opened
 * it, its frame size, the credits each side holds (tx: frames bluetoothd
 * may still send; rx: frames the peer may still send on credits given and
 * not used), the modem status exchange, the deadline of the answer it
 * waits for (0: none), and the reason it is told it closed for once its
 * DISC is answered (0: closed as asked; BTD_RFCOMM_CLOSED_TIMEOUT for a
 * DLC given up after a timer ran out, ws197-p005 section 3.2).  Credits the peer used are given back by the
 * difference to the DLC's share, so a credit frame the send hook refused is
 * not lost: the next pump sends the same difference.
 */
struct btd_rfcomm_dlc {
	unsigned state;
	unsigned dlci;
	int ours;
	unsigned n1;
	unsigned tx_credits;
	unsigned rx_credits;
	int msc_sent;
	int msc_answered;
	int msc_received;
	uint64_t deadline;
	int close_reason;
};

/* A control frame the send hook had no room for, written again before anything else. */
struct btd_rfcomm_pending {
	size_t length;
	uint8_t bytes[BTD_RFCOMM_CONTROL_MAX];
};

/*
 * One RFCOMM session on one ACL link.  It lives in the phone link's record
 * from the L2CAP channel's opening to its closing; btd_rfcomm_init empties
 * it for each channel.  initiator says which side sent the SABM on DLCI 0,
 * which sets the direction bit and the C/R bits; credit_flow says that the
 * first PN agreed on credit based flow control (0: not agreed yet, and no
 * DLC opens without it).  failed holds why the session must end (0: it need
 * not), set where ending at once would pull the session from under its
 * caller; each entry point ends it on the way out.
 */
struct btd_rfcomm {
	struct btd_rfcomm_events events;
	unsigned state;
	int initiator;
	int credit_flow;
	unsigned mtu;
	uint64_t deadline;
	uint64_t idle_since;
	unsigned servers[BTD_RFCOMM_SERVERS_MAX];
	unsigned server_count;
	struct btd_rfcomm_dlc dlcs[BTD_RFCOMM_DLCS_MAX];
	unsigned pending_count;
	struct btd_rfcomm_pending pending[BTD_RFCOMM_PENDING_MAX];
	int failed;
	unsigned malformed_run;
	unsigned malformed;
	unsigned refused;
	unsigned overrun;
	uint8_t frame[BTD_RFCOMM_N1_MAX + 6U];
};

uint8_t btd_rfcomm_fcs(const uint8_t *bytes, size_t length);
void btd_rfcomm_init(struct btd_rfcomm *rf, const struct btd_rfcomm_events *events, unsigned mtu);
int btd_rfcomm_listen(struct btd_rfcomm *rf, unsigned server_channel);
int btd_rfcomm_start(struct btd_rfcomm *rf, uint64_t now);
int btd_rfcomm_connect(struct btd_rfcomm *rf, unsigned server_channel, uint64_t now, unsigned *dlci);
int btd_rfcomm_write(struct btd_rfcomm *rf, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
int btd_rfcomm_close(struct btd_rfcomm *rf, unsigned dlci, uint64_t now);
void btd_rfcomm_input(struct btd_rfcomm *rf, const uint8_t *payload, size_t length, uint64_t now);
void btd_rfcomm_pump(struct btd_rfcomm *rf);
void btd_rfcomm_tick(struct btd_rfcomm *rf, uint64_t now);
uint64_t btd_rfcomm_deadline(const struct btd_rfcomm *rf);
void btd_rfcomm_lost(struct btd_rfcomm *rf);
const struct btd_rfcomm_dlc *btd_rfcomm_dlc(const struct btd_rfcomm *rf, unsigned dlci);

#endif
