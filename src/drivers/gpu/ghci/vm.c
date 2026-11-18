/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device's sessions and their GPU addresses: the session
 * numbers, CMD_SESSION_OPEN and CMD_SESSION_CLOSE, the mappings
 * CMD_VM_MAP and CMD_VM_UNMAP make, CP_HEAP's count, and what a recovery
 * does to the sessions: the culprit of a fault, the sessions that died
 * and the replay of the rest.
 *
 * Numbers.  Session 0 is the driver's and always open; an owner takes
 * one of 1 to 15 and gives it back only once the session is retired (its
 * close's fence passed, or a reset ended the close).
 *
 * Addresses.  The driver uses the low 4 GiB of GPU addresses (all of a
 * narrower VA), cut into 2 MiB chunks.  A chunk belongs to one session at
 * a time, so that no two sessions' mappings ever meet, even in the page
 * table every session shares on a GPU without PROT, and a level 0 table
 * of the walker serves one session only.  A session's mappings lie in
 * arenas, runs of the chunks it owns; an arena goes back when its last
 * mapping's unmap has passed.  The first and the last chunk of that range
 * are never used.
 *
 * CP_HEAP.  The command processor takes its page tables from CP_HEAP on
 * its own; the driver cannot see what it took and counts the bound
 * instead: each open session's root and upper tables, and a 4 KiB level
 * 0 table for each chunk a session (with PROT, which gives each session
 * its own table) or any session (without, one shared table) was given.
 * A request past the bound fails with ENOMEM before it reaches the GPU,
 * so that the processor never runs out (MMU_OOM).  A close gives its
 * session's tables back once its fence passed; the shared level 0 tables
 * stay until a reset, which empties CP_HEAP and is counted again.
 *
 * A GPU without a page-table walker has neither sessions nor mappings in
 * the GPU: every session runs as session 0, a mapping's address is its
 * bus address, and nothing is placed in the ring to map or unmap.
 *
 * Every change of a session's state happens under the device's lock and
 * cp_lock together, in the same section as the packets that make it, and
 * only once all of them are placed: a recovery that begins meanwhile
 * (the gate answers RETRY, or the ring's room ends with EAGAIN) leaves
 * nothing recorded, and the caller starts again.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "ghci.h"
#include "ghci-cmd.h"

/* The mappings one arena holds at once; a full arena sends a mapping to the next. */
#define ARENA_EXTENTS			64U

/* The first chunk the driver hands out; the last one is end_chunk's. */
#define FIRST_CHUNK			1U

/* The access every mapping of host memory has; the GPU keeps it coherent with the processor. */
#define HOST_MAP_FLAGS			(GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_COHERENT)

/*
 * The access every mapping of the GPU's local memory has: its address is
 * a local one (VM_FLAG_LOCAL), and the processor's caches have no part
 * in it.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 11.2,
 * VM_FLAG_LOCAL; section 11.3, phys of a mapping with VM_FLAG_LOCAL).
 */
#define LOCAL_MAP_FLAGS			(GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_LOCAL)

/*
 * One run of chunks a session owns and the GPU addresses its mappings
 * take in it.  It lives from the first mapping placed in it until the
 * last one leaves; the device's lock protects it.
 */
struct ghci_arena {
	struct ghci_arena *next;
	uint32_t first;
	uint32_t chunks;
	struct ghci_range_map map;
	struct ghci_range_extent extents[ARENA_EXTENTS];
};

static enum ghci_gate enter_ring(struct ghci_device *device);
static int has_walker(const struct ghci_device *device);
static uint32_t free_slot(const struct ghci_device *device);
static uint8_t *marks_of(struct ghci_device *device, struct ghci_vm *vm);
static uint64_t marks_need(struct ghci_device *device, struct ghci_vm *vm, uint32_t first, uint32_t chunks);
static void marks_take(struct ghci_device *device, struct ghci_vm *vm, uint32_t first, uint32_t chunks);
static unsigned marks_count(const uint8_t *marks);
static int va_take(struct ghci_device *device, struct ghci_vm *vm, uint64_t bytes, uint64_t *va, struct ghci_arena **result);
static void va_give(struct ghci_device *device, struct ghci_vm *vm, struct ghci_arena *arena, uint64_t va);
static int run_find(const struct ghci_device *device, uint32_t chunks, uint32_t *first);
static uint32_t end_chunk(const struct ghci_device *device);
static void mapping_drop(struct ghci_device *device, struct ghci_mapping *mapping);
static void vm_retire(struct ghci_device *device, struct ghci_vm *vm);
static void vm_fault(struct ghci_device *device, struct ghci_vm *vm);
static int place_map(struct ghci_device *device, struct ghci_vm *vm, const struct ghci_mapping *mapping);
static void current_ticket(struct ghci_device *device, struct ghci_ticket *ticket);

/*
 * Sets session 0's record and what CP_HEAP may hold, at the start: the
 * bound less the specification's spare, with session 0's root counted.
 */
void
ghci_vm_init(
	struct ghci_device *device)
{
	uint64_t spare;
	uint32_t slot;
	int walker;

	/* No number and no chunk is in use. */
	walker = has_walker(device);
	for (slot = 0U; slot < GHCI_SESSIONS; slot++)
		device->vm[slot] = NULL;
	kern_memset(device->chunk_owner, 0, sizeof(device->chunk_owner));
	kern_memset(device->l0_marks, 0, sizeof(device->l0_marks));

	/* Session 0, the driver's, is always open. */
	kern_memset(&device->vm0, 0, sizeof(device->vm0));
	device->vm0.session = 0U;
	device->vm0.flat = !walker;
	device->vm0.state = GHCI_VM_OPEN;
	device->vm[0] = &device->vm0;

	/* A GPU without a walker builds no tables beyond session 0's root. */
	device->heap_budget = 0U;
	device->heap_used = 0U;
	if (!walker)
		return;

	/* The bound, less the spare the specification's sizing keeps, and session 0's root. */
	spare = GHCI_HEAP_SPARE_BYTES;
	device->heap_budget = device->heap->bytes / 2U;
	if (device->heap->bytes >= 2U * spare)
		device->heap_budget = device->heap->bytes - spare;
	device->heap_used = ghci_heap_session_bytes(device->caps.va_bits);
	device->vm0.counted = 1;
}

/*
 * Gives back every session's record and mapping at the detach, once the
 * GPU is reset and reaches nothing.  The caller owns the device alone.
 */
void
ghci_vm_fini(
	struct ghci_device *device)
{
	struct ghci_vm *vm;
	uint32_t slot;

	/* Every session still closing retires. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm != NULL)
			vm_retire(device, vm);
	}

	/* Session 0's mappings go. */
	while (device->vm0.mappings != NULL)
		mapping_drop(device, device->vm0.mappings);
}

/*
 * Opens a GHCI session for owner (the GPU core's session to report a
 * loss to): takes a number and places CMD_SESSION_OPEN.
 *
 * Returns 0 with the session, EBUSY when all fifteen numbers are in use,
 * ENOSPC when CP_HEAP's bound has no room for its tables, ENOMEM when
 * the host has no memory for its record, or EIO when the device is lost.
 */
int
ghci_vm_open(
	struct ghci_device *device,
	void *owner,
	struct ghci_vm **result)
{
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	enum ghci_gate gate;
	uint32_t packet[GHCI_DW_SESSION_OPEN];
	uint32_t slot;
	uint32_t start;
	uint64_t need;
	unsigned long irq;
	int lost;
	int error;

	/* The session's record. */
	vm = kern_calloc(1U, sizeof(*vm));
	if (vm == NULL)
		return ENOMEM;
	vm->owner = owner;

	/* Sessions that finished closing give their numbers back first. */
	mutex_lock(&device->lock);

	(void)ghci_vm_reclaim(device);

	/* Without a walker a session is a number only; nothing is placed. */
	if (!has_walker(device)) {
		irq = spin_lock_irqsave(&device->irq_lock);

		lost = device->state == GHCI_STATE_LOST;

		spin_unlock_irqrestore(&device->irq_lock, irq);
		slot = free_slot(device);
		error = 0;
		if (lost) {
			error = EIO;
		} else if (slot == 0U) {
			error = EBUSY;
		} else {
			vm->slot = slot;
			vm->session = 0U;
			vm->flat = 1;
			vm->state = GHCI_VM_OPEN;
			device->vm[slot] = vm;
		}

		mutex_unlock(&device->lock);
		if (error != 0) {
			kern_free(vm);
			return error;
		}

		/* Succeeded: the number is the owner's. */
		*result = vm;
		return 0;
	}

	/* The number and CMD_SESSION_OPEN in one section past the gate. */
	for (;;) {
		gate = enter_ring(device);
		if (gate == GHCI_GATE_LOST) {
			error = EIO;
			break;
		}
		if (gate == GHCI_GATE_RETRY)
			continue;

		/* A free number, and room in CP_HEAP's bound for its tables. */
		slot = free_slot(device);
		need = ghci_heap_session_bytes(device->caps.va_bits);
		if (slot == 0U) {
			ghci_cp_leave(device);
			error = EBUSY;
			break;
		}
		if (device->heap_used + need > device->heap_budget) {
			ghci_cp_leave(device);
			error = ENOSPC;
			break;
		}

		/*
		 * CMD_SESSION_OPEN and a fence that says the processor opened it.
		 *
		 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
		 * 7.3, CMD_SESSION_OPEN; section 11.2, the root a session takes
		 * from CP_HEAP).
		 */
		start = device->ring.wptr;
		(void)ghci_cmd_session_open(packet, slot, 0U);
		error = ghci_cp_place(device, packet, GHCI_DW_SESSION_OPEN);
		if (error == 0)
			error = ghci_cp_place_fence(device, NULL, start, &ticket);
		ghci_cp_doorbell(device);

		/* A recovery began: nothing is recorded, and the open starts again. */
		if (error == EAGAIN) {
			ghci_cp_leave(device);
			continue;
		}
		if (error != 0) {
			ghci_cp_leave(device);
			break;
		}

		/* The session is open and counted. */
		vm->slot = slot;
		vm->session = slot;
		vm->state = GHCI_VM_OPEN;
		vm->counted = 1;
		vm->open_ticket = ticket;
		device->vm[slot] = vm;
		device->heap_used += need;
		ghci_cp_leave(device);
		break;
	}

	mutex_unlock(&device->lock);

	/* An open that failed leaves nothing behind. */
	if (error != 0) {
		kern_free(vm);
		return error;
	}

	/* Succeeded: the session is the owner's. */
	*result = vm;
	return 0;
}

/*
 * Closes a session; it cannot fail.  Every live mapping is unmapped, then
 * CMD_SESSION_CLOSE and a fence are placed, and the session retires once
 * that fence passes (or a reset ends it): its number, chunks, tables and
 * memory go back then.  A session a reset killed, or one of a lost
 * device, retires at once without the GPU.  The caller gives up the
 * session and its mappings.
 */
void
ghci_vm_close(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	enum ghci_gate gate;
	uint32_t packet[GHCI_DW_VM_UNMAP];
	uint32_t start;
	unsigned long irq;
	int lost;
	int error;

	/* Places the close, again after each recovery, until it is placed or needs no GPU. */
	mutex_lock(&device->lock);

	for (;;) {
		/* The device's life, as the recovery last set it. */
		irq = spin_lock_irqsave(&device->irq_lock);

		lost = device->state == GHCI_STATE_LOST;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/*
		 * A lost device, or a session a reset killed, needs no packet:
		 * a session dies only in a reset that ended (the GPU no longer
		 * reaches its memory), so the memory goes at once.  A session
		 * already closing has its packets in the ring.
		 */
		if (lost || vm->state == GHCI_VM_DEAD) {
			vm_retire(device, vm);
			break;
		}
		if (vm->state == GHCI_VM_CLOSING)
			break;

		/*
		 * An isolated session may still run in the GPU: nothing is placed
		 * and nothing goes back.  The device keeps the record, with its
		 * number, chunks and mappings, until a reset retires it.
		 */
		if (vm->state == GHCI_VM_ISOLATED) {
			vm->closed = 1;
			vm->owner = NULL;
			break;
		}

		/* Without a walker the close waits only for the work placed until now. */
		if (vm->flat) {
			mutex_lock(&device->cp_lock);

			current_ticket(device, &ticket);
			vm->state = GHCI_VM_CLOSING;
			vm->close_ticket = ticket;

			mutex_unlock(&device->cp_lock);
			break;
		}

		/* The gate; a recovery or a loss sends the close round again. */
		gate = enter_ring(device);
		if (gate != GHCI_GATE_OPEN)
			continue;
		if (vm->state != GHCI_VM_OPEN) {
			ghci_cp_leave(device);
			continue;
		}

		/*
		 * Every live mapping's unmap, then CMD_SESSION_CLOSE (never before
		 * the unmaps), then the fence.
		 *
		 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
		 * 7.3, CMD_VM_UNMAP and CMD_SESSION_CLOSE; section 11.2, the
		 * tables a session close returns to CP_HEAP).
		 */
		start = device->ring.wptr;
		error = 0;
		for (mapping = vm->mappings; mapping != NULL && error == 0; mapping = mapping->next) {
			/* A mapping already unmapping has its packet in the ring. */
			if (mapping->state != GHCI_MAPPING_LIVE)
				continue;
			(void)ghci_cmd_vm_unmap(packet, vm->session, mapping->va, mapping->bytes);
			error = ghci_cp_place(device, packet, GHCI_DW_VM_UNMAP);
		}
		if (error == 0) {
			(void)ghci_cmd_session_close(packet, vm->session);
			error = ghci_cp_place(device, packet, GHCI_DW_SESSION_CLOSE);
		}
		if (error == 0)
			error = ghci_cp_place_fence(device, NULL, start, &ticket);
		ghci_cp_doorbell(device);

		/* A recovery began: what was placed is lost to the reset, and the close starts again. */
		if (error != 0) {
			ghci_cp_leave(device);
			continue;
		}

		/* The session closes with that fence; its live mappings go with it. */
		for (mapping = vm->mappings; mapping != NULL; mapping = mapping->next) {
			if (mapping->state != GHCI_MAPPING_LIVE)
				continue;
			mapping->state = GHCI_MAPPING_UNMAPPING;
			mapping->ticket = ticket;
		}
		vm->state = GHCI_VM_CLOSING;
		vm->close_ticket = ticket;
		ghci_cp_leave(device);
		break;
	}

	mutex_unlock(&device->lock);
}

/*
 * Maps an allocation into a session: takes GPU addresses in a chunk the
 * session owns and places CMD_VM_MAP.  flags adds GHCI_VM_FLAG_EXEC for
 * code and GHCI_VM_MAP_NO_WRITE for memory the GPU only reads (the
 * mapping then lacks GHCI_VM_FLAG_WRITE); the rest of the access and the
 * coherence are the driver's.  The placement is not waited for: the
 * processor runs the ring in order.
 *
 * Local memory is mapped with GHCI_VM_FLAG_LOCAL and its local address,
 * and never coherent with the processor.
 *
 * Returns 0 with the mapping, which holds the allocation; ENOTSUP for
 * local memory on a GPU without a page-table walker (whose addresses are
 * bus addresses); ENOMEM when no addresses or no room in CP_HEAP's bound
 * are left; or EIO when the session or the device is lost.
 */
int
ghci_vm_map(
	struct ghci_device *device,
	struct ghci_vm *vm,
	struct ghci_allocation *allocation,
	uint32_t flags,
	struct ghci_mapping **result)
{
	struct ghci_mapping *mapping;
	struct ghci_arena *arena;
	enum ghci_gate gate;
	uint64_t va;
	int error;

	/* Local memory needs a walker; the flags a caller adds are EXEC and NO_WRITE. */
	if (allocation->kind == GHCI_MEMORY_LOCAL && vm->flat)
		return ENOTSUP;
	if ((flags & ~(GHCI_VM_FLAG_EXEC | GHCI_VM_MAP_NO_WRITE)) != 0U)
		return EINVAL;

	/* The mapping's record. */
	mapping = kern_calloc(1U, sizeof(*mapping));
	if (mapping == NULL)
		return ENOMEM;
	mapping->vm = vm;
	mapping->allocation = allocation;
	mapping->bytes = allocation->bytes;
	mapping->flags = HOST_MAP_FLAGS | (flags & GHCI_VM_FLAG_EXEC);
	if (allocation->kind == GHCI_MEMORY_LOCAL)
		mapping->flags = LOCAL_MAP_FLAGS | (flags & GHCI_VM_FLAG_EXEC);
	if ((flags & GHCI_VM_MAP_NO_WRITE) != 0U)
		mapping->flags &= ~GHCI_VM_FLAG_WRITE;
	mapping->state = GHCI_MAPPING_LIVE;

	/* Mappings that finished unmapping give their addresses back first. */
	mutex_lock(&device->lock);

	(void)ghci_vm_reclaim(device);

	/*
	 * Without a walker the address is the bus address, and nothing is
	 * placed; the gate still waits out a recovery and refuses a lost
	 * device.
	 */
	if (vm->flat) {
		for (;;) {
			gate = enter_ring(device);
			if (gate == GHCI_GATE_LOST) {
				error = EIO;
				break;
			}
			if (gate == GHCI_GATE_RETRY)
				continue;
			error = EIO;
			if (vm->state == GHCI_VM_OPEN) {
				mapping->va = allocation->bus;
				error = 0;
			}
			ghci_cp_leave(device);
			break;
		}
	} else {
		/* The addresses and CMD_VM_MAP in one section past the gate. */
		for (;;) {
			gate = enter_ring(device);
			if (gate == GHCI_GATE_LOST) {
				error = EIO;
				break;
			}
			if (gate == GHCI_GATE_RETRY)
				continue;

			/* An open session only. */
			if (vm->state != GHCI_VM_OPEN) {
				ghci_cp_leave(device);
				error = EIO;
				break;
			}

			/* Addresses in a chunk of the session's. */
			error = va_take(device, vm, mapping->bytes, &va, &arena);
			if (error != 0) {
				ghci_cp_leave(device);
				break;
			}
			mapping->va = va;
			mapping->arena = arena;

			/* The packet; a recovery that began gives the addresses back, and the map starts again. */
			error = place_map(device, vm, mapping);
			ghci_cp_doorbell(device);
			if (error != 0) {
				va_give(device, vm, arena, va);
				mapping->arena = NULL;
				ghci_cp_leave(device);
				if (error == EAGAIN)
					continue;
				break;
			}
			ghci_cp_leave(device);
			break;
		}
	}

	/* The mapping joins its session and holds the allocation. */
	if (error == 0) {
		mapping->next = vm->mappings;
		vm->mappings = mapping;
		allocation->reached = 1;
		ghci_memory_hold(allocation);
	}

	mutex_unlock(&device->lock);

	/* A map that failed leaves nothing behind. */
	if (error != 0) {
		kern_free(mapping);
		return error;
	}

	/* Succeeded: the mapping is the caller's until it unmaps it. */
	*result = mapping;
	return 0;
}

/*
 * Unmaps a mapping; it cannot fail.  CMD_VM_UNMAP and a fence are
 * placed, and the mapping lets its addresses and its allocation go once
 * that fence passes.  Without a walker nothing is placed: the mapping
 * waits for the work placed until now.  A mapping of a session a reset
 * killed, or of a lost device, goes at once.  The caller gives up the
 * mapping.
 */
void
ghci_vm_unmap(
	struct ghci_device *device,
	struct ghci_mapping *mapping)
{
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	enum ghci_gate gate;
	uint32_t packet[GHCI_DW_VM_UNMAP];
	uint32_t start;
	unsigned long irq;
	int lost;
	int error;

	/* Places the unmap, again after each recovery, until it is placed or needs no GPU. */
	vm = mapping->vm;
	mutex_lock(&device->lock);

	while (mapping->state == GHCI_MAPPING_LIVE) {
		/* The device's life, as the recovery last set it. */
		irq = spin_lock_irqsave(&device->irq_lock);

		lost = device->state == GHCI_STATE_LOST;

		spin_unlock_irqrestore(&device->irq_lock, irq);

		/* A lost device, or a session a reset killed, needs no packet. */
		if (lost || vm->state == GHCI_VM_DEAD) {
			mapping_drop(device, mapping);
			break;
		}

		/* An isolated session places nothing; the mapping is held until a reset ends. */
		if (vm->state == GHCI_VM_ISOLATED) {
			mapping->state = GHCI_MAPPING_HELD;
			break;
		}

		/* Without a walker the mapping waits for the work placed until now. */
		if (vm->flat) {
			mutex_lock(&device->cp_lock);

			current_ticket(device, &mapping->ticket);
			mapping->state = GHCI_MAPPING_UNMAPPING;

			mutex_unlock(&device->cp_lock);
			break;
		}

		/* The gate; a recovery or a loss sends the unmap round again, the mapping still live. */
		gate = enter_ring(device);
		if (gate != GHCI_GATE_OPEN)
			continue;

		/* CMD_VM_UNMAP and its fence. */
		start = device->ring.wptr;
		(void)ghci_cmd_vm_unmap(packet, vm->session, mapping->va, mapping->bytes);
		error = ghci_cp_place(device, packet, GHCI_DW_VM_UNMAP);
		if (error == 0)
			error = ghci_cp_place_fence(device, NULL, start, &ticket);
		ghci_cp_doorbell(device);
		if (error == 0) {
			mapping->state = GHCI_MAPPING_UNMAPPING;
			mapping->ticket = ticket;
		}
		ghci_cp_leave(device);
	}

	mutex_unlock(&device->lock);
}

/*
 * Retires what the GPU is done with: mappings whose unmap's fence passed,
 * and sessions whose close's fence passed.  On a lost device, what a
 * failed fence leaves goes too (its memory joins the quarantine).  The
 * caller holds the device's lock.  Returns 1 when anything was given
 * back.
 */
int
ghci_vm_reclaim(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_mapping *next;
	struct ghci_vm *vm;
	enum ghci_ticket_state state;
	unsigned long irq;
	uint32_t slot;
	int lost;
	int freed;

	/* The device's life, as the recovery last set it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	lost = device->state == GHCI_STATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Each session's mappings, then the session itself. */
	freed = 0;
	for (slot = 0U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm == NULL)
			continue;

		/* A mapping whose unmap passed goes. */
		for (mapping = vm->mappings; mapping != NULL; mapping = next) {
			next = mapping->next;
			if (mapping->state != GHCI_MAPPING_UNMAPPING)
				continue;
			state = ghci_cp_ticket(device, &mapping->ticket);
			if (state == GHCI_TICKET_DONE || (state == GHCI_TICKET_FAILED && lost)) {
				mapping_drop(device, mapping);
				freed = 1;
			}
		}

		/* A session whose close passed retires. */
		if (vm->state != GHCI_VM_CLOSING)
			continue;
		state = ghci_cp_ticket(device, &vm->close_ticket);
		if (state == GHCI_TICKET_DONE || (state == GHCI_TICKET_FAILED && lost)) {
			vm_retire(device, vm);
			freed = 1;
		}
	}

	/* Succeeded: whether anything went back. */
	return freed;
}

/*
 * Finds the culprits of a fault (the recovery's step 2): with a walker,
 * the session FAULT_INFO names when the fault is one a session's work
 * causes, and with PROT every confirmed open session the device no
 * longer shows active (the processor latched only the first fault);
 * without a walker, the session of the first submission whose fence the
 * device had not written.  Faults of the processor itself, of session 0
 * and a CP_HEAP exhaustion have no culprit.  Each culprit becomes
 * FAULTED.  The caller holds the device's lock and cp_lock.  Returns the
 * number of culprits.
 */
int
ghci_vm_find_culprits(
	struct ghci_device *device,
	uint32_t fault_info,
	uint32_t active,
	int halted,
	uint64_t fence)
{
	struct ghci_submit_entry *entry;
	struct ghci_vm *vm;
	enum ghci_ticket_state state;
	uint32_t kind;
	uint32_t unit;
	uint32_t session;
	uint32_t slot;
	unsigned index;
	int attributable;
	int passed;
	int found;

	/*
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 3.5, FAULT_INFO's kinds and units: a unit other than the processor
	 * reporting an invalid operation, a protection, page or permission
	 * fault is the session's work, as is a watchdog expiry; the
	 * processor's own faults and MMU_OOM are the driver's or the
	 * device's).
	 */
	kind = fault_info & GHCI_FAULT_KIND_MASK;
	unit = (fault_info >> GHCI_FAULT_UNIT_SHIFT) & GHCI_FAULT_UNIT_MASK;
	session = (fault_info >> GHCI_FAULT_SESSION_SHIFT) & GHCI_FAULT_SESSION_MASK;
	attributable = kind == GHCI_FAULT_KIND_WATCHDOG;
	if (unit != GHCI_UNIT_CP &&
	    (kind == GHCI_FAULT_KIND_INVALID_OP ||
	     kind == GHCI_FAULT_KIND_PROT_FAULT ||
	     kind == GHCI_FAULT_KIND_PAGE_FAULT ||
	     kind == GHCI_FAULT_KIND_PERM_FAULT))
		attributable = 1;

	/* Without a walker every session runs as session 0: the first submission not done is the culprit's. */
	found = 0;
	if (!has_walker(device)) {
		if (!attributable)
			return 0;
		for (index = 0U; index < device->submit_count; index++) {
			/* The oldest submission whose fence the device had not written. */
			entry = &device->submit_log[(device->submit_first + index) % GHCI_SUBMIT_LOG];
			passed = ghci_fence64_passed(fence, entry->seq);
			if (passed)
				continue;

			/* The driver's own work has no culprit. */
			vm = entry->owner;
			if (vm != NULL && vm->state == GHCI_VM_OPEN) {
				vm_fault(device, vm);
				found = 1;
			}
			break;
		}
		return found;
	}

	/* The session FAULT_INFO names; session 0 is the driver's own work. */
	if (attributable && session != 0U && session < GHCI_SESSIONS) {
		vm = device->vm[session];
		if (vm != NULL && (vm->state == GHCI_VM_OPEN || vm->state == GHCI_VM_REPLAY_WAIT)) {
			vm_fault(device, vm);
			found++;
		}
	}

	/*
	 * With PROT a session's fault ends only that session, and FAULT_INFO
	 * holds the first fault alone: a session the processor confirmed open
	 * that is no longer active faulted too.  Read only of a halted
	 * processor, and only when the first fault was a session's: after a
	 * fault of the device's own or an exhaustion of CP_HEAP (which drops
	 * sessions through the driver's count) the fault stays the device's,
	 * and is counted towards the storm.
	 */
	if (!ghci_caps_has(&device->caps, GHCI_HAS_PROT) || !halted || !attributable)
		return found;
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		/* An open session the processor confirmed opening. */
		vm = device->vm[slot];
		if (vm == NULL || vm->state != GHCI_VM_OPEN)
			continue;
		state = ghci_cp_ticket(device, &vm->open_ticket);
		if (state != GHCI_TICKET_DONE)
			continue;

		/* No longer active: it faulted. */
		if ((active & (1U << slot)) == 0U) {
			vm_fault(device, vm);
			found++;
		}
	}

	/* Succeeded: the culprits found. */
	return found;
}

