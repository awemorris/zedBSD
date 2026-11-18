/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB CCID reader driver (ws161-p003, usb-ccid.c), and its pure part,
 * the CCID 1.1 messages and the class descriptor (usb-ccid-proto.c), which
 * the host test compiles as it is.
 */

#ifndef KERN_DRIVERS_USB_CCID_H
#define KERN_DRIVERS_USB_CCID_H

#include <stddef.h>
#include <stdint.h>

/* The interface class, the class descriptor's type and its length. */
#define CCID_INTERFACE_CLASS		0x0bU
#define CCID_CLASS_DESCRIPTOR		0x21U
#define CCID_CLASS_DESCRIPTOR_LENGTH	54U

/* The header of every message, and the most bytes a message carries (header and the longest APDU's block). */
#define CCID_HEADER			10U
#define CCID_MESSAGE_MAX		(CCID_HEADER + 65544U)

/* The commands sent (PC_to_RDR_*) and the answers (RDR_to_PC_*) the driver knows. */
#define CCID_PC_TO_RDR_ICC_POWER_ON	0x62U
#define CCID_PC_TO_RDR_ICC_POWER_OFF	0x63U
#define CCID_PC_TO_RDR_GET_SLOT_STATUS	0x65U
#define CCID_PC_TO_RDR_XFR_BLOCK	0x6fU
#define CCID_PC_TO_RDR_ABORT		0x72U
#define CCID_RDR_TO_PC_DATA_BLOCK	0x80U
#define CCID_RDR_TO_PC_SLOT_STATUS	0x81U

/* The class request that aborts a command on the control pipe. */
#define CCID_REQUEST_ABORT		0x01U

/* dwFeatures: the reader chooses the voltage; it exchanges short, or short and extended, APDUs. */
#define CCID_FEATURE_AUTO_VOLTAGE	0x00000008U
#define CCID_FEATURE_EXCHANGE_MASK	0x00070000U
#define CCID_FEATURE_SHORT_APDU		0x00020000U
#define CCID_FEATURE_EXTENDED_APDU	0x00040000U

/* bStatus: the card's state (bits 0-1) and the command's (bits 6-7). */
#define CCID_ICC_ACTIVE			0U
#define CCID_ICC_INACTIVE		1U
#define CCID_ICC_ABSENT			2U
#define CCID_COMMAND_OK			0U
#define CCID_COMMAND_FAILED		1U
#define CCID_COMMAND_TIME_EXTENSION	2U

/* bChainParameter of an answer, and wLevelParameter of the request for its next part. */
#define CCID_CHAIN_COMPLETE		0x00U
#define CCID_CHAIN_BEGINS		0x01U
#define CCID_CHAIN_ENDS			0x02U
#define CCID_CHAIN_CONTINUES		0x03U
#define CCID_CHAIN_EMPTY		0x10U
#define CCID_LEVEL_CONTINUE		0x0010U

/* An APDU exchange level of a reader: none (TPDU or characters), short APDUs, short and extended. */
#define CCID_LEVEL_NONE			0
#define CCID_LEVEL_SHORT		1
#define CCID_LEVEL_EXTENDED		2

/* What the driver takes from a reader's class descriptor. */
struct ccid_class {
	uint16_t version;
	uint8_t max_slot_index;
	uint8_t max_busy_slots;
	uint32_t protocols;
	uint32_t features;
	uint32_t max_message;
};

/* One answer of a reader: its header's fields and its data (length bytes after the header). */
struct ccid_reply {
	uint8_t type;
	uint32_t length;
	uint8_t slot;
	uint8_t sequence;
	uint8_t status;
	uint8_t error;
	uint8_t parameter;
	const uint8_t *data;
};

int drv_usb_ccid_driver_register(void);
int drv_ccid_parse_class(const uint8_t *descriptor, size_t length, struct ccid_class *result);
int drv_ccid_level(uint32_t features);
void drv_ccid_header(uint8_t *message, uint8_t type, uint32_t length, uint8_t slot, uint8_t sequence, uint8_t parameter0, uint8_t parameter1, uint8_t parameter2);
int drv_ccid_parse_reply(const uint8_t *message, size_t size, struct ccid_reply *reply);

#endif
