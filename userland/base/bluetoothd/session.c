/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One controller's session in bluetoothd (ws143-p003 and ws143-p004, see
 * session.h, plan/ws143/phase003/phase.md sections 2 and 3, and
 * plan/ws143/phase004/phase.md section 10).
 *
 * Commands go one at a time: the session writes one and reads until its
 * answer (a Command Complete or a Command Status with its opcode) comes or
 * its time runs out; an answer to another command is passed over and
 * counted, and the other packets read meanwhile (a scan's results, a
 * connection's events and ACL data) are queued and handled after it, from
 * btd_session_input.
 */

#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The standard commands of the start and the scan (OGF and OCF as the Core names them). */
#define SESSION_INQUIRY			0x0401U
#define SESSION_INQUIRY_CANCEL		0x0402U
#define SESSION_SET_EVENT_MASK		0x0c01U
#define SESSION_RESET			0x0c03U
#define SESSION_WRITE_INQUIRY_MODE	0x0c45U
#define SESSION_WRITE_SSP_MODE		0x0c56U
#define SESSION_WRITE_LE_HOST		0x0c6dU
#define SESSION_WRITE_SC_HOST		0x0c7aU
#define SESSION_READ_LOCAL_VERSION	0x1001U
#define SESSION_READ_COMMANDS		0x1002U
#define SESSION_READ_FEATURES		0x1003U
#define SESSION_READ_BUFFER_SIZE	0x1005U
#define SESSION_READ_ADDRESS		0x1009U
#define SESSION_LE_SET_EVENT_MASK	0x2001U
#define SESSION_LE_READ_BUFFER_SIZE	0x2002U
#define SESSION_LE_SCAN_PARAMETERS	0x200bU
#define SESSION_LE_SCAN_ENABLE		0x200cU

/*
 * The supported commands' bits (Core Vol 4 Part E §6.27): Inquiry (octet 0
 * bit 0), LE Set Event Mask (25, 0), LE Set Scan Parameters and Enable (26,
 * 2 and 3), LE Read Local P-256 Public Key and LE Generate DHKey (34, 1 and
 * 2).
 */
#define SESSION_BIT_INQUIRY_OCTET	0U
#define SESSION_BIT_INQUIRY		0U
#define SESSION_BIT_LE_MASK_OCTET	25U
#define SESSION_BIT_LE_MASK		0U
#define SESSION_BIT_LE_SCAN_OCTET	26U
#define SESSION_BIT_LE_SCAN_PARAMETERS	2U
#define SESSION_BIT_LE_SCAN_ENABLE	3U
#define SESSION_BIT_CRYPTO_OCTET	34U
#define SESSION_BIT_P256		1U
#define SESSION_BIT_DHKEY		2U

/* The LMP features' byte and bit of "LE Supported (Controller)" (Core Vol 2 Part C §3.3, page 0). */
#define SESSION_FEATURE_LE_BYTE		4U
#define SESSION_FEATURE_LE_BIT		6U

/* The LMP feature "Non-flushable Packet Boundary Flag" (page 0, byte 6, bit 6). */
#define SESSION_FEATURE_NO_FLUSH_BYTE	6U
#define SESSION_FEATURE_NO_FLUSH_BIT	6U

/* Data Buffer Overflow: the controller dropped ACL data the host sent past its buffers. */
#define SESSION_EVENT_BUFFER_OVERFLOW	0x1aU

/* A queued packet's record: its length (2 bytes) and its flags; the flag of a connection event counted already. */
#define SESSION_RECORD_HEADER		3U
#define SESSION_RECORD_COUNTED		0x01U

/* The Hardware Error event: the controller must be set up again. */
#define SESSION_EVENT_HARDWARE_ERROR	0x10U

/* The events whose connections the session counts: Connection Complete, Disconnection Complete, Number Of Completed Packets. */
#define SESSION_EVENT_CONNECTED		0x03U
#define SESSION_EVENT_DISCONNECTED	0x05U
#define SESSION_EVENT_COMPLETED_PACKETS	0x13U

/* LE's Connection Complete subevent, and Connection Complete's link type of an ACL connection. */
#define SESSION_LE_CONNECTED		0x01U
#define SESSION_LE_ENHANCED		0x0aU
#define SESSION_LINK_ACL		0x01U

/* A connection handle's bits. */
#define SESSION_HANDLE_MASK		0x0fffU

/*
 * ws197-p002: an ACL packet's packet boundary flag (bits 12 and 13 of its
 * handle's field) for a continuing packet, the offsets of its first
 * packet's L2CAP channel, the signalling channels of BR/EDR and LE, the
 * LE extended advertising report, Data Buffer Overflow, and the half of
 * the counters' range that tells a count due from one to come.
 */
#define SESSION_BOUNDARY_SHIFT		12U
#define SESSION_BOUNDARY_CONTINUING	0x01U
#define SESSION_ACL_CID			7U
#define SESSION_CID_SIGNALLING		0x0001U
#define SESSION_CID_LE_SIGNALLING	0x0005U
#define SESSION_LE_EXTENDED_REPORT	0x0dU
#define SESSION_DUE_RANGE		0x80000000U

/* The ACL pool assumed when the controller does not tell its own (one small packet at a time). */
#define SESSION_POOL_LENGTH_LEAST	27U

/* The status Inquiry Cancel answers when no inquiry runs (Command Disallowed), which is not a failure. */
#define SESSION_STATUS_DISALLOWED	0x0cU

/* How many Secure Send answers the trace keeps. */
#define SESSION_TRACE_ANSWERS		3U

/* The general inquiry access code, least significant byte first, and the most inquiry length (units of 1.28 s). */
#define SESSION_GIAC_0			0x33U
#define SESSION_GIAC_1			0x8bU
#define SESSION_GIAC_2			0x9eU
#define SESSION_INQUIRY_LENGTH_MAX	0x30U

/* The timing of a real controller (FreeBSD's iwmbtfw uses the same). */
#define SESSION_COMMAND_MS		2000U
#define SESSION_DOWNLOAD_MS		5000U
#define SESSION_BOOT_MS			5000U

static void session_set(struct btd_session *session, enum btd_state state, const char *format, ...) __attribute__((format(printf, 3, 4)));
static int session_write(struct btd_session *session, const uint8_t *packet, size_t length);
static int session_read(struct btd_session *session, unsigned timeout_ms);
static void session_dispatch(struct btd_session *session);
static int session_command(struct btd_session *session, uint16_t opcode, const uint8_t *parameters, size_t count, unsigned timeout_ms);
static int session_wait_vendor(struct btd_session *session, uint8_t code, unsigned timeout_ms);
static int session_step(struct btd_session *session, const char *name, uint16_t opcode, const uint8_t *parameters, size_t count);
static int session_intel(struct btd_session *session);
static int session_intel_version(struct btd_session *session);
static int session_intel_load(struct btd_session *session);
static int session_intel_send(struct btd_session *session, const uint8_t *file, const struct btd_intel_plan *plan);
static void session_intel_ddc(struct btd_session *session);
static int session_read_file(const char *path, uint8_t **contents, size_t *length);
static int session_core(struct btd_session *session);
static int session_remaining(uint64_t deadline);
static int session_control(struct btd_session *session, unsigned long request, void *argument);
static void session_normal_path(struct btd_session *session);
static void session_trace(struct btd_session *session, unsigned index);
static int session_scan_le(struct btd_session *session);
static void session_enqueue(struct btd_session *session);
static int session_dequeue(struct btd_session *session);
static void session_hand(struct btd_session *session);
static int session_counted_event(struct btd_session *session, const struct btd_event *event);
static void session_completed(struct btd_session *session, const struct btd_event *event);
static void session_link_add(struct btd_session *session, uint16_t handle, int le, const uint8_t *address);
static void session_link_reset(struct btd_link_count *link);
static int session_acl_arrival(struct btd_session *session);
static void session_mark_link(struct btd_session *session, struct btd_link_count *link);
static void session_mark_events(struct btd_session *session, uint8_t flags);
static int session_due(const struct btd_session *session, uint32_t after);
static int session_notice(struct btd_session *session);
static int session_notice_waiting(const struct btd_session *session);
static int session_report(const struct btd_event *event);
static int session_oldest(const struct btd_session *session, uint16_t handle, unsigned *index);
static void session_frame_done(struct btd_session *session, unsigned index);
static void session_link_remove(struct btd_session *session, uint16_t handle);
static struct btd_link_count *session_link(struct btd_session *session, uint16_t handle);
static struct btd_pool *session_pool(struct btd_session *session, const struct btd_link_count *link);
static int session_flush(struct btd_session *session);
static void session_core_buffers(struct btd_session *session);
static int session_scan_event(const struct btd_event *event);
static uint16_t session_handle(const uint8_t *bytes);

/*
 * Prepares a session on an open node (or the tests' socket): its ioctls
 * (NULL: none), no state yet, a real controller's timing, and the load of
 * Intel's firmware allowed.
 */
void
btd_session_init(
	struct btd_session *session,
	int descriptor,
	btd_control_fn control,
	void *control_context,
	const char *path,
	const char *firmware_folder)
{
	/* Nothing learnt yet. */
	memset(session, 0, sizeof(*session));
	session->descriptor = descriptor;
	session->control = control;
	session->control_context = control_context;
	(void)snprintf(session->path, sizeof(session->path), "%s", path);
	session->firmware_folder = firmware_folder;
	session->load_allowed = 1;

	/* A real controller's timing; the tests shorten it. */
	session->timing.command_ms = SESSION_COMMAND_MS;
	session->timing.download_ms = SESSION_DOWNLOAD_MS;
	session->timing.boot_ms = SESSION_BOOT_MS;
	session->state = BTD_STATE_STARTING;
}

/*
 * Starts the controller: Intel's firmware when it needs it, then the HCI
 * core's set-up.  Returns 0 when it is ready, ENODEV when its node went (a
 * re-enumeration, the daemon opens the new one), or another errno with the
 * state and reason set.
 */
int
btd_session_start(
	struct btd_session *session)
{
	int error;

	/* The node's information: who made it, its name, and the path it is on. */
	session->state = BTD_STATE_STARTING;
	if (session->control != NULL) {
		error = session_control(session, BT_IOC_GET_INFO, &session->info);
		if (error != 0) {
			session_set(session, BTD_STATE_ERROR, "information %s", strerror(error));
			return error;
		}

		/* The information is known. */
		session->have_info = 1;
	}

	/* A bootloader path left on (a daemon that died in a load) goes off first. */
	if (session->have_info && (session->info.flags & BT_INFO_BOOTLOADER) != 0U)
		session_normal_path(session);

	/* An Intel controller has its firmware looked at first. */
	if (session->have_info && session->info.vendor == BTD_INTEL_VENDOR) {
		session->intel = 1;
		error = session_intel(session);
		if (error != 0)
			return error;
	}

	/* The HCI core's set-up. */
	error = session_core(session);
	if (error != 0)
		return error;

	/* Succeeded: the controller is ready. */
	session_set(session, BTD_STATE_READY, "%s", "");
	return 0;
}

/*
 * Handles one packet: the oldest that came while a command waited, else
 * one the node has (a scan's result, an inquiry's end, a connection's
 * event or ACL data, handed to the handler).  For the daemon's loop when
 * the node is readable or btd_session_pending says packets wait.  Returns
 * 0, EAGAIN when there was none, or ENODEV when the node went.
 */
int
btd_session_input(
	struct btd_session *session)
{
	int noticed;
	int queued;
	int dropped;
	int error;

	/* A notice of dropped packets that is due goes before the packets that came after the drop (ws197-p002). */
	noticed = session_notice(session);
	if (noticed)
		return 0;

	/* A packet that came while a command waited goes first, in its order. */
	queued = session_dequeue(session);
	if (queued) {
		session_dispatch(session);
		return 0;
	}

	/* Else one packet from the node, without waiting. */
	error = session_read(session, 0U);
	if (error == ETIMEDOUT)
		return EAGAIN;
	if (error != 0)
		return error;

	/* ACL data of a sealed link, or continuing data after its notice, is dropped as it arrives. */
	if (session->packet[0] == BT_PACKET_ACL) {
		dropped = session_acl_arrival(session);
		if (dropped)
			return 0;
	}

	/* Handles it. */
	session_dispatch(session);

	/* Succeeded: handled. */
	return 0;
}

/*
 * Starts a scan for some seconds: BR/EDR's inquiry and, when the controller
 * has LE, a passive LE scan (an active one would send the controller's
 * public address to every advertiser; the private address is p004's).  A
 * controller that refuses the LE scan while it inquires scans BR/EDR
 * alone.  The table of devices starts empty.  Returns 0, EBUSY when the
 * controller is not ready (or scans already), EINVAL for a length outside 1
 * to BTD_SCAN_SECONDS_MAX, or the failed command's error.
 */
int
btd_session_scan_start(
	struct btd_session *session,
	unsigned seconds)
{
	uint8_t inquiry[5];
	unsigned length;
	int inquiry_bit;
	int error;

	/* A ready controller and a length in range. */
	if (session->state != BTD_STATE_READY)
		return EBUSY;
	if (seconds == 0U || seconds > BTD_SCAN_SECONDS_MAX)
		return EINVAL;

	/* An empty table, filled from now on (the results may come while the scan's commands are answered). */
	btd_devices_clear(&session->devices);
	session->scanning = 1;

	/* The inquiry, as long as the scan (units of 1.28 s, rounded up), unless the controller has none. */
	inquiry_bit = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_INQUIRY_OCTET, SESSION_BIT_INQUIRY);
	if (!session->have_commands || inquiry_bit) {
		length = (seconds * 100U + 127U) / 128U;
		if (length > SESSION_INQUIRY_LENGTH_MAX)
			length = SESSION_INQUIRY_LENGTH_MAX;
		inquiry[0] = SESSION_GIAC_0;
		inquiry[1] = SESSION_GIAC_1;
		inquiry[2] = SESSION_GIAC_2;
		inquiry[3] = (uint8_t)length;
		inquiry[4] = 0U;
		error = session_command(session, SESSION_INQUIRY, inquiry, sizeof(inquiry), session->timing.command_ms);
		if (error != 0) {
			session->scanning = 0;
			return error;
		}

		/* The inquiry runs. */
		session->inquiring = 1;
	}

	/* The LE scan, when the controller has LE. */
	if (session->le) {
		error = session_scan_le(session);

		/* A refusal leaves the inquiry alone; a node that went, or no inquiry, ends the scan. */
		if (error == ENODEV || (error != 0 && !session->inquiring)) {
			session->scanning = 0;
			return error;
		}
	}

	/* Succeeded: the scan runs until its end. */
	session->scan_end_ms = btd_now_ms() + (uint64_t)seconds * 1000U;
	session_set(session, BTD_STATE_SCANNING, "%s", "");
	return 0;
}

