/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library mqueue support.
 */

#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <zedbsd/usync.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MQ_MAGIC 0x5a4d5131U
#define MQ_MAX_DESCRIPTORS 16U
#define MQ_MAX_MESSAGES 16U
#define MQ_MAX_MESSAGE_SIZE 256U

struct mq_slot {
	uint32_t length;
	uint32_t priority;
	uint8_t data[MQ_MAX_MESSAGE_SIZE];
};

struct mq_store {
	volatile uint32_t magic;
	volatile uint32_t guard;
	volatile uint32_t not_empty;
	volatile uint32_t not_full;
	uint32_t maximum;
	uint32_t message_size;
	uint32_t count;
	uint32_t reserved;
	int32_t notify_pid;
	int32_t notify_kind;
	int32_t notify_signo;
	uint32_t notify_reserved;
	uint64_t notify_value;
	struct mq_slot slots[MQ_MAX_MESSAGES];
};

struct mq_descriptor {
	int used;
	int fd;
	int flags;
	struct mq_store *store;
};

static struct mq_descriptor descriptors[MQ_MAX_DESCRIPTORS];
static volatile uint32_t descriptor_guard;
extern void __pthread_cancel_point(void) __attribute__((weak));
extern int __pthread_cancel_enabled(void) __attribute__((weak));

static int mq_storage_name(const char *name, char storage[PATH_MAX]);
static void mq_usync_wake(volatile uint32_t *word, unsigned count);
static intptr_t mq_call(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f);
static int mq_usync_wait(volatile uint32_t *word, uint32_t expected, const struct timespec *relative, int cancellation_point);
static uintptr_t cancelable_flag(int cancellation_point);
static int mq_descriptor_install(int fd, int flags, struct mq_store *store);
static void mq_private_lock(volatile uint32_t *word);
static void mq_private_unlock(volatile uint32_t *word);
static void mq_store_lock(struct mq_store *store);
static void mq_store_unlock(struct mq_store *store);
static struct mq_descriptor *mq_descriptor_get(mqd_t descriptor);
static void cancel_point(void);
static int mq_relative_deadline(const struct timespec *absolute, struct timespec *relative);

/*
 * Implements the mq open operation.
 */
mqd_t
mq_open(
	const char *name,
	int flags,
	...)
{
	int error;
	struct mq_attr requested = {0, 10, MQ_MAX_MESSAGE_SIZE, 0};
	struct mq_store *store;
	char storage_name[PATH_MAX];
	mode_t mode;
	int created, fd;

	mode = 0;
	created = 0;

	/* Checks the active flags. */
	if ((flags & O_ACCMODE) > O_RDWR ||
	    (flags &
	     ~(O_ACCMODE | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC)) != 0) {
		errno = EINVAL;

		/* Returns the computed result. */
		return (mqd_t)-1;
	}

	/* Handles a failed mq storage name operation. */
	if (mq_storage_name(name, storage_name) != 0)
		return (mqd_t)-1;

	/* Checks the active flags. */
	if ((flags & O_CREAT) != 0) {
		va_list ap;
		const struct mq_attr *attribute;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		attribute = va_arg(ap, const struct mq_attr *);

		/* Handles the attribute availability. */
		if (attribute != NULL)
			requested = *attribute;
		va_end(ap);

		/* Handles the requested condition. */
		if (requested.mq_maxmsg <= 0 ||
		    requested.mq_maxmsg > (long)MQ_MAX_MESSAGES ||
		    requested.mq_msgsize <= 0 ||
		    requested.mq_msgsize > (long)MQ_MAX_MESSAGE_SIZE) {
			errno = EINVAL;

			/* Returns the computed result. */
			return (mqd_t)-1;
		}
		fd = shm_open(storage_name,
			      (flags & ~(O_ACCMODE | O_TRUNC)) | O_RDWR |
				  O_CREAT | O_EXCL,
			      mode);

		/* Checks the file descriptor. */
		if (fd >= 0)
			created = 1;
		else if (errno == EEXIST && (flags & O_EXCL) == 0) {
			fd = shm_open(storage_name,
				      O_RDWR | (flags & O_CLOEXEC), mode);
		}
	} else {
		fd = shm_open(storage_name, O_RDWR | (flags & O_CLOEXEC), 0);
	}

	/* Checks the file descriptor. */
	if (fd < 0)
		return (mqd_t)-1;

	/* Handles a failed ftruncate operation. */
	if (created && ftruncate(fd, (off_t)sizeof(*store)) != 0) {
		(void)close(fd);
		(void)shm_unlink(storage_name);

		/* Returns the computed result. */
		return (mqd_t)-1;
	}
	store = mmap(NULL, sizeof(*store), PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);

	/* Handles an operation failure. */
	if (store == MAP_FAILED) {
		(void)close(fd);

		/* Handles the created condition. */
		if (created)
			(void)shm_unlink(storage_name);

		/* Returns the computed result. */
		return (mqd_t)-1;
	}

	/* Handles the created condition. */
	if (created) {
		memset(store, 0, sizeof(*store));
		store->maximum = (uint32_t)requested.mq_maxmsg;
		store->message_size = (uint32_t)requested.mq_msgsize;
		__atomic_store_n(&store->magic, MQ_MAGIC, __ATOMIC_RELEASE);
		mq_usync_wake(&store->magic, UINT32_MAX);
	} else {
		/* Continue while the operation condition remains true. */
		while (__atomic_load_n(&store->magic, __ATOMIC_ACQUIRE) !=
		       MQ_MAGIC) {
			error = mq_usync_wait(&store->magic, 0, NULL, 0);

			/* Handles an operation failure. */
			if (error != 0 && error != EAGAIN) {
				(void)munmap(store, sizeof(*store));
				(void)close(fd);
				errno = error;

				/* Returns the computed result. */
				return (mqd_t)-1;
			}
		}
	}

	/* Handles a failed mq descriptor install operation. */
	if (mq_descriptor_install(fd, flags, store) != 0) {
		(void)munmap(store, sizeof(*store));
		(void)close(fd);

		/* Returns the computed result. */
		return (mqd_t)-1;
	}

	/* Returns the computed result. */
	return fd;
}

