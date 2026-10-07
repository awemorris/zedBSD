/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the UCSI core and the Type-C layer (ws050-p002).
 *
 * src/drivers/typec/ucsi.c and typec.c are compiled with the host compiler
 * and run over a fake PPM: a C model that answers the commands as UCSI 1.2
 * (and 3.1 for the 2.x arrangement) describes, keeps its own copy of the
 * mailbox, and counts every break of the acknowledgement rules (a command
 * sent while a completion is not acknowledged, a change acknowledged that
 * was not indicated).  Scenarios: the start, a plug, an unplug, two
 * changes at once, a busy PPM, the 2.x arrangement with the orientation,
 * and the choice of the arrangement; the robustness of ws177-p003: a
 * command the PPM fails and its reason, a command busy past its time and
 * its CANCEL, a PPM that stops and its reset, one that does not come back,
 * a PPM whose notifications never come, and the record of the mailbox
 * replayed.  Prints "PASS name" or "FAIL name ..." per check and exits
 * with 1 when one failed.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include "drivers/typec/typec-os.h"
#include "drivers/typec/ucsi.h"

/* The commands the fake PPM answers (UCSI 1.2 Table A-1). */
#define FAKE_PPM_RESET 0x01U
#define FAKE_CANCEL 0x02U
#define FAKE_GET_ERROR_STATUS 0x13U
#define FAKE_CONNECTOR_RESET 0x03U
#define FAKE_SET_UOR 0x09U
#define FAKE_SET_PDR 0x0BU
#define FAKE_SET_NEW_CAM 0x0FU
#define FAKE_GET_CABLE_PROPERTY 0x11U
#define FAKE_ACK_CC_CI 0x04U
#define FAKE_SET_NOTIFICATION_ENABLE 0x05U
#define FAKE_GET_CAPABILITY 0x06U
#define FAKE_GET_CONNECTOR_CAPABILITY 0x07U
#define FAKE_GET_ALTERNATE_MODES 0x0CU
#define FAKE_GET_CAM_SUPPORTED 0x0DU
#define FAKE_GET_CURRENT_CAM 0x0EU
#define FAKE_GET_PDOS 0x10U
#define FAKE_GET_CONNECTOR_STATUS 0x12U
#define FAKE_GET_CAM_CS 0x18U

/* The CCI indicators (Table 3-2). */
#define FAKE_CCI_CANCELED (1U << 26)
#define FAKE_CCI_RESET (1U << 27)
#define FAKE_CCI_BUSY (1U << 28)
#define FAKE_CCI_ACK (1U << 29)
#define FAKE_CCI_ERROR (1U << 30)
#define FAKE_CCI_DONE (1U << 31)

/* The most connectors and modes of the model. */
#define FAKE_CONNECTORS 4U
#define FAKE_MODES 8U
#define FAKE_QUEUE 8U

/* The mailbox's room (the 2.x arrangement's). */
#define FAKE_MAILBOX 0x210U

/* The most lines of the mailbox's record a test keeps, and the longest one. */
#define RECORD_LINES 1024U
#define RECORD_LINE 512U

/*
 * One connector of the fake PPM: what it can do, what is attached, and
 * what the attached partner and cable offer.
 */
struct fake_connector {
	uint32_t capability;
	bool connected;
	unsigned power_operation;
	bool provider;
	unsigned partner_flags;
	unsigned partner_type;
	uint32_t rdo;
	bool flipped;
	struct drv_typec_alt_mode modes[FAKE_MODES];
	unsigned mode_count;
	struct drv_typec_alt_mode partner_modes[FAKE_MODES];
	unsigned partner_mode_count;
	struct drv_typec_alt_mode cable_modes[FAKE_MODES];
	unsigned cable_mode_count;
	uint8_t supported;
	uint8_t current;
	uint32_t pdos[DRV_TYPEC_PDO_MAX];
	unsigned pdo_count;
	uint16_t change;
	uint32_t dp_status;
	uint32_t dp_configuration;
};

/*
 * The fake PPM: its mailbox, its connectors, the notifications enabled,
 * and the state of the acknowledgement rules.
 */
struct fake_ppm {
	const struct drv_ucsi_layout *layout;
	uint16_t version;
	uint8_t mailbox[FAKE_MAILBOX];
	unsigned connector_count;
	struct fake_connector connectors[FAKE_CONNECTORS];
	uint32_t optional_features;
	unsigned alt_mode_count;
	uint16_t notifications;

	/* A notification not yet taken by wait. */
	bool notify;

	/* A completion the OPM has not acknowledged. */
	bool completion_pending;

	/* The connector whose change is indicated and not acknowledged (0: none), and the changes not indicated yet. */
	unsigned change_indicated;
	unsigned queue[FAKE_QUEUE];
	unsigned queue_count;

	/*
	 * Set once a change was acknowledged without a command completion:
	 * the PPM then answers no command, as the Latitude 5320's does (the
	 * reason drv_ucsi acknowledges a change with a completion, ws050).
	 */
	bool stalled;

	/* Waits a command stays busy, and the command and its CCI kept for then. */
	unsigned busy_waits;
	uint64_t busy_control;

	/* The CONTROL of the last operation (reset, role, mode) the PPM was asked. */
	uint64_t operation_control;

	/* The GET_CAM_CS asked, and the current mode it named last. */
	unsigned cam_cs_asked;
	unsigned cam_cs_mode;

	/* The command the PPM fails (0: none) and the Error Information it gives for it (ws177-p003). */
	unsigned fail_command;
	uint16_t error_information;

	/* The GET_ERROR_STATUS asked: how many, the last CONTROL, and whether a failed command's completion was acknowledged first. */
	unsigned error_status_asked;
	uint64_t error_status_control;

	/* A command kept busy until a CANCEL (busy_held: one is), and the CANCELs received. */
	bool busy_forever;
	bool busy_held;
	unsigned cancels;

	/* The held command finishes as the CANCEL comes, so the PPM drops the CANCEL. */
	bool cancel_late;

	/* No notification ever sent (a platform whose Notify does not come). */
	bool mute;

	/* PPM_RESET does not complete (a PPM that is gone), and the resets received. */
	bool dead;
	unsigned resets;

	/* Commands answered, waits that refreshed, and breaks of the rules. */
	unsigned commands;
	unsigned violations;
	char last_violation[160];
};

/* The fake PPM the transport's operations use. */
static struct fake_ppm fake;

/* How many times the layer woke the connector driver for an operation. */
static unsigned kicked;

/* The checks that failed. */
static int test_failures;

/*
 * The mailbox's record the driver wrote to the log while capturing is
 * on (ws177-p003), a line each, and how many there are.
 */
static bool capturing;
static char record_lines[RECORD_LINES][RECORD_LINE];
static unsigned record_count;

/*
 * The replay of a record: the next line to give, the CONTROLs the driver
 * wrote that the record did not have, and the reads past its end.
 */
static unsigned replay_next;
static unsigned replay_number;
static unsigned replay_mismatches;
static unsigned replay_overruns;

/* The listener's calls, and the last connector and generation it was told. */
static unsigned listened;
static unsigned listened_connector;
static uint64_t listened_generation;

/* Whether the Type-C layer's lock is held (the layer must not nest it). */
static bool locked;

/* Whether to print the driver's log. */
static bool verbose;

/* The time the layer is told (milliseconds), moved on by the scenarios. */
static uint64_t test_now_ms;

/* How many DisplayPort disagreements the layer logged. */
static unsigned logged_disagreements;

