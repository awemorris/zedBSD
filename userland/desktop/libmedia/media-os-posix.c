/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What libmedia asks of a POSIX system: the Vulkan loader, reading media
 * files, and POSIX threads, mutexes, conditions and once calls.
 */

#include "media-os.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The most bytes one pread asks for; a larger request is read in pieces of this size. */
#define MEDIA_OS_READ_PIECE ((size_t)1 << 30)

/* The nanoseconds of a second, for adding a wait to the realtime clock. */
#define MEDIA_OS_NS_PER_SECOND 1000000000L

/* A media file open for reading: the descriptor that open returned. */
struct media_os_file {
	int descriptor;
};

/* The names under which the Vulkan loader may be installed, in the order they are tried. */
static const char *const media_os_vulkan_names[] = {
	"libvulkan.so.1",
	"libvulkan.so"
};

static int media_os_posix_error(int code);
static int media_os_posix_measure(int descriptor, uint64_t *size);
static void *media_os_posix_thread_main(void *argument);

/*
 * Loads the system's Vulkan loader.
 *
 * The loader handle itself is the result, so no memory is held for it and
 * the loader stays loaded for the life of the process.
 */
int
media_os_vulkan_open(
	struct media_os_library **library)
{
	void *handle;
	size_t index;
	size_t count;

	/* A caller with no room for the result broke the contract. */
	if (library == NULL)
		return EINVAL;

	/* Reports no loader unless one opens below. */
	*library = NULL;

	/* Tries each name of the loader until one of them loads. */
	handle = NULL;
	count = sizeof(media_os_vulkan_names) / sizeof(media_os_vulkan_names[0]);
	for (index = 0;
	     index < count;
	     index++) {
		handle = dlopen(media_os_vulkan_names[index], RTLD_NOW | RTLD_LOCAL);
		if (handle != NULL)
			break;
	}

	/* No name loaded: this system has no Vulkan loader. */
	if (handle == NULL)
		return ENODEV;

	/* Hands the loader to the caller. */
	*library = (struct media_os_library *)handle;

	/* Succeeded: the loader is loaded. */
	return 0;
}

/*
 * Finds a function in the loader.
 *
 * The address is moved into a function pointer byte by byte because a
 * data pointer cannot be converted to a function pointer in ISO C.
 */
media_os_function
media_os_library_symbol(
	struct media_os_library *library,
	const char *name)
{
	void *address;
	media_os_function function;

	/* A missing loader or name has no function to find. */
	if (library == NULL)
		return NULL;
	if (name == NULL)
		return NULL;

	/* Looks the name up in the loader. */
	address = dlsym((void *)library, name);
	if (address == NULL)
		return NULL;

	/* Turns the address into a function pointer. */
	memcpy(&function, &address, sizeof(function));

	/* Succeeded: the function the loader exports under the name. */
	return function;
}

/*
 * Opens a file for reading and measures it.
 *
 * A path is a string of bytes on this system, so it is passed on as it is
 * and never tested for UTF-8.
 */
int
media_os_file_open(
	const char *path,
	struct media_os_file **file,
	uint64_t *size)
{
	struct media_os_file *opened;
	uint64_t length;
	int descriptor;
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

	/* Allocates the file object. */
	opened = malloc(sizeof(*opened));
	if (opened == NULL)
		return ENOMEM;

	/* Opens the file, trying again when a signal interrupted the call. */
	do {
		descriptor = open(path, O_RDONLY | O_CLOEXEC);
	} while (descriptor < 0 && errno == EINTR);
	if (descriptor < 0) {
		error = media_os_posix_error(errno);
		free(opened);
		return error;
	}

	/* Measures the file and refuses what is not a regular file. */
	error = media_os_posix_measure(descriptor, &length);
	if (error != 0) {
		close(descriptor);
		free(opened);
		return error;
	}

	/* Hands the file and its size to the caller. */
	opened->descriptor = descriptor;
	*file = opened;
	*size = length;

	/* Succeeded: the file is open. */
	return 0;
}

/*
 * Reads exactly size bytes at offset without using the file position.
 *
 * A read the kernel cuts short is continued, and one a signal interrupted
 * is repeated.
 */