/*
 * Ends a scan: cancels an inquiry still running and turns the LE scan off.
 * The table keeps what was found.  Returns 0, or the error of a command
 * that failed (the scan is over all the same).
 */
int
btd_session_scan_stop(
	struct btd_session *session)
{
	uint8_t disable[2];
	int first_error;
	int error;

	/* Nothing runs. */
	if (!session->scanning)
		return 0;

	/* An inquiry still running is cancelled (one that ended answers with an error, which does not matter). */
	first_error = 0;
	if (session->inquiring) {
		error = session_command(session, SESSION_INQUIRY_CANCEL, NULL, 0U, session->timing.command_ms);
		if (error == ENODEV)
			first_error = error;
		session->inquiring = 0;
	}

	/* The LE scan off. */
	if (session->le_scanning) {
		disable[0] = 0x00U;
		disable[1] = 0x00U;
		error = session_command(session, SESSION_LE_SCAN_ENABLE, disable, sizeof(disable), session->timing.command_ms);
		if (error != 0 && first_error == 0)
			first_error = error;
		session->le_scanning = 0;
	}

	/* The scan is over. */
	session->scanning = 0;
	if (session->state == BTD_STATE_SCANNING)
		session_set(session, BTD_STATE_READY, "%s", "");

	/* Reports a command that failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded: the scan ended. */
	return 0;
}

/*
 * Tells whether packets that came while a command waited are queued (the
 * daemon's loop then does not wait on the node before handling them).
 */
int
btd_session_pending(
	const struct btd_session *session)
{
	int waiting;

	/* Packets wait in the queue. */
	if (session->queue_used != 0U)
		return 1;

	/* A notice of dropped packets waits (with the queue empty, it is due). */
	waiting = session_notice_waiting(session);
	if (waiting)
		return 1;

	/* Nothing queued. */
	return 0;
}

/*
 * Sends a command for the handler and waits for its answer: 0 with the
 * return parameters in session->returned (none for a Command Status), EIO
 * for a status that is not 0 (in session->status), ETIMEDOUT, ECONNRESET,
 * or the node's error.  The packets that come meanwhile are queued.
 */
int
btd_session_command(
	struct btd_session *session,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	int error;

	/* The command, within a command's time. */
	error = session_command(session, opcode, parameters, count, session->timing.command_ms);
	if (error != 0)
		return error;

	/* Succeeded: answered with status 0. */
	return 0;
}

/*
 * Sends an L2CAP frame (a channel and its payload) on a connection, cut
 * into the packets its pool takes as the controller's buffers allow; what
 * does not go now waits for Number Of Completed Packets.  Returns 0,
 * ENOTCONN for a connection the session does not know, EMSGSIZE for a
 * payload over BTD_L2CAP_MAX, ENOBUFS when BTD_SEND_FRAMES frames wait
 * already, or the node's error.
 */
int
btd_session_send(
	struct btd_session *session,
	uint16_t handle,
	uint16_t cid,
	const uint8_t *payload,
	size_t length)
{
	struct btd_link_count *link;
	struct btd_frame *frame;
	int error;

	/* A connection the controller made. */
	link = session_link(session, handle);
	if (link == NULL)
		return ENOTCONN;

	/* A payload bluetoothd sends at all. */
	if (length > BTD_L2CAP_MAX)
		return EMSGSIZE;

	/* Room among the waiting frames (ws197-p002: a phone's link leaves room to the others within its share). */
	if (session->frame_count >= BTD_SEND_FRAMES) {
		session->frames_dropped++;
		return ENOBUFS;
	}

	/* Within the link's own share. */
	if (link->frames >= link->frame_limit) {
		session->frames_dropped++;
		return ENOBUFS;
	}

	/* The frame, at the end of those waiting. */
	frame = &session->frames[session->frame_count];
	frame->handle = handle;
	frame->sent = 0U;
	frame->length = btd_l2cap_frame(frame->bytes, sizeof(frame->bytes), cid, payload, length);
	session->frame_count++;
	link->frames++;

	/* As much as the buffers take now. */
	error = session_flush(session);
	if (error != 0)
		return error;

	/* Succeeded: sent, or waiting for buffers. */
	return 0;
}

/*
 * Sets the most frames a connection may have waiting (0: as many as the
 * session holds) and in the controller (0: as many as its pool holds), for
 * a phone's link (ws197-p002, plan/ws197/phase002/phase.md section 4.2).
 * Returns 0, or ENOTCONN for a connection the session does not know.
 */
int
btd_session_set_link_limits(
	struct btd_session *session,
	uint16_t handle,
	unsigned frame_limit,
	unsigned inflight_limit)
{
	struct btd_link_count *link;

	/* A connection the controller made. */
	link = session_link(session, handle);
	if (link == NULL)
		return ENOTCONN;

	/* The frames waiting: the session's whole table at most. */
	link->frame_limit = frame_limit;
	if (link->frame_limit == 0U || link->frame_limit > BTD_SEND_FRAMES)
		link->frame_limit = BTD_SEND_FRAMES;

	/* Succeeded: the packets in the controller (0: the pool alone bounds them). */
	link->inflight_limit = inflight_limit;
	return 0;
}

/*
 * Tells how many more frames a connection may queue now (0 for a
 * connection the session does not know).
 */
unsigned
btd_session_link_room(
	const struct btd_session *session,
	uint16_t handle)
{
	const struct btd_link_count *link;
	unsigned own;
	unsigned shared;
	unsigned index;

	/* The connection. */
	link = NULL;
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		if (session->links[index].used && session->links[index].handle == handle)
			link = &session->links[index];
	}

	/* Not counted. */
	if (link == NULL)
		return 0U;

	/* Its own share and the table's room. */
	own = 0U;
	if (link->frames < link->frame_limit)
		own = link->frame_limit - link->frames;
	shared = BTD_SEND_FRAMES - session->frame_count;

	/* Succeeded: the smaller. */
	if (own < shared)
		return own;
	return shared;
}

