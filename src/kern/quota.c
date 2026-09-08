/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Filesystem disk quotas.
 *
 * A quota state keeps a fixed number of user and group records with
 * block and inode usage, soft and hard limits, and the deadline after
 * which an exceeded soft limit is enforced.  Charges are reserved before
 * an allocation and committed or rolled back afterwards.  The limits are
 * exported to and imported from a checksummed on-disk configuration.
 */

#include "kern/quota.h"

#include <errno.h>
#include <string.h>

#define QUOTA_DISK_VERSION 1U
#define QUOTA_DISK_HEADER_SIZE 32U
#define QUOTA_DISK_RECORD_SIZE 56U

static uint32_t quota_get32(const uint8_t *p);
static uint64_t quota_get64(const uint8_t *p);
static void quota_put32(uint8_t *p, uint32_t v);
static void quota_put64(uint8_t *p, uint64_t v);
static uint32_t quota_digest(const uint8_t *p, size_t length);
static struct quota_record *quota_find(struct quota_state *state, enum quota_type type, uint32_t id, int create);
static int quota_check(struct quota_state *state, struct quota_record *record, uint64_t blocks, uint64_t inodes, uint64_t now);
static void quota_add(struct quota_state *state, struct quota_record *record, uint64_t blocks, uint64_t inodes, uint64_t now);
static void quota_subtract(struct quota_record *record, uint64_t blocks, uint64_t inodes);

/*
 * Initializes an empty quota state with the default grace period.
 */
void
quota_state_init(
	struct quota_state *state)
{
	/* Ignores a missing state. */
	if (state == NULL)
		return;

	/* Starts with no records and quotas disabled. */
	memset(state, 0, sizeof(*state));
	(void)mutex_init(&state->lock, LOCK_RANK_DEVICE, "filesystem quota");
	state->grace_seconds = QUOTA_DEFAULT_GRACE_SECONDS;
}

/*
 * Enables or disables enforcement for a quota type.
 */
int
quota_enable(
	struct quota_state *state,
	enum quota_type type,
	int enabled)
{
	/* Rejects a missing state or an unknown type. */
	if (state == NULL || type < QUOTA_USER || type >= QUOTA_TYPES)
		return EINVAL;

	/* Records the setting. */
	mutex_lock(&state->lock);
	state->enabled[type] = enabled != 0;
	mutex_unlock(&state->lock);

	/* Reports the changed setting. */
	return 0;
}

/*
 * Reports whether enforcement is enabled for a quota type.
 */
int
quota_enabled(
	struct quota_state *state,
	enum quota_type type,
	int *enabled)
{
	/* Rejects a missing state or result, or an unknown type. */
	if (state == NULL ||
	    enabled == NULL ||
	    type < QUOTA_USER ||
	    type >= QUOTA_TYPES)
		return EINVAL;

	/* Reads the setting. */
	mutex_lock(&state->lock);
	*enabled = state->enabled[type] != 0;
	mutex_unlock(&state->lock);

	/* Reports the read setting. */
	return 0;
}

/*
 * Reports the grace period in seconds.
 */
int
quota_get_grace(
	struct quota_state *state,
	uint64_t *seconds)
{
	/* Rejects a missing state or result. */
	if (state == NULL || seconds == NULL)
		return EINVAL;

	/* Reads the period. */
	mutex_lock(&state->lock);
	*seconds = state->grace_seconds;
	mutex_unlock(&state->lock);

	/* Reports the read period. */
	return 0;
}

/*
 * Sets the grace period in seconds.
 */
int
quota_set_grace(
	struct quota_state *state,
	uint64_t seconds)
{
	/* Rejects a missing state or an empty period. */
	if (state == NULL || seconds == 0)
		return EINVAL;

	/* Records the period. */
	mutex_lock(&state->lock);
	state->grace_seconds = seconds;
	mutex_unlock(&state->lock);

	/* Reports the changed period. */
	return 0;
}

/*
 * Copies the record of an identifier, or an empty record when none exists.
 */
int
quota_get(
	struct quota_state *state,
	enum quota_type type,
	uint32_t id,
	struct quota_record *result)
{
	struct quota_record *record;

	/* Rejects a missing state or result, or an unknown type. */
	if (state == NULL ||
	    result == NULL ||
	    type < QUOTA_USER ||
	    type >= QUOTA_TYPES)
		return EINVAL;

