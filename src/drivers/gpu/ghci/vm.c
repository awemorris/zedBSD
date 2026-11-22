/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI device's sessions and their GPU addresses: the session
 * numbers, CMD_SESSION_OPEN and CMD_SESSION_CLOSE, the mappings
 * CMD_VM_MAP and CMD_VM_UNMAP make, the memory their page tables take, and what a
 * recovery does to the sessions: the culprit of a fault, the sessions
 * that died and the replay of the rest.
 *
 * Numbers.  Session 0 is the driver's and always open; an owner takes
 * one of 1 to 15 and gives it back only once the session is retired (its
 * close's fence passed, or a reset ended the close).
 *
 * Addresses.  The driver uses the low 4 GiB of GPU addresses (all of a
 * narrower VA), cut into 2 MiB chunks.  A chunk belongs to one session at
 * a time, so that no two sessions' mappings ever meet.  A session's
 * mappings lie in arenas, runs of the chunks it owns; an arena goes back
 * when its last mapping's unmap has passed.  The first and the last
 * chunk of that range are never used.
 *
 * Page tables.  The processor builds and walks them itself, in memory
 * the driver gives it: local memory on a discrete GPU, host memory below
 * HOST_LIMIT otherwise, never the firmware's reservation.  The sizes it
 * needs come from CMD_VM_QUERY_PT_MEM at the start; session 0's initial
 * table goes with CMD_MMU_ENABLE, a client session's with
 * CMD_SESSION_OPEN.  A CMD_VM_MAP first offers no memory for
 * intermediate tables; one that needs some writes NEED_PT_MEM in its
 * completion record with the bytes and the alignment, and is placed
 * again with them.  The map waits for its record, so that no work uses a
 * mapping the processor did not make.  The processor gives tables back
 * in CMD_VM_UNMAP's record; everything a session had goes back once its
 * close has passed, and everything once a reset ended (the replay offers
 * memory again).
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

/* How long a map waits for its record before it looks again (the hang check ends a wait that never ends). */
#define MAP_WAIT_MS			1000U

/* The tries of one map at memory for its intermediate tables. */
#define MAP_TRIES			4U

/* The replay's passes over the maps that asked for memory for their tables. */
#define REPLAY_PASSES			4U

/* What a record reads before the processor writes it: no status the processor writes. */
#define RECORD_UNWRITTEN		0xFFFFFFFFU

/* The access every mapping of host memory has; the GPU keeps it coherent with the processor. */
#define HOST_MAP_FLAGS			(GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_COHERENT)

/*
 * The access every mapping of the GPU's local memory has: the processor's
 * caches have no part in it.  Its address is the local one, in
 * [MEM_VRAM_BASE, MEM_VRAM_BASE + MEM_VRAM_SIZE), which alone makes it
 * local memory.
 *
 * Values referenced: lumiere-gpu 875e54b3 ghci.md (section 7.3,
 * CMD_VM_MAP flags [4:0], [31:5] reserved, the target memory decided by
 * phys alone; section 11.3, the address routing).
 */
#define LOCAL_MAP_FLAGS			(GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE)

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
static int tables_take(struct ghci_device *device, uint64_t bytes, uint32_t align, int locked, struct ghci_tables **result);
static void tables_give(struct ghci_device *device, struct ghci_tables *tables);
static void tables_give_all(struct ghci_device *device, struct ghci_tables **list);
static void tables_keep(struct ghci_device *device, struct ghci_vm *vm, struct ghci_tables *tables, uint32_t used);
static void tables_return(struct ghci_device *device, struct ghci_vm *vm, const struct ghci_mapping *mapping);
static uint32_t record_word(const struct ghci_allocation *record, uint32_t offset);
static void record_clear(struct ghci_allocation *record);
static int map_wait(struct ghci_device *device, const struct ghci_ticket *ticket);
static int unmap_settle(struct ghci_device *device, struct ghci_vm *vm, struct ghci_mapping *mapping);
static void memory_put(struct ghci_device *device, struct ghci_allocation *allocation);
static int va_take(struct ghci_device *device, struct ghci_vm *vm, uint64_t bytes, uint64_t *va, struct ghci_arena **result);
static void va_give(struct ghci_device *device, struct ghci_vm *vm, struct ghci_arena *arena, uint64_t va);
static int run_find(const struct ghci_device *device, uint32_t chunks, uint32_t *first);
static uint32_t end_chunk(const struct ghci_device *device);
static void mapping_drop(struct ghci_device *device, struct ghci_mapping *mapping);
static void vm_retire(struct ghci_device *device, struct ghci_vm *vm);
static void vm_fault(struct ghci_device *device, struct ghci_vm *vm);
static int place_map(struct ghci_device *device, struct ghci_vm *vm, const struct ghci_mapping *mapping, const struct ghci_tables *tables);
static void current_ticket(struct ghci_device *device, struct ghci_ticket *ticket);

/*
 * Sets session 0's record at the start; its initial page table comes
 * with the MMU (ghci_vm_mmu_start()).
 */
void
ghci_vm_init(
	struct ghci_device *device)
{
	uint32_t slot;
	int walker;

	/* No number and no chunk is in use. */
	walker = has_walker(device);
	for (slot = 0U; slot < GHCI_SESSIONS; slot++)
		device->vm[slot] = NULL;
	kern_memset(device->chunk_owner, 0, sizeof(device->chunk_owner));

	/* Session 0, the driver's, is always open. */
	kern_memset(&device->vm0, 0, sizeof(device->vm0));
	device->vm0.session = 0U;
	device->vm0.flat = !walker;
	device->vm0.state = GHCI_VM_OPEN;
	device->vm[0] = &device->vm0;
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

	/* Session 0's mappings and page-table memory go. */
	while (device->vm0.mappings != NULL)
		mapping_drop(device, device->vm0.mappings);
	tables_give_all(device, &device->vm0.tables);
	tables_give_all(device, &device->vm0.root);
}

/*
 * Opens a GHCI session for owner (the GPU core's session to report a
 * loss to): takes a number and places CMD_SESSION_OPEN with the memory of
 * the session's initial page table.
 *
 * Returns 0 with the session, EBUSY when all fifteen numbers are in use,
 * ENOMEM when there is no memory for its record or its initial page
 * table, or EIO when the device is lost.
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
	unsigned long irq;
	int lost;
	int error;

	/* The session's record. */
	vm = kern_calloc(1U, sizeof(*vm));
	if (vm == NULL)
		return ENOMEM;
	vm->owner = owner;

	/* The memory of its initial page table, taken before the device's lock (local memory takes it). */
	if (has_walker(device)) {
		error = tables_take(device, device->root_bytes, device->root_align, 0, &vm->root);
		if (error != 0) {
			kern_free(vm);
			return ENOMEM;
		}
	}

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

		/* A free number. */
		slot = free_slot(device);
		if (slot == 0U) {
			ghci_cp_leave(device);
			error = EBUSY;
			break;
		}

		/*
		 * CMD_SESSION_OPEN with the initial page table, and a fence that
		 * says the processor opened it.  The memory is given whether or
		 * not the device has PROT, so that every session has a table of
		 * its own wherever the processor takes it.
		 *
		 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
		 * CMD_SESSION_OPEN: init_pt_pa and init_pt_bytes; section 11.2, the
		 * per-session initial table of the bring-up).
		 */
		start = device->ring.wptr;
		(void)ghci_cmd_session_open(packet, slot, 0U, vm->root->address, vm->root->bytes);
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

		/* The session is open. */
		vm->slot = slot;
		vm->session = slot;
		vm->state = GHCI_VM_OPEN;
		vm->open_ticket = ticket;
		device->vm[slot] = vm;
		ghci_cp_leave(device);
		break;
	}

	/* An open that failed leaves nothing behind. */
	if (error != 0) {
		tables_give_all(device, &vm->root);
		mutex_unlock(&device->lock);
		kern_free(vm);
		return error;
	}

	mutex_unlock(&device->lock);

	/* Succeeded: the session is the owner's. */
	*result = vm;
	return 0;
}

