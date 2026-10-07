/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UCSI core (ws050-p002; ucsi.h): runs one command at a time over a
 * transport, acknowledges each completion and each connector change as
 * the specification asks, and turns the PPM's answers into the Type-C
 * layer's connector records.
 *
 * Section and table numbers are those of UCSI 1.2 unless 3.1 is named
 * (the documents are named in ucsi.h).
 *
 * A command the PPM reports as failed is acknowledged and its reason read
 * with GET_ERROR_STATUS; one still busy when its time is up is canceled;
 * a PPM that stops answering is reset and every connector read again, and
 * one that does not come back after UCSI_RECOVER_MAX resets leaves the
 * interface failed (ws177-p003).  The first UCSI_RECORD_MAX mailbox
 * exchanges are written to the log in a form a host test replays.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include "typec-os.h"
#include "ucsi.h"

/*
 * The command codes (Table A-1; the same in 3.1 Table A-1).
 */
#define UCSI_PPM_RESET 0x01U
#define UCSI_CANCEL 0x02U
#define UCSI_CONNECTOR_RESET 0x03U
#define UCSI_SET_UOR 0x09U
#define UCSI_SET_PDR 0x0BU
#define UCSI_SET_NEW_CAM 0x0FU
#define UCSI_GET_CABLE_PROPERTY 0x11U
#define UCSI_ACK_CC_CI 0x04U
#define UCSI_SET_NOTIFICATION_ENABLE 0x05U
#define UCSI_GET_CAPABILITY 0x06U
#define UCSI_GET_CONNECTOR_CAPABILITY 0x07U
#define UCSI_GET_ALTERNATE_MODES 0x0CU
#define UCSI_GET_CAM_SUPPORTED 0x0DU
#define UCSI_GET_CURRENT_CAM 0x0EU
#define UCSI_GET_PDOS 0x10U
#define UCSI_GET_CONNECTOR_STATUS 0x12U
#define UCSI_GET_CAM_CS 0x18U
#define UCSI_GET_ERROR_STATUS 0x13U

/*
 * The fields of CCI (Table 3-2): the connector a change occurred on (bits
 * 1-7), the length of MESSAGE IN (bits 8-15), and the indicators.
 */
#define UCSI_CCI_CONNECTOR_SHIFT 1U
#define UCSI_CCI_CONNECTOR_MASK 0x7FU
#define UCSI_CCI_LENGTH_SHIFT 8U
#define UCSI_CCI_LENGTH_MASK 0xFFU
#define UCSI_CCI_NOT_SUPPORTED (1UL << 25)
#define UCSI_CCI_CANCEL_COMPLETED (1UL << 26)
#define UCSI_CCI_RESET_COMPLETED (1UL << 27)
#define UCSI_CCI_BUSY (1UL << 28)
#define UCSI_CCI_ACKNOWLEDGED (1UL << 29)
#define UCSI_CCI_ERROR (1UL << 30)
#define UCSI_CCI_COMPLETED (1UL << 31)

/*
 * Where the command-specific fields of CONTROL start (Table 3-3), and the
 * two acknowledgements of ACK_CC_CI (Table 4-7).
 */
#define UCSI_CONTROL_SPECIFIC_SHIFT 16U
#define UCSI_ACK_CONNECTOR_CHANGE (1ULL << 16)
#define UCSI_ACK_COMMAND_COMPLETED (1ULL << 17)

/*
 * The notifications of SET_NOTIFICATION_ENABLE (Table 4-9, bits of the
 * field at bit 16 of CONTROL).
 */
#define UCSI_NOTIFY_COMMAND_COMPLETED (1U << 0)
#define UCSI_NOTIFY_EXTERNAL_SUPPLY (1U << 1)
#define UCSI_NOTIFY_POWER_OPERATION (1U << 2)
#define UCSI_NOTIFY_PROVIDER_CAPABILITIES (1U << 5)
#define UCSI_NOTIFY_POWER_LEVEL (1U << 6)
#define UCSI_NOTIFY_PD_RESET (1U << 7)
#define UCSI_NOTIFY_SUPPORTED_CAM (1U << 8)
#define UCSI_NOTIFY_BATTERY_CHARGING (1U << 9)
#define UCSI_NOTIFY_PARTNER (1U << 11)
#define UCSI_NOTIFY_POWER_DIRECTION (1U << 12)
#define UCSI_NOTIFY_CONNECT (1U << 14)
#define UCSI_NOTIFY_ERROR (1U << 15)

/*
 * The optional features of GET_CAPABILITY's bmOptionalFeatures (Table
 * 4-54).
 */
#define UCSI_FEATURE_ALT_MODE_DETAILS (1U << 2)
#define UCSI_FEATURE_ALT_MODE_OVERRIDE (1U << 3)
#define UCSI_FEATURE_CABLE_DETAILS (1U << 5)
#define UCSI_FEATURE_PDO_DETAILS (1U << 4)
#define UCSI_FEATURE_EXTERNAL_SUPPLY (1U << 6)
#define UCSI_FEATURE_PD_RESET (1U << 7)

/*
 * The recipients of GET_ALTERNATE_MODES (Table 4-24).
 */
#define UCSI_RECIPIENT_CONNECTOR 0U
#define UCSI_RECIPIENT_SOP 1U
#define UCSI_RECIPIENT_SOP_PRIME 2U

/*
 * How many Alternate Modes one GET_ALTERNATE_MODES returns at most (its
 * 2-bit count is the number less one, at most 1: Table 4-24, and the same
 * in 3.1), and the bytes of one (a 16-bit SVID and a 32-bit MID, Table
 * 4-26).
 */
#define UCSI_ALT_MODES_PER_COMMAND 2U
#define UCSI_ALT_MODE_BYTES 6U

/*
 * How many PDOs one GET_PDOS returns at most, and the largest offset plus
 * count field it accepts (Table 4-34 and the text under Table 4-36).
 */
#define UCSI_PDOS_PER_COMMAND 4U
#define UCSI_PDO_LAST_INDEX 7U

/*
 * The fields of the operations' CONTROL (3.1 Table 6-5, 6-20, 6-22 and
 * 6-33; the same bits in 1.2): CONNECTOR_RESET's reset type at bit 23,
 * SET_UOR's and SET_PDR's role (bit 23 swap to DFP or Source, 24 to UFP
 * or Sink, 25 accept the partner's swaps), SET_NEW_CAM's EnterOrExit at
 * 23, its New CAM at 24 and its AMSpecific at 32.
 *
 * CONNECTOR_RESET's bit 23 changed meaning: from 2.0 it is 1 for a Data
 * Reset and 0 for a Hard Reset (3.1 Table 6-5); in 1.0 it was 1 for a Hard
 * Reset, and from 1.1 it is not used (inferred from the Linux driver's
 * definitions; unconfirmed in the 1.x documents).
 */
#define UCSI_RESET_TYPE_BIT (1ULL << 23)
#define UCSI_ROLE_FIRST_BIT (1ULL << 23)
#define UCSI_ROLE_SECOND_BIT (1ULL << 24)
#define UCSI_ROLE_ACCEPT_BIT (1ULL << 25)
#define UCSI_CAM_ENTER_BIT (1ULL << 23)
#define UCSI_CAM_SHIFT 24U
#define UCSI_CAM_SPECIFIC_SHIFT 32U
#define UCSI_VERSION_1_0 0x0100U

/*
 * GET_CURRENT_CAM's value for "in no Alternate Mode" (Table 4-31).
 */
#define UCSI_NO_CURRENT_MODE 0xFFU

/*
 * The first UCSI version whose GET_CONNECTOR_STATUS has the orientation
 * (3.1 Table 6-43 bit 86; that 2.0 has it is unconfirmed), and the bytes
 * the status needs to hold it.
 */
#define UCSI_VERSION_2 0x0200U
#define UCSI_STATUS_ORIENTATION_BIT 86U
#define UCSI_STATUS_ORIENTATION_BYTES 11U

/*
 * The first UCSI version whose GET_CAM_CS this driver asks (3.1 section
 * 6.5.22; which revision brought the command is not in the 3.1 document's
 * history, so the 2.x PPMs, whose documents were not at hand, are not
 * asked).  Its data (Table 6-60): the Status of the current mode at bit
 * 8, the number of VDOs at bit 40 and the first VDO at bit 48.
 */
#define UCSI_VERSION_3 0x0300U
#define UCSI_CAM_CS_STATUS_BIT 8U
#define UCSI_CAM_CS_COUNT_BIT 40U
#define UCSI_CAM_CS_VDO_BIT 48U

/*
 * The DisplayPort Status VDO's hot plug detect (bit 7) and the
 * Configuration VDO's pin assignment (bits 15:8, one bit per pin from A at
 * bit 8) (VESA DisplayPort Alt Mode on USB Type-C, Tables 5-3 and 5-4).
 */
#define UCSI_DP_STATUS_HPD (1U << 7)
#define UCSI_DP_CONFIG_PIN_SHIFT 8U
#define UCSI_DP_CONFIG_PIN_MASK 0x3FU

/*
 * How long a command may take, how long PPM_RESET may take, and the
 * first and the longest step of the wait between two looks at CCI when
 * no notification comes.
 */
#define UCSI_COMMAND_TIMEOUT_MS 5000U
#define UCSI_RESET_TIMEOUT_MS 1000U
#define UCSI_STEP_FIRST_MS 20U
#define UCSI_STEP_LONGEST_MS 100U

/*
 * The bits of GET_ERROR_STATUS's Error Information (3.1 Table 6-48; bits
 * 0 to 12 are the same in 1.2), which the errno value of a failed command
 * is chosen by.
 */
