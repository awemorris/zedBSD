/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device's command processor: the ring, the fence
 * page, the gate, the submissions and their tickets, and the interrupt.
 *
 * The ring is host memory the driver alone writes; RING_WPTR is the
 * doorbell and both indices count dwords.  A packet never crosses the
 * end of the ring: the rest of the ring is filled with a NOP first.  A
 * submission is placed whole under cp_lock (in pieces at packet
 * boundaries, with a doorbell before each wait, when the ring is short of
 * room) and ends with a CMD_FENCE of the next 64-bit sequence.  A wait
 * for room ends only when RING_RPTR moves or a recovery begins: a
 * processor that stopped is the hang check's to find.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 6,
 * the ring, the doorbell and the fence; section 9, the interrupt;
 * Appendix A, the order of the start).
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <uapi/errno.h>

#include "ghci.h"
#include "ghci-cmd.h"

/* The interrupts that start a recovery, and every interrupt the handler takes. */
#define IRQ_RECOVER			(GHCI_IRQ_FAULT | GHCI_IRQ_WATCHDOG)
#define IRQ_TAKEN			(GHCI_IRQ_FENCE | IRQ_RECOVER)

/* Every bit of IRQ_STATUS, which the start clears. */
#define IRQ_ALL				0xffffffffU

static int liveness(struct ghci_device *device, int want_interrupt);
static void release_memory(struct ghci_device *device);
static int ring_room(struct ghci_device *device, uint32_t need, int log_entry);
static void ring_store(struct ghci_device *device, uint32_t index, uint32_t word);
static void retire_log(struct ghci_device *device);
static int packet_length(const uint32_t *packets, uint32_t left, uint32_t *length);
static void fault_ring_take(struct ghci_device *device, struct ghci_fault_record *record);
static void record_fault(struct ghci_device *device, uint32_t irq_status, uint64_t generation);
static void take_fence(struct ghci_device *device, uint32_t irq_status, uint64_t generation);
static enum ghci_ticket_state ticket_state(struct ghci_device *device, const struct ghci_ticket *ticket);
static void log_registers(struct ghci_device *device, const char *what);

/*
 * Starts the command processor: the ring, the fence page and
 * the capability block's buffer, the interrupt, bus mastering, RUN and
 * the first fence.  The watchdog is set once the capability block is
 * read (ghci_cp_set_watchdog()).
 *
 * Returns 0 with the ring running, or an error with the GPU reset, the
 * interrupt let go and the memory given back.
 */
int
ghci_cp_start(
	struct ghci_device *device)
{
	unsigned long irq;
	int error;

	/* The ring's size: the glue's, or the default; a power of two between 4 KiB and 16 MiB. */
	if (device->ring_bytes == 0U)
		device->ring_bytes = GHCI_RING_BYTES;
	if (device->ring_bytes < GHCI_RING_BYTES_MIN || device->ring_bytes > GHCI_RING_BYTES_MAX ||
	    (device->ring_bytes & (device->ring_bytes - 1U)) != 0U) {
		release_memory(device);
		return EINVAL;
	}

	/* The ring and the fence page, in host memory below the driver's limit. */
	error = ghci_memory_alloc(device, device->ring_bytes, 0U, &device->ring.memory);
	if (error != 0) {
		release_memory(device);
		return error;
	}
	error = ghci_memory_alloc(device, GHCI_FENCE_PAGE_BYTES, 0U, &device->fence_page);
	if (error != 0) {
		release_memory(device);
		return error;
	}

	/* The buffer CMD_QUERY_CAPS writes the capability block to, in the same memory. */
	error = ghci_memory_alloc(device, GHCI_CAPS_ANSWER_BYTES, 0U, &device->caps_answer);
	if (error != 0) {
		release_memory(device);
		return error;
	}
	device->ring.dwords = device->ring_bytes / 4U;
	device->ring.wptr = 0U;
	device->fence_placed = 0U;
	device->submit_first = 0U;
	device->submit_count = 0U;
	device->recovery_placing = 0;

	/* Session 0's record; its page table comes once the ring runs. */
	ghci_vm_init(device);

	/* The ring's place and size, written while the processor is idle. */
	ghci_write(device, GHCI_REG_RING_BASE_LO, (uint32_t)device->ring.memory->bus);
	ghci_write(device, GHCI_REG_RING_BASE_HI, (uint32_t)(device->ring.memory->bus >> 32));
	ghci_write(device, GHCI_REG_RING_SIZE, device->ring_bytes);
	ghci_write(device, GHCI_REG_RING_WPTR, 0U);

	/* The interrupt, taken only now that the reset has cleared what a kernel before this one left. */
	error = device->bus_ops->interrupt_take(device);
	if (error != 0) {
		release_memory(device);
		return error;
	}
	device->interrupt_taken = 1;

	/*
	 * The watchdog keeps its reset default until the capability block
	 * gives the core clock (ghci_cp_set_watchdog()); the halt and the
	 * reset wait their long defaults until then.
	 */
	device->wdt_cycles = ghci_read(device, GHCI_REG_WDT_CYCLES);
	device->halt_wait_ms = ghci_halt_wait_ms(device->caps.gclk_hz, device->wdt_cycles);
	device->reset_wait_ms = ghci_reset_wait_ms(device->caps.gclk_hz, device->wdt_cycles);

	/* The fences, the generations and the interrupts the handler takes, before any can come. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->irq_mask = IRQ_TAKEN;
	if (device->display.present)
		device->irq_mask |= GHCI_IRQ_DISPLAY;
	device->completed = 0U;
	device->issued = 0U;
	device->recovering = 0;
	device->fault_records = 0U;
	device->progress_tick = clock_ticks();

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Nothing pending, and the interrupts the driver takes (none on a polled device). */
	ghci_write(device, GHCI_REG_IRQ_STATUS, IRQ_ALL);
	ghci_cp_irq_enable(device);

	/* Bus mastering, unless the firmware left it on for a boot display. */
	if (!device->bus_master_found) {
		error = device->bus_ops->bus_master(device, 1);
		if (error != 0) {
			(void)ghci_cp_stop(device);
			return error;
		}
		device->bus_master_set = 1;
	}

	/* The ring runs, with the MMU off until the driver starts it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->state = GHCI_STATE_RUNNING;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	ghci_cp_run(device);

	/* One fence must be written, with its interrupt unless the device is polled. */
	error = liveness(device, !device->polled);
	if (error != 0) {
		log_registers(device, "first fence not written");
		(void)ghci_cp_stop(device);
		return EIO;
	}

