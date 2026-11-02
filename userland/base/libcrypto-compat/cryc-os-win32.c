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

#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

/* The most bytes one BCryptGenRandom call is asked for (its length is a ULONG). */
#define OS_RANDOM_CHUNK	0x40000000U

/* The most bytes one recv or send call is given (their length is an int). */
#define OS_SOCKET_CHUNK	0x40000000U

/* The size of the first buffer cryc_os_read_file allocates; it doubles as the file turns out to be larger. */
#define OS_READ_FIRST	16384U

/* The largest path GetModuleFileNameW can return, in UTF-16 units (the system's own limit, including the zero). */
#define OS_PATH_MAX	32768U

/* The first size of the buffer the executable's path is read into, in UTF-16 units. */
#define OS_PATH_FIRST	512U

/*
 * Set to 1 once the fiber-local slot of the per-thread value exists.
 *
 * Zero means the slot has not been made, or that making it failed; the
 * value of the calling thread is then NULL.
 */
static LONG os_key_ready;

/*
 * The index of the slot of the per-thread value, made once by
 * os_key_make.
 *
 * It is never freed (no FlsFree), so a thread that exits later still finds
 * its destroy function mapped.
 */
static DWORD os_key_index;

/*
 * The function the slot calls with a thread's value when the thread exits.
 *
 * It is written once, inside os_key_make, before any thread can store a
 * value, and never changed.
 */
static void (*os_key_destroy)(void *);

/* The one-time guard of os_key_make. */
static INIT_ONCE os_key_once = INIT_ONCE_STATIC_INIT;

/*
 * What a caller of cryc_os_thread_set_value hands to os_key_make through
 * InitOnceExecuteOnce, which can pass a pointer to data but not to a
 * function.
 */
struct os_key_request {
	void (*destroy)(void *);
};

/* Makes the slot of the per-thread value; os_key_ready stays 0 when the system refuses. */
static BOOL CALLBACK os_key_make(PINIT_ONCE once, PVOID parameter, PVOID *context);

/* Calls the destroy function with a value a thread leaves behind. */
static VOID WINAPI os_key_callback(PVOID value);

/* Converts UTF-8 text to a new UTF-16 string; returns 0, EINVAL (not valid UTF-8) or ENOMEM. */
static int os_utf8_to_wide(const char *text, WCHAR **wide);

/* Maps a Winsock error code to the nearest errno value. */
static int os_errno_of_socket_error(int code);

/* Reads or writes at a socket: shared parts of the two socket functions' error reports. */
static int os_socket_failure(int code, int *os_error);

/* Reads the executable's own path into a new buffer; *wide is NULL (and 0 returned) when it is too long to use. */
static int os_executable_path(WCHAR **wide, size_t *length);

/* Hands the certificate file beside the executable to add_file. */
static int os_hand_beside_file(int (*add_file)(void *context, const char *path), void *context);

/* Hands every certificate of one logical system store to callback. */
static int os_hand_store(DWORD location, const WCHAR *name, int server_auth_only, int (*callback)(void *context, const uint8_t *der, size_t size), void *context);