#define UCSI_ERROR_UNRECOGNIZED (1U << 0)
#define UCSI_ERROR_NO_CONNECTOR (1U << 1)
#define UCSI_ERROR_INVALID_PARAMETERS (1U << 2)
#define UCSI_ERROR_INCOMPATIBLE_PARTNER (1U << 3)
#define UCSI_ERROR_CC_COMMUNICATION (1U << 4)
#define UCSI_ERROR_DEAD_BATTERY (1U << 5)
#define UCSI_ERROR_CONTRACT_NEGOTIATION (1U << 6)
#define UCSI_ERROR_PARTNER_REJECTED_SWAP (1U << 9)
#define UCSI_ERROR_POLICY_CONFLICT (1U << 11)
#define UCSI_ERROR_SWAP_REJECTED (1U << 12)
#define UCSI_ERROR_BITS 15U

/*
 * How many resets in a row may fail to bring a PPM that stopped answering
 * back before the interface is left failed, and the pause before the
 * second (doubled before each later one).
 */
#define UCSI_RECOVER_MAX 3U
#define UCSI_RECOVER_PAUSE_MS 500U

/*
 * The record of the mailbox (ws177-p003): how many exchanges are written
 * to the log from the start, and how many MESSAGE IN bytes one line holds
 * (the kernel's log lines are at most 256 bytes).
 */
#define UCSI_RECORD_MAX 256U
#define UCSI_RECORD_CHUNK 64U

static int ucsi_enumerate(struct drv_ucsi *ucsi, bool again);
static int ucsi_settle(struct drv_ucsi *ucsi, int error);
static int ucsi_recover(struct drv_ucsi *ucsi);
static int ucsi_execute(struct drv_ucsi *ucsi, uint64_t control, uint8_t *message_in, size_t *length);
static int ucsi_command(struct drv_ucsi *ucsi, uint64_t control);
static int ucsi_cancel(struct drv_ucsi *ucsi, uint64_t control);
static int ucsi_error_status(struct drv_ucsi *ucsi, uint64_t control);
static int ucsi_error_errno(uint32_t information);
static void ucsi_error_log(uint64_t control, unsigned number, uint32_t information);
static unsigned ucsi_control_connector(uint64_t control);
static void ucsi_record_write(struct drv_ucsi *ucsi, uint64_t control);
static void ucsi_record_read(struct drv_ucsi *ucsi, uint32_t cci, bool refresh, const uint8_t *message_in, size_t size);
static int ucsi_acknowledge(struct drv_ucsi *ucsi, uint64_t which);
static int ucsi_acknowledge_change(struct drv_ucsi *ucsi);
static int ucsi_reset(struct drv_ucsi *ucsi);
static int ucsi_notifications(struct drv_ucsi *ucsi, uint16_t notifications);
static int ucsi_capability(struct drv_ucsi *ucsi);
static int ucsi_connector_start(struct drv_ucsi *ucsi, unsigned number);
static int ucsi_connector_update(struct drv_ucsi *ucsi, unsigned number);
static int ucsi_status(struct drv_ucsi *ucsi, unsigned number, struct drv_typec_connector *record);
static void ucsi_partner_clear(struct drv_typec_connector *record);
static int ucsi_alt_modes(struct drv_ucsi *ucsi, unsigned number, unsigned recipient, struct drv_typec_alt_mode_list *list);
static int ucsi_current_modes(struct drv_ucsi *ucsi, unsigned number, struct drv_typec_connector *record);
static int ucsi_partner_pdos(struct drv_ucsi *ucsi, unsigned number, struct drv_typec_connector *record);
static int ucsi_dp_status(struct drv_ucsi *ucsi, unsigned number, struct drv_typec_connector *record);
static int ucsi_pending_handle(struct drv_ucsi *ucsi);
static int ucsi_request_control(const struct drv_ucsi *ucsi, const struct drv_typec_request *request, uint64_t *control);
static int ucsi_cable(struct drv_ucsi *ucsi, unsigned number, struct drv_typec_connector *record);
static void ucsi_latch(struct drv_ucsi *ucsi, uint32_t cci);
static uint64_t ucsi_connector_control(unsigned command, unsigned number);
static uint32_t ucsi_get16(const uint8_t *data);
static uint32_t ucsi_get32(const uint8_t *data);
static uint32_t ucsi_bits(const uint8_t *data, size_t length, unsigned offset, unsigned width);

/*
 * The arrangement of UCSI 1.x (1.2 Table 3-1): VERSION, CCI and CONTROL,
 * then 16 bytes of MESSAGE IN at 16 and of MESSAGE OUT at 32.
 */
const struct drv_ucsi_layout drv_ucsi_layout_1 = {
	.name = "1.x",
	.size = 0x30U,
	.version_offset = 0U,
	.cci_offset = 4U,
	.control_offset = 8U,
	.message_in_offset = 16U,
	.message_out_offset = 32U,
	.message_in_size = 16U,
	.message_out_size = 16U,
};

/*
 * The arrangement of UCSI 2.x and later (3.1 Table 4-1, unconfirmed for
 * 2.0): 255 bytes of MESSAGE IN at 16 and of MESSAGE OUT at 272.
 */
const struct drv_ucsi_layout drv_ucsi_layout_2 = {
	.name = "2.x",
	.size = 0x210U,
	.version_offset = 0U,
	.cci_offset = 4U,
	.control_offset = 8U,
	.message_in_offset = 16U,
	.message_out_offset = 272U,
	.message_in_size = 255U,
	.message_out_size = 255U,
};

/*
 * Picks the mailbox arrangement that a region of a size holds: the 2.x one
 * when the region is large enough for it, else the 1.x one.  Returns NULL
 * for a region too small for either.
 */
const struct drv_ucsi_layout *
drv_ucsi_layout_select(
	size_t region_size)
{
	/* A region too small for either. */
	if (region_size < drv_ucsi_layout_1.size)
		return NULL;

	/* A region that holds the large messages. */
	if (region_size >= drv_ucsi_layout_2.size)
		return &drv_ucsi_layout_2;

	/* Succeeded: a region that holds the 1.x mailbox (a PC's is often a little larger). */
	return &drv_ucsi_layout_1;
}

/*
 * Starts a UCSI interface: resets the PPM, enables the completion
 * notification, reads the capability and every connector, then enables
 * the change notifications the PPM supports and reads every connector
 * again, so a change made before the notifications were on is not lost.
 *
 * Returns 0, or an errno value (the interface is not usable).
 */
int
drv_ucsi_start(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport,
	const struct drv_ucsi_layout *layout,
	uint16_t version)
{
	int error;

	/* Begins with nothing known. */
	kern_memset(ucsi, 0, sizeof(*ucsi));
	ucsi->transport = transport;
	ucsi->layout = layout;
	ucsi->version = version;
	drv_typec_os_log("ucsi: version %x.%x.%x, %s mailbox\n", version >> 8, (version >> 4) & 0xFU, version & 0xFU, layout->name);

	/* Resets the PPM and reads every connector. */
	error = ucsi_enumerate(ucsi, false);
	if (error != 0)
		return error;

	/* Succeeded: the records are current and changes will be notified. */
	return 0;
}

/*
 * Handles a notification that came while no command was running: reads
 * CCI as the notification left it and each connector whose change it
 * indicates, acknowledging each change.  A PPM that stopped answering is
 * reset and read again.
 *
 * Returns 0, ENODEV for an interface that failed, or an errno value.
 */
int
drv_ucsi_service(
	struct drv_ucsi *ucsi)
{
	uint32_t cci;
	int status;
	int error;

	/* An interface that failed answers nothing. */
	if (ucsi->failed)
		return ENODEV;

	/* Reads CCI as the notification left it, keeping the change it indicates. */
	status = ucsi->transport->read(ucsi->transport->context, false, &cci, ucsi->message_in, ucsi->layout->message_in_size);
	if (status < 0) {
		ucsi->stuck = true;
		error = ucsi_settle(ucsi, EIO);
		return error;
	}

	/* Recorded, and the change it indicates kept. */
	ucsi_record_read(ucsi, cci, false, ucsi->message_in, ucsi->layout->message_in_size);
	ucsi_latch(ucsi, cci);

	/* Reads and acknowledges each connector that changed; a PPM that stopped is brought back. */
	error = ucsi_pending_handle(ucsi);
	error = ucsi_settle(ucsi, error);
	if (error != 0)
		return error;

	/* Succeeded: the records of the changed connectors are current. */
	return 0;
}

/*
 * Asks the PPM for CCI although no notification came (a notification
 * that was lost, or a PPM that never notifies) and handles the connector
 * change it indicates.  A PPM that stopped answering is reset and read
 * again.
 *
 * Returns 0, ENODEV for an interface that failed, or an errno value.
 */
int
drv_ucsi_poll(
	struct drv_ucsi *ucsi)
{
	uint32_t cci;
	int status;
	int error;

	/* An interface that failed answers nothing. */
	if (ucsi->failed)
		return ENODEV;

	/* A PPM that stopped answering is brought back first. */
	if (ucsi->stuck) {
		error = ucsi_settle(ucsi, ETIMEDOUT);
		return error;
	}

	/* Fetches CCI from the PPM, keeping the change it indicates. */
	status = ucsi->transport->read(ucsi->transport->context, true, &cci, ucsi->message_in, ucsi->layout->message_in_size);
	if (status < 0) {
		ucsi->stuck = true;
		error = ucsi_settle(ucsi, EIO);
		return error;
	}

	/* Recorded, and the change it indicates kept. */
	ucsi_record_read(ucsi, cci, true, ucsi->message_in, ucsi->layout->message_in_size);
	ucsi_latch(ucsi, cci);

	/* Reads and acknowledges each connector that changed. */
	error = ucsi_pending_handle(ucsi);
	error = ucsi_settle(ucsi, error);
	if (error != 0)
		return error;

	/* Succeeded: no change waits unseen. */
	return 0;
}

/*
 * Tells whether the PPM's notifications come: true once a command's
 * completion arrived with a notification since the start.  A PPM whose
 * completions are all found by asking does not notify, and its changes are
 * found by polling.
 */
bool
drv_ucsi_notifying(
	const struct drv_ucsi *ucsi)
{
	/* No completion came with a notification. */
	if (ucsi->notified_completions == 0U)
		return false;

	/* Succeeded: the notifications come. */
	return true;
}

