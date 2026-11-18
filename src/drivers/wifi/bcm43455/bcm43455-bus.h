/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the bus to the running firmware and its thread.
 *
 * Once the firmware runs, one thread owns the SDIO bus: it waits for the
 * card's interrupt (or polls every 100 ms in case one was missed), reads
 * the SDIO core's interrupt status and the firmware's mailbox, reads every
 * frame the firmware has, hands events and data to the device's hooks and
 * control answers to whoever asked, and sends control requests and data
 * frames within the firmware's credit.
 *
 * Before the thread starts and after it stops, the lifecycle owner runs
 * the same code directly ("direct mode", bcm43455_bus_direct()): bring-up
 * and power-down send their commands themselves.  Nothing else may use
 * the bus then.
 *
 * The device's radio work (a scan's start, a connection's commands) runs on
 * the bus thread itself, through the work hook: the thread sends those
 * commands and reads what the chip answers in the meantime, so nobody
 * else waits for them.
 *
 * Locks: control_lock (a mutex) admits one control request at a time; it
 * is held while the requester waits.  lock (an IRQ-saving spinlock, a
 * leaf) guards the request slot and the thread's state.  The bus thread
 * never takes control_lock or any lock of the device, so a requester that
 * holds them while it waits cannot deadlock it; the hooks it calls may
 * take only spinlocks and must not wait.
 */

#ifndef KERN_DRIVERS_BCM43455_BUS_H
#define KERN_DRIVERS_BCM43455_BUS_H

#include <stddef.h>
#include <stdint.h>

#include <kern/lock.h>
#include <kern/waitq.h>

#include "drivers/wifi/bcm43455/bcm43455-bcdc.h"
#include "drivers/wifi/bcm43455/bcm43455-chip.h"
#include "drivers/wifi/bcm43455/bcm43455-event.h"
#include "drivers/wifi/bcm43455/bcm43455-sdpcm.h"

/*
 * The most bytes one received frame may take; a longer one is read and dropped.
 */
#define BCM43455_BUS_RX_MAX		4096U

/*
 * The most bytes one sent frame takes on the bus, header and padding included.
 */
#define BCM43455_BUS_TX_MAX		4096U

/*
 * The longest Ethernet frame the data channel carries, without its BCDC header.
 */
#define BCM43455_BUS_ETHERNET_MAX	1600U

/* How long one control request may wait for its answer, in milliseconds. */
#define BCM43455_BUS_CONTROL_TIMEOUT_MS	500U

/* How often the bus thread looks at the chip without an interrupt. */
#define BCM43455_BUS_POLL_MS		100U

/*
 * What the device does with what the bus receives.
 *
 * The first four run on whoever runs the bus (the bus thread, or the
 * lifecycle owner in direct mode) and must not wait: they copy what they
 * keep into the device's own rings under a spinlock.  event gets the
 * decoded event and its Ethernet frame; data gets a received Ethernet
 * frame.  transmit copies the next frame to send into frame and returns
 * 1, or returns 0 when there is none.  failed reports that the bus thread
 * stopped on an error of the bus or the firmware.
 *
 * work runs on the bus thread only, after bcm43455_bus_request_work():
 * it does the device's queued radio work with
 * bcm43455_bus_thread_control() and returns 0, or the bus's error, which
 * stops the thread.  It may take only spinlocks, never a mutex.
 */
struct bcm43455_bus_hooks {
	void (*event)(void *context, const struct bcm43455_event *event, const uint8_t *frame, size_t length);
	void (*data)(void *context, const uint8_t *frame, size_t length);
	int (*transmit)(void *context, uint8_t *frame, size_t capacity, size_t *length);
	void (*failed)(void *context, int error);
	int (*work)(void *context);
};

/*
 * The states of the one control request slot.
 *
 * EMPTY: free.  QUEUED: a requester filled it and waits; the thread has
 * not sent it.  SENT: the thread sent it and waits for its answer.
 * ANSWERED: the thread copied the answer in; the requester empties it.
 * ABANDONED: the requester gave up after it was sent; the thread empties
 * it when the answer comes, or the next request reuses it.
 */
enum bcm43455_slot_state {
	BCM43455_SLOT_EMPTY = 0,
	BCM43455_SLOT_QUEUED,
	BCM43455_SLOT_SENT,
	BCM43455_SLOT_ANSWERED,
	BCM43455_SLOT_ABANDONED
};

/*
 * One control request and its answer, as the requester and the thread hand
 * it over.  Both buffers are cleared as soon as their bytes have moved on,
 * because a request may carry a key.
 */
struct bcm43455_control_slot {
	enum bcm43455_slot_state state;
	uint16_t id;
	uint32_t command;
	unsigned set;
	size_t request_length;
	size_t answer_capacity;
	size_t answer_length;
	int error;
	uint8_t request[BCM43455_BCDC_CONTROL_MAX];
	uint8_t answer[BCM43455_BCDC_CONTROL_MAX];
};

/*
 * A control request run in direct mode, which the receive path completes.
 */
struct bcm43455_direct_request {
	unsigned active;
	unsigned done;
	uint16_t id;
	uint32_t command;
	uint8_t *answer;
	size_t answer_capacity;
	size_t answer_length;
	int error;
};