/*
 * Marks every session that lost work to the reset: a submission of its
 * whose fence the device had not written (the recovery's step 4, before
 * the ring is emptied).  The caller holds cp_lock.
 */
void
ghci_vm_mark_lost(
	struct ghci_device *device,
	uint64_t fence)
{
	struct ghci_submit_entry *entry;
	unsigned index;
	int passed;

	/* Each submission the reset threw away. */
	for (index = 0U; index < device->submit_count; index++) {
		entry = &device->submit_log[(device->submit_first + index) % GHCI_SUBMIT_LOG];
		passed = ghci_fence64_passed(fence, entry->seq);
		if (!passed && entry->owner != NULL)
			entry->owner->lost_work = 1;
	}
}

/*
 * Settles the sessions after a reset (the recovery's step 6): a session
 * that was closing retires, a culprit and a session that lost work die,
 * the rest wait to be replayed; every unmap the reset ended is done (the
 * reset stopped the GPU); and CP_HEAP's count is made again from the
 * sessions that are replayed.  The caller holds the device's lock and
 * cp_lock.
 */
void
ghci_vm_after_reset(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_mapping *next;
	struct ghci_arena *arena;
	struct ghci_vm *vm;
	uint32_t slot;

	/* Each session's state. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm == NULL)
			continue;

		/* A close the reset ended is done; the count is made again below. */
		if (vm->state == GHCI_VM_CLOSING) {
			vm->counted = 0;
			vm_retire(device, vm);
			continue;
		}

		/*
		 * The reset stopped an isolated session's work: one its owner gave
		 * up retires, and one still owned is dead (its memory goes at its
		 * close, as the GPU no longer reaches it).
		 */
		if (vm->state == GHCI_VM_ISOLATED && vm->closed) {
			vm->counted = 0;
			vm_retire(device, vm);
			continue;
		}
		if (vm->state == GHCI_VM_ISOLATED) {
			vm->state = GHCI_VM_DEAD;
			vm->counted = 0;
			vm->lost_work = 0;
			continue;
		}

		/* A culprit, and a session whose work was thrown away, die. */
		if (vm->state == GHCI_VM_FAULTED || (vm->lost_work && vm->state != GHCI_VM_DEAD)) {
			vm->state = GHCI_VM_DEAD;
			vm->counted = 0;
		} else if (vm->state == GHCI_VM_OPEN || vm->state == GHCI_VM_REPLAY_WAIT) {
			vm->state = GHCI_VM_REPLAY_WAIT;
		}
		vm->lost_work = 0;
	}

	/* Every unmap the reset ended or that passed is done, and so is every mapping an isolation held. */
	for (slot = 0U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm == NULL)
			continue;
		for (mapping = vm->mappings; mapping != NULL; mapping = next) {
			next = mapping->next;
			if (mapping->state == GHCI_MAPPING_UNMAPPING || mapping->state == GHCI_MAPPING_HELD)
				mapping_drop(device, mapping);
		}
	}

	/* CP_HEAP holds nothing after the reset: the count of what the replay builds. */
	if (!has_walker(device))
		return;
	kern_memset(device->l0_marks, 0, sizeof(device->l0_marks));
	device->heap_used = ghci_heap_session_bytes(device->caps.va_bits);
	for (slot = 0U; slot < GHCI_SESSIONS; slot++) {
		/* Session 0 and the sessions the replay opens again. */
		vm = device->vm[slot];
		if (vm == NULL || (slot != 0U && vm->state != GHCI_VM_REPLAY_WAIT))
			continue;

		/* Their tables, and a level 0 table for each chunk of theirs. */
		if (slot != 0U)
			device->heap_used += ghci_heap_session_bytes(device->caps.va_bits);
		vm->counted = 1;
		kern_memset(vm->l0_marks, 0, sizeof(vm->l0_marks));
		for (arena = vm->arenas; arena != NULL; arena = arena->next) {
			device->heap_used += marks_need(device, vm, arena->first, arena->chunks);
			marks_take(device, vm, arena->first, arena->chunks);
		}
	}
}

