/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of Windows: the Vulkan loader, reading media files, and
 * the system's own threads, slim reader-writer locks taken exclusively,
 * condition variables and one-time initializations (no POSIX thread layer).
 * Paths are converted from UTF-8 to UTF-16 and only the W functions are called.
 */

#include "media-os.h"

#include <windows.h>
#include <process.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The rooms media-os.h gives hold the system's objects (each is one pointer). */
_Static_assert(sizeof(SRWLOCK) <= sizeof(((struct media_os_mutex *)NULL)->room), "a lock fits its room");
_Static_assert(_Alignof(SRWLOCK) <= _Alignof(struct media_os_mutex), "a lock is aligned in its room");
_Static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(((struct media_os_cond *)NULL)->room), "a condition fits its room");
_Static_assert(_Alignof(CONDITION_VARIABLE) <= _Alignof(struct media_os_cond), "a condition is aligned in its room");
_Static_assert(sizeof(INIT_ONCE) <= sizeof(((struct media_os_once *)NULL)->room), "a once fits its room");
_Static_assert(_Alignof(INIT_ONCE) <= _Alignof(struct media_os_once), "a once is aligned in its room");

/* The most bytes one ReadFile asks for; a larger request is read in pieces of this size. */
#define MEDIA_OS_READ_PIECE ((size_t)1 << 30)

/* The most UTF-16 units a path may have (the limit of the extended-length path form). */
#define MEDIA_OS_PATH_UNITS 32767

/* The most UTF-8 bytes a path of that many units can take (three bytes for one unit). */
#define MEDIA_OS_PATH_BYTES (MEDIA_OS_PATH_UNITS * 3)

/* A media file open for reading: the handle CreateFileW returned, opened for synchronous I/O. */
struct media_os_file {
	HANDLE handle;
};

/*
 * The function a once runs, carried to the system's callback in a record
 * on the caller's stack (a function pointer does not travel in a data
 * pointer).
 */
struct media_os_once_call {
	void (*run)(void);
};

static int media_os_win32_widen(const char *path, wchar_t **wide);
static int media_os_win32_error(DWORD code);
static int media_os_win32_is_directory(const wchar_t *wide);
static int media_os_win32_measure(HANDLE handle, uint64_t *size);
static SRWLOCK *media_os_win32_lock(struct media_os_mutex *mutex);
static CONDITION_VARIABLE *media_os_win32_condition(struct media_os_cond *cond);
static BOOL CALLBACK media_os_win32_once_run(PINIT_ONCE once, PVOID parameter, PVOID *context);
static unsigned __stdcall media_os_win32_thread_main(void *argument);

/*
 * Loads the system's Vulkan loader from the system directory only.
 *
 * The module handle itself is the result, so no memory is held for it and
 * the loader stays loaded for the life of the process.
 */
int
media_os_vulkan_open(
	struct media_os_library **library)
{
	HMODULE module;
	DWORD code;

	/* A caller with no room for the result broke the contract. */
	if (library == NULL)
		return EINVAL;

	/* Reports no loader unless one loads below. */
	*library = NULL;

	/* Loads the loader the GPU driver installed, never a copy beside the program. */
	module = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (module == NULL) {
		code = GetLastError();

		/* Memory ran out while loading. */
		if (code == ERROR_NOT_ENOUGH_MEMORY)
			return ENOMEM;
		if (code == ERROR_OUTOFMEMORY)
			return ENOMEM;

		/* Any other failure means this system has no usable loader. */
		return ENODEV;
	}

	/* Hands the loader to the caller. */
	*library = (struct media_os_library *)module;

	/* Succeeded: the loader is loaded. */
	return 0;
}

/*
 * Finds a function in the loader.
 *
 * The address is moved into the function pointer byte by byte so that no
 * cast between unrelated function types is needed.
 */
