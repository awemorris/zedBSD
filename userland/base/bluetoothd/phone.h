/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * bluetoothd's phone link (ws197-p002 and p003, plan/ws197/phase002/
 * phase.md sections 7.4, 7.5 and 11, phase003 sections 3 and 5): the one
 * phone used as a phone, its owner's record, and its ACL link: handed
 * over by a phone's pairing (PAIR ... phone=1), paged by bluetoothd while
 * the owner sits at the seat, or accepted when the phone connects; then
 * authenticated, encrypted with a key of 16 bytes, and ready.  On the
 * link: its L2CAP channels, an SDP server on each SDP channel the phone
 * opens, SDP queries of the phone's records, one RFCOMM session and its
 * DLCs, and the frames sent, queued within the link's share of the
 * controller's buffers.
 *
 * The profiles (MAP from p003; PBAP and HFP later) use the link through
 * struct btd_phone_profile: the link's readiness and end, their SDP query,
 * their DLCs.
 *
 * Without system calls besides the session's and the records' files; the
 * host tests build it.
 */

#ifndef BLUETOOTHD_PHONE_H
#define BLUETOOTHD_PHONE_H

#include "userland/base/bluetoothd/acl.h"
#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/l2cap.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/phonerec.h"
#include "userland/base/bluetoothd/rfcomm.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/sdp.h"
#include "userland/base/bluetoothd/sdps.h"
#include "userland/base/bluetoothd/session.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * The phone link's states (ws197-p003 section 5.1): no link, ready, its
 * disconnection asked for, bluetoothd's page out, that page being
 * cancelled, the phone's connection accepted, the link being secured.
 */
#define BTD_PHONE_NONE		0U
#define BTD_PHONE_READY		1U
#define BTD_PHONE_CLOSING	2U
#define BTD_PHONE_PAGING	3U
#define BTD_PHONE_CANCELLING	4U
#define BTD_PHONE_ACCEPTING	5U
#define BTD_PHONE_SECURING	6U

/* The steps of securing a link (section 5.4): waiting for the phone's own encryption, authentication, encryption, the key's size. */
#define BTD_PHONE_SECURE_WAIT_PEER	1U
#define BTD_PHONE_SECURE_AUTH		2U
#define BTD_PHONE_SECURE_ENCRYPT	3U
#define BTD_PHONE_SECURE_KEY_SIZE	4U

/*
 * The times (milliseconds, section 5): a page, its cancel, an accepted
 * connection, the securing, the wait for the phone's own encryption, the
 * wait before authentication is asked again after a collision, how long a
 * link lasts to count as stable, how long a refusal's own Connection
 * Complete is awaited, and the short wait for a busy page or link.
 */
#define BTD_PHONE_PAGE_MS		12000U
#define BTD_PHONE_CANCEL_MS		5000U
#define BTD_PHONE_ACCEPT_MS		15000U
#define BTD_PHONE_SECURE_MS		10000U
#define BTD_PHONE_PEER_MS		3000U
#define BTD_PHONE_AUTH_AGAIN_MS		200U
#define BTD_PHONE_STABLE_MS		120000U
#define BTD_PHONE_REJECT_MS		3000U
#define BTD_PHONE_WAIT_MS		2000U

/* The steps of the wait between pages (0, 30 s ... 600 s, section 5.2), and how many short links the phone ends before bluetoothd stops paging it. */
#define BTD_PHONE_BACKOFF_STEPS		7U
#define BTD_PHONE_PEER_CLOSED_MAX	3U

/* What follows a link bluetoothd ended (section 5.5): no page, the next step's wait, the longest wait. */
#define BTD_PHONE_AFTER_NONE		0U
#define BTD_PHONE_AFTER_STEP		1U
#define BTD_PHONE_AFTER_LONG		2U

/* How many DLCs a profile may ask for before the RFCOMM session is up. */
#define BTD_PHONE_PENDING_DLCS		2U

/* How many frames wait for the session, and how many SDP channels the phone may open at a time. */
#define BTD_PHONE_QUEUE		16U
#define BTD_PHONE_SDP_SERVERS	2U

/* How many HID devices may be connected while a phone is wanted (section 7.5: the session's 8 links less pairing, refusal and the phone). */
#define BTD_PHONE_HID_LIMIT	5U