	/* Succeeded: the ring runs. */
	return 0;
}

/*
 * Stops the command processor at the detach or after a failed start: it
 * halts, resets and stops, the interrupt and bus mastering go, the poll
 * thread leaves, and the memory is given back unless the GPU or a late
 * handler may still reach it.  The fence page waits for the flip records
 * the display controller still owes: one that stays due while bus
 * mastering stays on keeps the page in the quarantine.
 *
 * Returns 0, or EBUSY when the interrupt could not be let go (a handler
 * may still run): the memory is then kept and the caller keeps the
 * device; a later call tries again.
 */
int
ghci_cp_stop(
	struct ghci_device *device)
{
	struct ghci_allocation *record_page;
	enum ghci_device_state state;
	unsigned long irq;
	uint32_t ctrl;
	int reset_done;
	int records_done;
	int master_off;
	int released;
	int error;

	/* The device's life as the last recovery left it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	state = device->state;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A running processor halts first, between packets. */
	if (state == GHCI_STATE_RUNNING) {
		ctrl = ghci_read(device, GHCI_REG_CTRL);
		ghci_write(device, GHCI_REG_CTRL, ctrl | GHCI_CTRL_HALT);
		error = ghci_wait_halted(device);
		if (error != 0)
			kern_logf("ghci: %s: processor did not halt at the stop\n", device->name);
	}

	/* The reset stops every access to the ring and the page tables. */
	ghci_write(device, GHCI_REG_CTRL, GHCI_CTRL_RESET);
	error = ghci_wait_register(device, GHCI_REG_CTRL, GHCI_CTRL_RESET, 0U, device->reset_wait_ms);
	reset_done = 1;
	if (error != 0) {
		kern_logf("ghci: %s: reset at the stop did not end\n", device->name);
		reset_done = 0;
	}

	/* The display's flip records still due go to the fence page now that the processor is idle. */
	records_done = 1;
	error = ghci_display_records_settle(device);
	if (error != 0) {
		kern_logf("ghci: %s: a flip's record still due at the stop\n", device->name);
		records_done = 0;
	}

	/* No interrupt (the handler restores nothing from here), nothing running. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->irq_mask = 0U;
	device->state = GHCI_STATE_DETACHED;
	ghci_write(device, GHCI_REG_IRQ_ENABLE, 0U);

	spin_unlock_irqrestore(&device->irq_lock, irq);
	ghci_write(device, GHCI_REG_CTRL, 0U);

	/* The handler goes, so that a late interrupt does not reach the ring or the fence page. */
	released = 1;
	if (device->interrupt_taken) {
		error = device->bus_ops->interrupt_release(device);
		if (error == 0)
			device->interrupt_taken = 0;
		else
			released = 0;
	}

	/*
	 * Bus mastering goes if the driver turned it on, or if the reset did
	 * not end; one the firmware left on for a boot display stays, with
	 * the GPU reset and its ring stopped.
	 */
	master_off = 0;
	if (device->bus_master_set || !reset_done) {
		error = device->bus_ops->bus_master(device, 0);
		if (error == 0) {
			master_off = 1;
			device->bus_master_set = 0;
		}
	}

	/* The poll thread leaves before the fence page it reads goes. */
	ghci_poll_stop(device);

	/* A handler that may still run keeps the memory, and the device. */
	if (!released) {
		kern_logf("ghci: %s: interrupt not let go; the device is kept\n", device->name);
		return EBUSY;
	}

	/* A GPU that may still reach the memory keeps it (the device itself can go). */
	if (!reset_done && !master_off) {
		kern_logf("ghci: %s: GPU not stopped; its memory is kept\n", device->name);
		return 0;
	}

	/* A record that may still come while bus mastering stays on keeps the fence page out of the allocator. */
	record_page = NULL;
	if (!records_done && !master_off) {
		record_page = device->fence_page;
		device->fence_page = NULL;
	}

	/* The sessions' records and the memory go back. */
	ghci_vm_fini(device);
	release_memory(device);

	/* The fence page waits in the quarantine, never given back while the device lives. */
	if (record_page != NULL) {
		kern_logf("ghci: %s: fence page kept for a record still due\n", device->name);
		ghci_memory_quarantine(device, record_page);
	}

	/* Succeeded: the device holds nothing the GPU may reach. */
	return 0;
}

/*
 * Has the command processor write its capability block (CMD_QUERY_CAPS
 * and a fence after it) and copies the block into words, which hold
 * GHCI_CAPS_ANSWER_DWORDS.  The start calls it once, after the first
 * fence; the values are constants, so a recovery does not ask again.
 *
 * Returns 0 with the block copied, EIO when the command could not be
 * placed, or ETIMEDOUT when its fence did not pass; the buffer is then
 * left to the stop, which gives it back once the GPU is reset.
 */
int
ghci_cp_query_caps(
	struct ghci_device *device,
	uint32_t *words)
{
	volatile uint32_t *answer;
	struct ghci_ticket ticket;
	uint32_t packet[GHCI_DW_QUERY_CAPS];
	uint32_t written;
	uint32_t start;
	uint32_t index;
	int error;

	/* The command, for the block's bytes at the start of the buffer (the rest of it is not written). */
	written = ghci_cmd_query_caps(packet, device->caps_answer->bus, GHCI_CAPS_BLOCK_BYTES);
	if (written == 0U)
		return EIO;

	/* The command and the fence that follows its write, placed and rung under the ring's lock. */
	mutex_lock(&device->cp_lock);

	start = device->ring.wptr;
	error = ghci_cp_place(device, packet, written);
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
	if (error == 0)
		ghci_cp_doorbell(device);

	mutex_unlock(&device->cp_lock);
	if (error != 0)
		return EIO;

	/*
	 * The fence page must reach the fence; the block's write is visible
	 * by then, and the fence's read barrier keeps the copy below after it.
	 *
	 * Values referenced: amaterrace-gpu ea5d39c1 rtl/gpu/cp/atg_cp_seq.v
	 * (CMD_QUERY_CAPS writes the block through the fence unit's RESULT
	 * writes, which a later CMD_FENCE follows).
	 */
	error = ghci_cp_await_fence(device, ticket.seq, GHCI_LIVENESS_MS);
	if (error != 0) {
		/* Where the processor stopped: a GPU that does not know the command faults or stalls on it. */
		log_registers(device, "capability block's fence not written");
		return ETIMEDOUT;
	}

	/* Copies the block out of the uncached buffer. */
	answer = (volatile uint32_t *)(void *)device->caps_answer->view;
	for (index = 0U; index < GHCI_CAPS_ANSWER_DWORDS; index++)
		words[index] = answer[index];

	/* The buffer goes back; the device writes it no more. */
	error = ghci_memory_free(device, device->caps_answer);
	if (error != 0)
		ghci_memory_quarantine(device, device->caps_answer);
	device->caps_answer = NULL;

	/* Succeeded: the block is the caller's. */
	return 0;
}

