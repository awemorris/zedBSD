/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * POSIX record locks.
 *
 * Each regular file inode carries a sorted list of byte-range locks owned
 * either by a process (F_SETLK) or by an open file description
 * (F_OFD_SETLK).  A lock request replaces the owner's overlapping ranges,
 * splitting neighbours as needed, and a generation counter lets a waiter
 * that slept on a conflict retry against the current state.
 */

#include "kern/record-lock.h"

#include "kern/file.h"
#include "kern/inode.h"
#include "kern/kmem.h"
#include "kern/process.h"
#include "kern/waitq.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <zedbsd/fcntl.h>

#define RECORD_LOCK_INFINITY INT64_MAX

struct record_lock {
	void *owner;
	unsigned owner_is_file;
	int64_t start;
	int64_t end;
	short type;
	struct record_lock *next;
};

struct record_lock_state {
	struct spinlock lock;
	struct wait_queue waiters;
	uint64_t generation;
	struct record_lock *head;
};

static void lock_node_free(struct record_lock *node);
static void lock_list_free(struct record_lock *head);
static struct record_lock_state *record_lock_state_get(struct inode *inode, int create);
static int add_i64(int64_t first, int64_t second, int64_t *result);
static int normalize_range(struct file *file, const struct flock_record *request, int64_t *start, int64_t *end);
static int ranges_overlap(int64_t a, int64_t b, int64_t c, int64_t d);
static struct record_lock *find_conflict(struct record_lock_state *state, void *owner, unsigned owner_is_file, int64_t start, int64_t end, short type);
static void insert_sorted(struct record_lock_state *state, struct record_lock *node);
static struct record_lock *coalesce_locked(struct record_lock_state *state);
static struct record_lock *new_node(void *owner, unsigned owner_is_file, int64_t start, int64_t end, short type);
static int replace_owner_range(struct record_lock_state *state, void *owner, unsigned owner_is_file, int64_t start, int64_t end, short type, uint64_t expected, unsigned needed);

/*
 * Drops the record lock state of an inode that is going away.
 */
void
record_lock_inode_destroy(
	struct inode *inode)
{
	struct record_lock_state *state;
	struct record_lock *locks;
	unsigned long irq;

	/* Ignores a missing inode. */
	if (inode == NULL)
		return;

	/* Detaches the state from the inode. */
	mutex_lock(&inode->i_lock);
	state = inode->i_record_locks;
	inode->i_record_locks = NULL;
	mutex_unlock(&inode->i_lock);
	if (state == NULL)
		return;

	/* Takes the locks away from any waiters, then frees everything. */
	irq = spin_lock_irqsave(&state->lock);
	locks = state->head;
	state->head = NULL;
	waitq_wake_all(&state->waiters);
	spin_unlock_irqrestore(&state->lock, irq);
	lock_list_free(locks);
	kern_free(state);
}

/*
 * Handles the record lock fcntl commands.
 *
 * A get command reports the first conflicting lock.  A set command
 * replaces the owner's overlapping ranges, waiting for conflicts to clear
 * with the waiting variants; the replacement is retried when the state
 * changed while the lock was dropped for allocation.
 */
int
record_lock_fcntl(
	struct process *owner,
	struct file *file,
	int command,
	struct flock_record *request)
{
	struct record_lock_state *state;
	struct record_lock *conflict;
	struct record_lock *owned;
	void *lock_owner;
	unsigned owner_is_file;
	unsigned needed;
	int get_command;
	int wait_command;
	int64_t start;
	int64_t end;
	uint64_t generation;
	uint64_t sequence;
	unsigned long irq;
	int error;

	/* Rejects anything but a regular file with a well-formed request. */
	if (owner == NULL ||
	    file == NULL ||
	    file->f_inode == NULL ||
	    request == NULL ||
	    file->f_inode->i_type != INODE_REG)
		return EBADF;
	if (request->reserved0 != 0 ||
	    request->reserved1 != 0 ||
	    (request->type != F_RDLCK &&
	     request->type != F_WRLCK &&
	     request->type != F_UNLCK))
		return EINVAL;

