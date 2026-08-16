/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_KERN_LOCK_H
#define ZEDBSD_KERN_LOCK_H

#include <kern/atomic.h>
#include <kern/waitq.h>

enum lock_rank {
	LOCK_RANK_PROCESS_TREE = 10,
	LOCK_RANK_PROCESS = 20,
	LOCK_RANK_FILEDESC = 30,
	LOCK_RANK_FILE = 40,
	LOCK_RANK_NAMESPACE = 50,
	LOCK_RANK_INODE = 60,
	LOCK_RANK_NAMECACHE = 70,
	LOCK_RANK_VMSPACE = 80,
	LOCK_RANK_VM_OBJECT = 90,
	LOCK_RANK_SOCKET_REGISTRY = 100,
	LOCK_RANK_SOCKET = 110,
	LOCK_RANK_NETWORK = 120,
	/* Block I/O is an independent leaf domain; do not enter VFS/VM from it. */
	LOCK_RANK_DISK = 130,
	LOCK_RANK_SCHEDULER = 200
};

struct spinlock {
	atomic_uint_t held;
	enum lock_rank rank;
	const char *name;
	unsigned owner_cpu;
	unsigned owner_valid;
};

void spin_init(struct spinlock *, enum lock_rank, const char *);
void spin_lock(struct spinlock *);
int spin_trylock(struct spinlock *);
void spin_unlock(struct spinlock *);
unsigned long spin_lock_irqsave(struct spinlock *);
void spin_unlock_irqrestore(struct spinlock *, unsigned long);

struct thread;
struct mutex {
	struct spinlock guard;
	struct thread *owner;
	struct wait_queue waiters;
	unsigned locked;
};
int mutex_init(struct mutex *, enum lock_rank, const char *);
int mutex_lock_interruptible(struct mutex *);
void mutex_lock(struct mutex *);
void mutex_unlock(struct mutex *);

#endif
