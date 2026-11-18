/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The parts of bluetoothd's ACL data path without system calls
 * (ws143-p004): an ACL packet taken apart and built, an L2CAP frame put
 * together from the packets of one connection, and a frame cut into the
 * packets the controller takes.  The host tests build them.
 */

#ifndef BLUETOOTHD_ACL_H
#define BLUETOOTHD_ACL_H

#include <stddef.h>
#include <stdint.h>

/* The headers: ACL (handle and flags, length) and L2CAP (length, channel). */
#define BTD_ACL_HEADER		4U
#define BTD_L2CAP_HEADER	4U

/* The longest L2CAP payload bluetoothd puts together; a longer frame is dropped. */
#define BTD_L2CAP_MAX		1024U

/* The packet boundary flags: a first packet the controller does not flush, a continuing one. */
#define BTD_ACL_FIRST		0x00U
#define BTD_ACL_CONTINUING	0x01U
#define BTD_ACL_FIRST_FLUSHABLE	0x02U

/* One ACL packet taken apart: its connection, its boundary flag, and its data (pointing into the packet). */
struct btd_acl {
	uint16_t handle;
	uint8_t boundary;
	const uint8_t *data;
	size_t length;
};

/*
 * The L2CAP frame being put together on one connection: how long it will
 * be (its header and payload), what has come, and how many frames were
 * dropped.  It lives in the connection's record.
 */
struct btd_reassembly {
	int active;
	size_t expected;
	size_t used;
	unsigned dropped;
	uint8_t frame[BTD_L2CAP_HEADER + BTD_L2CAP_MAX];
};

int btd_acl_parse(const uint8_t *packet, size_t length, struct btd_acl *acl);
size_t btd_acl_build(uint8_t *packet, size_t size, uint16_t handle, uint8_t boundary, const uint8_t *data, size_t length);
int btd_reassembly_feed(struct btd_reassembly *reassembly, const struct btd_acl *acl);
size_t btd_l2cap_frame(uint8_t *frame, size_t size, uint16_t cid, const uint8_t *payload, size_t length);

#endif