/*
 * Resets the PPM and reads it whole: the completion notification, the
 * capability, every connector, then the change notifications the PPM
 * supports and every connector again, so a change made before the
 * notifications were on is not lost.  again says it reads a PPM that was
 * reset to bring it back: the records keep their operations' outcomes,
 * and they are emptied only when the number of connectors changed.
 *
 * Returns 0, or an errno value.
 */
static int
ucsi_enumerate(
	struct drv_ucsi *ucsi,
	bool again)
{
	uint16_t notifications;
	unsigned previous_count;
	unsigned number;
	int error;

	/* Resets the PPM, which leaves every notification off (section 4.5.1). */
	error = ucsi_reset(ucsi);
	if (error != 0)
		return error;

	/* Turns on the completion and error notifications first (section 4.3). */
	error = ucsi_notifications(ucsi, UCSI_NOTIFY_COMMAND_COMPLETED | UCSI_NOTIFY_ERROR);
	if (error != 0)
		return error;

	/* Reads how many connectors there are and what the PPM supports. */
	previous_count = ucsi->connector_count;
	error = ucsi_capability(ucsi);
	if (error != 0)
		return error;

	/* A first reading, or a PPM whose connectors changed, starts from empty records. */
	if (!again) {
		drv_typec_connectors_reset(ucsi->connector_count);
	} else if (previous_count != ucsi->connector_count) {
		drv_typec_connectors_reset(ucsi->connector_count);
	}

	/* Reads what each connector can do and its state now. */
	for (number = 1; number <= ucsi->connector_count; number++) {
		error = ucsi_connector_start(ucsi, number);
		if (error != 0)
			return error;
	}

	/*
	 * Turns on the change notifications: the ones every PPM supports, and
	 * the optional ones of the features it reported.
	 */
	notifications = UCSI_NOTIFY_COMMAND_COMPLETED | UCSI_NOTIFY_ERROR | UCSI_NOTIFY_CONNECT | UCSI_NOTIFY_PARTNER | UCSI_NOTIFY_POWER_DIRECTION | UCSI_NOTIFY_POWER_OPERATION | UCSI_NOTIFY_BATTERY_CHARGING;
	if ((ucsi->optional_features & UCSI_FEATURE_ALT_MODE_DETAILS) != 0)
		notifications |= UCSI_NOTIFY_SUPPORTED_CAM;
	if ((ucsi->optional_features & UCSI_FEATURE_PDO_DETAILS) != 0)
		notifications |= UCSI_NOTIFY_PROVIDER_CAPABILITIES | UCSI_NOTIFY_POWER_LEVEL;
	if ((ucsi->optional_features & UCSI_FEATURE_EXTERNAL_SUPPLY) != 0)
		notifications |= UCSI_NOTIFY_EXTERNAL_SUPPLY;
	if ((ucsi->optional_features & UCSI_FEATURE_PD_RESET) != 0)
		notifications |= UCSI_NOTIFY_PD_RESET;
	error = ucsi_notifications(ucsi, notifications);
	if (error != 0)
		return error;

	/* Reads every connector again, catching what changed meanwhile. */
	for (number = 1; number <= ucsi->connector_count; number++) {
		error = ucsi_connector_update(ucsi, number);
		if (error != 0)
			return error;
	}

	/* Acknowledges the changes the PPM indicated meanwhile. */
	error = ucsi_pending_handle(ucsi);
	if (error != 0)
		return error;

	/* Succeeded: the records are current and changes will be notified. */
	return 0;
}

/*
 * Carries out an operation another driver asked of a connector
 * (SET_UOR, SET_PDR, CONNECTOR_RESET or SET_NEW_CAM), notes its outcome
 * (its errno value and, for a command the PPM failed, the Error
 * Information) in the connector's record, and reads the connector again,
 * which publishes the record and tells the listeners.  A PPM that stopped
 * answering is reset and read again after the outcome is published.
 *
 * Returns 0 when the connector was read again (the operation's own errno
 * value is in the record), ENODEV for an interface that failed, or the
 * errno value of that read.
 */
int
drv_ucsi_request(
	struct drv_ucsi *ucsi,
	const struct drv_typec_request *request)
{
	struct drv_typec_connector *record;
	uint64_t control;
	uint32_t information;
	unsigned number;
	int request_error;
	int error;
	int got;

	/* The command; one the PPM cannot be asked, or an interface that failed, is the operation's outcome. */
	number = request->connector + 1U;
	ucsi->error_information = 0U;
	request_error = ENODEV;
	if (!ucsi->failed)
		request_error = ucsi_request_control(ucsi, request, &control);
	if (request_error == 0)
		request_error = ucsi_command(ucsi, control);
	information = ucsi->error_information;
	drv_typec_os_log("ucsi: connector %u request %u kind %u error %d information 0x%04x\n", number, (unsigned)request->serial, (unsigned)request->kind, request_error, (unsigned)information);

	/* The outcome in the record. */
	error = drv_typec_request_finish(request, request_error, information);
	if (error != 0)
		return error;

	/* An interface that failed reads nothing more: the outcome is published as the record stands. */
	if (ucsi->failed) {
		record = &ucsi->record;
		got = drv_typec_connector_get(request->connector, record);
		if (got == 0)
			(void)drv_typec_connector_publish(request->connector, record);
		return ENODEV;
	}

	/* The connector read again and published, and the changes the PPM indicated meanwhile. */
	error = ucsi_connector_update(ucsi, number);
	if (error == 0) {
		error = ucsi_pending_handle(ucsi);
		error = ucsi_settle(ucsi, error);
		if (error != 0)
			return error;
		return 0;
	}

	/* A connector that could not be read is published as it was, so the outcome is told. */
	record = &ucsi->record;
	got = drv_typec_connector_get(request->connector, record);
	if (got == 0)
		(void)drv_typec_connector_publish(request->connector, record);

	/* A PPM that stopped answering is brought back now that the outcome is out. */
	error = ucsi_settle(ucsi, error);
	if (error != 0)
		return error;

	/* Succeeded: the PPM was reset and every connector read again. */
	return 0;
}

/*
 * Brings a PPM that stopped answering back after one of the public steps
 * ended with `error`: the PPM is reset and every connector read again.
 * Returns `error` when the PPM answers, 0 when it was brought back (the
 * records are read anew), or the errno value of a recovery that failed
 * (ENODEV once the interface failed).
 */
static int
ucsi_settle(
	struct drv_ucsi *ucsi,
	int error)
{
	int recovered;

	/* A PPM that answers keeps the step's outcome. */
	if (!ucsi->stuck)
		return error;

	/* A recovery under way does not start another. */
	if (ucsi->recovering)
		return error;

	/* Resets the PPM and reads it again. */
	recovered = ucsi_recover(ucsi);
	if (recovered != 0)
		return recovered;

	/* Succeeded: the PPM answers again and the records are current. */
	return 0;
}

/*
 * Resets a PPM that stopped answering and reads every connector again
 * (the start's reading; the records keep their operations' outcomes).
 * Up to UCSI_RECOVER_MAX attempts are made, each after a longer pause; a
 * reading that fails for any reason counts as a failed attempt (it may
 * have stopped before the change notifications were on).  When none
 * succeeds the interface is failed.
 *
 * Returns 0, or ENODEV when the interface failed.
 */
static int
ucsi_recover(
	struct drv_ucsi *ucsi)
{
	const struct drv_ucsi_transport *transport;
	uint32_t pause;
	unsigned attempt;
	int error;

	/*
	 * recovering keeps a failure inside the reading from starting another
	 * recovery; stuck is cleared before each attempt so its commands are
	 * sent at all.
	 */
	transport = ucsi->transport;
	ucsi->recovering = true;
	error = ETIMEDOUT;
	pause = UCSI_RECOVER_PAUSE_MS;
	for (attempt = 1U; attempt <= UCSI_RECOVER_MAX; attempt++) {
		/* A pause before every attempt but the first, longer each time. */
		if (attempt > 1U) {
			(void)transport->wait(transport->context, pause);
			pause *= 2U;
		}

		/* Resets the PPM and reads it again. */
		drv_typec_os_log("ucsi: the PPM stopped answering: reset and read again (attempt %u)\n", attempt);
		ucsi->stuck = false;
		error = ucsi_enumerate(ucsi, true);
		ucsi->recoveries = attempt;
		if (error == 0 && !ucsi->stuck)
			break;
		drv_typec_os_log("ucsi: attempt %u did not bring the PPM back (error %d)\n", attempt, error);
	}

	/* The recovery is over; a later silence may start another. */
	ucsi->recovering = false;

	/* No attempt succeeded: the interface failed. */
	if (error != 0 || ucsi->stuck) {
		ucsi->failed = true;
		drv_typec_os_log("ucsi: the PPM did not come back after %u resets: the interface failed\n", UCSI_RECOVER_MAX);
		return ENODEV;
	}

	/* Succeeded: the PPM answers and every connector was read again. */
	drv_typec_os_log("ucsi: the PPM answers again\n");
	return 0;
}

/*
 * Runs one command: writes CONTROL, waits for the PPM to complete it (or,
 * for ACK_CC_CI, to acknowledge), and keeps MESSAGE IN and the CCI it
 * ended with (ucsi->cci, which says whether the command failed or is not
 * supported).  A completion is not acknowledged here.  Returns 0 once the
 * PPM answered, EIO when the transport fails, or ETIMEDOUT when no answer
 * came in time (ucsi->busy says whether the PPM was busy with it).
 */