/*
 * What the phone link asks of the daemon (ws197-p003): the name of a
 * uid's account, which tells whether the owner of a record is still the
 * account that made it.
 */
struct btd_phone_hooks {
	void *context;
	btd_phonerec_account_fn account;
};

/*
 * A profile on the phone's link (ws197-p003 section 5.8: MAP; PBAP and HFP
 * later): told when the link is ready for it (secured, its owner at the
 * seat) and when it ends, the end of its SDP query (error 0: the records
 * in sdp), whether a server channel of bluetoothd's is offered to the
 * phone, and its DLCs: opened, data, writable again, closed (a DLC asked
 * for whose RFCOMM session never came up is told as open_failed with its
 * server channel).
 */
struct btd_phone_profile {
	void *context;
	void (*ready)(void *context);
	void (*ended)(void *context);
	void (*sdp_done)(void *context, const struct btd_sdp *sdp, int error);
	int (*accept)(void *context, unsigned server_channel);
	void (*opened)(void *context, unsigned dlci);
	void (*data)(void *context, unsigned dlci, const uint8_t *data, size_t length);
	void (*writable)(void *context, unsigned dlci);
	void (*closed)(void *context, unsigned dlci, int reason);
	void (*open_failed)(void *context, unsigned server_channel);
};

/* One L2CAP frame waiting for the session: its channel (the phone's end of it) and payload. */
struct btd_phone_frame {
	uint16_t cid;
	size_t length;
	uint8_t bytes[BTD_L2CAP_MAX];
};

/* The SDP server of one SDP channel the phone opened (cid 0: the slot is free). */
struct btd_phone_sdps {
	uint16_t cid;
	struct btd_sdps server;
};

/*
 * The phone link of a controller's session: the session, the router, the
 * HID host (whose connections it limits while a phone is wanted), the SDP
 * records offered, the bonds' folder and the daemon's hooks; the record
 * and the seat; the pages; and the one phone's link: its state, device,
 * handle, the uid that paired it, its channels and frame, the SDP servers,
 * the SDP query, the RFCOMM session and its channel, the profile, and the
 * frames waiting.  It lives in the daemon for the daemon's life; a link
 * fills it from its page, acceptance or handoff to its end.
 */
struct btd_phone {
	struct btd_session *session;
	struct btd_router *router;
	struct btd_hid *hid;
	const struct btd_sdps_db *db;
	const char *keys_folder;
	struct btd_phone_hooks hooks;

	/*
	 * The record of the phone used as a phone (ws197-p003 section 3),
	 * read when the controller opens and kept as it is written: whether
	 * there is one, whether it is valid (an invalid record owns nothing),
	 * and the record.
	 */
	int have_record;
	int record_valid;
	struct btd_phonerec record;

	/*
	 * The seat (section 3.3): whether the daemon knows its user, the
	 * user's uid, and whether the record's owner is there (present: the
	 * phone is wanted).
	 */
	int have_seat;
	uid_t seat_uid;
	int present;

	/*
	 * Bluetoothd's pages (section 5.2): the step of the wait, when the
	 * next page may go, whether pages stopped until the next sign (the
	 * phone's user ended short links, or its bond is gone), how many short
	 * links the phone ended in a row, and why the link last ended.
	 */
	unsigned backoff_step;
	uint64_t next_page_at;
	int stopped;
	unsigned peer_closed;
	const char *why;

	/*
	 * Making the link (sections 5.2 to 5.5): whether the phone connected
	 * by itself, whether bluetoothd's Create Connection still has its
	 * Connection Complete to come, whether the link is to end as soon as it
	 * is up, the state's deadline, the securing's step and deadlines and
	 * whether authentication was asked again, when the link became ready,
	 * what follows a link bluetoothd ends, and a refusal whose own
	 * Connection Complete is awaited (its device and until when).
	 */
	int inbound;
	int page_outstanding;
	int stop_wanted;
	uint64_t state_deadline;
	unsigned secure_step;
	uint64_t secure_deadline;
	uint64_t auth_again_at;
	int auth_retried;
	uint64_t ready_since;
	unsigned after;
	int reject_pending;
	uint8_t reject_address[BTD_ADDRESS_BYTES];
	uint64_t reject_until;

