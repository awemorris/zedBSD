/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the profiles' share of the phone link (ws197-p005,
 * plan/ws197/phase005/phase.md section 3.1), built with the host's
 * compiler under ASan and UBSan, with a fake phone link and two fake
 * children.
 *
 *   children   three added, a fourth refused; ready and ended to all
 *   sdp        a query's answer to the child that asked; a second query
 *              busy; the phone link's error passed on; an answer nobody
 *              asked for dropped
 *   dlc        a DLC to its child: opened, data, room, closed; a DLC
 *              refused or given up told only as closed, by its channel;
 *              the phone's DLC and this side's on the same channel number
 *              apart; accept asked twice keeps one row
 *   busy       a channel busy while its DLC waits, is open, until it
 *              closes; the phone link's refusal frees the row; EEXIST is
 *              busy; a failure told inside the call; the rows full
 *   ended      the link's end frees every row
 *   nobody     a DLC opened for nobody is closed; a close for nobody
 *              dropped; a child not added refused
 *
 *   plan/ws197/tests/bt-phone-host-test.sh
 */

#include "userland/base/bluetoothd/phonemux.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/*
 * The fake phone link: what the next query and DLC are answered, whether
 * the next DLC's failure is told inside the call, and what was asked.
 */
struct link {
	int sdp_error;
	int open_error;
	int fail_inside;
	unsigned sdp_count;
	uint16_t sdp_uuid;
	unsigned open_count;
	unsigned open_channel;
	unsigned close_count;
	unsigned close_dlci;
};

/*
 * A fake child: whether it accepts server channel 16, and what it heard.
 */
struct child {
	int accepts;
	unsigned ready_count;
	unsigned ended_count;
	unsigned order;
	unsigned sdp_count;
	int sdp_error;
	unsigned opened_count;
	unsigned opened_dlci;
	unsigned opened_channel;
	int opened_ours;
	unsigned data_count;
	unsigned writable_count;
	unsigned closed_count;
	unsigned closed_dlci;
	int closed_reason;
	unsigned failed_count;
	unsigned failed_channel;
};

/* The fake link, the mux, its profile for the link, the children and their indexes. */
static struct link link;
static struct btd_phonemux mux;
static struct btd_phone_profile profile;
static struct child first;
static struct child second;
static unsigned first_index;
static unsigned second_index;

/* The order the children were told in. */
static unsigned told;

