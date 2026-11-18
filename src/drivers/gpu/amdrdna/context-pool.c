/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hub contexts the GPU's address spaces share, and their identifiers.
 *
 * The graphics hub translates for sixteen contexts.  Context 0 is the
 * kernel's; contexts 1 to 15 are handed out while a session's address
 * space runs work, and taken back for another space once every ring has
 * finished the work the context ran, the least recently used first.  A
 * space keeps its context between runs as long as nobody needs it, so
 * its cached translations are dropped only when its tables changed or the
 * context changed hands.  Each space also has an identifier the interrupt
 * controller attaches to the page faults of its context, so a fault names
 * the space even after the context moved on.
 *
 * Nothing here touches the hardware: the caller reads the rings' fences
 * and writes the commands, so the same code runs in the host tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-hw.h"

static bool slot_idle_elsewhere(const struct amdrdna_context_slot *slot, unsigned ring, const uint32_t *passed, bool *sync);
static bool slot_needs_flush(const struct amdrdna_context_slot *slot, const struct amdrdna_context_request *request, const uint32_t *passed);
static void slot_forget(struct amdrdna_context_slot *slot);

/*
 * Empties a context pool: every user context free, the clock and the
 * counts at zero.  Context 0 is never handed out.
 */
void
amdrdna_context_pool_init(
	struct amdrdna_context_pool *pool)
{
	unsigned context;

	/* Every slot nobody's, with no work behind it. */
	for (context = 0U; context < AMDRDNA_CONTEXTS; context++) {
		slot_forget(&pool->slots[context]);
		pool->slots[context].stamp = 0U;
		pool->slots[context].held = false;
	}

	/* The kernel's context is never in the pool. */
	pool->slots[0].held = true;

	/* No use yet. */
	pool->clock = 0U;
	pool->flushes = 0U;
	pool->takeovers = 0U;
}

/*
 * Finds the context a run of an address space uses.
 *
 * passed holds, for each tracked ring, the fence the ring last wrote.  The
 * space's own context is kept when it still has it; it is dropped
 * (grant->flush) when the space's tables changed since the last drop, or
 * when the last drop was put on another ring that has not passed it yet.
 * Otherwise the least recently used context whose work on the other rings
 * has ended is taken and dropped; work of it still on the run's own ring
 * only makes the run wait for that ring first (grant->sync).  When every
 * context still runs work elsewhere, grant->wait names the fence of the
 * least recently used one the caller waits for before asking again.
 *
 * Returns 0, or ENOSPC when every context is held.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/amd/amdgpu/amdgpu_ids.c (amdgpu_vmid_grab,
 * amdgpu_vmid_grab_used, amdgpu_vmid_grab_idle: the least recently used
 * order, the fences of other rings, the flush after a context changed
 * hands or the tables were updated, the last flush on another ring).
 */
int
amdrdna_context_grant(
	struct amdrdna_context_pool *pool,
	const struct amdrdna_context_request *request,
	const uint32_t *passed,
	struct amdrdna_context_grant *grant)
{
	struct amdrdna_context_slot *slot;
	struct amdrdna_context_slot *oldest;
	unsigned context;
	unsigned chosen;
	unsigned ring;
	bool idle;
	bool sync;

	/* Nothing granted yet. */
	grant->context = 0U;
	grant->flush = false;
	grant->sync = false;
	grant->wait = false;
	grant->wait_ring = 0U;
	grant->wait_sequence = 0U;

	/* The space keeps the context it held when it still has it. */
	if (request->held >= AMDRDNA_FIRST_USER_CONTEXT && request->held < AMDRDNA_CONTEXTS) {
		slot = &pool->slots[request->held];
		if (!slot->held &&
		    slot->owner == request->owner &&
		    slot->table == request->table) {
			grant->context = request->held;
			grant->flush = slot_needs_flush(slot, request, passed);
			return 0;
		}
	}

	/* Takes the least recently used context whose work elsewhere has ended. */
	chosen = 0U;
	sync = false;
	for (context = AMDRDNA_FIRST_USER_CONTEXT; context < AMDRDNA_CONTEXTS; context++) {
		/* A held context is never handed out. */
		slot = &pool->slots[context];
		if (slot->held)
			continue;

		/* Only a context whose other rings are done. */
		idle = slot_idle_elsewhere(slot, request->ring, passed, &sync);
		if (!idle)
			continue;

		/* The least recently used of them. */
		if (chosen == 0U || slot->stamp < pool->slots[chosen].stamp)
			chosen = context;
	}

	/* A free context: it changes hands and its translations are dropped. */
	if (chosen != 0U) {
		slot = &pool->slots[chosen];
		(void)slot_idle_elsewhere(slot, request->ring, passed, &sync);
		if (slot->owner != 0U && slot->owner != request->owner)
			pool->takeovers++;
		slot->owner = request->owner;
		slot->table = request->table;
		slot->flushed = 0U;
		slot->flush_known = false;
		grant->context = chosen;
		grant->flush = true;
		grant->sync = sync;
		return 0;
	}