	/*
	 * The profile (section 5.8): its hooks, whether it was told the link
	 * is ready, its SDP query's class (0: none), and the server channels of
	 * its DLCs asked for before the RFCOMM session is up.
	 */
	int have_profile;
	struct btd_phone_profile profile;
	int profile_started;
	uint16_t sdp_uuid;
	unsigned pending_count;
	unsigned pending_channels[BTD_PHONE_PENDING_DLCS];

	/*
	 * The phone: where its link is, its device, its handle, who paired
	 * it, the link's encryption, and whether the scan knew no Class of
	 * Device for it (taken as a phone because the user asked, section
	 * 7.4; the daemon logs it).
	 */
	unsigned state;
	uint8_t address[BTD_ADDRESS_BYTES];
	uint16_t handle;
	uid_t uid;
	int encrypted;
	unsigned key_size;
	int class_unknown;

	/* The link's channels, its frame being put together, and the channel the last frame began on (0: none). */
	struct btd_l2cap l2cap;
	struct btd_reassembly reassembly;
	uint16_t frame_cid;

	/* The SDP servers, the query of the phone's records (its channel: 0 for none) and its transaction. */
	struct btd_phone_sdps sdps[BTD_PHONE_SDP_SERVERS];
	uint16_t sdp_cid;
	uint16_t sdp_transaction;
	struct btd_sdp sdp;

	/*
	 * The RFCOMM session (its channel: 0 for none; active once the channel
	 * opened), and bluetoothd's channel's requests: how many went, whether
	 * the phone just refused one, and when the next goes (0: none waits).
	 */
	uint16_t rfcomm_cid;
	int rfcomm_active;
	struct btd_rfcomm rfcomm;
	unsigned rfcomm_tries;
	int rfcomm_refused;
	uint64_t rfcomm_retry_at;

	/* The frames waiting for room in the session, a ring: the oldest's slot and how many. */
	unsigned queue_first;
	unsigned queue_count;
	struct btd_phone_frame queue[BTD_PHONE_QUEUE];

	/* The counters: frames dropped for a full queue, notices, links refused. */
	unsigned queue_dropped;
	unsigned notices;
	unsigned refused;
};

void btd_phone_init(struct btd_phone *phone, struct btd_session *session, struct btd_router *router, struct btd_hid *hid, const struct btd_sdps_db *db, const char *keys_folder, const struct btd_phone_hooks *hooks);
void btd_phone_set_profile(struct btd_phone *phone, const struct btd_phone_profile *profile);
void btd_phone_set_seat(struct btd_phone *phone, int have_seat, uid_t uid, uint64_t now);
void btd_phone_resume(struct btd_phone *phone, uint64_t now);
int btd_phone_sdp_query(struct btd_phone *phone, uint16_t uuid);
int btd_phone_dlc_open(struct btd_phone *phone, unsigned server_channel, uint64_t now);
int btd_phone_dlc_write(struct btd_phone *phone, unsigned dlci, const uint8_t *data, size_t length, size_t *written);
int btd_phone_dlc_close(struct btd_phone *phone, unsigned dlci, uint64_t now);
int btd_phone_load(struct btd_phone *phone);
const char *btd_phone_pair_check(struct btd_phone *phone, const uint8_t *address, uid_t uid, int phone_pairing, int seated);
int btd_phone_link_set(struct btd_phone *phone, const uint8_t *address, uid_t uid, int on, int profiles);
int btd_phone_forget(struct btd_phone *phone, const uint8_t *address, uid_t uid);
int btd_phone_show(const struct btd_phone *phone, uid_t uid, int permitted, char *line, size_t size);
int btd_phone_handoff(void *context, const struct btd_pair_handoff *handoff, const char **why);
int btd_phone_wants(void *context, const uint8_t *address);
int btd_phone_claims(void *context, const uint8_t *address);
void btd_phone_handle(void *context, struct btd_session *session, const uint8_t *packet, size_t length);
void btd_phone_pump(struct btd_phone *phone);
void btd_phone_tick(struct btd_phone *phone, uint64_t now);
uint64_t btd_phone_deadline(const struct btd_phone *phone);
void btd_phone_lost(struct btd_phone *phone);
int btd_phone_drop(struct btd_phone *phone, const uint8_t *address);
int btd_phone_owns(const struct btd_phone *phone, uint16_t handle);

#endif
