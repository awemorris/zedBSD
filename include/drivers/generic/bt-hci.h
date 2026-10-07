/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth controllers' HCI class (ws143-p002; the node's interface
 * is <uapi/bluetooth.h>).
 *
 * A transport (usb-bt) registers a controller with what it is and its
 * operations, and hands every event and ACL packet it has put together to
 * drv_bt_hci_input().  The class publishes btN, keeps the packets in two
 * queues (events and ACL, so that a flood of data does not starve the
 * events), and answers the file's calls.  drv_bt_hci_input() refuses
 * a packet its queue has no room for; the transport then keeps the
 * packet, stops taking from the controller, and tries again when the
 * class calls the room operation after a read has made room.
 *
 * Its pure part (bt-hci-proto.c: the queues' ring, the reassembly of a
 * received byte stream into packets, the checks of a written packet)
 * builds in the host test as it is.
 */

#ifndef DRIVERS_GENERIC_BT_HCI_H
#define DRIVERS_GENERIC_BT_HCI_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/bluetooth.h>

struct drv_bt_hci;

/* The most controllers published at once. */
#define DRV_BT_HCI_MAX			16U

/* The bytes a record takes in a ring besides its body: length (2), type (1), sequence (4). */
#define BT_HCI_RECORD_HEADER		7U

/* The ways a received stream is cut into packets: by an event's header, or by an ACL packet's. */
#define BT_HCI_ASSEMBLE_EVENT		0U
#define BT_HCI_ASSEMBLE_ACL		1U

/* The headers of an event and of an ACL packet, without the type's byte. */
#define BT_HCI_EVENT_HEADER		2U
#define BT_HCI_ACL_HEADER		4U

/* The Intel bootloader's Secure Send command, which goes on the bulk OUT pipe. */
#define BT_HCI_OPCODE_INTEL_SECURE_SEND	0xfc09U

/*
 * One queue of packets: records laid one after another in a ring of
 * capacity bytes.  head is where the oldest record starts, used how many
 * bytes the records take, count how many there are.  reserve bytes are
 * kept for the kernel's notices, which must never be refused for room.
 */
struct bt_hci_ring {
	uint8_t *bytes;
	size_t capacity;
	size_t reserve;
	size_t head;
	size_t used;
	unsigned count;
};

/*
 * The reassembly of one received stream into packets.
 *
 * A USB transfer may end in the middle of a packet or hold several, so the
 * bytes are gathered in buffer until the header's length is complete.
 * have is how many are gathered; acl_data_max bounds the ACL packets, and
 * one longer than it makes the rest of its transfer malformed.  A whole
 * packet the class refused stays gathered (have equals its length) until
 * it is delivered.
 */
struct bt_hci_assembler {
	uint8_t *buffer;
	size_t capacity;
	size_t have;
	unsigned mode;
	size_t acl_data_max;
};

/*
 * Takes one packet the assembler has put together: its type
 * (BT_PACKET_EVENT or BT_PACKET_ACL) and its bytes, header first, without
 * the type's byte.  Returns 0, or an errno value that stops the feed.
 */
typedef int (*bt_hci_packet_fn)(void *context, uint8_t type, const uint8_t *packet, size_t length);

/*
 * What a transport does for its controller.
 *
 * send sends one checked H4 packet (the type's byte first) and returns
 * when the controller has taken it; set_bootloader turns the Intel
 * bootloader's path on or off; reset resets the controller in place.
 * These three run in the calling process's context, one at a time, and
 * may sleep.  room tells the transport that a read has made room in a
 * queue; it is called with no lock held and must not sleep.
 */
struct drv_bt_hci_ops {
	int (*send)(void *context, const uint8_t *packet, size_t length);
	int (*set_bootloader)(void *context, int on);
	int (*reset)(void *context);
	void (*room)(void *context);
};

/* What a transport says of its controller at registration. */
struct drv_bt_hci_description {
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
	uint16_t bus;
	const char *name;
	const char *physical_path;
};

/* The counts a transport keeps and the class gives out (struct bt_stats). */
struct drv_bt_hci_counts {
	uint64_t malformed;
	uint64_t stalls;
};

int drv_bt_hci_register(const struct drv_bt_hci_description *description, const struct drv_bt_hci_ops *ops, void *context, struct drv_bt_hci **result);
int drv_bt_hci_input(struct drv_bt_hci *hci, uint8_t type, const uint8_t *packet, size_t length);
void drv_bt_hci_count(struct drv_bt_hci *hci, const struct drv_bt_hci_counts *counts);
void drv_bt_hci_notice(struct drv_bt_hci *hci, uint8_t type);
void drv_bt_hci_withdraw(struct drv_bt_hci *hci);
void drv_bt_hci_release(struct drv_bt_hci *hci);

void bt_hci_ring_init(struct bt_hci_ring *ring, uint8_t *bytes, size_t capacity, size_t reserve);
size_t bt_hci_ring_room(const struct bt_hci_ring *ring);
int bt_hci_ring_push(struct bt_hci_ring *ring, uint8_t type, uint32_t sequence, const uint8_t *body, size_t length, int notice);
int bt_hci_ring_peek(const struct bt_hci_ring *ring, uint8_t *type, uint32_t *sequence, size_t *length);
size_t bt_hci_ring_copy(const struct bt_hci_ring *ring, uint8_t *target);
void bt_hci_ring_pop(struct bt_hci_ring *ring);
void bt_hci_ring_clear(struct bt_hci_ring *ring);

void bt_hci_assembler_init(struct bt_hci_assembler *assembler, uint8_t *buffer, size_t capacity, unsigned mode, size_t acl_data_max);
void bt_hci_assembler_restart(struct bt_hci_assembler *assembler, unsigned mode, size_t acl_data_max);
int bt_hci_assembler_feed(struct bt_hci_assembler *assembler, const uint8_t *data, size_t size, bt_hci_packet_fn deliver, void *context, size_t *consumed);

int bt_hci_check_write(const uint8_t *packet, size_t size, size_t acl_data_max);
int bt_hci_is_secure_send(const uint8_t *packet, size_t size);
size_t bt_hci_assembly_capacity(unsigned mode);

#endif
