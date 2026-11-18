/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The OHCI driver's transfer descriptors: how one transfer is cut into
 * general transfer descriptors, and how the descriptors the controller
 * retired tell how the transfer ended.  Pure computation on descriptor
 * words in the processor's byte order, so that it can be tested on the
 * host; the driver converts the words to the controller's little-endian
 * memory form.
 */

#ifndef KERN_DRIVERS_PCI_OHCI_TD_H
#define KERN_DRIVERS_PCI_OHCI_TD_H

#include <stddef.h>
#include <stdint.h>

/* The bytes one descriptor serves: a page, so its buffer never crosses into a second. */
#define OHCI_TD_DATA_MAX		4096U

/* The descriptors one transfer may take, the setup and status stages included. */
#define OHCI_TD_MAX			66U

/* The size and alignment of an endpoint or a transfer descriptor. */
#define OHCI_DESCRIPTOR_SIZE		16U

/* A transfer descriptor's control word. */
#define OHCI_TD_ROUNDING		0x00040000U
#define OHCI_TD_PID_SETUP		0x00000000U
#define OHCI_TD_PID_OUT			0x00080000U
#define OHCI_TD_PID_IN			0x00100000U
#define OHCI_TD_DELAY_NONE		0x00e00000U
#define OHCI_TD_TOGGLE_CARRY		0x00000000U
#define OHCI_TD_TOGGLE_DATA0		0x02000000U
#define OHCI_TD_TOGGLE_DATA1		0x03000000U
#define OHCI_TD_CC_SHIFT		28U

/* The condition codes the controller writes into a descriptor it retires. */
#define OHCI_CC_NO_ERROR		0x0U
#define OHCI_CC_STALL			0x4U
#define OHCI_CC_DATA_UNDERRUN		0x9U
#define OHCI_CC_NOT_ACCESSED		0xfU
#define OHCI_CC_NOT_ACCESSED_ALSO	0xeU

/* An endpoint descriptor's control word. */
#define OHCI_ED_DIRECTION_TD		0x00000000U
#define OHCI_ED_DIRECTION_OUT		0x00000800U
#define OHCI_ED_DIRECTION_IN		0x00001000U
#define OHCI_ED_LOW_SPEED		0x00002000U
#define OHCI_ED_SKIP			0x00004000U
#define OHCI_ED_ENDPOINT_SHIFT		7U
#define OHCI_ED_PACKET_SHIFT		16U

/* An endpoint descriptor's head pointer: halted, and the data toggle carried between transfers. */
#define OHCI_ED_HEAD_HALTED		0x1U
#define OHCI_ED_HEAD_TOGGLE		0x2U
#define OHCI_ED_POINTER_MASK		0xfffffff0U

/* What a transfer descriptor holds: the control word, the buffer's start and end, and the next one. */
struct ohci_td_words {
	uint32_t control;
	uint32_t buffer;
	uint32_t next;
	uint32_t end;
};

/*
 * One transfer as it is cut into descriptors: a control transfer (its
 * setup packet at setup_address, eight bytes) or a bulk or interrupt
 * one; its data in one buffer from data_address, which is page-aligned;
 * and where in bus memory the descriptors lie, one after the other
 * from td_address, the one after the last being the endpoint's empty
 * tail.
 */
struct ohci_transfer_plan {
	int control;
	int input;
	int zero_packet;
	size_t length;
	uint32_t setup_address;
	uint32_t data_address;
	uint32_t td_address;
};

/* How a transfer ended. */
enum ohci_transfer_result {
	OHCI_TRANSFER_PENDING,
	OHCI_TRANSFER_COMPLETE,
	OHCI_TRANSFER_STALL,
	OHCI_TRANSFER_ERROR
};

unsigned
ohci_td_count(
	const struct ohci_transfer_plan *plan);

int
ohci_td_build(
	const struct ohci_transfer_plan *plan,
	struct ohci_td_words *tds,
	unsigned capacity,
	unsigned *count);

uint32_t
ohci_ed_control(
	unsigned address,
	unsigned endpoint,
	int control,
	int input,
	int low_speed,
	unsigned packet);

enum ohci_transfer_result
ohci_td_outcome(
	const struct ohci_transfer_plan *plan,
	const struct ohci_td_words *tds,
	unsigned count,
	uint32_t head,
	size_t *actual,
	unsigned *condition);

#endif