/*
 * Sets the watchdog to GHCI_WATCHDOG_MS at the core clock of the
 * capability block, or leaves the reset default when the clock is not
 * known, and sizes the halt's and the reset's waits by it.  WDT_CYCLES
 * survives a reset, so a recovery keeps it.
 */
void
ghci_cp_set_watchdog(
	struct ghci_device *device)
{
	uint32_t cycles;

	/* The limit, written only when the clock is known. */
	cycles = ghci_watchdog_cycles(device->caps.gclk_hz, GHCI_WATCHDOG_MS);
	if (cycles != 0U)
		ghci_write(device, GHCI_REG_WDT_CYCLES, cycles);

	/* The limit the GPU holds, and the waits that follow from it. */
	device->wdt_cycles = ghci_read(device, GHCI_REG_WDT_CYCLES);
	device->halt_wait_ms = ghci_halt_wait_ms(device->caps.gclk_hz, device->wdt_cycles);
	device->reset_wait_ms = ghci_reset_wait_ms(device->caps.gclk_hz, device->wdt_cycles);
}

/*
 * Passes the gate of the ring: takes cp_lock unless a recovery runs or
 * the device is lost.
 *
 * held names the outer locks the caller holds (GHCI_HOLD_DEVICE,
 * GHCI_HOLD_DISPLAY).  OPEN: cp_lock is held, with the outer locks.
 * RETRY: a recovery ran or runs.  With wait, every lock the caller held
 * has been let go and the gate has waited for the recovery to end; the
 * caller starts again and reads its state again.  Without wait, only
 * cp_lock is let go: the caller keeps its outer locks, and with them the
 * state it read, and lets them go itself so that the recovery can take
 * them.  LOST: the device is lost; the locks go as for RETRY.
 */
enum ghci_gate
ghci_cp_enter(
	struct ghci_device *device,
	unsigned held,
	int wait)
{
	enum ghci_gate gate;
	unsigned long irq;
	uint64_t generation;
	uint64_t sequence;

	/* The ring's lock, and what the recovery says under the interrupt's. */
	mutex_lock(&device->cp_lock);
	irq = spin_lock_irqsave(&device->irq_lock);

	gate = GHCI_GATE_OPEN;
	if (device->state == GHCI_STATE_LOST)
		gate = GHCI_GATE_LOST;
	else if (device->recovering)
		gate = GHCI_GATE_RETRY;
	generation = device->gate_generation;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded at once: the ring is the caller's. */
	if (gate == GHCI_GATE_OPEN)
		return GHCI_GATE_OPEN;

	/* The ring's lock goes; a caller that does not wait keeps its own and is answered now. */
	mutex_unlock(&device->cp_lock);
	if (!wait)
		return gate;

	/* A caller that waits lets go of its locks, the inner one first, so that the recovery can take them. */
	if ((held & GHCI_HOLD_DISPLAY) != 0U)
		mutex_unlock(&device->display_lock);
	if ((held & GHCI_HOLD_DEVICE) != 0U)
		mutex_unlock(&device->lock);

	/* A lost device is answered now. */
	if (gate == GHCI_GATE_LOST)
		return gate;

	/* Waits for the gate to open again or the device to be lost. */
	irq = spin_lock_irqsave(&device->irq_lock);

	while (device->gate_generation == generation && device->state != GHCI_STATE_LOST) {
		sequence = waitq_sequence(&device->gate_wake);
		(void)waitq_sleep(&device->gate_wake, &device->irq_lock, sequence, 0U, 0U);
	}
	if (device->state == GHCI_STATE_LOST)
		gate = GHCI_GATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the caller starts again (or hears that the device is lost). */
	return gate;
}

/*
 * Passes the gate of the ring without waiting for anything: takes cp_lock
 * only when nobody holds it, no recovery runs, and the ring has room now
 * for need dwords (with the padding before its end) and for an entry of
 * the submission log, so that placing them and a fence of that size does
 * not wait.  The caller's own locks stay held whatever the answer.
 *
 * Returns OPEN with cp_lock held, RETRY when the ring is in use,
 * recovers or is full, or LOST for a lost device.
 */
enum ghci_gate
ghci_cp_try_enter(
	struct ghci_device *device,
	uint32_t need)
{
	enum ghci_gate gate;
	unsigned long irq;
	uint32_t padding;
	uint32_t rptr;
	int locked;
	int error;

	/* The ring's lock, only when it is free: its holder may be waiting for room. */
	locked = mutex_trylock(&device->cp_lock);
	if (!locked)
		return GHCI_GATE_RETRY;

	/* What the recovery says, under the interrupt's lock. */
	irq = spin_lock_irqsave(&device->irq_lock);

	gate = GHCI_GATE_OPEN;
	if (device->state == GHCI_STATE_LOST)
		gate = GHCI_GATE_LOST;
	else if (device->recovering)
		gate = GHCI_GATE_RETRY;

	spin_unlock_irqrestore(&device->irq_lock, irq);
	if (gate != GHCI_GATE_OPEN) {
		mutex_unlock(&device->cp_lock);
		return gate;
	}

	/* The submissions whose fences passed leave the log; the room is what the reader left now. */
	retire_log(device);
	rptr = ghci_read(device, GHCI_REG_RING_RPTR);
	error = EAGAIN;
	if (rptr < device->ring.dwords)
		error = ghci_ring_plan(device->ring.wptr, rptr, device->ring.dwords, need, &padding);
	if (error == 0 && device->submit_count >= GHCI_SUBMIT_LOG)
		error = EAGAIN;

	/* A ring without the room is left as it is. */
	if (error != 0) {
		mutex_unlock(&device->cp_lock);
		return GHCI_GATE_RETRY;
	}

	/* Succeeded: the ring is the caller's, with the room. */
	return GHCI_GATE_OPEN;
}

/*
 * Leaves the ring after ghci_cp_enter() opened it.
 */
void
ghci_cp_leave(
	struct ghci_device *device)
{
	/* The ring is free for the next caller. */
	mutex_unlock(&device->cp_lock);
}

