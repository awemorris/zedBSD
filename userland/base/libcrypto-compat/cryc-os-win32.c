/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The operating-system services on Windows.
 */

#include "cryc-os.h"

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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Returns the current time as seconds since 1970-01-01T00:00:00Z
 * (time(NULL); the UCRT's time is 64-bit).
 */
int64_t
cryc_os_time(void)
{
	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Makes a mutex.
 */
int
cryc_os_mutex_init(
	struct cryc_os_mutex *mutex)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mutex);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Locks a mutex made by cryc_os_mutex_init (not recursive).
 */
void
cryc_os_mutex_lock(
	struct cryc_os_mutex *mutex)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mutex);
}

/*
 * Unlocks a mutex the calling thread holds.
 */
void
cryc_os_mutex_unlock(
	struct cryc_os_mutex *mutex)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mutex);
}

/*
 * Frees a mutex no thread holds; impl becomes NULL.
 */
void
cryc_os_mutex_destroy(
	struct cryc_os_mutex *mutex)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(mutex);
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(delta);

	/* Not implemented: the failure value. */
	return 0;
}

/*
 * Returns the calling thread's value (NULL until the thread sets one).
 */
void *
cryc_os_thread_value(void)
{
	/* Not implemented: the failure value. */
	return NULL;
}

/*
 * Sets the calling thread's value.
 */
int
cryc_os_thread_set_value(
	void *value,
	void (*destroy)(void *))
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(value);
	UNUSED_PARAMETER(destroy);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(path);
	UNUSED_PARAMETER(max);
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(size);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(descriptor);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(done);
	UNUSED_PARAMETER(os_error);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
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
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(descriptor);
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(size);
	UNUSED_PARAMETER(done);
	UNUSED_PARAMETER(os_error);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}

/*
 * Hands the system's trusted roots to the two callbacks, in this order:
 * POSIX:   add_file(context, CRYC_OS_DEFAULT_CA_FILE).
 */
int
cryc_os_system_roots(
	int (*add_der)(void *context, const uint8_t *der, size_t size),
	int (*add_file)(void *context, const char *path),
	void *context)
{
	/* The parameters are used once the function is written. */
	UNUSED_PARAMETER(add_der);
	UNUSED_PARAMETER(add_file);
	UNUSED_PARAMETER(context);

	/* Not implemented: ENOSYS. */
	return ENOSYS;
}