static int
ucsi_execute(
	struct drv_ucsi *ucsi,
	uint64_t control,
	uint8_t *message_in,
	size_t *length)
{
	const struct drv_ucsi_transport *transport;
	uint32_t elapsed;
	uint32_t step;
	uint32_t cci;
	uint32_t done;
	size_t size;
	int notified;
	int status;
	bool refresh;

	/* Nothing is known of the answer yet. */
	ucsi->cci = 0U;
	ucsi->busy = false;

	/* Tells the PPM the command. */
	transport = ucsi->transport;
	size = ucsi->layout->message_in_size;
	status = transport->write(transport->context, control, NULL, 0);
	ucsi_record_write(ucsi, control);
	if (status < 0)
		return EIO;

	/*
	 * The indicator that ends the wait: the acknowledgement for ACK_CC_CI
	 * (whose completion is not acknowledged), else the completion.
	 */
	done = UCSI_CCI_COMPLETED;
	if ((control & 0xFFU) == UCSI_ACK_CC_CI)
		done = UCSI_CCI_ACKNOWLEDGED;

	/*
	 * Waits for a notification and reads CCI after it; without one, looks
	 * again after a step that grows to the longest, until the timeout.  A
	 * busy PPM completes the command later (section 4).
	 */
	elapsed = 0;
	step = UCSI_STEP_FIRST_MS;
	for (;;) {
		/* The notification, or the step. */
		notified = transport->wait(transport->context, step);
		if (notified < 0)
			return EIO;

		/* CCI and MESSAGE IN, fetched from the PPM when nothing said they changed. */
		refresh = false;
		if (notified == 0)
			refresh = true;
		status = transport->read(transport->context, refresh, &cci, message_in, size);
		if (status < 0)
			return EIO;
		ucsi_record_read(ucsi, cci, refresh, message_in, size);
		ucsi_latch(ucsi, cci);

		/* What the PPM said: busy, or the answer. */
		ucsi->cci = cci;
		ucsi->busy = false;
		if ((cci & UCSI_CCI_BUSY) != 0)
			ucsi->busy = true;

		/* Done unless the PPM is busy or has not answered yet; counted by how the answer was found. */
		if (!ucsi->busy && (cci & done) != 0) {
			if (notified != 0) {
				ucsi->notified_completions++;
			} else {
				ucsi->polled_completions++;
			}

			/* The answer is here. */
			break;
		}

		/* Gives up after the timeout. */
		elapsed += step;
		if (elapsed >= UCSI_COMMAND_TIMEOUT_MS) {
			drv_typec_os_log("ucsi: command 0x%02x timed out (CCI 0x%08x)\n", (unsigned)(control & 0xFFU), (unsigned)cci);
			return ETIMEDOUT;
		}

		/* The next step is longer, up to the longest. */
		if (!notified && step < UCSI_STEP_LONGEST_MS)
			step *= 2U;
		if (step > UCSI_STEP_LONGEST_MS)
			step = UCSI_STEP_LONGEST_MS;
	}

	/* The length of MESSAGE IN, which cannot be more than the mailbox holds. */
	*length = (cci >> UCSI_CCI_LENGTH_SHIFT) & UCSI_CCI_LENGTH_MASK;
	if (*length > size)
		*length = size;

	/* Succeeded: the PPM answered; ucsi->cci says how. */
	return 0;
}

/*
 * Runs a command and acknowledges its completion (section 4.5.4), keeping
 * MESSAGE IN in the instance.  A command the PPM failed is acknowledged
 * and its reason read (GET_ERROR_STATUS); one still busy at the timeout is
 * canceled; a PPM that does not answer is marked stuck, and every command
 * after it fails at once until the PPM is reset.
 *
 * Returns 0, ENOTSUP when the PPM does not support the command, the
 * errno value of the reason the PPM failed it, ETIMEDOUT, or EIO.
 */
static int
ucsi_command(
	struct drv_ucsi *ucsi,
	uint64_t control)
{
	uint32_t cci;
	int command_error;
	int error;

	/* A PPM that stopped answering is not asked until it is reset. */
	if (ucsi->stuck)
		return ETIMEDOUT;

	/* Runs it. */
	ucsi->message_length = 0;
	command_error = ucsi_execute(ucsi, control, ucsi->message_in, &ucsi->message_length);

	/*
	 * No answer in time: canceled when the PPM said it was busy, else the
	 * PPM stopped.  A command that finished as it was canceled goes on as
	 * any completion.
	 */
	if (command_error == ETIMEDOUT) {
		error = ucsi_cancel(ucsi, control);
		if (error == ECANCELED)
			return ETIMEDOUT;
		if (error != 0)
			return error;
	} else if (command_error != 0) {
		/* A transport that failed leaves the PPM unreachable. */
		ucsi->stuck = true;
		return command_error;
	}

	/* Acknowledges the completion before the next command (section 4), whatever it says. */
	cci = ucsi->cci;
	error = ucsi_acknowledge(ucsi, UCSI_ACK_COMMAND_COMPLETED);
	if (error != 0)
		return error;

	/* A command the PPM does not support. */
	if ((cci & UCSI_CCI_NOT_SUPPORTED) != 0)
		return ENOTSUP;

	/* A command whose failure its caller expects (a partner with nothing to tell) is not asked why. */
	if ((cci & UCSI_CCI_ERROR) != 0 && ucsi->tolerant)
		return EIO;

	/* A command the PPM could not complete: its reason, read now. */
	if ((cci & UCSI_CCI_ERROR) != 0) {
		error = ucsi_error_status(ucsi, control);
		return error;
	}

	/* Succeeded: the answer is in the instance. */
	return 0;
}

/*
 * Cancels a command still busy at its timeout (CANCEL, 3.1 section 6.5.2:
 * sent only after the PPM answered the command with Busy).  A PPM that
 * never said it was busy, or that does not answer the CANCEL either, has
 * stopped and is marked stuck.
 *
 * Returns ECANCELED once the PPM canceled the command (its completion is
 * acknowledged here); 0 when the command finished first, so the PPM
 * dropped the CANCEL: its completion is in ucsi->cci and its MESSAGE IN in
 * the instance, not acknowledged yet; else ETIMEDOUT or EIO.
 */
static int
ucsi_cancel(
	struct drv_ucsi *ucsi,
	uint64_t control)
{
	uint32_t cci;
	int error;

	/* A command the PPM never said it was busy with cannot be canceled: the PPM is silent. */
	if (!ucsi->busy) {
		drv_typec_os_log("ucsi: command 0x%02x got no answer: the PPM stopped\n", (unsigned)(control & 0xFFU));
		ucsi->stuck = true;
		return ETIMEDOUT;
	}

	/* Asks the PPM to drop the command; it answers like any command, into the instance's MESSAGE IN. */
	ucsi->message_length = 0;
	error = ucsi_execute(ucsi, UCSI_CANCEL, ucsi->message_in, &ucsi->message_length);
	if (error != 0) {
		drv_typec_os_log("ucsi: command 0x%02x could not be canceled (error %d): the PPM stopped\n", (unsigned)(control & 0xFFU), error);
		ucsi->stuck = true;
		return error;
	}

	/* The command finished before the CANCEL came: its own completion stands. */
	cci = ucsi->cci;
	if ((cci & UCSI_CCI_CANCEL_COMPLETED) == 0) {
		drv_typec_os_log("ucsi: command 0x%02x finished as it was canceled (CCI 0x%08x)\n", (unsigned)(control & 0xFFU), (unsigned)cci);
		return 0;
	}

	/* Canceled: the CANCEL's completion is acknowledged. */
	error = ucsi_acknowledge(ucsi, UCSI_ACK_COMMAND_COMPLETED);
	if (error != 0)
		return error;

	/* Succeeded: the command is over, undone. */
	drv_typec_os_log("ucsi: command 0x%02x canceled after its timeout\n", (unsigned)(control & 0xFFU));
	return ECANCELED;
}

/*
 * Reads why the PPM failed a command (GET_ERROR_STATUS, 3.1 section
 * 6.5.18), keeps the Error Information in ucsi->error_information, logs it
 * and returns the errno value it stands for.
 *
 * The failed command's completion was acknowledged first, as Linux's
 * ucsi_read_error() does on the PPMs it runs on; 3.1 lets a PPM clear the
 * Error Status after that acknowledgement, so a PPM that does reads as no
 * reason (EIO).  From 3.0 the command names the failed command's
 * connector; 1.x and 2.x leave that field reserved (2.x unconfirmed).  The
 * failure of GET_ERROR_STATUS itself is not read again.
 */
static int
ucsi_error_status(
	struct drv_ucsi *ucsi,
	uint64_t control)
{
	uint64_t error_control;
	uint32_t information;
	unsigned number;
	int error;

	/* GET_ERROR_STATUS that failed has no reason to read. */
	if ((control & 0xFFU) == UCSI_GET_ERROR_STATUS)
		return EIO;

	/* The command, naming the connector from 3.0. */
	number = ucsi_control_connector(control);
	error_control = UCSI_GET_ERROR_STATUS;
	if (ucsi->version >= UCSI_VERSION_3)
		error_control |= (uint64_t)number << UCSI_CONTROL_SPECIFIC_SHIFT;

	/* Asks; a reason that cannot be read leaves the failure plain. */
	error = ucsi_command(ucsi, error_control);
	if (error != 0) {
		drv_typec_os_log("ucsi: command 0x%02x failed; its reason was not read (error %d)\n", (unsigned)(control & 0xFFU), error);
		if (error == ETIMEDOUT)
			return ETIMEDOUT;
		return EIO;
	}

	/* The Error Information (bits 0-15 of MESSAGE IN). */
	information = ucsi_bits(ucsi->message_in, ucsi->message_length, 0, 16);
	ucsi->error_information = (uint16_t)information;
	ucsi_error_log(control, number, information);

	/* Succeeded: the errno value the reason stands for. */
	error = ucsi_error_errno(information);
	return error;
}

/*
 * Chooses the errno value of a failed command's Error Information: the
 * command, its connector or its parameters are wrong (EINVAL), the
 * partner cannot do it (EOPNOTSUPP, the same value as ENOTSUP), the talk
 * with the partner failed (EPROTO), it was refused (EPERM), else EIO.
 * The values are coarse; the record's request_error_information keeps the
 * reason itself.
 */