/*
 * Implements the mq close operation.
 */
int
mq_close(
	mqd_t descriptor)
{
	int function_result;
	unsigned i;
	struct mq_store *store;

	store = NULL;
	mq_private_lock(&descriptor_guard);

	/* Process each element required by the operation. */
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++) {
		/* Handles the descriptors condition. */
		if (descriptors[i].used && descriptors[i].fd == descriptor) {
			store = descriptors[i].store;
			memset(&descriptors[i], 0, sizeof(descriptors[i]));
			break;
		}
	}
	mq_private_unlock(&descriptor_guard);

	/* Handles the store availability. */
	if (store == NULL) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}
	mq_store_lock(store);

	/* Handles a failed getpid operation. */
	if (store->notify_pid == getpid())
		store->notify_pid = 0;
	mq_store_unlock(store);

	/* Keep named queues durable across the last descriptor being closed. */
	(void)msync(store, sizeof(*store), MS_SYNC);
	(void)munmap(store, sizeof(*store));

	/* Obtains the close result. */
	function_result = close(descriptor);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mq unlink operation.
 */
int
mq_unlink(
	const char *name)
{
	int function_result;
	char storage_name[PATH_MAX];

	/* Computes the function result. */
	function_result = mq_storage_name(name, storage_name) == 0
		   ? shm_unlink(storage_name)
		   : -1;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mq timedsend operation.
 */
int
mq_timedsend(
	mqd_t descriptor,
	const char *message,
	size_t length,
	unsigned priority,
	const struct timespec *absolute)
{
	union sigval value;
	int notify_pid, notify_kind;
	int notify_signo;
	uint64_t notify_value;
	int error;
	uint32_t sequence;
	struct timespec relative, *timeout;
	unsigned position;
	int was_empty;
	struct mq_descriptor *entry;
	struct mq_store *store;

	entry = mq_descriptor_get(descriptor);
	cancel_point();

	/* Handles the entry availability. */
	if (entry == NULL || message == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the priority condition. */
	if (priority >= MQ_PRIO_MAX) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the entry condition. */
	if ((entry->flags & O_ACCMODE) == O_RDONLY) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}
	store = entry->store;

	/* Checks the current data length. */
	if (length > store->message_size) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		timeout = NULL;
		mq_store_lock(store);

		/* Handles the store condition. */
		if (store->count < store->maximum) {
			/* Process each remaining element. */
			notify_pid = 0;
			notify_kind = SIGEV_NONE;
			notify_signo = 0;
			notify_value = 0;
			was_empty = store->count == 0;
			for (position = store->count;
			     position != 0 &&
			     store->slots[position - 1U].priority < priority;
			     position--) {
				store->slots[position] =
				    store->slots[position - 1U];
			}
			store->slots[position].length = (uint32_t)length;
			store->slots[position].priority = priority;
			memcpy(store->slots[position].data, message, length);
			store->count++;

			/* Handles the was empty condition. */
			if (was_empty && store->notify_pid != 0) {
				notify_pid = store->notify_pid;
				notify_kind = store->notify_kind;
				notify_signo = store->notify_signo;
				notify_value = store->notify_value;
				store->notify_pid = 0;
			}
			(void)__atomic_add_fetch(&store->not_empty, 1,
						 __ATOMIC_RELEASE);
			mq_store_unlock(store);
			mq_usync_wake(&store->not_empty, 1);

			/* Handles the notify pid condition. */
			if (notify_pid != 0 && notify_kind == SIGEV_SIGNAL) {
				memcpy(&value, &notify_value,
				       sizeof(notify_value));
				(void)sigqueue((pid_t)notify_pid, notify_signo,
					       value);
			}
			cancel_point();

			/* Reports successful completion. */
			return 0;
		}
		sequence = store->not_full;
		mq_store_unlock(store);

		/* Handles the entry condition. */
		if ((entry->flags & O_NONBLOCK) != 0) {
			errno = EAGAIN;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the absolute availability. */
		if (absolute != NULL) {
			/* Handles a failed mq relative deadline operation. */
			if (mq_relative_deadline(absolute, &relative) != 0)
				return -1;
			timeout = &relative;
		}

		error = mq_usync_wait(&store->not_full, sequence,
					  timeout, 1);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN) {
			/* Handles an operation failure. */
			if (error == EINTR)
				cancel_point();
			errno = error;

			/* Reports operation failure. */
			return -1;
		}
		cancel_point();
	}
}

