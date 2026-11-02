/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The operating-system services on POSIX systems (zedBSD, and the hosts of
 * the tests).
 */

#include "cryc-os.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The most bytes one getrandom call is asked for, which every system that has the call fills completely. */
#define OS_RANDOM_CHUNK	256U

/* The most bytes one socket call is given, so the length fits every system's int and ssize_t. */
#define OS_SOCKET_CHUNK	0x40000000U

/* The size of the first buffer cryc_os_read_file allocates; it doubles as the file turns out to be larger. */
#define OS_READ_FIRST	16384U

/*
 * Set to 1 (with release order) once the key of the per-thread value exists.
 *
 * Zero means the key has not been made, or that making it failed; the
 * value of the calling thread is then NULL.
 */
static int os_key_ready;

/*
 * The key of the per-thread value, made once by os_key_make.
 *
 * It is never deleted, so a thread that exits later still finds its
 * destroy function mapped.
 */
static pthread_key_t os_key;

/*
 * The function the key calls with a thread's value when the thread exits.
 *
 * Every caller passes the same function, so the stores from several first
 * callers agree; it is read once, inside os_key_make.
 */
static void (*os_key_destroy)(void *);

/* The one-time guard of os_key_make. */
static pthread_once_t os_key_once = PTHREAD_ONCE_INIT;

/* Makes the key of the per-thread value; os_key_ready stays 0 when the system refuses. */
static void os_key_make(void);

/*
 * Fills size bytes at out with random bytes from the operating system:
 * getrandom(out, n, 0) in a loop (EINTR retried, at most 256 bytes per
 * call as getentropy allows on every system) on POSIX;
 * BCryptGenRandom(NULL, out, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) on
 * Windows.
 */
int
cryc_os_random(
	void *context,
	uint8_t *out,
	size_t size)
{
	size_t filled;
	size_t want;
	ssize_t got;

	UNUSED_PARAMETER(context);

	/* Nothing to fill needs no call. */
	if (size == 0)
		return 0;

	/* Refuses a missing buffer instead of writing through it. */
	if (out == NULL)
		return EINVAL;

	/* Fills the buffer one chunk at a time until every byte came from the system. */
	filled = 0;
	while (filled < size) {
		want = size - filled;
		if (want > OS_RANDOM_CHUNK)
			want = OS_RANDOM_CHUNK;

		/* Asks the system for the next chunk. */
		got = getrandom(out + filled, want, 0);
		if (got < 0) {
			/* A signal interrupted the call before any byte was drawn. */
			if (errno == EINTR)
				continue;

			/* The system refused: nothing partial may be used. */
			cryc_wipe(out, size);
			return EIO;
		}

		/* Refuses a call that drew nothing. */
		if (got == 0) {
			/* A call that draws nothing and reports no error would never end. */
			cryc_wipe(out, size);
			return EIO;
		}

		/* Counts the bytes drawn. */
		filled += (size_t)got;
	}

	/* Succeeded: every byte of out is random. */
	return 0;
}

/*
 * Returns the current time as seconds since 1970-01-01T00:00:00Z
 * (time(NULL); the UCRT's time is 64-bit).
 */
int64_t
cryc_os_time(void)
{
	time_t now;

	/* Reads the wall clock. */
	now = time(NULL);

	/* A clock that cannot be read reports 0 (the failure value). */
	if (now == (time_t)-1)
		return 0;

	/* Succeeded: the seconds since the epoch. */
	return (int64_t)now;
}

/*
 * Makes a mutex.
 */
int
cryc_os_mutex_init(
	struct cryc_os_mutex *mutex)
{
	pthread_mutex_t *native;
	int error;

	/* Refuses a missing mutex. */
	if (mutex == NULL)
		return EINVAL;

	/* Allocates the system's mutex object. */
	native = (pthread_mutex_t *)malloc(sizeof(*native));
	if (native == NULL) {
		mutex->impl = NULL;
		return ENOMEM;
	}

	/* Initializes it with the default attributes. */
	error = pthread_mutex_init(native, NULL);
	if (error != 0) {
		free(native);
		mutex->impl = NULL;
		return ENOMEM;
	}

