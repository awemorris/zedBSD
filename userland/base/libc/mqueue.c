/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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
static void
cancel_point(void)
{
	if (__pthread_cancel_point != NULL)
		__pthread_cancel_point();
}
static uintptr_t
cancelable_flag(int cancellation_point)
{
	return cancellation_point && __pthread_cancel_enabled != NULL &&
		       __pthread_cancel_enabled()
		   ? ZEDBSD_USYNC_CANCELABLE
		   : 0;
}

static int
mq_storage_name(const char *name, char storage[PATH_MAX])
{
	size_t length;
	if (name == NULL || name[0] != '/' || name[1] == '\0' ||
	    strchr(name + 1, '/') != NULL) {
		errno = EINVAL;
		return -1;
	}
	length = strlen(name + 1);
	if (length > PATH_MAX - sizeof("/mq.")) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(storage, "/mq.", sizeof("/mq.") - 1U);
	memcpy(storage + sizeof("/mq.") - 1U, name + 1, length + 1U);
	return 0;
}

static intptr_t
mq_call(uint32_t number, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d,
	uintptr_t e, uintptr_t f)
{
	return syscall_result(__syscall6(number, a, b, c, d, e, f));
}

static int
mq_usync_wait(volatile uint32_t *word, uint32_t expected,
	      const struct timespec *relative, int cancellation_point)
{
	intptr_t result = mq_call(
	    ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAIT, expected,
	    (uintptr_t)relative, 0, cancelable_flag(cancellation_point));
	return result < 0 ? errno : 0;
}

static void
mq_usync_wake(volatile uint32_t *word, unsigned count)
{
	(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAKE, 0,
		      0, count, 0);
}

static void
mq_private_lock(volatile uint32_t *word)
{
	while (__atomic_exchange_n(word, 1, __ATOMIC_ACQUIRE) != 0) {
		(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word,
			      ZEDBSD_USYNC_WAIT, 1, 0, 0, ZEDBSD_USYNC_PRIVATE);
	}
}

static void
mq_private_unlock(volatile uint32_t *word)
{
	__atomic_store_n(word, 0, __ATOMIC_RELEASE);
	(void)mq_call(ZEDBSD_SYS_usync, (uintptr_t)word, ZEDBSD_USYNC_WAKE, 0,
		      0, 1, ZEDBSD_USYNC_PRIVATE);
}

static void
mq_store_lock(struct mq_store *store)
{
	while (__atomic_exchange_n(&store->guard, 1, __ATOMIC_ACQUIRE) != 0)
		(void)mq_usync_wait(&store->guard, 1, NULL, 0);
}

static void
mq_store_unlock(struct mq_store *store)
{
	__atomic_store_n(&store->guard, 0, __ATOMIC_RELEASE);
	mq_usync_wake(&store->guard, 1);
}

static int
mq_relative_deadline(const struct timespec *absolute, struct timespec *relative)
{
	struct timespec now;
	if (absolute == NULL)
		return 0;
	if (absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L) {
		errno = EINVAL;
		return -1;
	}
	if (clock_gettime(CLOCK_REALTIME, &now) != 0)
		return -1;
	if (absolute->tv_sec < now.tv_sec ||
	    (absolute->tv_sec == now.tv_sec &&
	     absolute->tv_nsec <= now.tv_nsec)) {
		errno = ETIMEDOUT;
		return -1;
	}
	relative->tv_sec = absolute->tv_sec - now.tv_sec;
	relative->tv_nsec = absolute->tv_nsec - now.tv_nsec;
	if (relative->tv_nsec < 0) {
		relative->tv_sec--;
		relative->tv_nsec += 1000000000L;
	}
	return 0;
}

static struct mq_descriptor *
mq_descriptor_get(mqd_t descriptor)
{
	unsigned i;
	struct mq_descriptor *found = NULL;
	mq_private_lock(&descriptor_guard);
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++)
		if (descriptors[i].used && descriptors[i].fd == descriptor) {
			found = &descriptors[i];
			break;
		}
	mq_private_unlock(&descriptor_guard);
	return found;
}

