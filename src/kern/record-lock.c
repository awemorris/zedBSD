/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
	struct process *owner;
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

static void
lock_node_free(struct record_lock *node)
{
	if (node == NULL)
		return;
	process_release(node->owner);
	kern_free(node);
}

static void
lock_list_free(struct record_lock *head)
{
	while (head != NULL) {
		struct record_lock *next = head->next;
		lock_node_free(head);
		head = next;
	}
}

static struct record_lock_state *
record_lock_state_get(struct inode *inode, int create)
{
	struct record_lock_state *state, *candidate = NULL;
	if (inode == NULL)
		return NULL;
	if (create) {
		candidate = kern_calloc(1, sizeof(*candidate));
		if (candidate != NULL) {
			spin_init(&candidate->lock, LOCK_RANK_RECORD_LOCK,
			    "record locks");
			waitq_init(&candidate->waiters, "record lock waiters");
			candidate->generation = 1;
		}
	}
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
	return state;
}

void
record_lock_inode_destroy(struct inode *inode)
{
	struct record_lock_state *state;
	struct record_lock *locks;
	unsigned long irq;
	if (inode == NULL)
		return;
	mutex_lock(&inode->i_lock);
	state = inode->i_record_locks;
	inode->i_record_locks = NULL;
	mutex_unlock(&inode->i_lock);
	if (state == NULL)
		return;
	irq = spin_lock_irqsave(&state->lock);
	locks = state->head;
	state->head = NULL;
	waitq_wake_all(&state->waiters);
	spin_unlock_irqrestore(&state->lock, irq);
	lock_list_free(locks);
	kern_free(state);
}

static int
add_i64(int64_t first, int64_t second, int64_t *result)
{
	if ((second > 0 && first > INT64_MAX - second) ||
	    (second < 0 && first < INT64_MIN - second))
		return EOVERFLOW;
	*result = first + second;
	return 0;
}

static int
normalize_range(struct file *file, const struct zedbsd_flock_request *request,
	int64_t *start, int64_t *end)
{
	int64_t base, point;
	int error;
	if (request->whence == SEEK_SET)
		base = 0;
	else if (request->whence == SEEK_CUR) {
		mutex_lock(&file->f_lock);
		base = file->f_offset;
		mutex_unlock(&file->f_lock);
	} else if (request->whence == SEEK_END) {
		mutex_lock(&file->f_inode->i_lock);
		base = file->f_inode->i_size;
		mutex_unlock(&file->f_inode->i_lock);
	} else
		return EINVAL;
	error = add_i64(base, request->start, &point);
	if (error != 0)
		return error;
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
	if (*start < 0 || *end <= *start)
		return EINVAL;
	return 0;
}

static int ranges_overlap(int64_t a, int64_t b, int64_t c, int64_t d)
{ return a < d && c < b; }

static struct record_lock *
find_conflict(struct record_lock_state *state, struct process *owner,
	int64_t start, int64_t end, short type)
{
	struct record_lock *lock;
	for (lock = state->head; lock != NULL; lock = lock->next)
		if (lock->owner != owner && ranges_overlap(start, end,
		    lock->start, lock->end) &&
		    (type == F_WRLCK || lock->type == F_WRLCK))
			return lock;
	return NULL;
}

static void
insert_sorted(struct record_lock_state *state, struct record_lock *node)
{
	struct record_lock **link = &state->head;
	while (*link != NULL && ((*link)->start < node->start ||
	    ((*link)->start == node->start && (*link)->owner->pid <=
	    node->owner->pid)))
		link = &(*link)->next;
	node->next = *link;
	*link = node;
}

static struct record_lock *
coalesce_locked(struct record_lock_state *state)
{
	struct record_lock *lock, *garbage = NULL;
	for (lock = state->head; lock != NULL && lock->next != NULL;) {
		struct record_lock *next = lock->next;
		if (lock->owner == next->owner && lock->type == next->type &&
		    next->start <= lock->end) {
			if (next->end > lock->end)
				lock->end = next->end;
			lock->next = next->next;
			next->next = garbage;
			garbage = next;
		} else
			lock = next;
	}
	return garbage;
}

static struct record_lock *
new_node(struct process *owner, int64_t start, int64_t end, short type)
{
	struct record_lock *node = kern_calloc(1, sizeof(*node));
	if (node != NULL) {
		process_ref(owner);
		node->owner = owner;
		node->start = start;
		node->end = end;
		node->type = type;
	}
	return node;
}