	/* Copies the record, or reports an empty one under the identifier. */
	mutex_lock(&state->lock);
	record = quota_find(state, type, id, 0);
	if (record != NULL)
		*result = *record;
	else
		memset(result, 0, sizeof(*result));
	result->id = id;
	mutex_unlock(&state->lock);

	/* Reports the copied record. */
	return 0;
}

/*
 * Sets the limits of an identifier, creating its record.
 *
 * A soft limit above a non-zero hard limit is rejected.  A deadline is
 * cleared when the usage no longer exceeds the new soft limit.
 */
int
quota_set(
	struct quota_state *state,
	enum quota_type type,
	const struct quota_record *source)
{
	struct quota_record *record;

	/* Rejects a missing operand, an unknown type, or inverted limits. */
	if (state == NULL ||
	    source == NULL ||
	    type < QUOTA_USER ||
	    type >= QUOTA_TYPES ||
	    (source->block_hard != 0 && source->block_soft > source->block_hard) ||
	    (source->inode_hard != 0 && source->inode_soft > source->inode_hard))
		return EINVAL;

	/* Finds or creates the record. */
	mutex_lock(&state->lock);
	record = quota_find(state, type, source->id, 1);
	if (record == NULL) {
		mutex_unlock(&state->lock);
		return ENOSPC;
	}

	/* Copies the limits and drops deadlines that no longer apply. */
	record->block_soft = source->block_soft;
	record->block_hard = source->block_hard;
	record->inode_soft = source->inode_soft;
	record->inode_hard = source->inode_hard;
	if (record->blocks <= record->block_soft || record->block_soft == 0)
		record->block_deadline = 0;
	if (record->inodes <= record->inode_soft || record->inode_soft == 0)
		record->inode_deadline = 0;
	mutex_unlock(&state->lock);

	/* Reports the changed limits. */
	return 0;
}

/*
 * Reserves blocks and inodes for a user and group.
 *
 * Both records are checked against their limits when their type is
 * enforced, then charged; the charge stays active until committed or
 * rolled back.
 */
int
quota_reserve(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now,
	struct quota_charge *charge)
{
	struct quota_record *user;
	struct quota_record *group;
	int error;

	user = NULL;
	group = NULL;
	error = 0;

	/* Rejects a missing state or charge. */
	if (state == NULL || charge == NULL)
		return EINVAL;
	memset(charge, 0, sizeof(*charge));

	/* Finds or creates both records and checks the enforced limits. */
	mutex_lock(&state->lock);
	user = quota_find(state, QUOTA_USER, uid, 1);
	group = quota_find(state, QUOTA_GROUP, gid, 1);
	if (user == NULL || group == NULL)
		error = ENOSPC;
	if (error == 0 && state->enabled[QUOTA_USER])
		error = quota_check(state, user, blocks, inodes, now);
	if (error == 0 && state->enabled[QUOTA_GROUP])
		error = quota_check(state, group, blocks, inodes, now);

	/* Charges both records and describes the charge for a rollback. */
	if (error == 0) {
		quota_add(state, user, blocks, inodes, now);
		quota_add(state, group, blocks, inodes, now);
		charge->state = state;
		charge->uid = uid;
		charge->gid = gid;
		charge->blocks = blocks;
		charge->inodes = inodes;
		charge->active = 1;
	}
	mutex_unlock(&state->lock);

	/* Reports why the reservation failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Makes a charge permanent.
 */
void
quota_commit(
	struct quota_charge *charge)
{
	/* A committed charge can no longer be rolled back. */
	if (charge != NULL)
		charge->active = 0;
}

/*
 * Returns an active charge to its user and group records.
 */
void
quota_rollback(
	struct quota_charge *charge)
{
	struct quota_record *record;

	/* Ignores a missing, inactive, or stateless charge. */
	if (charge == NULL || !charge->active || charge->state == NULL)
		return;

