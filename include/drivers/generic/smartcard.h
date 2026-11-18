/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The smart card slots' class (ws161-p003; the node's interface is
 * <uapi/ccid.h>, docs/reference/security-keys.md).
 *
 * A transport (usb-ccid, or the test kernel's loopback card) registers a
 * slot with what it is and three operations, and tells the class when a
 * card comes or goes with drv_smartcard_card().  The class publishes
 * smartcardN, keeps the claim (the one open file that may power the card
 * and send it APDUs), the events of each open and the slot's state, and
 * calls the operations one at a time for the slot.  drv_smartcard_unregister()
 * takes the transport away: no operation runs after it returns, and the
 * files still open answer ENODEV.
 */

#ifndef DRIVERS_GENERIC_SMARTCARD_H
#define DRIVERS_GENERIC_SMARTCARD_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/ccid.h>

struct drv_smartcard;

/* The most slots published at once. */
#define DRV_SMARTCARD_MAX	16U

/*
 * What a transport does for a slot, in the calling process's context
 * (they may sleep), each by deadline_ms (clock_milliseconds()):
 *
 *   power_on   powers the card and gives its ATR (at most CCID_ATR_MAX
 *              bytes); ENXIO when no card is there
 *   power_off  powers it off
 *   transmit   sends one command APDU and gives the answer with its status
 *              word; EMSGSIZE for an answer longer than capacity, ENXIO
 *              when the card went, ETIMEDOUT at the deadline (the command
 *              is then aborted at the reader)
 */
struct drv_smartcard_ops {
	int (*power_on)(void *context, unsigned slot, uint8_t *atr, size_t *atr_size, uint64_t deadline_ms);
	int (*power_off)(void *context, unsigned slot);
	int (*transmit)(void *context, unsigned slot, const uint8_t *command, size_t command_size,
	    uint8_t *response, size_t capacity, size_t *response_size, uint64_t deadline_ms);
};

/*
 * What a transport says of a slot at registration: the information the
 * requests give out (the slot's number among its reader's in info.slot)
 * and whether a card is there now.
 */
struct drv_smartcard_description {
	struct ccid_info info;
	int present;
};

int drv_smartcard_register(const struct drv_smartcard_description *description, const struct drv_smartcard_ops *ops, void *context, struct drv_smartcard **result);
void drv_smartcard_card(struct drv_smartcard *smartcard, int present);
void drv_smartcard_unregister(struct drv_smartcard *smartcard);

/* The test kernel's loopback card (CONFIG_SECURITY_KEY_TEST_LOOPBACK, smartcard-loopback.c). */
int drv_smartcard_loopback_register(void);

#endif