static void fake_reset(const struct drv_ucsi_layout *layout, uint16_t version);
static void fake_violation(const char *what);
static void fake_set(uint8_t *data, unsigned offset, unsigned width, uint32_t value);
static uint32_t fake_get(uint64_t control, unsigned offset, unsigned width);
static void fake_complete(uint32_t length);
static void fake_answer(uint64_t control);
static void fake_answer_status(unsigned number, uint8_t *message);
static uint32_t fake_answer_modes(uint64_t control, uint8_t *message);
static uint32_t fake_answer_pdos(uint64_t control, uint8_t *message);
static void fake_indicate_next(void);
static void fake_event(unsigned number);
static int fake_write(void *context, uint64_t control, const uint8_t *message_out, size_t length);
static int fake_read(void *context, bool refresh, uint32_t *cci, uint8_t *message_in, size_t size);
static int fake_wait(void *context, uint32_t milliseconds);
static void test_listener(void *argument, unsigned connector, uint64_t generation);
static void test_check(const char *name, bool condition, const char *detail);
static void test_start_1(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_changes(struct drv_ucsi *ucsi);
static void test_busy(struct drv_ucsi *ucsi);
static void test_start_2(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_layouts(void);
static void test_requests(struct drv_ucsi *ucsi);
static void test_requests_2(struct drv_ucsi *ucsi);
static void test_kick(void *argument);
static bool test_take_run(struct drv_ucsi *ucsi, uint32_t serial);
static void test_display(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_locations(void);
static void test_failure_reason(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_cancel(struct drv_ucsi *ucsi);
static void test_recovery(struct drv_ucsi *ucsi);
static void test_mute(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_record_replay(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void test_layer_stop(struct drv_ucsi *ucsi, const struct drv_ucsi_transport *transport);
static void fake_three_connectors(uint16_t version);
static int replay_write(void *context, uint64_t control, const uint8_t *message_out, size_t length);
static int replay_read(void *context, bool refresh, uint32_t *cci, uint8_t *message_in, size_t size);
static int replay_wait(void *context, uint32_t milliseconds);
static unsigned replay_find(char kind, const char **fields);

/*
 * Runs the scenarios.
 */
int
main(
	int argc,
	char **argv)
{
	static struct drv_ucsi ucsi;
	struct drv_ucsi_transport transport;
	int compared;
	int error;

	/* -v prints the driver's log. */
	compared = 1;
	if (argc > 1)
		compared = strcmp(argv[1], "-v");
	if (compared == 0)
		verbose = true;

	/* The fake PPM's operations, and the listener. */
	transport.write = fake_write;
	transport.read = fake_read;
	transport.wait = fake_wait;
	transport.context = &fake;
	error = drv_typec_listener_register(test_listener, NULL);
	test_check("listener", error == 0, "registered");

	/* The scenarios. */
	test_start_1(&ucsi, &transport);
	test_changes(&ucsi);
	test_busy(&ucsi);
	test_requests(&ucsi);
	test_start_2(&ucsi, &transport);
	test_requests_2(&ucsi);
	test_display(&ucsi, &transport);
	test_layouts();
	test_locations();
	test_failure_reason(&ucsi, &transport);
	test_cancel(&ucsi);
	test_recovery(&ucsi);
	test_mute(&ucsi, &transport);
	test_record_replay(&ucsi, &transport);
	test_layer_stop(&ucsi, &transport);

	/* Reports whether every check passed. */
	if (test_failures != 0)
		return 1;

	/* Succeeded: every check passed. */
	return 0;
}

/*
 * Takes the layer's lock (the host has one thread; the test checks that
 * the lock is never taken twice).
 */
void
drv_typec_os_lock(void)
{
	/* A nested lock would deadlock in the kernel. */
	if (locked)
		fake_violation("the Type-C lock taken twice");
	locked = true;
}

/*
 * Releases the layer's lock.
 */
void
drv_typec_os_unlock(void)
{
	/* Released. */
	locked = false;
}

/*
 * Reports the scenario's time.
 */
uint64_t
drv_typec_os_now_ms(void)
{
	/* The time the scenarios set. */
	return test_now_ms;
}

/*
 * Counts the DisplayPort disagreements logged, and prints a line of the
 * driver's log when asked to.
 */
void
drv_typec_os_log(
	const char *format,
	...)
{
	va_list arguments;
	char line[512];
	int prefix;

	/* The line. */
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);

	/* A disagreement, counted. */
	if (strstr(line, "DisplayPort disagree") != NULL)
		logged_disagreements++;

	/* A line of the mailbox's record, kept while capturing. */
	prefix = strncmp(line, "ucsi: rec ", 10);
	if (capturing && prefix == 0 && record_count < RECORD_LINES) {
		(void)snprintf(record_lines[record_count], RECORD_LINE, "%s", line);
		record_count++;
	}

	/* Printed with -v. */
	if (verbose)
		(void)fputs(line, stdout);
}

/* Starts the fake PPM afresh with an arrangement and a version. */
static void
fake_reset(
	const struct drv_ucsi_layout *layout,
	uint16_t version)
{
	/* Nothing known, the version in VERSION. */
	memset(&fake, 0, sizeof(fake));
	fake.layout = layout;
	fake.version = version;
	fake.mailbox[layout->version_offset] = (uint8_t)(version & 0xFFU);
	fake.mailbox[layout->version_offset + 1U] = (uint8_t)(version >> 8);
}

/* Counts a break of the rules and keeps the last one's words. */
static void
fake_violation(
	const char *what)
{
	/* Counted. */
	fake.violations++;
	(void)snprintf(fake.last_violation, sizeof(fake.last_violation), "%s", what);
}

/* Writes a field of up to 32 bits at a bit offset of a little-endian message. */
static void
fake_set(
	uint8_t *data,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	unsigned bit;
	unsigned at;

	/* Each bit. */
	for (bit = 0; bit < width; bit++) {
		at = offset + bit;
		if ((value >> bit) & 1U)
			data[at / 8U] |= (uint8_t)(1U << (at % 8U));
		else
			data[at / 8U] &= (uint8_t)~(1U << (at % 8U));
	}
}

/* Reads a field of CONTROL. */
static uint32_t
fake_get(
	uint64_t control,
	unsigned offset,
	unsigned width)
{
	uint64_t mask;

	/* The bits. */
	mask = (1ULL << width) - 1U;
	return (uint32_t)((control >> offset) & mask);
}

/*
 * Completes a command: CCI with the completion, the length of MESSAGE IN
 * and the connector whose change is indicated; a notification when the
 * completion notification is on.
 */
static void
fake_complete(
	uint32_t length)
{
	uint32_t cci;

	/* CCI. */
	cci = FAKE_CCI_DONE | (length << 8) | (fake.change_indicated << 1);
	memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));

	/* The OPM owes an acknowledgement, and is told. */
	fake.completion_pending = true;
	if ((fake.notifications & 1U) != 0)
		fake.notify = true;
}

/* Answers a command (not PPM_RESET or ACK_CC_CI) into MESSAGE IN. */
static void
fake_answer(
	uint64_t control)
{
	struct fake_connector *connector;
	uint8_t *message;
	unsigned command;
	unsigned number;
	uint32_t length;
	uint32_t first;
	uint32_t second;
	uint32_t cci;

	/* MESSAGE IN, emptied. */
	message = &fake.mailbox[fake.layout->message_in_offset];
	memset(message, 0, fake.layout->message_in_size);
	command = (unsigned)(control & 0xFFU);
	number = fake_get(control, 16, 7);
	connector = NULL;
	if (number >= 1U && number <= fake.connector_count)
		connector = &fake.connectors[number - 1U];
	length = 0;

	/* The command the test makes the PPM fail: an error, and its reason kept for GET_ERROR_STATUS. */
	if (fake.fail_command != 0U && command == fake.fail_command) {
		cci = FAKE_CCI_DONE | FAKE_CCI_ERROR | (fake.change_indicated << 1);
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		fake.completion_pending = true;
		if ((fake.notifications & 1U) != 0)
			fake.notify = true;
		return;
	}

	/* Each command's answer. */
	switch (command) {
	case FAKE_GET_ERROR_STATUS:
		/* The reason of the last failure (3.1 Table 6-48), and what was asked. */
		fake.error_status_asked++;
		fake.error_status_control = control;
		fake_set(message, 0, 16, fake.error_information);
		length = 16;
		break;
	case FAKE_SET_NOTIFICATION_ENABLE:
		fake.notifications = (uint16_t)fake_get(control, 16, 16);
		break;
	case FAKE_GET_CAPABILITY:
		fake_set(message, 0, 32, 1U << 2);
		fake_set(message, 32, 7, fake.connector_count);
		fake_set(message, 40, 24, fake.optional_features);
		fake_set(message, 64, 8, fake.alt_mode_count);
		fake_set(message, 96, 16, 0x0300U);
		length = 16;
		break;
	case FAKE_GET_CONNECTOR_CAPABILITY:
		if (connector != NULL)
			fake_set(message, 0, 16, connector->capability);
		length = 2;
		break;
	case FAKE_GET_CONNECTOR_STATUS:
		fake_answer_status(number, message);
		length = 9;
		if (fake.version >= 0x0200U)
			length = 19;
		break;
	case FAKE_GET_ALTERNATE_MODES:
		length = fake_answer_modes(control, message);
		break;
	case FAKE_GET_CAM_SUPPORTED:
		if (connector != NULL)
			message[0] = connector->supported;
		length = 1;
		break;
	case FAKE_GET_CURRENT_CAM:
		if (connector != NULL)
			message[0] = connector->current;
		length = 1;
		break;
	case FAKE_GET_PDOS:
		length = fake_answer_pdos(control, message);
		break;
	case FAKE_GET_CAM_CS:
		/* The mode asked, its DisplayPort Status, and one VDO: its Configuration (3.1 Table 6-60). */
		fake.cam_cs_asked++;
		fake.cam_cs_mode = fake_get(control, 24, 8);
		if (fake.version < 0x0300U)
			fake_violation("GET_CAM_CS asked of a PPM before 3.0");
		if (connector != NULL) {
			fake_set(message, 0, 8, fake.cam_cs_mode);
			fake_set(message, 8, 32, connector->dp_status);
			fake_set(message, 40, 8, 1);
			fake_set(message, 48, 32, connector->dp_configuration);
		}
		length = 10;
		break;
	case FAKE_CONNECTOR_RESET:
		fake.operation_control = control;
		break;
	case FAKE_SET_UOR:
		/* A swap to DFP leaves a UFP partner, to UFP a DFP one. */
		fake.operation_control = control;
		first = fake_get(control, 23, 1);
		second = fake_get(control, 24, 1);
		if (connector != NULL && first != 0)
			connector->partner_type = 2;
		if (connector != NULL && second != 0)
			connector->partner_type = 1;
		break;
	case FAKE_SET_PDR:
		/* A swap to Source or to Sink. */
		fake.operation_control = control;
		first = fake_get(control, 23, 1);
		second = fake_get(control, 24, 1);
		if (connector != NULL && first != 0)
			connector->provider = true;
		if (connector != NULL && second != 0)
			connector->provider = false;
		break;
	case FAKE_SET_NEW_CAM:
		/* The mode entered, or none any more. */
		fake.operation_control = control;
		first = fake_get(control, 23, 1);
		second = fake_get(control, 24, 8);
		if (connector != NULL && first != 0)
			connector->current = (uint8_t)second;
		if (connector != NULL && first == 0)
			connector->current = 0xFFU;
		break;
	case FAKE_GET_CABLE_PROPERTY:
		/* A passive Type-C cable of 10 Gb/s and 5 A that carries VBUS. */
		fake_set(message, 0, 2, 3);
		fake_set(message, 2, 14, 10);
		fake_set(message, 16, 8, 100);
		fake_set(message, 24, 1, 1);
		fake_set(message, 27, 2, 2);
		length = 5;
		break;
	default:
		fake_violation("a command the fake PPM does not know");
		break;
	}

	/* Done. */
	fake_complete(length);
}

/* Writes GET_CONNECTOR_STATUS's answer (1.2 Table 4-42, 3.1 Table 6-43). */
static void
fake_answer_status(
	unsigned number,
	uint8_t *message)
{
	struct fake_connector *connector;

	/* A connector that is there. */
	if (number < 1U || number > fake.connector_count)
		return;
	connector = &fake.connectors[number - 1U];

	/* The change, then the state; reading it clears the change. */
	fake_set(message, 0, 16, connector->change);
	connector->change = 0;
	fake_set(message, 16, 3, connector->power_operation);
	fake_set(message, 19, 1, connector->connected);
	fake_set(message, 20, 1, connector->provider);
	fake_set(message, 21, 8, connector->partner_flags);
	fake_set(message, 29, 3, connector->partner_type);
	fake_set(message, 32, 32, connector->rdo);
	if (fake.version >= 0x0200U)
		fake_set(message, 86, 1, connector->flipped);
}

/*
 * Writes GET_ALTERNATE_MODES's answer (Table 4-24, 4-26): the modes of the
 * recipient from the offset, as many as asked and there are.  Returns the
 * length of the answer.
 */
static uint32_t
fake_answer_modes(
	uint64_t control,
	uint8_t *message)
{
	struct fake_connector *connector;
	const struct drv_typec_alt_mode *modes;
	unsigned recipient;
	unsigned number;
	unsigned offset;
	unsigned wanted;
	unsigned count;
	unsigned index;

	/* The fields. */
	recipient = fake_get(control, 16, 3);
	number = fake_get(control, 24, 7);
	offset = fake_get(control, 32, 8);
	wanted = fake_get(control, 40, 2) + 1U;
	if (wanted > 2U)
		fake_violation("GET_ALTERNATE_MODES asked for more than two");
	if (number < 1U || number > fake.connector_count)
		return 0;
	connector = &fake.connectors[number - 1U];

	/* The recipient's list. */
	modes = connector->modes;
	count = connector->mode_count;
	if (recipient == 1U) {
		modes = connector->partner_modes;
		count = connector->partner_mode_count;
	} else if (recipient == 2U) {
		modes = connector->cable_modes;
		count = connector->cable_mode_count;
	}

	/* The modes from the offset. */
	for (index = 0; index < wanted && offset + index < count; index++) {
		fake_set(message, index * 48U, 16, modes[offset + index].svid);
		fake_set(message, index * 48U + 16U, 32, modes[offset + index].vdo);
	}

	/* Six bytes a mode. */
	return index * 6U;
}

/*
 * Writes GET_PDOS's answer (Table 4-34, 4-36): the partner's PDOs from the
 * offset.  Returns the length of the answer.
 */
static uint32_t
fake_answer_pdos(
	uint64_t control,
	uint8_t *message)
{
	struct fake_connector *connector;
	unsigned number;
	unsigned offset;
	unsigned wanted;
	unsigned partner;
	unsigned index;

	/* The fields. */
	number = fake_get(control, 16, 7);
	offset = fake_get(control, 24, 8);
	wanted = fake_get(control, 32, 2) + 1U;
	partner = fake_get(control, 23, 1);
	if (partner == 0)
		fake_violation("GET_PDOS without the partner bit");
	if (offset + wanted - 1U > 7U)
		fake_violation("GET_PDOS past the seventh");
	if (number < 1U || number > fake.connector_count)
		return 0;
	connector = &fake.connectors[number - 1U];

	/* The PDOs from the offset. */
	for (index = 0; index < wanted && offset + index < connector->pdo_count; index++)
		fake_set(message, index * 32U, 32, connector->pdos[offset + index]);
	return index * 4U;
}

/* Indicates the next queued change when none is waiting for its acknowledgement. */
static void
fake_indicate_next(void)
{
	uint32_t cci;

	/* One change at a time (UCSI 1.2 section 4). */
	if (fake.change_indicated != 0 || fake.queue_count == 0)
		return;
	fake.change_indicated = fake.queue[0];
	memmove(&fake.queue[0], &fake.queue[1], (fake.queue_count - 1U) * sizeof(fake.queue[0]));
	fake.queue_count--;

	/* In CCI, with a notification when changes are notified. */
	memcpy(&cci, &fake.mailbox[fake.layout->cci_offset], sizeof(cci));
	cci = (cci & ~(0x7FU << 1)) | (fake.change_indicated << 1);
	memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
	if ((fake.notifications & (1U << 14)) != 0)
		fake.notify = true;
}

/* A connector changed (the test already set its new state and change bits). */
static void
fake_event(
	unsigned number)
{
	/* Queued, and indicated when nothing else is. */
	fake.queue[fake.queue_count++] = number;
	fake_indicate_next();
}

/* The transport's write: the fake PPM receives CONTROL. */
static int
fake_write(
	void *context,
	uint64_t control,
	const uint8_t *message_out,
	size_t length)
{
	unsigned command;
	unsigned acknowledged;
	unsigned change;
	uint32_t cci;

	/* The fake PPM is the file's; MESSAGE OUT is not used by these commands. */
	(void)context;
	(void)message_out;
	(void)length;

	/* CONTROL in the mailbox. */
	memcpy(&fake.mailbox[fake.layout->control_offset], &control, sizeof(control));
	command = (unsigned)(control & 0xFFU);
	fake.commands++;

	/* PPM_RESET: everything off, the reset completed (polled, no notification); a dead PPM does nothing. */
	if (command == FAKE_PPM_RESET) {
		fake.resets++;
		if (fake.dead)
			return 0;
		fake.stalled = false;
		fake.busy_held = false;
		fake.busy_waits = 0;
		fake.notifications = 0;
		fake.completion_pending = false;
		fake.change_indicated = 0;
		cci = FAKE_CCI_RESET;
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		return 0;
	}

	/* ACK_CC_CI: the acknowledgements it carries must be owed. */
	if (command == FAKE_ACK_CC_CI) {
		acknowledged = fake_get(control, 17, 1);
		change = fake_get(control, 16, 1);
		if (acknowledged != 0 && !fake.completion_pending)
			fake_violation("a completion acknowledged that was not owed");
		if (change != 0 && fake.change_indicated == 0)
			fake_violation("a change acknowledged that was not indicated");
		if (change != 0 && acknowledged == 0) {
			fake_violation("a change acknowledged without a command completion (the 5320's PPM stops answering)");
			fake.stalled = true;
		}
		if (acknowledged != 0)
			fake.completion_pending = false;
		if (change != 0)
			fake.change_indicated = 0;

		/* Acknowledged; the next change, if any, is indicated with it. */
		cci = FAKE_CCI_ACK;
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		fake_indicate_next();
		if ((fake.notifications & 1U) != 0)
			fake.notify = true;
		return 0;
	}

	/* CANCEL: only for a command answered busy (3.1 section 6.5.2); it ends that command. */
	if (command == FAKE_CANCEL) {
		fake.cancels++;
		if (!fake.busy_held && fake.busy_waits == 0) {
			fake_violation("a CANCEL without a busy command");
			return 0;
		}

		/* The busy command ends. */
		fake.busy_held = false;
		fake.busy_waits = 0;

		/* The command finished first: its answer, and the CANCEL dropped. */
		if (fake.cancel_late) {
			fake.cancel_late = false;
			fake_answer(fake.busy_control);
			return 0;
		}

		/* Canceled. */
		cci = FAKE_CCI_DONE | FAKE_CCI_CANCELED | (fake.change_indicated << 1);
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		fake.completion_pending = true;
		if ((fake.notifications & 1U) != 0)
			fake.notify = true;
		return 0;
	}

	/* Any other command: the previous completion must have been acknowledged. */
	if (fake.completion_pending)
		fake_violation("a command sent before the last completion was acknowledged");

	/* A stalled PPM answers nothing and leaves CCI empty. */
	if (fake.stalled) {
		cci = 0;
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		return 0;
	}

	/* A PPM kept busy answers only Busy, until a CANCEL. */
	if (fake.busy_forever) {
		fake.busy_forever = false;
		fake.busy_held = true;
		fake.busy_control = control;
		cci = FAKE_CCI_BUSY;
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		if ((fake.notifications & 1U) != 0)
			fake.notify = true;
		return 0;
	}

	/* A busy PPM answers later. */
	if (fake.busy_waits != 0) {
		fake.busy_control = control;
		cci = FAKE_CCI_BUSY;
		memcpy(&fake.mailbox[fake.layout->cci_offset], &cci, sizeof(cci));
		if ((fake.notifications & 1U) != 0)
			fake.notify = true;
		return 0;
	}

	/* The answer. */
	fake_answer(control);
	return 0;
}

/* The transport's read: CCI and MESSAGE IN as the mailbox holds them. */
static int
fake_read(
	void *context,
	bool refresh,
	uint32_t *cci,
	uint8_t *message_in,
	size_t size)
{
	(void)context;
	(void)refresh;

	/* The bytes. */
	memcpy(cci, &fake.mailbox[fake.layout->cci_offset], sizeof(*cci));
	memcpy(message_in, &fake.mailbox[fake.layout->message_in_offset], size);
	return 0;
}

/* The transport's wait: a notification at once, else the time passes (a busy command completes after its waits). */
static int
fake_wait(
	void *context,
	uint32_t milliseconds)
{
	(void)context;
	(void)milliseconds;

	/* A busy command: one wait less, answered after the last. */
	if (fake.busy_waits != 0) {
		fake.busy_waits--;
		if (fake.busy_waits == 0)
			fake_answer(fake.busy_control);
	}

	/* A platform whose notifications never come. */
	if (fake.mute) {
		fake.notify = false;
		return 0;
	}

	/* A notification, taken. */
	if (fake.notify) {
		fake.notify = false;
		return 1;
	}

	/* No notification. */
	return 0;
}

/* Counts the changes the layer tells. */
static void
test_listener(
	void *argument,
	unsigned connector,
	uint64_t generation)
{
	(void)argument;

	/* The call. */
	if (locked)
		fake_violation("a listener called under the lock");
	listened++;
	listened_connector = connector;
	listened_generation = generation;
}

/* Prints one check's result. */
static void
test_check(
	const char *name,
	bool condition,
	const char *detail)
{
	/* PASS, or FAIL with what was seen. */
	if (condition) {
		printf("PASS %s\n", name);
		return;
	}

	/* A failure, counted. */
	printf("FAIL %s: %s\n", name, detail);
	test_failures++;
}

/*
 * The start on UCSI 1.2: two connectors, the first attached to a partner
 * in DisplayPort Alternate Mode over a USB PD contract, the second empty.
 */
static void
test_start_1(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	struct fake_connector *first;
	char detail[320];
	int error;

	/* The fake PPM. */
	fake_reset(&drv_ucsi_layout_1, 0x0120U);
	fake.connector_count = 2;
	fake.optional_features = (1U << 2) | (1U << 4);
	fake.alt_mode_count = 2;
	first = &fake.connectors[0];
	first->capability = (1U << 2) | (1U << 5) | (1U << 6) | (1U << 7) | (1U << 8) | (1U << 9);
	first->connected = true;
	first->power_operation = 3;
	first->provider = false;
	first->partner_flags = 0x3U;
	first->partner_type = 2;
	first->rdo = 0x1304B12CU;
	first->modes[0].svid = 0xFF01U;
	first->modes[0].vdo = 0x001C0045U;
	first->modes[1].svid = 0x8087U;
	first->modes[1].vdo = 0x00000001U;
	first->mode_count = 2;
	first->partner_modes[0].svid = 0xFF01U;
	first->partner_modes[0].vdo = 0x000C0005U;
	first->partner_modes[1].svid = 0x1234U;
	first->partner_modes[1].vdo = 0x11U;
	first->partner_modes[2].svid = 0x5678U;
	first->partner_modes[2].vdo = 0x22U;
	first->partner_mode_count = 3;
	first->supported = 0x01U;
	first->current = 0;
	first->pdos[0] = 0x0801912CU;
	first->pdos[1] = 0x0002D12CU;
	first->pdos[2] = 0x0003C12CU;
	first->pdos[3] = 0x0004B12CU;
	first->pdos[4] = 0x00064145U;
	first->pdo_count = 5;
	fake.connectors[1].capability = (1U << 2) | (1U << 5) | (1U << 6);
	fake.connectors[1].current = 0xFFU;

	/* The start. */
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_1, 0x0120U);
	(void)snprintf(detail, sizeof(detail), "error %d", error);
	test_check("start-1.x", error == 0, detail);
	test_check("start-count", drv_typec_connector_count() == 2U, "not 2 connectors");
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("start-rules", fake.violations == 0, detail);
	test_check("start-notifications", (fake.notifications & ((1U << 14) | (1U << 11) | (1U << 8) | 1U)) == ((1U << 14) | (1U << 11) | (1U << 8) | 1U), "connect, partner, CAM or completion not on");

	/* The first connector's record. */
	(void)drv_typec_connector_get(0, &record);
	test_check("first-connected", record.connected && record.power_operation == DRV_TYPEC_POWER_PD && record.power_role == DRV_TYPEC_ROLE_SINK, "not attached, PD, sink");
	test_check("first-partner", record.partner_type == DRV_TYPEC_PARTNER_UFP && record.partner_flags == (DRV_TYPEC_PARTNER_USB | DRV_TYPEC_PARTNER_ALT_MODE) && record.request_data_object == 0x1304B12CU, "partner, flags or RDO");
	test_check("first-capability", record.capability == first->capability, "capability bits");
	test_check("first-connector-modes", record.connector_modes.count == 2U && record.connector_modes.modes[0].svid == DRV_TYPEC_SVID_DISPLAYPORT && record.connector_modes.modes[1].vdo == 1U, "the connector's modes");
	(void)snprintf(detail, sizeof(detail), "%u modes", record.partner_modes.count);
	test_check("first-partner-modes", record.partner_modes.count == 3U && record.partner_modes.modes[2].svid == 0x5678U && record.partner_modes.modes[2].vdo == 0x22U, detail);
	test_check("first-cable-modes", record.cable_modes.count == 0U, "a cable mode");
	test_check("first-current", record.current_mode_count == 1U && record.current_modes[0] == 0U && record.supported_modes[0] == 0x01U, "the current or supported modes");
	(void)snprintf(detail, sizeof(detail), "%u PDOs", record.partner_pdo_count);
	test_check("first-pdos", record.partner_pdo_count == 5U && record.partner_pdos[4] == 0x00064145U, detail);
	test_check("first-orientation", record.orientation == DRV_TYPEC_ORIENTATION_UNKNOWN, "a 1.x orientation");
	test_check("first-dp-1.x", !record.dp_ucsi.known && fake.cam_cs_asked == 0U && record.dp_source == DRV_TYPEC_DP_SOURCE_NONE, "GET_CAM_CS asked of a 1.2 PPM");

	/* The second, empty. */
	(void)drv_typec_connector_get(1, &record);
	test_check("second-empty", !record.connected && record.partner_type == DRV_TYPEC_PARTNER_NONE && record.current_mode_count == 0U, "attached");
}

/* A plug, an unplug, and two changes at once. */
static void
test_changes(
	struct drv_ucsi *ucsi)
{
	struct drv_typec_connector record;
	struct fake_connector *first;
	struct fake_connector *second;
	uint64_t before;
	char detail[320];
	int error;

	/* A charger-like partner on the second connector: this side the source at 3 A. */
	first = &fake.connectors[0];
	second = &fake.connectors[1];
	(void)drv_typec_connector_get(1, &record);
	before = record.generation;
	second->connected = true;
	second->power_operation = 5;
	second->provider = true;
	second->partner_flags = 0x1U;
	second->partner_type = 2;
	second->change = (uint16_t)(1U << 14);
	fake_event(2);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(1, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, connected %d, power %d, role %d", error, record.connected, record.power_operation, record.power_role);
	test_check("plug", error == 0 && record.connected && record.power_operation == DRV_TYPEC_POWER_TYPEC_3A && record.power_role == DRV_TYPEC_ROLE_SOURCE, detail);
	test_check("plug-generation", record.generation > before && listened_connector == 1U && listened_generation == record.generation, "no newer generation told");
	test_check("plug-acknowledged", fake.change_indicated == 0 && fake.violations == 0, fake.last_violation);

	/* The first connector's partner goes. */
	first->connected = false;
	first->change = (uint16_t)(1U << 14);
	fake_event(1);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(0, &record);
	test_check("unplug", error == 0 && !record.connected && record.partner_modes.count == 0U && record.partner_pdo_count == 0U && record.current_mode_count == 0U, "the partner's things kept");
	test_check("unplug-modes-kept", record.connector_modes.count == 2U, "the connector's own modes lost");

	/* Two changes before the OPM looks: the first back, the second gone. */
	first->connected = true;
	first->change = (uint16_t)(1U << 14);
	second->connected = false;
	second->change = (uint16_t)(1U << 14);
	fake_event(1);
	fake_event(2);
	error = drv_ucsi_service(ucsi);
	(void)snprintf(detail, sizeof(detail), "error %d, queue %u, indicated %u, breaks %u (%s)", error, fake.queue_count, fake.change_indicated, fake.violations, fake.last_violation);
	test_check("two-changes", error == 0 && fake.queue_count == 0U && fake.change_indicated == 0U && fake.violations == 0U, detail);
	(void)drv_typec_connector_get(0, &record);
	test_check("two-changes-first", record.connected && record.partner_modes.count == 3U, "the first not read again");
	(void)drv_typec_connector_get(1, &record);
	test_check("two-changes-second", !record.connected, "the second not read again");
}

/* A PPM busy for a while on a command. */
static void
test_busy(
	struct drv_ucsi *ucsi)
{
	struct drv_typec_connector record;
	char detail[320];
	int error;

	/* The second connector attached again, the status busy for three waits. */
	fake.connectors[1].connected = true;
	fake.connectors[1].power_operation = 1;
	fake.connectors[1].change = (uint16_t)(1U << 14);
	fake_event(2);
	fake.busy_waits = 3;
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(1, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, connected %d, breaks %u (%s)", error, record.connected, fake.violations, fake.last_violation);
	test_check("busy", error == 0 && record.connected && record.power_operation == DRV_TYPEC_POWER_USB_DEFAULT && fake.violations == 0U, detail);
}

/*
 * The start on the 2.x arrangement (version 2.0): one connector attached
 * with the plug flipped, carrying USB4, at 5 A.
 */
static void
test_start_2(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	struct fake_connector *only;
	char detail[320];
	int error;

	/* The fake PPM. */
	fake_reset(&drv_ucsi_layout_2, 0x0200U);
	fake.connector_count = 1;
	fake.optional_features = 0;
	only = &fake.connectors[0];
	only->capability = (1U << 2) | (1U << 6);
	only->connected = true;
	only->power_operation = 6;
	only->provider = false;
	only->partner_flags = 0x5U;
	only->partner_type = 1;
	only->flipped = true;
	only->current = 0xFFU;

	/* The start. */
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0200U);
	(void)snprintf(detail, sizeof(detail), "error %d, breaks %u (%s)", error, fake.violations, fake.last_violation);
	test_check("start-2.x", error == 0 && fake.violations == 0U, detail);
	(void)drv_typec_connector_get(0, &record);
	test_check("2.x-orientation", record.orientation == DRV_TYPEC_ORIENTATION_FLIPPED, "not flipped");
	test_check("2.x-status", record.power_operation == DRV_TYPEC_POWER_TYPEC_5A && record.partner_flags == (DRV_TYPEC_PARTNER_USB | DRV_TYPEC_PARTNER_USB4) && record.partner_type == DRV_TYPEC_PARTNER_DFP, "5 A, USB4 or DFP");
	test_check("2.x-count", drv_typec_connector_count() == 1U, "not one connector");
}

/* The choice of the arrangement by the region's size. */
static void
test_layouts(void)
{
	/* 5330's 0x38-byte region is 1.x; 0x210 bytes hold 2.x; 0x20 holds neither. */
	test_check("layout-0x38", drv_ucsi_layout_select(0x38U) == &drv_ucsi_layout_1, "not 1.x");
	test_check("layout-0x210", drv_ucsi_layout_select(0x210U) == &drv_ucsi_layout_2, "not 2.x");
	test_check("layout-0x20", drv_ucsi_layout_select(0x20U) == NULL, "an arrangement");
}

/* Counts a wake-up of the connector driver. */
static void
test_kick(
	void *argument)
{
	(void)argument;
	kicked++;
}

/* Takes the oldest operation, checks it is the one asked, and carries it out. */
static bool
test_take_run(
	struct drv_ucsi *ucsi,
	uint32_t serial)
{
	struct drv_typec_request request;
	bool taken;
	int error;

	/* The operation. */
	taken = drv_typec_request_take(&request);
	if (!taken || request.serial != serial)
		return false;

	/* Carried out. */
	error = drv_ucsi_request(ucsi, &request);
	if (error != 0)
		return false;
	return true;
}

/* The operations on a 1.2 PPM: roles, resets, modes, and the cable read with them. */
static void
test_requests(
	struct drv_ucsi *ucsi)
{
	struct drv_typec_connector record;
	struct drv_typec_request request;
	char text[2048];
	char detail[320];
	uint32_t serial;
	uint32_t other;
	unsigned commands;
	unsigned index;
	bool ran;
	int error;

	/* The PPM lets the OPM choose the mode and reports cables. */
	fake.optional_features |= (1U << 3) | (1U << 5);
	ucsi->optional_features = fake.optional_features;
	drv_typec_operator_set(test_kick, NULL);
	kicked = 0;

	/* A swap to Source on connector 1. */
	error = drv_typec_connector_set_power_role(0, DRV_TYPEC_ROLE_SOURCE, &serial);
	test_check("request-queued", error == 0 && serial != 0 && kicked == 1U, "the operation was not queued or the driver not woken");
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "control 0x%llx, role %d, serial %u error %d", (unsigned long long)fake.operation_control, record.power_role, (unsigned)record.request_serial, record.request_error);
	test_check("power-role", ran && fake.operation_control == (0x0BULL | (1ULL << 16) | (1ULL << 23) | (1ULL << 25)) && record.power_role == DRV_TYPEC_ROLE_SOURCE && record.request_serial == serial && record.request_error == 0, detail);
	test_check("power-role-told", listened_connector == 0U && listened_generation == record.generation, "the listener was not told");

	/* The cable, read with the connector. */
	(void)snprintf(detail, sizeof(detail), "known %d speed %llu current %u", record.cable.known, (unsigned long long)record.cable.speed_bps, record.cable.current_ma);
	test_check("cable", record.cable.known && record.cable.speed_bps == 10000000000ULL && record.cable.current_ma == 5000U && record.cable.vbus && !record.cable.active && record.cable.plug_end == DRV_TYPEC_PLUG_TYPE_C, detail);

	/* A swap to UFP. */
	(void)drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_UFP, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("data-role", ran && fake.operation_control == (0x09ULL | (1ULL << 16) | (1ULL << 24) | (1ULL << 25)) && record.partner_type == DRV_TYPEC_PARTNER_DFP && record.request_error == 0, "SET_UOR to UFP");

	/* A Hard Reset (1.2: bit 23 clear), and a Data Reset the 1.2 PPM is not asked. */
	(void)drv_typec_connector_reset(0, DRV_TYPEC_RESET_HARD, &serial);
	ran = test_take_run(ucsi, serial);
	test_check("reset-hard", ran && fake.operation_control == (0x03ULL | (1ULL << 16)), "CONNECTOR_RESET hard on 1.2");
	commands = fake.commands;
	fake.operation_control = 0;
	(void)drv_typec_connector_reset(0, DRV_TYPEC_RESET_DATA, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("reset-data-1.x", ran && fake.operation_control == 0 && record.request_serial == serial && record.request_error == ENOTSUP && fake.commands > commands, "a Data Reset on 1.2 is not supported (the connector is read again)");

	/* DisplayPort entered with a configuration, then left. */
	(void)drv_typec_connector_enter_mode(0, 0, 0x00000406U, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "control 0x%llx, current %u/%u", (unsigned long long)fake.operation_control, record.current_mode_count, record.current_modes[0]);
	test_check("enter-mode", ran && fake.operation_control == (0x0FULL | (1ULL << 16) | (1ULL << 23) | (0x00000406ULL << 32)) && record.current_mode_count == 1U && record.current_modes[0] == 0U, detail);
	(void)drv_typec_connector_exit_mode(0, 0, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("exit-mode", ran && fake.operation_control == (0x0FULL | (1ULL << 16)) && record.current_mode_count == 0U, "SET_NEW_CAM exit");

	/* A mode the connector does not list. */
	(void)drv_typec_connector_enter_mode(0, 5, 0, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("enter-unknown", ran && record.request_error == EINVAL, "a mode beyond the list");

	/* The text shows the cable and the last operation. */
	(void)drv_typec_text(text, sizeof(text));
	(void)snprintf(detail, sizeof(detail), " error=%d ", EINVAL);
	test_check("text-cable", strstr(text, "cable=passive speed=10000000000 current=5000mA") != NULL && strstr(text, detail) != NULL, text);

	/* A connector that is not there, and a full queue. */
	error = drv_typec_connector_reset(7, DRV_TYPEC_RESET_HARD, &other);
	test_check("request-absent", error == ENOENT, "a connector that is not there");
	for (index = 0; index < DRV_TYPEC_REQUEST_MAX; index++)
		(void)drv_typec_connector_reset(1, DRV_TYPEC_RESET_HARD, &other);
	error = drv_typec_connector_reset(1, DRV_TYPEC_RESET_HARD, &other);
	test_check("request-full", error == EBUSY, "a ninth operation");
	for (index = 0; index < DRV_TYPEC_REQUEST_MAX; index++)
		(void)drv_typec_request_take(&request);

	/* No rule broken. */
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("request-rules", fake.violations == 0, detail);
}

/* The resets on a 2.x PPM: bit 23 set for a Data Reset, clear for a Hard Reset. */
static void
test_requests_2(
	struct drv_ucsi *ucsi)
{
	uint32_t serial;
	bool ran;

	/* A Data Reset. */
	(void)drv_typec_connector_reset(0, DRV_TYPEC_RESET_DATA, &serial);
	ran = test_take_run(ucsi, serial);
	test_check("reset-data-2.x", ran && fake.operation_control == (0x03ULL | (1ULL << 16) | (1ULL << 23)), "CONNECTOR_RESET data on 2.x");

	/* A Hard Reset. */
	(void)drv_typec_connector_reset(0, DRV_TYPEC_RESET_HARD, &serial);
	ran = test_take_run(ucsi, serial);
	test_check("reset-hard-2.x", ran && fake.operation_control == (0x03ULL | (1ULL << 16)), "CONNECTOR_RESET hard on 2.x");
}

/*
 * DisplayPort from two sources on a 3.1 PPM: UCSI's GET_CAM_CS alone, the
 * display driver's report before and after its port is bound, a difference
 * that becomes a disagreement after DRV_TYPEC_DISAGREE_MS and is logged
 * once, agreement again, a second binding refused, and an unbinding.
 */
static void
test_display(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	struct drv_typec_display display;
	struct drv_typec_dp_state report;
	struct fake_connector *only;
	uint64_t before;
	uint32_t wait;
	char text[2048];
	char detail[320];
	unsigned logged;
	int error;

	/* A 3.1 PPM with one connector in DisplayPort mode, HPD high, pin D. */
	fake_reset(&drv_ucsi_layout_2, 0x0310U);
	fake.connector_count = 1;
	fake.optional_features = 1U << 2;
	fake.alt_mode_count = 1;
	only = &fake.connectors[0];
	only->capability = (1U << 2) | (1U << 7);
	only->connected = true;
	only->power_operation = 1;
	only->partner_flags = 0x2U;
	only->partner_type = 2;
	only->modes[0].svid = 0xFF01U;
	only->modes[0].vdo = 0x001C0045U;
	only->mode_count = 1;
	only->partner_modes[0].svid = 0xFF01U;
	only->partner_modes[0].vdo = 0x000C0005U;
	only->partner_mode_count = 1;
	only->supported = 0x01U;
	only->current = 0;
	only->dp_status = (1U << 7) | 0x2U;
	only->dp_configuration = (1U << 11) | 0x2U;
	test_now_ms = 1000;

	/* UCSI's report alone. */
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0310U);
	(void)snprintf(detail, sizeof(detail), "error %d, breaks %u (%s)", error, fake.violations, fake.last_violation);
	test_check("dp-start-3.x", error == 0 && fake.violations == 0U, detail);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "asked %u mode %u, known %d hpd %d pin %d source %d", fake.cam_cs_asked, fake.cam_cs_mode, record.dp_ucsi.known, record.dp_ucsi.hpd, (int)record.dp_ucsi.pin, (int)record.dp_source);
	test_check("dp-ucsi", fake.cam_cs_asked != 0 && fake.cam_cs_mode == 0 && record.dp_ucsi.known && record.dp_ucsi.hpd && record.dp_ucsi.pin == DRV_TYPEC_DP_PIN_D, detail);
	test_check("dp-ucsi-taken", record.dp_source == DRV_TYPEC_DP_SOURCE_UCSI && record.dp.hpd && record.dp.pin == DRV_TYPEC_DP_PIN_D && record.display_port == DRV_TYPEC_DISPLAY_PORT_NONE && !record.dp_disagree, detail);

	/* The display driver's report of an unbound port goes into no connector. */
	before = record.generation;
	listened = 0;
	kicked = 0;
	drv_typec_operator_set(test_kick, NULL);
	memset(&report, 0, sizeof(report));
	report.hpd = true;
	report.pin = DRV_TYPEC_DP_PIN_C;
	report.lanes = 4;
	error = drv_typec_display_report(0, &report);
	(void)drv_typec_connector_get(0, &record);
	(void)drv_typec_display_get(0, &display);
	test_check("dp-unbound", error == 0 && record.generation == before && listened == 0U && record.dp_source == DRV_TYPEC_DP_SOURCE_UCSI, "an unbound report changed the connector");
	test_check("dp-unbound-kept", display.generation != 0 && display.state.known && display.state.pin == DRV_TYPEC_DP_PIN_C && display.connector == DRV_TYPEC_CONNECTOR_NONE, "the report was not kept");

	/* Bound: the display driver's report is taken, UCSI's beside it; pin C and pin D start a wait. */
	error = drv_typec_display_bind(0, 0);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, told %u, port %u, source %d, pin %d, lanes %u", error, listened, record.display_port, (int)record.dp_source, (int)record.dp.pin, record.dp.lanes);
	test_check("dp-bound", error == 0 && listened == 1U && listened_generation == record.generation && record.display_port == 0U && record.dp_source == DRV_TYPEC_DP_SOURCE_DISPLAY && record.dp.pin == DRV_TYPEC_DP_PIN_C && record.dp.lanes == 4U, detail);
	wait = drv_typec_display_check();
	(void)snprintf(detail, sizeof(detail), "wait %u, disagree %d", (unsigned)wait, record.dp_disagree);
	test_check("dp-wait", wait == DRV_TYPEC_DISAGREE_MS && !record.dp_disagree, detail);

	/* Half the wait later the difference still waits; the same report again kicks the thread and is no new change. */
	test_now_ms += 100;
	before = record.generation;
	listened = 0;
	error = drv_typec_display_report(0, &report);
	wait = drv_typec_display_check();
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "wait %u, disagree %d, kicked %u, logged %u", (unsigned)wait, record.dp_disagree, kicked, logged_disagreements);
	test_check("dp-wait-half", error == 0 && wait == 100U && !record.dp_disagree && kicked == 1U && logged_disagreements == 0U, detail);
	test_check("dp-same-report", record.generation == before && listened == 0U, "the same report again was told as a change");

	/* The wait over: a disagreement, logged once, shown in the text. */
	test_now_ms += 150;
	logged = logged_disagreements;
	wait = drv_typec_display_check();
	(void)drv_typec_connector_get(0, &record);
	(void)drv_typec_display_check();
	(void)snprintf(detail, sizeof(detail), "wait %u, disagree %d, logged %u", (unsigned)wait, record.dp_disagree, logged_disagreements - logged);
	test_check("dp-disagree", wait == 0 && record.dp_disagree && logged_disagreements - logged == 1U, detail);
	(void)drv_typec_text(text, sizeof(text));
	test_check("dp-text", strstr(text, " display-port=1 hpd=1(display) ucsi=1 pin=C(display) ucsi=D lanes=4 disagree") != NULL && strstr(text, "display-port 1: hpd=1 pin=C lanes=4 connector=1 generation=") != NULL, text);

	/* ws177-p003: the display driver reads pin E: the disagreement goes on and is logged again; the same report again is not. */
	logged = logged_disagreements;
	report.pin = DRV_TYPEC_DP_PIN_E;
	error = drv_typec_display_report(0, &report);
	(void)drv_typec_display_check();
	(void)drv_typec_display_report(0, &report);
	(void)drv_typec_display_check();
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, disagree %d, logged %u", error, record.dp_disagree, logged_disagreements - logged);
	test_check("dp-disagree-changed", error == 0 && record.dp_disagree && logged_disagreements - logged == 1U, detail);

	/* The display driver now reads pin D: the reports agree and the disagreement ends. */
	report.pin = DRV_TYPEC_DP_PIN_D;
	error = drv_typec_display_report(0, &report);
	wait = drv_typec_display_check();
	(void)drv_typec_connector_get(0, &record);
	test_check("dp-agree", error == 0 && wait == 0 && !record.dp_disagree && record.dp.pin == DRV_TYPEC_DP_PIN_D, "the disagreement did not end");

	/* ws177-p003: the display driver stops: its report is forgotten, UCSI's taken, the listeners told. */
	listened = 0;
	error = drv_typec_display_forget(0);
	(void)drv_typec_connector_get(0, &record);
	(void)drv_typec_display_get(0, &display);
	(void)snprintf(detail, sizeof(detail), "error %d, told %u, display known %d, source %d", error, listened, record.dp_display.known, (int)record.dp_source);
	test_check("dp-forget", error == 0 && listened == 1U && !record.dp_display.known && !display.state.known && record.dp_source == DRV_TYPEC_DP_SOURCE_UCSI && !record.dp_disagree, detail);
	error = drv_typec_display_forget(DRV_TYPEC_DISPLAY_PORT_MAX);
	test_check("dp-forget-range", error == EINVAL, "a port beyond the room");

	/* A second port is not bound to the same connector; a port and a connector beyond the room are refused. */
	error = drv_typec_display_bind(1, 0);
	test_check("dp-bind-busy", error == EBUSY, "a second port bound to one connector");
	error = drv_typec_display_bind(DRV_TYPEC_DISPLAY_PORT_MAX, 0);
	test_check("dp-bind-range", error == EINVAL, "a port beyond the room");
	error = drv_typec_display_report(DRV_TYPEC_DISPLAY_PORT_MAX, &report);
	test_check("dp-report-range", error == EINVAL, "a report beyond the room");

	/* Unbound: UCSI's report is taken again. */
	error = drv_typec_display_bind(0, DRV_TYPEC_CONNECTOR_NONE);
	(void)drv_typec_connector_get(0, &record);
	test_check("dp-unbind", error == 0 && record.display_port == DRV_TYPEC_DISPLAY_PORT_NONE && record.dp_source == DRV_TYPEC_DP_SOURCE_UCSI, "the binding was not removed");

	/* The partner leaves: UCSI's report is no longer known. */
	only->connected = false;
	only->change = (uint16_t)(1U << 14);
	fake_event(1);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(0, &record);
	test_check("dp-unplug", error == 0 && !record.dp_ucsi.known && record.dp_source == DRV_TYPEC_DP_SOURCE_NONE, "UCSI's DisplayPort kept after the unplug");

	/* No rule broken. */
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("dp-rules", fake.violations == 0, detail);
}

/*
 * ws050-p005: a _PLD buffer read into a location (the 5330's: ssdt8's TPLD
 * of revision 2 and ssdt12's PLCA of revision 1), and a display port
 * matched to the one visible connector at its place.
 */
static void
test_locations(void)
{
	/* ssdt8's TPLD (One, 2): revision 2, visible, group position 2, round, 8 by 3. */
	static const uint8_t tpld[16] = { 0x82, 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0x00, 0x11, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
	/* ssdt12's PLCA: revision 1, visible, group position 1. */
	static const uint8_t plca[16] = { 0x81, 0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0x00, 0x71, 0x04, 0x80, 0x00, 0x03, 0x00, 0x00, 0x00 };
	/* ssdt12's PLDU: revision 1, not visible. */
	static const uint8_t pldu[16] = { 0x81, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	/* Group token 0x55, position 0xAA: bits 86:79 and 94:87. */
	static const uint8_t token[16] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x2A, 0x55, 0x00, 0x00, 0x00, 0x00 };
	struct drv_typec_location connectors[3];
	struct drv_typec_location port;
	unsigned found;
	int error;

	/* The firmware's buffers. */
	error = drv_typec_location_decode(tpld, sizeof(tpld), &port);
	test_check("pld-tpld", error == 0 && port.known && port.visible && port.group_token == 0U && port.group_position == 2U, "visible, group 0, position 2");
	error = drv_typec_location_decode(plca, sizeof(plca), &port);
	test_check("pld-plca", error == 0 && port.visible && port.group_position == 1U, "visible, position 1");
	error = drv_typec_location_decode(pldu, sizeof(pldu), &port);
	test_check("pld-pldu", error == 0 && !port.visible && port.group_position == 0U, "not visible");
	error = drv_typec_location_decode(token, sizeof(token), &port);
	test_check("pld-fields", error == 0 && port.visible && port.group_token == 0x55U && port.group_position == 0xAAU, "token 0x55, position 0xaa");

	/* Too short, revision 0, none. */
	error = drv_typec_location_decode(plca, 15U, &port);
	test_check("pld-short", error == EINVAL && !port.known, "15 bytes refused");
	error = drv_typec_location_decode(pldu + 1, 15U, &port);
	test_check("pld-short-2", error == EINVAL, "refused");
	error = drv_typec_location_decode(NULL, 16U, &port);
	test_check("pld-null", error == EINVAL, "refused");

	/* Connectors at positions 2 and 1 (CR01 2, CR02 1), and a hidden one at 1. */
	(void)drv_typec_location_decode(tpld, sizeof(tpld), &connectors[0]);
	(void)drv_typec_location_decode(plca, sizeof(plca), &connectors[1]);
	connectors[2] = connectors[1];
	connectors[2].visible = false;
	(void)drv_typec_location_decode(plca, sizeof(plca), &port);
	found = drv_typec_location_match(connectors, 3U, &port);
	test_check("match-1", found == 1U, "position 1 is connector 1 (the hidden one does not count)");
	port.group_position = 2U;
	found = drv_typec_location_match(connectors, 3U, &port);
	test_check("match-2", found == 0U, "position 2 is connector 0");
	port.group_position = 5U;
	found = drv_typec_location_match(connectors, 3U, &port);
	test_check("match-none", found == DRV_TYPEC_CONNECTOR_NONE, "no connector at position 5");

	/* Two visible connectors at one place: uncertain, none. */
	connectors[2].visible = true;
	port.group_position = 1U;
	found = drv_typec_location_match(connectors, 3U, &port);
	test_check("match-two", found == DRV_TYPEC_CONNECTOR_NONE, "two connectors at position 1");

	/* A port that is not visible, or whose location is unknown, matches nothing. */
	(void)drv_typec_location_decode(pldu, sizeof(pldu), &port);
	found = drv_typec_location_match(connectors, 2U, &port);
	test_check("match-hidden", found == DRV_TYPEC_CONNECTOR_NONE, "a hidden port");
	memset(&port, 0, sizeof(port));
	found = drv_typec_location_match(connectors, 2U, &port);
	test_check("match-unknown", found == DRV_TYPEC_CONNECTOR_NONE, "an unknown port");
}

/*
 * ws177-p003: a fake PPM of UCSI `version` on the 2.x arrangement with
 * three connectors, the first attached to a UFP partner, the others empty.
 */
static void
fake_three_connectors(
	uint16_t version)
{
	/* The PPM. */
	fake_reset(&drv_ucsi_layout_2, version);
	fake.connector_count = 3;
	fake.optional_features = 0;
	fake.connectors[0].capability = (1U << 2) | (1U << 5) | (1U << 10) | (1U << 11) | (1U << 12) | (1U << 13);
	fake.connectors[0].connected = true;
	fake.connectors[0].power_operation = 1;
	fake.connectors[0].partner_flags = 0x1U;
	fake.connectors[0].partner_type = 2;
	fake.connectors[0].current = 0xFFU;
	fake.connectors[1].capability = (1U << 2) | (1U << 5);
	fake.connectors[1].current = 0xFFU;
	fake.connectors[2].capability = (1U << 2) | (1U << 5);
	fake.connectors[2].current = 0xFFU;
}

/*
 * ws177-p003: a command the PPM fails.  Its completion is acknowledged,
 * then GET_ERROR_STATUS is asked (naming the connector from 3.0, not on
 * 1.x), and the operation's outcome is the errno value of the reason with
 * the reason's bits in the record.
 */
static void
test_failure_reason(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	uint32_t serial;
	char detail[320];
	bool ran;
	int error;

	/* A 3.0 PPM: a power role swap rejected (Swap Rejected, bit 12). */
	fake_three_connectors(0x0300U);
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	test_check("reason-start", error == 0 && fake.violations == 0U, fake.last_violation);
	drv_typec_operator_set(test_kick, NULL);
	fake.fail_command = FAKE_SET_PDR;
	fake.error_information = 1U << 12;
	error = drv_typec_connector_set_power_role(0, DRV_TYPEC_ROLE_SOURCE, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d ran %d serial %u/%u outcome %d information 0x%x", error, ran, (unsigned)record.request_serial, (unsigned)serial, record.request_error, (unsigned)record.request_error_information);
	test_check("reason-swap-rejected", error == 0 && ran && record.request_serial == serial && record.request_error == EPERM && record.request_error_information == (1U << 12), detail);
	(void)snprintf(detail, sizeof(detail), "asked %u, control 0x%llx", fake.error_status_asked, (unsigned long long)fake.error_status_control);
	test_check("reason-asked-3.0", fake.error_status_asked == 1U && fake.error_status_control == (FAKE_GET_ERROR_STATUS | (1ULL << 16)), detail);
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("reason-acknowledged", fake.violations == 0U && !fake.completion_pending, detail);

	/* The next operation, which the PPM does not fail, has no reason. */
	fake.fail_command = 0U;
	error = drv_typec_connector_set_power_role(0, DRV_TYPEC_ROLE_SINK, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("reason-cleared", error == 0 && ran && record.request_error == 0 && record.request_error_information == 0U, "a reason kept from before");

	/* An incompatible partner (bit 3) is EOPNOTSUPP, a non-existent connector (bit 1) EINVAL. */
	fake.fail_command = FAKE_SET_UOR;
	fake.error_information = 1U << 3;
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_UFP, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("reason-incompatible", ran && record.request_error == EOPNOTSUPP, "not EOPNOTSUPP");
	fake.error_information = (1U << 1) | (1U << 3);
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_DFP, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	test_check("reason-no-connector", ran && record.request_error == EINVAL && record.request_error_information == ((1U << 1) | (1U << 3)), "not EINVAL with both bits");
	fake.fail_command = 0U;

	/* A 1.2 PPM: GET_ERROR_STATUS names no connector (the field is reserved there). */
	fake_three_connectors(0x0120U);
	fake.layout = &drv_ucsi_layout_1;
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_1, 0x0120U);
	test_check("reason-start-1.2", error == 0, "the 1.2 PPM did not start");
	fake.fail_command = FAKE_SET_PDR;
	fake.error_information = 1U << 2;
	error = drv_typec_connector_set_power_role(0, DRV_TYPEC_ROLE_SOURCE, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "outcome %d, control 0x%llx", record.request_error, (unsigned long long)fake.error_status_control);
	test_check("reason-1.2", ran && record.request_error == EINVAL && fake.error_status_control == FAKE_GET_ERROR_STATUS, detail);
	fake.fail_command = 0U;

	/* A partner without Alternate Modes, whose GET_ALTERNATE_MODES the PPM fails, still reads whole. */
	fake.optional_features = 1U << 2;
	ucsi->optional_features = 1U << 2;
	fake.fail_command = FAKE_GET_ALTERNATE_MODES;
	fake.error_information = 1U << 3;
	fake.connectors[0].change = (uint16_t)(1U << 14);
	fake_event(1);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, %u breaks (%s)", error, fake.violations, fake.last_violation);
	test_check("reason-ignored-modes", error == 0 && record.connected && record.partner_modes.count == 0U && fake.violations == 0U, detail);
	fake.fail_command = 0U;
	fake.optional_features = 0U;
	ucsi->optional_features = 0U;
	drv_typec_operator_set(NULL, NULL);
}

/*
 * ws177-p003: a command the PPM keeps busy past its time is canceled; the
 * operation's outcome is ETIMEDOUT, the PPM is not reset, and the next
 * commands run.
 */
static void
test_cancel(
	struct drv_ucsi *ucsi)
{
	struct drv_typec_connector record;
	uint32_t serial;
	unsigned resets;
	char detail[320];
	bool ran;
	int error;

	/* A data role swap the PPM keeps busy. */
	drv_typec_operator_set(test_kick, NULL);
	resets = fake.resets;
	fake.busy_forever = true;
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_UFP, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d ran %d outcome %d cancels %u resets %u stuck %d", error, ran, record.request_error, fake.cancels, fake.resets - resets, ucsi->stuck);
	test_check("cancel-timeout", ran && record.request_serial == serial && record.request_error == ETIMEDOUT && fake.cancels == 1U, detail);
	test_check("cancel-no-reset", fake.resets == resets && !ucsi->stuck && !ucsi->failed, detail);
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("cancel-rules", fake.violations == 0U && !fake.completion_pending, detail);

	/* The command finishes as the CANCEL comes: the PPM drops the CANCEL, and the outcome is the command's. */
	fake.busy_forever = true;
	fake.cancel_late = true;
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_DFP, &serial);
	ran = test_take_run(ucsi, serial);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "ran %d outcome %d, %u breaks (%s)", ran, record.request_error, fake.violations, fake.last_violation);
	test_check("cancel-late", ran && record.request_serial == serial && record.request_error == 0 && fake.violations == 0U && !fake.completion_pending, detail);
	drv_typec_operator_set(NULL, NULL);
}

/*
 * ws177-p003: a PPM that stops answering (the stall after a lone change
 * acknowledgement) is reset and read again; the last operation's outcome
 * stays in the record.  A PPM that does not come back fails the interface
 * after three resets, and an operation then ends with ENODEV.
 */
static void
test_recovery(
	struct drv_ucsi *ucsi)
{
	struct drv_typec_connector record;
	struct drv_typec_request request;
	uint32_t serial;
	uint32_t kept_serial;
	int kept_error;
	unsigned resets;
	char detail[320];
	bool ran;
	int error;

	/* The PPM stops; a change comes. */
	(void)drv_typec_connector_get(0, &record);
	kept_serial = record.request_serial;
	kept_error = record.request_error;
	resets = fake.resets;
	fake.stalled = true;
	fake.connectors[1].connected = true;
	fake.connectors[1].power_operation = 1;
	fake.connectors[1].partner_type = 2;
	fake.connectors[1].change = (uint16_t)(1U << 14);
	fake_event(2);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(1, &record);
	(void)snprintf(detail, sizeof(detail), "error %d resets %u connected %d stuck %d", error, fake.resets - resets, record.connected, ucsi->stuck);
	test_check("recover-reset", error == 0 && fake.resets == resets + 1U && record.connected && !ucsi->stuck && ucsi->recoveries == 1U && !ucsi->failed, detail);
	(void)drv_typec_connector_get(0, &record);
	test_check("recover-outcome-kept", kept_serial != 0U && record.request_serial == kept_serial && record.request_error == kept_error, "the last operation's outcome lost");
	(void)snprintf(detail, sizeof(detail), "%u breaks, the last: %s", fake.violations, fake.last_violation);
	test_check("recover-rules", fake.violations == 0U, detail);

	/* The PPM goes for good: the recovery resets it three times, then the interface failed. */
	resets = fake.resets;
	fake.stalled = true;
	fake.dead = true;
	fake.connectors[1].connected = false;
	fake.connectors[1].change = (uint16_t)(1U << 14);
	fake_event(2);
	error = drv_ucsi_service(ucsi);
	(void)snprintf(detail, sizeof(detail), "error %d recoveries %u failed %d resets %u", error, ucsi->recoveries, ucsi->failed, fake.resets - resets);
	test_check("dead-failed", error == ENODEV && ucsi->failed && ucsi->recoveries == 3U && fake.resets == resets + 3U, detail);
	error = drv_ucsi_poll(ucsi);
	test_check("dead-poll", error == ENODEV, "a poll of a failed interface");

	/* A failed interface asks the PPM nothing; an operation ends with ENODEV in its record. */
	resets = fake.commands;
	error = drv_ucsi_service(ucsi);
	test_check("dead-quiet", error == ENODEV && fake.commands == resets, "the failed interface asked the PPM");
	drv_typec_operator_set(test_kick, NULL);
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_DFP, &serial);
	ran = drv_typec_request_take(&request);
	if (ran)
		error = drv_ucsi_request(ucsi, &request);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "ran %d error %d outcome %d", ran, error, record.request_error);
	test_check("dead-request", ran && error == ENODEV && record.request_serial == serial && record.request_error == ENODEV, detail);
	drv_typec_operator_set(NULL, NULL);
	fake.dead = false;
	fake.stalled = false;
}