	/* Subtracts the charge from whichever records still exist. */
	mutex_lock(&charge->state->lock);
	record = quota_find(charge->state, QUOTA_USER, charge->uid, 0);
	if (record != NULL)
		quota_subtract(record, charge->blocks, charge->inodes);
	record = quota_find(charge->state, QUOTA_GROUP, charge->gid, 0);
	if (record != NULL)
		quota_subtract(record, charge->blocks, charge->inodes);
	mutex_unlock(&charge->state->lock);
	charge->active = 0;
}

/*
 * Releases blocks and inodes from a user and group.
 *
 * Releasing more than an enforced record holds indicates corrupt
 * accounting and is reported as EIO.
 */
int
quota_release(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes)
{
	struct quota_record *user;
	struct quota_record *group;

	/* Rejects a missing state. */
	if (state == NULL)
		return EINVAL;

	/* An enforced record must exist and hold at least the released amount. */
	mutex_lock(&state->lock);
	user = quota_find(state, QUOTA_USER, uid, 0);
	group = quota_find(state, QUOTA_GROUP, gid, 0);
	if ((state->enabled[QUOTA_USER] &&
	     (user == NULL || user->blocks < blocks || user->inodes < inodes)) ||
	    (state->enabled[QUOTA_GROUP] &&
	     (group == NULL || group->blocks < blocks || group->inodes < inodes))) {
		mutex_unlock(&state->lock);
		return EIO;
	}

	/* Subtracts from whichever records exist. */
	if (user != NULL)
		quota_subtract(user, blocks, inodes);
	if (group != NULL)
		quota_subtract(group, blocks, inodes);
	mutex_unlock(&state->lock);

	/* Reports the release. */
	return 0;
}

/*
 * Moves usage from one owner to another, as a transfer to commit or roll back.
 *
 * The new owner's records are checked against their limits when their
 * type is enforced.  A transfer to the same owner does nothing.
 */
int
quota_transfer_begin(
	struct quota_state *state,
	uid_t old_uid,
	gid_t old_gid,
	uid_t new_uid,
	gid_t new_gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now,
	struct quota_transfer *transfer)
{
	struct quota_record *old_user;
	struct quota_record *old_group;
	struct quota_record *new_user;
	struct quota_record *new_group;
	int error;

	error = 0;

	/* Rejects a missing state or transfer. */
	if (state == NULL || transfer == NULL)
		return EINVAL;
	memset(transfer, 0, sizeof(*transfer));

	/* A transfer to the same owner changes nothing. */
	if (old_uid == new_uid && old_gid == new_gid)
		return 0;

	/* The old records must hold the amount; the new ones are created. */
	mutex_lock(&state->lock);
	old_user = quota_find(state, QUOTA_USER, old_uid, 0);
	old_group = quota_find(state, QUOTA_GROUP, old_gid, 0);
	new_user = quota_find(state, QUOTA_USER, new_uid, 1);
	new_group = quota_find(state, QUOTA_GROUP, new_gid, 1);
	if (old_user == NULL ||
	    old_group == NULL ||
	    new_user == NULL ||
	    new_group == NULL ||
	    old_user->blocks < blocks ||
	    old_user->inodes < inodes ||
	    old_group->blocks < blocks ||
	    old_group->inodes < inodes)
		error = EIO;

	/* Checks the new owner's enforced limits. */
	if (error == 0 && old_uid != new_uid && state->enabled[QUOTA_USER])
		error = quota_check(state, new_user, blocks, inodes, now);
	if (error == 0 && old_gid != new_gid && state->enabled[QUOTA_GROUP])
		error = quota_check(state, new_group, blocks, inodes, now);

	/* Moves the usage and describes the transfer for a rollback. */
	if (error == 0) {
		if (old_uid != new_uid) {
			quota_subtract(old_user, blocks, inodes);
			quota_add(state, new_user, blocks, inodes, now);
		}
		if (old_gid != new_gid) {
			quota_subtract(old_group, blocks, inodes);
			quota_add(state, new_group, blocks, inodes, now);
		}
		transfer->state = state;
		transfer->old_uid = old_uid;
		transfer->old_gid = old_gid;
		transfer->new_uid = new_uid;
		transfer->new_gid = new_gid;
		transfer->blocks = blocks;
		transfer->inodes = inodes;
		transfer->now = now;
		transfer->active = 1;
	}
	mutex_unlock(&state->lock);