static int
replace_owner_range(struct record_lock_state *state, struct process *owner,
	int64_t start, int64_t end, short type, uint64_t expected, unsigned needed)
{
	struct record_lock *lock, *nodes = NULL, *garbage = NULL, *merged;
	unsigned index;
	unsigned long irq;
	for (index = 0; index < needed; index++) {
		struct record_lock *node = new_node(owner, 0, 0, type);
		if (node == NULL) {
			lock_list_free(nodes);
			return ENOMEM;
		}
		node->next = nodes;
		nodes = node;
	}
	irq = spin_lock_irqsave(&state->lock);
	if (state->generation != expected ||
	    (type != F_UNLCK && find_conflict(state, owner, start, end, type))) {
		spin_unlock_irqrestore(&state->lock, irq);
		lock_list_free(nodes);
		return EAGAIN;
	}
	{
		struct record_lock **link = &state->head;
		while (*link != NULL) {
			lock = *link;
			if (lock->owner != owner || !ranges_overlap(start, end,
			    lock->start, lock->end)) {
				link = &lock->next;
				continue;
			}
			*link = lock->next;
			if (lock->start < start) {
				struct record_lock *node = nodes;
				nodes = nodes->next;
				node->start = lock->start;
				node->end = start;
				node->type = lock->type;
				insert_sorted(state, node);
			}
			if (lock->end > end) {
				struct record_lock *node = nodes;
				nodes = nodes->next;
				node->start = end;
				node->end = lock->end;
				node->type = lock->type;
				insert_sorted(state, node);
			}
			lock->next = garbage;
			garbage = lock;
		}
	}
	if (type != F_UNLCK) {
		struct record_lock *node = nodes;
		nodes = nodes->next;
		node->start = start;
		node->end = end;
		node->type = type;
		insert_sorted(state, node);
	}
	merged = coalesce_locked(state);
	state->generation++;
	waitq_wake_all(&state->waiters);
	spin_unlock_irqrestore(&state->lock, irq);
	lock_list_free(nodes);
	lock_list_free(garbage);
	lock_list_free(merged);
	return 0;
}

int
record_lock_fcntl(struct process *owner, struct file *file, int command,
	struct zedbsd_flock_request *request)
{
	struct record_lock_state *state;
	int64_t start, end;
	int error;
	if (owner == NULL || file == NULL || file->f_inode == NULL ||
	    request == NULL || file->f_inode->i_type != INODE_REG)
		return EBADF;
	if (request->reserved0 != 0 || request->reserved1 != 0 ||
	    (request->type != F_RDLCK && request->type != F_WRLCK &&
	    request->type != F_UNLCK))
		return EINVAL;
	if (request->type == F_RDLCK &&
	    (file->f_flags & O_ACCMODE) == O_WRONLY)
		return EBADF;
	if (request->type == F_WRLCK &&
	    (file->f_flags & O_ACCMODE) == O_RDONLY)
		return EBADF;
	error = normalize_range(file, request, &start, &end);
	if (error != 0)
		return error;
	state = record_lock_state_get(file->f_inode, command != F_GETLK);
	if (state == NULL) {
		if (command == F_GETLK) {
			request->type = F_UNLCK;
			return 0;
		}
		return ENOMEM;
	}
	for (;;) {
		struct record_lock *conflict;
		uint64_t generation, sequence;
		unsigned needed;
		unsigned long irq = spin_lock_irqsave(&state->lock);
		conflict = request->type == F_UNLCK ? NULL :
		    find_conflict(state, owner, start, end, request->type);
		if (command == F_GETLK) {
			if (conflict == NULL)
				request->type = F_UNLCK;
			else {
				request->type = conflict->type;
				request->whence = SEEK_SET;
				request->start = conflict->start;
				request->length = conflict->end ==
				    RECORD_LOCK_INFINITY ? 0 :
				    conflict->end - conflict->start;
				request->pid = conflict->owner->pid;
			}
			spin_unlock_irqrestore(&state->lock, irq);
			return 0;
		}
		if (conflict != NULL) {
			if (command != F_SETLKW) {
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
		generation = state->generation;
		needed = request->type == F_UNLCK ? 0U : 1U;
		{
			struct record_lock *owned;
			for (owned = state->head; owned != NULL;
			    owned = owned->next)
				if (owned->owner == owner &&
				    ranges_overlap(start, end, owned->start,
				    owned->end)) {
					if (owned->start < start) needed++;
					if (owned->end > end) needed++;
				}
		}
		spin_unlock_irqrestore(&state->lock, irq);
		error = replace_owner_range(state, owner, start, end,
		    request->type, generation, needed);
		if (error != EAGAIN)
			return error;
	}
}

void
record_lock_release_process_inode(struct process *owner, struct inode *inode)
{
	struct record_lock_state *state;
	struct record_lock *garbage = NULL;
	struct record_lock **link;
	unsigned long irq;
	if (owner == NULL || inode == NULL)
		return;
	state = record_lock_state_get(inode, 0);
	if (state == NULL)
		return;
	irq = spin_lock_irqsave(&state->lock);
	for (link = &state->head; *link != NULL;) {
		struct record_lock *lock = *link;
		if (lock->owner != owner) {
			link = &lock->next;
			continue;
		}
		*link = lock->next;
		lock->next = garbage;
		garbage = lock;
	}
	if (garbage != NULL) {
		state->generation++;
		waitq_wake_all(&state->waiters);
	}
	spin_unlock_irqrestore(&state->lock, irq);
	lock_list_free(garbage);
}