/*
 * ws177-p003: a platform whose notifications never come.  The start
 * finds every completion by asking, the core says the PPM does not
 * notify, and a change is found by polling.
 */
static void
test_mute(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	char detail[320];
	int error;

	/* The start without a notification. */
	fake_three_connectors(0x0300U);
	fake.mute = true;
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	(void)snprintf(detail, sizeof(detail), "error %d notified %u polled %u", error, ucsi->notified_completions, ucsi->polled_completions);
	test_check("mute-start", error == 0 && !drv_ucsi_notifying(ucsi) && ucsi->polled_completions != 0U, detail);

	/* A change no notification tells of, found by the poll. */
	fake.connectors[2].connected = true;
	fake.connectors[2].power_operation = 1;
	fake.connectors[2].partner_type = 2;
	fake.connectors[2].change = (uint16_t)(1U << 14);
	fake_event(3);
	error = drv_ucsi_poll(ucsi);
	(void)drv_typec_connector_get(2, &record);
	(void)snprintf(detail, sizeof(detail), "error %d connected %d indicated %u breaks %u (%s)", error, record.connected, fake.change_indicated, fake.violations, fake.last_violation);
	test_check("mute-poll", error == 0 && record.connected && fake.change_indicated == 0U && fake.violations == 0U, detail);

	/* A PPM that notifies says so. */
	fake_three_connectors(0x0300U);
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	test_check("notifying", error == 0 && drv_ucsi_notifying(ucsi), "the notifications were not seen");
}