	/* No context is free: the least recently used one that is not held is waited for. */
	oldest = NULL;
	for (context = AMDRDNA_FIRST_USER_CONTEXT; context < AMDRDNA_CONTEXTS; context++) {
		/* A held context is never waited for. */
		slot = &pool->slots[context];
		if (slot->held)
			continue;

		/* The least recently used. */
		if (oldest == NULL || slot->stamp < oldest->stamp)
			oldest = slot;
	}

	/* Every context is held: nothing can be had. */
	if (oldest == NULL)
		return ENOSPC;

	/* The first ring of it other than the run's own whose work has not ended. */
	for (ring = 0U; ring < AMDRDNA_TRACKED_RINGS; ring++) {
		/* The run's own ring orders itself. */
		if (ring == request->ring)
			continue;

		/* A ring that ran nothing, or whose work ended, is not waited for. */
		if (!oldest->used[ring])
			continue;
		idle = amdrdna_fence_passed(passed[ring], oldest->last[ring]);
		if (idle)
			continue;

		/* The fence the caller waits for. */
		grant->wait = true;
		grant->wait_ring = ring;
		grant->wait_sequence = oldest->last[ring];
		break;
	}

	/* Succeeded: the caller waits and asks again. */
	return 0;
}

/*
 * Records a run a context was granted for: the fence after it on its
 * ring, its place as the most recently used context, and, when its
 * translations were dropped before it, the generation they were dropped
 * for and the drop's fence.
 */
void
amdrdna_context_submitted(
	struct amdrdna_context_pool *pool,
	unsigned context,
	const struct amdrdna_context_request *request,
	uint32_t sequence,
	bool flushed)
{
	struct amdrdna_context_slot *slot;

	/* A context outside the pool records nothing. */
	if (context < AMDRDNA_FIRST_USER_CONTEXT || context >= AMDRDNA_CONTEXTS)
		return;
	if (request->ring >= AMDRDNA_TRACKED_RINGS)
		return;

	/* The work's fence on its ring, and the context's place in the order of use. */
	slot = &pool->slots[context];
	slot->used[request->ring] = true;
	slot->last[request->ring] = sequence;
	pool->clock++;
	slot->stamp = pool->clock;

	/* The drop before the run, and what it covered. */
	if (flushed) {
		slot->flushed = request->generation;
		slot->flush_known = true;
		slot->flush_ring = request->ring;
		slot->flush_sequence = sequence;
		pool->flushes++;
	}
}

/*
 * Reports whether every ring has finished the work a context ran: no
 * fence it recorded is ahead of the ring's.
 */
bool
amdrdna_context_idle(
	const struct amdrdna_context_pool *pool,
	unsigned context,
	const uint32_t *passed)
{
	const struct amdrdna_context_slot *slot;
	unsigned ring;
	bool done;

	/* A context outside the pool runs nothing of a space. */
	if (context < AMDRDNA_FIRST_USER_CONTEXT || context >= AMDRDNA_CONTEXTS)
		return true;

	/* Each ring the context ran work on. */
	slot = &pool->slots[context];
	for (ring = 0U; ring < AMDRDNA_TRACKED_RINGS; ring++) {
		/* A ring that ran nothing in it. */
		if (!slot->used[ring])
			continue;

		/* A ring that has not passed the context's last work. */
		done = amdrdna_fence_passed(passed[ring], slot->last[ring]);
		if (!done)
			return false;
	}

	/* Succeeded: the context runs nothing. */
	return true;
}

/*
 * Gives back the context an address space held when the space goes: it
 * is nobody's from now on, so a new space with the same identifier never
 * finds it as its own.  The fences of its work stay, so it is handed out
 * again only once that work has ended.
 */
void
amdrdna_context_release(
	struct amdrdna_context_pool *pool,
	unsigned context,
	uint32_t owner)
{
	struct amdrdna_context_slot *slot;

	/* A context outside the pool, or one the space no longer has. */
	if (context < AMDRDNA_FIRST_USER_CONTEXT || context >= AMDRDNA_CONTEXTS)
		return;
	slot = &pool->slots[context];
	if (slot->owner != owner)
		return;

	/* Nobody's: the next space to take it drops its translations. */
	slot->owner = 0U;
	slot->table = 0U;
	slot->flushed = 0U;
	slot->flush_known = false;
}

/*
 * Keeps a context out of the pool for good: it may still run work nobody
 * can stop, under translations nobody may change.
 */
void
amdrdna_context_hold(
	struct amdrdna_context_pool *pool,
	unsigned context)
{
	/* A context outside the pool is never handed out anyway. */
	if (context < AMDRDNA_FIRST_USER_CONTEXT || context >= AMDRDNA_CONTEXTS)
		return;

	/* Held: neither granted nor waited for. */
	pool->slots[context].held = true;
}

/* Counts the contexts the pool may still hand out. */
unsigned
amdrdna_context_free_count(
	const struct amdrdna_context_pool *pool)
{
	unsigned context;
	unsigned count;

	/* Every user context that is not held. */
	count = 0U;
	for (context = AMDRDNA_FIRST_USER_CONTEXT; context < AMDRDNA_CONTEXTS; context++) {
		if (!pool->slots[context].held)
			count++;
	}

	/* Succeeded: the count. */
	return count;
}