static void check(int condition, const char *what);
static int link_sdp(void *context, uint16_t uuid);
static int link_open(void *context, unsigned server_channel);
static void link_close(void *context, unsigned dlci);
static void child_ready(void *context);
static void child_ended(void *context);
static void child_sdp_done(void *context, const struct btd_sdp *sdp, int error);
static int child_accept(void *context, unsigned server_channel);
static void child_opened(void *context, unsigned dlci, unsigned server_channel, int ours);
static void child_data(void *context, unsigned dlci, const uint8_t *data, size_t length);
static void child_writable(void *context, unsigned dlci);
static void child_closed(void *context, unsigned dlci, int reason);
static void child_open_failed(void *context, unsigned server_channel);
static void child_profile(struct child *child, struct btd_phone_profile *child_profile);
static void setup(void);
static unsigned rows_used(void);
static void test_children(void);
static void test_sdp(void);
static void test_dlc(void);
static void test_busy(void);
static void test_ended(void);
static void test_nobody(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_children();
	test_sdp();
	test_dlc();
	test_busy();
	test_ended();
	test_nobody();

	/* The count of what failed. */
	printf("bt-phonemux-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check, and reports it when it failed. */
static void
check(
	int condition,
	const char *what)
{
	/* One more check. */
	checks++;

	/* A failure is reported. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* The fake link's SDP query: counted, answered as set. */
static int
link_sdp(
	void *context,
	uint16_t uuid)
{
	/* Counted. */
	(void)context;
	link.sdp_count++;
	link.sdp_uuid = uuid;

	/* The answer set. */
	return link.sdp_error;
}

/* The fake link's DLC: counted, answered as set, its failure told inside the call when asked. */
static int
link_open(
	void *context,
	unsigned server_channel)
{
	/* Counted. */
	(void)context;
	link.open_count++;
	link.open_channel = server_channel;

	/* The failure told at once, as a link whose session cannot start would. */
	if (link.fail_inside) {
		link.fail_inside = 0;
		profile.open_failed(profile.context, server_channel);
	}

	/* The answer set. */
	return link.open_error;
}

/* The fake link's close of a DLC: counted. */
static void
link_close(
	void *context,
	unsigned dlci)
{
	/* Counted. */
	(void)context;
	link.close_count++;
	link.close_dlci = dlci;
}

/* A child told the link is ready: counted, with its turn. */
static void
child_ready(
	void *context)
{
	struct child *child;

	/* Counted. */
	child = context;
	child->ready_count++;
	told++;
	child->order = told;
}

/* A child told the link ended: counted, with its turn. */
static void
child_ended(
	void *context)
{
	struct child *child;

	/* Counted. */
	child = context;
	child->ended_count++;
	told++;
	child->order = told;
}

/* A child's SDP answer: counted. */
static void
child_sdp_done(
	void *context,
	const struct btd_sdp *sdp,
	int error)
{
	struct child *child;

	/* Counted. */
	(void)sdp;
	child = context;
	child->sdp_count++;
	child->sdp_error = error;
}

/* A child's offer of a server channel: channel 16 when it accepts. */
static int
child_accept(
	void *context,
	unsigned server_channel)
{
	struct child *child;

	/* Its channel, when it accepts. */
	child = context;
	if (child->accepts && server_channel == 16U)
		return 1;

	/* Not offered. */
	return 0;
}

/* A child's DLC opened: counted. */
static void
child_opened(
	void *context,
	unsigned dlci,
	unsigned server_channel,
	int ours)
{
	struct child *child;

	/* Counted. */
	child = context;
	child->opened_count++;
	child->opened_dlci = dlci;
	child->opened_channel = server_channel;
	child->opened_ours = ours;
}

/* A child's data: counted. */
static void
child_data(
	void *context,
	unsigned dlci,
	const uint8_t *data,
	size_t length)
{
	struct child *child;

	/* Counted. */
	(void)dlci;
	(void)data;
	(void)length;
	child = context;
	child->data_count++;
}

/* A child's room to write: counted. */
static void
child_writable(
	void *context,
	unsigned dlci)
{
	struct child *child;

	/* Counted. */
	(void)dlci;
	child = context;
	child->writable_count++;
}

/* A child's DLC closed: counted. */
static void
child_closed(
	void *context,
	unsigned dlci,
	int reason)
{
	struct child *child;

	/* Counted. */
	child = context;
	child->closed_count++;
	child->closed_dlci = dlci;
	child->closed_reason = reason;
}

/* A child's DLC that could not be asked for: counted. */
static void
child_open_failed(
	void *context,
	unsigned server_channel)
{
	struct child *child;

	/* Counted. */
	child = context;
	child->failed_count++;
	child->failed_channel = server_channel;
}

/* Fills a child's profile with the fake child's hooks. */
static void
child_profile(
	struct child *child,
	struct btd_phone_profile *child_profile)
{
	/* Every hook. */
	memset(child_profile, 0, sizeof(*child_profile));
	child_profile->context = child;
	child_profile->ready = child_ready;
	child_profile->ended = child_ended;
	child_profile->sdp_done = child_sdp_done;
	child_profile->accept = child_accept;
	child_profile->opened = child_opened;
	child_profile->data = child_data;
	child_profile->writable = child_writable;
	child_profile->closed = child_closed;
	child_profile->open_failed = child_open_failed;
}

/* A fresh link, mux and two children, the first accepting channel 16. */
static void
setup(void)
{
	struct btd_phonemux_hooks hooks;
	struct btd_phone_profile child_hooks;
	int error;

	/* Nothing seen yet. */
	memset(&link, 0, sizeof(link));
	memset(&first, 0, sizeof(first));
	memset(&second, 0, sizeof(second));
	first.accepts = 1;
	told = 0U;

	/* The mux and its profile for the link. */
	memset(&hooks, 0, sizeof(hooks));
	hooks.sdp_query = link_sdp;
	hooks.dlc_open = link_open;
	hooks.dlc_close = link_close;
	btd_phonemux_init(&mux, &hooks);
	btd_phonemux_profile(&mux, &profile);

	/* The two children. */
	child_profile(&first, &child_hooks);
	error = btd_phonemux_add(&mux, &child_hooks, &first_index);
	check(error == 0 && first_index == 0U, "setup: first child");
	child_profile(&second, &child_hooks);
	error = btd_phonemux_add(&mux, &child_hooks, &second_index);
	check(error == 0 && second_index == 1U, "setup: second child");
}

/* Counts the rows in use. */
static unsigned
rows_used(void)
{
	unsigned index;
	unsigned count;

	/* Each row. */
	count = 0U;
	for (index = 0U; index < BTD_PHONEMUX_ROWS; index++) {
		/* In use. */
		if (mux.rows[index].used)
			count++;
	}

	/* The count. */
	return count;
}

/* Children added, told the link's readiness and end in order. */
static void
test_children(void)
{
	struct btd_phone_profile child_hooks;
	unsigned index;
	int error;

	/* A third added, a fourth refused. */
	setup();
	child_profile(&second, &child_hooks);
	error = btd_phonemux_add(&mux, &child_hooks, &index);
	check(error == 0 && index == 2U, "children: third added");
	error = btd_phonemux_add(&mux, &child_hooks, &index);
	check(error == ENOSPC, "children: fourth refused");

	/* Ready and ended to every child, in the order added. */
	setup();
	profile.ready(profile.context);
	check(first.ready_count == 1U && second.ready_count == 1U, "children: ready to both");
	check(first.order == 1U && second.order == 2U, "children: ready in order");
	profile.ended(profile.context);
	check(first.ended_count == 1U && second.ended_count == 1U, "children: ended to both");
	check(first.order == 3U && second.order == 4U, "children: ended in order");
}

/* SDP queries and their answers. */
static void
test_sdp(void)
{
	int error;

	/* The first child's query goes to the link. */
	setup();
	error = btd_phonemux_sdp_query(&mux, first_index, 0x1132U);
	check(error == 0 && link.sdp_count == 1U && link.sdp_uuid == 0x1132U, "sdp: first child's query asked");

	/* The second child's waits: busy, and the link not asked. */
	error = btd_phonemux_sdp_query(&mux, second_index, 0x112fU);
	check(error == EBUSY && link.sdp_count == 1U, "sdp: a second query busy");

	/* The answer to the first child alone. */
	profile.sdp_done(profile.context, NULL, 0);
	check(first.sdp_count == 1U && second.sdp_count == 0U, "sdp: answer to the child that asked");

	/* Then the second child's query, and its answer to it. */
	error = btd_phonemux_sdp_query(&mux, second_index, 0x112fU);
	check(error == 0 && link.sdp_count == 2U, "sdp: the second child's query after");
	profile.sdp_done(profile.context, NULL, ENOENT);
	check(second.sdp_count == 1U && second.sdp_error == ENOENT && first.sdp_count == 1U, "sdp: answer to the second child");

	/* The link's error passed on, nobody waiting. */
	link.sdp_error = EBUSY;
	error = btd_phonemux_sdp_query(&mux, first_index, 0x1132U);
	check(error == EBUSY && mux.sdp_child == BTD_PHONEMUX_NONE, "sdp: the link's busy passed on");

	/* An answer nobody asked for is dropped. */
	profile.sdp_done(profile.context, NULL, 0);
	check(first.sdp_count == 1U && second.sdp_count == 1U, "sdp: an answer nobody asked for dropped");
}

/* DLCs to their children. */
static void
test_dlc(void)
{
	int error;
	int offered;

	/* The first child's DLC to channel 5: opened, data, room, closed, to it alone. */
	setup();
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == 0 && link.open_channel == 5U, "dlc: asked for");
	profile.opened(profile.context, 10U, 5U, 1);
	check(first.opened_count == 1U && first.opened_dlci == 10U && first.opened_ours == 1, "dlc: opened to the first child");
	profile.data(profile.context, 10U, (const uint8_t *)"x", 1U);
	profile.writable(profile.context, 10U);
	check(first.data_count == 1U && first.writable_count == 1U, "dlc: data and room to it");
	check(second.data_count == 0U && second.opened_count == 0U, "dlc: nothing to the second child");
	profile.closed(profile.context, 10U, BTD_RFCOMM_CLOSED_REMOTE);
	check(first.closed_count == 1U && first.closed_reason == BTD_RFCOMM_CLOSED_REMOTE, "dlc: closed to it");
	check(rows_used() == 0U, "dlc: no longer followed");

	/* The second child's DLC to channel 6 refused (DM): told only as closed, by its channel. */
	error = btd_phonemux_dlc_open(&mux, second_index, 6U);
	check(error == 0, "dlc: second child's asked for");
	profile.closed(profile.context, 12U, BTD_RFCOMM_CLOSED_REFUSED);
	check(second.closed_count == 1U && second.closed_dlci == 12U && second.closed_reason == BTD_RFCOMM_CLOSED_REFUSED, "dlc: refused told to the child that asked");
	check(first.closed_count == 1U && rows_used() == 0U, "dlc: nothing to the other, the row gone");

	/* Given up (the phone's direction bit on the DLCI): found the same way. */
	error = btd_phonemux_dlc_open(&mux, second_index, 7U);
	check(error == 0, "dlc: channel 7 asked for");
	profile.closed(profile.context, 15U, BTD_RFCOMM_CLOSED_TIMEOUT);
	check(second.closed_count == 2U && second.closed_reason == BTD_RFCOMM_CLOSED_TIMEOUT, "dlc: given up told by its channel");

	/* The phone's DLC on bluetoothd's channel 16 and the second child's to the phone's channel 16: apart. */
	offered = profile.accept(profile.context, 16U);
	check(offered == 1, "dlc: channel 16 offered by the first child");
	offered = profile.accept(profile.context, 16U);
	check(offered == 1 && rows_used() == 1U, "dlc: accept asked twice keeps one row");
	error = btd_phonemux_dlc_open(&mux, second_index, 16U);
	check(error == 0 && rows_used() == 2U, "dlc: the phone's channel 16 asked for, another row");
	profile.opened(profile.context, 33U, 16U, 0);
	profile.opened(profile.context, 32U, 16U, 1);
	check(first.opened_count == 2U && first.opened_dlci == 33U && first.opened_ours == 0, "dlc: the phone's DLC to the child that accepted");
	check(second.opened_count == 1U && second.opened_dlci == 32U && second.opened_ours == 1, "dlc: this side's DLC to the child that asked");
	profile.closed(profile.context, 32U, BTD_RFCOMM_CLOSED_LOCAL);
	check(second.closed_count == 3U && second.closed_dlci == 32U, "dlc: this side's closed to its child");
	profile.closed(profile.context, 33U, BTD_RFCOMM_CLOSED_REMOTE);
	check(first.closed_count == 2U && first.closed_dlci == 33U, "dlc: the phone's closed to its child");

	/* A server channel no child offers. */
	offered = profile.accept(profile.context, 17U);
	check(offered == 0 && rows_used() == 0U, "dlc: channel 17 not offered");
}

/* A channel busy while its DLC is followed, and the link's refusals. */
static void
test_busy(void)
{
	unsigned channel;
	int error;

	/* Busy while waiting, while open, free after the close. */
	setup();
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == 0, "busy: asked for");
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == EBUSY && link.open_count == 1U, "busy: again while waiting");
	profile.opened(profile.context, 10U, 5U, 1);
	error = btd_phonemux_dlc_open(&mux, second_index, 5U);
	check(error == EBUSY, "busy: again while open (or closing)");
	profile.closed(profile.context, 10U, BTD_RFCOMM_CLOSED_LOCAL);
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == 0 && link.open_count == 2U, "busy: free after the close");

	/* The link refuses at once: its error, and the row gone. */
	setup();
	link.open_error = ENOTCONN;
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == ENOTCONN && rows_used() == 0U, "busy: the link's refusal frees the row");
	link.open_error = EEXIST;
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == EBUSY && rows_used() == 0U, "busy: EEXIST is busy, the row freed");

	/* A failure told inside the call: to the child, the row gone. */
	link.open_error = 0;
	link.fail_inside = 1;
	error = btd_phonemux_dlc_open(&mux, second_index, 6U);
	check(error == 0 && second.failed_count == 1U && second.failed_channel == 6U, "busy: failure inside the call told");
	check(rows_used() == 0U, "busy: and its row gone");

	/* open_failed later: to the child, the row gone. */
	error = btd_phonemux_dlc_open(&mux, first_index, 7U);
	check(error == 0, "busy: channel 7 asked for");
	profile.open_failed(profile.context, 7U);
	check(first.failed_count == 1U && rows_used() == 0U, "busy: open_failed to the child");

	/* Eight rows, the ninth refused. */
	for (channel = 1U; channel <= BTD_PHONEMUX_ROWS; channel++)
		(void)btd_phonemux_dlc_open(&mux, first_index, channel);
	error = btd_phonemux_dlc_open(&mux, first_index, 20U);
	check(rows_used() == BTD_PHONEMUX_ROWS && error == ENOSPC, "busy: the rows full");
}