static int
mq_descriptor_install(int fd, int flags, struct mq_store *store)
{
	unsigned i;
	mq_private_lock(&descriptor_guard);
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++)
		if (!descriptors[i].used) {
			descriptors[i].used = 1;
			descriptors[i].fd = fd;
			descriptors[i].flags = flags & (O_ACCMODE | O_NONBLOCK);
			descriptors[i].store = store;
			mq_private_unlock(&descriptor_guard);
			return 0;
		}
	mq_private_unlock(&descriptor_guard);
	errno = EMFILE;
	return -1;
}

mqd_t
mq_open(const char *name, int flags, ...)
{
	struct mq_attr requested = {0, 10, MQ_MAX_MESSAGE_SIZE, 0};
	struct mq_store *store;
	char storage_name[PATH_MAX];
	mode_t mode = 0;
	int created = 0, fd;
	if ((flags & O_ACCMODE) > O_RDWR ||
	    (flags &
	     ~(O_ACCMODE | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC)) != 0) {
		errno = EINVAL;
		return (mqd_t)-1;
	}
	if (mq_storage_name(name, storage_name) != 0)
		return (mqd_t)-1;
	if ((flags & O_CREAT) != 0) {
		va_list ap;
		const struct mq_attr *attribute;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		attribute = va_arg(ap, const struct mq_attr *);
		if (attribute != NULL)
			requested = *attribute;
		va_end(ap);
		if (requested.mq_maxmsg <= 0 ||
		    requested.mq_maxmsg > (long)MQ_MAX_MESSAGES ||
		    requested.mq_msgsize <= 0 ||
		    requested.mq_msgsize > (long)MQ_MAX_MESSAGE_SIZE) {
			errno = EINVAL;
			return (mqd_t)-1;
		}
		fd = shm_open(storage_name,
			      (flags & ~(O_ACCMODE | O_TRUNC)) | O_RDWR |
				  O_CREAT | O_EXCL,
			      mode);
		if (fd >= 0)
			created = 1;
		else if (errno == EEXIST && (flags & O_EXCL) == 0)
			fd = shm_open(storage_name,
				      O_RDWR | (flags & O_CLOEXEC), mode);
	} else {
		fd = shm_open(storage_name, O_RDWR | (flags & O_CLOEXEC), 0);
	}
	if (fd < 0)
		return (mqd_t)-1;
	if (created && ftruncate(fd, (off_t)sizeof(*store)) != 0) {
		(void)close(fd);
		(void)shm_unlink(storage_name);
		return (mqd_t)-1;
	}
	store = mmap(NULL, sizeof(*store), PROT_READ | PROT_WRITE, MAP_SHARED,
		     fd, 0);
	if (store == MAP_FAILED) {
		(void)close(fd);
		if (created)
			(void)shm_unlink(storage_name);
		return (mqd_t)-1;
	}
	if (created) {
		memset(store, 0, sizeof(*store));
		store->maximum = (uint32_t)requested.mq_maxmsg;
		store->message_size = (uint32_t)requested.mq_msgsize;
		__atomic_store_n(&store->magic, MQ_MAGIC, __ATOMIC_RELEASE);
		mq_usync_wake(&store->magic, UINT32_MAX);
	} else {
		while (__atomic_load_n(&store->magic, __ATOMIC_ACQUIRE) !=
		       MQ_MAGIC) {
			int error = mq_usync_wait(&store->magic, 0, NULL, 0);
			if (error != 0 && error != EAGAIN) {
				(void)munmap(store, sizeof(*store));
				(void)close(fd);
				errno = error;
				return (mqd_t)-1;
			}
		}
	}
	if (mq_descriptor_install(fd, flags, store) != 0) {
		(void)munmap(store, sizeof(*store));
		(void)close(fd);
		return (mqd_t)-1;
	}
	return fd;
}

int
mq_close(mqd_t descriptor)
{
	unsigned i;
	struct mq_store *store = NULL;
	mq_private_lock(&descriptor_guard);
	for (i = 0; i < MQ_MAX_DESCRIPTORS; i++)
		if (descriptors[i].used && descriptors[i].fd == descriptor) {
			store = descriptors[i].store;
			memset(&descriptors[i], 0, sizeof(descriptors[i]));
			break;
		}
	mq_private_unlock(&descriptor_guard);
	if (store == NULL) {
		errno = EBADF;
		return -1;
	}
	mq_store_lock(store);
	if (store->notify_pid == getpid())
		store->notify_pid = 0;
	mq_store_unlock(store);
	/* Keep named queues durable across the last descriptor being closed. */
	(void)msync(store, sizeof(*store), MS_SYNC);
	(void)munmap(store, sizeof(*store));
	return close(descriptor);
}