/*
 * Places packets in the ring, without the doorbell.
 *
 * The caller holds cp_lock.  The packets are placed one at a time, each
 * whole and never across the end of the ring; while the ring has no room
 * the doorbell rings for what is placed and the placement waits for
 * RING_RPTR.  Returns 0, EINVAL for a packet that is malformed or larger
 * than the ring, or EAGAIN when a recovery began (or the device was lost)
 * while it waited; the packets placed until then are lost to the reset.
 */
int
ghci_cp_place(
	struct ghci_device *device,
	const uint32_t *packets,
	uint32_t dwords)
{
	uint32_t length;
	uint32_t index;
	uint32_t done;
	int error;

	/* Each packet in turn. */
	done = 0U;
	while (done < dwords) {
		/* The packet's length from its header. */
		error = packet_length(&packets[done], dwords - done, &length);
		if (error != 0)
			return error;

		/* Room for it, with the padding before the end of the ring. */
		error = ring_room(device, length, 0);
		if (error != 0)
			return error;

		/* Copies it to the ring. */
		for (index = 0U; index < length; index++)
			ring_store(device, device->ring.wptr + index, packets[done + index]);
		device->ring.wptr = (device->ring.wptr + length) % device->ring.dwords;
		done += length;
	}

	/* Succeeded: every packet is in the ring. */
	return 0;
}

/*
 * Rings the doorbell for everything placed: the ring's writes become
 * visible first, then RING_WPTR moves.  The caller holds cp_lock.
 */
void
ghci_cp_doorbell(
	struct ghci_device *device)
{
	/* The ring's dwords before the doorbell (section 6.3). */
	kern_io_write_barrier();
	ghci_write(device, GHCI_REG_RING_WPTR, device->ring.wptr);
}

/*
 * Places the CMD_FENCE that ends a submission: the next 64-bit sequence
 * into the fence page, with the fence interrupt.
 *
 * The caller holds cp_lock; start is where the submission began in the
 * ring and owner the session whose work it ends (NULL for the driver's
 * own).  Returns 0 with the submission's ticket, or EAGAIN as
 * ghci_cp_place() does.
 */
int
ghci_cp_place_fence(
	struct ghci_device *device,
	struct ghci_vm *owner,
	uint32_t start,
	struct ghci_ticket *ticket)
{
	struct ghci_submit_entry *entry;
	uint32_t packet[GHCI_DW_FENCE];
	unsigned long irq;
	uint64_t seq;
	uint32_t index;
	int idle;
	int error;

	/* Room for the fence and for the submission's entry in the log. */
	error = ring_room(device, GHCI_DW_FENCE, 1);
	if (error != 0)
		return error;

	/* The fence of the next sequence. */
	seq = device->fence_placed + 1U;
	(void)ghci_cmd_fence(packet, device->fence_page->bus, seq, GHCI_FENCE_FLAG_MEM_WRITE | GHCI_FENCE_FLAG_IRQ);
	for (index = 0U; index < GHCI_DW_FENCE; index++)
		ring_store(device, device->ring.wptr + index, packet[index]);
	device->ring.wptr = (device->ring.wptr + GHCI_DW_FENCE) % device->ring.dwords;
	device->fence_placed = seq;

	/* The submission's entry in the log, until its fence is seen passed. */
	entry = &device->submit_log[(device->submit_first + device->submit_count) % GHCI_SUBMIT_LOG];
	entry->start = start;
	entry->end = device->ring.wptr;
	entry->seq = seq;
	entry->owner = owner;
	device->submit_count++;

	/*
	 * The ticket, in the generation the fence was placed in, and the
	 * last fence placed for the hang check; a ring that was idle starts
	 * the hang check's clock now.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	idle = ghci_fence64_passed(device->completed, device->issued);
	if (idle)
		device->progress_tick = clock_ticks();
	device->issued = seq;
	ticket->seq = seq;
	ticket->generation = device->generation;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the fence is placed. */
	return 0;
}

/*
 * Submits packets of a session (NULL for the driver's own work in
 * session 0): binds the session, places the packets and a fence, binds
 * session 0 again, and rings the doorbell.  A GPU without a page-table
 * walker has no sessions to bind.
 *
 * Returns 0 with the ticket, EIO when the device is lost, the session is
 * not open (a reset ended it) or a recovery began after part of the
 * submission was placed (that part is lost to the reset), or EINVAL for
 * a malformed packet, before anything is placed.
 */
int
ghci_cp_submit(
	struct ghci_device *device,
	struct ghci_vm *vm,
	const uint32_t *packets,
	uint32_t dwords,
	struct ghci_ticket *ticket)
{
	struct ghci_ticket placed_ticket;
	int placed;
	int error;

	/* The submission; what a failure placed is the reset's to throw away, and its ticket is not reported. */
	error = ghci_cp_submit_placed(device, vm, packets, dwords, &placed_ticket, &placed);
	if (error != 0)
		return error;

	/* Succeeded: the submission is in the ring. */
	*ticket = placed_ticket;
	return 0;
}

/*
 * Submits packets as ghci_cp_submit() does, and reports in *placed
 * whether any of them reached the ring.  When some did and the
 * submission failed (EIO: a recovery began in the middle), *ticket is
 * still a ticket of the generation they were placed in: one whose fence
 * was placed, or one past every fence placed, which the reset that
 * throws them away ends (GHCI_TICKET_FAILED once it has, pending until
 * then), so that what they read is kept until the GPU can no longer run
 * them.  With nothing placed, *placed is 0 and *ticket is not set.
 */