media_os_function
media_os_library_symbol(
	struct media_os_library *library,
	const char *name)
{
	FARPROC address;
	media_os_function function;

	/* A missing loader or name has no function to find. */
	if (library == NULL)
		return NULL;
	if (name == NULL)
		return NULL;

	/* Looks the name up in the loader. */
	address = GetProcAddress((HMODULE)library, name);
	if (address == NULL)
		return NULL;

	/* Turns the address into the function pointer the caller expects. */
	memcpy(&function, &address, sizeof(function));

	/* Succeeded: the function the loader exports under the name. */
	return function;
}

/*
 * Opens a file for reading and measures it.
 *
 * The file may be open for writing elsewhere, and may be deleted or renamed
 * while it is read, as a file being downloaded can be.
 */
int
media_os_file_open(
	const char *path,
	struct media_os_file **file,
	uint64_t *size)
{
	struct media_os_file *opened;
	wchar_t *wide;
	HANDLE handle;
	uint64_t length;
	DWORD code;
	int directory;
	int error;

	/* The results need somewhere to go. */
	if (file == NULL)
		return EINVAL;

	/* Reports no file unless the open succeeds below, whatever else is wrong. */
	*file = NULL;
	if (size == NULL)
		return EINVAL;

	/* Reports no size unless the open succeeds below. */
	*size = 0;

	/* A missing or empty path names nothing. */
	if (path == NULL)
		return EINVAL;
	if (path[0] == '\0')
		return EINVAL;

	/* Converts the path to UTF-16 for the W functions. */
	error = media_os_win32_widen(path, &wide);
	if (error != 0)
		return error;

	/* Allocates the file object. */
	opened = malloc(sizeof(*opened));
	if (opened == NULL) {
		free(wide);
		return ENOMEM;
	}

	/* Opens the existing file for reading, shared with every other user and not inherited. */
	handle = CreateFileW(
		wide,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		code = GetLastError();
		error = media_os_win32_error(code);

		/* Opening a directory fails with access denied; asks what the path names. */
		if (code == ERROR_ACCESS_DENIED) {
			directory = media_os_win32_is_directory(wide);
			if (directory != 0)
				error = EISDIR;
		}

		/* Releases the converted path and the file's record, and reports the failure. */
		free(wide);
		free(opened);
		return error;
	}

	/* The path is no longer needed. */
	free(wide);

	/* Measures the file and refuses what is not a disk file. */
	error = media_os_win32_measure(handle, &length);
	if (error != 0) {
		CloseHandle(handle);
		free(opened);
		return error;
	}

	/* Hands the file and its size to the caller. */
	opened->handle = handle;
	*file = opened;
	*size = length;

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Reads exactly size bytes at offset without using the file position.
 *
 * Each ReadFile carries its own offset in an OVERLAPPED on the synchronous
 * handle, asks for at most 1 GiB, and a short read is continued.
 */
int
media_os_file_read_at(
	struct media_os_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	unsigned char *bytes;
	OVERLAPPED overlapped;
	uint64_t last;
	uint64_t position;
	BOOL succeeded;
	DWORD count;
	size_t done;
	size_t piece;

	/* A missing file cannot be read. */
	if (file == NULL)
		return EINVAL;

	/* Reading nothing succeeds wherever it is asked, even past the end. */
	if (size == 0)
		return 0;

	/* A read with no room for the bytes broke the contract. */
	if (data == NULL)
		return EINVAL;

	/* A range whose end does not fit in 64 bits broke the contract. */
	if ((uint64_t)size > UINT64_MAX - offset)
		return EINVAL;

	/* A file offset is a signed 64-bit number, so a range ending beyond it ends beyond any file. */
	last = offset + (uint64_t)size - 1;
	if (last > (uint64_t)INT64_MAX)
		return EIO;

	/* Reads the range piece by piece until every byte has arrived. */
	bytes = data;
	done = 0;
	while (done < size) {
		piece = size - done;
		if (piece > MEDIA_OS_READ_PIECE)
			piece = MEDIA_OS_READ_PIECE;

		/* Names where this piece starts in the file. */
		position = offset + (uint64_t)done;
		memset(&overlapped, 0, sizeof(overlapped));
		overlapped.Offset = (DWORD)(position & 0xffffffffU);
		overlapped.OffsetHigh = (DWORD)(position >> 32);

		/* Reads the next piece at the position after what was read. */
		count = 0;
		succeeded = ReadFile(file->handle, bytes + done, (DWORD)piece, &count, &overlapped);

		/* The read failed (reading at or past the end is one of the ways). */
		if (!succeeded)
			return EIO;

		/* The file ended before the range did: it shrank. */
		if (count == 0)
			return EIO;

		/* Moves past the bytes that were read. */
		done += (size_t)count;
	}

	/* Succeeded: every byte of the range is in the buffer. */
	return 0;
}

/* Closes a file and frees it. */
void
media_os_file_close(
	struct media_os_file *file)
{
	/* Closing nothing does nothing. */
	if (file == NULL)
		return;

	/* Gives the handle back to the system and frees the object. */
	CloseHandle(file->handle);
	free(file);
}

/* Does nothing: Windows never makes a GPU picture that carries a descriptor. */
void
media_os_close_descriptor(
	int descriptor)
{
	/* There is nothing to close. */
	(void)descriptor;
}

/* Makes a mutex: a slim reader-writer lock, taken exclusively. */
int
media_os_mutex_init(
	struct media_os_mutex *mutex)
{
	SRWLOCK *lock;

	/* The lock in the mutex's room; making it cannot fail. */
	lock = media_os_win32_lock(mutex);
	InitializeSRWLock(lock);

	/* Succeeded: the mutex can be taken. */
	return 0;
}

/* Lets a mutex go. */
void
media_os_mutex_destroy(
	struct media_os_mutex *mutex)
{
	/* A slim reader-writer lock needs no ending. */
	(void)mutex;
}

/* Takes a mutex. */
void
media_os_mutex_lock(
	struct media_os_mutex *mutex)
{
	SRWLOCK *lock;

	/* Waits while another thread holds it. */
	lock = media_os_win32_lock(mutex);
	AcquireSRWLockExclusive(lock);
}

/* Gives back a mutex. */
void
media_os_mutex_unlock(
	struct media_os_mutex *mutex)
{
	SRWLOCK *lock;

	/* Another thread may take it now. */
	lock = media_os_win32_lock(mutex);
	ReleaseSRWLockExclusive(lock);
}

/* Makes a condition: a condition variable. */
int
media_os_cond_init(
	struct media_os_cond *cond)
{
	CONDITION_VARIABLE *condition;

	/* The condition variable in the condition's room; making it cannot fail. */
	condition = media_os_win32_condition(cond);
	InitializeConditionVariable(condition);

	/* Succeeded: the condition can be waited on. */
	return 0;
}

/* Lets a condition go. */
void
media_os_cond_destroy(
	struct media_os_cond *cond)
{
	/* A condition variable needs no ending. */
	(void)cond;
}

/*
 * Waits on a condition at most a number of milliseconds.
 *
 * The system counts the wait from now, so a change of the date does not
 * move it.
 */
void
media_os_cond_wait_for(
	struct media_os_cond *cond,
	struct media_os_mutex *mutex,
	unsigned milliseconds)
{
	CONDITION_VARIABLE *condition;
	SRWLOCK *lock;
	DWORD wait;

	/* The wait in milliseconds; INFINITE is not a wait of milliseconds, so one less. */
	wait = (DWORD)milliseconds;
	if (wait == INFINITE)
		wait = INFINITE - 1UL;

	/* Woken, timed out or for nothing, with the lock held again (taken exclusively: no flags). */
	condition = media_os_win32_condition(cond);
	lock = media_os_win32_lock(mutex);
	(void)SleepConditionVariableSRW(condition, lock, wait, 0UL);
}

/* Wakes every waiter of a condition. */
void
media_os_cond_broadcast(
	struct media_os_cond *cond)
{
	CONDITION_VARIABLE *condition;

	/* Every waiting thread runs again. */
	condition = media_os_win32_condition(cond);
	WakeAllConditionVariable(condition);
}

/*
 * Makes a call once in the process.
 *
 * MEDIA_OS_ONCE_INIT's null pointer is INIT_ONCE_STATIC_INIT, a call not
 * made yet.
 */
int
media_os_once(
	struct media_os_once *once,
	void (*run)(void))
{
	struct media_os_once_call call;
	INIT_ONCE *room;
	BOOL made;

	/* The function the system's callback runs. */
	call.run = run;

	/* Run by the first caller; the others wait for it to return. */
	room = (INIT_ONCE *)(void *)&once->room;
	made = InitOnceExecuteOnce(room, media_os_win32_once_run, &call, NULL);
	if (made == FALSE)
		return EIO;

	/* Succeeded: the call was made, by this caller or an earlier one. */
	return 0;
}

/*
 * Starts a thread.
 *
 * The thread is made with the C runtime's _beginthreadex (not
 * CreateThread), so that the runtime is set up for it.
 */
int
media_os_thread_start(
	struct media_os_thread *thread,
	void (*run)(void *argument),
	void *argument)
{
	uintptr_t created;

	/* What the thread runs, read by the thread itself. */
	thread->run = run;
	thread->argument = argument;
	thread->handle = NULL;

	/* The system's thread, with the default stack. */
	created = _beginthreadex(NULL, 0U, media_os_win32_thread_main, thread, 0U, NULL);
	if (created == 0U)
		return EAGAIN;

	/* Succeeded: the thread runs; its handle is waited on by the join. */
	thread->handle = (void *)created;
	return 0;
}

/* Waits for a thread to end and closes its handle. */
void
media_os_thread_join(
	struct media_os_thread *thread)
{
	/* No thread was started. */
	if (thread->handle == NULL)
		return;

	/* The thread's end, then its handle given back. */
	(void)WaitForSingleObject((HANDLE)thread->handle, INFINITE);
	(void)CloseHandle((HANDLE)thread->handle);
	thread->handle = NULL;
}

/*
 * Reads the monotonic clock in nanoseconds.
 *
 * The counter is turned into nanoseconds in whole seconds and the rest,
 * so that the product does not overflow 64 bits.
 */
uint64_t
media_os_clock_ns(void)
{
	LARGE_INTEGER frequency;
	LARGE_INTEGER counter;
	uint64_t ticks;
	uint64_t rate;
	uint64_t now;
	BOOL read;

	/* The counter's rate; one that cannot be read counts as zero. */
	read = QueryPerformanceFrequency(&frequency);
	if (read == FALSE || frequency.QuadPart <= 0)
		return 0U;

	/* The counter. */
	read = QueryPerformanceCounter(&counter);
	if (read == FALSE || counter.QuadPart < 0)
		return 0U;

	/* The counter in nanoseconds: the whole seconds, then the rest of a second. */
	ticks = (uint64_t)counter.QuadPart;
	rate = (uint64_t)frequency.QuadPart;
	now = (ticks / rate) * 1000000000ULL + (ticks % rate) * 1000000000ULL / rate;

	/* Succeeded: the time. */
	return now;
}

/* Sleeps a number of milliseconds. */
void
media_os_sleep_ms(
	unsigned milliseconds)
{
	DWORD wait;

	/* INFINITE is not a sleep of milliseconds, so one less. */
	wait = (DWORD)milliseconds;
	if (wait == INFINITE)
		wait = INFINITE - 1UL;

	/* The sleep. */
	Sleep(wait);
}

/*
 * Converts a UTF-8 path to a UTF-16 string allocated with malloc.
 *
 * Returns EILSEQ for bytes that are not UTF-8, ENAMETOOLONG for a path
 * longer than Windows names, ENOMEM and EIO.
 */
static int
media_os_win32_widen(
	const char *path,
	wchar_t **wide)
{
	wchar_t *converted;
	size_t length;
	DWORD code;
	int units;
	int written;

	/* Reports nothing unless the conversion succeeds below. */
	*wide = NULL;

	/* Refuses a path no Windows name can need, before sizing a buffer for it. */
	length = strlen(path);
	if (length > MEDIA_OS_PATH_BYTES)
		return ENAMETOOLONG;

	/* Counts the UTF-16 units of the path with its end, refusing bytes that are not UTF-8. */
	units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)length + 1, NULL, 0);
	if (units == 0) {
		code = GetLastError();
		if (code == ERROR_NO_UNICODE_TRANSLATION)
			return EILSEQ;

		/* Reports any other failure as a general error. */
		return EIO;
	}

	/* Refuses a path with more units than Windows names (the end is not counted). */
	if (units - 1 > MEDIA_OS_PATH_UNITS)
		return ENAMETOOLONG;

	/* Allocates the UTF-16 string. */
	converted = malloc((size_t)units * sizeof(*converted));
	if (converted == NULL)
		return ENOMEM;

	/* Converts the path. */
	written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, (int)length + 1, converted, units);
	if (written != units) {
		free(converted);
		return EIO;
	}

	/* Hands the string to the caller. */
	*wide = converted;

	/* Succeeded: the caller frees the string. */
	return 0;
}