static int
ucsi_error_errno(
	uint32_t information)
{
	/* The connector named is not there. */
	if ((information & UCSI_ERROR_NO_CONNECTOR) != 0)
		return EINVAL;

	/* The command, or its parameters, are not ones the PPM takes. */
	if ((information & UCSI_ERROR_UNRECOGNIZED) != 0)
		return EINVAL;
	if ((information & UCSI_ERROR_INVALID_PARAMETERS) != 0)
		return EINVAL;

	/* The partner cannot do it. */
	if ((information & UCSI_ERROR_INCOMPATIBLE_PARTNER) != 0)
		return EOPNOTSUPP;

	/* The talk with the partner failed. */
	if ((information & UCSI_ERROR_CC_COMMUNICATION) != 0)
		return EPROTO;
	if ((information & UCSI_ERROR_CONTRACT_NEGOTIATION) != 0)
		return EPROTO;

	/* It was refused: by a dead battery, by the partner, or by the PPM's policy. */
	if ((information & UCSI_ERROR_DEAD_BATTERY) != 0)
		return EPERM;
	if ((information & UCSI_ERROR_PARTNER_REJECTED_SWAP) != 0)
		return EPERM;
	if ((information & UCSI_ERROR_POLICY_CONFLICT) != 0)
		return EPERM;
	if ((information & UCSI_ERROR_SWAP_REJECTED) != 0)
		return EPERM;

	/* Succeeded: no more particular reason, an I/O error. */
	return EIO;
}

/* Logs a failed command's Error Information: the value, then the name of each reason. */
static void
ucsi_error_log(
	uint64_t control,
	unsigned number,
	uint32_t information)
{
	static const char *const names[UCSI_ERROR_BITS] = {
		"unrecognized command",
		"non-existent connector",
		"invalid command parameters",
		"incompatible connector partner",
		"CC communication error",
		"dead battery",
		"contract negotiation failure",
		"overcurrent",
		"undefined",
		"port partner rejected swap",
		"hard reset",
		"PPM policy conflict",
		"swap rejected",
		"reverse current protection",
		"set sink path rejected"
	};
	unsigned bit;

	/* The value; none may mean the PPM cleared it at the failed command's acknowledgement. */
	drv_typec_os_log("ucsi: command 0x%02x connector %u failed: error information 0x%04x\n", (unsigned)(control & 0xFFU), number, (unsigned)information);
	if (information == 0U)
		drv_typec_os_log("ucsi:   no reason given (the PPM may clear it when the failure is acknowledged)\n");

	/* Each reason it names. */
	for (bit = 0U; bit < UCSI_ERROR_BITS; bit++) {
		if ((information & (1U << bit)) != 0U)
			drv_typec_os_log("ucsi:   %s\n", names[bit]);
	}
}

/*
 * Names the connector a command's CONTROL names: GET_ALTERNATE_MODES at
 * bit 24, the commands of the whole PPM none (0), every other at bit 16.
 */
static unsigned
ucsi_control_connector(
	uint64_t control)
{
	unsigned command;

	/* The command's code. */
	command = (unsigned)(control & 0xFFU);

	/* The commands that name no connector. */
	switch (command) {
	case UCSI_PPM_RESET:
	case UCSI_CANCEL:
	case UCSI_ACK_CC_CI:
	case UCSI_SET_NOTIFICATION_ENABLE:
	case UCSI_GET_CAPABILITY:
	case UCSI_GET_ERROR_STATUS:
		return 0U;
	case UCSI_GET_ALTERNATE_MODES:
		/* Its connector follows the recipient (Table 4-24). */
		return (unsigned)((control >> 24) & UCSI_CCI_CONNECTOR_MASK);
	default:
		break;
	}

	/* Succeeded: the connector at bit 16. */
	return (unsigned)((control >> UCSI_CONTROL_SPECIFIC_SHIFT) & UCSI_CCI_CONNECTOR_MASK);
}

/*
 * Writes a command the core sent to the log as one line of the mailbox's
 * record (ws177-p003): "ucsi: rec <number> W <CONTROL>", numbered from 1.
 * Only the first UCSI_RECORD_MAX exchanges are written; the last line of
 * a full record says so.
 */
static void
ucsi_record_write(
	struct drv_ucsi *ucsi,
	uint64_t control)
{
	/* The record is full. */
	if (ucsi->recorded >= UCSI_RECORD_MAX)
		return;
	ucsi->recorded++;

	/* The line, numbered so a line lost from the log is seen. */
	drv_typec_os_log("ucsi: rec %u W %016llx\n", ucsi->recorded, (unsigned long long)control);

	/* The last line a full record takes says so. */
	if (ucsi->recorded == UCSI_RECORD_MAX)
		drv_typec_os_log("ucsi: rec %u end (the record is full)\n", ucsi->recorded);
}

/*
 * Writes what the core read of the mailbox to the log as lines of its
 * record: "ucsi: rec <number> R <CCI> <refresh> <length>", then the
 * MESSAGE IN bytes CCI's length names, UCSI_RECORD_CHUNK a line, under the
 * same number: "ucsi: rec <number> I <offset> <hex>".  Only the first
 * UCSI_RECORD_MAX exchanges are written.
 */
static void
ucsi_record_read(
	struct drv_ucsi *ucsi,
	uint32_t cci,
	bool refresh,
	const uint8_t *message_in,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	char hex[UCSI_RECORD_CHUNK * 2U + 1U];
	size_t length;
	size_t offset;
	size_t index;
	size_t at;

	/* The record is full. */
	if (ucsi->recorded >= UCSI_RECORD_MAX)
		return;
	ucsi->recorded++;

	/* CCI, whether it was fetched, and how many bytes of MESSAGE IN follow. */
	length = (cci >> UCSI_CCI_LENGTH_SHIFT) & UCSI_CCI_LENGTH_MASK;
	if (length > size)
		length = size;
	drv_typec_os_log("ucsi: rec %u R %08x %u %u\n", ucsi->recorded, (unsigned)cci, (unsigned)refresh, (unsigned)length);

	/* MESSAGE IN, a chunk a line. */
	for (offset = 0U; offset < length; offset += UCSI_RECORD_CHUNK) {
		at = 0U;
		for (index = offset; index < length && index < offset + UCSI_RECORD_CHUNK; index++) {
			hex[at] = digits[message_in[index] >> 4];
			hex[at + 1U] = digits[message_in[index] & 0xFU];
			at += 2U;
		}

		/* The chunk's line. */
		hex[at] = '\0';
		drv_typec_os_log("ucsi: rec %u I %u %s\n", ucsi->recorded, (unsigned)offset, hex);
	}

	/* The last line a full record takes says so. */
	if (ucsi->recorded == UCSI_RECORD_MAX)
		drv_typec_os_log("ucsi: rec %u end (the record is full)\n", ucsi->recorded);
}

/*
 * Sends ACK_CC_CI with the acknowledgements given and waits for the PPM's
 * acknowledgement.  Its MESSAGE IN (empty) goes to a buffer of its own, so
 * the answer of the command acknowledged is kept.
 */
static int
ucsi_acknowledge(
	struct drv_ucsi *ucsi,
	uint64_t which)
{
	uint8_t discarded[DRV_UCSI_MESSAGE_MAX];
	size_t length;
	int error;

	/* Sends it and waits; a PPM that does not acknowledge has stopped. */
	error = ucsi_execute(ucsi, UCSI_ACK_CC_CI | which, discarded, &length);
	if (error != 0) {
		drv_typec_os_log("ucsi: ACK_CC_CI got no acknowledgement (error %d): the PPM stopped\n", error);
		ucsi->stuck = true;
		return error;
	}

	/* Succeeded: the PPM may send its next completion or change. */
	return 0;
}

/*
 * Acknowledges a connector change together with the completion of a
 * command that changes nothing (GET_CAPABILITY), never alone.
 *
 * Some PPMs stop answering after an ACK_CC_CI that acknowledges a
 * connector change without a command completion: the next command never
 * completes.  The Latitude 5320's does (its GET_CONNECTOR_STATUS after the
 * first change acknowledgement timed out with CCI 0 at every boot), and
 * Linux v6.8's ucsi_acpi works around the same on every Dell machine
 * (ucsi_dell_sync_write()) in this way.  Acknowledging both at once is
 * what the specification allows (section 4.5.4), so it is done for every
 * PPM: the kernel cannot tell a Dell machine.  The command's own CCI may
 * repeat the change it is acknowledged with; the caller clears that
 * connector's pending change after this returns.
 */
static int
ucsi_acknowledge_change(
	struct drv_ucsi *ucsi)
{
	uint8_t discarded[DRV_UCSI_MESSAGE_MAX];
	size_t length;
	int command_error;
	int error;

	/* A PPM that stopped answering is not asked until it is reset. */
	if (ucsi->stuck)
		return ETIMEDOUT;

	/* Runs the command whose completion goes with the change; its answer is not kept, a silence stops the PPM. */
	command_error = ucsi_execute(ucsi, UCSI_GET_CAPABILITY, discarded, &length);
	if (command_error != 0) {
		ucsi->stuck = true;
		return command_error;
	}

	/* Acknowledges the change and that completion in one ACK_CC_CI. */
	error = ucsi_acknowledge(ucsi, UCSI_ACK_CONNECTOR_CHANGE | UCSI_ACK_COMMAND_COMPLETED);
	if (error != 0)
		return error;

	/* Succeeded: the change is acknowledged and no completion is owed. */
	return 0;
}

/*
 * Resets the PPM and looks at CCI until it reports the reset completed
 * (section 4.5.1: the OPM polls, as notifications are off).
 */
static int
ucsi_reset(
	struct drv_ucsi *ucsi)
{
	const struct drv_ucsi_transport *transport;
	uint32_t elapsed;
	uint32_t cci;
	int status;
	bool completed;

	/* Tells the PPM; nothing it was busy with is waited for after this. */
	transport = ucsi->transport;
	ucsi->busy = false;
	status = transport->write(transport->context, UCSI_PPM_RESET, NULL, 0);
	ucsi_record_write(ucsi, UCSI_PPM_RESET);
	if (status < 0) {
		ucsi->stuck = true;
		return EIO;
	}

