/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "ufs-consistency.h"

#include <errno.h>
#include <string.h>

#define SECTOR_SIZE 512U
#define DESC_MAGIC 0x4a534655U /* UFSJ */
#define COMMIT_MAGIC 0x434a4655U /* UFJC */
#define JOURNAL_VERSION 2U

static uint32_t
checksum(const void *buffer, size_t length)
{
	const uint8_t *bytes = buffer;
	uint32_t value = 2166136261U;
	size_t index;
	for (index = 0; index < length; index++) {
		value ^= bytes[index];
		value *= 16777619U;
	}
	return value;
}

static void put32(uint8_t *p,uint32_t v)
{ p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24); }
static void put64(uint8_t *p,uint64_t v)
{ put32(p,(uint32_t)v);put32(p+4,(uint32_t)(v>>32)); }
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint64_t get64(const uint8_t *p)
{ return get32(p)|(uint64_t)get32(p+4)<<32; }

static int
clear_record(struct ufs_journal *journal, uint64_t sector)
{
	uint8_t zero[SECTOR_SIZE];
	int error;
	memset(zero,0,sizeof(zero));
	error=journal->io.write(journal->io.context,sector,1,zero);
	return error!=0?error:journal->io.flush(journal->io.context);
}

#define GROUP_VERSION JOURNAL_VERSION
#define GROUP_HEADER 32U
#define GROUP_ENTRY 16U
#define IMAGE_READERS_CLOSED (UINT32_C(1) << 31)

static uint32_t group_checksum(uint8_t *descriptor);
static int journal_finish(struct ufs_journal *journal);
static int group_validate(struct ufs_journal *journal, uint8_t *descriptor);
static int journal_replay(struct ufs_journal *journal, uint64_t expected_sequence, uint32_t expected_digest, int apply, uint8_t *view);
static void journal_close_views(struct ufs_journal *journal);
static int journal_view_transfer(const struct ufs_journal_view *view, uint64_t first, uint32_t count, void *buffer, int copy);

/*
 * Initializes the bounded multi-target journal using checked volume geometry.
 */
int
ufs_journal_init(
	struct ufs_journal *journal,
	const struct ufs_journal_io *io,
	uint64_t first,
	uint32_t count,
	uint64_t home_sectors)
{
	/* Checks media callbacks and disjoint home/locator/journal geometry. */
	if (journal == NULL || io == NULL || io->read == NULL ||
	    io->write == NULL || io->flush == NULL || count < 3U ||
	    first > UINT64_MAX - count || home_sectors == 0 || home_sectors >= first)
		return EINVAL;
	memset(journal, 0, sizeof(*journal));
	journal->io = *io;
	journal->first_sector = first;
	journal->sector_count = count;
	journal->next_sequence = 1;
	journal->home_sectors = home_sectors;
	journal->image_readers = IMAGE_READERS_CLOSED;
	return 0;
}

/*
 * Binds owner-accounted redo storage before any transaction is admitted.
 */
int
ufs_journal_bind_image(
	struct ufs_journal *journal,
	void *image,
	size_t bytes)
{
	/* Rejects incomplete storage and live ownership without changing the binding. */
	if (journal == NULL || (image == NULL && bytes != 0) ||
	    (image != NULL && bytes < UFS_JOURNAL_IMAGE_BYTES))
		return EINVAL;
	if (journal->pending_sequence != 0)
		return EBUSY;
	journal_close_views(journal);
	if (ufs_journal_views_busy(journal))
		return EBUSY;
	journal->image = image;
	journal->image_valid = 0;

	/* Reports exclusive storage ready for the next publication or boot replay. */
	return 0;
}

/*
 * Publishes durable redo without installing any home extent.
 */