	/* Reports why the transfer failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Makes a transfer permanent.
 */
void
quota_transfer_commit(
	struct quota_transfer *transfer)
{
	/* A committed transfer can no longer be rolled back. */
	if (transfer != NULL)
		transfer->active = 0;
}

/*
 * Moves the usage of an active transfer back to the old owner.
 */
void
quota_transfer_rollback(
	struct quota_transfer *transfer)
{
	struct quota_record *old_user;
	struct quota_record *old_group;
	struct quota_record *new_user;
	struct quota_record *new_group;

	/* Ignores a missing, inactive, or stateless transfer. */
	if (transfer == NULL || !transfer->active || transfer->state == NULL)
		return;

	/* Recreates the old records and finds the new ones. */
	mutex_lock(&transfer->state->lock);
	old_user = quota_find(transfer->state, QUOTA_USER, transfer->old_uid, 1);
	old_group = quota_find(transfer->state, QUOTA_GROUP, transfer->old_gid, 1);
	new_user = quota_find(transfer->state, QUOTA_USER, transfer->new_uid, 0);
	new_group = quota_find(transfer->state, QUOTA_GROUP, transfer->new_gid, 0);

	/* Moves back whatever the new owner still holds. */
	if (transfer->old_uid != transfer->new_uid &&
	    old_user != NULL &&
	    new_user != NULL &&
	    new_user->blocks >= transfer->blocks &&
	    new_user->inodes >= transfer->inodes) {
		quota_subtract(new_user, transfer->blocks, transfer->inodes);
		quota_add(transfer->state, old_user, transfer->blocks,
		    transfer->inodes, transfer->now);
	}
	if (transfer->old_gid != transfer->new_gid &&
	    old_group != NULL &&
	    new_group != NULL &&
	    new_group->blocks >= transfer->blocks &&
	    new_group->inodes >= transfer->inodes) {
		quota_subtract(new_group, transfer->blocks, transfer->inodes);
		quota_add(transfer->state, old_group, transfer->blocks,
		    transfer->inodes, transfer->now);
	}
	mutex_unlock(&transfer->state->lock);
	transfer->active = 0;
}

/*
 * Moves usage from one owner to another in one step.
 */
int
quota_transfer(
	struct quota_state *state,
	uid_t old_uid,
	gid_t old_gid,
	uid_t new_uid,
	gid_t new_gid,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now)
{
	struct quota_transfer transfer;
	int error;

	/* Begins the transfer and commits it at once. */
	error = quota_transfer_begin(state, old_uid, old_gid, new_uid, new_gid, blocks,
	    inodes, now, &transfer);
	if (error == 0)
		quota_transfer_commit(&transfer);

	/* Reports why the transfer failed. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Adds usage found while rebuilding the accounting from the filesystem.
 *
 * Limits are not checked; only counter overflow is refused.
 */
int
quota_rebuild_add(
	struct quota_state *state,
	uid_t uid,
	gid_t gid,
	uint64_t blocks,
	uint64_t inodes)
{
	struct quota_record *user;
	struct quota_record *group;

	/* Rejects a missing state. */
	if (state == NULL)
		return EINVAL;

	/* Creates both records and refuses a counter overflow. */
	mutex_lock(&state->lock);
	user = quota_find(state, QUOTA_USER, uid, 1);
	group = quota_find(state, QUOTA_GROUP, gid, 1);
	if (user == NULL ||
	    group == NULL ||
	    blocks > UINT64_MAX - user->blocks ||
	    blocks > UINT64_MAX - group->blocks ||
	    inodes > UINT64_MAX - user->inodes ||
	    inodes > UINT64_MAX - group->inodes) {
		mutex_unlock(&state->lock);
		return ENOSPC;
	}

	/* Adds the usage without touching the deadlines. */
	user->blocks += blocks;
	user->inodes += inodes;
	group->blocks += blocks;
	group->inodes += inodes;
	mutex_unlock(&state->lock);

	/* Reports the added usage. */
	return 0;
}

/*
 * Serializes the limits and deadlines into the on-disk configuration.
 *
 * Without a buffer only the required size is reported.  Records without
 * limits or deadlines are omitted.
 */
int
quota_export_config(
	struct quota_state *state,
	void *buffer,
	size_t capacity,
	size_t *length)
{
	struct quota_record *record;
	uint8_t *bytes;
	uint8_t *entry;
	size_t needed;
	size_t offset;
	unsigned type;
	unsigned index;
	unsigned count;
	uint32_t enabled;

	bytes = buffer;
	needed = QUOTA_DISK_HEADER_SIZE;
	count = 0;

	/* Rejects a missing state or length. */
	if (state == NULL || length == NULL)
		return EINVAL;

	/* Sizes the configuration by counting the records worth storing. */
	mutex_lock(&state->lock);
	for (type = 0; type < QUOTA_TYPES; type++) {
		for (index = 0; index < QUOTA_MAX_RECORDS; index++) {
			record = &state->records[type][index];
			if (record->present &&
			    (record->block_soft != 0 ||
			     record->block_hard != 0 ||
			     record->inode_soft != 0 ||
			     record->inode_hard != 0 ||
			     record->block_deadline != 0 ||
			     record->inode_deadline != 0)) {
				needed += QUOTA_DISK_RECORD_SIZE;
				count++;
			}
		}
	}
	*length = needed;

	/* Without a buffer only the size is reported; a small one fails. */
	if (buffer == NULL || capacity < needed) {
		mutex_unlock(&state->lock);
		if (buffer == NULL)
			return 0;
		return ENOSPC;
	}

	/* Writes the header with the enforcement flags and grace period. */
	memset(bytes, 0, needed);
	memcpy(bytes, "ZQ01", 4);
	quota_put32(bytes + 4, QUOTA_DISK_VERSION);
	quota_put32(bytes + 8, (uint32_t)needed);
	enabled = 0;
	if (state->enabled[QUOTA_USER])
		enabled |= 1U;
	if (state->enabled[QUOTA_GROUP])
		enabled |= 2U;
	quota_put32(bytes + 16, enabled);
	quota_put32(bytes + 20, count);
	quota_put64(bytes + 24, state->grace_seconds);

	/* Writes the records. */
	offset = QUOTA_DISK_HEADER_SIZE;
	for (type = 0; type < QUOTA_TYPES; type++) {
		for (index = 0; index < QUOTA_MAX_RECORDS; index++) {
			record = &state->records[type][index];
			if (!record->present ||
			    (record->block_soft == 0 &&
			     record->block_hard == 0 &&
			     record->inode_soft == 0 &&
			     record->inode_hard == 0 &&
			     record->block_deadline == 0 &&
			     record->inode_deadline == 0))
				continue;
			entry = bytes + offset;
			quota_put32(entry, type);
			quota_put32(entry + 4, record->id);
			quota_put64(entry + 8, record->block_soft);
			quota_put64(entry + 16, record->block_hard);
			quota_put64(entry + 24, record->inode_soft);
			quota_put64(entry + 32, record->inode_hard);
			quota_put64(entry + 40, record->block_deadline);
			quota_put64(entry + 48, record->inode_deadline);
			offset += QUOTA_DISK_RECORD_SIZE;
		}
	}

	/* Seals the configuration with its digest. */
	quota_put32(bytes + 12, quota_digest(bytes, needed));
	mutex_unlock(&state->lock);

	/* Reports the written configuration. */
	return 0;
}

/*
 * Replaces the limits and deadlines from an on-disk configuration.
 *
 * The whole configuration is validated, and the record capacity checked,
 * before any limit changes; usage counters are kept.
 */
int
quota_import_config(
	struct quota_state *state,
	const void *buffer,
	size_t length)
{
	struct quota_record *record;
	const uint8_t *bytes;
	uint64_t grace;
	uint64_t block_soft;
	uint64_t block_hard;
	uint64_t inode_soft;
	uint64_t inode_hard;
	uint32_t enabled;
	uint32_t count;
	uint32_t id;
	size_t offset;
	size_t before;
	unsigned index;
	unsigned type;
	unsigned prior;
	unsigned free_count;
	unsigned missing;
	unsigned n;

	bytes = buffer;

	/* Rejects anything but a complete, sealed configuration of this version. */
	if (state == NULL ||
	    buffer == NULL ||
	    length < QUOTA_DISK_HEADER_SIZE ||
	    memcmp(bytes, "ZQ01", 4) != 0 ||
	    quota_get32(bytes + 4) != QUOTA_DISK_VERSION ||
	    quota_get32(bytes + 8) != length ||
	    quota_get32(bytes + 12) != quota_digest(bytes, length))
		return EINVAL;

	/* Validates the header fields against the length. */
	enabled = quota_get32(bytes + 16);
	count = quota_get32(bytes + 20);
	grace = quota_get64(bytes + 24);
	if ((enabled & ~3U) != 0 ||
	    grace == 0 ||
	    count > (length - QUOTA_DISK_HEADER_SIZE) / QUOTA_DISK_RECORD_SIZE ||
	    length != QUOTA_DISK_HEADER_SIZE + (size_t)count * QUOTA_DISK_RECORD_SIZE)
		return EINVAL;

	/* Validates every record: known type, ordered limits, unique identity. */
	for (index = 0, offset = QUOTA_DISK_HEADER_SIZE; index < count;
	     index++, offset += QUOTA_DISK_RECORD_SIZE) {
		block_soft = quota_get64(bytes + offset + 8);
		block_hard = quota_get64(bytes + offset + 16);
		inode_soft = quota_get64(bytes + offset + 24);
		inode_hard = quota_get64(bytes + offset + 32);
		id = quota_get32(bytes + offset + 4);
		type = quota_get32(bytes + offset);
		if (type >= QUOTA_TYPES ||
		    (block_hard != 0 && block_soft > block_hard) ||
		    (inode_hard != 0 && inode_soft > inode_hard))
			return EINVAL;
		for (prior = 0; prior < index; prior++) {
			before = QUOTA_DISK_HEADER_SIZE +
			    (size_t)prior * QUOTA_DISK_RECORD_SIZE;
			if (quota_get32(bytes + before) == type &&
			    quota_get32(bytes + before + 4) == id)
				return EINVAL;
		}
	}

	mutex_lock(&state->lock);

	/* Preflights the record capacity without changing the active policy. */
	for (type = 0; type < QUOTA_TYPES; type++) {
		free_count = 0;
		missing = 0;
		for (n = 0; n < QUOTA_MAX_RECORDS; n++) {
			if (!state->records[type][n].present)
				free_count++;
		}
		for (index = 0, offset = QUOTA_DISK_HEADER_SIZE; index < count;
		     index++, offset += QUOTA_DISK_RECORD_SIZE) {
			if (quota_get32(bytes + offset) == type &&
			    quota_find(state, (enum quota_type)type,
				       quota_get32(bytes + offset + 4),
				       0) == NULL)
				missing++;
		}
		if (missing > free_count) {
			mutex_unlock(&state->lock);
			return ENOSPC;
		}
	}

	/* Clears the limits and deadlines of every existing record. */
	for (type = 0; type < QUOTA_TYPES; type++) {
		for (index = 0; index < QUOTA_MAX_RECORDS; index++) {
			record = &state->records[type][index];
			if (record->present) {
				record->block_soft = 0;
				record->block_hard = 0;
				record->inode_soft = 0;
				record->inode_hard = 0;
				record->block_deadline = 0;
				record->inode_deadline = 0;
			}
		}
	}

	/* Installs the stored limits and deadlines. */
	for (index = 0, offset = QUOTA_DISK_HEADER_SIZE; index < count;
	     index++, offset += QUOTA_DISK_RECORD_SIZE) {
		type = quota_get32(bytes + offset);
		record = quota_find(state, (enum quota_type)type,
		    quota_get32(bytes + offset + 4), 1);
		record->block_soft = quota_get64(bytes + offset + 8);
		record->block_hard = quota_get64(bytes + offset + 16);
		record->inode_soft = quota_get64(bytes + offset + 24);
		record->inode_hard = quota_get64(bytes + offset + 32);
		record->block_deadline = quota_get64(bytes + offset + 40);
		record->inode_deadline = quota_get64(bytes + offset + 48);
	}

	/* Installs the enforcement flags and the grace period. */
	state->enabled[QUOTA_USER] = (enabled & 1U) != 0;
	state->enabled[QUOTA_GROUP] = (enabled & 2U) != 0;
	state->grace_seconds = grace;
	mutex_unlock(&state->lock);

	/* Reports the imported configuration. */
	return 0;
}

/* Reads a little-endian 32-bit field. */
static uint32_t
quota_get32(
	const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

/* Reads a little-endian 64-bit field. */
static uint64_t
quota_get64(
	const uint8_t *p)
{
	return quota_get32(p) | (uint64_t)quota_get32(p + 4) << 32;
}

/* Writes a little-endian 32-bit field. */
static void
quota_put32(
	uint8_t *p,
	uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* Writes a little-endian 64-bit field. */
static void
quota_put64(
	uint8_t *p,
	uint64_t v)
{
	quota_put32(p, (uint32_t)v);
	quota_put32(p + 4, (uint32_t)(v >> 32));
}

/* Computes the FNV-1a digest of a configuration with its digest field zeroed. */
static uint32_t
quota_digest(
	const uint8_t *p,
	size_t length)
{
	uint32_t value;
	size_t n;
	uint8_t byte;

	/* Folds every byte, treating the digest field as zero. */
	value = 2166136261U;
	for (n = 0; n < length; n++) {
		if (n >= 12U && n < 16U)
			byte = 0;
		else
			byte = p[n];
		value ^= byte;
		value *= 16777619U;
	}

	/* Reports the digest. */
	return value;
}

/* Finds the record of an identifier, creating it in a free slot on request. */
static struct quota_record *
quota_find(
	struct quota_state *state,
	enum quota_type type,
	uint32_t id,
	int create)
{
	struct quota_record *free_record;
	struct quota_record *record;
	unsigned index;

	free_record = NULL;

	/* Searches the type's records, remembering the first free slot. */
	for (index = 0; index < QUOTA_MAX_RECORDS; index++) {
		record = &state->records[type][index];
		if (record->present && record->id == id)
			return record;
		if (!record->present && free_record == NULL)
			free_record = record;
	}

	/* Creates the record when asked and a slot is free. */
	if (!create || free_record == NULL)
		return NULL;
	memset(free_record, 0, sizeof(*free_record));
	free_record->id = id;
	free_record->present = 1;

	/* Reports the created record. */
	return free_record;
}

/* Tests whether a record may take on more usage under its limits. */
static int
quota_check(
	struct quota_state *state,
	struct quota_record *record,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now)
{
	uint64_t new_blocks;
	uint64_t new_inodes;

	(void)state;

	/* A counter overflow counts as exceeding the quota. */
	if (blocks > UINT64_MAX - record->blocks || inodes > UINT64_MAX - record->inodes)
		return EDQUOT;
	new_blocks = record->blocks + blocks;
	new_inodes = record->inodes + inodes;

	/* A hard limit is never exceeded. */
	if ((record->block_hard != 0 && new_blocks > record->block_hard) ||
	    (record->inode_hard != 0 && new_inodes > record->inode_hard))
		return EDQUOT;

	/* A soft limit is enforced once its grace deadline has passed. */
	if (record->block_soft != 0 &&
	    new_blocks > record->block_soft &&
	    record->block_deadline != 0 &&
	    now >= record->block_deadline)
		return EDQUOT;
	if (record->inode_soft != 0 &&
	    new_inodes > record->inode_soft &&
	    record->inode_deadline != 0 &&
	    now >= record->inode_deadline)
		return EDQUOT;

	/* Reports permitted usage. */
	return 0;
}

/* Adds usage to a record, starting the grace period on a crossed soft limit. */
static void
quota_add(
	struct quota_state *state,
	struct quota_record *record,
	uint64_t blocks,
	uint64_t inodes,
	uint64_t now)
{
	record->blocks += blocks;
	record->inodes += inodes;

	/* A soft limit crossed for the first time starts its deadline. */
	if (record->block_soft != 0 &&
	    record->blocks > record->block_soft &&
	    record->block_deadline == 0)
		record->block_deadline = now + state->grace_seconds;
	if (record->inode_soft != 0 &&
	    record->inodes > record->inode_soft &&
	    record->inode_deadline == 0)
		record->inode_deadline = now + state->grace_seconds;
}

/* Subtracts usage from a record, ending a grace period that no longer applies. */
static void
quota_subtract(
	struct quota_record *record,
	uint64_t blocks,
	uint64_t inodes)
{
	record->blocks -= blocks;
	record->inodes -= inodes;

	/* Usage back under the soft limit clears its deadline. */
	if (record->block_soft == 0 || record->blocks <= record->block_soft)
		record->block_deadline = 0;
	if (record->inode_soft == 0 || record->inodes <= record->inode_soft)
		record->inode_deadline = 0;
}