	/* Hands the finished object to the caller's structure. */
	mutex->impl = native;

	/* Succeeded: the mutex is unlocked and usable. */
	return 0;
}

/*
 * Locks a mutex made by cryc_os_mutex_init (not recursive).
 */
void
cryc_os_mutex_lock(
	struct cryc_os_mutex *mutex)
{
	/* A structure without an object has nothing to lock. */
	if (mutex == NULL || mutex->impl == NULL)
		return;

	/* Takes the mutex, waiting for the holder. */
	(void)pthread_mutex_lock((pthread_mutex_t *)mutex->impl);
}

/*
 * Unlocks a mutex the calling thread holds.
 */
void
cryc_os_mutex_unlock(
	struct cryc_os_mutex *mutex)
{
	/* A structure without an object has nothing to unlock. */
	if (mutex == NULL || mutex->impl == NULL)
		return;

	/* Releases the mutex. */
	(void)pthread_mutex_unlock((pthread_mutex_t *)mutex->impl);
}

/*
 * Frees a mutex no thread holds; impl becomes NULL.
 */
void
cryc_os_mutex_destroy(
	struct cryc_os_mutex *mutex)
{
	/* A structure without an object has nothing to free. */
	if (mutex == NULL || mutex->impl == NULL)
		return;

	/* Releases the system's object and its memory. */
	(void)pthread_mutex_destroy((pthread_mutex_t *)mutex->impl);
	free(mutex->impl);

	/* The structure no longer names an object, so a second destroy does nothing. */
	mutex->impl = NULL;
}

/*
 * Atomically adds delta (+1 or -1) to *count and returns the new value
 * (reference counts of the public objects).
 */
long
cryc_os_atomic_add(
	volatile long *count,
	long delta)
{
	long updated;

	/* Adds and reads back in one indivisible step, ordered against every other memory access. */
	updated = __atomic_add_fetch(count, delta, __ATOMIC_SEQ_CST);

	/* Succeeded: the value after the addition. */
	return updated;
}

/*
 * Returns the calling thread's value (NULL until the thread sets one).
 */