/*
 * Lists the connections the session counts, their handles and addresses
 * (at most max), for the router's check of its routes after a lost
 * connection event (ws197-p002 section 3.5).  Returns the count listed.
 */
unsigned
btd_session_links(
	const struct btd_session *session,
	uint16_t *handles,
	uint8_t (*addresses)[BTD_ADDRESS_BYTES],
	unsigned max)
{
	unsigned index;
	unsigned count;

	/* Each connection counted. */
	count = 0U;
	for (index = 0U; index < BTD_LINKS_MAX && count < max; index++) {
		if (!session->links[index].used)
			continue;
		handles[count] = session->links[index].handle;
		memcpy(addresses[count], session->links[index].address, BTD_ADDRESS_BYTES);
		count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Names a state for the socket's STATE line.
 */
const char *
btd_state_name(
	enum btd_state state)
{
	/* Each state's word. */
	switch (state) {
	case BTD_STATE_NONE:
		return "none";
	case BTD_STATE_STARTING:
		return "starting";
	case BTD_STATE_READY:
		return "ready";
	case BTD_STATE_SCANNING:
		return "scanning";
	case BTD_STATE_FIRMWARE_NEEDED:
		return "firmware-needed";
	case BTD_STATE_FIRMWARE_FAILED:
		return "firmware-failed";
	case BTD_STATE_UNSUPPORTED:
		return "unsupported";
	case BTD_STATE_ERROR:
		return "error";
	case BTD_STATE_LOST:
		return "lost";
	default:
		break;
	}

	/* A state with no word. */
	return "unknown";
}

/*
 * Gives the monotonic clock in milliseconds.
 */
uint64_t
btd_now_ms(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Sets the state and its reason (a formatted text, "" for none). */
static void
session_set(
	struct btd_session *session,
	enum btd_state state,
	const char *format,
	...)
{
	va_list arguments;

	/* The state. */
	session->state = state;

	/* Its reason. */
	va_start(arguments, format);
	(void)vsnprintf(session->reason, sizeof(session->reason), format, arguments);
	va_end(arguments);
}

/* Writes one H4 packet; returns 0, ENODEV when the node went (the state is then lost), or another errno. */
static int
session_write(
	struct btd_session *session,
	const uint8_t *packet,
	size_t length)
{
	ssize_t written;
	int error;

	/* One write is one packet; an interrupted one is tried again. */
	for (;;) {
		written = write(session->descriptor, packet, length);
		if (written >= 0)
			break;
		error = errno;
		if (error == EINTR)
			continue;

		/* A node that went is lost; the daemon opens the new one. */
		if (error == ENODEV)
			session_set(session, BTD_STATE_LOST, "%s", "the node went");
		return error;
	}

	/* The whole packet, or the node broke its promise. */
	if ((size_t)written != length)
		return EIO;

	/* Succeeded: written, and traced. */
	if (session->packet_trace != NULL)
		session->packet_trace(session->packet_trace_context, packet, length, 0);
	return 0;
}

/* Reads one packet into session->packet, waiting at most timeout_ms; returns 0, ETIMEDOUT, ENODEV (lost) or another errno. */
static int
session_read(
	struct btd_session *session,
	unsigned timeout_ms)
{
	struct pollfd descriptor;
	ssize_t got;
	int ready;
	int error;

	/* Waits for the node. */
	descriptor.fd = session->descriptor;
	descriptor.events = POLLIN;
	descriptor.revents = 0;
	ready = poll(&descriptor, 1U, (int)timeout_ms);
	if (ready < 0) {
		error = errno;
		if (error == EINTR)
			return ETIMEDOUT;
		return error;
	}

	/* Nothing came in time. */
	if (ready == 0)
		return ETIMEDOUT;

	/* One read is one packet. */
	got = read(session->descriptor, session->packet, sizeof(session->packet));
	if (got < 0) {
		error = errno;
		if (error == EAGAIN || error == EINTR)
			return ETIMEDOUT;
		if (error == ENODEV)
			session_set(session, BTD_STATE_LOST, "%s", "the node went");
		return error;
	}

	/* An end of file is a node that went (the tests' controller closing its side). */
	if (got == 0) {
		session_set(session, BTD_STATE_LOST, "%s", "the node went");
		return ENODEV;
	}

	/* Succeeded: a packet, not counted yet, traced. */
	session->packet_length = (size_t)got;
	session->packet_counted = 0;
	if (session->packet_trace != NULL)
		session->packet_trace(session->packet_trace_context, session->packet, session->packet_length, 1);
	return 0;
}

/*
 * Handles a packet no command waits for: the kernel's reset notice, a
 * scan's results and an inquiry's end, the connections the session counts
 * and the buffers the controller gives back; a connection's other events
 * and every ACL packet go to the handler.
 */
static void
session_dispatch(
	struct btd_session *session)
{
	struct btd_event event;
	struct btd_answer answer;
	int scan_event;
	int counted;
	int taken;
	int error;

	/* The kernel's notice of a reset: the controller must be set up again (the daemon starts it again). */
	if (session->packet[0] == BT_PACKET_NOTICE_RESET) {
		session->resets++;
		session_set(session, BTD_STATE_ERROR, "%s", "the controller was reset");
		return;
	}

	/* ACL data belongs to a connection: the handler's. */
	if (session->packet[0] == BT_PACKET_ACL) {
		session_hand(session);
		return;
	}

	/* Anything else must be an event. */
	error = btd_hci_event(session->packet, session->packet_length, &event);
	if (error != 0) {
		if (session->packet[0] == BT_PACKET_EVENT)
			session->malformed++;
		return;
	}

	/* An answer nobody waits for (a command that timed out) is counted. */
	taken = btd_hci_answer(&event, &answer);
	if (taken != 0) {
		session->stray_answers++;
		return;
	}

	/* An inquiry that ended by itself. */
	if (event.code == BTD_EVENT_INQUIRY_COMPLETE) {
		session->inquiring = 0;
		return;
	}

	/* A hardware error: the controller must be set up again (the daemon starts it again). */
	if (event.code == SESSION_EVENT_HARDWARE_ERROR) {
		session->hardware_errors++;
		session_set(session, BTD_STATE_ERROR, "%s", "hardware error");
		return;
	}

	/* The controller gives buffers back: the waiting frames may go on. */
	if (event.code == SESSION_EVENT_COMPLETED_PACKETS) {
		session_completed(session, &event);
		return;
	}

	/* The controller dropped data sent past its buffers: the count of buffers is wrong somewhere. */
	if (event.code == SESSION_EVENT_BUFFER_OVERFLOW) {
		session->buffer_overflows++;
		return;
	}

	/* A scan's result goes in the table while a scan runs (a malformed one is counted); a late one is passed over. */
	scan_event = session_scan_event(&event);
	if (scan_event) {
		if (!session->scanning)
			return;

		/* Kept in the table. */
		taken = btd_devices_take(&session->devices, &event);
		if (taken < 0)
			session->malformed++;
		return;
	}

	/* A connection made or ended is counted (unless that was done while it was queued), then handed on. */
	counted = 0;
	if (!session->packet_counted)
		counted = session_counted_event(session, &event);
	if (counted < 0) {
		session->malformed++;
		return;
	}

	/* The handler's. */
	session_hand(session);
}

/*
 * Writes a command and reads until its answer: 0 with the return
 * parameters in session->returned (none for a Command Status), EIO for a
 * status that is not 0, ETIMEDOUT, ECONNRESET when the controller was reset
 * meanwhile, or the node's error.
 */
static int
session_command(
	struct btd_session *session,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count,
	unsigned timeout_ms)
{
	struct btd_event event;
	struct btd_answer answer;
	uint64_t deadline;
	size_t length;
	int remaining;
	int taken;
	int error;

	/* The command, written. */
	length = btd_hci_command(session->command, sizeof(session->command), opcode, parameters, count);
	if (length == 0U)
		return EINVAL;
	error = session_write(session, session->command, length);
	if (error != 0)
		return error;

	/* Reads until its answer or the time is up. */
	session->returned_length = 0U;
	deadline = btd_now_ms() + timeout_ms;
	for (;;) {
		/* The next packet, within what is left of the time. */
		remaining = session_remaining(deadline);
		error = session_read(session, (unsigned)remaining);
		if (error != 0)
			return error;

		/* A reset under the command ends it. */
		if (session->packet[0] == BT_PACKET_NOTICE_RESET) {
			session_dispatch(session);
			return ECONNRESET;
		}

		/* Anything that is not an answer waits in the queue until the command is over. */
		error = btd_hci_event(session->packet, session->packet_length, &event);
		taken = 0;
		if (error == 0)
			taken = btd_hci_answer(&event, &answer);
		if (taken <= 0) {
			session_enqueue(session);
			continue;
		}

		/* An answer to another command is passed over. */
		if (answer.opcode != opcode) {
			session->stray_answers++;
			continue;
		}

		/* This command's answer: its status, and its return parameters after the status. */
		session->status = answer.status;
		if (answer.returned_length > 1U) {
			session->returned_length = answer.returned_length - 1U;
			memcpy(session->returned, answer.returned + 1, session->returned_length);
		}

		break;
	}

	/* A status that is not 0 is the command's failure. */
	if (answer.status != 0U)
		return EIO;

	/* Succeeded: answered. */
	return 0;
}

/* Reads until a vendor event (0xFF) whose first byte is code; other packets are queued.  Returns 0, ETIMEDOUT or the node's error. */
static int
session_wait_vendor(
	struct btd_session *session,
	uint8_t code,
	unsigned timeout_ms)
{
	struct btd_event event;
	uint64_t deadline;
	int remaining;
	int error;

	/* Reads until the event or the time is up. */
	deadline = btd_now_ms() + timeout_ms;
	for (;;) {
		/* The next packet. */
		remaining = session_remaining(deadline);
		error = session_read(session, (unsigned)remaining);
		if (error != 0)
			return error;

		/* A reset under the wait ends it, as it ends a command. */
		if (session->packet[0] == BT_PACKET_NOTICE_RESET) {
			session_dispatch(session);
			return ECONNRESET;
		}

		/* The vendor event asked for ends the wait. */
		error = btd_hci_event(session->packet, session->packet_length, &event);
		if (error == 0 &&
		    event.code == BTD_EVENT_VENDOR &&
		    event.length >= 1U &&
		    event.parameters[0] == code)
			return 0;

		/* Anything else waits in the queue until the wait is over. */
		session_enqueue(session);
	}
}

/* Runs one step of the start: its command, failing the start with the step's name. */
static int
session_step(
	struct btd_session *session,
	const char *name,
	uint16_t opcode,
	const uint8_t *parameters,
	size_t count)
{
	int error;

	/* The step's command. */
	error = session_command(session, opcode, parameters, count, session->timing.command_ms);
	if (error == ENODEV)
		return error;
	if (error != 0) {
		session_set(session, BTD_STATE_ERROR, "%s %s", name, strerror(error));
		return error;
	}

	/* Succeeded: the step is done. */
	return 0;
}

/*
 * Looks at an Intel controller's firmware (design section 3): loaded
 * already, loaded now, or the reason it cannot be.  Returns 0 to go on with
 * the core's set-up, or the error with the state set.
 */
static int
session_intel(
	struct btd_session *session)
{
	int error;

	/* What the controller runs. */
	error = session_intel_version(session);
	if (error != 0)
		return error;

	/* An operational image needs nothing more. */
	if (session->intel_version.image_type == BTD_INTEL_IMAGE_OPERATIONAL)
		return 0;

	/* Neither the bootloader nor the operational image is not one this daemon knows. */
	if (session->intel_version.image_type != BTD_INTEL_IMAGE_BOOTLOADER) {
		session_set(session, BTD_STATE_UNSUPPORTED, "image-type 0x%02x", session->intel_version.image_type);
		return ENOTSUP;
	}

	/* A controller whose load was tried and that came back in its bootloader is not loaded again. */
	if (!session->load_allowed) {
		session_set(session, BTD_STATE_FIRMWARE_FAILED, "%s", "still in the bootloader after a load");
		return EIO;
	}

	/* The bootloader: the firmware is loaded. */
	error = session_intel_load(session);
	if (error != 0)
		return error;

	/* Succeeded: the operational image runs. */
	return 0;
}

/* Sends Intel Read Version and keeps its answer; returns 0 or the error with the state set. */
static int
session_intel_version(
	struct btd_session *session)
{
	static const uint8_t tlv_form[1] = { 0xffU };
	int error;

	/* The TLV form of the version. */
	error = session_step(session, "intel-version", BTD_INTEL_READ_VERSION, tlv_form, sizeof(tlv_form));
	if (error != 0)
		return error;

	/* Its fields, the status first (put back before the return parameters). */
	memmove(session->returned + 1, session->returned, session->returned_length);
	session->returned[0] = 0U;
	error = btd_intel_version_parse(session->returned, session->returned_length + 1U, &session->intel_version);
	if (error != 0 || !session->intel_version.have_image_type) {
		session_set(session, BTD_STATE_UNSUPPORTED, "%s", "the version answer is malformed");
		return EBADMSG;
	}

	/* Succeeded: the version is known. */
	return 0;
}

/*
 * Loads the operational firmware into an Intel controller in its
 * bootloader, boots it and applies its DDC (design section 3, steps 2 to
 * 7).  Returns 0, or the error with the state set (firmware-needed,
 * firmware-failed, unsupported, or lost when the node went).
 */
static int
session_intel_load(
	struct btd_session *session)
{
	struct btd_intel_plan plan;
	const char *reason;
	uint8_t *file;
	size_t length;
	char path[256];
	int error;

	/* Only the method this daemon knows. */
	error = btd_intel_loadable(&session->intel_version, &reason);
	if (error != 0) {
		session_set(session, BTD_STATE_UNSUPPORTED, "%s", reason);
		return error;
	}

	/* The firmware file. */
	error = btd_intel_file_name(&session->intel_version, session->firmware_folder, "sfi", path, sizeof(path));
	if (error != 0) {
		session_set(session, BTD_STATE_UNSUPPORTED, "%s", "the firmware's name");
		return error;
	}

	/* Its bytes; a missing file is firmware the user must install. */
	error = session_read_file(path, &file, &length);
	if (error == ENOENT) {
		session_set(session, BTD_STATE_FIRMWARE_NEEDED, "%s", path);
		return error;
	}

	/* A file that cannot be read fails the load. */
	if (error != 0) {
		session_set(session, BTD_STATE_FIRMWARE_FAILED, "%s %s", path, strerror(error));
		return error;
	}

	/* Its fragments. */
	memset(&plan, 0, sizeof(plan));
	plan.capacity = btd_intel_plan_capacity(length);
	plan.fragments = calloc(plan.capacity, sizeof(*plan.fragments));
	if (plan.fragments == NULL) {
		free(file);
		session_set(session, BTD_STATE_FIRMWARE_FAILED, "%s", "out of memory");
		return ENOMEM;
	}

	/* The plan of the file's fragments. */
	error = btd_intel_plan_make(file, length, &session->intel_version, &plan, &reason);
	if (error != 0) {
		free(plan.fragments);
		free(file);
		session_set(session, BTD_STATE_FIRMWARE_FAILED, "%s", reason);
		return error;
	}

	/* The download, then the boot. */
	error = session_intel_send(session, file, &plan);
	free(plan.fragments);
	free(file);
	if (error != 0)
		return error;

	/* The operational image must run now. */
	error = session_intel_version(session);
	if (error != 0)
		return error;
	if (session->intel_version.image_type != BTD_INTEL_IMAGE_OPERATIONAL) {
		session_set(session, BTD_STATE_FIRMWARE_FAILED, "%s", "still in the bootloader after the boot");
		return EIO;
	}

	/* The device configuration, and Intel's event mask (failures do not stop an operational controller). */
	session_intel_ddc(session);
	session->firmware_loaded = 1;

	/* Succeeded: loaded and booted. */
	return 0;
}

/*
 * Sends the plan's fragments on the bootloader's path, waits for the
 * download's end, and boots the controller with Intel Reset.  The path goes
 * back to normal whatever happens.  Returns 0, or the error with the state
 * set.
 */
static int
session_intel_send(
	struct btd_session *session,
	const uint8_t *file,
	const struct btd_intel_plan *plan)
{
	static const uint8_t reset_head[4] = { 0x00U, 0x00U, 0x00U, 0x01U };
	struct btd_event event;
	struct btd_answer answer;
	uint8_t parameters[1U + BTD_INTEL_FRAGMENT_MAX];
	uint8_t reset[8];
	uint32_t on;
	size_t index;
	size_t length;
	int downloaded;
	int taken;
	int error;

	/* The bootloader's path: Secure Send goes on bulk OUT, its answers come from bulk IN. */
	if (session->control != NULL) {
		on = 1U;
		error = session_control(session, BT_IOC_SET_BOOTLOADER, &on);
		if (error != 0) {
			session_set(session, BTD_STATE_FIRMWARE_FAILED, "bootloader-path %s", strerror(error));
			return error;
		}
	}

	/* A download is under way: the daemon remembers the controller. */
	session->load_sent = 1;
	session->trace[0] = '\0';

	/* Each fragment: [type][data], then the next event is its answer (the download's end may come first). */
	downloaded = 0;
	for (index = 0U; index < plan->count; index++) {
		/* The fragment, written. */
		parameters[0] = plan->fragments[index].type;
		memcpy(parameters + 1, file + plan->fragments[index].offset, plan->fragments[index].length);
		length = btd_hci_command(session->command,
					 sizeof(session->command),
					 BTD_INTEL_SECURE_SEND,
					 parameters,
					 1U + plan->fragments[index].length);
		error = session_write(session, session->command, length);
		if (error != 0) {
			if (error != ENODEV) {
				session_normal_path(session);
				session_set(session, BTD_STATE_FIRMWARE_FAILED, "fragment %u %s", (unsigned)index, strerror(error));
			}

			/* Reports the failure (a node that went is the daemon's to open again). */
			return error;
		}

		/* Its answer. */
		error = session_read(session, session->timing.command_ms);
		if (error != 0) {
			if (error != ENODEV) {
				session_normal_path(session);
				session_set(session, BTD_STATE_FIRMWARE_FAILED, "fragment %u answer %s", (unsigned)index, strerror(error));
			}

			/* Reports the failure (a node that went is the daemon's to open again). */
			return error;
		}

		/* Kept for the log. */
		session_trace(session, (unsigned)index);

		/* A Command Complete for Secure Send must say success. */
		error = btd_hci_event(session->packet, session->packet_length, &event);
		taken = 0;
		if (error == 0)
			taken = btd_hci_answer(&event, &answer);
		if (taken > 0 && answer.opcode == BTD_INTEL_SECURE_SEND && answer.status != 0U) {
			session_normal_path(session);
			session_set(session, BTD_STATE_FIRMWARE_FAILED, "fragment %u status 0x%02x", (unsigned)index, answer.status);
			return EIO;
		}

		/* The download's end, if it came instead. */
		if (error == 0 &&
		    event.code == BTD_EVENT_VENDOR &&
		    event.length >= 1U &&
		    event.parameters[0] == BTD_INTEL_EVENT_DOWNLOADED)
			downloaded = 1;
	}

	/* The download's end. */
	if (!downloaded) {
		error = session_wait_vendor(session, BTD_INTEL_EVENT_DOWNLOADED, session->timing.download_ms);
		if (error != 0) {
			if (error != ENODEV) {
				session_normal_path(session);
				session_set(session, BTD_STATE_FIRMWARE_FAILED, "download-end %s", strerror(error));
			}

			/* Reports the failure (a node that went is the daemon's to open again). */
			return error;
		}
	}

	/* Back on the normal path before the boot. */
	session_normal_path(session);

	/*
	 * Intel Reset with the boot parameter.  It has no Command Complete: the
	 * controller resets, so the write itself may fail; the boot's vendor
	 * event, or the node going (a re-enumeration), is the answer.
	 */
	memcpy(reset, reset_head, sizeof(reset_head));
	reset[4] = (uint8_t)(plan->boot_parameter & 0xffU);
	reset[5] = (uint8_t)((plan->boot_parameter >> 8) & 0xffU);
	reset[6] = (uint8_t)((plan->boot_parameter >> 16) & 0xffU);
	reset[7] = (uint8_t)((plan->boot_parameter >> 24) & 0xffU);
	length = btd_hci_command(session->command, sizeof(session->command), BTD_INTEL_RESET, reset, sizeof(reset));
	error = session_write(session, session->command, length);
	if (error == ENODEV)
		return error;

	/* The boot. */
	error = session_wait_vendor(session, BTD_INTEL_EVENT_BOOTED, session->timing.boot_ms);
	if (error != 0) {
		if (error != ENODEV)
			session_set(session, BTD_STATE_FIRMWARE_FAILED, "boot %s", strerror(error));
		return error;
	}

	/* Succeeded: the controller booted its firmware. */
	return 0;
}

/* Applies the .ddc file's records, then Intel's event mask; a failure is kept in the reason and stops nothing. */
static void
session_intel_ddc(
	struct btd_session *session)
{
	static const uint8_t intel_mask[8] = { 0x87U, 0x0cU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	const uint8_t *record;
	uint8_t *file;
	size_t record_length;
	size_t offset;
	size_t length;
	char path[256];
	int more;
	int error;

	/* The DDC file, when there is one. */
	error = btd_intel_file_name(&session->intel_version, session->firmware_folder, "ddc", path, sizeof(path));
	file = NULL;
	length = 0U;
	if (error == 0)
		error = session_read_file(path, &file, &length);

	/* Each record in one Intel Write DDC, until one fails. */
	offset = 0U;
	while (error == 0) {
		more = btd_intel_ddc_next(file, length, &offset, &record, &record_length);
		if (more <= 0)
			break;
		error = session_command(session, BTD_INTEL_WRITE_DDC, record, record_length, session->timing.command_ms);
	}

	/* The file is no longer needed. */
	free(file);

	/* Intel's event mask. */
	(void)session_command(session, BTD_INTEL_SET_EVENT_MASK, intel_mask, sizeof(intel_mask), session->timing.command_ms);
}

/* Reads a whole file of at most BTD_FIRMWARE_MAX bytes into memory the caller frees. */
static int
session_read_file(
	const char *path,
	uint8_t **contents,
	size_t *length)
{
	struct stat status;
	uint8_t *buffer;
	ssize_t got;
	size_t used;
	int descriptor;
	int error;

	/* The file, and its size. */
	*contents = NULL;
	*length = 0U;
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return errno;
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* A file of a sane size. */
	if (status.st_size <= 0 || (uint64_t)status.st_size > BTD_FIRMWARE_MAX) {
		(void)close(descriptor);
		return EFBIG;
	}

	/* Room for it. */
	buffer = malloc((size_t)status.st_size);
	if (buffer == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* Its bytes, as many reads as it takes. */
	used = 0U;
	while (used < (size_t)status.st_size) {
		got = read(descriptor, buffer + used, (size_t)status.st_size - used);
		if (got <= 0) {
			error = EIO;
			if (got < 0)
				error = errno;
			if (got < 0 && error == EINTR)
				continue;
			free(buffer);
			(void)close(descriptor);
			return error;
		}

		/* The bytes read so far. */
		used += (size_t)got;
	}

	/* The file is read. */
	(void)close(descriptor);

	/* Succeeded: the file's bytes. */
	*contents = buffer;
	*length = used;
	return 0;
}

/*
 * The HCI core's set-up (p003's plan section 2 with p004's plan section
 * 10.2): reset, the version, the address, the supported commands and
 * features, the buffer sizes, the event masks, Secure Simple Pairing,
 * Secure Connections and LE on the host's side, and the inquiry mode.
 * Returns 0, or the error with the state set.
 */
static int
session_core(
	struct btd_session *session)
{
	/*
	 * The events (bit n is the event of code n + 1, Core Vol 4 Part E
	 * §7.3.1): the inquiry's (bits 0, 1, 33, 46), the connections' and the
	 * authentication's (2 to 5, 7), Hardware Error (15), the link keys and
	 * PIN (21 to 23), Data Buffer Overflow (25), Encryption Key Refresh
	 * Complete (47), Secure Simple Pairing's (48 to 51, 53, 58), and LE
	 * Meta (61).
	 */
	static const uint8_t event_mask[8] = { 0xbfU, 0x80U, 0xe0U, 0x02U, 0x02U, 0xc0U, 0x2fU, 0x24U };
	/*
	 * LE's subevents (bit n is subevent n + 1, §7.8.1): Connection Complete,
	 * Advertising Report, Connection Update Complete, Read Local P-256
	 * Public Key Complete, Generate DHKey Complete, Enhanced Connection
	 * Complete (ws143-p005 i03: the identity of a resolved address).
	 */
	static const uint8_t le_event_mask[8] = { 0x87U, 0x03U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t extended_inquiry[1] = { 0x02U };
	static const uint8_t enabled[1] = { 0x01U };
	static const uint8_t le_host[2] = { 0x01U, 0x00U };
	uint32_t acl;
	int le_mask;
	int le_parameters;
	int le_enable;
	int error;

	/* Reset. */
	error = session_step(session, "reset", SESSION_RESET, NULL, 0U);
	if (error != 0)
		return error;

	/*
	 * Nothing from before the reset is handed out after it (a hardware
	 * error queued during Intel's boot would start the controller again),
	 * and no connection survives it.
	 */
	session->queue_head = 0U;
	session->queue_used = 0U;
	memset(session->links, 0, sizeof(session->links));
	session->frame_count = 0U;

	/* The version: HCI's version and revision, and the manufacturer. */
	error = session_step(session, "local-version", SESSION_READ_LOCAL_VERSION, NULL, 0U);
	if (error != 0)
		return error;
	if (session->returned_length < 8U) {
		session_set(session, BTD_STATE_ERROR, "%s", "local-version short");
		return EBADMSG;
	}

	/* Its fields. */
	session->hci_version = session->returned[0];
	session->hci_revision = (uint16_t)(session->returned[1] | (session->returned[2] << 8));
	session->manufacturer = (uint16_t)(session->returned[4] | (session->returned[5] << 8));

	/* The address. */
	error = session_step(session, "address", SESSION_READ_ADDRESS, NULL, 0U);
	if (error != 0)
		return error;
	if (session->returned_length < BTD_ADDRESS_BYTES) {
		session_set(session, BTD_STATE_ERROR, "%s", "address short");
		return EBADMSG;
	}

	/* Kept. */
	memcpy(session->address, session->returned, BTD_ADDRESS_BYTES);
	session->have_address = 1;

	/* The supported commands: LE's scan, and the P-256 and DHKey commands D5 b1 needs. */
	error = session_command(session, SESSION_READ_COMMANDS, NULL, 0U, session->timing.command_ms);
	if (error == ENODEV)
		return error;
	if (error == 0 && session->returned_length >= BTD_COMMANDS_BYTES) {
		memcpy(session->commands, session->returned, BTD_COMMANDS_BYTES);
		session->have_commands = 1;
	}

	/* Whether the controller does P-256 and DHKey itself (D5's b1). */
	session->p256 = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_CRYPTO_OCTET, SESSION_BIT_P256);
	session->dhkey = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_CRYPTO_OCTET, SESSION_BIT_DHKEY);

	/* The LMP features: whether the controller has LE at all (a controller that does not answer is judged by its commands). */
	session->le_supported = 1;
	error = session_command(session, SESSION_READ_FEATURES, NULL, 0U, session->timing.command_ms);
	if (error == ENODEV)
		return error;
	if (error == 0 && session->returned_length > SESSION_FEATURE_LE_BYTE) {
		if ((session->returned[SESSION_FEATURE_LE_BYTE] & (1U << SESSION_FEATURE_LE_BIT)) == 0U)
			session->le_supported = 0;
	}

	/* Whether a BR/EDR packet may be marked not to be flushed (else it is sent flushable, as before that feature). */
	session->no_flush = 0;
	if (error == 0 && session->returned_length > SESSION_FEATURE_NO_FLUSH_BYTE) {
		if ((session->returned[SESSION_FEATURE_NO_FLUSH_BYTE] & (1U << SESSION_FEATURE_NO_FLUSH_BIT)) != 0U)
			session->no_flush = 1;
	}

	/* LE's scan needs LE Set Event Mask and LE Set Scan Parameters and Enable. */
	le_mask = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_MASK_OCTET, SESSION_BIT_LE_MASK);
	le_parameters = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_SCAN_OCTET, SESSION_BIT_LE_SCAN_PARAMETERS);
	le_enable = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_SCAN_OCTET, SESSION_BIT_LE_SCAN_ENABLE);
	session->le = 0;
	if (session->le_supported && le_mask && le_parameters && le_enable)
		session->le = 1;

	/* The buffer size: the longest ACL packet the controller takes tells the node, and BR/EDR's pool. */
	error = session_command(session, SESSION_READ_BUFFER_SIZE, NULL, 0U, session->timing.command_ms);
	if (error == ENODEV)
		return error;
	if (error == 0 && session->returned_length >= 2U) {
		session->acl_length = (uint16_t)(session->returned[0] | (session->returned[1] << 8));
		acl = session->acl_length;
		if (session->control != NULL && acl >= BT_ACL_DATA_MIN && acl <= BT_ACL_DATA_MAX)
			(void)session_control(session, BT_IOC_SET_ACL_MAX, &acl);
	}

	/* The pools of ACL buffers, BR/EDR's from that answer, LE's of its own. */
	session_core_buffers(session);
	if (session->state == BTD_STATE_LOST)
		return ENODEV;

	/* The events the scan, the connections and the pairing need; without the mask none of them would come. */
	error = session_step(session, "event-mask", SESSION_SET_EVENT_MASK, event_mask, sizeof(event_mask));
	if (error != 0)
		return error;

	/* LE's events; a controller that refuses them does not use LE. */
	if (session->le) {
		error = session_command(session, SESSION_LE_SET_EVENT_MASK, le_event_mask, sizeof(le_event_mask), session->timing.command_ms);
		if (error == ENODEV)
			return error;
		if (error != 0)
			session->le = 0;
	}

	/*
	 * Secure Simple Pairing on the host's side.  It is sent whatever the
	 * supported commands say, and a refusal means the controller has none
	 * (BR/EDR pairing would then ask for a PIN, which is refused).
	 */
	error = session_command(session, SESSION_WRITE_SSP_MODE, enabled, sizeof(enabled), session->timing.command_ms);
	if (error == ENODEV)
		return error;
	session->ssp = 0;
	if (error == 0)
		session->ssp = 1;

	/* Secure Connections on the host's side, likewise (a refusal leaves BR/EDR's P-192 pairing). */
	error = session_command(session, SESSION_WRITE_SC_HOST, enabled, sizeof(enabled), session->timing.command_ms);
	if (error == ENODEV)
		return error;
	session->secure_connections = 0;
	if (error == 0)
		session->secure_connections = 1;

	/* LE on the host's side, for a controller of both kinds (a refusal is only noted by the controller staying as it was). */
	if (session->le_supported) {
		error = session_command(session, SESSION_WRITE_LE_HOST, le_host, sizeof(le_host), session->timing.command_ms);
		if (error == ENODEV)
			return error;
	}

	/* Extended inquiry results, which carry the names (optional: without them the results come plain). */
	error = session_command(session, SESSION_WRITE_INQUIRY_MODE, extended_inquiry, sizeof(extended_inquiry), session->timing.command_ms);
	if (error == ENODEV)
		return error;

	/* Succeeded: set up. */
	return 0;
}

/* Gives the milliseconds left before a deadline, 0 once it passed. */
static int
session_remaining(
	uint64_t deadline)
{
	uint64_t now;

	/* The time now. */
	now = btd_now_ms();
	if (now >= deadline)
		return 0;

	/* Succeeded: what is left. */
	return (int)(deadline - now);
}

/* Calls one of the node's ioctls; returns 0 or the errno value. */
static int
session_control(
	struct btd_session *session,
	unsigned long request,
	void *argument)
{
	int error;

	/* No ioctls on this descriptor. */
	if (session->control == NULL)
		return ENOTTY;

	/* The call. */
	error = session->control(session->control_context, request, argument);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Puts the node back on its normal path (a failure is only logged in the reason by the caller). */
static void
session_normal_path(
	struct btd_session *session)
{
	uint32_t off;

	/* Bulk IN read as ACL again, Secure Send no longer on bulk OUT. */
	off = 0U;
	if (session->control != NULL)
		(void)session_control(session, BT_IOC_SET_BOOTLOADER, &off);
}

/* Keeps the first Secure Send answers, as hex, for the log. */
static void
session_trace(
	struct btd_session *session,
	unsigned index)
{
	size_t used;
	size_t byte;

	/* Only the first few. */
	if (index >= SESSION_TRACE_ANSWERS)
		return;

	/* Appended, a space between two answers; what does not fit is left out. */
	used = strlen(session->trace);
	if (used != 0U && used + 1U < sizeof(session->trace)) {
		session->trace[used] = ' ';
		used++;
		session->trace[used] = '\0';
	}

	/* Its bytes as hex, as many as fit. */
	for (byte = 0U; byte < session->packet_length && used + 2U < sizeof(session->trace); byte++) {
		(void)snprintf(session->trace + used, sizeof(session->trace) - used, "%02x", session->packet[byte]);
		used += 2U;
	}
}

/* Starts the passive LE scan: the scan parameters, then on with every report kept. */
static int
session_scan_le(
	struct btd_session *session)
{
	static const uint8_t parameters[7] = { 0x00U, 0x10U, 0x00U, 0x10U, 0x00U, 0x00U, 0x00U };
	static const uint8_t enable[2] = { 0x01U, 0x00U };
	int error;

	/* Passive, 10 ms interval and window, the public address (no request is sent), no filter. */
	error = session_command(session, SESSION_LE_SCAN_PARAMETERS, parameters, sizeof(parameters), session->timing.command_ms);
	if (error != 0)
		return error;

	/* On, no duplicate filter. */
	error = session_command(session, SESSION_LE_SCAN_ENABLE, enable, sizeof(enable), session->timing.command_ms);
	if (error != 0)
		return error;

	/* Succeeded: LE scans. */
	session->le_scanning = 1;
	return 0;
}

/*
 * Queues the packet just read (it came while a command or a vendor event
 * was awaited) to be handled after the wait.  What only counts the
 * controller's buffers and connections, or resets the controller, is done
 * now, so a full queue cannot lose it: Number Of Completed Packets, a
 * Hardware Error and a Data Buffer Overflow are not queued, and a
 * connection made or ended is counted and queued marked as counted.  The
 * queue is a ring of records, a header (length and flags) and the packet.
 *
 * ws197-p002 (plan/ws197/phase002/phase.md section 3.2): the last bytes are
 * kept for the counted events, and more before them for the other events;
 * ACL data and the scans' reports take neither.  ACL data of a sealed link
 * is dropped; a dropped ACL packet seals its link and an event dropped
 * marks the events, each noticed to the handler after what came before.
 */
static void
session_enqueue(
	struct btd_session *session)
{
	struct btd_event event;
	struct btd_link_count *link;
	uint16_t handle;
	uint8_t flags;
	size_t tail;
	size_t index;
	size_t need;
	size_t free_bytes;
	size_t reserve;
	int counted;
	int report;
	int dropped;
	int error;

	/* ACL data of a sealed link, or continuing data after its notice: dropped as it arrives. */
	flags = 0U;
	counted = 0;
	report = 0;
	error = EINVAL;
	if (session->packet[0] == BT_PACKET_ACL) {
		dropped = session_acl_arrival(session);
		if (dropped)
			return;
	} else {
		error = btd_hci_event(session->packet, session->packet_length, &event);
	}

	/* The buffers given back are counted now and not queued. */
	if (error == 0 && event.code == SESSION_EVENT_COMPLETED_PACKETS) {
		session_completed(session, &event);
		return;
	}

	/* A hardware error and an overflow are handled now (as dispatch would) and not queued. */
	if (error == 0 && event.code == SESSION_EVENT_HARDWARE_ERROR) {
		session->hardware_errors++;
		session_set(session, BTD_STATE_ERROR, "%s", "hardware error");
		return;
	}

	/* An overflow is counted (the buffers' count is wrong somewhere). */
	if (error == 0 && event.code == SESSION_EVENT_BUFFER_OVERFLOW) {
		session->buffer_overflows++;
		return;
	}

	/* A connection made or ended is counted now and marked so; a scan's report is the least kept. */
	if (error == 0) {
		counted = session_counted_event(session, &event);
		if (counted > 0)
			flags |= SESSION_RECORD_COUNTED;
		report = session_report(&event);
	}

	/* The room each kind may take: counted events all, other events all but their reserve, ACL data and reports less. */
	need = SESSION_RECORD_HEADER + session->packet_length;
	free_bytes = BTD_QUEUE_BYTES - session->queue_used;
	reserve = BTD_QUEUE_EVENT_RESERVE;
	if (counted > 0)
		reserve = 0U;
	else if (error == 0 && !report)
		reserve = BTD_QUEUE_COUNTED_RESERVE;

	/* No room: dropped, and noticed by kind. */
	if (free_bytes < reserve || need > free_bytes - reserve) {
		session->queue_dropped++;
		if (session->packet[0] == BT_PACKET_ACL) {
			handle = session_handle(session->packet + 1);
			link = session_link(session, handle);
			if (link != NULL)
				session_mark_link(session, link);
		} else if (session->packet[0] != BT_PACKET_EVENT) {
			/* Neither data nor an event: nobody to tell. */
		} else if (counted > 0) {
			session_mark_events(session, BTD_DROP_COUNTED);
		} else if (report) {
			session->scan_dropped++;
		} else {
			session->events_dropped++;
			session_mark_events(session, BTD_DROP_EVENT);
		}

		/* Not queued. */
		return;
	}

	/* The record, byte by byte round the ring, after the last one. */
	tail = (session->queue_head + session->queue_used) % BTD_QUEUE_BYTES;
	session->queue[tail] = (uint8_t)(session->packet_length & 0xffU);
	tail = (tail + 1U) % BTD_QUEUE_BYTES;
	session->queue[tail] = (uint8_t)(session->packet_length >> 8);
	tail = (tail + 1U) % BTD_QUEUE_BYTES;
	session->queue[tail] = flags;
	tail = (tail + 1U) % BTD_QUEUE_BYTES;
	for (index = 0U; index < session->packet_length; index++) {
		session->queue[tail] = session->packet[index];
		tail = (tail + 1U) % BTD_QUEUE_BYTES;
	}

	/* Succeeded: the record is in the queue, and counted for the notices' order. */
	session->queue_used += need;
	session->enqueued++;
}

/* Takes the oldest queued packet into session->packet (and its flags); returns 1, or 0 when none waits. */
static int
session_dequeue(
	struct btd_session *session)
{
	uint8_t flags;
	size_t length;
	size_t index;

	/* Nothing waits. */
	if (session->queue_used == 0U)
		return 0;

	/* The record's length and flags. */
	length = session->queue[session->queue_head];
	session->queue_head = (session->queue_head + 1U) % BTD_QUEUE_BYTES;
	length |= (size_t)session->queue[session->queue_head] << 8;
	session->queue_head = (session->queue_head + 1U) % BTD_QUEUE_BYTES;
	flags = session->queue[session->queue_head];
	session->queue_head = (session->queue_head + 1U) % BTD_QUEUE_BYTES;

	/* The packet. */
	for (index = 0U; index < length; index++) {
		session->packet[index] = session->queue[session->queue_head];
		session->queue_head = (session->queue_head + 1U) % BTD_QUEUE_BYTES;
	}

	/* The record left the queue; a counted connection event is not counted again. */
	session->packet_length = length;
	session->packet_counted = 0;
	if ((flags & SESSION_RECORD_COUNTED) != 0U)
		session->packet_counted = 1;
	session->queue_used -= SESSION_RECORD_HEADER + length;
	session->dequeued++;

	/* Succeeded: a packet to handle. */
	return 1;
}

/* Hands the packet just read to the handler, as a copy the handler's own commands do not overwrite. */
static void
session_hand(
	struct btd_session *session)
{
	/* No handler: nobody wants the connections' packets. */
	if (session->handler == NULL)
		return;

	/* The copy, then the handler. */
	memcpy(session->handed, session->packet, session->packet_length);
	session->handler(session->handler_context, session, session->handed, session->packet_length);
}

/*
 * Counts the connections an event makes or ends: an ACL Connection
 * Complete or LE Connection Complete that succeeded adds its handle, a
 * Disconnection Complete removes it.  Returns 1 for such an event, 0 for
 * any other, -1 for one too short.
 */
static int
session_counted_event(
	struct btd_session *session,
	const struct btd_event *event)
{
	uint16_t handle;

	/* BR/EDR's Connection Complete: status, handle, address, link type, encryption. */
	if (event->code == SESSION_EVENT_CONNECTED) {
		if (event->length < 11U)
			return -1;

		/* A connection made, of ACL data. */
		handle = session_handle(event->parameters + 1);
		if (event->parameters[0] == 0U && event->parameters[9] == SESSION_LINK_ACL)
			session_link_add(session, handle, 0, event->parameters + 3);
		return 1;
	}

	/* LE's (Enhanced, ws143-p005 i03) Connection Complete: subevent, status, handle, and the rest. */
	if (event->code == BTD_EVENT_LE_META && event->length >= 1U && (event->parameters[0] == SESSION_LE_CONNECTED || event->parameters[0] == SESSION_LE_ENHANCED)) {
		if (event->length < 19U || (event->parameters[0] == SESSION_LE_ENHANCED && event->length < 31U))
			return -1;

		/* A connection made. */
		handle = session_handle(event->parameters + 2);
		if (event->parameters[1] == 0U)
			session_link_add(session, handle, 1, event->parameters + 6);
		return 1;
	}

	/* Disconnection Complete: status, handle, reason. */
	if (event->code == SESSION_EVENT_DISCONNECTED) {
		if (event->length < 4U)
			return -1;

		/* A connection ended. */
		handle = session_handle(event->parameters + 1);
		if (event->parameters[0] == 0U)
			session_link_remove(session, handle);
		return 1;
	}

	/* Not one the session counts. */
	return 0;
}

/*
 * Takes Number Of Completed Packets (the number of handles, then each
 * handle with its count): the packets are given back to their pools and
 * the waiting frames go on.  A malformed one is counted.
 */
static void
session_completed(
	struct btd_session *session,
	const struct btd_event *event)
{
	struct btd_link_count *link;
	struct btd_pool *pool;
	const uint8_t *entry;
	unsigned handles;
	unsigned index;
	unsigned count;
	uint16_t handle;

	/* The number of handles, and room for each handle and count. */
	if (event->length < 1U) {
		session->malformed++;
		return;
	}

	/* The handles and their counts must fill the event. */
	handles = event->parameters[0];
	if (event->length != 1U + 4U * (size_t)handles) {
		session->malformed++;
		return;
	}

	/* Each handle's packets back to its pool, no more than were sent. */
	for (index = 0U; index < handles; index++) {
		entry = event->parameters + 1U + 4U * index;
		handle = session_handle(entry);
		count = (unsigned)(entry[2] | (entry[3] << 8));
		link = session_link(session, handle);
		if (link == NULL)
			continue;

		/* The controller is done with those packets: they no longer hold the pool's buffers. */
		if (count > link->outstanding)
			count = link->outstanding;
		link->outstanding -= count;
		pool = session_pool(session, link);
		pool->free += count;
		if (pool->free > pool->total)
			pool->free = pool->total;
	}

	/* The frames that waited for buffers (a node that went is the next read's to see). */
	(void)session_flush(session);
}

/* Counts a new connection's packets from now on (a handle counted already starts again). */
static void
session_link_add(
	struct btd_session *session,
	uint16_t handle,
	int le,
	const uint8_t *address)
{
	struct btd_link_count *link;
	unsigned index;

	/* A handle the controller reused after a disconnection the session missed. */
	session_link_remove(session, handle);

	/* A free slot; with none the connection's packets cannot be sent. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		link = &session->links[index];
		if (link->used)
			continue;

		/* A fresh record: no drop, no limit of an earlier connection of the handle. */
		session_link_reset(link);
		link->used = 1;
		link->handle = handle;
		link->le = le;
		memcpy(link->address, address, BTD_ADDRESS_BYTES);
		return;
	}
}

/*
 * Ends a connection's count: its packets not completed go back to the pool
 * (the controller dropped them with the connection) and its waiting frames
 * are dropped.
 */
static void
session_link_remove(
	struct btd_session *session,
	uint16_t handle)
{
	struct btd_link_count *link;
	struct btd_pool *pool;
	unsigned index;

	/* The connection, if counted. */
	link = session_link(session, handle);
	if (link == NULL)
		return;

	/* Its buffers back. */
	pool = session_pool(session, link);
	pool->free += link->outstanding;
	if (pool->free > pool->total)
		pool->free = pool->total;

	/* The record emptied: a notice still waiting for it is not given (its owner hears of the disconnection). */
	session_link_reset(link);

	/* Its frames out of the queue, the others kept in order. */
	index = 0U;
	while (index < session->frame_count) {
		if (session->frames[index].handle != handle) {
			index++;
			continue;
		}

		/* The frame goes; the later ones move up. */
		memmove(&session->frames[index], &session->frames[index + 1U], sizeof(session->frames[0]) * (session->frame_count - index - 1U));
		session->frame_count--;
	}
}

/* Finds a counted connection by its handle, or NULL. */
static struct btd_link_count *
session_link(
	struct btd_session *session,
	uint16_t handle)
{
	unsigned index;

	/* Each slot in use. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		if (session->links[index].used && session->links[index].handle == handle)
			return &session->links[index];
	}

	/* Not counted. */
	return NULL;
}

/* Gives the pool a connection's packets use: LE's own, or BR/EDR's. */
static struct btd_pool *
session_pool(
	struct btd_session *session,
	const struct btd_link_count *link)
{
	/* An LE connection with a pool of its own. */
	if (link->le && !session->le_shared)
		return &session->le_pool;

	/* BR/EDR's pool. */
	return &session->acl_pool;
}

/*
 * Sends the waiting frames as far as their pools have buffers, one packet
 * of one connection at a time, the connections in turn (ws197-p002 section
 * 4.2: a connection waiting for buffers, or at its limit in the
 * controller, does not hold the others back); each connection's frames go
 * in their order, each packet as long as its pool takes, the first of a
 * frame marked first (not to be flushed), the others continuing.  Returns
 * 0, or the node's error.
 */
static int
session_flush(
	struct btd_session *session)
{
	struct btd_link_count *link;
	struct btd_frame *frame;
	struct btd_pool *pool;
	uint8_t boundary;
	size_t chunk;
	size_t length;
	unsigned index;
	unsigned step;
	unsigned slot;
	int found;
	int sent;
	int error;

	/* Frames whose connection went are dropped first (the removal drops them; this is a guard). */
	index = 0U;
	while (index < session->frame_count) {
		link = session_link(session, session->frames[index].handle);
		if (link != NULL) {
			index++;
			continue;
		}

		/* Nobody's: out of the table, the later ones move up. */
		memmove(&session->frames[index], &session->frames[index + 1U], sizeof(session->frames[0]) * (session->frame_count - index - 1U));
		session->frame_count--;
	}

	/* Rounds of the connections, one packet each turn, until none can send. */
	sent = 1;
	while (sent) {
		sent = 0;
		for (step = 0U; step < BTD_LINKS_MAX; step++) {
			/* The next connection in turn, with a frame waiting. */
			slot = (session->flush_next + step) % BTD_LINKS_MAX;
			link = &session->links[slot];
			if (!link->used)
				continue;
			found = session_oldest(session, link->handle, &index);
			if (!found)
				continue;

			/* A buffer of its pool, and room under its limit in the controller. */
			pool = session_pool(session, link);
			if (pool->free == 0U)
				continue;
			if (link->inflight_limit != 0U && link->outstanding >= link->inflight_limit)
				continue;

			/* The next piece, as long as the pool's packets. */
			frame = &session->frames[index];
			chunk = frame->length - frame->sent;
			if (chunk > pool->length)
				chunk = pool->length;
			boundary = BTD_ACL_CONTINUING;
			if (frame->sent == 0U) {
				/* A first packet: LE's, or a BR/EDR one the controller may hold, is not flushable; else as before that feature. */
				boundary = BTD_ACL_FIRST_FLUSHABLE;
				if (link->le || session->no_flush)
					boundary = BTD_ACL_FIRST;
			}

			/* The packet, written. */
			length = btd_acl_build(session->outgoing, sizeof(session->outgoing), frame->handle, boundary, frame->bytes + frame->sent, chunk);
			error = session_write(session, session->outgoing, length);
			if (error != 0)
				return error;

			/* The controller holds one more buffer of the pool for this connection. */
			pool->free--;
			link->outstanding++;
			frame->sent += chunk;

			/* A frame sent whole leaves the queue. */
			if (frame->sent == frame->length)
				session_frame_done(session, index);

			/* The next turn begins with the connection after this one. */
			session->flush_next = (slot + 1U) % BTD_LINKS_MAX;
			sent = 1;
			break;
		}
	}

	/* Succeeded: nothing waits, or what waits waits for buffers. */
	return 0;
}

/*
 * Sets the pools of ACL buffers: BR/EDR's from Read Buffer Size (the
 * longest packet and how many), LE's from LE Read Buffer Size, which says
 * 0 when LE shares BR/EDR's pool.  A controller that does not tell is
 * given one small packet at a time.
 */
static void
session_core_buffers(
	struct btd_session *session)
{
	int error;

	/* BR/EDR's pool from Read Buffer Size's answer (the last command; its length was read already). */
	session->acl_pool.length = session->acl_length;
	session->acl_pool.total = 0U;
	if (session->returned_length >= 5U)
		session->acl_pool.total = (unsigned)(session->returned[3] | (session->returned[4] << 8));
	if (session->acl_pool.length < SESSION_POOL_LENGTH_LEAST || session->acl_pool.total == 0U) {
		session->acl_pool.length = SESSION_POOL_LENGTH_LEAST;
		session->acl_pool.total = 1U;
	}

	/* The kernel's limit holds over the controller's. */
	if (session->acl_pool.length > BT_ACL_DATA_MAX)
		session->acl_pool.length = BT_ACL_DATA_MAX;
	session->acl_pool.free = session->acl_pool.total;

	/* LE shares BR/EDR's pool until its own is known. */
	session->le_shared = 1;
	memset(&session->le_pool, 0, sizeof(session->le_pool));
	if (!session->le_supported)
		return;

	/* LE Read Buffer Size: the longest LE packet and how many (0: shared). */
	error = session_command(session, SESSION_LE_READ_BUFFER_SIZE, NULL, 0U, session->timing.command_ms);
	if (error != 0 || session->returned_length < 3U)
		return;
	session->le_pool.length = (uint16_t)(session->returned[0] | (session->returned[1] << 8));
	session->le_pool.total = session->returned[2];
	if (session->le_pool.length == 0U || session->le_pool.total == 0U)
		return;

	/* LE's own pool, within the kernel's limit. */
	if (session->le_pool.length > BT_ACL_DATA_MAX)
		session->le_pool.length = BT_ACL_DATA_MAX;
	session->le_pool.free = session->le_pool.total;
	session->le_shared = 0;
}

/* Empties a counted connection's record: unused, no drop, no limit but the session's. */
static void
session_link_reset(
	struct btd_link_count *link)
{
	/* Nothing of an earlier connection is kept. */
	memset(link, 0, sizeof(*link));
	link->frame_limit = BTD_SEND_FRAMES;
}

/*
 * Takes an ACL packet as it arrives from the node, in the order the
 * packets come (ws197-p002 section 3.3): a first packet sets its link's
 * last channel and ends the passing over of continuing packets after a
 * notice; a packet of a sealed link, or a continuing one after a notice,
 * is dropped.  Returns 1 when it is dropped.
 */
static int
session_acl_arrival(
	struct btd_session *session)
{
	struct btd_link_count *link;
	unsigned field;
	unsigned boundary;
	size_t data;

	/* A packet with its header, of a counted connection. */
	if (session->packet_length < 5U)
		return 0;
	field = (unsigned)session->packet[1] | ((unsigned)session->packet[2] << 8);
	link = session_link(session, (uint16_t)(field & SESSION_HANDLE_MASK));
	if (link == NULL)
		return 0;
	boundary = (field >> SESSION_BOUNDARY_SHIFT) & 0x03U;
	data = (size_t)session->packet[3] | ((size_t)session->packet[4] << 8);

	/* A first packet: its channel when the L2CAP header is in it (0: not known). */
	if (boundary != SESSION_BOUNDARY_CONTINUING) {
		link->skip_continuing = 0;
		link->last_cid = 0U;
		if (data >= 4U && session->packet_length >= SESSION_ACL_CID + 2U)
			link->last_cid = (uint16_t)(session->packet[SESSION_ACL_CID] | (session->packet[SESSION_ACL_CID + 1U] << 8));
	} else if (link->skip_continuing) {
		/* A continuing packet of a frame whose start was dropped before the notice. */
		session->continuing_skipped++;
		return 1;
	}

	/* A sealed link drops everything until its notice is handed. */
	if (link->sealed) {
		link->drop_count++;
		return 1;
	}

	/* Taken. */
	return 0;
}

/*
 * Marks a link whose ACL packet was dropped: the first drop seals it and
 * sets its notice after the packets queued before it; its flags say what
 * the channel was (the last first packet's: signalling, data, or not
 * known).
 */
static void
session_mark_link(
	struct btd_session *session,
	struct btd_link_count *link)
{
	uint8_t flags;

	/* The channel's kind. */
	if (link->last_cid == SESSION_CID_SIGNALLING || link->last_cid == SESSION_CID_LE_SIGNALLING) {
		flags = BTD_DROP_SIGNAL;
	} else if (link->last_cid == 0U) {
		flags = BTD_DROP_UNKNOWN;
	} else {
		flags = BTD_DROP_DATA;
	}

	/* The first drop seals the link, its notice after what was queued before it. */
	if (!link->sealed) {
		link->sealed = 1;
		link->notice_after = session->enqueued;
		link->drop_cid = 0U;
		link->drop_flags = 0U;
		link->drop_count = 0U;
	}

	/* Succeeded: the drop's kind and count, the first data channel kept. */
	link->drop_flags |= flags;
	if ((flags & BTD_DROP_DATA) != 0U && link->drop_cid == 0U)
		link->drop_cid = link->last_cid;
	link->drop_count++;
}

/* Marks events dropped that are not one connection's ACL data: their notice after the packets queued before the first. */
static void
session_mark_events(
	struct btd_session *session,
	uint8_t flags)
{
	/* The first sets when the notice is due. */
	if (!session->events_noticed) {
		session->events_noticed = 1;
		session->events_notice_after = session->enqueued;
		session->events_flags = 0U;
	}

	/* Succeeded: the kind. */
	session->events_flags |= flags;
}

/* Tells whether a notice set after a count of queued packets is due: that many were taken (counted modulo 2^32). */
static int
session_due(
	const struct btd_session *session,
	uint32_t after)
{
	uint32_t taken;

	/* The packets taken since, as a difference that wraps. */
	taken = session->dequeued - after;
	if (taken < SESSION_DUE_RANGE)
		return 1;

	/* Not yet. */
	return 0;
}

/*
 * Hands one notice of dropped packets that is due to the handler: the
 * link's mark is cleared and its seal lifted before (a drop inside the
 * handler's own commands starts a new mark), continuing packets passed
 * over until a first one.  Returns 1 when one was handed.
 */
static int
session_notice(
	struct btd_session *session)
{
	struct btd_link_count *link;
	uint8_t notice[BTD_DROP_LENGTH];
	unsigned index;
	unsigned count;
	int due;

	/* A link's notice that is due. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		link = &session->links[index];
		if (!link->used || !link->sealed)
			continue;
		due = session_due(session, link->notice_after);
		if (!due)
			continue;

		/* The notice, its mark cleared first. */
		notice[0] = BTD_PACKET_DROP;
		notice[1] = (uint8_t)(link->handle & 0xffU);
		notice[2] = (uint8_t)(link->handle >> 8);
		notice[3] = link->drop_flags;
		notice[4] = (uint8_t)(link->drop_cid & 0xffU);
		notice[5] = (uint8_t)(link->drop_cid >> 8);
		count = link->drop_count;
		if (count > 0xffffU)
			count = 0xffffU;
		notice[6] = (uint8_t)(count & 0xffU);
		notice[7] = (uint8_t)(count >> 8);
		link->sealed = 0;
		link->skip_continuing = 1;
		link->drop_flags = 0U;
		link->drop_cid = 0U;
		link->drop_count = 0U;

		/* Succeeded: handed. */
		if (session->handler != NULL)
			session->handler(session->handler_context, session, notice, sizeof(notice));
		return 1;
	}

	/* The events' notice when it is due. */
	if (!session->events_noticed)
		return 0;
	due = session_due(session, session->events_notice_after);
	if (!due)
		return 0;
	notice[0] = BTD_PACKET_DROP;
	notice[1] = (uint8_t)(BTD_DROP_ALL & 0xffU);
	notice[2] = (uint8_t)(BTD_DROP_ALL >> 8);
	notice[3] = session->events_flags;
	notice[4] = 0U;
	notice[5] = 0U;
	notice[6] = 0U;
	notice[7] = 0U;
	session->events_noticed = 0;
	session->events_flags = 0U;

	/* Succeeded: handed. */
	if (session->handler != NULL)
		session->handler(session->handler_context, session, notice, sizeof(notice));
	return 1;
}