int
ufs_journal_publishv(
	struct ufs_journal *journal,
	const struct ufs_journal_extent *extents,
	unsigned count)
{
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint8_t *entry;
	uint64_t cursor;
	uint32_t total;
	unsigned index;
	int error;

	/* Validates every extent and the complete footprint before modifying the slot. */
	if (journal == NULL ||
	    extents == NULL || count == 0 || count > UFS_JOURNAL_EXTENTS)
		return EINVAL;
	if (journal->poisoned)
		return EIO;
	if (journal->pending_sequence != 0)
		return EBUSY;
	if (ufs_journal_views_busy(journal))
		return EBUSY;
	if (journal->next_sequence == 0 || journal->next_sequence == UINT64_MAX)
		return EOVERFLOW;
	memset(descriptor, 0, sizeof(descriptor));
	put32(descriptor, DESC_MAGIC);
	put32(descriptor + 4, GROUP_VERSION);
	put64(descriptor + 8, journal->next_sequence);
	put32(descriptor + 16, count);
	total = 0;
	for (index = 0; index < count; index++) {
		if (extents[index].payload == NULL || extents[index].sectors == 0 ||
		    extents[index].sectors > UFS_JOURNAL_GROUP_SECTORS - total)
			return EINVAL;
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put64(entry, extents[index].target);
		put32(entry + 8, extents[index].sectors);
		total += extents[index].sectors;
	}
	put32(descriptor + 20, total);
	put32(descriptor + 28, group_checksum(descriptor));
	error = group_validate(journal, descriptor);
	if (error != 0)
		return EINVAL;

	/* Refuses a live slot instead of overwriting committed or unresolved ownership. */
	error = journal->io.read(journal->io.context, journal->first_sector, 1, commit);
	if (error != 0)
		return error;
	if (get32(commit) != 0)
		return EBUSY;

	/* Binds each payload only after all addresses and lengths have passed validation. */
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		put32(entry + 12, checksum(extents[index].payload,
		    (size_t)extents[index].sectors * SECTOR_SIZE));
	}
	put32(descriptor + 28, group_checksum(descriptor));
	memset(commit, 0, sizeof(commit));
	put32(commit, COMMIT_MAGIC);
	put32(commit + 4, GROUP_VERSION);
	put64(commit + 8, journal->next_sequence++);
	put32(commit + 16, get32(descriptor + 28));
	put32(commit + 24, checksum(commit, 24));

	/* Retains uncertain ownership before the first write can reach media. */
	journal->pending_sequence = get64(descriptor + 8);
	journal->pending_digest = get32(descriptor + 28);
	journal->pending_ready = 0;
	journal->image_valid = 0;

	/* Makes old commit evidence unreachable before publishing immutable redo bytes. */
	error = clear_record(journal, journal->first_sector + journal->sector_count - 1U);
	if (error == 0)
		error = journal->io.write(journal->io.context, journal->first_sector, 1, descriptor);
	cursor = journal->first_sector + 1U;
	for (index = 0; error == 0 && index < count; index++) {
		error = journal->io.write(journal->io.context, cursor,
		    extents[index].sectors, extents[index].payload);
		cursor += extents[index].sectors;
	}
	if (error == 0)
		error = journal->io.flush(journal->io.context);
	if (error == 0)
		error = journal->io.write(journal->io.context,
		    journal->first_sector + journal->sector_count - 1U, 1, commit);
	if (error == 0)
		error = journal->io.flush(journal->io.context);

	/* Confirms this exact commit before exposing redo to metadata readers. */
	if (error == 0)
		error = journal_replay(journal, journal->pending_sequence,
		    journal->pending_digest, 0, NULL);
	return error;
}

/*
 * Publishes and checkpoints one group for synchronous callers.
 */
int
ufs_journal_commitv(
	struct ufs_journal *journal,
	const struct ufs_journal_extent *extents,
	unsigned count)
{
	int error;

	/* Never recover a different caller's already pending group as a side effect. */
	if (journal != NULL && journal->pending_sequence != 0)
		return journal->poisoned ? EIO : EBUSY;

	/* Preserves the operation error even when recovery establishes a safe slot. */
	error = ufs_journal_publishv(journal, extents, count);
	if (error == 0)
		error = ufs_journal_checkpoint(journal);
	if (error != 0 && journal != NULL && journal->pending_sequence != 0 &&
	    ufs_journal_replay(journal) != 0) {
		journal->poisoned = 1;
		journal_close_views(journal);
	}
	return error;
}

/*
 * Installs a verified pending group while retaining its witness on failure.
 */
int
ufs_journal_checkpoint(
	struct ufs_journal *journal)
{
	/* Requires recovery to resolve an interrupted publication before normal reuse. */
	if (journal == NULL)
		return EINVAL;
	if (journal->pending_sequence == 0)
		return 0;
	if (!journal->pending_ready)
		return EBUSY;
	if (journal->pending_clearing)
		return journal_finish(journal);
	return journal_replay(journal, journal->pending_sequence,
	    journal->pending_digest, 1, NULL);
}