/* Tells whether the enhanced key usage property of a certificate allows TLS server authentication. */
static int os_allows_server_auth(PCCERT_CONTEXT certificate, int *allowed);

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
	NTSTATUS status;

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

		/* Asks the system's preferred generator for the next chunk. */
		status = BCryptGenRandom(NULL, out + filled, (ULONG)want, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
		if (!BCRYPT_SUCCESS(status)) {
			/* The system refused: nothing partial may be used. */
			cryc_wipe(out, size);
			return EIO;
		}
		filled += want;
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
	SRWLOCK *native;

	/* Refuses a missing mutex. */
	if (mutex == NULL)
		return EINVAL;

	/* Allocates the system's lock object. */
	native = (SRWLOCK *)malloc(sizeof(*native));
	if (native == NULL) {
		mutex->impl = NULL;
		return ENOMEM;
	}

	/* Initializes it as unlocked. */
	InitializeSRWLock(native);

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

	/* Takes the lock exclusively, waiting for the holder. */
	AcquireSRWLockExclusive((SRWLOCK *)mutex->impl);
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

	/* Releases the lock. */
	ReleaseSRWLockExclusive((SRWLOCK *)mutex->impl);
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

	/* An SRWLOCK needs no teardown, only its memory. */
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
	long before;
	long updated;

	/* Adds in one indivisible, fully ordered step; the system returns the value before. */
	before = InterlockedExchangeAdd((volatile LONG *)count, (LONG)delta);

	/* The new value, computed unsigned so that a wrap is not signed overflow. */
	updated = (long)((unsigned long)before + (unsigned long)delta);

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
	LONG ready;

	/* A slot that was never made means no thread has set a value. */
	ready = InterlockedCompareExchange(&os_key_ready, 0, 0);
	if (ready == 0)
		return NULL;

	/* Reads this thread's value in the slot (NULL when unset). */
	value = FlsGetValue(os_key_index);

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
	struct os_key_request request;
	LONG ready;
	BOOL stored;
	BOOL once;

	/* Makes the slot once for the whole process, from this caller's destroy function. */
	request.destroy = destroy;
	once = InitOnceExecuteOnce(&os_key_once, os_key_make, &request, NULL);
	if (!once)
		return ENOMEM;

	/* A slot the system refused to make cannot hold the value. */
	ready = InterlockedCompareExchange(&os_key_ready, 0, 0);
	if (ready == 0)
		return ENOMEM;

	/* Stores the value for the calling thread. */
	stored = FlsSetValue(os_key_index, value);
	if (!stored)
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
	WCHAR *wide;
	uint8_t *buffer;
	uint8_t *grown;
	size_t capacity;
	size_t limit;
	size_t total;
	size_t got;
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

	/* Converts the UTF-8 path; text that is not UTF-8 names no file. */
	error = os_utf8_to_wide(path, &wide);
	if (error != 0)
		return error;

	/* Opens the file in binary mode (no CRLF change, no stop at 0x1A). */
	file = _wfopen(wide, L"rb");
	free(wide);
	if (file == NULL) {
		/* A missing file, or a missing directory on the way to it, is not there. */
		if (errno == ENOENT)
			return ENOENT;

		/* Anything else (permission, a directory, a name too long) is a read failure. */
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
			buffer = grown;
		}

		/* Reads into the free room. */
		got = fread(buffer + total, 1, capacity - 1U - total, file);
		if (got == 0) {
			/* A failed read leaves the file unusable. */
			if (ferror(file) != 0)
				error = EIO;

			/* The end of the file. */
			break;
		}
		total += got;
	}
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
	size_t want;
	int got;
	int code;
	int error;

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

	/* Limits one call to a length recv accepts. */
	want = size;
	if (want > OS_SOCKET_CHUNK)
		want = OS_SOCKET_CHUNK;

	/* Reads, again when the call was interrupted before any byte arrived. */
	for (;;) {
		got = recv((SOCKET)descriptor, (char *)buffer, (int)want, 0);
		if (got != SOCKET_ERROR)
			break;
		code = WSAGetLastError();
		if (code != WSAEINTR)
			break;
	}

	/* Delivers what was read; zero is the end of the stream. */
	if (got != SOCKET_ERROR) {
		*done = (size_t)got;
		return 0;
	}

	/* Reports the failure: EAGAIN for a socket with nothing now, EIO with the codes otherwise. */
	error = os_socket_failure(code, os_error);

	/* The failure's value. */
	return error;
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
	size_t want;
	int put;
	int code;
	int error;

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

	/* Limits one call to a length send accepts. */
	want = size;
	if (want > OS_SOCKET_CHUNK)
		want = OS_SOCKET_CHUNK;

	/* Writes, again when the call was interrupted before any byte left. */
	for (;;) {
		put = send((SOCKET)descriptor, (const char *)buffer, (int)want, 0);
		if (put != SOCKET_ERROR)
			break;
		code = WSAGetLastError();
		if (code != WSAEINTR)
			break;
	}

	/* Delivers how many bytes left; fewer than size is a partial write. */
	if (put != SOCKET_ERROR) {
		*done = (size_t)put;
		return 0;
	}

	/* Reports the failure: EAGAIN for a socket with no room now, EIO with the codes otherwise. */
	error = os_socket_failure(code, os_error);

	/* The failure's value. */
	return error;
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

	/* The distrusted certificates of the current user come first. */
	error = os_hand_store(CERT_SYSTEM_STORE_CURRENT_USER, L"Disallowed", 0, add_distrusted, context);
	if (error != 0)
		return error;

	/* Then those of the local machine. */
	error = os_hand_store(CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Disallowed", 0, add_distrusted, context);
	if (error != 0)
		return error;

	/* The trusted roots of the current user, those usable for server authentication only. */
	error = os_hand_store(CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT", 1, add_der, context);
	if (error != 0)
		return error;

	/* Then those of the local machine. */
	error = os_hand_store(CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT", 1, add_der, context);
	if (error != 0)
		return error;

	/* The bundle beside the executable comes last. */
	error = os_hand_beside_file(add_file, context);
	if (error != 0)
		return error;

	/* Succeeded: every source was handed over. */
	return 0;
}

/*
 * Makes the slot of the per-thread value from the destroy function the first
 * caller gave.
 */
static BOOL CALLBACK
os_key_make(
	PINIT_ONCE once,
	PVOID parameter,
	PVOID *context)
{
	const struct os_key_request *request;
	DWORD index;

	UNUSED_PARAMETER(once);
	UNUSED_PARAMETER(context);

	/* Keeps the destroy function the slot's callback will call. */
	request = (const struct os_key_request *)parameter;
	os_key_destroy = request->destroy;

	/* Asks the system for a slot. */
	index = FlsAlloc(os_key_callback);
	if (index == FLS_OUT_OF_INDEXES) {
		/* The once object is complete either way; os_key_ready says whether it worked. */
		return TRUE;
	}
	os_key_index = index;

	/* Publishes the slot to the readers that test os_key_ready first. */
	(void)InterlockedExchange(&os_key_ready, 1);

	/* Succeeded: the once routine is done. */
	return TRUE;
}

/*
 * Passes the value of an exiting thread to the destroy function.
 */
static VOID WINAPI
os_key_callback(
	PVOID value)
{
	/* A thread that never set a value has nothing to destroy. */
	if (value == NULL)
		return;

	/* A slot made without a function (destroy was NULL) leaves the value to its owner. */
	if (os_key_destroy == NULL)
		return;

	/* Lets the owner free the value. */
	os_key_destroy(value);
}

/*
 * Converts UTF-8 text to a new UTF-16 string.
 */
static int
os_utf8_to_wide(
	const char *text,
	WCHAR **wide)
{
	WCHAR *converted;
	int count;
	int written;

	/* An error leaves no string. */
	*wide = NULL;

	/* Counts the UTF-16 units, the terminating zero included; text that is not UTF-8 gives 0. */
	count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
	if (count <= 0)
		return EINVAL;

	/* Allocates the string. */
	converted = (WCHAR *)malloc(((size_t)count) * sizeof(WCHAR));
	if (converted == NULL)
		return ENOMEM;

	/* Converts for real. */
	written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, converted, count);
	if (written != count) {
		free(converted);
		return EINVAL;
	}

	/* Hands the string to the caller. */
	*wide = converted;

	/* Succeeded: *wide is a zero-terminated UTF-16 string. */
	return 0;
}

/*
 * Maps a Winsock error code to the nearest errno value.
 */
static int
os_errno_of_socket_error(
	int code)
{
	/* Picks the errno value that names the same condition. */
	switch (code) {
	case WSAEWOULDBLOCK:
		return EAGAIN;
	case WSAECONNRESET:
		return ECONNRESET;
	case WSAECONNABORTED:
		return ECONNABORTED;
	case WSAECONNREFUSED:
		return ECONNREFUSED;
	case WSAETIMEDOUT:
		return ETIMEDOUT;
	case WSAENOTCONN:
		return ENOTCONN;
	case WSAENOTSOCK:
		return ENOTSOCK;
	case WSAESHUTDOWN:
		return EPIPE;
	case WSAEMSGSIZE:
		return EMSGSIZE;
	case WSAENOBUFS:
		return ENOBUFS;
	case WSAENETDOWN:
		return ENETDOWN;
	case WSAENETUNREACH:
		return ENETUNREACH;
	case WSAENETRESET:
		return ENETRESET;
	case WSAEHOSTUNREACH:
		return EHOSTUNREACH;
	case WSAEINVAL:
		return EINVAL;
	case WSAEFAULT:
		return EFAULT;
	case WSAEACCES:
		return EACCES;
	case WSAEBADF:
		return EBADF;
	case WSAEINTR:
		return EINTR;
	case WSANOTINITIALISED:
		return ENETDOWN;
	default:
		return EIO;
	}
}

/*
 * Turns a failed recv or send into the function's result and its codes.
 */
static int
os_socket_failure(
	int code,
	int *os_error)
{
	/* Tells the caller the system's own code. */
	if (os_error != NULL)
		*os_error = code;

	/* A non-blocking socket with nothing to do now is not a failure. */
	if (code == WSAEWOULDBLOCK)
		return EAGAIN;

	/* A program written for OpenSSL reads errno after SSL_ERROR_SYSCALL, so it gets the nearest value. */
	errno = os_errno_of_socket_error(code);

	/* Every other failure is the transport's. */
	return EIO;
}

/*
 * Reads the executable's own path into a new buffer.
 */
static int
os_executable_path(
	WCHAR **wide,
	size_t *length)
{
	WCHAR *path;
	WCHAR *grown;
	size_t capacity;
	DWORD got;

	/* An unusable path leaves no buffer. */
	*wide = NULL;
	*length = 0;

	/* Starts small, since nearly every path fits. */
	capacity = OS_PATH_FIRST;
	path = (WCHAR *)malloc(capacity * sizeof(WCHAR));
	if (path == NULL)
		return ENOMEM;

	/* Grows the buffer until the whole path fits or the system's limit is reached. */
	for (;;) {
		got = GetModuleFileNameW(NULL, path, (DWORD)capacity);

		/* The call failed outright: no path to use. */
		if (got == 0) {
			free(path);
			return 0;
		}

		/* A result shorter than the buffer is the whole, zero-terminated path. */
		if ((size_t)got < capacity)
			break;

		/* A path that fills the largest buffer is longer than any the library uses. */
		if (capacity >= OS_PATH_MAX) {
			free(path);
			return 0;
		}

		/* Doubles the buffer for another try. */
		capacity = capacity * 2U;
		if (capacity > OS_PATH_MAX)
			capacity = OS_PATH_MAX;
		grown = (WCHAR *)realloc(path, capacity * sizeof(WCHAR));
		if (grown == NULL) {
			free(path);
			return ENOMEM;
		}
		path = grown;
	}

	/* Hands the path to the caller. */
	*wide = path;
	*length = (size_t)got;

	/* Succeeded: *wide holds the path of length units. */
	return 0;
}

/*
 * Hands the certificate file beside the executable to add_file.
 */
static int
os_hand_beside_file(
	int (*add_file)(void *context, const char *path),
	void *context)
{
	WCHAR *path;
	WCHAR *full;
	char *utf8;
	const char *name;
	size_t length;
	size_t directory;
	size_t name_length;
	size_t index;
	int count;
	int written;
	int error;

	/* Finds the executable. */
	error = os_executable_path(&path, &length);
	if (error != 0)
		return error;

	/* Without a usable path there is no file to name; that is as if the file were missing. */
	if (path == NULL)
		return 0;

	/* The directory is what precedes the last backslash. */
	directory = length;
	while (directory > 0U && path[directory - 1U] != L'\\')
		directory--;
	if (directory == 0U) {
		free(path);
		return 0;
	}
	directory--;

	/* Builds <directory>\cert.pem as UTF-16, the name widened one character at a time. */
	name = CRYC_OS_WIN32_CA_FILE;
	name_length = strlen(name);
	full = (WCHAR *)malloc((directory + 1U + name_length + 1U) * sizeof(WCHAR));
	if (full == NULL) {
		free(path);
		return ENOMEM;
	}
	memcpy(full, path, directory * sizeof(WCHAR));
	free(path);
	full[directory] = L'\\';
	for (index = 0; index < name_length; index++)
		full[directory + 1U + index] = (WCHAR)(unsigned char)name[index];
	full[directory + 1U + name_length] = 0;

	/* Counts the UTF-8 bytes; a path that does not convert is skipped. */
	count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, NULL, 0, NULL, NULL);
	if (count <= 0) {
		free(full);
		return 0;
	}
	utf8 = (char *)malloc((size_t)count);
	if (utf8 == NULL) {
		free(full);
		return ENOMEM;
	}

	/* Converts for real. */
	written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, full, -1, utf8, count, NULL, NULL);
	free(full);
	if (written != count) {
		free(utf8);
		return 0;
	}

	/* Hands the path to the callback. */
	error = add_file(context, utf8);
	free(utf8);

	/* A missing file is no failure. */
	if (error == ENOENT)
		return 0;

	/* Passes back the callback's own error. */
	if (error != 0)
		return error;

	/* Succeeded: the file was handed over or is not there. */
	return 0;
}