	/* Classifies the command: open file description or process owner. */
	owner_is_file = 0;
	if (command == F_OFD_GETLK ||
	    command == F_OFD_SETLK ||
	    command == F_OFD_SETLKW)
		owner_is_file = 1;
	if (owner_is_file && request->pid != 0)
		return EINVAL;
	get_command = command == F_GETLK || command == F_OFD_GETLK;
	wait_command = command == F_SETLKW || command == F_OFD_SETLKW;
	if (owner_is_file)
		lock_owner = (void *)file;
	else
		lock_owner = (void *)owner;

	/* The lock type must agree with the open mode. */
	if (request->type == F_RDLCK &&
	    (file_status_flags_get(file) & O_ACCMODE) == O_WRONLY)
		return EBADF;
	if (request->type == F_WRLCK &&
	    (file_status_flags_get(file) & O_ACCMODE) == O_RDONLY)
		return EBADF;

	/* Resolves the range against the file offset or size. */
	error = normalize_range(file, request, &start, &end);
	if (error != 0)
		return error;

	/* A get command needs no state; a set command creates it. */
	state = record_lock_state_get(file->f_inode, !get_command);
	if (state == NULL) {
		if (get_command) {
			request->type = F_UNLCK;
			return 0;
		}
		return ENOMEM;
	}

	/* Retries until the replacement succeeds or a conflict blocks. */
	for (;;) {
		irq = spin_lock_irqsave(&state->lock);
		if (request->type == F_UNLCK)
			conflict = NULL;
		else
			conflict = find_conflict(state, lock_owner, owner_is_file,
			    start, end, request->type);

		/* A get command describes the conflict, or reports none. */
		if (get_command) {
			if (conflict == NULL) {
				request->type = F_UNLCK;
			} else {
				request->type = conflict->type;
				request->whence = SEEK_SET;
				request->start = conflict->start;
				if (conflict->end == RECORD_LOCK_INFINITY)
					request->length = 0;
				else
					request->length = conflict->end - conflict->start;
				if (conflict->owner_is_file)
					request->pid = -1;
				else
					request->pid = ((struct process *)conflict->owner)->pid;
			}
			spin_unlock_irqrestore(&state->lock, irq);
			return 0;
		}

		/* A conflict fails a non-waiting set, or sleeps until a change. */
		if (conflict != NULL) {
			if (!wait_command) {
				spin_unlock_irqrestore(&state->lock, irq);
				return EAGAIN;
			}
			sequence = waitq_sequence(&state->waiters);
			error = waitq_sleep(&state->waiters, &state->lock,
			    sequence, 0, WAITQ_INTERRUPTIBLE);
			spin_unlock_irqrestore(&state->lock, irq);
			if (error == EAGAIN)
				continue;
			if (error != 0)
				return error;
			continue;
		}

		/* Counts the nodes the replacement needs: one per split side. */
		generation = state->generation;
		if (request->type == F_UNLCK)
			needed = 0U;
		else
			needed = 1U;
		for (owned = state->head; owned != NULL;
		     owned = owned->next) {
			if (owned->owner == lock_owner &&
			    owned->owner_is_file == owner_is_file &&
			    ranges_overlap(start, end, owned->start,
			    owned->end)) {
				if (owned->start < start)
					needed++;
				if (owned->end > end)
					needed++;
			}
		}
		spin_unlock_irqrestore(&state->lock, irq);

		/* Replaces the range, retrying if the state moved meanwhile. */
		error = replace_owner_range(state, lock_owner, owner_is_file, start,
		    end, request->type, generation, needed);
		if (error != EAGAIN)
			return error;
	}
}

/*
 * Releases every process-owned lock a process holds on an inode.
 */
