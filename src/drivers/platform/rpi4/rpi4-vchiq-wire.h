/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shared-memory layout of VCHIQ, the message queue between the ARM and
 * the VideoCore's VPU on the Raspberry Pi 4.
 *
 * The ARM is the slave side: it owns the memory, writes the first
 * slot (slot zero) and hands its bus address to the firmware, which then
 * acts as the master.  Each side writes messages into its own 4 KiB slots,
 * advances its tx_pos and signals the other side's trigger; the reader hands
 * a slot back through the writer's slot queue once it has read it.
 *
 * This file holds what does not touch hardware, so a host test can run it
 * against a simulated master: the layout, message framing, the slot
 * arithmetic and the bulk page lists.
 */

#ifndef KERN_DRIVERS_RPI4_VCHIQ_WIRE_H
#define KERN_DRIVERS_RPI4_VCHIQ_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * The shared memory's constants and message types.
 *
 * Values referenced: FreeBSD sys/contrib/vchiq (the VCHIQ shared-memory
 * layout and message codes).
 */

/* "VCHI" read as a big-endian four-character code: the slot zero's magic. */
#define DRV_RPI4_VCHIQ_MAGIC			0x56434849U


/* The protocol version the ARM speaks, and the oldest it accepts. */
#define DRV_RPI4_VCHIQ_VERSION			8U
#define DRV_RPI4_VCHIQ_VERSION_MIN		3U

/* The slot size and the slot counts both sides agree on. */
#define DRV_RPI4_VCHIQ_SLOT_SIZE		4096U
#define DRV_RPI4_VCHIQ_MAX_SLOTS		128U
#define DRV_RPI4_VCHIQ_MAX_SLOTS_PER_SIDE	64U

/*
 * The number of debug words at the end of each side's shared state.  The
 * firmware is built with the debug words, so they stay part of the layout
 * even though the ARM only writes the first one (the count).
 */
#define DRV_RPI4_VCHIQ_DEBUG_WORDS		11U

/* The data slots the ARM allocates after slot zero: half for each side. */
#define DRV_RPI4_VCHIQ_DATA_SLOTS		64U

/* The message header: the message id and the payload size, eight bytes. */
#define DRV_RPI4_VCHIQ_HEADER_BYTES		8U

/* The largest payload one message carries: a whole slot less its header. */
#define DRV_RPI4_VCHIQ_MAX_PAYLOAD		(DRV_RPI4_VCHIQ_SLOT_SIZE - DRV_RPI4_VCHIQ_HEADER_BYTES)

/* The message types, in the top byte of a message id. */
#define DRV_RPI4_VCHIQ_MSG_PADDING		0U
#define DRV_RPI4_VCHIQ_MSG_CONNECT		1U
#define DRV_RPI4_VCHIQ_MSG_OPEN			2U
#define DRV_RPI4_VCHIQ_MSG_OPENACK		3U
#define DRV_RPI4_VCHIQ_MSG_CLOSE		4U
#define DRV_RPI4_VCHIQ_MSG_DATA			5U
#define DRV_RPI4_VCHIQ_MSG_BULK_RX		6U
#define DRV_RPI4_VCHIQ_MSG_BULK_TX		7U
#define DRV_RPI4_VCHIQ_MSG_BULK_RX_DONE		8U
#define DRV_RPI4_VCHIQ_MSG_BULK_TX_DONE		9U
#define DRV_RPI4_VCHIQ_MSG_PAUSE		10U
#define DRV_RPI4_VCHIQ_MSG_RESUME		11U
#define DRV_RPI4_VCHIQ_MSG_REMOTE_USE		12U
#define DRV_RPI4_VCHIQ_MSG_REMOTE_RELEASE	13U
#define DRV_RPI4_VCHIQ_MSG_REMOTE_USE_ACTIVE	14U

/* The largest port number a message id carries (twelve bits). */
#define DRV_RPI4_VCHIQ_PORT_MASK		0xfffU

/* The page list types: the VPU reads the pages (WRITE) or writes them (READ). */
#define DRV_RPI4_VCHIQ_PAGELIST_WRITE		0U
#define DRV_RPI4_VCHIQ_PAGELIST_READ		1U

/* The page list's header bytes before its address words. */
#define DRV_RPI4_VCHIQ_PAGELIST_HEADER_BYTES	8U

/* The most pages one address word of the Pi 4's long bulk form covers. */
#define DRV_RPI4_VCHIQ_PAGELIST_RUN_PAGES	256U

/* The page size the page lists count in. */
#define DRV_RPI4_VCHIQ_PAGE_SIZE		4096U

/*
 * One side's event: the waiter sets armed before it sleeps, the signaller
 * sets fired and rings the waiter's doorbell only when armed.
 */
struct drv_rpi4_vchiq_event {
	uint32_t armed;
	uint32_t fired;
	uint32_t unused;
};

/*
 * One side's half of slot zero.  Its owner writes everything here but the
 * slot queue entries the other side recycles into it; both sides read it.
 */