int
ghci_cp_submit_placed(
	struct ghci_device *device,
	struct ghci_vm *vm,
	const uint32_t *packets,
	uint32_t dwords,
	struct ghci_ticket *ticket,
	int *placed)
{
	enum ghci_gate gate;
	uint32_t bind[GHCI_DW_BIND_SESSION];
	unsigned long irq;
	uint32_t session;
	uint32_t start;
	int fenced;
	int walker;
	int error;

	/* A run of packets the ring cannot take is refused before anything is placed. */
	*placed = 0;
	error = ghci_cp_check_packets(packets, dwords);
	if (error != 0)
		return error;

	/* Passes the gate, waiting out a recovery; nothing was placed before it. */
	for (;;) {
		gate = ghci_cp_enter(device, 0U, 1);
		if (gate == GHCI_GATE_LOST)
			return EIO;
		if (gate == GHCI_GATE_OPEN)
			break;
	}

	/* Only an open session submits; its state does not change while cp_lock is held. */
	if (vm != NULL && vm->state != GHCI_VM_OPEN) {
		ghci_cp_leave(device);
		return EIO;
	}

	/* The session the packets run in. */
	session = 0U;
	if (vm != NULL)
		session = vm->session;
	start = device->ring.wptr;
	walker = ghci_caps_has(&device->caps, GHCI_HAS_HW_WALKER);
	error = 0;
	if (walker) {
		(void)ghci_cmd_bind_session(bind, session);
		error = ghci_cp_place(device, bind, GHCI_DW_BIND_SESSION);
	}

	/* The packets, and the fence that says they are done. */
	fenced = 0;
	if (error == 0)
		error = ghci_cp_place(device, packets, dwords);
	if (error == 0) {
		error = ghci_cp_place_fence(device, vm, start, ticket);
		if (error == 0)
			fenced = 1;
	}

	/* Session 0 again, so that the driver's later packets do not run in the client's session. */
	if (error == 0 && walker) {
		(void)ghci_cmd_bind_session(bind, 0U);
		error = ghci_cp_place(device, bind, GHCI_DW_BIND_SESSION);
	}

	/*
	 * A part placed without its fence: a ticket past every fence placed,
	 * in the generation the part was placed in, which only the reset
	 * that throws the part away ends.
	 */
	if (device->ring.wptr != start)
		*placed = 1;
	if (*placed && !fenced) {
		irq = spin_lock_irqsave(&device->irq_lock);

		ticket->seq = device->fence_placed + 1U;
		ticket->generation = device->generation;

		spin_unlock_irqrestore(&device->irq_lock, irq);
	}

	/* The doorbell for what was placed, even a part a recovery will throw away. */
	ghci_cp_doorbell(device);
	ghci_cp_leave(device);

	/* A recovery that began in the middle lost the submission. */
	if (error == EAGAIN)
		return EIO;
	if (error != 0)
		return error;

	/* Succeeded: the submission is in the ring. */
	return 0;
}

/*
 * Waits a bounded time for a ticket.  The caller holds neither the
 * device's lock nor the display's.
 *
 * Returns 0 once its fence passed, EIO when a reset or the loss of the
 * device ended it, or ETIMEDOUT.
 */
int
ghci_cp_wait(
	struct ghci_device *device,
	const struct ghci_ticket *ticket,
	unsigned milliseconds)
{
	enum ghci_ticket_state state;
	unsigned long irq;
	uint64_t deadline;
	uint64_t sequence;
	uint64_t now;

	/* The wait ends at the deadline. */
	deadline = clock_ticks() + kern_ms_to_ticks(milliseconds);

	/*
	 * Sleeps on the fence interrupts until the ticket is answered or the
	 * time is up; the queue's sequence is read before the ticket in the
	 * same section, so that no wake between them is missed.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	for (;;) {
		sequence = waitq_sequence(&device->fence_wake);
		state = ticket_state(device, ticket);
		if (state != GHCI_TICKET_PENDING)
			break;
		now = clock_ticks();
		if (now >= deadline)
			break;
		(void)waitq_sleep(&device->fence_wake, &device->irq_lock, sequence, deadline, 0U);
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* A ticket a reset ended, or a wait that ran out. */
	if (state == GHCI_TICKET_FAILED)
		return EIO;
	if (state == GHCI_TICKET_PENDING)
		return ETIMEDOUT;

	/* Succeeded: the fence has passed. */
	return 0;
}

/*
 * Reports the state of a ticket: pending, done, or failed because a
 * reset or the loss of the device ended it before its fence passed.
 */
enum ghci_ticket_state
ghci_cp_ticket(
	struct ghci_device *device,
	const struct ghci_ticket *ticket)
{
	enum ghci_ticket_state state;
	unsigned long irq;

	/* The fence and the generations, read together. */
	irq = spin_lock_irqsave(&device->irq_lock);

	state = ticket_state(device, ticket);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Succeeded: the ticket's state. */
	return state;
}

/*
 * Reports the state of a ticket for a caller that holds irq_lock (the
 * retirement of completions, which reads several tickets in one section).
 */
enum ghci_ticket_state
ghci_cp_ticket_locked(
	struct ghci_device *device,
	const struct ghci_ticket *ticket)
{
	enum ghci_ticket_state state;

	/* The fence and the generations, which the caller's section keeps still. */
	state = ticket_state(device, ticket);

	/* Succeeded: the ticket's state. */
	return state;
}

/*
 * Checks a run of packets before any is placed: every header names a
 * length of at least one dword that stays inside the run.  Returns 0, or
 * EINVAL.
 */
int
ghci_cp_check_packets(
	const uint32_t *packets,
	uint32_t dwords)
{
	uint32_t length;
	uint32_t done;
	int error;

	/* Each packet's length in turn. */
	done = 0U;
	while (done < dwords) {
		error = packet_length(&packets[done], dwords - done, &length);
		if (error != 0)
			return error;
		done += length;
	}

	/* Succeeded: the run is whole packets. */
	return 0;
}

/*
 * Waits a bounded time for the fence page itself to reach a sequence,
 * whatever the recovery says; only the start and the recovery use it,
 * for the fences they place to see that the processor runs.
 *
 * Returns 0, or ETIMEDOUT.
 */
int
ghci_cp_await_fence(
	struct ghci_device *device,
	uint64_t seq,
	unsigned milliseconds)
{
	uint64_t word;
	unsigned waited;
	int passed;

	/* Reads the fence page every millisecond until it reaches the sequence or the time is up. */
	for (waited = 0U; waited <= milliseconds; waited++) {
		word = ghci_fence_read(device);
		passed = ghci_fence64_passed(word, seq);
		if (passed)
			return 0;

		/* Sleeps for a millisecond before the next read. */
		ghci_sleep_ms(1U);
	}

	/* The fence never came. */
	return ETIMEDOUT;
}

/*
 * Empties the ring as a reset left it: RING_WPTR and RING_RPTR are 0
 * and no submission is outstanding.  The caller holds cp_lock.
 */
void
ghci_cp_ring_reset(
	struct ghci_device *device)
{
	/* The driver's write index follows the device's. */
	device->ring.wptr = 0U;

	/* Every submission of the log was lost to the reset. */
	device->submit_first = 0U;
	device->submit_count = 0U;
}

/*
 * Lets the processor run the ring: with the watchdog, and with the
 * interrupt unless the device is polled.  The caller holds cp_lock or
 * owns the device alone.
 */