/*
 * Resolves retained checkpoint work without hiding an initial device failure.
 */
int
ufs_journal_drain(struct ufs_journal *journal)
{
	int error;

	/* A poisoned owner requires remount recovery, even if no slot is pending. */
	if (journal == NULL)
		return EINVAL;
	if (journal->poisoned)
		return EIO;
	error = ufs_journal_checkpoint(journal);
	if (error != 0 && journal->pending_sequence != 0 &&
	    ufs_journal_replay(journal) != 0) {
		journal->poisoned = 1;
		journal_close_views(journal);
	}
	return error;
}

/* Uses the grouped durability protocol for a single extent. */
int
ufs_journal_commit(
	struct ufs_journal *journal,
	uint64_t target,
	const void *payload,
	uint32_t sectors)
{
	struct ufs_journal_extent extent;

	/* Shares validation, ordering and recovery with multi-target callers. */
	extent.target = target;
	extent.sectors = sectors;
	extent.payload = payload;
	return ufs_journal_commitv(journal, &extent, 1);
}

/* Checks the entire descriptor without including its own checksum field. */
static uint32_t
group_checksum(
	uint8_t *descriptor)
{
	uint32_t saved;
	uint32_t digest;

	/* Restores the immutable caller image after sampling its checksum. */
	saved = get32(descriptor + 28);
	put32(descriptor + 28, 0);
	digest = checksum(descriptor, SECTOR_SIZE);
	put32(descriptor + 28, saved);
	return digest;
}

/* Rejects malformed, overlapping or out-of-volume redo before any home write. */
static int
group_validate(
	struct ufs_journal *journal,
	uint8_t *descriptor)
{
	uint64_t target;
	uint64_t previous;
	uint32_t sectors;
	uint32_t total;
	uint32_t count;
	unsigned index;
	unsigned other;
	const uint8_t *entry;
	const uint8_t *prior;

	/* Validates the fixed header and bounded record count. */
	count = get32(descriptor + 16);
	if (get32(descriptor) != DESC_MAGIC || get32(descriptor + 4) != GROUP_VERSION ||
	    get64(descriptor + 8) == 0 || get64(descriptor + 8) == UINT64_MAX ||
	    count == 0 || count > UFS_JOURNAL_EXTENTS ||
	    get32(descriptor + 28) != group_checksum(descriptor))
		return EIO;

	/* Checks all addresses before reading payloads or changing persistent homes. */
	total = 0;
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		target = get64(entry);
		sectors = get32(entry + 8);
		if (sectors == 0 || sectors > UFS_JOURNAL_GROUP_SECTORS - total ||
		    target >= journal->home_sectors || sectors > journal->home_sectors - target)
			return EIO;
		for (other = 0; other < index; other++) {
			prior = descriptor + GROUP_HEADER + other * GROUP_ENTRY;
			previous = get64(prior);
			if (target < previous + get32(prior + 8) && previous < target + sectors)
				return EIO;
		}
		total += sectors;
	}
	if (total != get32(descriptor + 20) || total > journal->sector_count - 2U)
		return EIO;
	return 0;
}

/*
 * Replays a committed group, validating every byte before touching homes.
 */
int
ufs_journal_replay(
	struct ufs_journal *journal)
{
	/* Boot recovery may discard incomplete redo without claiming a new commit. */
	if (journal == NULL)
		return EINVAL;
	if (journal->pending_clearing)
		return journal_finish(journal);
	return journal_replay(journal,
	    journal->pending_ready ? journal->pending_sequence : 0,
	    journal->pending_ready ? journal->pending_digest : 0, 1, NULL);
}