void
record_lock_release_process_inode(
	struct process *owner,
	struct inode *inode)
{
	struct record_lock_state *state;
	struct record_lock *garbage;
	struct record_lock **link;
	struct record_lock *lock;
	unsigned long irq;

	garbage = NULL;

	/* Ignores a missing owner or inode, or an inode without locks. */
	if (owner == NULL || inode == NULL)
		return;
	state = record_lock_state_get(inode, 0);
	if (state == NULL)
		return;

	/* Unlinks the process's locks onto a garbage list. */
	irq = spin_lock_irqsave(&state->lock);
	for (link = &state->head; *link != NULL;) {
		lock = *link;
		if (lock->owner_is_file || lock->owner != owner) {
			link = &lock->next;
			continue;
		}
		*link = lock->next;
		lock->next = garbage;
		garbage = lock;
	}

	/* Wakes the waiters when anything was released. */
	if (garbage != NULL) {
		state->generation++;
		waitq_wake_all(&state->waiters);
	}
	spin_unlock_irqrestore(&state->lock, irq);

	lock_list_free(garbage);
}

/*
 * Releases every lock an open file description holds when it closes.
 */
void
record_lock_release_file(
	struct file *file)
{
	struct record_lock_state *state;
	struct record_lock *garbage;
	struct record_lock **link;
	struct record_lock *lock;
	unsigned long irq;

	garbage = NULL;

	/* Ignores a file without an inode, or an inode without locks. */
	if (file == NULL || file->f_inode == NULL)
		return;
	state = record_lock_state_get(file->f_inode, 0);
	if (state == NULL)
		return;

	/* Unlinks the file's locks onto a garbage list. */
	irq = spin_lock_irqsave(&state->lock);
	for (link = &state->head; *link != NULL;) {
		lock = *link;
		if (!lock->owner_is_file || lock->owner != file) {
			link = &lock->next;
			continue;
		}
		*link = lock->next;
		lock->next = garbage;
		garbage = lock;
	}

	/* Wakes the waiters when anything was released. */
	if (garbage != NULL) {
		state->generation++;
		waitq_wake_all(&state->waiters);
	}
	spin_unlock_irqrestore(&state->lock, irq);

	lock_list_free(garbage);
}

/* Frees a lock node and the process reference it holds. */
static void
lock_node_free(
	struct record_lock *node)
{
	/* Ignores a missing node. */
	if (node == NULL)
		return;

	/* A process-owned node references its process. */
	if (!node->owner_is_file)
		process_release(node->owner);
	kern_free(node);
}

/* Frees a list of lock nodes. */
static void
lock_list_free(
	struct record_lock *head)
{
	struct record_lock *next;

	/* Frees the nodes in list order. */
	while (head != NULL) {
		next = head->next;
		lock_node_free(head);
		head = next;
	}
}

/* Finds the lock state of an inode, creating it on request. */
static struct record_lock_state *
record_lock_state_get(
	struct inode *inode,
	int create)
{
	struct record_lock_state *state;
	struct record_lock_state *candidate;

	candidate = NULL;

	/* There is no state without an inode. */
	if (inode == NULL)
		return NULL;

	/* Prepares a candidate outside the inode lock. */
	if (create) {
		candidate = kern_calloc(1, sizeof(*candidate));
		if (candidate != NULL) {
			spin_init(&candidate->lock, LOCK_RANK_RECORD_LOCK,
			    "record locks");
			waitq_init(&candidate->waiters, "record lock waiters");
			candidate->generation = 1;
		}
	}

	/* Installs the candidate unless the inode already has a state. */
	mutex_lock(&inode->i_lock);
	state = inode->i_record_locks;
	if (state == NULL && candidate != NULL) {
		inode->i_record_locks = candidate;
		state = candidate;
		candidate = NULL;
	}
	mutex_unlock(&inode->i_lock);
	if (candidate != NULL)
		kern_free(candidate);

	/* Reports the state, or none. */
	return state;
}