/*
 * ws177-p003: the record of the mailbox.  A start and a change on the
 * fake PPM are written to the log; the same start and change replayed
 * from those lines alone send the same CONTROLs and publish the same
 * records.
 */
static void
test_record_replay(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector first[3];
	struct drv_typec_connector second;
	struct drv_ucsi_transport replay;
	unsigned index;
	unsigned same;
	char detail[320];
	int error;

	/* The run recorded: a start, and a change on the second connector. */
	fake_three_connectors(0x0300U);
	record_count = 0U;
	capturing = true;
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	fake.connectors[1].connected = true;
	fake.connectors[1].power_operation = 5;
	fake.connectors[1].provider = true;
	fake.connectors[1].partner_type = 2;
	fake.connectors[1].change = (uint16_t)(1U << 14);
	fake_event(2);
	if (error == 0)
		error = drv_ucsi_service(ucsi);
	capturing = false;
	(void)snprintf(detail, sizeof(detail), "error %d, %u lines", error, record_count);
	test_check("record-run", error == 0 && record_count > 20U && ucsi->recorded <= 256U, detail);
	for (index = 0U; index < 3U; index++)
		(void)drv_typec_connector_get(index, &first[index]);

	/* The same steps over the record alone. */
	fake_three_connectors(0x0300U);
	fake.connector_count = 0U;
	replay.write = replay_write;
	replay.read = replay_read;
	replay.wait = replay_wait;
	replay.context = NULL;
	replay_next = 0U;
	replay_number = 0U;
	replay_mismatches = 0U;
	replay_overruns = 0U;
	error = drv_ucsi_start(ucsi, &replay, &drv_ucsi_layout_2, 0x0300U);
	if (error == 0)
		error = drv_ucsi_service(ucsi);
	(void)snprintf(detail, sizeof(detail), "error %d, mismatches %u, overruns %u, at line %u of %u", error, replay_mismatches, replay_overruns, replay_next, record_count);
	test_check("replay-run", error == 0 && replay_mismatches == 0U && replay_overruns == 0U && replay_next == record_count, detail);

	/* The records as the recorded run left them. */
	same = 0U;
	for (index = 0U; index < 3U; index++) {
		(void)drv_typec_connector_get(index, &second);
		if (second.connected == first[index].connected &&
		    second.capability == first[index].capability &&
		    second.power_operation == first[index].power_operation &&
		    second.power_role == first[index].power_role &&
		    second.partner_type == first[index].partner_type)
			same++;
	}

	/* Every record as the run left it. */
	test_check("replay-records", same == 3U, "a record differs from the recorded run's");
}

