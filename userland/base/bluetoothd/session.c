/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One controller's session in bluetoothd (ws143-p003, see session.h and
 * plan/ws143/phase003/phase.md sections 2 and 3).
 *
 * Commands go one at a time: the session writes one and reads until its
 * answer (a Command Complete or a Command Status with its opcode) comes or
 * its time runs out; an answer to another command is passed over and
 * counted, and the other events read meanwhile (a scan's results) are
 * handled as they come.
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
#define SESSION_READ_LOCAL_VERSION	0x1001U
#define SESSION_READ_COMMANDS		0x1002U
#define SESSION_READ_FEATURES		0x1003U
#define SESSION_READ_BUFFER_SIZE	0x1005U
#define SESSION_READ_ADDRESS		0x1009U
#define SESSION_LE_SET_EVENT_MASK	0x2001U
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

/* The Hardware Error event: the controller must be set up again. */
#define SESSION_EVENT_HARDWARE_ERROR	0x10U

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
 * Reads one packet the node has and handles it (a scan's result, an
 * inquiry's end).  For the daemon's loop when the node is readable.
 * Returns 0, EAGAIN when there was none, or ENODEV when the node went.
 */
int
btd_session_input(
	struct btd_session *session)
{
	int error;

	/* One packet, without waiting. */
	error = session_read(session, 0U);
	if (error == ETIMEDOUT)
		return EAGAIN;
	if (error != 0)
		return error;

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

	/* Succeeded: written. */
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

	/* Succeeded: a packet. */
	session->packet_length = (size_t)got;
	return 0;
}

/* Handles a packet no command waits for: a scan's results, an inquiry's end, the kernel's reset notice. */
static void
session_dispatch(
	struct btd_session *session)
{
	struct btd_event event;
	struct btd_answer answer;
	int taken;
	int error;

	/* The kernel's notice of a reset: the controller must be set up again (the daemon starts it again). */
	if (session->packet[0] == BT_PACKET_NOTICE_RESET) {
		session->resets++;
		session_set(session, BTD_STATE_ERROR, "%s", "the controller was reset");
		return;
	}

	/* Only events matter here (an ACL packet has no connection to go to yet). */
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

	/* A scan's result goes in the table while a scan runs; a malformed one is counted. */
	if (session->scanning) {
		taken = btd_devices_take(&session->devices, &event);
		if (taken < 0)
			session->malformed++;
	}
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

		/* Anything that is not an answer is handled as it comes. */
		error = btd_hci_event(session->packet, session->packet_length, &event);
		taken = 0;
		if (error == 0)
			taken = btd_hci_answer(&event, &answer);
		if (taken <= 0) {
			session_dispatch(session);
			continue;
		}

		/* An answer to another command is passed over. */
		if (answer.opcode != opcode) {
			session->stray_answers++;
			continue;
		}

		/* This command's answer: its return parameters, after the status. */
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

/* Reads until a vendor event (0xFF) whose first byte is code; other packets are handled as they come.  Returns 0, ETIMEDOUT or the node's error. */
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

		/* The vendor event asked for ends the wait. */
		error = btd_hci_event(session->packet, session->packet_length, &event);
		if (error == 0 &&
		    event.code == BTD_EVENT_VENDOR &&
		    event.length >= 1U &&
		    event.parameters[0] == code)
			return 0;

		/* Anything else is handled as it comes. */
		session_dispatch(session);
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
 * The HCI core's set-up (plan section 2, steps 2 to 8): reset, the version,
 * the address, the supported commands and features, the buffer size, the
 * event masks and the inquiry mode.  Returns 0, or the error with the state
 * set.
 */
static int
session_core(
	struct btd_session *session)
{
	/* Inquiry Complete, Inquiry Result, Hardware Error, Inquiry Result with RSSI, Extended Inquiry Result, LE Meta (bits 0, 1, 15, 33, 46, 61). */
	static const uint8_t event_mask[8] = { 0x03U, 0x80U, 0x00U, 0x00U, 0x02U, 0x40U, 0x00U, 0x20U };
	/* LE Advertising Report (bit 1). */
	static const uint8_t le_event_mask[8] = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
	static const uint8_t extended_inquiry[1] = { 0x02U };
	uint32_t acl;
	int le_mask;
	int le_parameters;
	int le_enable;
	int error;

	/* Reset. */
	error = session_step(session, "reset", SESSION_RESET, NULL, 0U);
	if (error != 0)
		return error;

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

	/* LE's scan needs LE Set Event Mask and LE Set Scan Parameters and Enable. */
	le_mask = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_MASK_OCTET, SESSION_BIT_LE_MASK);
	le_parameters = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_SCAN_OCTET, SESSION_BIT_LE_SCAN_PARAMETERS);
	le_enable = btd_hci_supported(session->commands, sizeof(session->commands), SESSION_BIT_LE_SCAN_OCTET, SESSION_BIT_LE_SCAN_ENABLE);
	session->le = 0;
	if (session->le_supported && le_mask && le_parameters && le_enable)
		session->le = 1;

	/* The buffer size: the longest ACL packet the controller takes tells the node. */
	error = session_command(session, SESSION_READ_BUFFER_SIZE, NULL, 0U, session->timing.command_ms);
	if (error == ENODEV)
		return error;
	if (error == 0 && session->returned_length >= 2U) {
		session->acl_length = (uint16_t)(session->returned[0] | (session->returned[1] << 8));
		acl = session->acl_length;
		if (session->control != NULL && acl >= BT_ACL_DATA_MIN && acl <= BT_ACL_DATA_MAX)
			(void)session_control(session, BT_IOC_SET_ACL_MAX, &acl);
	}

	/* The events the scan needs; without the mask no result would come. */
	error = session_step(session, "event-mask", SESSION_SET_EVENT_MASK, event_mask, sizeof(event_mask));
	if (error != 0)
		return error;

	/* LE's events; a controller that refuses them does not scan LE. */
	if (session->le) {
		error = session_command(session, SESSION_LE_SET_EVENT_MASK, le_event_mask, sizeof(le_event_mask), session->timing.command_ms);
		if (error == ENODEV)
			return error;
		if (error != 0)
			session->le = 0;
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