/*
 * Closes a session; it cannot fail.  CMD_SESSION_CLOSE and a fence are
 * placed, and the session retires once that fence passes (or a reset
 * ends it): its number, chunks, mappings, page tables and memory go back
 * then.  A session a reset killed, or one of a lost
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
	uint32_t packet[GHCI_DW_SESSION_CLOSE];
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
		 * CMD_SESSION_CLOSE, which ends every mapping of the session, then
		 * the fence after which its page-table memory comes back.
		 *
		 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 3.3,
		 * CMD_SESSION_CLOSE and a fence reclaim the session's initial and
		 * intermediate tables; section 11.2, CMD_SESSION_CLOSE destroys the
		 * session's context).
		 */
		start = device->ring.wptr;
		(void)ghci_cmd_session_close(packet, vm->session);
		error = ghci_cp_place(device, packet, GHCI_DW_SESSION_CLOSE);
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
			mapping->unmap_placed = 0;
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
 * session owns, places CMD_VM_MAP and a fence, and waits for the map's
 * record.  A map that needs memory for intermediate page tables is
 * placed again with the bytes its record asks for; a reset meanwhile
 * places it again too.  flags adds GHCI_VM_FLAG_EXEC for code and
 * GHCI_VM_MAP_NO_WRITE for memory the GPU only reads (the mapping then
 * lacks GHCI_VM_FLAG_WRITE); the rest of the access and the coherence
 * are the driver's.  The caller holds no lock of the device and does not
 * close the session meanwhile.
 *
 * Local memory is mapped at its local address (which tells the GPU it is local),
 * and never coherent with the processor.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
 * CMD_VM_MAP: the record is visible once a later fence passes, a map
 * that needs memory changes nothing and writes NEED_PT_MEM with
 * required_bytes and required_align, the driver waits for OK before work
 * uses the mapping, the rest of the memory given comes back at once).
 *
 * Returns 0 with the mapping, which holds the allocation; ENOTSUP for
 * local memory on a GPU without a page-table walker (whose addresses are
 * bus addresses); ENOMEM when no addresses or no memory for the record
 * or the tables are left; or EIO when the session or the device is lost.
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
	struct ghci_tables *offered;
	struct ghci_arena *arena;
	struct ghci_ticket ticket;
	enum ghci_gate gate;
	uint32_t required;
	uint32_t align;
	uint32_t status;
	uint32_t start;
	unsigned tries;
	unsigned long irq;
	uint64_t va;
	int placed;
	int lost;
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

	/* The page the processor writes the map's and the unmap's records to, with a walker. */
	if (!vm->flat) {
		error = ghci_memory_alloc(device, GHCI_PAGE_BYTES, 0U, &mapping->record);
		if (error != 0) {
			kern_free(mapping);
			return ENOMEM;
		}
	}

	/* Mappings that finished unmapping give their addresses back first. */
	mutex_lock(&device->lock);

	(void)ghci_vm_reclaim(device);

	/*
	 * Without a walker the address is the bus address, and nothing is
	 * placed; the gate still waits out a recovery and refuses a lost
	 * device.
	 */
	offered = NULL;
	arena = NULL;
	va = 0U;
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
		/* The addresses, CMD_VM_MAP and its fence in one section past the gate, then the wait for the record. */
		tries = 0U;
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

			/* Addresses in a chunk of the session's, kept over the tries. */
			if (arena == NULL) {
				error = va_take(device, vm, mapping->bytes, &va, &arena);
				if (error != 0) {
					ghci_cp_leave(device);
					break;
				}
				mapping->va = va;
				mapping->arena = arena;
			}

			/* The packet and its fence; a recovery that began places the map again. */
			record_clear(mapping->record);
			start = device->ring.wptr;
			error = place_map(device, vm, mapping, offered);
			if (error == 0)
				error = ghci_cp_place_fence(device, NULL, start, &ticket);
			ghci_cp_doorbell(device);
			ghci_cp_leave(device);
			if (error == EAGAIN)
				continue;
			if (error != 0)
				break;

			/* The record, once the fence passed; a reset meanwhile places the map again. */
			mutex_unlock(&device->lock);
			error = map_wait(device, &ticket);
			mutex_lock(&device->lock);
			if (error != 0) {
				irq = spin_lock_irqsave(&device->irq_lock);

				lost = device->state == GHCI_STATE_LOST;

				spin_unlock_irqrestore(&device->irq_lock, irq);
				if (lost)
					break;
				continue;
			}

			/* Made: the memory offered is the processor's as far as it kept it. */
			status = record_word(mapping->record, GHCI_MAP_COMP_STATUS);
			if (status == GHCI_MAP_OK) {
				if (offered != NULL)
					tables_keep(device, vm, offered, record_word(mapping->record, GHCI_MAP_COMP_USED));
				offered = NULL;
				error = 0;
				break;
			}

			/* A session the processor no longer has, or addresses it says are taken. */
			if (status != GHCI_MAP_NEED_PT_MEM) {
				kern_logf("ghci: %s: CMD_VM_MAP of session %u at %llx answered %u\n",
				    device->name,
				    (unsigned)vm->session,
				    (unsigned long long)mapping->va,
				    (unsigned)status);
				error = EIO;
				break;
			}

			/* Memory for its tables, as much as the record asks, outside the device's lock. */
			required = record_word(mapping->record, GHCI_MAP_COMP_REQUIRED);
			align = record_word(mapping->record, GHCI_MAP_COMP_ALIGN);
			if (offered != NULL)
				tables_give(device, offered);
			offered = NULL;
			tries++;
			if (tries >= MAP_TRIES) {
				error = ENOMEM;
				break;
			}
			mutex_unlock(&device->lock);
			error = tables_take(device, required, align, 0, &offered);
			mutex_lock(&device->lock);
			if (error != 0) {
				error = ENOMEM;
				break;
			}
		}
	}

	/* The mapping joins its session and holds the allocation. */
	placed = error == 0;
	if (placed) {
		mapping->next = vm->mappings;
		vm->mappings = mapping;
		allocation->reached = 1;
		ghci_memory_hold(allocation);
	}

	/* A map that failed gives back its addresses and the memory it offered. */
	if (!placed) {
		if (arena != NULL)
			va_give(device, vm, arena, va);
		if (offered != NULL)
			tables_give(device, offered);
		if (mapping->record != NULL)
			memory_put(device, mapping->record);
	}

	mutex_unlock(&device->lock);

	/* A map that failed leaves nothing behind. */
	if (!placed) {
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
 * that fence passes (with the intermediate tables its record gives
 * back).  Without a walker nothing is placed: the mapping
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

		/*
		 * CMD_VM_UNMAP with the mapping's record page for the tables it
		 * gives back, and its fence.
		 *
		 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
		 * CMD_VM_UNMAP: comp_pa on 64 bytes, comp_bytes in whole 64 bytes).
		 */
		record_clear(mapping->record);
		start = device->ring.wptr;
		(void)ghci_cmd_vm_unmap(packet, vm->session, mapping->va, mapping->bytes, mapping->record->bus, GHCI_PAGE_BYTES);
		error = ghci_cp_place(device, packet, GHCI_DW_VM_UNMAP);
		if (error == 0)
			error = ghci_cp_place_fence(device, NULL, start, &ticket);
		ghci_cp_doorbell(device);
		if (error == 0) {
			mapping->state = GHCI_MAPPING_UNMAPPING;
			mapping->ticket = ticket;
			mapping->unmap_placed = 1;
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
	int unmapped;
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

		/* A mapping whose unmap passed goes, with the tables its record gives back. */
		for (mapping = vm->mappings; mapping != NULL; mapping = next) {
			next = mapping->next;
			if (mapping->state != GHCI_MAPPING_UNMAPPING)
				continue;
			state = ghci_cp_ticket(device, &mapping->ticket);
			if (state == GHCI_TICKET_DONE && mapping->unmap_placed) {
				unmapped = unmap_settle(device, vm, mapping);
				if (!unmapped)
					continue;
			}
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
 * device had not written.  Faults of the processor itself and of session
 * 0 have no culprit.  Each culprit becomes
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
	 * processor's own faults are the driver's or the device's).
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
	 * fault of the device's own the fault stays the device's, and is
	 * counted towards the storm.
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
 * reset stopped the GPU); and the memory of every intermediate table
 * goes back, as the processor forgot every page table (the initial
 * tables are kept for the replay).  The caller holds the device's lock
 * and cp_lock.
 */
void
ghci_vm_after_reset(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_mapping *next;
	struct ghci_vm *vm;
	uint32_t slot;

	/* Each session's state. */
	for (slot = 1U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm == NULL)
			continue;

		/* A close the reset ended is done. */
		if (vm->state == GHCI_VM_CLOSING) {
			vm_retire(device, vm);
			continue;
		}

		/*
		 * The reset stopped an isolated session's work: one its owner gave
		 * up retires, and one still owned is dead (its memory goes at its
		 * close, as the GPU no longer reaches it).
		 */
		if (vm->state == GHCI_VM_ISOLATED && vm->closed) {
			vm_retire(device, vm);
			continue;
		}
		if (vm->state == GHCI_VM_ISOLATED) {
			vm->state = GHCI_VM_DEAD;
			vm->lost_work = 0;
			continue;
		}

		/* A culprit, and a session whose work was thrown away, die. */
		if (vm->state == GHCI_VM_FAULTED || (vm->lost_work && vm->state != GHCI_VM_DEAD))
			vm->state = GHCI_VM_DEAD;
		else if (vm->state == GHCI_VM_OPEN || vm->state == GHCI_VM_REPLAY_WAIT)
			vm->state = GHCI_VM_REPLAY_WAIT;
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

	/* The processor forgot every page table: the intermediate tables' memory goes back. */
	for (slot = 0U; slot < GHCI_SESSIONS; slot++) {
		vm = device->vm[slot];
		if (vm != NULL)
			tables_give_all(device, &vm->tables);
	}
}

/*
 * Replays the sessions after a reset (the recovery's step 7): the MMU
 * with session 0's initial table and the fault log ring, session 0's
 * mappings, then each waiting session's CMD_SESSION_OPEN and live
 * mappings, and a fence that confirms the opens.  The maps offer no
 * memory for intermediate tables; ghci_vm_replay_tables() reads their
 * records and gives what they ask.  The caller holds the device's lock
 * and cp_lock, with the recovery's placement set.  Returns 0, or EAGAIN
 * when a new fault or the deadline ended a wait for room.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 5, the
 * recovery after CTRL.RESET: the fault log ring set up again, the MMU
 * enabled again with the same memory, the sessions opened and mapped
 * again).
 */
int
ghci_vm_replay(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	uint32_t packet[GHCI_DW_FAULT_LOG_SETUP];
	uint32_t slot;
	uint32_t start;
	int error;

	/* Nothing is in a GPU without a walker. */
	if (!has_walker(device))
		return 0;

	/* The MMU and the fault log ring the reset turned off. */
	start = device->ring.wptr;
	(void)ghci_cmd_mmu_enable(packet, device->vm0.root->address, device->vm0.root->bytes);
	error = ghci_cp_place(device, packet, GHCI_DW_MMU_ENABLE);
	if (error == 0) {
		(void)ghci_cmd_fault_log_setup(packet, device->fault_ring->bus, GHCI_FAULT_RING_ENTRIES);
		error = ghci_cp_place(device, packet, GHCI_DW_FAULT_LOG_SETUP);
		device->fault_seq = 1U;
	}

	/* Session 0 and every waiting session, in turn. */
	for (slot = 0U; slot < GHCI_SESSIONS && error == 0; slot++) {
		vm = device->vm[slot];
		if (vm == NULL || (slot != 0U && vm->state != GHCI_VM_REPLAY_WAIT))
			continue;

		/* The session opens again with its initial table (session 0 is always open). */
		if (slot != 0U) {
			(void)ghci_cmd_session_open(packet, vm->session, 0U, vm->root->address, vm->root->bytes);
			error = ghci_cp_place(device, packet, GHCI_DW_SESSION_OPEN);
		}

		/* Its live mappings, each with its record to read. */
		for (mapping = vm->mappings; mapping != NULL && error == 0; mapping = mapping->next) {
			if (mapping->state != GHCI_MAPPING_LIVE)
				continue;
			record_clear(mapping->record);
			error = place_map(device, vm, mapping, NULL);
			if (error == 0) {
				mapping->replayed = device->recovery_generation;
				mapping->pending = 1;
			}
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
 * Ends the replay's maps (the recovery's step 7, after ghci_vm_replay()):
 * waits for the last fence placed, reads each replayed map's record, and
 * places again with memory for its tables every map that asked for some,
 * with a fence, until none asks.  The caller holds the device's lock and
 * cp_lock, with the recovery's placement set.  Returns 0; EAGAIN when a
 * new fault or the deadline ended a wait; ETIMEDOUT when a fence did not
 * come; ENOMEM when the memory asked for cannot be had; or EIO when a
 * record says what a replay cannot answer.
 */
int
ghci_vm_replay_tables(
	struct ghci_device *device)
{
	struct ghci_mapping *mapping;
	struct ghci_tables *offered;
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	uint32_t status;
	uint32_t slot;
	uint32_t start;
	unsigned pass;
	int placed;
	int error;

	/* Nothing is in a GPU without a walker. */
	if (!has_walker(device))
		return 0;

	for (pass = 0U; pass < REPLAY_PASSES; pass++) {
		/* The records of what was placed are visible once its fence passed. */
		error = ghci_cp_await_fence(device, device->fence_placed, GHCI_LIVENESS_MS);
		if (error != 0)
			return ETIMEDOUT;

		/* Each map still to be read, of session 0 and the waiting sessions. */
		start = device->ring.wptr;
		placed = 0;
		for (slot = 0U; slot < GHCI_SESSIONS; slot++) {
			vm = device->vm[slot];
			if (vm == NULL || (slot != 0U && vm->state != GHCI_VM_REPLAY_WAIT))
				continue;
			for (mapping = vm->mappings; mapping != NULL; mapping = mapping->next) {
				if (!mapping->pending)
					continue;

				/* Made: the memory offered is kept as far as the processor used it. */
				status = record_word(mapping->record, GHCI_MAP_COMP_STATUS);
				offered = mapping->offered;
				mapping->offered = NULL;
				if (status == GHCI_MAP_OK) {
					if (offered != NULL)
						tables_keep(device, vm, offered, record_word(mapping->record, GHCI_MAP_COMP_USED));
					mapping->pending = 0;
					continue;
				}
				if (offered != NULL)
					tables_give(device, offered);
				if (status != GHCI_MAP_NEED_PT_MEM)
					return EIO;

				/* Placed again with the memory its record asks for. */
				error = tables_take(device,
				    record_word(mapping->record, GHCI_MAP_COMP_REQUIRED),
				    record_word(mapping->record, GHCI_MAP_COMP_ALIGN),
				    1,
				    &offered);
				if (error != 0)
					return ENOMEM;
				mapping->offered = offered;
				record_clear(mapping->record);
				error = place_map(device, vm, mapping, offered);
				if (error != 0)
					return error;
				placed = 1;
			}
		}

		/* Every map is made. */
		if (!placed)
			return 0;

		/* The fence after the maps placed again. */
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
		ghci_cp_doorbell(device);
		if (error != 0)
			return error;
	}

	/* The maps kept asking for memory. */
	return EIO;
}

/*
 * Starts the MMU, once the ring runs and before any session or mapping:
 * asks the processor for the sizes of its page-table memory
 * (CMD_VM_QUERY_PT_MEM), gives session 0's initial table with
 * CMD_MMU_ENABLE, and sets up the fault log ring, each waited for by a
 * fence.  A GPU without a walker needs none of it.  The caller owns the
 * device alone.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 6.0, the MMU
 * off after a reset and the bring-up; section 7.3, CMD_VM_QUERY_PT_MEM,
 * GhciVmPtMemInfo (struct_bytes 64, sizes and alignments), CMD_MMU_ENABLE
 * and CMD_FAULT_LOG_SETUP; section 11.2, the bring-up sequence).
 *
 * Returns 0, ENOMEM, ETIMEDOUT when a fence did not come, or EIO for
 * sizes the driver cannot use.
 */
int
ghci_vm_mmu_start(
	struct ghci_device *device)
{
	struct ghci_allocation *info;
	struct ghci_ticket ticket;
	uint32_t packet[GHCI_DW_FAULT_LOG_SETUP];
	uint32_t size;
	uint32_t start;
	int error;

	/* A GPU without a walker runs with the MMU off. */
	if (!has_walker(device))
		return 0;

	/* The sizes, written to a page of host memory. */
	error = ghci_memory_alloc(device, GHCI_PAGE_BYTES, 0U, &info);
	if (error != 0)
		return ENOMEM;
	record_clear(info);
	mutex_lock(&device->cp_lock);

	start = device->ring.wptr;
	(void)ghci_cmd_vm_query_pt_mem(packet, info->bus, GHCI_PT_INFO_BYTES);
	error = ghci_cp_place(device, packet, GHCI_DW_VM_QUERY_PT_MEM);
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
	if (error == 0)
		ghci_cp_doorbell(device);

	mutex_unlock(&device->cp_lock);
	if (error == 0)
		error = ghci_cp_await_fence(device, ticket.seq, GHCI_LIVENESS_MS);
	if (error != 0) {
		kern_logf("ghci: %s: page-table sizes not written (%d)\n", device->name, error);
		return ETIMEDOUT;
	}

	/* The sizes; a record of another size, a zero size or an alignment not a power of two is refused. */
	size = record_word(info, 0U);
	device->root_bytes = record_word(info, GHCI_PT_INFO_INIT_BYTES);
	device->root_align = record_word(info, GHCI_PT_INFO_INIT_ALIGN);
	device->table_bytes = record_word(info, GHCI_PT_INFO_TABLE_BYTES);
	device->table_align = record_word(info, GHCI_PT_INFO_TABLE_ALIGN);
	memory_put(device, info);
	if (size != GHCI_PT_INFO_BYTES || device->root_bytes == 0U || device->table_bytes == 0U ||
	    device->root_align == 0U || (device->root_align & (device->root_align - 1U)) != 0U ||
	    device->table_align == 0U || (device->table_align & (device->table_align - 1U)) != 0U) {
		kern_logf("ghci: %s: page-table sizes not usable (%u bytes: root %u on %u, tables %u on %u)\n",
		    device->name,
		    (unsigned)size,
		    (unsigned)device->root_bytes,
		    (unsigned)device->root_align,
		    (unsigned)device->table_bytes,
		    (unsigned)device->table_align);
		return EIO;
	}

	/* Session 0's initial table, and the fault log ring. */
	error = tables_take(device, device->root_bytes, device->root_align, 0, &device->vm0.root);
	if (error != 0)
		return ENOMEM;
	error = ghci_memory_alloc(device, GHCI_PAGE_BYTES, 0U, &device->fault_ring);
	if (error != 0)
		return ENOMEM;

	/* CMD_MMU_ENABLE, CMD_FAULT_LOG_SETUP and a fence. */
	mutex_lock(&device->cp_lock);

	start = device->ring.wptr;
	(void)ghci_cmd_mmu_enable(packet, device->vm0.root->address, device->vm0.root->bytes);
	error = ghci_cp_place(device, packet, GHCI_DW_MMU_ENABLE);
	if (error == 0) {
		(void)ghci_cmd_fault_log_setup(packet, device->fault_ring->bus, GHCI_FAULT_RING_ENTRIES);
		error = ghci_cp_place(device, packet, GHCI_DW_FAULT_LOG_SETUP);
	}
	if (error == 0)
		error = ghci_cp_place_fence(device, NULL, start, &ticket);
	if (error == 0)
		ghci_cp_doorbell(device);
	device->fault_seq = 1U;

	mutex_unlock(&device->cp_lock);
	if (error == 0)
		error = ghci_cp_await_fence(device, ticket.seq, GHCI_LIVENESS_MS);
	if (error != 0) {
		kern_logf("ghci: %s: MMU not enabled (%d)\n", device->name, error);
		return ETIMEDOUT;
	}

	/* Succeeded: the MMU is on, with session 0's table. */
	kern_logf("ghci: %s: MMU on (initial table %u bytes, tables %u bytes)\n",
	    device->name,
	    (unsigned)device->root_bytes,
	    (unsigned)device->table_bytes);
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
 * Takes GPU addresses for bytes in a session: the first arena of one
 * chunk with room, or a new arena of the chunks the bytes need.  The
 * caller holds the device's lock.  Returns 0 with the address and its
 * arena, or ENOMEM.
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
	uint32_t address;
	uint32_t chunks;
	uint32_t first;
	uint32_t chunk;
	uint32_t end;
	int error;

	/* A mapping within the chunks handed out; one of a chunk or less shares an arena of one chunk. */
	end = end_chunk(device);
	if (bytes == 0U || bytes > (uint64_t)(end - FIRST_CHUNK) * GHCI_CHUNK_BYTES)
		return ENOMEM;
	if (bytes <= GHCI_CHUNK_BYTES) {
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

	/* A new arena: a run of free chunks. */
	chunks = (uint32_t)((bytes + GHCI_CHUNK_BYTES - 1U) / GHCI_CHUNK_BYTES);
	error = run_find(device, chunks, &first);
	if (error != 0)
		return error;
	arena = kern_calloc(1U, sizeof(*arena));
	if (arena == NULL)
		return ENOMEM;

	/* The chunks are the session's. */
	arena->first = first;
	arena->chunks = chunks;
	ghci_range_init(&arena->map,
	    (uint64_t)first * GHCI_CHUNK_BYTES,
	    (uint64_t)(first + chunks) * GHCI_CHUNK_BYTES,
	    arena->extents,
	    ARENA_EXTENTS);
	for (chunk = first; chunk < first + chunks; chunk++)
		device->chunk_owner[chunk] = vm;
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
 * chunks back.  The caller holds the device's lock.
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

	/* Its addresses, its allocation, the memory it offered for tables, its record page and its record go. */
	if (mapping->arena != NULL)
		va_give(device, vm, mapping->arena, mapping->va);
	ghci_memory_unhold(device, mapping->allocation);
	if (mapping->offered != NULL)
		tables_give(device, mapping->offered);
	if (mapping->record != NULL)
		memory_put(device, mapping->record);
	kern_free(mapping);
}

/*
 * Retires a session the GPU no longer reaches: its mappings and chunks go
 * back, the memory of its page tables goes back, and its number is
 * free.  The caller holds the device's lock, or owns the device alone.
 */
static void
vm_retire(
	struct ghci_device *device,
	struct ghci_vm *vm)
{
	/* Every mapping, and with the last of them every chunk. */
	while (vm->mappings != NULL)
		mapping_drop(device, vm->mappings);

	/* The page tables the processor no longer has. */
	tables_give_all(device, &vm->tables);
	tables_give_all(device, &vm->root);

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

/*
 * Places the CMD_VM_MAP of a mapping, offering tables for its
 * intermediate page tables (none when NULL), with its record in the
 * mapping's record page.  The caller holds cp_lock.  Returns as
 * ghci_cp_place() does.
 */
static int
place_map(
	struct ghci_device *device,
	struct ghci_vm *vm,
	const struct ghci_mapping *mapping,
	const struct ghci_tables *tables)
{
	struct ghci_map_args args;
	uint32_t packet[GHCI_DW_VM_MAP];

	/* The session, the access, the GPU address, the bus address, the tables and the record. */
	args.session = vm->session;
	args.flags = mapping->flags;
	args.va = mapping->va;
	args.bus = mapping->allocation->bus;
	args.bytes = mapping->bytes;
	args.pt = 0U;
	args.pt_bytes = 0U;
	if (tables != NULL) {
		args.pt = tables->address;
		args.pt_bytes = tables->bytes;
	}
	args.comp = mapping->record->bus;
	(void)ghci_cmd_vm_map(packet, &args);

	/* Succeeded: placed, or the placement's error. */
	return ghci_cp_place(device, packet, GHCI_DW_VM_MAP);
}

/*
 * Takes memory for page tables: bytes on align, in local memory on a
 * discrete GPU and in host memory otherwise (below the driver's limit,
 * which keeps under HOST_LIMIT), never in the firmware's reservation (the
 * local allocator keeps it out).  The run is taken larger when align is
 * above a page, and address is its first aligned byte.  locked says that
 * the caller holds the device's lock.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 11.2, page
 * table memory in [MEM_VRAM_BASE, MEM_VRAM_BASE + MEM_VRAM_SIZE) outside
 * MEM_FW_VRAM on a discrete GPU, host memory below MEM_HOST_LIMIT on a
 * unified one).
 *
 * Returns 0 with the record of the run, ENOTSUP on a discrete GPU whose
 * local memory the driver does not hand out, or ENOMEM.
 */
static int
tables_take(
	struct ghci_device *device,
	uint64_t bytes,
	uint32_t align,
	int locked,
	struct ghci_tables **result)
{
	struct ghci_tables *tables;
	struct ghci_allocation *memory;
	uint64_t extra;
	int error;

	/* A size the commands can carry, and an alignment that is a power of two. */
	if (bytes == 0U || bytes > 0xFFFFFFFFULL || align == 0U || (align & (align - 1U)) != 0U)
		return ENOMEM;

	/* Room to align the start within the run. */
	extra = 0U;
	if (align > GHCI_PAGE_BYTES)
		extra = align - GHCI_PAGE_BYTES;

	/* Local memory on a discrete GPU, host memory on a unified one. */
	tables = kern_calloc(1U, sizeof(*tables));
	if (tables == NULL)
		return ENOMEM;
	if (device->caps.local_bytes != 0U) {
		if (locked)
			error = ghci_memory_alloc_local_locked(device, bytes + extra, &memory);
		else
			error = ghci_memory_alloc_local(device, bytes + extra, &memory);
	} else {
		error = ghci_memory_alloc(device, bytes + extra, 0U, &memory);
	}
	if (error != 0) {
		kern_free(tables);
		return error;
	}

	/* Succeeded: the aligned start of the run. */
	tables->memory = memory;
	tables->address = (memory->bus + align - 1U) & ~(uint64_t)(align - 1U);
	tables->bytes = (uint32_t)bytes;
	*result = tables;
	return 0;
}

/* Gives back memory of page tables the processor no longer has.  The caller holds the device's lock or owns it alone. */
static void
tables_give(
	struct ghci_device *device,
	struct ghci_tables *tables)
{
	/* Succeeded: the run and its record go. */
	memory_put(device, tables->memory);
	kern_free(tables);
}

/* Gives back every run of a list.  The caller holds the device's lock or owns it alone. */
static void
tables_give_all(
	struct ghci_device *device,
	struct ghci_tables **list)
{
	struct ghci_tables *tables;

	/* Each run in turn. */
	while (*list != NULL) {
		tables = *list;
		*list = tables->next;
		tables_give(device, tables);
	}
}

/*
 * Keeps the memory a CMD_VM_MAP was offered as far as the processor used
 * it: used bytes join the session's tables, and memory it did not use
 * goes back at once.  The caller holds the device's lock.
 */
static void
tables_keep(
	struct ghci_device *device,
	struct ghci_vm *vm,
	struct ghci_tables *tables,
	uint32_t used)
{
	/* None used: it goes back. */
	if (used == 0U) {
		tables_give(device, tables);
		return;
	}

	/* Succeeded: the session keeps it until the processor gives it back. */
	tables->used = used;
	tables->returned = 0U;
	tables->next = vm->tables;
	vm->tables = tables;
}

/*
 * Counts the intermediate tables a CMD_VM_UNMAP's record gives back
 * against the runs of the session they lie in; a run all of whose used
 * memory came back goes.  The caller holds the device's lock.
 */
static void
tables_return(
	struct ghci_device *device,
	struct ghci_vm *vm,
	const struct ghci_mapping *mapping)
{
	struct ghci_tables **position;
	struct ghci_tables *tables;
	uint64_t address;
	uint32_t written;
	uint32_t entry;
	uint32_t at;
	uint32_t bytes;

	/* The entries the record holds. */
	written = record_word(mapping->record, GHCI_UNMAP_COMP_WRITTEN);
	for (entry = 0U; entry < written; entry++) {
		/* One entry: the address and the bytes of a table given back. */
		at = GHCI_UNMAP_COMP_ENTRIES + entry * GHCI_UNMAP_ENTRY_BYTES;
		if (at + GHCI_UNMAP_ENTRY_BYTES > GHCI_PAGE_BYTES)
			break;
		address = record_word(mapping->record, at) | ((uint64_t)record_word(mapping->record, at + 4U) << 32);
		bytes = record_word(mapping->record, at + 8U);

		/* The run it lies in counts it; a run whose used memory all came back goes. */
		for (position = &vm->tables; *position != NULL; position = &(*position)->next) {
			tables = *position;
			if (address < tables->address || address >= tables->address + tables->bytes)
				continue;
			tables->returned += bytes;
			if (tables->returned >= tables->used) {
				*position = tables->next;
				tables_give(device, tables);
			}
			break;
		}
	}
}

/*
 * Reads a passed unmap's record: the tables it gave back are counted,
 * and the mapping may go (1).  A record buffer too small for the tables
 * left the mapping as it was: it stays live until its session closes or
 * a reset ends (0), which the log says.  The caller holds the device's
 * lock.
 *
 * Values referenced: lumiere-gpu 78d1b6b7 ghci.md (section 7.3,
 * CMD_VM_UNMAP: NEED_BUF leaves the mapping and the TLBs as they were;
 * INVALID_SESSION for a session the processor no longer has).
 */
static int
unmap_settle(
	struct ghci_device *device,
	struct ghci_vm *vm,
	struct ghci_mapping *mapping)
{
	uint32_t status;

	/* Done: the tables it gave back. */
	status = record_word(mapping->record, GHCI_UNMAP_COMP_STATUS);
	if (status == GHCI_UNMAP_OK) {
		tables_return(device, vm, mapping);
		return 1;
	}

	/* A session the processor no longer has keeps nothing of it. */
	if (status != GHCI_UNMAP_NEED_BUF)
		return 1;

	/* Too many tables for the record page: the mapping stays until its session goes. */
	kern_logf("ghci: %s: CMD_VM_UNMAP of session %u at %llx needs %u bytes for its record; kept until the session goes\n",
	    device->name,
	    (unsigned)vm->session,
	    (unsigned long long)mapping->va,
	    (unsigned)record_word(mapping->record, GHCI_UNMAP_COMP_REQUIRED));
	mapping->state = GHCI_MAPPING_LIVE;
	mapping->unmap_placed = 0;
	return 0;
}

/* Reads a 32-bit word the processor wrote to a record page. */
static uint32_t
record_word(
	const struct ghci_allocation *record,
	uint32_t offset)
{
	const volatile uint32_t *word;

	/* Succeeded: the word, read through the record's view. */
	word = (const volatile uint32_t *)(const void *)(record->view + offset);
	return *word;
}

/* Marks a record page unwritten before a command writes to it. */
static void
record_clear(
	struct ghci_allocation *record)
{
	volatile uint32_t *word;

	/* The first word reads as no status until the processor writes it. */
	word = (volatile uint32_t *)(void *)record->view;
	*word = RECORD_UNWRITTEN;
}

/*
 * Waits for a map's fence, again while it is not answered (the hang
 * check ends a fence that never comes).  The caller holds no lock of the
 * device.  Returns 0 once it passed, or EIO when a reset or the loss of
 * the device ended it.
 */
static int
map_wait(
	struct ghci_device *device,
	const struct ghci_ticket *ticket)
{
	int error;

	/* Until the ticket is answered. */
	do {
		error = ghci_cp_wait(device, ticket, MAP_WAIT_MS);
	} while (error == ETIMEDOUT);

	/* Succeeded: passed, or ended. */
	return error;
}

/*
 * Gives a run of host or local memory back, or keeps it in the
 * quarantine while a lost GPU may still reach it.  The caller holds the
 * device's lock or owns the device alone.
 */
static void
memory_put(
	struct ghci_device *device,
	struct ghci_allocation *allocation)
{
	unsigned long irq;
	int lost;
	int error;

	/* The device's life, as the recovery last set it. */
	irq = spin_lock_irqsave(&device->irq_lock);

	lost = device->state == GHCI_STATE_LOST;

	spin_unlock_irqrestore(&device->irq_lock, irq);

	/* Memory a lost GPU may still reach waits for bus mastering to go. */
	if (lost) {
		ghci_memory_quarantine(device, allocation);
		return;
	}

	/* Succeeded: the run goes back, or stays in the quarantine when its view cannot go. */
	error = ghci_memory_free(device, allocation);
	if (error != 0)
		ghci_memory_quarantine(device, allocation);
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