/*
 * Replays the sessions after a reset (the recovery's step 7): session
 * 0's mappings, then each waiting session's CMD_SESSION_OPEN and live
 * mappings, and a fence that confirms the opens.  The caller holds the
 * device's lock and cp_lock, with the recovery's placement set.  Returns
 * 0, or EAGAIN when a new fault or the deadline ended a wait for room.
 */
int
ghci_vm_replay(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	uint32_t packet[GHCI_DW_SESSION_OPEN];
	uint32_t slot;
	uint32_t start;
	int error;

	/* Nothing is in a GPU without a walker. */
	if (!has_walker(device))
		return 0;

	/* Session 0 and every waiting session, in turn. */
	start = device->ring.wptr;
	error = 0;
	for (slot = 0U; slot < GHCI_SESSIONS && error == 0; slot++) {
		vm = device->vm[slot];
		if (vm == NULL || (slot != 0U && vm->state != GHCI_VM_REPLAY_WAIT))
			continue;

		/* The session opens again (session 0 is always open). */
		if (slot != 0U) {
			(void)ghci_cmd_session_open(packet, vm->session, 0U);
			error = ghci_cp_place(device, packet, GHCI_DW_SESSION_OPEN);
		}

		/* Its live mappings. */
		for (mapping = vm->mappings; mapping != NULL && error == 0; mapping = mapping->next) {
			if (mapping->state != GHCI_MAPPING_LIVE)
				continue;
			error = place_map(device, vm, mapping);
			if (error == 0)
				mapping->replayed = device->recovery_generation;
		}
	}

	/* The fence that confirms the opens. */
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
	ghci_cp_doorbell(device);
	if (error != 0)
		return error;

	/* Each replayed session's open is confirmed by that fence. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm != NULL && vm->state == GHCI_VM_REPLAY_WAIT)
			vm->open_ticket = ticket;
	}

	/* Succeeded: the replay is in the ring. */
	return 0;
}