/* Requires a caller's committed identity when checkpoint follows publication. */
static int
journal_replay(
	struct ufs_journal *journal,
	uint64_t expected_sequence,
	uint32_t expected_digest,
	int apply,
	uint8_t *view)
{
	uint8_t descriptor[SECTOR_SIZE];
	uint8_t commit[SECTOR_SIZE];
	uint8_t sector[SECTOR_SIZE];
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t sequence;
	uint32_t digest;
	uint32_t count;
	uint32_t offset;
	uint32_t closed;
	unsigned index;
	unsigned part;
	unsigned byte;
	int error;

	/* Refuses a missing owner before reading its reserved slot. */
	if (journal == NULL)
		return EINVAL;

	/* Reuses only the immutable image of this owner's verified pending identity. */
	if (journal->image_valid) {
		memcpy(descriptor, journal->image, SECTOR_SIZE);
		sequence = get64(descriptor + 8);
		if (sequence != expected_sequence || get32(descriptor + 28) != expected_digest)
			return EIO;
		count = get32(descriptor + 16);
	} else {
		/* Retired readers still own the old bytes even after its slot was cleared. */
		if (ufs_journal_views_busy(journal))
			return EBUSY;
		/* An empty descriptor is the durable terminal state of the slot. */
		error = journal->io.read(journal->io.context, journal->first_sector, 1, descriptor);
		if (error != 0)
			return error;
		if (get32(descriptor) == 0) {
			if (expected_sequence != 0)
				return EIO;
			journal->pending_sequence = 0;
			journal->pending_digest = 0;
			journal->pending_ready = 0;
			return 0;
		}
		error = group_validate(journal, descriptor);
		if (error != 0)
			return error;
		/* A writer must verify its own group, not merely any valid redo transaction. */
		sequence = get64(descriptor + 8);
		if (expected_sequence != 0 &&
		    (sequence != expected_sequence || get32(descriptor + 28) != expected_digest))
			return EIO;
		count = get32(descriptor + 16);
		error = journal->io.read(journal->io.context,
		    journal->first_sector + journal->sector_count - 1U, 1, commit);
		if (error != 0)
			return error;

		/* Drops uncommitted redo, whose home blocks have never been installed. */
		if (get32(commit) != COMMIT_MAGIC || get32(commit + 4) != GROUP_VERSION ||
		    get64(commit + 8) != sequence || get32(commit + 16) != get32(descriptor + 28) ||
		    get32(commit + 24) != checksum(commit, 24)) {
			if (expected_sequence != 0)
				return EIO;
			error = clear_record(journal, journal->first_sector);
			if (error == 0) {
				journal->pending_sequence = 0;
				journal->pending_digest = 0;
				journal->pending_ready = 0;
			}
			return error;
		}

		/* Fetches one bounded immutable payload image when the owner supplied storage. */
		if (journal->image != NULL) {
			error = journal->io.read(journal->io.context, journal->first_sector + 1U,
			    get32(descriptor + 20), journal->image + SECTOR_SIZE);
			if (error != 0)
				return error;
		}

		/* Validates all payload extents before the first home write. */
		offset = SECTOR_SIZE;
		cursor = journal->first_sector + 1U;
		for (index = 0; index < count; index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			digest = 2166136261U;
			for (part = 0; part < get32(entry + 8); part++) {
				if (journal->image != NULL) {
					memcpy(sector, journal->image + offset, SECTOR_SIZE);
					offset += SECTOR_SIZE;
				} else {
					error = journal->io.read(journal->io.context, cursor++, 1, sector);
					if (error != 0)
						return error;
				}
				for (byte = 0; byte < SECTOR_SIZE; byte++) {
					digest ^= sector[byte];
					digest *= 16777619U;
				}
			}
			if (digest != get32(entry + 12))
				return EIO;
		}

		if (journal->image != NULL) {
			memcpy(journal->image, descriptor, SECTOR_SIZE);
			journal->image_valid = 1;
		}
	}

	/* Keeps the verified identity strict across any later failed home installation. */
	journal->pending_sequence = sequence;
	journal->pending_digest = get32(descriptor + 28);
	journal->pending_ready = 1;
	journal->committed_sequence = sequence;
	journal->committed_digest = journal->pending_digest;

	/* Opens acquisition once, after every immutable byte and witness is validated. */
	if (journal->image_valid && !journal->poisoned) {
		closed = IMAGE_READERS_CLOSED;

		(void)__atomic_compare_exchange_n(&journal->image_readers, &closed, 0,
		    0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
	}
	if (!apply) {
		if (view != NULL)
			memcpy(view, descriptor, SECTOR_SIZE);
		return 0;
	}

	/* Installs checked homes, then releases the slot only after their flush succeeds. */
	offset = SECTOR_SIZE;
	cursor = journal->first_sector + 1U;
	for (index = 0; index < count; index++) {
		entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
		if (journal->image_valid) {
			error = journal->io.write(journal->io.context, get64(entry),
			    get32(entry + 8), journal->image + offset);
			if (error != 0)
				return error;
			offset += get32(entry + 8) * SECTOR_SIZE;
			continue;
		}
		for (part = 0; part < get32(entry + 8); part++) {
			error = journal->io.read(journal->io.context, cursor++, 1, sector);
			if (error == 0)
				error = journal->io.write(journal->io.context, get64(entry) + part, 1, sector);
			if (error != 0)
				return error;
		}
	}
	error = journal->io.flush(journal->io.context);
	if (error != 0)
		return error;

	/* Homes are durable even if clearing the descriptor has an uncertain result. */
	journal->pending_clearing = 1;
	return journal_finish(journal);
}

/* Retries only slot retirement after home durability is already established. */
static int
journal_finish(
	struct ufs_journal *journal)
{
	int error;

	/* Keeps the home-durable witness until clearing also crosses its flush boundary. */
	error = clear_record(journal, journal->first_sector);
	if (error != 0)
		return error;
	journal_close_views(journal);
	if (journal->pending_sequence >= journal->next_sequence)
		journal->next_sequence = journal->pending_sequence + 1U;
	journal->pending_sequence = 0;
	journal->pending_digest = 0;
	journal->pending_ready = 0;
	journal->pending_clearing = 0;
	journal->image_valid = 0;
	return 0;
}

/*
 * Reads coherent home sectors through a verified pending redo group.
 */
int
ufs_journal_read(
	struct ufs_journal *journal,
	uint64_t first,
	uint32_t count,
	void *buffer)
{
	uint8_t descriptor[SECTOR_SIZE];
	const uint8_t *entry;
	uint64_t cursor;
	uint64_t current;
	uint64_t source;
	uint64_t target;
	uint32_t sectors;
	uint32_t done;
	uint32_t run;
	unsigned index;
	int error;

	/* Bounds the complete caller buffer before issuing any partial read. */
	if (journal == NULL || buffer == NULL || count == 0 ||
	    count > UFS_JOURNAL_GROUP_SECTORS || first >= journal->home_sectors ||
	    count > journal->home_sectors - first)
		return EINVAL;
	if (journal->poisoned)
		return EIO;
	if (journal->pending_sequence == 0 || journal->pending_clearing)
		return journal->io.read(journal->io.context, first, count, buffer);
	if (!journal->pending_ready)
		return EBUSY;

	/* Validates the entire pending group before exposing any of its payloads. */
	error = journal_replay(journal, journal->pending_sequence,
	    journal->pending_digest, 0, descriptor);
	if (error != 0)
		return error;

	/* Coalesces each home gap or redo extent instead of issuing one read per sector. */
	done = 0;
	while (done < count) {
		current = first + done;
		source = current;
		run = count - done;
		cursor = journal->first_sector + 1U;
		for (index = 0; index < get32(descriptor + 16); index++) {
			entry = descriptor + GROUP_HEADER + index * GROUP_ENTRY;
			target = get64(entry);
			sectors = get32(entry + 8);
			if (current >= target && current - target < sectors) {
				source = cursor + current - target;
				if (run > sectors - (current - target))
					run = (uint32_t)(sectors - (current - target));
				break;
			}
			if (target > current && target - current < run)
				run = (uint32_t)(target - current);
			cursor += sectors;
		}
		if (journal->image_valid && source >= journal->first_sector + 1U) {
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			    journal->image + (size_t)(source - journal->first_sector) * SECTOR_SIZE,
			    (size_t)run * SECTOR_SIZE);
		} else {
			error = journal->io.read(journal->io.context, source, run,
			    (uint8_t *)buffer + (size_t)done * SECTOR_SIZE);
			if (error != 0)
				return error;
		}
		done += run;
	}
	return 0;
}