struct drv_rpi4_vchiq_shared {
	uint32_t initialised;
	uint32_t slot_first;
	uint32_t slot_last;
	uint32_t slot_sync;
	struct drv_rpi4_vchiq_event trigger;
	uint32_t tx_pos;
	struct drv_rpi4_vchiq_event recycle;
	uint32_t slot_queue_recycle;
	struct drv_rpi4_vchiq_event sync_trigger;
	struct drv_rpi4_vchiq_event sync_release;
	uint32_t slot_queue[DRV_RPI4_VCHIQ_MAX_SLOTS_PER_SIDE];
	uint32_t debug[DRV_RPI4_VCHIQ_DEBUG_WORDS];
};

/*
 * The use and release counts of one slot, kept by the slot's reader: the
 * slot goes back to its writer when the two are equal.
 */
struct drv_rpi4_vchiq_slot_info {
	int16_t use_count;
	int16_t release_count;
};

/* The first slot: the configuration both sides check and both halves of the state. */
struct drv_rpi4_vchiq_slot_zero {
	uint32_t magic;
	uint16_t version;
	uint16_t version_min;
	uint32_t slot_zero_size;
	uint32_t slot_size;
	uint32_t max_slots;
	uint32_t max_slots_per_side;
	uint32_t platform_data[2];
	struct drv_rpi4_vchiq_shared master;
	struct drv_rpi4_vchiq_shared slave;
	struct drv_rpi4_vchiq_slot_info slots[DRV_RPI4_VCHIQ_MAX_SLOTS];
};

/*
 * The ARM's view of the message streams: where it writes next, how many of
 * its slots it may use, and where it reads the VPU's stream next.
 *
 * The positions are byte offsets into each side's endless stream of slots;
 * the slot queue index of a position is its slot count modulo the queue
 * length.  tx_slots counts the ARM's slots entered, compared with
 * slot_queue_available by difference, so the comparison survives the
 * position's wrap.  They wrap as 32-bit integers, which both sides compare only for
 * equality or by difference.  The caller serializes writers and readers.
 */
struct drv_rpi4_vchiq_ring {
	uint8_t *base;
	struct drv_rpi4_vchiq_slot_zero *zero;
	struct drv_rpi4_vchiq_shared *local;
	struct drv_rpi4_vchiq_shared *remote;
	uint32_t tx_pos;
	uint32_t tx_slots;
	uint32_t slot_queue_available;
	uint8_t *tx_data;
	uint32_t rx_pos;
	uint8_t *rx_data;
	uint32_t rx_slot;
};

/* Makes a message id from its type and its source and destination ports. */
uint32_t drv_rpi4_vchiq_msgid(uint32_t type, uint32_t source, uint32_t destination);

/* Reads the type of a message id. */
uint32_t drv_rpi4_vchiq_msgid_type(uint32_t msgid);

/* Reads the source port of a message id. */
uint32_t drv_rpi4_vchiq_msgid_source(uint32_t msgid);

/* Reads the destination port of a message id. */
uint32_t drv_rpi4_vchiq_msgid_destination(uint32_t msgid);

/* Reports the bytes a message of a payload size takes in a slot. */
uint32_t drv_rpi4_vchiq_stride(uint32_t size);

/* Reports the bytes the shared memory needs for the slots. */
size_t drv_rpi4_vchiq_slot_bytes(void);

/* Lays out slot zero and the ARM's half of it in zeroed memory. */
int drv_rpi4_vchiq_ring_init(struct drv_rpi4_vchiq_ring *ring, void *base, size_t bytes, uint32_t fragments_bus, uint32_t fragment_count);

/* Takes back the ARM's slots the VPU has finished reading. */
void drv_rpi4_vchiq_ring_recycle(struct drv_rpi4_vchiq_ring *ring);

/* Reserves room for one message and reports where its payload goes. */
int drv_rpi4_vchiq_ring_reserve(struct drv_rpi4_vchiq_ring *ring, uint32_t size, uint8_t **payload);

/* Publishes the message the last reservation made room for. */
void drv_rpi4_vchiq_ring_commit(struct drv_rpi4_vchiq_ring *ring, uint32_t msgid, uint32_t size);

/* Reports the VPU's next unread message. */
int drv_rpi4_vchiq_ring_next(struct drv_rpi4_vchiq_ring *ring, uint32_t *msgid, uint32_t *size, const uint8_t **payload);

/* Moves past the message just read, handing its slot back at the slot's end. */
bool drv_rpi4_vchiq_ring_advance(struct drv_rpi4_vchiq_ring *ring, uint32_t size);

/* Marks an event fired and reports whether its waiter must be woken. */
bool drv_rpi4_vchiq_event_fire(struct drv_rpi4_vchiq_event *event);

/* Builds the page list of one physically contiguous bulk buffer. */
int drv_rpi4_vchiq_pagelist(void *pagelist, size_t capacity, uint64_t physical, uint32_t bytes, uint16_t type, size_t *used);

#endif