/*
 * Reports the sessions the processor should show active after a replay:
 * session 0 and every session waiting or open.  The caller holds the
 * device's lock.
 */
uint32_t
ghci_vm_active_mask(
	struct ghci_device *device)
{
	struct ghci_vm *vm;
	uint32_t mask;
	uint32_t slot;

	/* Session 0 always. */
	mask = 1U;
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm == NULL || vm->flat)
			continue;
		if (vm->state == GHCI_VM_REPLAY_WAIT || vm->state == GHCI_VM_OPEN)
			mask |= 1U << vm->session;
	}

	/* Succeeded: the mask. */
	return mask;
}

/*
 * Opens the replayed sessions for their owners again (the end of the
 * recovery's step 8).  The caller holds the device's lock and cp_lock.
 */
void
ghci_vm_settle(
	struct ghci_device *device)
{
	struct ghci_vm *vm;
	uint32_t slot;

	/* Each waiting session is open. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm != NULL && vm->state == GHCI_VM_REPLAY_WAIT)
			vm->state = GHCI_VM_OPEN;
	}
}

/*
 * Takes the owners of the sessions that died and were not yet reported,
 * at most capacity of them, for the report to the GPU core (the
 * recovery's step 10).  The caller holds the device's lock.  Returns the
 * number taken.
 */
unsigned
ghci_vm_take_dead(
	struct ghci_device *device,
	void **owners,
	unsigned capacity)
{
	struct ghci_vm *vm;
	uint32_t slot;
	unsigned count;

	/* Each dead session once. */
	count = 0U;
	for (slot = 1U; slot < GHCI_SESSIONS && count < capacity; slot++) {
		vm = device->vm[slot];
		if (vm == NULL || vm->state != GHCI_VM_DEAD || vm->reported)
			continue;
		vm->reported = 1;
		if (vm->owner == NULL)
			continue;
		owners[count] = vm->owner;
		count++;
	}

	/* Succeeded: the owners to report. */
	return count;
}

/*
 * Isolates a session the GPU core could not confirm stopped: it takes no
 * new work, and its unmaps and close place nothing and give nothing back
 * until a reset that ends (ghci_vm_after_reset()).  A session that is no
 * longer open needs nothing.
 */
void
ghci_vm_isolate(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	/* The state changes under both locks, as every state change of a session does. */
	mutex_lock(&device->lock);
	mutex_lock(&device->cp_lock);

	if (vm->state == GHCI_VM_OPEN || vm->state == GHCI_VM_REPLAY_WAIT) {
		vm->state = GHCI_VM_ISOLATED;
		kern_logf("ghci: %s: session %u isolated\n", device->name, (unsigned)vm->slot);
	}

	mutex_unlock(&device->cp_lock);
	mutex_unlock(&device->lock);
}

/*
 * Passes the gate with the device's lock held.  Returns with the device's
 * lock held in every case, and cp_lock too when the gate is OPEN; RETRY
 * says a recovery ran meanwhile (the caller reads its state again).
 */
static enum ghci_gate
enter_ring(
	struct ghci_device *device)
{
	enum ghci_gate gate;