void
ghci_cp_run(
	struct ghci_device *device)
{
	uint32_t ctrl;

	/* RUN and WDT_EN; IRQ_GLOBAL only when an interrupt is taken. */
	ctrl = GHCI_CTRL_RUN | GHCI_CTRL_WDT_EN;
	if (!device->polled)
		ctrl |= GHCI_CTRL_IRQ_GLOBAL;
	ghci_write(device, GHCI_REG_CTRL, ctrl);
}

/*
 * Writes IRQ_ENABLE from irq_mask, or zero on a polled device, whose
 * IRQ_STATUS still latches what the poll reads.
 */
void
ghci_cp_irq_enable(
	struct ghci_device *device)
{
	unsigned long irq;
	uint32_t mask;

	/* The mask the handler takes, written in the section the handler reads it in; none when polled. */
	irq = spin_lock_irqsave(&device->irq_lock);

	mask = device->irq_mask;
	if (device->polled)
		mask = 0U;
	ghci_write(device, GHCI_REG_IRQ_ENABLE, mask);

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * The interrupt handler of the device, and the poll's work on a polled
 * device: a fault or the watchdog starts a recovery, a fence moves the
 * completed fence on, and the display's events (acknowledged in
 * DISP_IRQ_STATUS before IRQ_STATUS[3]) go to the service thread.
 *
 * Returns 1 when the device had an event, 0 otherwise.
 */
int
ghci_cp_interrupt(
	void *argument)
{
	struct ghci_device *device;
	unsigned long irq;
	uint64_t generation;
	uint32_t status;
	uint32_t mask;

	/*
	 * Masks the interrupt, reads what is pending, clears it (write 1 to
	 * clear) and unmasks, all with the mask and the recovery generation
	 * of one section, so that the start and the stop never see a mask
	 * written back behind them.  A polled device's IRQ_STATUS latches
	 * whatever IRQ_ENABLE says.
	 */
	device = argument;
	irq = spin_lock_irqsave(&device->irq_lock);

	mask = device->irq_mask;
	generation = device->recovery_generation;
	if (!device->polled)
		ghci_write(device, GHCI_REG_IRQ_ENABLE, 0U);
	status = ghci_read(device, GHCI_REG_IRQ_STATUS);
	status &= mask;
	if ((status & GHCI_IRQ_DISPLAY) != 0U)
		ghci_display_interrupt(device);
	if (status != 0U)
		ghci_write(device, GHCI_REG_IRQ_STATUS, status);
	if (!device->polled)
		ghci_write(device, GHCI_REG_IRQ_ENABLE, mask);

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Nothing of the GPU's to serve: another device's interrupt on a shared line, or a quiet poll. */
	if (status == 0U)
		return 0;

	/* A fault or the watchdog is taken before a fence. */
	if ((status & IRQ_RECOVER) != 0U)
		record_fault(device, status, generation);

	/* A fence wrote the fence page. */
	if ((status & GHCI_IRQ_FENCE) != 0U)
		take_fence(device, status, generation);

	/* Wakes the waiters of tickets, of the ring's room and of flips, and the service thread. */
	waitq_wake_all(&device->fence_wake);
	waitq_wake_all(&device->ring_wake);
	if ((status & GHCI_IRQ_DISPLAY) != 0U)
		waitq_wake_all(&device->display.flip_wake);
	waitq_wake_all(&device->service_wake);

	/* Succeeded: the interrupt was the device's. */
	return 1;
}

/*
 * Places one fence of the driver's and waits a bounded time for the
 * fence page to reach it, and with want_interrupt for its interrupt too.
 * Returns 0, or ETIMEDOUT.
 */
static int
liveness(
	struct ghci_device *device,
	int want_interrupt)
{
	struct ghci_ticket ticket;
	unsigned long irq;
	unsigned interrupts;
	unsigned waited;
	int error;

	/* The interrupts seen so far. */
	irq = spin_lock_irqsave(&device->irq_lock);

	interrupts = device->fence_interrupts;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The fence, placed and rung under the ring's lock. */
	mutex_lock(&device->cp_lock);

	error = ghci_cp_place_fence(device, NULL, device->ring.wptr, &ticket);
	if (error == 0)
		ghci_cp_doorbell(device);

	mutex_unlock(&device->cp_lock);
	if (error != 0)
		return ETIMEDOUT;

	/* The fence page must reach it. */
	error = ghci_cp_await_fence(device, ticket.seq, GHCI_LIVENESS_MS);
	if (error != 0)
		return ETIMEDOUT;

	/* A device with an interrupt must have raised it too. */
	for (waited = 0U; want_interrupt && waited <= GHCI_LIVENESS_MS; waited++) {
		irq = spin_lock_irqsave(&device->irq_lock);

		if (device->fence_interrupts != interrupts)
			want_interrupt = 0;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* Sleeps for a millisecond while the interrupt has not come. */
		if (want_interrupt)
			ghci_sleep_ms(1U);
	}
	if (want_interrupt)
		return ETIMEDOUT;

	/* Succeeded: the processor runs and the interrupt comes. */
	return 0;
}

/*
 * Gives back the device's own memory: the ring, the fence page, the fault log ring,
 * the capability block's buffer and the quarantine.
 */
static void
release_memory(
	struct ghci_device *device)
{
	int error;

	/* Each run, if it was taken; one that cannot be given back is kept in the quarantine. */
	error = ghci_memory_free(device, device->fault_ring);
	if (error != 0)
		ghci_memory_quarantine(device, device->fault_ring);
	device->fault_ring = NULL;
	error = ghci_memory_free(device, device->ring.memory);
	if (error != 0)
		ghci_memory_quarantine(device, device->ring.memory);
	device->ring.memory = NULL;
	error = ghci_memory_free(device, device->fence_page);
	if (error != 0)
		ghci_memory_quarantine(device, device->fence_page);
	device->fence_page = NULL;
	error = ghci_memory_free(device, device->caps_answer);
	if (error != 0)
		ghci_memory_quarantine(device, device->caps_answer);
	device->caps_answer = NULL;

	/* The quarantine of a lost device, now that the GPU reaches nothing, then the local allocator. */
	ghci_memory_release_quarantine(device);
	ghci_memory_local_stop(device);
}