/* Tells whether a notice of dropped packets waits (sealed links, or events). */
static int
session_notice_waiting(
	const struct btd_session *session)
{
	unsigned index;

	/* A sealed link. */
	for (index = 0U; index < BTD_LINKS_MAX; index++) {
		if (session->links[index].used && session->links[index].sealed)
			return 1;
	}

	/* The events. */
	if (session->events_noticed)
		return 1;

	/* None. */
	return 0;
}

/* Tells whether an event is a scan's report, the least kept in the queue: an inquiry's result or an LE advertising report (legacy or extended). */
static int
session_report(
	const struct btd_event *event)
{
	int scan;

	/* The scans' results. */
	scan = session_scan_event(event);
	if (scan)
		return 1;

	/* LE's extended advertising report. */
	if (event->code == BTD_EVENT_LE_META && event->length >= 1U && event->parameters[0] == SESSION_LE_EXTENDED_REPORT)
		return 1;

	/* Anything else. */
	return 0;
}

/* Finds the oldest frame of a connection in the table.  Returns 1 with its index, or 0 when none waits. */
static int
session_oldest(
	const struct btd_session *session,
	uint16_t handle,
	unsigned *index)
{
	unsigned at;

	/* The first in the table's order. */
	for (at = 0U; at < session->frame_count; at++) {
		if (session->frames[at].handle == handle) {
			*index = at;
			return 1;
		}
	}

	/* None. */
	return 0;
}