/* Adds two signed offsets, refusing overflow. */
static int
add_i64(
	int64_t first,
	int64_t second,
	int64_t *result)
{
	/* Rejects a sum that overflows in either direction. */
	if ((second > 0 && first > INT64_MAX - second) ||
	    (second < 0 && first < INT64_MIN - second))
		return EOVERFLOW;

	*result = first + second;

	/* Reports the sum. */
	return 0;
}

/* Converts a request into an absolute half-open byte range. */
static int
normalize_range(
	struct file *file,
	const struct flock_record *request,
	int64_t *start,
	int64_t *end)
{
	int64_t base;
	int64_t point;
	int error;

	/* Takes the base the whence names. */
	if (request->whence == SEEK_SET) {
		base = 0;
	} else if (request->whence == SEEK_CUR) {
		mutex_lock(&file->f_lock);
		base = file->f_offset;
		mutex_unlock(&file->f_lock);
	} else if (request->whence == SEEK_END) {
		mutex_lock(&file->f_inode->i_lock);
		base = file->f_inode->i_size;
		mutex_unlock(&file->f_inode->i_lock);
	} else {
		return EINVAL;
	}
	error = add_i64(base, request->start, &point);
	if (error != 0)
		return error;

	/* A positive length extends forward, zero to the end, negative back. */
	if (request->length > 0) {
		*start = point;
		error = add_i64(point, request->length, end);
		if (error != 0)
			return error;
	} else if (request->length == 0) {
		*start = point;
		*end = RECORD_LOCK_INFINITY;
	} else {
		*end = point;
		error = add_i64(point, request->length, start);
		if (error != 0)
			return error;
	}

	/* The range must start in the file and be non-empty. */
	if (*start < 0 || *end <= *start)
		return EINVAL;

	/* Reports the normalized range. */
	return 0;
}

/* Tests whether the half-open ranges [a, b) and [c, d) overlap. */
static int
ranges_overlap(
	int64_t a,
	int64_t b,
	int64_t c,
	int64_t d)
{
	/* Each range must start before the other ends. */
	if (a >= d)
		return 0;
	if (c >= b)
		return 0;

	/* Reports an overlap. */
	return 1;
}

/* Finds another owner's lock that conflicts with a request. */
static struct record_lock *
find_conflict(
	struct record_lock_state *state,
	void *owner,
	unsigned owner_is_file,
	int64_t start,
	int64_t end,
	short type)
{
	struct record_lock *lock;

	/* Two locks conflict when they overlap and either is a write lock. */
	for (lock = state->head; lock != NULL; lock = lock->next) {
		if ((lock->owner != owner ||
		    lock->owner_is_file != owner_is_file) &&
		    ranges_overlap(start, end,
		    lock->start, lock->end) &&
		    (type == F_WRLCK || lock->type == F_WRLCK))
			return lock;
	}

	/* Reports no conflict. */
	return NULL;
}

/* Inserts a node keeping the list ordered by start, then by owner. */
static void
insert_sorted(
	struct record_lock_state *state,
	struct record_lock *node)
{
	struct record_lock **link;

	/* Finds the first node that orders after the new one. */
	link = &state->head;
	while (*link != NULL && ((*link)->start < node->start ||
	    ((*link)->start == node->start &&
	    (uintptr_t)(*link)->owner <= (uintptr_t)node->owner)))
		link = &(*link)->next;

	/* Links the node in front of it. */
	node->next = *link;
	*link = node;
}

/* Merges adjacent same-owner same-type locks; returns the merged-away nodes. */
static struct record_lock *
coalesce_locked(
	struct record_lock_state *state)
{
	struct record_lock *lock;
	struct record_lock *garbage;
	struct record_lock *next;

	garbage = NULL;