/*
 * Finds the next line of the record (from replay_next) if it is of a kind
 * (W, R or I), and where its fields start; record_count when it is not.
 * Lines must be numbered without a gap (a line lost from the log), counted
 * in replay_mismatches.
 */
static unsigned
replay_find(
	char kind,
	const char **fields)
{
	unsigned number;
	int consumed;
	int scanned;
	char found;

	/* The line at the cursor: its number and its kind. */
	if (replay_next >= record_count)
		return record_count;
	consumed = 0;
	scanned = sscanf(record_lines[replay_next], "ucsi: rec %u %c %n", &number, &found, &consumed);
	if (scanned != 2 || found != kind)
		return record_count;

	/* W and R lines number the exchanges one after another; I lines share their R's. */
	if (kind != 'I') {
		if (number != replay_number + 1U)
			replay_mismatches++;
		replay_number = number;
	}

	/* Succeeded: the line and its fields. */
	*fields = &record_lines[replay_next][consumed];
	return replay_next;
}

/* The replay's write: the CONTROL must be the record's next W line. */
static int
replay_write(
	void *context,
	uint64_t control,
	const uint8_t *message_out,
	size_t length)
{
	unsigned long long recorded;
	const char *fields;
	unsigned line;

	(void)context;
	(void)message_out;
	(void)length;

	/* The next line, a W, with the same CONTROL. */
	line = replay_find('W', &fields);
	if (line == record_count) {
		replay_overruns++;
		return 0;
	}

	/* Compared, and taken. */
	recorded = strtoull(fields, NULL, 16);
	if (recorded != (unsigned long long)control)
		replay_mismatches++;
	replay_next++;
	return 0;
}