/* The link's end frees every row. */
static void
test_ended(void)
{
	int error;

	/* Rows waiting and open, then the end. */
	setup();
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == 0, "ended: asked for");
	error = btd_phonemux_dlc_open(&mux, second_index, 6U);
	check(error == 0, "ended: another asked for");
	profile.opened(profile.context, 12U, 6U, 1);
	error = btd_phonemux_sdp_query(&mux, first_index, 0x1132U);
	check(error == 0, "ended: a query under way");
	profile.ended(profile.context);
	check(rows_used() == 0U && mux.sdp_child == BTD_PHONEMUX_NONE, "ended: nothing followed");

	/* The channels are free again. */
	error = btd_phonemux_dlc_open(&mux, first_index, 5U);
	check(error == 0, "ended: channel 5 free again");
}

/* What nobody owns. */
static void
test_nobody(void)
{
	int error;

	/* A DLC opened for nobody is closed. */
	setup();
	profile.opened(profile.context, 20U, 10U, 1);
	check(link.close_count == 1U && link.close_dlci == 20U, "nobody: opened for nobody, closed");
	check(first.opened_count == 0U && second.opened_count == 0U, "nobody: no child told");

	/* A close for nobody is dropped. */
	profile.closed(profile.context, 22U, BTD_RFCOMM_CLOSED_REMOTE);
	check(first.closed_count == 0U && second.closed_count == 0U, "nobody: a close for nobody dropped");

	/* Data for nobody is dropped. */
	profile.data(profile.context, 22U, (const uint8_t *)"x", 1U);
	check(first.data_count == 0U && second.data_count == 0U, "nobody: data for nobody dropped");

	/* A child not added. */
	error = btd_phonemux_dlc_open(&mux, 2U, 5U);
	check(error == EINVAL, "nobody: DLC of a child not added");
	error = btd_phonemux_sdp_query(&mux, 2U, 0x1132U);
	check(error == EINVAL, "nobody: query of a child not added");
}