/*
 * Hands every certificate of one logical system store to callback.
 */
static int
os_hand_store(
	DWORD location,
	const WCHAR *name,
	int server_auth_only,
	int (*callback)(void *context, const uint8_t *der, size_t size),
	void *context)
{
	HCERTSTORE store;
	PCCERT_CONTEXT certificate;
	PCCERT_CONTEXT next;
	int allowed;
	int error;

	/* Opens the store read-only and only when it exists; a store that cannot be opened is empty. */
	store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, location | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, name);
	if (store == NULL)
		return 0;

	/* Visits every certificate in the store's order. */
	certificate = NULL;
	for (;;) {
		next = CertEnumCertificatesInStore(store, certificate);
		if (next == NULL)
			break;
		certificate = next;

		/* A certificate that is not an X.509 encoding cannot be parsed by the library. */
		if ((certificate->dwCertEncodingType & X509_ASN_ENCODING) == 0)
			continue;

		/* A trust anchor limited by its property to other uses is not trusted for servers. */
		if (server_auth_only != 0) {
			error = os_allows_server_auth(certificate, &allowed);
			if (error != 0) {
				(void)CertFreeCertificateContext(certificate);
				(void)CertCloseStore(store, 0);
				return error;
			}
			if (allowed == 0)
				continue;
		}

		/* Hands the certificate's DER to the callback. */
		error = callback(context, certificate->pbCertEncoded, (size_t)certificate->cbCertEncoded);
		if (error != 0) {
			(void)CertFreeCertificateContext(certificate);
			(void)CertCloseStore(store, 0);
			return error;
		}
	}

	/* Closes the store. */
	(void)CertCloseStore(store, 0);

	/* Succeeded: every certificate was handed over. */
	return 0;
}