/* Takes a frame sent whole out of the table: its connection has one frame less waiting, the later ones move up. */
static void
session_frame_done(
	struct btd_session *session,
	unsigned index)
{
	struct btd_link_count *link;

	/* One less for its connection. */
	link = session_link(session, session->frames[index].handle);
	if (link != NULL && link->frames != 0U)
		link->frames--;

	/* Succeeded: out of the table. */
	memmove(&session->frames[index], &session->frames[index + 1U], sizeof(session->frames[0]) * (session->frame_count - index - 1U));
	session->frame_count--;
}

/* Tells whether an event is a scan's result: an inquiry result of any form, or an LE advertising report. */
static int
session_scan_event(
	const struct btd_event *event)
{
	/* The inquiry's results. */
	if (event->code == BTD_EVENT_INQUIRY_RESULT)
		return 1;
	if (event->code == BTD_EVENT_INQUIRY_RSSI)
		return 1;
	if (event->code == BTD_EVENT_EXTENDED_INQUIRY)
		return 1;

	/* LE's advertising reports. */
	if (event->code == BTD_EVENT_LE_META && event->length >= 1U && event->parameters[0] == BTD_LE_ADVERTISING_REPORT)
		return 1;

	/* Anything else. */
	return 0;
}

/* Reads a connection handle (two bytes, least significant first, without the flags above its 12 bits). */
static uint16_t
session_handle(
	const uint8_t *bytes)
{
	unsigned value;

	/* The two bytes, then the handle's bits alone. */
	value = (unsigned)bytes[0] | ((unsigned)bytes[1] << 8);
	value &= SESSION_HANDLE_MASK;

	/* Succeeded: the handle. */
	return (uint16_t)value;
}