/*
 * Implements the mq send operation.
 */
int
mq_send(
	mqd_t descriptor,
	const char *message,
	size_t length,
	unsigned priority)
{
	int function_result;

	/* Obtains the mq timedsend result. */
	function_result = mq_timedsend(descriptor, message, length, priority, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mq timedreceive operation.
 */
ssize_t
mq_timedreceive(
	mqd_t descriptor,
	char *message,
	size_t length,
	unsigned *priority,
	const struct timespec *absolute)
{
	int error;
	uint32_t sequence, copied;
	struct timespec relative, *timeout;
	unsigned i;
	struct mq_descriptor *entry;
	struct mq_store *store;

	entry = mq_descriptor_get(descriptor);
	cancel_point();

	/* Handles the entry availability. */
	if (entry == NULL || message == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the entry condition. */
	if ((entry->flags & O_ACCMODE) == O_WRONLY) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}
	store = entry->store;

	/* Checks the current data length. */
	if (length < store->message_size) {
		errno = EMSGSIZE;

		/* Reports operation failure. */
		return -1;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		timeout = NULL;
		mq_store_lock(store);

		/* Handles the store condition. */
		if (store->count != 0) {
			copied = store->slots[0].length;
			memcpy(message, store->slots[0].data, copied);

			/* Handles the priority availability. */
			if (priority != NULL)

			/* Process each remaining element. */
				*priority = store->slots[0].priority;
			for (i = 1; i < store->count; i++)
				store->slots[i - 1U] = store->slots[i];
			store->count--;
			(void)__atomic_add_fetch(&store->not_full, 1,
						 __ATOMIC_RELEASE);
			mq_store_unlock(store);
			mq_usync_wake(&store->not_full, 1);
			cancel_point();

			/* Returns the computed result. */
			return (ssize_t)copied;
		}
		sequence = store->not_empty;
		mq_store_unlock(store);

		/* Handles the entry condition. */
		if ((entry->flags & O_NONBLOCK) != 0) {
			errno = EAGAIN;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the absolute availability. */
		if (absolute != NULL) {
			/* Handles a failed mq relative deadline operation. */
			if (mq_relative_deadline(absolute, &relative) != 0)
				return -1;
			timeout = &relative;
		}

		error = mq_usync_wait(&store->not_empty, sequence,
					  timeout, 1);

		/* Handles an operation failure. */
		if (error != 0 && error != EAGAIN) {
			/* Handles an operation failure. */
			if (error == EINTR)
				cancel_point();
			errno = error;

			/* Reports operation failure. */
			return -1;
		}
		cancel_point();
	}
}

/*
 * Implements the mq receive operation.
 */
ssize_t
mq_receive(
	mqd_t descriptor,
	char *message,
	size_t length,
	unsigned *priority)
{
	ssize_t function_result;

	/* Obtains the mq timedreceive result. */
	function_result = mq_timedreceive(descriptor, message, length, priority, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the mq getattr operation.
 */
int
mq_getattr(
	mqd_t descriptor,
	struct mq_attr *attribute)
{
	struct mq_descriptor *entry;

	entry = mq_descriptor_get(descriptor);

	/* Handles the entry availability. */
	if (entry == NULL || attribute == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	mq_store_lock(entry->store);
	attribute->mq_flags = entry->flags & O_NONBLOCK;
	attribute->mq_maxmsg = entry->store->maximum;
	attribute->mq_msgsize = entry->store->message_size;
	attribute->mq_curmsgs = entry->store->count;
	mq_store_unlock(entry->store);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the mq setattr operation.
 */
int
mq_setattr(
	mqd_t descriptor,
	const struct mq_attr *attribute,
	struct mq_attr *old_attribute)
{
	struct mq_descriptor *entry;

	entry = mq_descriptor_get(descriptor);

	/* Handles the entry availability. */
	if (entry == NULL || attribute == NULL ||
	    (attribute->mq_flags & ~O_NONBLOCK) != 0) {
		errno = entry == NULL ? EBADF : EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed mq getattr operation. */
	if (old_attribute != NULL && mq_getattr(descriptor, old_attribute) != 0)
		return -1;
	entry->flags = (entry->flags & O_ACCMODE) |
		       ((int)attribute->mq_flags & O_NONBLOCK);

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the mq notify operation.
 */
int
mq_notify(
	mqd_t descriptor,
	const struct sigevent *notification)
{
	int function_result;
	pid_t owner;
	union sigval signal_value;
	struct mq_descriptor *entry;
	struct mq_store *store;
	pid_t self;
	int immediate;
	int kind, signo;
	uint64_t value;

	entry = mq_descriptor_get(descriptor);
	self = getpid();
	immediate = 0;
	kind = SIGEV_NONE;
	signo = 0;
	value = 0;

	/* Handles the entry availability. */
	if (entry == NULL) {
		errno = EBADF;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the notification availability. */
	if (notification != NULL) {
		/* Handles the notification condition. */
		if (notification->sigev_notify != SIGEV_NONE &&
		    notification->sigev_notify != SIGEV_SIGNAL) {
			errno = ENOTSUP;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles the notification condition. */
		if (notification->sigev_notify == SIGEV_SIGNAL &&
		    (notification->sigev_signo <= 0 ||
		     notification->sigev_signo > SIGRTMAX)) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		kind = notification->sigev_notify;
		signo = notification->sigev_signo;
		memcpy(&value, &notification->sigev_value, sizeof(value));
	}
	store = entry->store;
	mq_store_lock(store);

	/* Handles the notification availability. */
	if (notification == NULL) {
		/* Handles the store condition. */
		if (store->notify_pid == self)
			store->notify_pid = 0;
		mq_store_unlock(store);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the store condition. */
	if (store->notify_pid != 0) {
		owner = store->notify_pid;
		mq_store_unlock(store);

		/* Handles the reported system error. */
		if (kill(owner, 0) == 0 || errno != ESRCH) {
			errno = EBUSY;

			/* Reports operation failure. */
			return -1;
		}
		mq_store_lock(store);

		/* Handles the store condition. */
		if (store->notify_pid != owner && store->notify_pid != 0) {
			mq_store_unlock(store);
			errno = EBUSY;

			/* Reports operation failure. */
			return -1;
		}
	}
	store->notify_pid = self;
	store->notify_kind = kind;
	store->notify_signo = signo;
	store->notify_value = value;

	/* Handles the store condition. */
	if (store->count != 0) {
		store->notify_pid = 0;
		immediate = 1;
	}
	mq_store_unlock(store);

	/* Handles the immediate condition. */
	if (immediate && kind == SIGEV_SIGNAL) {
		memcpy(&signal_value, &value, sizeof(value));

		/* Obtains the sigqueue result. */
		function_result = sigqueue(self, signo, signal_value);

		/* Returns the computed result. */
		return function_result;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the mq storage name operation. */
static int
mq_storage_name(
	const char *name,
	char storage[PATH_MAX])
{
	size_t length;

	/* Handles a failed strchr operation. */
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	length = strlen(name + 1);

	/* Checks the current data length. */
	if (length > PATH_MAX - sizeof("/mq.")) {
		errno = ENAMETOOLONG;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(storage, "/mq.", sizeof("/mq.") - 1U);
	memcpy(storage + sizeof("/mq.") - 1U, name + 1, length + 1U);

	/* Reports successful completion. */
	return 0;
}

/* Supports the mq usync wake operation. */
static void
mq_usync_wake(
	volatile uint32_t *word,
	unsigned count)
{
	(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAKE, 0,
		      0, count, 0);
}

/* Supports the mq call operation. */
static intptr_t
mq_call(
	uint32_t number,
	uintptr_t a,
	uintptr_t b,
	uintptr_t c,
	uintptr_t d,
	uintptr_t e,
	uintptr_t f)
{
	intptr_t function_result;

	/* Obtains the syscall result result. */
	function_result = syscall_result(__syscall6(number, a, b, c, d, e, f));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the mq usync wait operation. */
static int
mq_usync_wait(
	volatile uint32_t *word,
	uint32_t expected,
	const struct timespec *relative,
	int cancellation_point)
{
	intptr_t result;

	result = mq_call(
	    ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAIT, expected,
	    (uintptr_t)relative, 0, cancelable_flag(cancellation_point));

	/* Returns the computed result. */
	return result < 0 ? errno : 0;
}

/* Supports the cancelable flag operation. */
static uintptr_t
cancelable_flag(
	int cancellation_point)
{
	uintptr_t function_result;

	/* Computes the function result. */
	function_result = cancellation_point && __pthread_cancel_enabled != NULL &&
		       __pthread_cancel_enabled()
		   ? ZEDBSD_USYNC_CANCELABLE
		   : 0;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the mq descriptor install operation. */
static int
mq_descriptor_install(
	int fd,
	int flags,
	struct mq_store *store)
{
	unsigned i;

	mq_private_lock(&descriptor_guard);

	/* Process each element required by the operation. */
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++) {
		/* Handles the descriptors condition. */
		if (!descriptors[i].used) {
			descriptors[i].used = 1;
			descriptors[i].fd = fd;
			descriptors[i].flags = flags & (O_ACCMODE | O_NONBLOCK);
			descriptors[i].store = store;
			mq_private_unlock(&descriptor_guard);

			/* Reports successful completion. */
			return 0;
		}
	}
	mq_private_unlock(&descriptor_guard);
	errno = EMFILE;

	/* Reports operation failure. */
	return -1;
}

/* Supports the mq private lock operation. */
static void
mq_private_lock(
	volatile uint32_t *word)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(word, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word,
			      ZEDBSD_USYNC_WAIT, 1, 0, 0, ZEDBSD_USYNC_PRIVATE);
	}
}

/* Supports the mq private unlock operation. */
static void
mq_private_unlock(
	volatile uint32_t *word)
{
	__atomic_store_n(word, 0, __ATOMIC_RELEASE);
	(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAKE, 0,
		      0, 1, ZEDBSD_USYNC_PRIVATE);
}

/* Supports the mq store lock operation. */
static void
mq_store_lock(
	struct mq_store *store)
{
	/* Continue while the operation condition remains true. */
	while (__atomic_exchange_n(&store->guard, 1, __ATOMIC_ACQUIRE) != 0)
		(void)mq_usync_wait(&store->guard, 1, NULL, 0);
}

/* Supports the mq store unlock operation. */
static void
mq_store_unlock(
	struct mq_store *store)
{
	__atomic_store_n(&store->guard, 0, __ATOMIC_RELEASE);
	mq_usync_wake(&store->guard, 1);
}

/* Supports the mq descriptor get operation. */
static struct mq_descriptor *
mq_descriptor_get(
	mqd_t descriptor)
{
	unsigned i;
	struct mq_descriptor *found;

	found = NULL;
	mq_private_lock(&descriptor_guard);

	/* Process each element required by the operation. */
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++) {
		/* Handles the descriptors condition. */
		if (descriptors[i].used && descriptors[i].fd == descriptor) {
			found = &descriptors[i];
			break;
		}
	}
	mq_private_unlock(&descriptor_guard);

	/* Returns the computed result. */
	return found;
}

/* Supports the cancel point operation. */
static void
cancel_point(
	void)
{
	/* Handles the pthread cancel point availability. */
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}

/* Supports the mq relative deadline operation. */
static int
mq_relative_deadline(
	const struct timespec *absolute,
	struct timespec *relative)
{
	struct timespec now;

	/* Handles the absolute availability. */
	if (absolute == NULL)
		return 0;

	/* Handles the absolute condition. */
	if (absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return -1;

	/* Handles the absolute condition. */
	if (absolute->tv_sec < now.tv_sec ||
	    (absolute->tv_sec == now.tv_sec &&
	     absolute->tv_nsec <= now.tv_nsec)) {
		errno = ETIMEDOUT;

		/* Reports operation failure. */
		return -1;
	}
	relative->tv_sec = absolute->tv_sec - now.tv_sec;
	relative->tv_nsec = absolute->tv_nsec - now.tv_nsec;

	/* Handles the relative condition. */
	if (relative->tv_nsec < 0) {
		relative->tv_sec--;
		relative->tv_nsec += 1000000000L;
	}

	/* Reports successful completion. */
	return 0;
}