/* The replay's read: CCI and MESSAGE IN from the record's next R line and its I lines. */
static int
replay_read(
	void *context,
	bool refresh,
	uint32_t *cci,
	uint8_t *message_in,
	size_t size)
{
	unsigned value;
	unsigned recorded_refresh;
	unsigned length;
	unsigned offset;
	unsigned line;
	unsigned at;
	const char *fields;
	const char *hex;

	(void)context;

	/* The next line, an R. */
	memset(message_in, 0, size);
	*cci = 0U;
	line = replay_find('R', &fields);
	if (line == record_count) {
		replay_overruns++;
		return 0;
	}

	/* CCI as recorded, read the same way (fetched or after a notification). */
	(void)sscanf(fields, "%x %u %u", &value, &recorded_refresh, &length);
	*cci = value;
	if ((recorded_refresh != 0U) != refresh)
		replay_mismatches++;
	replay_next++;

	/* Its MESSAGE IN, an I line a chunk. */
	for (;;) {
		line = replay_find('I', &fields);
		if (line == record_count)
			break;
		offset = (unsigned)strtoul(fields, NULL, 10);
		hex = strchr(fields, ' ');
		at = offset;
		while (hex != NULL && hex[1] != '\0' && hex[1] != '\n' && hex[2] != '\0' && at < size) {
			(void)sscanf(&hex[1], "%2x", &value);
			message_in[at] = (uint8_t)value;
			at++;
			hex += 2;
		}

		/* The chunk taken. */
		replay_next++;
	}

	/* Succeeded: the recorded answer. */
	return 0;
}