/*
 * Reports positive commit proof independently of the current pending slot.
 */
int
ufs_journal_committed(
	const struct ufs_journal *journal,
	uint64_t sequence,
	uint32_t digest)
{
	/* Rejects absent and unissued witnesses without confusing them with a commit. */
	if (journal == NULL || sequence == 0)
		return 0;
	if (journal->committed_sequence != sequence)
		return 0;
	if (journal->committed_digest != digest)
		return 0;
	return 1;
}

/* Closes new acquisition without revoking readers that already own the image. */
static void
journal_close_views(struct ufs_journal *journal)
{
	(void)__atomic_fetch_or(&journal->image_readers, IMAGE_READERS_CLOSED, __ATOMIC_ACQ_REL);
}

/* Stops admission for an owner that will drain readers before destroying backing. */
void
ufs_journal_views_close(struct ufs_journal *journal)
{
	if (journal != NULL)
		journal_close_views(journal);
}

/* Reports backing ownership independently of whether the durable slot is empty. */
int
ufs_journal_views_busy(const struct ufs_journal *journal)
{
	uint32_t readers;

	if (journal == NULL)
		return 0;
	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);
	return (readers & ~IMAGE_READERS_CLOSED) != 0;
}

/* Pins one validated generation without waiting for checkpoint device I/O. */
int
ufs_journal_view_acquire(struct ufs_journal *journal, struct ufs_journal_view *view)
{
	uint32_t readers;

	/* Requires a fresh handle so repeated acquisition cannot lose a reference. */
	if (journal == NULL || view == NULL)
		return EINVAL;
	if (view->journal != NULL)
		return EBUSY;
	readers = __atomic_load_n(&journal->image_readers, __ATOMIC_ACQUIRE);
	for (;;) {
		if ((readers & IMAGE_READERS_CLOSED) != 0)
			return ENOENT;
		if (readers == IMAGE_READERS_CLOSED - 1U)
			return EOVERFLOW;
		if (__atomic_compare_exchange_n(&journal->image_readers, &readers,
		    readers + 1U, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			break;
	}

	/* The acquired count prevents pointer rebinding and payload reuse. */
	view->journal = journal;
	view->image = journal->image;
	view->sequence = get64(view->image + 8);
	view->home_sectors = journal->home_sectors;
	return 0;
}

/* Walks only immutable redo extents; uncovered home ranges never cause I/O. */
static int
journal_view_transfer(const struct ufs_journal_view *view, uint64_t first,
    uint32_t count, void *buffer, int copy)
{
	const uint8_t *entry;
	uint64_t current;
	uint64_t target;
	uint32_t done;
	uint32_t offset;
	uint32_t sectors;
	uint32_t run;
	unsigned index;

	/* Resolves every requested segment against the pinned generation. */
	done = 0;
	while (done < count) {
		current = first + done;
		offset = SECTOR_SIZE;
		run = 0;
		for (index = 0; index < get32(view->image + 16); index++) {
			entry = view->image + GROUP_HEADER + index * GROUP_ENTRY;
			target = get64(entry);
			sectors = get32(entry + 8);
			if (current >= target && current - target < sectors) {
				run = sectors - (uint32_t)(current - target);
				if (run > count - done)
					run = count - done;
				offset += (uint32_t)(current - target) * SECTOR_SIZE;
				break;
			}
			offset += sectors * SECTOR_SIZE;
		}
		if (run == 0)
			return ENOENT;
		if (copy) {
			memcpy((uint8_t *)buffer + (size_t)done * SECTOR_SIZE,
			    view->image + offset, (size_t)run * SECTOR_SIZE);
		}
		done += run;
	}
	return 0;
}

/* Copies a fully covered range, leaving the destination intact on refusal. */
int
ufs_journal_view_copy(const struct ufs_journal_view *view, uint64_t first,
    uint32_t count, void *buffer)
{
	int error;

	/* Bounds the entire request before resolving or copying any segment. */
	if (view == NULL || view->journal == NULL || buffer == NULL || count == 0 ||
	    count > UFS_JOURNAL_GROUP_SECTORS || first >= view->home_sectors ||
	    count > view->home_sectors - first)
		return EINVAL;
	error = journal_view_transfer(view, first, count, buffer, 0);
	if (error != 0)
		return error;
	error = journal_view_transfer(view, first, count, buffer, 1);
	return error;
}

/* Releases backing only after the caller's last immutable copy has completed. */
void
ufs_journal_view_release(struct ufs_journal_view *view)
{
	struct ufs_journal *journal;

	if (view == NULL || view->journal == NULL)
		return;
	journal = view->journal;
	memset(view, 0, sizeof(*view));
	(void)__atomic_fetch_sub(&journal->image_readers, 1U, __ATOMIC_RELEASE);
}