/*
 * Waits until the ring has room for need dwords (and the padding before
 * its end, which this writes), and with log_entry a free entry of the
 * submission log.  The caller holds cp_lock.
 *
 * Returns 0, EINVAL for a run no ring of this size takes, or EAGAIN when
 * a recovery began or the device was lost while it waited.  The
 * recovery's own placement (recovery_placing) waits on through the
 * recovery that runs, and ends with EAGAIN at a new fault or at its
 * deadline.
 */
static int
ring_room(
	struct ghci_device *device,
	uint32_t need,
	int log_entry)
{
	unsigned long irq;
	uint64_t deadline;
	uint64_t sequence;
	uint32_t padding;
	uint32_t written;
	uint32_t index;
	uint32_t rptr;
	uint64_t now;
	int stopping;
	int error;

	/* Looks again after each fence interrupt or poll period, until there is room. */
	for (;;) {
		/* The submissions whose fences passed leave the log. */
		retire_log(device);

		/* The reader's index; one outside the ring leaves no room until a recovery. */
		rptr = ghci_read(device, GHCI_REG_RING_RPTR);
		error = EAGAIN;
		if (rptr < device->ring.dwords)
			error = ghci_ring_plan(device->ring.wptr, rptr, device->ring.dwords, need, &padding);
		if (error == EINVAL)
			return EINVAL;
		if (error == 0 && log_entry && device->submit_count >= GHCI_SUBMIT_LOG)
			error = EAGAIN;
		if (error == 0)
			break;

		/* The processor gets what is placed, so that it can make room. */
		ghci_cp_doorbell(device);

		/* Sleeps until a fence interrupt or the poll period; a recovery ends the wait. */
		now = clock_ticks();
		irq = spin_lock_irqsave(&device->irq_lock);

		stopping = device->recovering || device->state == GHCI_STATE_LOST;
		if (device->recovery_placing)
			stopping = device->fault_count != device->recovery_faults || now >= device->recovery_deadline;
		if (!stopping) {
			deadline = clock_ticks() + kern_ms_to_ticks(GHCI_POLL_MS);
			sequence = waitq_sequence(&device->ring_wake);
			(void)waitq_sleep(&device->ring_wake, &device->irq_lock, sequence, deadline, 0U);
		}

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* The recovery takes the ring from here. */
		if (stopping)
			return EAGAIN;
	}

	/* The rest of the ring is padded with NOPs when the run does not fit before its end. */
	while (padding != 0U) {
		written = padding;
		if (written > GHCI_PACKET_DWORDS_MAX)
			written = GHCI_PACKET_DWORDS_MAX;
		ring_store(device, device->ring.wptr, ghci_cmd_header(GHCI_OP_NOP, written, 0U));
		for (index = 1U; index < written; index++)
			ring_store(device, device->ring.wptr + index, 0U);
		device->ring.wptr = (device->ring.wptr + written) % device->ring.dwords;
		padding -= written;
	}

	/* Succeeded: the run fits at the write index. */
	return 0;
}

/* Stores one dword of the ring, little-endian as the GPU reads it. */
static void
ring_store(
	struct ghci_device *device,
	uint32_t index,
	uint32_t word)
{
	volatile uint32_t *ring;

	/* The processor's view of the ring; the platforms the driver runs on are little-endian. */
	ring = (volatile uint32_t *)(void *)device->ring.memory->view;
	ring[index] = word;
}

/* Drops the submissions whose fences the driver has accepted from the log.  The caller holds cp_lock. */
static void
retire_log(
	struct ghci_device *device)
{
	unsigned long irq;
	uint64_t completed;
	int passed;

	/* The fence the driver has accepted. */
	irq = spin_lock_irqsave(&device->irq_lock);

	completed = device->completed;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* The oldest entries first, while their fences have passed. */
	while (device->submit_count != 0U) {
		passed = ghci_fence64_passed(completed, device->submit_log[device->submit_first].seq);
		if (!passed)
			break;
		device->submit_first = (device->submit_first + 1U) % GHCI_SUBMIT_LOG;
		device->submit_count--;
	}
}

/*
 * Reads the length of the packet at the head of a run of packets.
 * Returns 0, or EINVAL for a header of no dwords or past the run.
 */
static int
packet_length(
	const uint32_t *packets,
	uint32_t left,
	uint32_t *length)
{
	uint32_t dwords;

	/* The dwords field of the header. */
	dwords = (packets[0] >> GHCI_HEADER_DWORDS_SHIFT) & GHCI_HEADER_DWORDS_MASK;
	if (dwords == 0U || dwords > left)
		return EINVAL;

	/* Succeeded: the packet's length. */
	*length = dwords;
	return 0;
}

/*
 * Reports the state of a ticket.  The caller holds irq_lock.
 */
static enum ghci_ticket_state
ticket_state(
	struct ghci_device *device,
	const struct ghci_ticket *ticket)
{
	struct ghci_reset_record *record;
	unsigned index;
	int passed;

	/* A ticket of this generation is done once the accepted fence reaches it. */
	if (ticket->generation == device->generation) {
		passed = ghci_fence64_passed(device->completed, ticket->seq);
		if (passed)
			return GHCI_TICKET_DONE;
		return GHCI_TICKET_PENDING;
	}

	/* A ticket of no generation the driver placed in failed. */
	if (ticket->generation > device->generation)
		return GHCI_TICKET_FAILED;

	/* An older ticket passed if the fence had reached it when the reset ended its generation, unless a culprit's. */
	record = &device->history[ticket->generation % GHCI_RESET_HISTORY];
	passed = ghci_fence64_passed(record->completed, ticket->seq);
	for (index = 0U; index < record->failed_count && passed; index++) {
		if (record->failed[index] == ticket->seq)
			passed = 0;
	}
	if (record->generation == ticket->generation && passed)
		return GHCI_TICKET_DONE;

	/* Succeeded: the reset ended it first (or it is older than the history). */
	return GHCI_TICKET_FAILED;
}

/*
 * Takes the records the processor wrote to the fault log ring since the
 * last look, keeping the first in record when the registers latched no
 * fault.  With the ring set up, a client session's fault is written there
 * and not latched in FAULT_INFO and FAULT_ADDR; the session it names is
 * the recovery's culprit.  A record whose sequence is not the next one
 * expected is not written yet, or was overwritten; either ends the look,
 * and the recovery's reset sets the ring up again from sequence 1.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 3.3, a client
 * session's fault with the ring enabled is written to the ring without
 * FAULT_INFO and FAULT_ADDR; section 10, GhciFaultRecord of 32 bytes:
 * seq, fault_addr, timestamp, session_id and fault_info_word, at slot
 * (seq - setup_next_seq) % capacity_entries, the next seq 1 after
 * CMD_FAULT_LOG_SETUP following a reset).
 */