/* The replay's wait: a notification exactly when the recorded read was not a fetch. */
static int
replay_wait(
	void *context,
	uint32_t milliseconds)
{
	unsigned value;
	unsigned recorded_refresh;
	unsigned length;
	unsigned number;
	const char *fields;
	int consumed;
	int scanned;
	char kind;

	(void)context;
	(void)milliseconds;

	/* The read that follows, as recorded (looked at, not taken). */
	if (replay_next >= record_count)
		return 0;
	consumed = 0;
	scanned = sscanf(record_lines[replay_next], "ucsi: rec %u %c %n", &number, &kind, &consumed);
	if (scanned != 2 || kind != 'R')
		return 0;
	fields = &record_lines[replay_next][consumed];
	(void)sscanf(fields, "%x %u %u", &value, &recorded_refresh, &length);
	if (recorded_refresh != 0U)
		return 0;

	/* Succeeded: it followed a notification. */
	return 1;
}

/*
 * ws177-p003: the layer's side.  An operation canceled while it waits
 * gets ECANCELED, which the connector driver's next publishing keeps; a
 * change whose reading failed (the PPM answering) is acknowledged all
 * the same; the connector driver's stop ends what waits with ENODEV,
 * refuses new operations and publishing, and a new start clears it.
 */
static void
test_layer_stop(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport)
{
	struct drv_typec_connector record;
	struct drv_typec_request request;
	uint32_t first;
	uint32_t second;
	unsigned ended;
	char text[2048];
	char detail[320];
	bool taken;
	int error;

	/* A started PPM; two operations wait. */
	fake_three_connectors(0x0300U);
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	test_check("layer-start", error == 0, "the PPM did not start");
	drv_typec_operator_set(test_kick, NULL);
	(void)drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_UFP, &first);
	(void)drv_typec_connector_set_power_role(0, DRV_TYPEC_ROLE_SOURCE, &second);

	/* The first canceled: ECANCELED published; again it no longer waits; an unknown serial is not one. */
	listened = 0;
	error = drv_typec_request_cancel(first);
	(void)drv_typec_connector_get(0, &record);
	(void)snprintf(detail, sizeof(detail), "error %d, told %u, serial %u/%u, outcome %d", error, listened, (unsigned)record.request_serial, (unsigned)first, record.request_error);
	test_check("cancel-waiting", error == 0 && listened == 1U && record.request_serial == first && record.request_error == ECANCELED, detail);
	error = drv_typec_request_cancel(first);
	test_check("cancel-again", error == EBUSY, "a canceled operation canceled again");
	error = drv_typec_request_cancel(second + 100U);
	test_check("cancel-unknown", error == ENOENT, "a serial never asked");

	/* The connector driver's publishing keeps the outcome. */
	fake.connectors[0].change = (uint16_t)(1U << 14);
	fake_event(1);
	error = drv_ucsi_service(ucsi);
	(void)drv_typec_connector_get(0, &record);
	test_check("cancel-kept", error == 0 && record.request_serial == first && record.request_error == ECANCELED, "the publishing overwrote the outcome");

	/* Only the second is taken; once taken it is not canceled. */
	taken = drv_typec_request_take(&request);
	test_check("cancel-left", taken && request.serial == second, "the canceled operation was taken");
	error = drv_typec_request_cancel(second);
	test_check("cancel-taken", error == EBUSY, "a taken operation canceled");
	error = drv_ucsi_request(ucsi, &request);
	test_check("cancel-run", error == 0, "the second did not run");

	/* A change whose reading the PPM fails is acknowledged all the same. */
	fake.fail_command = FAKE_GET_CONNECTOR_STATUS;
	fake.error_information = 1U << 4;
	fake.connectors[1].change = (uint16_t)(1U << 14);
	fake_event(2);
	error = drv_ucsi_service(ucsi);
	(void)snprintf(detail, sizeof(detail), "error %d, indicated %u, stuck %d, %u breaks (%s)", error, fake.change_indicated, ucsi->stuck, fake.violations, fake.last_violation);
	test_check("change-failed-acknowledged", error == 0 && fake.change_indicated == 0U && !ucsi->stuck && fake.violations == 0U, detail);
	fake.fail_command = 0U;

	/* Two operations wait, then the connector driver stops. */
	(void)drv_typec_connector_set_data_role(1, DRV_TYPEC_DATA_UFP, &first);
	(void)drv_typec_connector_set_data_role(2, DRV_TYPEC_DATA_UFP, &second);
	listened = 0;
	ended = drv_typec_driver_stop(ENODEV);
	(void)drv_typec_connector_get(1, &record);
	(void)snprintf(detail, sizeof(detail), "ended %u, told %u, outcome %d", ended, listened, record.request_error);
	test_check("stop-ended", ended == 2U && listened == 3U && record.request_serial == first && record.request_error == ENODEV && drv_typec_driver_stopped(), detail);
	(void)drv_typec_connector_get(2, &record);
	test_check("stop-ended-second", record.request_serial == second && record.request_error == ENODEV, "the second's outcome");
	taken = drv_typec_request_take(&request);
	error = drv_typec_connector_set_data_role(0, DRV_TYPEC_DATA_DFP, &first);
	test_check("stop-refused", !taken && error == ENODEV, "an operation after the stop");
	error = drv_typec_connector_publish(0, &record);
	test_check("stop-no-publish", error == ENODEV, "a record published after the stop");
	(void)drv_typec_text(text, sizeof(text));
	test_check("stop-text", strstr(text, "the connector driver stopped") != NULL, text);

	/* A new start clears the stop. */
	fake_three_connectors(0x0300U);
	error = drv_ucsi_start(ucsi, transport, &drv_ucsi_layout_2, 0x0300U);
	test_check("stop-restart", error == 0 && !drv_typec_driver_stopped(), "the stop outlived a new start");
	drv_typec_operator_set(NULL, NULL);
}