	/* Looks at CCI every step until the reset completed or the timeout. */
	completed = false;
	for (elapsed = 0; elapsed < UCSI_RESET_TIMEOUT_MS && !completed; elapsed += UCSI_STEP_FIRST_MS) {
		/* The step. */
		status = transport->wait(transport->context, UCSI_STEP_FIRST_MS);
		if (status < 0) {
			ucsi->stuck = true;
			return EIO;
		}

		/* CCI fetched from the PPM. */
		status = transport->read(transport->context, true, &cci, ucsi->message_in, ucsi->layout->message_in_size);
		if (status < 0) {
			ucsi->stuck = true;
			return EIO;
		}

		/* Recorded. */
		ucsi_record_read(ucsi, cci, true, ucsi->message_in, ucsi->layout->message_in_size);

		/* The reset completed. */
		if ((cci & UCSI_CCI_RESET_COMPLETED) != 0)
			completed = true;
	}

	/* The PPM did not complete the reset. */
	if (!completed) {
		drv_typec_os_log("ucsi: PPM_RESET did not complete\n");
		ucsi->stuck = true;
		return ETIMEDOUT;
	}

	/* Succeeded: nothing from before the reset is pending any more. */
	kern_memset(ucsi->pending, 0, sizeof(ucsi->pending));
	return 0;
}

/* Enables the notifications given (SET_NOTIFICATION_ENABLE, section 4.5.5). */
static int
ucsi_notifications(
	struct drv_ucsi *ucsi,
	uint16_t notifications)
{
	uint64_t control;
	int error;

	/* Sends the set. */
	control = UCSI_SET_NOTIFICATION_ENABLE | ((uint64_t)notifications << UCSI_CONTROL_SPECIFIC_SHIFT);
	error = ucsi_command(ucsi, control);
	if (error != 0)
		return error;

	/* Succeeded: these are on. */
	ucsi->notifications = notifications;
	return 0;
}

/*
 * Reads GET_CAPABILITY (section 4.5.6, Table 4-13): the attributes, the
 * number of connectors (bits 32-38), the optional features (bits 40-63)
 * and the number of Alternate Modes (bits 64-71).
 */
static int
ucsi_capability(
	struct drv_ucsi *ucsi)
{
	int error;

	/* Asks. */
	error = ucsi_command(ucsi, UCSI_GET_CAPABILITY);
	if (error != 0)
		return error;

	/* Reads the fields. */
	ucsi->attributes = ucsi_bits(ucsi->message_in, ucsi->message_length, 0, 32);
	ucsi->connector_count = ucsi_bits(ucsi->message_in, ucsi->message_length, 32, 7);
	ucsi->optional_features = ucsi_bits(ucsi->message_in, ucsi->message_length, 40, 24);
	ucsi->alt_mode_count = ucsi_bits(ucsi->message_in, ucsi->message_length, 64, 8);
	drv_typec_os_log("ucsi: %u connectors, %u Alternate Modes, features 0x%06x\n", ucsi->connector_count, ucsi->alt_mode_count, (unsigned)ucsi->optional_features);

	/* Keeps the connectors the Type-C layer has room for. */
	if (ucsi->connector_count > DRV_TYPEC_CONNECTOR_MAX)
		ucsi->connector_count = DRV_TYPEC_CONNECTOR_MAX;

	/* Succeeded: the PPM's capability is known. */
	return 0;
}

/*
 * Reads what a connector can do (GET_CONNECTOR_CAPABILITY, section 4.5.7)
 * and the Alternate Modes it supports, then its state.  The layer keeps
 * the outcome of the connector's last operation across the publishing, so
 * a reset that brings the PPM back reads the same way.
 */
static int
ucsi_connector_start(
	struct drv_ucsi *ucsi,
	unsigned number)
{
	struct drv_typec_connector *record;
	int error;

	/*
	 * Asks for the capability: the operation modes (bits 0-7) and the
	 * provider, consumer and swap bits (bits 8-13) are the layer's
	 * capability bits as they stand (Table 4-17).
	 */
	record = &ucsi->record;
	kern_memset(record, 0, sizeof(*record));
	error = ucsi_command(ucsi, ucsi_connector_control(UCSI_GET_CONNECTOR_CAPABILITY, number));
	if (error != 0)
		return error;
	record->capability = ucsi_bits(ucsi->message_in, ucsi->message_length, 0, 14);

	/* The connector's own Alternate Modes, which the current mode indexes. */
	if ((ucsi->optional_features & UCSI_FEATURE_ALT_MODE_DETAILS) != 0 && ucsi->alt_mode_count != 0) {
		error = ucsi_alt_modes(ucsi, number, UCSI_RECIPIENT_CONNECTOR, &record->connector_modes);
		if (error != 0)
			return error;
	}

	/* Publishes what does not change, then reads the state. */
	error = drv_typec_connector_publish(number - 1U, record);
	if (error != 0)
		return error;
	error = ucsi_connector_update(ucsi, number);
	if (error != 0)
		return error;

	/* Succeeded: the connector's record is complete. */
	return 0;
}

/*
 * Reads a connector's state (status, and when something is attached its
 * Alternate Modes and PDOs) into a new record, keeping what the connector
 * can do, and publishes it.
 */