int
mq_unlink(const char *name)
{
	char storage_name[PATH_MAX];
	return mq_storage_name(name, storage_name) == 0
		   ? shm_unlink(storage_name)
		   : -1;
}

int
mq_timedsend(mqd_t descriptor, const char *message, size_t length,
	     unsigned priority, const struct timespec *absolute)
{
	struct mq_descriptor *entry = mq_descriptor_get(descriptor);
	struct mq_store *store;
	cancel_point();
	if (entry == NULL || message == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;
		return -1;
	}
	if (priority >= MQ_PRIO_MAX) {
		errno = EINVAL;
		return -1;
	}
	if ((entry->flags & O_ACCMODE) == O_RDONLY) {
		errno = EBADF;
		return -1;
	}
	store = entry->store;
	if (length > store->message_size) {
		errno = EMSGSIZE;
		return -1;
	}
	for (;;) {
		uint32_t sequence;
		struct timespec relative, *timeout = NULL;
		unsigned position;
		mq_store_lock(store);
		if (store->count < store->maximum) {
			int notify_pid = 0, notify_kind = SIGEV_NONE;
			int notify_signo = 0;
			uint64_t notify_value = 0;
			int was_empty = store->count == 0;
			for (position = store->count;
			     position != 0 &&
			     store->slots[position - 1U].priority < priority;
			     position--)
				store->slots[position] =
				    store->slots[position - 1U];
			store->slots[position].length = (uint32_t)length;
			store->slots[position].priority = priority;
			memcpy(store->slots[position].data, message, length);
			store->count++;
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
			if (notify_pid != 0 && notify_kind == SIGEV_SIGNAL) {
				union sigval value;
				memcpy(&value, &notify_value,
				       sizeof(notify_value));
				(void)sigqueue((pid_t)notify_pid, notify_signo,
					       value);
			}
			cancel_point();
			return 0;
		}
		sequence = store->not_full;
		mq_store_unlock(store);
		if ((entry->flags & O_NONBLOCK) != 0) {
			errno = EAGAIN;
			return -1;
		}
		if (absolute != NULL) {
			if (mq_relative_deadline(absolute, &relative) != 0)
				return -1;
			timeout = &relative;
		}
		{
			int error = mq_usync_wait(&store->not_full, sequence,
						  timeout, 1);
			if (error != 0 && error != EAGAIN) {
				if (error == EINTR)
					cancel_point();
				errno = error;
				return -1;
			}
		}
		cancel_point();
	}
}

int
mq_send(mqd_t descriptor, const char *message, size_t length, unsigned priority)
{
	return mq_timedsend(descriptor, message, length, priority, NULL);
}

ssize_t
mq_timedreceive(mqd_t descriptor, char *message, size_t length,
		unsigned *priority, const struct timespec *absolute)
{
	struct mq_descriptor *entry = mq_descriptor_get(descriptor);
	struct mq_store *store;
	cancel_point();
	if (entry == NULL || message == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;
		return -1;
	}
	if ((entry->flags & O_ACCMODE) == O_WRONLY) {
		errno = EBADF;
		return -1;
	}
	store = entry->store;
	if (length < store->message_size) {
		errno = EMSGSIZE;
		return -1;
	}
	for (;;) {
		uint32_t sequence, copied;
		struct timespec relative, *timeout = NULL;
		unsigned i;
		mq_store_lock(store);
		if (store->count != 0) {
			copied = store->slots[0].length;
			memcpy(message, store->slots[0].data, copied);
			if (priority != NULL)
				*priority = store->slots[0].priority;
			for (i = 1; i < store->count; i++)
				store->slots[i - 1U] = store->slots[i];
			store->count--;
			(void)__atomic_add_fetch(&store->not_full, 1,
						 __ATOMIC_RELEASE);
			mq_store_unlock(store);
			mq_usync_wake(&store->not_full, 1);
			cancel_point();
			return (ssize_t)copied;
		}
		sequence = store->not_empty;
		mq_store_unlock(store);
		if ((entry->flags & O_NONBLOCK) != 0) {
			errno = EAGAIN;
			return -1;
		}
		if (absolute != NULL) {
			if (mq_relative_deadline(absolute, &relative) != 0)
				return -1;
			timeout = &relative;
		}
		{
			int error = mq_usync_wait(&store->not_empty, sequence,
						  timeout, 1);
			if (error != 0 && error != EAGAIN) {
				if (error == EINTR)
					cancel_point();
				errno = error;
				return -1;
			}
		}
		cancel_point();
	}
}

