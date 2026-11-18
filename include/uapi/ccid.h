/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smart card slots: /dev/smartcardN (ws161-p003, the user's approval U2
 * of 2026-10-05; docs/reference/security-keys.md).
 *
 * Every slot of a USB CCID reader that exchanges whole APDUs is a node:
 * an NFC reader's contactless slot (the reader frames ISO 14443-4 for the
 * card), its SAM slot, a security key's CCID interface.  Any number of
 * opens may read the status and the card events; CCID_POWER_ON claims the
 * slot for one open file, which alone may then send APDUs, until
 * CCID_POWER_OFF or its last close (which powers the card off).
 *
 *   read()   struct ccid_event records, whole (a shorter buffer: EINVAL);
 *            an empty queue waits, or EAGAIN for O_NONBLOCK.
 *   poll()   POLLIN while a record waits, POLLHUP once the reader is gone.
 *
 * The request numbers and the structures are zedBSD's own.
 */

#ifndef KERN_UAPI_CCID_H
#define KERN_UAPI_CCID_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define KERN_CCID_IOC_GROUP	'S'

/* The longest ATR, a name's room, the records an open keeps, and the longest APDU either way. */
#define CCID_ATR_MAX		33U
#define CCID_TEXT_MAX		64U
#define CCID_EVENT_QUEUE	16U
#define CCID_APDU_MAX		65544U

/* The default and the longest wait of CCID_TRANSMIT, in milliseconds. */
#define CCID_TRANSMIT_DEFAULT_MS	30000U
#define CCID_TRANSMIT_MAX_MS		120000U

/* Whether a card is in the slot, and whether it is powered. */
#define CCID_CARD_ABSENT	0U
#define CCID_CARD_PRESENT	1U
#define CCID_CARD_POWERED	2U

/* What a ccid_event says. */
#define CCID_EVENT_INSERTED	1U
#define CCID_EVENT_REMOVED	2U

/* ccid_info flags: the reader takes extended APDUs; this open holds the slot's claim. */
#define CCID_INFO_EXTENDED_APDU	0x0001U
#define CCID_INFO_CLAIMED	0x0002U

/*
 * What a slot is: the reader's identity, the interface, this slot and the
 * reader's slot count, the class descriptor's dwFeatures, dwProtocols and
 * dwMaxCCIDMessageLength, the longest command and answer this slot takes,
 * the CCID_INFO_* flags, and the product's name.
 */
struct ccid_info {
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	uint16_t interface_number;
	uint8_t slot;
	uint8_t slot_count;
	uint16_t reserved0;
	uint32_t features;
	uint32_t protocols;
	uint32_t max_message;
	uint32_t max_command;
	uint32_t max_response;
	uint32_t flags;
	uint32_t reserved[4];
	char name[CCID_TEXT_MAX];
};

/*
 * A slot's state: CCID_CARD_*, the ATR (atr_size bytes, while powered),
 * and the change count, which grows at every insertion and removal.
 */
struct ccid_status {
	uint32_t state;
	uint32_t atr_size;
	uint8_t atr[CCID_ATR_MAX];
	uint8_t reserved0[3];
	uint32_t changes;
	uint32_t reserved[3];
};

/*
 * One APDU exchange: the command (command_size bytes at command), where
 * the answer goes (response_capacity bytes at response), the wait
 * (timeout_ms, 0 for CCID_TRANSMIT_DEFAULT_MS), and, filled by the kernel,
 * the answer's size with its status word.  The pointers are 64 bits wide
 * so every program passes the same structure.
 */
struct ccid_transmit {
	uint64_t command;
	uint64_t response;
	uint32_t command_size;
	uint32_t response_capacity;
	uint32_t response_size;
	uint32_t timeout_ms;
	uint32_t reserved[4];
};

/* A card came (CCID_EVENT_INSERTED) or went (CCID_EVENT_REMOVED); changes is the slot's count after it. */
struct ccid_event {
	uint32_t kind;
	uint32_t changes;
};

#define CCID_GET_INFO		_IOR(KERN_CCID_IOC_GROUP, 0, struct ccid_info)
#define CCID_GET_STATUS		_IOR(KERN_CCID_IOC_GROUP, 1, struct ccid_status)
#define CCID_POWER_ON		_IOR(KERN_CCID_IOC_GROUP, 2, struct ccid_status)
#define CCID_POWER_OFF		_IO(KERN_CCID_IOC_GROUP, 3)
#define CCID_TRANSMIT		_IOWR(KERN_CCID_IOC_GROUP, 4, struct ccid_transmit)

#endif