	/* The gate lets go of the device's lock when it waits, and it is taken again. */
	gate = ghci_cp_enter(device, GHCI_HOLD_DEVICE, 1);
	if (gate == GHCI_GATE_OPEN)
		return gate;
	mutex_lock(&device->lock);

	/* Succeeded: the caller starts again, or hears the device is lost. */
	return gate;
}

/* Reports whether the GPU walks page tables: 1 when it does. */
static int
has_walker(
	const struct ghci_device *device)
{
	/* The capability the start read. */
	return ghci_caps_has(&device->caps, GHCI_HAS_HW_WALKER);
}

/* Finds a free session number 1 to 15.  Returns it, or 0 when none is free. */
static uint32_t
free_slot(
	const struct ghci_device *device)
{
	uint32_t slot;

	/* The lowest number whose session retired. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		if (device->vm[slot] == NULL)
			return slot;
	}

	/* Every number is in use. */
	return 0U;
}

/*
 * Reports the record of level 0 tables a session's chunks are counted
 * in: its own with PROT (its own page table), the device's without (one
 * table every session shares).
 */
static uint8_t *
marks_of(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	/* One table per session with PROT. */
	if (ghci_caps_has(&device->caps, GHCI_HAS_PROT))
		return vm->l0_marks;

	/* Succeeded: the shared table's. */
	return device->l0_marks;
}

/* Reports the CP_HEAP bytes the level 0 tables of a run of chunks add for a session. */
static uint64_t
marks_need(
	struct ghci_device *device,
	struct ghci_vm *vm,
	uint32_t first,
	uint32_t chunks)
{
	uint8_t *marks;
	uint64_t need;
	uint32_t chunk;

	/* A table for each chunk not yet counted. */
	marks = marks_of(device, vm);
	need = 0U;
	for (chunk = first; chunk < first + chunks; chunk++) {
		if ((marks[chunk / 8U] & (1U << (chunk % 8U))) == 0U)
			need += GHCI_TABLE_BYTES;
	}

	/* Succeeded: the bytes. */
	return need;
}

/* Counts the level 0 tables of a run of chunks for a session. */
static void
marks_take(
	struct ghci_device *device,
	struct ghci_vm *vm,
	uint32_t first,
	uint32_t chunks)
{
	uint8_t *marks;
	uint32_t chunk;

	/* Each chunk's table is counted. */
	marks = marks_of(device, vm);
	for (chunk = first; chunk < first + chunks; chunk++)
		marks[chunk / 8U] |= (uint8_t)(1U << (chunk % 8U));
}

/* Reports how many level 0 tables a record counts. */
static unsigned
marks_count(
	const uint8_t *marks)
{
	unsigned count;
	unsigned chunk;

	/* One for each bit. */
	count = 0U;
	for (chunk = 0U; chunk < GHCI_CHUNKS; chunk++) {
		if ((marks[chunk / 8U] & (1U << (chunk % 8U))) != 0U)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Takes GPU addresses for bytes in a session: the first arena of one
 * chunk with room, or a new arena of the chunks the bytes need (its level
 * 0 tables counted in CP_HEAP's bound).  The caller holds the device's
 * lock.  Returns 0 with the address and its arena, or ENOMEM.
 */
static int
va_take(
	struct ghci_device *device,
	struct ghci_vm *vm,
	uint64_t bytes,
	uint64_t *va,
	struct ghci_arena **result)
{
	struct ghci_arena *arena;
	uint64_t need;
	uint32_t address;
	uint32_t chunks;
	uint32_t first;
	uint32_t chunk;
	uint32_t end;
	int error;

	/* A mapping within the chunks handed out; one of a chunk or less shares an arena of one chunk. */
	end = end_chunk(device);
	if (bytes == 0U || bytes > (uint64_t)(end - FIRST_CHUNK) * GHCI_L0_SPAN_BYTES)
		return ENOMEM;
	if (bytes <= GHCI_L0_SPAN_BYTES) {
		for (arena = vm->arenas; arena != NULL; arena = arena->next) {
			if (arena->chunks != 1U)
				continue;
			error = ghci_range_alloc(&arena->map, (uint32_t)bytes, GHCI_PAGE_BYTES, &address);
			if (error == 0) {
				*va = address;
				*result = arena;
				return 0;
			}
		}
	}

	/*
	 * A new arena: a run of free chunks, and room for their tables.
	 *
	 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section
	 * 11.2, a 4 KiB level 0 table for each 2 MiB of addresses a page
	 * table maps, taken from CP_HEAP; section 11.6, one table per session
	 * with PROT and one shared table without).
	 */
	chunks = (uint32_t)((bytes + GHCI_L0_SPAN_BYTES - 1U) / GHCI_L0_SPAN_BYTES);
	error = run_find(device, chunks, &first);
	if (error != 0)
		return error;
	need = marks_need(device, vm, first, chunks);
	if (device->heap_used + need > device->heap_budget)
		return ENOMEM;
	arena = kern_calloc(1U, sizeof(*arena));
	if (arena == NULL)
		return ENOMEM;

	/* The chunks are the session's, and their tables are counted. */
	arena->first = first;
	arena->chunks = chunks;
	ghci_range_init(&arena->map,
	    (uint64_t)first * GHCI_L0_SPAN_BYTES,
	    (uint64_t)(first + chunks) * GHCI_L0_SPAN_BYTES,
	    arena->extents,
	    ARENA_EXTENTS);
	for (chunk = first; chunk < first + chunks; chunk++)
		device->chunk_owner[chunk] = vm;
	marks_take(device, vm, first, chunks);
	device->heap_used += need;
	arena->next = vm->arenas;
	vm->arenas = arena;

	/* The addresses at the start of the arena. */
	(void)ghci_range_alloc(&arena->map, (uint32_t)bytes, GHCI_PAGE_BYTES, &address);

	/* Succeeded: the address. */
	*va = address;
	*result = arena;
	return 0;
}

/*
 * Gives GPU addresses back to their arena; an arena left empty gives its
 * chunks back.  Their level 0 tables stay counted: the processor keeps
 * them until the session closes (with PROT) or a reset.  The caller holds
 * the device's lock.
 */
static void
va_give(
	struct ghci_device *device,
	struct ghci_vm *vm,
	struct ghci_arena *arena,
	uint64_t va)
{
	struct ghci_arena **position;
	uint32_t chunk;

	/* The addresses go back. */
	(void)ghci_range_free(&arena->map, (uint32_t)va);
	if (arena->map.count != 0U)
		return;

	/* The empty arena leaves the session, and its chunks are free. */
	position = &vm->arenas;
	while (*position != NULL && *position != arena)
		position = &(*position)->next;
	if (*position == arena)
		*position = arena->next;
	for (chunk = arena->first; chunk < arena->first + arena->chunks; chunk++)
		device->chunk_owner[chunk] = NULL;
	kern_free(arena);
}

/*
 * Finds the first run of chunks free chunks.  Returns 0 with its first
 * chunk, or ENOMEM.
 */
static int
run_find(
	const struct ghci_device *device,
	uint32_t chunks,
	uint32_t *first)
{
	uint32_t chunk;
	uint32_t length;
	uint32_t end;

	/* First fit over the chunks the driver hands out. */
	end = end_chunk(device);
	length = 0U;
	for (chunk = FIRST_CHUNK; chunk < end; chunk++) {
		if (device->chunk_owner[chunk] != NULL) {
			length = 0U;
			continue;
		}
		length++;
		if (length == chunks) {
			*first = chunk + 1U - chunks;
			return 0;
		}
	}

	/* No run is long enough. */
	return ENOMEM;
}

/*
 * Reports the end of the chunks the driver hands out: the last chunk of
 * the GPU's VA, or of the low 4 GiB of a wider one, is never used.
 */
static uint32_t
end_chunk(
	const struct ghci_device *device)
{
	uint32_t chunks;

	/* The chunks the VA holds. */
	chunks = ghci_va_chunks(device->caps.va_bits);

	/* A VA too narrow for a chunk between the first and the last has none to hand out. */
	if (chunks <= FIRST_CHUNK + 1U)
		return FIRST_CHUNK;

	/* Succeeded: the last chunk of the VA. */
	return chunks - 1U;
}

/*
 * Removes a mapping the GPU no longer reaches: its addresses go back and
 * it lets its allocation go.  The caller holds the device's lock.
 */
static void
mapping_drop(
	struct ghci_device *device,
	struct ghci_mapping *mapping)
{
	struct ghci_mapping **position;
	struct ghci_vm *vm;

	/* The mapping leaves its session. */
	vm = mapping->vm;
	position = &vm->mappings;
	while (*position != NULL && *position != mapping)
		position = &(*position)->next;
	if (*position == mapping)
		*position = mapping->next;

	/* Its addresses, its allocation and its record go. */
	if (mapping->arena != NULL)
		va_give(device, vm, mapping->arena, mapping->va);
	ghci_memory_unhold(device, mapping->allocation);
	kern_free(mapping);
}

/*
 * Retires a session the GPU no longer reaches: its tables leave CP_HEAP's
 * count when they were counted (with PROT, its level 0 tables too), its
 * mappings and chunks go back, and its number is free.  The caller holds
 * the device's lock, or owns the device alone.
 */
static void
vm_retire(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	uint64_t bytes;

	/* The tables the close gave back to CP_HEAP. */
	if (vm->counted && !vm->flat) {
		bytes = ghci_heap_session_bytes(device->caps.va_bits);
		if (ghci_caps_has(&device->caps, GHCI_HAS_PROT))
			bytes += (uint64_t)marks_count(vm->l0_marks) * GHCI_TABLE_BYTES;
		device->heap_used -= bytes;
		vm->counted = 0;
	}

	/* Every mapping, and with the last of them every chunk. */
	while (vm->mappings != NULL)
		mapping_drop(device, vm->mappings);

	/* The number is free again. */
	vm->state = GHCI_VM_RETIRED;
	device->vm[vm->slot] = NULL;
	kern_free(vm);
}

/*
 * Makes a session the culprit of a fault: the reset fails every ticket of
 * its work the device had not finished before the fault, even one whose
 * fence the device wrote.  The caller holds the device's lock and
 * cp_lock.
 */
static void
vm_fault(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	/* The session is the culprit. */
	vm->state = GHCI_VM_FAULTED;
	kern_logf("ghci: %s: session %u faulted\n", device->name, (unsigned)vm->slot);
}

/* Places the CMD_VM_MAP of a mapping.  The caller holds cp_lock.  Returns as ghci_cp_place() does. */
static int
place_map(
	struct ghci_device *device,
	struct ghci_vm *vm,
	const struct ghci_mapping *mapping)
{
	uint32_t packet[GHCI_DW_VM_MAP];

	/* The session, the access, the GPU address and the bus address. */
	(void)ghci_cmd_vm_map(packet, vm->session, mapping->flags, mapping->va, mapping->allocation->bus, mapping->bytes);

	/* Succeeded: placed, or the placement's error. */
	return ghci_cp_place(device, packet, GHCI_DW_VM_MAP);
}

/*
 * Makes the ticket of the work placed until now: the last fence placed,
 * in the current reset generation.  The caller holds cp_lock.
 */
static void
current_ticket(
	struct ghci_device *device,
	struct ghci_ticket *ticket)
{
	unsigned long irq;

	/* The fence and the generation. */
	irq = spin_lock_irqsave(&device->irq_lock);

	ticket->seq = device->fence_placed;
	ticket->generation = device->generation;

	spin_unlock_irqrestore(&device->irq_lock, irq);
}