/*
 * Tells whether the enhanced key usage property of a certificate allows TLS
 * server authentication.
 */
static int
os_allows_server_auth(
	PCCERT_CONTEXT certificate,
	int *allowed)
{
	CERT_ENHKEY_USAGE *usage;
	DWORD size;
	DWORD index;
	BOOL found;

	/* A certificate is refused until its property says otherwise. */
	*allowed = 0;

	/* Asks how large the property is; without the property the certificate has no limit. */
	size = 0;
	found = CertGetEnhancedKeyUsage(certificate, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, NULL, &size);
	if (!found) {
		/* No property: the certificate's own extension is the CA's business, not a local restriction. */
		if (GetLastError() == (DWORD)CRYPT_E_NOT_FOUND)
			*allowed = 1;

		/* Any other failure to read the property is taken as a restriction. */
		return 0;
	}

	/* Allocates the property's copy. */
	usage = (CERT_ENHKEY_USAGE *)malloc(size);
	if (usage == NULL)
		return ENOMEM;

	/* Reads the property. */
	SetLastError(0);
	found = CertGetEnhancedKeyUsage(certificate, CERT_FIND_PROP_ONLY_ENHKEY_USAGE_FLAG, usage, &size);
	if (!found) {
		free(usage);
		return 0;
	}

	/*
	 * An empty list is either no property at all (the call succeeds and
	 * the last error is CRYPT_E_NOT_FOUND: no local limit) or a property
	 * that disables every purpose (the last error is 0), as the function's
	 * documentation tells the two apart.
	 */
	if (usage->cUsageIdentifier == 0) {
		*allowed = GetLastError() == (DWORD)CRYPT_E_NOT_FOUND;
		free(usage);
		return 0;
	}

	/* Otherwise server authentication must be listed. */
	for (index = 0; index < usage->cUsageIdentifier; index++) {
		if (strcmp(usage->rgpszUsageIdentifier[index], szOID_PKIX_KP_SERVER_AUTH) == 0)
			*allowed = 1;
	}
	free(usage);

	/* Succeeded: *allowed says whether the property permits server authentication. */
	return 0;
}