/* What the bus has seen, for the log and the tests. */
struct bcm43455_bus_counters {
	uint64_t interrupts;
	uint64_t polls;
	uint64_t frames_in;
	uint64_t frames_out;
	uint64_t control_in;
	uint64_t events_in;
	uint64_t data_in;
	uint64_t data_out;
	uint64_t dropped;
	uint64_t glom_frames;
	uint64_t bad_frames;
	uint64_t stale_answers;
};

/*
 * The bus to one chip.
 *
 * chip, the SDPCM state, both frame buffers, direct and counters belong to
 * whoever runs the bus: the bus thread while thread_running is set, else
 * the lifecycle owner in direct mode.  Everything under lock is shared.
 */
struct bcm43455_bus {
	struct bcm43455_chip chip;
	const struct bcm43455_bus_hooks *hooks;
	void *hook_context;

	uint8_t tx_sequence;
	uint8_t tx_credit_limit;
	uint8_t flow_control;
	unsigned firmware_protocol;
	uint16_t next_id;
	struct bcm43455_direct_request direct;
	struct bcm43455_bus_counters counters;
	uint8_t rx_frame[BCM43455_BUS_RX_MAX];
	uint8_t tx_frame[BCM43455_BUS_TX_MAX];

	struct mutex control_lock;
	unsigned direct_allowed;

	struct spinlock lock;
	struct wait_queue answer_wait;
	struct wait_queue finished_wait;
	unsigned thread_running;
	unsigned stop_requested;
	unsigned work_requested;
	unsigned radio_work_pending;
	int thread_error;
	struct bcm43455_control_slot slot;
};

/*
 * Prepares a bus whose chip is attached later.  The hooks and their
 * context must outlive the bus.
 */
int bcm43455_bus_init(struct bcm43455_bus *bus, const struct bcm43455_bus_hooks *hooks, void *hook_context);

/*
 * Forgets the SDPCM state of a previous firmware run, before a new one.
 */
void bcm43455_bus_forget(struct bcm43455_bus *bus);

/*
 * Allows (1) or forbids (0) direct mode.  The lifecycle owner allows it
 * for bring-up and power-down while the thread does not run.
 */
void bcm43455_bus_direct(struct bcm43455_bus *bus, int allowed);

/*
 * Sends one control command and waits for its answer.
 *
 * For a set, input is the payload.  For a get, input starts the payload
 * (an iovar's name) and answer_capacity bytes of answer are asked for;
 * answer_length receives how many came.  Returns ENETDOWN when the bus
 * neither runs its thread nor allows direct mode, ETIMEDOUT without an
 * answer in BCM43455_BUS_CONTROL_TIMEOUT_MS, EIO for an answer the
 * firmware marks failed, and the bus's error otherwise.  Thread context;
 * never called from the bus's hooks.
 */
int bcm43455_bus_control(struct bcm43455_bus *bus, uint32_t command, int set, const uint8_t *input, size_t input_length, uint8_t *answer, size_t answer_capacity, size_t *answer_length);

/* Reads an iovar into answer. */
int bcm43455_bus_iovar_get(struct bcm43455_bus *bus, const char *name, uint8_t *answer, size_t capacity, size_t *answer_length);

/* Sets an iovar to value. */
int bcm43455_bus_iovar_set(struct bcm43455_bus *bus, const char *name, const void *value, size_t length);

/* Sets an iovar that holds one 32-bit number. */
int bcm43455_bus_iovar_set_u32(struct bcm43455_bus *bus, const char *name, uint32_t value);

/*
 * Starts the bus thread, which takes the bus from direct mode.
 *
 * Returns the kernel's error when the thread cannot be made.
 */
int bcm43455_bus_thread_start(struct bcm43455_bus *bus);

/*
 * Stops the bus thread and waits until it has let the bus go, which
 * returns the bus to its owner for direct mode.  Returns the error the
 * thread stopped on, 0 for a stop on request.
 */
int bcm43455_bus_thread_stop(struct bcm43455_bus *bus);

/*
 * Tells the bus thread that the device has a frame to send.  Any context
 * but an interrupt handler.
 */
void bcm43455_bus_wake(struct bcm43455_bus *bus);

/*
 * Asks the bus thread to run the work hook.  Requests made before the
 * hook starts are one run; a request made while it runs makes another.
 * Any context but an interrupt handler.
 */
void bcm43455_bus_request_work(struct bcm43455_bus *bus);

/*
 * Sends one control command from the work hook and waits for its answer,
 * reading what the chip sends meanwhile.
 *
 * Only the bus thread calls this, from its work hook.  The return value
 * is the bus's own error (the thread must stop); outcome receives the
 * command's: 0 when answered, ETIMEDOUT without an answer in
 * BCM43455_BUS_CONTROL_TIMEOUT_MS, ECANCELED when the thread is asked to
 * stop meanwhile, EIO or EOPNOTSUPP when the firmware refused it.  answer
 * and answer_length are as for bcm43455_bus_control().
 */
int bcm43455_bus_thread_control(struct bcm43455_bus *bus, uint32_t command, int set, const uint8_t *input, size_t input_length, uint8_t *answer, size_t answer_capacity, size_t *answer_length, int *outcome);

#endif