int
media_os_file_read_at(
	struct media_os_file *file,
	uint64_t offset,
	void *data,
	size_t size)
{
	unsigned char *bytes;
	uint64_t last;
	off_t position;
	ssize_t count;
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

	/*
	 * The last byte of the range must be an offset the system can name;
	 * a file never reaches one that it cannot, so such a range ends
	 * beyond the file.
	 */
	last = offset + (uint64_t)size - 1;
	position = (off_t)last;
	if (position < 0)
		return EIO;
	if ((uint64_t)position != last)
		return EIO;

	/* Reads the range piece by piece until every byte has arrived. */
	bytes = data;
	done = 0;
	while (done < size) {
		piece = size - done;
		if (piece > MEDIA_OS_READ_PIECE)
			piece = MEDIA_OS_READ_PIECE;

		/* Reads the next piece at the position after what was read. */
		count = pread(file->descriptor, bytes + done, piece, (off_t)(offset + (uint64_t)done));

		/* A signal interrupted the call before it read: repeats it. */
		if (count < 0 && errno == EINTR)
			continue;

		/* The read failed. */
		if (count < 0)
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

	/* Gives the descriptor back to the system and frees the object. */
	close(file->descriptor);
	free(file);
}

/* Closes a descriptor that came with a GPU picture. */
void
media_os_close_descriptor(
	int descriptor)
{
	/* A negative value is no descriptor. */
	if (descriptor < 0)
		return;

	/* Gives the descriptor back to the system. */
	close(descriptor);
}

/* Makes a mutex with the default attributes. */
int
media_os_mutex_init(
	struct media_os_mutex *mutex)
{
	int error;

	/* The system's mutex. */
	error = pthread_mutex_init(&mutex->mutex, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the mutex can be taken. */
	return 0;
}

/* Lets a mutex go. */
void
media_os_mutex_destroy(
	struct media_os_mutex *mutex)
{
	/* The system's mutex, held by nobody. */
	(void)pthread_mutex_destroy(&mutex->mutex);
}

/* Takes a mutex. */
void
media_os_mutex_lock(
	struct media_os_mutex *mutex)
{
	/* Waits while another thread holds it. */
	(void)pthread_mutex_lock(&mutex->mutex);
}

/* Gives back a mutex. */
void
media_os_mutex_unlock(
	struct media_os_mutex *mutex)
{
	/* Another thread may take it now. */
	(void)pthread_mutex_unlock(&mutex->mutex);
}

/* Makes a condition with the default attributes. */
int
media_os_cond_init(
	struct media_os_cond *cond)
{
	int error;

	/* The system's condition. */
	error = pthread_cond_init(&cond->cond, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the condition can be waited on. */
	return 0;
}

/* Lets a condition go. */
void
media_os_cond_destroy(
	struct media_os_cond *cond)
{
	/* The system's condition, waited on by nobody. */
	(void)pthread_cond_destroy(&cond->cond);
}

/*
 * Waits on a condition at most a number of milliseconds.
 *
 * The condition has the default clock, the realtime clock, so the end of
 * the wait is the realtime clock now plus the milliseconds.
 */
void
media_os_cond_wait_for(
	struct media_os_cond *cond,
	struct media_os_mutex *mutex,
	unsigned milliseconds)
{
	struct timespec until;

	/* The end of the wait on the condition's clock. */
	(void)clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += (time_t)(milliseconds / 1000U);
	until.tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
	if (until.tv_nsec >= MEDIA_OS_NS_PER_SECOND) {
		until.tv_sec++;
		until.tv_nsec -= MEDIA_OS_NS_PER_SECOND;
	}

	/* Woken, timed out or for nothing, with the mutex held again. */
	(void)pthread_cond_timedwait(&cond->cond, &mutex->mutex, &until);
}

/* Wakes every waiter of a condition. */
void
media_os_cond_broadcast(
	struct media_os_cond *cond)
{
	/* Every waiting thread runs again. */
	(void)pthread_cond_broadcast(&cond->cond);
}

/* Makes a call once in the process. */
int
media_os_once(
	struct media_os_once *once,
	void (*run)(void))
{
	int error;

	/* Run by the first caller; the others wait for it to return. */
	error = pthread_once(&once->once, run);
	if (error != 0)
		return error;

	/* Succeeded: the call was made, by this caller or an earlier one. */
	return 0;
}

/* Starts a thread with the default attributes. */
int
media_os_thread_start(
	struct media_os_thread *thread,
	void (*run)(void *argument),
	void *argument)
{
	int error;

	/* What the thread runs, read by the thread itself. */
	thread->run = run;
	thread->argument = argument;

	/* The system's thread. */
	error = pthread_create(&thread->thread, NULL, media_os_posix_thread_main, thread);
	if (error != 0)
		return error;

	/* Succeeded: the thread runs. */
	return 0;
}

/* Waits for a thread to end. */
void
media_os_thread_join(
	struct media_os_thread *thread)
{
	/* The thread's end; what it returns is nothing. */
	(void)pthread_join(thread->thread, NULL);
}

/* Reads the monotonic clock in nanoseconds. */
uint64_t
media_os_clock_ns(void)
{
	struct timespec now;
	int error;

	/* The clock; one that cannot be read counts as zero. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0U;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* Sleeps a number of milliseconds. */
void
media_os_sleep_ms(
	unsigned milliseconds)
{
	struct timespec wait;

	/* One sleep; a signal may end it early. */
	wait.tv_sec = (time_t)(milliseconds / 1000U);
	wait.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&wait, NULL);
}

/*
 * Turns the errno of a failed open into the error this interface reports.
 */
static int
media_os_posix_error(
	int code)
{
	/* Groups the system's reasons into the interface's few. */
	switch (code) {
	case ENOENT:
	case ENOTDIR:
		return ENOENT;
	case EACCES:
	case EPERM:
		return EACCES;
	case EISDIR:
		return EISDIR;
	case ENAMETOOLONG:
		return ENAMETOOLONG;
	case ENOMEM:
	case EMFILE:
	case ENFILE:
		return ENOMEM;
	default:
		break;
	}

	/* Anything else is a failure of the file system or the device. */
	return EIO;
}

/*
 * Reads the size of an open descriptor and refuses a directory or any
 * file that has no size to read at offsets (a device, a pipe).
 */
static int
media_os_posix_measure(
	int descriptor,
	uint64_t *size)
{
	struct stat status;
	int failed;

	/* Asks the system about the open file. */
	failed = fstat(descriptor, &status);
	if (failed != 0)
		return EIO;

	/* A directory can be opened for reading but holds no bytes to read. */
	if ((status.st_mode & S_IFMT) == S_IFDIR)
		return EISDIR;

	/* Only a regular file can be read at any offset. */
	if ((status.st_mode & S_IFMT) != S_IFREG)
		return EIO;

	/* A negative size is no size. */
	if (status.st_size < 0)
		return EIO;

	/* Reports the size. */
	*size = (uint64_t)status.st_size;

	/* Succeeded: the size is known. */
	return 0;
}

/* Runs what a thread was started for (the system's start routine). */
static void *
media_os_posix_thread_main(
	void *argument)
{
	struct media_os_thread *thread;

	/* The record media_os_thread_start filled. */
	thread = argument;
	thread->run(thread->argument);

	/* Succeeded: the thread returns nothing. */
	return NULL;
}