static void
fault_ring_take(
	struct ghci_device *device,
	struct ghci_fault_record *record)
{
	const volatile uint32_t *word;
	unsigned long irq;
	uint64_t expected;
	uint64_t seq;
	uint32_t slot;
	uint32_t taken;
	uint32_t base;

	/* A GPU without the ring (no walker, or before the MMU starts) writes no record. */
	if (device->fault_ring == NULL)
		return;

	/* The sequence the driver expects next. */
	irq = spin_lock_irqsave(&device->irq_lock);

	expected = device->fault_seq;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Each record written since, up to one ring's worth. */
	for (taken = 0U; taken < GHCI_FAULT_RING_ENTRIES; taken++) {
		/* The record's slot; the ring was set up with its first sequence 1. */
		slot = (uint32_t)((expected - 1U) % GHCI_FAULT_RING_ENTRIES);
		base = slot * GHCI_FAULT_RECORD_BYTES;
		word = (const volatile uint32_t *)(const void *)(device->fault_ring->view + base);
		seq = word[GHCI_FAULT_RECORD_SEQ / 4U];
		seq |= (uint64_t)word[GHCI_FAULT_RECORD_SEQ / 4U + 1U] << 32;
		kern_io_read_barrier();

		/* A slot that does not hold the expected record is not written yet. */
		if (seq != expected)
			break;

		/* The first record fills a fault the registers did not latch. */
		if (record->fault_info == 0U) {
			record->fault_info = word[GHCI_FAULT_RECORD_INFO / 4U];
			record->fault_address = word[GHCI_FAULT_RECORD_ADDRESS / 4U];
			record->fault_address |= (uint64_t)word[GHCI_FAULT_RECORD_ADDRESS / 4U + 1U] << 32;
		}

		/* The session the processor names, for the log. */
		kern_logf("ghci: %s: fault record %llu: session %u, fault %08x at %llx\n",
		    device->name,
		    (unsigned long long)seq,
		    (unsigned)word[GHCI_FAULT_RECORD_SESSION / 4U],
		    (unsigned)word[GHCI_FAULT_RECORD_INFO / 4U],
		    (unsigned long long)record->fault_address);
		expected++;
	}

	/* The next sequence the driver looks for. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->fault_seq = expected;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Records a fault or a watchdog expiry for the service thread and closes
 * the gate.  The registers are read before the interrupt's lock; the
 * record carries the recovery generation the interrupt saw it in, so that
 * a recovery that ended meanwhile has it dropped.
 */
static void
record_fault(
	struct ghci_device *device,
	uint32_t irq_status,
	uint64_t generation)
{
	struct ghci_fault_record record;
	unsigned long irq;
	uint32_t high;

	/* What the device latched, and where its processor is. */
	kern_memset(&record, 0, sizeof(record));
	record.irq_status = irq_status;
	record.status = ghci_read(device, GHCI_REG_STATUS);
	record.fault_info = ghci_read(device, GHCI_REG_FAULT_INFO);
	record.fault_address = ghci_read(device, GHCI_REG_FAULT_ADDR_LO);
	high = ghci_read(device, GHCI_REG_FAULT_ADDR_HI);
	record.fault_address |= (uint64_t)high << 32;
	record.session_active = ghci_read(device, GHCI_REG_SESSION_ACTIVE);
	record.rptr = ghci_read(device, GHCI_REG_RING_RPTR);
	record.fence = ghci_fence_read(device);

	/* A client session's fault, which the fault log ring has instead of the registers. */
	fault_ring_take(device, &record);

	/*
	 * Closes the gate, counts the fault (the recovery's last check sees a
	 * new one), keeps the record in this recovery generation, and asks
	 * the service thread to recover.
	 */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->recovering = 1;
	device->fault_count++;
	record.generation = generation;
	if (device->fault_records < GHCI_FAULT_RECORDS) {
		device->faults[device->fault_records] = record;
		device->fault_records++;
	} else {
		device->faults_dropped++;
	}
	device->service_work |= GHCI_WORK_RECOVER;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/*
 * Takes a fence interrupt: reads the fence page, then reads FAULT and
 * WATCHDOG again, so that a fence written after a fault latched is never
 * taken as the work's completion; the completed fence moves only while
 * no recovery runs.
 */
static void
take_fence(
	struct ghci_device *device,
	uint32_t irq_status,
	uint64_t generation)
{
	unsigned long irq;
	uint64_t word;
	uint32_t late;
	int newer;

	/* The fence the device wrote, then any fault that latched since IRQ_STATUS was read. */
	word = ghci_fence_read(device);
	late = ghci_read(device, GHCI_REG_IRQ_STATUS);
	late &= IRQ_RECOVER;
	if ((irq_status & IRQ_RECOVER) != 0U)
		late = 0U;
	if (late != 0U) {
		ghci_write(device, GHCI_REG_IRQ_STATUS, late);
		record_fault(device, late, generation);
	}

	/* Moves the completed fence on, unless a recovery runs or the device is lost. */
	irq = spin_lock_irqsave(&device->irq_lock);

	device->fence_interrupts++;
	newer = ghci_fence64_passed(word, device->completed);
	if (!device->recovering && device->state != GHCI_STATE_LOST && newer) {
		device->completed = word;
		device->service_work |= GHCI_WORK_RECLAIM;
	}

	spin_unlock_irqrestore(&device->irq_lock, irq);
}

/* Logs the registers that say where the processor stopped. */
static void
log_registers(
	struct ghci_device *device,
	const char *what)
{
	uint32_t status;
	uint32_t fault_info;
	uint32_t address_low;
	uint32_t address_high;
	uint32_t rptr;

	/* The state, the fault and its address, and where in the ring the processor is. */
	status = ghci_read(device, GHCI_REG_STATUS);
	fault_info = ghci_read(device, GHCI_REG_FAULT_INFO);
	address_low = ghci_read(device, GHCI_REG_FAULT_ADDR_LO);
	address_high = ghci_read(device, GHCI_REG_FAULT_ADDR_HI);
	rptr = ghci_read(device, GHCI_REG_RING_RPTR);
	kern_logf("ghci: %s: %s (status %08x, fault %08x at %08x%08x, rptr %u)\n",
	    device->name,
	    what,
	    (unsigned)status,
	    (unsigned)fault_info,
	    (unsigned)address_high,
	    (unsigned)address_low,
	    (unsigned)rptr);
}