	/* Absorbs each node that touches or overlaps its predecessor. */
	for (lock = state->head; lock != NULL && lock->next != NULL;) {
		next = lock->next;
		if (lock->owner == next->owner &&
		    lock->owner_is_file == next->owner_is_file &&
		    lock->type == next->type &&
		    next->start <= lock->end) {
			if (next->end > lock->end)
				lock->end = next->end;
			lock->next = next->next;
			next->next = garbage;
			garbage = next;
		} else {
			lock = next;
		}
	}

	/* Reports the absorbed nodes for freeing outside the lock. */
	return garbage;
}

/* Allocates a lock node, referencing a process owner. */
static struct record_lock *
new_node(
	void *owner,
	unsigned owner_is_file,
	int64_t start,
	int64_t end,
	short type)
{
	struct record_lock *node;

	/* Fills the node; a process owner is referenced by it. */
	node = kern_calloc(1, sizeof(*node));
	if (node != NULL) {
		if (!owner_is_file)
			process_ref(owner);
		node->owner = owner;
		node->owner_is_file = owner_is_file;
		node->start = start;
		node->end = end;
		node->type = type;
	}

	/* Reports the node, or none. */
	return node;
}

/* Replaces an owner's locks over a range, given the nodes it needs. */
static int
replace_owner_range(
	struct record_lock_state *state,
	void *owner,
	unsigned owner_is_file,
	int64_t start,
	int64_t end,
	short type,
	uint64_t expected,
	unsigned needed)
{
	struct record_lock *lock;
	struct record_lock *nodes;
	struct record_lock *garbage;
	struct record_lock *merged;
	struct record_lock *node;
	struct record_lock *before;
	struct record_lock *after;
	struct record_lock *inserted;
	struct record_lock **link;
	unsigned index;
	unsigned long irq;

	nodes = NULL;
	garbage = NULL;

	/* Allocates every node outside the lock. */
	for (index = 0; index < needed; index++) {
		node = new_node(owner, owner_is_file, 0, 0,
		    type);
		if (node == NULL) {
			lock_list_free(nodes);
			return ENOMEM;
		}
		node->next = nodes;
		nodes = node;
	}

	/* Retries from the caller if the state or a conflict changed meanwhile. */
	irq = spin_lock_irqsave(&state->lock);
	if (state->generation != expected ||
	    (type != F_UNLCK && find_conflict(state, owner, owner_is_file,
	    start, end, type))) {
		spin_unlock_irqrestore(&state->lock, irq);
		lock_list_free(nodes);
		return EAGAIN;
	}

	/* Removes the owner's overlapping locks, keeping their outer parts. */
	link = &state->head;
	while (*link != NULL) {
		lock = *link;
		if (lock->owner != owner ||
		    lock->owner_is_file != owner_is_file ||
		    !ranges_overlap(start, end,
		    lock->start, lock->end)) {
			link = &lock->next;
			continue;
		}
		*link = lock->next;
		if (lock->start < start) {
			before = nodes;
			nodes = nodes->next;
			before->start = lock->start;
			before->end = start;
			before->type = lock->type;
			insert_sorted(state, before);
		}
		if (lock->end > end) {
			after = nodes;
			nodes = nodes->next;
			after->start = end;
			after->end = lock->end;
			after->type = lock->type;
			insert_sorted(state, after);
		}
		lock->next = garbage;
		garbage = lock;
	}

	/* Inserts the new lock, unless the request was an unlock. */
	if (type != F_UNLCK) {
		inserted = nodes;
		nodes = nodes->next;
		inserted->start = start;
		inserted->end = end;
		inserted->type = type;
		insert_sorted(state, inserted);
	}

	/* Publishes the change and wakes the waiters. */
	merged = coalesce_locked(state);
	state->generation++;
	waitq_wake_all(&state->waiters);
	spin_unlock_irqrestore(&state->lock, irq);

	/* Frees the unused, removed, and merged nodes outside the lock. */
	lock_list_free(nodes);
	lock_list_free(garbage);
	lock_list_free(merged);

	/* Reports the replaced range. */
	return 0;
}