/*
 * Turns the Windows error of a failed open into the error this interface reports.
 */
static int
media_os_win32_error(
	DWORD code)
{
	/* Groups Windows' reasons into the interface's few. */
	switch (code) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_BAD_NETPATH:
	case ERROR_DIRECTORY:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_FILENAME_EXCED_RANGE:
		return ENAMETOOLONG;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
	case ERROR_TOO_MANY_OPEN_FILES:
		return ENOMEM;
	default:
		break;
	}

	/* Anything else is a failure of the file system or the device. */
	return EIO;
}

/*
 * Tells whether a path names a directory.
 */
static int
media_os_win32_is_directory(
	const wchar_t *wide)
{
	DWORD attributes;

	/* Asks what the path names. */
	attributes = GetFileAttributesW(wide);
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return 0;

	/* A directory carries the directory attribute. */
	if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
		return 1;

	/* Anything else is a file. */
	return 0;
}

/*
 * Reads the size of an open handle and refuses anything that is not a disk
 * file (a device, a pipe).
 */
static int
media_os_win32_measure(
	HANDLE handle,
	uint64_t *size)
{
	LARGE_INTEGER length;
	DWORD type;
	BOOL succeeded;

	/* Only a disk file can be read at any offset. */
	type = GetFileType(handle);
	if (type != FILE_TYPE_DISK)
		return EIO;

	/* Asks the system for the size in bytes. */
	succeeded = GetFileSizeEx(handle, &length);
	if (!succeeded)
		return EIO;

	/* A negative size is no size. */
	if (length.QuadPart < 0)
		return EIO;

	/* Reports the size. */
	*size = (uint64_t)length.QuadPart;

	/* Succeeded: the size is known. */
	return 0;
}