void *
cryc_os_thread_value(void)
{
	void *value;
	int ready;

	/* A key that was never made means no thread has set a value. */
	ready = __atomic_load_n(&os_key_ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return NULL;

	/* Reads this thread's value under the key (NULL when unset). */
	value = pthread_getspecific(os_key);

	/* Succeeded: the calling thread's value. */
	return value;
}

/*
 * Sets the calling thread's value.
 */
int
cryc_os_thread_set_value(
	void *value,
	void (*destroy)(void *))
{
	int ready;
	int error;

	/* Publishes the destroy function before the first caller makes the key from it. */
	__atomic_store_n(&os_key_destroy, destroy, __ATOMIC_RELAXED);

	/* Makes the key once for the whole process. */
	error = pthread_once(&os_key_once, os_key_make);
	if (error != 0)
		return ENOMEM;

	/* A key the system refused to make cannot hold the value. */
	ready = __atomic_load_n(&os_key_ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return ENOMEM;

	/* Stores the value for the calling thread. */
	error = pthread_setspecific(os_key, value);
	if (error != 0)
		return ENOMEM;

	/* Succeeded: the thread's value is now value. */
	return 0;
}

/*
 * Reads a whole file into a new buffer.
 */
int
cryc_os_read_file(
	const char *path,
	size_t max,
	uint8_t **data,
	size_t *size)
{
	FILE *file;
	uint8_t *buffer;
	uint8_t *grown;
	size_t capacity;
	size_t limit;
	size_t total;
	size_t got;
	int failed;
	int error;

	/* Refuses missing outputs; with them there is nothing to clear. */
	if (data == NULL || size == NULL)
		return EINVAL;

	/* An error leaves no buffer and no size. */
	*data = NULL;
	*size = 0;

	/* Refuses a missing path and a limit above what the library reads at all. */
	if (path == NULL || max > CRYC_OS_FILE_MAX)
		return EINVAL;

	/* Opens the file in binary mode. */
	file = fopen(path, "rb");
	if (file == NULL) {
		/* A missing file, or a missing directory on the way to it, is not there. */
		if (errno == ENOENT || errno == ENOTDIR)
			return ENOENT;

		/* Anything else (permission, a name too long) is a read failure. */
		return EIO;
	}

	/*
	 * Reads at most max + 1 bytes: one byte more than allowed is how a file
	 * above the limit is told from one exactly at it.  The buffer always
	 * has one byte after the data for the terminating zero.
	 */
	limit = max + 1U;
	capacity = OS_READ_FIRST;
	if (capacity > limit + 1U)
		capacity = limit + 1U;
	buffer = (uint8_t *)malloc(capacity);
	if (buffer == NULL) {
		(void)fclose(file);
		return ENOMEM;
	}

	/* Starts with an empty file. */
	total = 0;
	error = 0;

	/* Reads until the end of the file, the limit or a failure. */
	for (;;) {
		/* Grows the buffer when the data has filled all but the byte kept for the zero. */
		if (total + 1U == capacity) {
			/* Past the limit the file is too large. */
			if (total >= limit) {
				error = EFBIG;
				break;
			}

			/* Doubles, but never beyond what the limit needs. */
			capacity = capacity * 2U;
			if (capacity > limit + 1U)
				capacity = limit + 1U;
			grown = (uint8_t *)realloc(buffer, capacity);
			if (grown == NULL) {
				error = ENOMEM;
				break;
			}

			/* Takes the grown buffer. */
			buffer = grown;
		}

		/* Reads into the free room. */
		got = fread(buffer + total, 1, capacity - 1U - total, file);
		if (got == 0) {
			/* A signal that interrupted the read is retried. */
			failed = ferror(file);
			if (failed != 0 && errno == EINTR) {
				clearerr(file);
				continue;
			}

			/* A failed read leaves the file unusable. */
			if (failed != 0)
				error = EIO;

			/* The end of the file. */
			break;
		}

		/* Counts the bytes read. */
		total += got;
	}

	/* Closes the file. */
	(void)fclose(file);

	/* A file that reached the limit's extra byte is larger than max. */
	if (error == 0 && total > max)
		error = EFBIG;

	/* A failure frees what was read. */
	if (error != 0) {
		free(buffer);
		return error;
	}

	/* The terminating zero, and a buffer no larger than the file needs (a failed shrink keeps the larger one). */
	buffer[total] = 0;
	grown = (uint8_t *)realloc(buffer, total + 1U);
	if (grown != NULL)
		buffer = grown;

	/* Hands the data to the caller. */
	*data = buffer;
	*size = total;

	/* Succeeded: *data holds the whole file. */
	return 0;
}

/*
 * Reads up to size bytes from a connected socket (or, on POSIX, any file
 * descriptor) without waiting longer than the socket's own mode makes it.
 */
int
cryc_os_socket_read(
	intptr_t descriptor,
	uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	ssize_t got;
	size_t want;
	int code;

	/* Refuses a missing count. */
	if (done == NULL)
		return EINVAL;
	*done = 0;
	if (os_error != NULL)
		*os_error = 0;

	/*
	 * A read of nothing would answer 0, which the caller takes for the end
	 * of the stream, so it is a caller's mistake rather than an answer.
	 */
	if (buffer == NULL || size == 0)
		return EINVAL;

	/* A value that is not a descriptor is a bad descriptor, not a crash. */
	if (descriptor < 0 || (uintmax_t)descriptor > (uintmax_t)0x7fffffffU) {
		if (os_error != NULL)
			*os_error = EBADF;
		return EIO;
	}

	/* Limits one call to a length every system accepts. */
	want = size;
	if (want > OS_SOCKET_CHUNK)
		want = OS_SOCKET_CHUNK;

	/* Reads, again when a signal interrupted the call before any byte arrived. */
	for (;;) {
		got = read((int)descriptor, buffer, want);
		if (got >= 0)
			break;
		if (errno != EINTR)
			break;
	}

	/* Delivers what was read; zero is the end of the stream. */
	if (got >= 0) {
		*done = (size_t)got;
		return 0;
	}

	/* Nothing to read now on a non-blocking descriptor. */
	code = errno;
	if (os_error != NULL)
		*os_error = code;
	if (code == EAGAIN || code == EWOULDBLOCK)
		return EAGAIN;

	/* Every other failure is reported with the system's own code. */
	return EIO;
}

/*
 * Writes up to size bytes (write() on POSIX, send() on Windows); a partial
 * write sets *done below size and returns 0.
 */
int
cryc_os_socket_write(
	intptr_t descriptor,
	const uint8_t *buffer,
	size_t size,
	size_t *done,
	int *os_error)
{
	ssize_t put;
	size_t want;
	int code;

	/* Refuses a missing count. */
	if (done == NULL)
		return EINVAL;
	*done = 0;
	if (os_error != NULL)
		*os_error = 0;

	/* Nothing to write is written at once. */
	if (size == 0)
		return 0;

	/* Refuses a missing buffer instead of reading through it. */
	if (buffer == NULL)
		return EINVAL;

	/* A value that is not a descriptor is a bad descriptor, not a crash. */
	if (descriptor < 0 || (uintmax_t)descriptor > (uintmax_t)0x7fffffffU) {
		if (os_error != NULL)
			*os_error = EBADF;
		return EIO;
	}

	/* Limits one call to a length every system accepts. */
	want = size;
	if (want > OS_SOCKET_CHUNK)
		want = OS_SOCKET_CHUNK;

	/* Writes, again when a signal interrupted the call before any byte left. */
	for (;;) {
#ifdef MSG_NOSIGNAL
		/* A socket is written with send so that a closed peer is an error, not SIGPIPE. */
		put = send((int)descriptor, buffer, want, MSG_NOSIGNAL);
		if (put < 0 && errno == ENOTSOCK) {
			/* A pipe or a file: write() is the only call that accepts it. */
			put = write((int)descriptor, buffer, want);
		}
#else
		/* This system has no way to suppress SIGPIPE per call: the program handles the signal. */
		put = write((int)descriptor, buffer, want);
#endif
		if (put >= 0)
			break;
		if (errno != EINTR)
			break;
	}

	/* Delivers how many bytes left; fewer than size is a partial write. */
	if (put >= 0) {
		*done = (size_t)put;
		return 0;
	}

	/* No room now on a non-blocking descriptor. */
	code = errno;
	if (os_error != NULL)
		*os_error = code;
	if (code == EAGAIN || code == EWOULDBLOCK)
		return EAGAIN;

	/* Every other failure (a closed peer included) is reported with the system's own code. */
	return EIO;
}

/*
 * Hands the system's trust decisions to the three callbacks, in this
 * order (POSIX: the default CA file; Windows: Disallowed, ROOT, then
 * cert.pem beside the executable).
 */
int
cryc_os_system_roots(
	int (*add_distrusted)(void *context, const uint8_t *der, size_t size),
	int (*add_der)(void *context, const uint8_t *der, size_t size),
	int (*add_file)(void *context, const char *path),
	void *context)
{
	int error;

	/* All three callbacks are required on every system, so that a caller behaves the same on each. */
	if (add_distrusted == NULL || add_der == NULL || add_file == NULL)
		return EINVAL;

	/* The bundle holds only trusted roots, so it is the one thing handed over. */
	error = add_file(context, CRYC_OS_DEFAULT_CA_FILE);

	/* A bundle that is not installed means no roots, not a failure. */
	if (error == ENOENT)
		return 0;

	/* Passes back the callback's own error. */
	if (error != 0)
		return error;

	/* Succeeded: the bundle was handed over. */
	return 0;
}

/*
 * Makes the key of the per-thread value from the destroy function the first
 * caller gave.
 */
static void
os_key_make(void)
{
	void (*destroy)(void *);
	int error;

	/* Reads the destroy function the first caller published. */
	destroy = __atomic_load_n(&os_key_destroy, __ATOMIC_RELAXED);

	/* Asks the system for a key. */
	error = pthread_key_create(&os_key, destroy);
	if (error != 0)
		return;

	/* Publishes the key to the readers that test os_key_ready first. */
	__atomic_store_n(&os_key_ready, 1, __ATOMIC_RELEASE);
}
