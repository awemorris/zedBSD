/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The UCSI core and the transports it runs over (kernel-internal).
 *
 * UCSI (the USB Type-C Connector System Software Interface) is a mailbox
 * between the operating system (the OPM) and the platform's policy
 * manager (the PPM): the OPM writes a command to CONTROL, the PPM answers
 * in CCI and MESSAGE IN and notifies.  The core runs the commands, keeps
 * the acknowledgement rules and turns the answers into the Type-C layer's
 * connector records.  A transport (ACPI on a PC, ws050-p003) moves the
 * mailbox's bytes and delivers the notifications.
 *
 * The values are from "USB Type-C Connector System Software Interface
 * (UCSI) Requirements Specification", Intel, Revision 1.2 (January 2020,
 * document 336205-002), and "USB Type-C Connector System Software
 * Interface (UCSI) Specification", USB Promoter Group, Revision 3.1 (June
 * 2026); the section of each is named where it is used.  The 2.0 and 2.1
 * documents were not at hand: where the 2.x arrangement is taken from the
 * 3.1 document, that is marked unconfirmed for 2.0.
 */

#ifndef KERN_DRIVERS_TYPEC_UCSI_H
#define KERN_DRIVERS_TYPEC_UCSI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/typec/typec.h>

/*
 * The largest MESSAGE IN or MESSAGE OUT of any arrangement (UCSI 3.1
 * Table 4-1: 2040 bits, 255 bytes; MAX_DATA_LENGTH 0xFF, Table A-2).
 */
#define DRV_UCSI_MESSAGE_MAX 255U

/*
 * One arrangement of the mailbox: where each data structure is and how
 * large the messages are.  The transport picks it from the size of the
 * platform's mailbox region, not from VERSION, which the platform writes
 * when it likes.
 */
struct drv_ucsi_layout {
	/* The arrangement's name in the log. */
	const char *name;

	/* The smallest mailbox region that holds it. */
	size_t size;

	/* Where VERSION, CCI, CONTROL and the two messages are (byte offsets). */
	size_t version_offset;
	size_t cci_offset;
	size_t control_offset;
	size_t message_in_offset;
	size_t message_out_offset;

	/* How large the two messages are. */
	size_t message_in_size;
	size_t message_out_size;
};

/*
 * The arrangement of UCSI 1.x (1.2 Table 3-1): messages of 16 bytes.
 */
extern const struct drv_ucsi_layout drv_ucsi_layout_1;

/*
 * The arrangement of UCSI 2.x and later (3.1 Table 4-1, unconfirmed for
 * 2.0): messages of 255 bytes.
 */
extern const struct drv_ucsi_layout drv_ucsi_layout_2;

/*
 * The operations a transport gives the core.
 *
 * write puts CONTROL (and MESSAGE OUT when length is not 0) in the mailbox
 * and tells the PPM; read gives CCI and MESSAGE IN (refresh asks the
 * transport to fetch them from the PPM first, which is needed when no
 * notification said they changed); wait returns 1 as soon as the PPM
 * notifies, or 0 when the milliseconds have passed without one.  Each
 * returns a negative value when the transport fails.
 */
struct drv_ucsi_transport {
	int (*write)(void *context, uint64_t control, const uint8_t *message_out, size_t length);
	int (*read)(void *context, bool refresh, uint32_t *cci, uint8_t *message_in, size_t size);
	int (*wait)(void *context, uint32_t milliseconds);
	void *context;
};

/*
 * One UCSI interface: the transport it runs over, what its PPM reported,
 * and the connector changes not handled yet.
 *
 * The transport owns the instance; only the core's thread uses it.
 */
struct drv_ucsi {
	/* The transport and its mailbox arrangement. */
	const struct drv_ucsi_transport *transport;
	const struct drv_ucsi_layout *layout;

	/* The UCSI version the PPM wrote to VERSION (BCD, 0x0120 is 1.2). */
	uint16_t version;

	/* What GET_CAPABILITY reported. */
	uint32_t attributes;
	uint32_t optional_features;
	unsigned connector_count;
	unsigned alt_mode_count;

	/* The notifications enabled. */
	uint16_t notifications;

	/*
	 * The connectors whose change the PPM indicated in a CCI and the core
	 * has not acknowledged yet, one bit per connector number (1 to 127).
	 * A change seen while another command ran is kept here, not lost.
	 */
	uint8_t pending[16];

	/* The MESSAGE IN of the last command, and its length. */
	uint8_t message_in[DRV_UCSI_MESSAGE_MAX];
	size_t message_length;

	/* The record being built for one connector (kept here, not on the stack). */
	struct drv_typec_connector record;

	/*
	 * The CCI the last command ended with, and whether its wait last saw
	 * the PPM busy (a command still busy at the timeout is canceled).
	 */
	uint32_t cci;
	bool busy;

	/*
	 * The PPM's health (ws177-p003).  stuck: a command got no answer (or
	 * the transport failed); every later command fails at once until the
	 * PPM is reset and the connectors are read again.  recovering: that
	 * reset runs, so a failure inside it does not start another.
	 * recoveries: the attempts the last recovery took; when its last
	 * attempt failed too, failed is set, and the transport answers it by
	 * stopping the driver.  tolerant: the command running is one whose
	 * failure its caller expects, so its reason is not asked.
	 */
	bool stuck;
	bool recovering;
	bool failed;
	bool tolerant;
	unsigned recoveries;

	/* The Error Information GET_ERROR_STATUS gave for the last command that failed (0: none). */
	uint16_t error_information;

	/*
	 * The completions the core found after a notification and the ones it
	 * found only by asking the PPM: a PPM whose notifications never come
	 * is polled by the transport (drv_ucsi_notifying()).
	 */
	unsigned notified_completions;
	unsigned polled_completions;

	/* The mailbox exchanges written to the log so far (the record of a start, at most UCSI_RECORD_MAX). */
	unsigned recorded;
};

/*
 * Picks the mailbox arrangement that a region of a size holds.
 */
const struct drv_ucsi_layout *
drv_ucsi_layout_select(
	size_t region_size);

/*
 * Starts a UCSI interface: resets the PPM, reads its capability and every
 * connector, and enables the notifications.
 */
int
drv_ucsi_start(
	struct drv_ucsi *ucsi,
	const struct drv_ucsi_transport *transport,
	const struct drv_ucsi_layout *layout,
	uint16_t version);

/*
 * Carries out an operation another driver asked of a connector and
 * publishes the connector's record with its outcome.
 */
int
drv_ucsi_request(
	struct drv_ucsi *ucsi,
	const struct drv_typec_request *request);

/*
 * Handles a notification that came while no command was running: reads
 * CCI and every connector whose change it indicates.
 */
int
drv_ucsi_service(
	struct drv_ucsi *ucsi);

/*
 * Asks the PPM for CCI without a notification and handles the connector
 * change it indicates (a notification that never came).
 */
int
drv_ucsi_poll(
	struct drv_ucsi *ucsi);

/*
 * Tells whether the PPM's notifications come: true once a command's
 * completion arrived with one.
 */
bool
drv_ucsi_notifying(
	const struct drv_ucsi *ucsi);

#endif