/*
 * Reports whether a ring's fence has reached a sequence: the difference,
 * read as signed, handles the sequence's wrap.
 */
bool
amdrdna_fence_passed(
	uint32_t fence,
	uint32_t sequence)
{
	/* The fence is at or past the sequence. */
	if ((int32_t)(fence - sequence) >= 0)
		return true;

	/* The fence is still behind. */
	return false;
}

/*
 * Empties the set of address-space identifiers: none in use, the search
 * starting at 1.
 */
void
amdrdna_space_ids_init(
	struct amdrdna_space_ids *ids)
{
	unsigned word;

	/* Every bit clear. */
	for (word = 0U; word < AMDRDNA_SPACE_ID_WORDS; word++)
		ids->used[word] = 0U;

	/* Identifier 0 names no space and is never handed out. */
	ids->used[0] = 1U;
	ids->next = 1U;
	ids->count = 0U;
}

/*
 * Takes a free address-space identifier, searching from the one after
 * the last taken, so an identifier just given back is not reused at once.
 *
 * Returns 0, or ENOSPC when all 65535 are in use.
 */
int
amdrdna_space_id_take(
	struct amdrdna_space_ids *ids,
	uint32_t *id)
{
	uint32_t candidate;
	uint32_t tried;

	/* Looks at each identifier once, from where the last search stopped. */
	candidate = ids->next;
	for (tried = 0U; tried < AMDRDNA_SPACE_IDS; tried++) {
		/* Wraps past the last identifier, skipping 0. */
		if (candidate >= AMDRDNA_SPACE_IDS)
			candidate = 1U;

		/* A free one is taken. */
		if ((ids->used[candidate / 32U] & (1U << (candidate % 32U))) == 0U) {
			ids->used[candidate / 32U] |= 1U << (candidate % 32U);
			ids->next = candidate + 1U;
			ids->count++;
			*id = candidate;
			return 0;
		}

		/* The next one. */
		candidate++;
	}

	/* Every identifier is in use. */
	return ENOSPC;
}

/* Gives an address-space identifier back; 0 and a free one are ignored. */
void
amdrdna_space_id_give(
	struct amdrdna_space_ids *ids,
	uint32_t id)
{
	/* Identifier 0 and identifiers outside the range are never in use. */
	if (id == 0U || id >= AMDRDNA_SPACE_IDS)
		return;

	/* One that is not in use is left alone. */
	if ((ids->used[id / 32U] & (1U << (id % 32U))) == 0U)
		return;

	/* Free again. */
	ids->used[id / 32U] &= ~(1U << (id % 32U));
	ids->count--;
}

/*
 * Reports whether a context's work on every ring but the run's has ended;
 * *sync is set when its work on the run's own ring has not.
 */
static bool
slot_idle_elsewhere(
	const struct amdrdna_context_slot *slot,
	unsigned ring,
	const uint32_t *passed,
	bool *sync)
{
	unsigned other;
	bool done;

	/* Each ring the context ran work on. */
	*sync = false;
	for (other = 0U; other < AMDRDNA_TRACKED_RINGS; other++) {
		/* A ring that ran nothing in it. */
		if (!slot->used[other])
			continue;

		/* A ring whose work in it ended. */
		done = amdrdna_fence_passed(passed[other], slot->last[other]);
		if (done)
			continue;

		/* The run's own ring only makes the run wait for it first. */
		if (other == ring) {
			*sync = true;
			continue;
		}

		/* Another ring still runs work in it. */
		return false;
	}

	/* Succeeded: no other ring runs work in it. */
	return true;
}

/*
 * Reports whether a space's own context needs its translations dropped
 * before a run: its tables changed since the last drop, or the last drop
 * went on another ring that has not passed it yet.
 */
static bool
slot_needs_flush(
	const struct amdrdna_context_slot *slot,
	const struct amdrdna_context_request *request,
	const uint32_t *passed)
{
	bool done;

	/* No drop for this generation of the tables yet. */
	if (!slot->flush_known)
		return true;
	if (slot->flushed != request->generation)
		return true;

	/* A drop on the run's own ring is ahead of the run anyway. */
	if (slot->flush_ring == request->ring)
		return false;

	/* A drop on another ring counts only once that ring passed it. */
	done = amdrdna_fence_passed(passed[slot->flush_ring], slot->flush_sequence);
	if (!done)
		return true;

	/* The context's translations are current. */
	return false;
}

/* Makes a slot nobody's, with no work and no drop recorded. */
static void
slot_forget(
	struct amdrdna_context_slot *slot)
{
	unsigned ring;

	/* Nobody's, with nothing dropped. */
	slot->owner = 0U;
	slot->table = 0U;
	slot->flushed = 0U;
	slot->flush_known = false;
	slot->flush_ring = 0U;
	slot->flush_sequence = 0U;

	/* No ring ran work in it. */
	for (ring = 0U; ring < AMDRDNA_TRACKED_RINGS; ring++) {
		slot->used[ring] = false;
		slot->last[ring] = 0U;
	}
}