/* The slim reader-writer lock in a mutex's room. */
static SRWLOCK *
media_os_win32_lock(
	struct media_os_mutex *mutex)
{
	/* The room itself. */
	return (SRWLOCK *)(void *)&mutex->room;
}

/* The condition variable in a condition's room. */
static CONDITION_VARIABLE *
media_os_win32_condition(
	struct media_os_cond *cond)
{
	/* The room itself. */
	return (CONDITION_VARIABLE *)(void *)&cond->room;
}

/* Runs a once's function (the system's callback for InitOnceExecuteOnce). */
static BOOL CALLBACK
media_os_win32_once_run(
	PINIT_ONCE once,
	PVOID parameter,
	PVOID *context)
{
	struct media_os_once_call *call;

	/* The system's own arguments are not needed. */
	(void)once;
	(void)context;

	/* The function media_os_once was given. */
	call = parameter;
	call->run();

	/* Succeeded: the once is made. */
	return TRUE;
}

/* Runs what a thread was started for (the C runtime's start routine). */
static unsigned __stdcall
media_os_win32_thread_main(
	void *argument)
{
	struct media_os_thread *thread;

	/* The record media_os_thread_start filled. */
	thread = argument;
	thread->run(thread->argument);

	/* Succeeded: the thread's exit code is 0. */
	return 0U;
}