ssize_t
mq_receive(mqd_t descriptor, char *message, size_t length, unsigned *priority)
{
	return mq_timedreceive(descriptor, message, length, priority, NULL);
}

int
mq_getattr(mqd_t descriptor, struct mq_attr *attribute)
{
	struct mq_descriptor *entry = mq_descriptor_get(descriptor);
	if (entry == NULL || attribute == NULL) {
		errno = entry == NULL ? EBADF : EINVAL;
		return -1;
	}
	mq_store_lock(entry->store);
	attribute->mq_flags = entry->flags & O_NONBLOCK;
	attribute->mq_maxmsg = entry->store->maximum;
	attribute->mq_msgsize = entry->store->message_size;
	attribute->mq_curmsgs = entry->store->count;
	mq_store_unlock(entry->store);
	return 0;
}

int
mq_setattr(mqd_t descriptor, const struct mq_attr *attribute,
	   struct mq_attr *old_attribute)
{
	struct mq_descriptor *entry = mq_descriptor_get(descriptor);
	if (entry == NULL || attribute == NULL ||
	    (attribute->mq_flags & ~O_NONBLOCK) != 0) {
		errno = entry == NULL ? EBADF : EINVAL;
		return -1;
	}
	if (old_attribute != NULL && mq_getattr(descriptor, old_attribute) != 0)
		return -1;
	entry->flags = (entry->flags & O_ACCMODE) |
		       ((int)attribute->mq_flags & O_NONBLOCK);
	return 0;
}

int
mq_notify(mqd_t descriptor, const struct sigevent *notification)
{
	struct mq_descriptor *entry = mq_descriptor_get(descriptor);
	struct mq_store *store;
	pid_t self = getpid();
	int immediate = 0;
	int kind = SIGEV_NONE, signo = 0;
	uint64_t value = 0;
	if (entry == NULL) {
		errno = EBADF;
		return -1;
	}
	if (notification != NULL) {
		if (notification->sigev_notify != SIGEV_NONE &&
		    notification->sigev_notify != SIGEV_SIGNAL) {
			errno = ENOTSUP;
			return -1;
		}
		if (notification->sigev_notify == SIGEV_SIGNAL &&
		    (notification->sigev_signo <= 0 ||
		     notification->sigev_signo > SIGRTMAX)) {
			errno = EINVAL;
			return -1;
		}
		kind = notification->sigev_notify;
		signo = notification->sigev_signo;
		memcpy(&value, &notification->sigev_value, sizeof(value));
	}
	store = entry->store;
	mq_store_lock(store);
	if (notification == NULL) {
		if (store->notify_pid == self)
			store->notify_pid = 0;
		mq_store_unlock(store);
		return 0;
	}
	if (store->notify_pid != 0) {
		pid_t owner = store->notify_pid;
		mq_store_unlock(store);
		if (kill(owner, 0) == 0 || errno != ESRCH) {
			errno = EBUSY;
			return -1;
		}
		mq_store_lock(store);
		if (store->notify_pid != owner && store->notify_pid != 0) {
			mq_store_unlock(store);
			errno = EBUSY;
			return -1;
		}
	}
	store->notify_pid = self;
	store->notify_kind = kind;
	store->notify_signo = signo;
	store->notify_value = value;
	if (store->count != 0) {
		store->notify_pid = 0;
		immediate = 1;
	}
	mq_store_unlock(store);
	if (immediate && kind == SIGEV_SIGNAL) {
		union sigval signal_value;
		memcpy(&signal_value, &value, sizeof(value));
		return sigqueue(self, signo, signal_value);
	}
	return 0;
}