static int
ucsi_connector_update(
	struct drv_ucsi *ucsi,
	unsigned number)
{
	struct drv_typec_connector *record;
	int error;

	/* Starts from the published record, which holds the connector's capability and modes. */
	record = &ucsi->record;
	error = drv_typec_connector_get(number - 1U, record);
	if (error != 0)
		return error;

	/* The status. */
	error = ucsi_status(ucsi, number, record);
	if (error != 0)
		return error;

	/* Nothing attached: no partner, no cable, no mode, no PDO, and that is the record. */
	ucsi_partner_clear(record);
	if (!record->connected) {
		error = drv_typec_connector_publish(number - 1U, record);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * The partner's and the cable's Alternate Modes and the mode the
	 * connector is in.  A partner or a cable that has none can make the PPM
	 * report an error; the list is then empty.
	 */
	if ((ucsi->optional_features & UCSI_FEATURE_ALT_MODE_DETAILS) != 0) {
		/* tolerant: these failures are expected and not asked about (GET_ERROR_STATUS). */
		ucsi->tolerant = true;
		(void)ucsi_alt_modes(ucsi, number, UCSI_RECIPIENT_SOP, &record->partner_modes);
		(void)ucsi_alt_modes(ucsi, number, UCSI_RECIPIENT_SOP_PRIME, &record->cable_modes);
		ucsi->tolerant = false;
		error = ucsi_current_modes(ucsi, number, record);
		if (error != 0)
			return error;

		/*
		 * The hot plug detect and the pin assignment of a DisplayPort mode
		 * the connector is in, which a 3.x PPM reports; one that cannot
		 * leaves them unknown.
		 */
		if (ucsi->version >= UCSI_VERSION_3) {
			ucsi->tolerant = true;
			(void)ucsi_dp_status(ucsi, number, record);
			ucsi->tolerant = false;
		}
	}

	/* The partner's PDOs, when the PPM reports PDOs and the contract is USB PD (a failure expected). */
	if ((ucsi->optional_features & UCSI_FEATURE_PDO_DETAILS) != 0 && record->power_operation == DRV_TYPEC_POWER_PD) {
		ucsi->tolerant = true;
		(void)ucsi_partner_pdos(ucsi, number, record);
		ucsi->tolerant = false;
	}

	/* The cable's properties, when the PPM reports them (a cable that tells nothing leaves them unknown). */
	if ((ucsi->optional_features & UCSI_FEATURE_CABLE_DETAILS) != 0) {
		ucsi->tolerant = true;
		(void)ucsi_cable(ucsi, number, record);
		ucsi->tolerant = false;
	}

	/* Publishes the new state. */
	error = drv_typec_connector_publish(number - 1U, record);
	if (error != 0)
		return error;

	/* Succeeded: the connector's record is current. */
	return 0;
}

/*
 * Reads GET_CONNECTOR_STATUS (section 4.5.17, Table 4-42; 3.1 Table 6-43
 * for the later fields) into a record.
 */
static int
ucsi_status(
	struct drv_ucsi *ucsi,
	unsigned number,
	struct drv_typec_connector *record)
{
	const uint8_t *data;
	uint32_t operation;
	uint32_t flags;
	uint32_t bit;
	size_t length;
	int error;

	/* Asks. */
	error = ucsi_command(ucsi, ucsi_connector_control(UCSI_GET_CONNECTOR_STATUS, number));
	if (error != 0)
		return error;
	data = ucsi->message_in;
	length = ucsi->message_length;

	/* Whether something is attached (bit 19). */
	record->connected = false;
	bit = ucsi_bits(data, length, 19, 1);
	if (bit != 0)
		record->connected = true;

	/* How power is delivered (bits 16-18; 6, 5 A, is from 3.1), unknown when the value is reserved. */
	operation = ucsi_bits(data, length, 16, 3);
	record->power_operation = DRV_TYPEC_POWER_UNKNOWN;
	if (operation >= DRV_TYPEC_POWER_USB_DEFAULT && operation <= DRV_TYPEC_POWER_TYPEC_5A)
		record->power_operation = (enum drv_typec_power_operation)operation;

	/* The role in the power (bit 20: 1 is the provider). */
	record->power_role = DRV_TYPEC_ROLE_SINK;
	bit = ucsi_bits(data, length, 20, 1);
	if (bit != 0)
		record->power_role = DRV_TYPEC_ROLE_SOURCE;

	/* What is carried to the partner (bits 21-28: USB, Alternate Mode; USB4 from 3.1 only). */
	flags = ucsi_bits(data, length, 21, 8);
	record->partner_flags = flags & (DRV_TYPEC_PARTNER_USB | DRV_TYPEC_PARTNER_ALT_MODE);
	if (ucsi->version >= UCSI_VERSION_2)
		record->partner_flags = flags & (DRV_TYPEC_PARTNER_USB | DRV_TYPEC_PARTNER_ALT_MODE | DRV_TYPEC_PARTNER_USB4);

	/* The partner's kind (bits 29-31; 7 is reserved). */
	record->partner_type = (enum drv_typec_partner_type)ucsi_bits(data, length, 29, 3);
	if (record->partner_type > DRV_TYPEC_PARTNER_AUDIO_ACCESSORY)
		record->partner_type = DRV_TYPEC_PARTNER_NONE;

	/* The contract's Request Data Object (bits 32-63). */
	record->request_data_object = ucsi_bits(data, length, 32, 32);

	/* The orientation (bit 86), which a 1.x status does not hold. */
	record->orientation = DRV_TYPEC_ORIENTATION_UNKNOWN;
	if (ucsi->version >= UCSI_VERSION_2 && length >= UCSI_STATUS_ORIENTATION_BYTES) {
		record->orientation = DRV_TYPEC_ORIENTATION_NORMAL;
		bit = ucsi_bits(data, length, UCSI_STATUS_ORIENTATION_BIT, 1);
		if (bit != 0)
			record->orientation = DRV_TYPEC_ORIENTATION_FLIPPED;
	}

	/* Nothing attached: none of the attached fields mean anything. */
	if (!record->connected) {
		record->power_operation = DRV_TYPEC_POWER_UNKNOWN;
		record->power_role = DRV_TYPEC_ROLE_SINK;
		record->partner_flags = 0;
		record->partner_type = DRV_TYPEC_PARTNER_NONE;
		record->request_data_object = 0;
		record->orientation = DRV_TYPEC_ORIENTATION_UNKNOWN;
	}

	/* Succeeded: the record holds the status. */
	return 0;
}

/* Empties what a record knows of the partner and the cable (read again when something is attached). */
static void
ucsi_partner_clear(
	struct drv_typec_connector *record)
{
	/* The partner's and the cable's modes, the current modes and the PDOs. */
	kern_memset(&record->partner_modes, 0, sizeof(record->partner_modes));
	kern_memset(&record->cable_modes, 0, sizeof(record->cable_modes));
	kern_memset(record->supported_modes, 0, sizeof(record->supported_modes));
	kern_memset(record->current_modes, 0, sizeof(record->current_modes));
	record->current_mode_count = 0;
	kern_memset(record->partner_pdos, 0, sizeof(record->partner_pdos));
	record->partner_pdo_count = 0;
	kern_memset(&record->cable, 0, sizeof(record->cable));
	kern_memset(&record->dp_ucsi, 0, sizeof(record->dp_ucsi));
}

/*
 * Reads a list of Alternate Modes with GET_ALTERNATE_MODES (section
 * 4.5.11), two at a time with the offset moved on, until the PPM returns
 * fewer than asked or a blank SVID, or the list is full.
 */
static int
ucsi_alt_modes(
	struct drv_ucsi *ucsi,
	unsigned number,
	unsigned recipient,
	struct drv_typec_alt_mode_list *list)
{
	const uint8_t *mode;
	uint64_t control;
	unsigned offset;
	unsigned count;
	unsigned index;
	uint16_t svid;
	int error;

	/*
	 * Asks for two modes from the offset each time: the recipient (bits
	 * 16-18), the connector (bits 24-30), the offset (bits 32-39) and the
	 * count less one (bits 40-41) (Table 4-24).
	 */
	list->count = 0;
	for (offset = 0; offset < DRV_TYPEC_ALT_MODE_MAX; offset += UCSI_ALT_MODES_PER_COMMAND) {
		control = UCSI_GET_ALTERNATE_MODES;
		control |= (uint64_t)recipient << 16;
		control |= (uint64_t)number << 24;
		control |= (uint64_t)offset << 32;
		control |= (uint64_t)(UCSI_ALT_MODES_PER_COMMAND - 1U) << 40;
		error = ucsi_command(ucsi, control);
		if (error != 0)
			return error;

		/* Each mode returned: its SVID and its MID (Table 4-26); a blank one ends the list. */
		count = (unsigned)(ucsi->message_length / UCSI_ALT_MODE_BYTES);
		if (count > UCSI_ALT_MODES_PER_COMMAND)
			count = UCSI_ALT_MODES_PER_COMMAND;
		for (index = 0; index < count && list->count < DRV_TYPEC_ALT_MODE_MAX; index++) {
			mode = &ucsi->message_in[index * UCSI_ALT_MODE_BYTES];
			svid = (uint16_t)ucsi_get16(mode);
			if (svid == 0)
				return 0;
			list->modes[list->count].svid = svid;
			list->modes[list->count].vdo = ucsi_get32(&mode[2]);
			list->count++;
		}

		/* Fewer than asked: the end of the list. */
		if (count < UCSI_ALT_MODES_PER_COMMAND)
			break;
	}

	/* Succeeded: the list holds every mode returned. */
	return 0;
}

/*
 * Reads which of the connector's modes can be entered now
 * (GET_CAM_SUPPORTED, section 4.5.12: one bit per index) and which it is
 * in (GET_CURRENT_CAM, section 4.5.13: one index per byte, 0xFF for none).
 */
static int
ucsi_current_modes(
	struct drv_ucsi *ucsi,
	unsigned number,
	struct drv_typec_connector *record)
{
	size_t length;
	size_t index;
	uint8_t mode;
	int error;

	/* The modes that can be entered. */
	error = ucsi_command(ucsi, ucsi_connector_control(UCSI_GET_CAM_SUPPORTED, number));
	if (error != 0)
		return error;
	length = ucsi->message_length;
	if (length > sizeof(record->supported_modes))
		length = sizeof(record->supported_modes);
	kern_memcpy(record->supported_modes, ucsi->message_in, length);

	/* The modes it is in, each an index into the connector's list. */
	error = ucsi_command(ucsi, ucsi_connector_control(UCSI_GET_CURRENT_CAM, number));
	if (error != 0)
		return error;
	record->current_mode_count = 0;
	for (index = 0; index < ucsi->message_length; index++) {
		mode = ucsi->message_in[index];
		if (mode == UCSI_NO_CURRENT_MODE || mode >= record->connector_modes.count)
			continue;
		if (record->current_mode_count < DRV_TYPEC_CURRENT_MODE_MAX)
			record->current_modes[record->current_mode_count++] = mode;
	}

	/* Succeeded: the record holds the connector's modes. */
	return 0;
}

/*
 * Reads the partner's PDOs with GET_PDOS (section 4.5.15): its source ones
 * when this side sinks, else its sink ones, four at a time.
 */
static int
ucsi_partner_pdos(
	struct drv_ucsi *ucsi,
	unsigned number,
	struct drv_typec_connector *record)
{
	uint64_t control;
	unsigned offset;
	unsigned wanted;
	unsigned count;
	unsigned index;
	int error;

	/*
	 * Asks for up to four from the offset, never past the seventh: the
	 * connector (bits 16-22), the partner bit (23), the offset (bits
	 * 24-31), the count less one (bits 32-33), source or sink (bit 34),
	 * and the current capabilities (bits 35-36 zero) (Table 4-34).
	 */
	record->partner_pdo_count = 0;
	for (offset = 0; offset < DRV_TYPEC_PDO_MAX; offset += count) {
		wanted = DRV_TYPEC_PDO_MAX - offset;
		if (wanted > UCSI_PDOS_PER_COMMAND)
			wanted = UCSI_PDOS_PER_COMMAND;
		control = UCSI_GET_PDOS;
		control |= (uint64_t)number << 16;
		control |= 1ULL << 23;
		control |= (uint64_t)offset << 24;
		control |= (uint64_t)(wanted - 1U) << 32;
		if (record->power_role == DRV_TYPEC_ROLE_SINK)
			control |= 1ULL << 34;
		error = ucsi_command(ucsi, control);
		if (error != 0)
			return error;

		/* Each PDO returned (Table 4-36). */
		count = (unsigned)(ucsi->message_length / 4U);
		if (count > wanted)
			count = wanted;
		for (index = 0; index < count; index++)
			record->partner_pdos[record->partner_pdo_count++] = ucsi_get32(&ucsi->message_in[index * 4U]);

		/* Fewer than asked: the end. */
		if (count < wanted || count == 0)
			break;
	}

	/* Succeeded: the record holds the partner's PDOs. */
	return 0;
}

/*
 * Reads the configuration and status of the DisplayPort mode a connector
 * is in (GET_CAM_CS, 3.1 section 6.5.22): the hot plug detect from the
 * DisplayPort Status, and the pin assignment from the Configuration VDO.
 *
 * The command names the mode by "one of the current Alternate Modes
 * obtained from GET_CURRENT_CAM"; this driver gives the mode's index as
 * GET_CURRENT_CAM returned it (an index into the connector's modes), not
 * its place in that array -- the 3.1 text reads either way and no PPM was
 * at hand to tell (unconfirmed).
 *
 * Returns 0 with record->dp_ucsi known, ENOENT when the connector is in no
 * DisplayPort mode, or the command's errno value (the state stays unknown).
 */
static int
ucsi_dp_status(
	struct drv_ucsi *ucsi,
	unsigned number,
	struct drv_typec_connector *record)
{
	uint64_t control;
	uint32_t status;
	uint32_t configuration;
	uint32_t pins;
	unsigned count;
	unsigned mode;
	unsigned index;
	unsigned pin;
	int error;

	/* The first current mode that is DisplayPort. */
	mode = UCSI_NO_CURRENT_MODE;
	for (index = 0; index < record->current_mode_count; index++) {
		if (record->connector_modes.modes[record->current_modes[index]].svid == DRV_TYPEC_SVID_DISPLAYPORT) {
			mode = record->current_modes[index];
			break;
		}
	}

	/* A connector in no DisplayPort mode has nothing to report. */
	if (mode == UCSI_NO_CURRENT_MODE)
		return ENOENT;

	/* Asks: the connector (bits 16-22) and the current mode (bits 24-31) (Table 6-58). */
	control = ucsi_connector_control(UCSI_GET_CAM_CS, number);
	control |= (uint64_t)mode << 24;
	error = ucsi_command(ucsi, control);
	if (error != 0)
		return error;

	/* The DisplayPort Status: the hot plug detect. */
	status = ucsi_bits(ucsi->message_in, ucsi->message_length, UCSI_CAM_CS_STATUS_BIT, 32);
	record->dp_ucsi.hpd = false;
	if ((status & UCSI_DP_STATUS_HPD) != 0)
		record->dp_ucsi.hpd = true;

	/* The Configuration VDO, when one came: its lowest pin bit names the assignment (A for bit 8). */
	record->dp_ucsi.pin = DRV_TYPEC_DP_PIN_NONE;
	count = ucsi_bits(ucsi->message_in, ucsi->message_length, UCSI_CAM_CS_COUNT_BIT, 8);
	if (count != 0) {
		configuration = ucsi_bits(ucsi->message_in, ucsi->message_length, UCSI_CAM_CS_VDO_BIT, 32);
		pins = (configuration >> UCSI_DP_CONFIG_PIN_SHIFT) & UCSI_DP_CONFIG_PIN_MASK;
		for (pin = 0; pin < (unsigned)DRV_TYPEC_DP_PIN_F; pin++) {
			if ((pins & (1U << pin)) != 0) {
				record->dp_ucsi.pin = (enum drv_typec_dp_pin)(DRV_TYPEC_DP_PIN_A + (int)pin);
				break;
			}
		}
	}

	/* The orientation is the status's; the lanes are the display driver's to know. */
	record->dp_ucsi.orientation = record->orientation;
	record->dp_ucsi.lanes = 0;

	/* Succeeded: UCSI's report of DisplayPort on the connector. */
	record->dp_ucsi.known = true;
	return 0;
}

/*
 * Reads and acknowledges each connector whose change the PPM indicated,
 * lowest number first.  A connector stays pending until its change is
 * acknowledged: the PPM repeats the indication in every CCI until then.
 */
static int
ucsi_pending_handle(
	struct drv_ucsi *ucsi)
{
	unsigned number;
	unsigned byte;
	uint8_t bit;
	int error;

	/* Each pending connector, looking again after each (another may have come). */
	number = 1;
	while (number < 128U) {
		/* Skips a connector that did not change. */
		byte = number / 8U;
		bit = (uint8_t)(1U << (number % 8U));
		if ((ucsi->pending[byte] & bit) == 0) {
			number++;
			continue;
		}

		/*
		 * Reads its state, unless it is not one the layer keeps.  A reading
		 * that fails while the PPM answers still lets the change be
		 * acknowledged: the PPM tells of no other change until then.  The
		 * record keeps what was published before.
		 */
		if (number <= ucsi->connector_count) {
			error = ucsi_connector_update(ucsi, number);
			if (error != 0 && ucsi->stuck)
				return error;
			if (error != 0)
				drv_typec_os_log("ucsi: connector %u's change was not read (error %d); acknowledged all the same\n", number, error);
		}

		/* Acknowledges the change; the PPM may then indicate the next. */
		error = ucsi_acknowledge_change(ucsi);
		if (error != 0)
			return error;
		ucsi->pending[byte] &= (uint8_t)~bit;
		number = 1;
	}

	/* Succeeded: no change is pending. */
	return 0;
}

/*
 * Makes the CONTROL of an operation.  Returns 0, EINVAL for a mode the
 * connector does not list or a kind that is not one, or ENOTSUP for an
 * operation this PPM's version or features cannot do.
 */
static int
ucsi_request_control(
	const struct drv_ucsi *ucsi,
	const struct drv_typec_request *request,
	uint64_t *control)
{
	struct drv_typec_connector record;
	unsigned number;
	int error;

	/* The connector it names. */
	number = request->connector + 1U;
	if (number > ucsi->connector_count)
		return EINVAL;

	/* Each kind's command. */
	switch (request->kind) {
	case DRV_TYPEC_REQUEST_DATA_ROLE:
		/* A swap to DFP or to UFP, and the partner's swaps accepted (policy 0: all). */
		*control = ucsi_connector_control(UCSI_SET_UOR, number) | UCSI_ROLE_ACCEPT_BIT;
		if (request->value == DRV_TYPEC_DATA_DFP) {
			*control |= UCSI_ROLE_FIRST_BIT;
		} else {
			*control |= UCSI_ROLE_SECOND_BIT;
		}

		break;
	case DRV_TYPEC_REQUEST_POWER_ROLE:
		/* A swap to Source or to Sink, and the partner's swaps accepted. */
		*control = ucsi_connector_control(UCSI_SET_PDR, number) | UCSI_ROLE_ACCEPT_BIT;
		if (request->value == DRV_TYPEC_ROLE_SOURCE) {
			*control |= UCSI_ROLE_FIRST_BIT;
		} else {
			*control |= UCSI_ROLE_SECOND_BIT;
		}

		break;
	case DRV_TYPEC_REQUEST_RESET:
		/* The reset type's bit as the PPM's version reads it; a 1.x PPM has no Data Reset. */
		*control = ucsi_connector_control(UCSI_CONNECTOR_RESET, number);
		if (request->value == DRV_TYPEC_RESET_DATA) {
			if (ucsi->version < UCSI_VERSION_2)
				return ENOTSUP;
			*control |= UCSI_RESET_TYPE_BIT;
		} else if (ucsi->version == UCSI_VERSION_1_0) {
			*control |= UCSI_RESET_TYPE_BIT;
		}

		break;
	case DRV_TYPEC_REQUEST_ENTER_MODE:
	case DRV_TYPEC_REQUEST_EXIT_MODE:
		/* Only a PPM that lets the OPM choose the mode, and only a mode the connector lists. */
		if ((ucsi->optional_features & UCSI_FEATURE_ALT_MODE_OVERRIDE) == 0)
			return ENOTSUP;
		error = drv_typec_connector_get(request->connector, &record);
		if (error != 0)
			return error;
		if (request->mode >= record.connector_modes.count)
			return EINVAL;
		*control = ucsi_connector_control(UCSI_SET_NEW_CAM, number) | ((uint64_t)request->mode << UCSI_CAM_SHIFT);
		if (request->kind == DRV_TYPEC_REQUEST_ENTER_MODE)
			*control |= UCSI_CAM_ENTER_BIT | ((uint64_t)request->configuration << UCSI_CAM_SPECIFIC_SHIFT);
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the command. */
	return 0;
}

/*
 * Reads the attached cable's properties (GET_CABLE_PROPERTY, 3.1 Table
 * 6-38 to 6-40): its speed (a mantissa at bits 2-15 times 1000 to the
 * exponent at bits 0-1, in bits per second), its current (bits 16-23, in
 * 50 mA), whether it carries VBUS (24), is active (25), has configurable
 * lanes (26), its far end (27-28) and, for an active cable, whether it
 * has Alternate Modes (29).
 */
static int
ucsi_cable(
	struct drv_ucsi *ucsi,
	unsigned number,
	struct drv_typec_connector *record)
{
	struct drv_typec_cable *cable;
	uint32_t mantissa;
	uint32_t exponent;
	uint64_t speed;
	int error;

	/* Asks. */
	cable = &record->cable;
	kern_memset(cable, 0, sizeof(*cable));
	error = ucsi_command(ucsi, ucsi_connector_control(UCSI_GET_CABLE_PROPERTY, number));
	if (error != 0)
		return error;

	/* A cable that answered nothing is not known. */
	if (ucsi->message_length < 5U)
		return 0;

	/* The speed in bits per second. */
	exponent = ucsi_bits(ucsi->message_in, ucsi->message_length, 0, 2);
	mantissa = ucsi_bits(ucsi->message_in, ucsi->message_length, 2, 14);
	speed = mantissa;
	while (exponent > 0) {
		speed *= 1000U;
		exponent--;
	}

	/* The fields. */
	cable->known = true;
	cable->speed_bps = speed;
	cable->current_ma = ucsi_bits(ucsi->message_in, ucsi->message_length, 16, 8) * 50U;
	cable->vbus = ucsi_bits(ucsi->message_in, ucsi->message_length, 24, 1);
	cable->active = ucsi_bits(ucsi->message_in, ucsi->message_length, 25, 1);
	cable->directional = ucsi_bits(ucsi->message_in, ucsi->message_length, 26, 1);
	cable->plug_end = (enum drv_typec_plug_end)ucsi_bits(ucsi->message_in, ucsi->message_length, 27, 2);
	cable->modes = ucsi_bits(ucsi->message_in, ucsi->message_length, 29, 1);

	/* Succeeded: the cable is known. */
	return 0;
}

/* Keeps the connector a CCI indicates a change on as pending. */
static void
ucsi_latch(
	struct drv_ucsi *ucsi,
	uint32_t cci)
{
	unsigned number;

	/* The connector number (bits 1-7), 0 for none. */
	number = (cci >> UCSI_CCI_CONNECTOR_SHIFT) & UCSI_CCI_CONNECTOR_MASK;
	if (number != 0)
		ucsi->pending[number / 8U] |= (uint8_t)(1U << (number % 8U));
}

/* Makes the CONTROL of a command that names a connector at bit 16. */
static uint64_t
ucsi_connector_control(
	unsigned command,
	unsigned number)
{
	/* The command and the connector. */
	return (uint64_t)command | ((uint64_t)number << UCSI_CONTROL_SPECIFIC_SHIFT);
}

/* Reads a little-endian 16-bit value. */
static uint32_t
ucsi_get16(
	const uint8_t *data)
{
	/* Low byte first. */
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8);
}

/* Reads a little-endian 32-bit value. */
static uint32_t
ucsi_get32(
	const uint8_t *data)
{
	/* Low byte first. */
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/*
 * Reads a field of up to 32 bits at a bit offset of a little-endian
 * message (the specification numbers bits from bit 0 of byte 0); bits past
 * the message's length read as zero.
 */
static uint32_t
ucsi_bits(
	const uint8_t *data,
	size_t length,
	unsigned offset,
	unsigned width)
{
	uint32_t value;
	unsigned bit;
	unsigned at;

	/* Each bit, lowest first. */
	value = 0;
	for (bit = 0; bit < width; bit++) {
		at = offset + bit;
		if (at / 8U >= length)
			break;
		if ((data[at / 8U] & (1U << (at % 8U))) != 0)
			value |= 1UL << bit;
	}

	/* The field. */
	return value;
}
