/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system services of the operating-system layer on POSIX systems.
 */

/*
 * pthread_setname_np is declared only when the C library is asked for its
 * GNU facilities; the build switch WB_OS_THREAD_NAMES says it is wanted.
 */
#if defined(WB_OS_THREAD_NAMES) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "base/os-system.h"
#include "base/base.h"

#include "include/kern/os-name.h"
#include "userland/desktop/paths.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The processor's name in navigator.platform, as the compiler targets it. */
#if defined(__x86_64__)
#define OS_MACHINE_NAME		"x86_64"
#elif defined(__aarch64__)
#define OS_MACHINE_NAME		"aarch64"
#elif defined(__riscv) && __riscv_xlen == 64
#define OS_MACHINE_NAME		"riscv64"
#elif defined(__i386__)
#define OS_MACHINE_NAME		"i686"
#else
#define OS_MACHINE_NAME		"unknown"
#endif

/* The largest alignment wb_os_aligned_alloc accepts, in bytes. */
#define OS_ALIGNMENT_MAX	65536U

/* The most bytes a thread name keeps (the limit of Linux's thread names, without the NUL). */
#define OS_THREAD_NAME_MAX	15U

/* The most bytes one read of the random device asks for, so a huge request is served in pieces. */
#define OS_RANDOM_PIECE		(1U << 20)

/* The name of the device that serves the system's random bytes. */
#define OS_RANDOM_DEVICE	"/dev/urandom"

/*
 * The value of navigator.platform: the operating system's name and the
 * processor, composed from the pieces at build time.
 */
static const char os_platform_name[] = ZEDBSD_OS_NAME " " OS_MACHINE_NAME;

/* The system part of the User-Agent that browsers on these systems send. */
static const char os_user_agent_system[] = "X11; Linux x86_64";

/*
 * Allocates memory whose address is a multiple of alignment.
 *
 * The block is released with wb_os_aligned_free.
 */
int
wb_os_aligned_alloc(
	size_t alignment,
	size_t size,
	void **memory)
{
	void *block;
	int status;

	/* Refuses a call that names no place for the block. */
	if (memory == NULL)
		return EINVAL;

	/* Leaves the output in its failure state until the allocation has worked. */
	*memory = NULL;

	/* Refuses an empty block. */
	if (size == 0)
		return EINVAL;

	/* Refuses an alignment below a pointer's or above the largest the layer offers. */
	if (alignment < sizeof(void *) || alignment > OS_ALIGNMENT_MAX)
		return EINVAL;

	/* Refuses an alignment that is not a power of two. */
	if ((alignment & (alignment - 1U)) != 0)
		return EINVAL;

	/* Allocates the block at the alignment. */
	block = NULL;
	status = posix_memalign(&block, alignment, size);
	if (status != 0) {
		if (status == ENOMEM)
			return ENOMEM;
		return EINVAL;
	}

	/* Hands the block to the caller. */
	*memory = block;

	/* Succeeded: the caller owns the block. */
	return 0;
}

/*
 * Frees a block from wb_os_aligned_alloc.
 */
void
wb_os_aligned_free(
	void *memory)
{
	/* A block from posix_memalign is released with free. */
	free(memory);
}

/*
 * Fills bytes with random bytes from the system's random device.
 *
 * Short reads and interrupted reads are retried; there is no fallback to a
 * predictable source.
 */
int
wb_os_random(
	void *bytes,
	size_t length)
{
	unsigned char *cursor;
	size_t left;
	size_t piece;
	ssize_t got;
	int descriptor;
	int flags;

	/* Does nothing for a request for no bytes, whatever the pointer is. */
	if (length == 0)
		return 0;

	/* Refuses a request for bytes with no place to put them. */
	if (bytes == NULL)
		return EINVAL;

	/* Opens the random device, closing it on exec where the system can. */
	flags = O_RDONLY;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	descriptor = open(OS_RANDOM_DEVICE, flags);
	if (descriptor < 0)
		return EIO;

	/* Reads until every byte is filled, a piece at a time. */
	cursor = (unsigned char *)bytes;
	left = length;
	while (left > 0) {
		piece = left;
		if (piece > OS_RANDOM_PIECE)
			piece = OS_RANDOM_PIECE;

		got = read(descriptor, cursor, piece);
		if (got < 0) {
			/* A signal interrupted the read; asks again. */
			if (errno == EINTR)
				continue;

			close(descriptor);
			return EIO;
		}
		if (got == 0) {
			/* The device ended, which a random source never does; refuses to go on. */
			close(descriptor);
			return EIO;
		}

		cursor += (size_t)got;
		left -= (size_t)got;
	}

	/* Releases the random device. */
	close(descriptor);

	/* Succeeded: every byte came from the system's generator. */
	return 0;
}

/*
 * Names the calling thread for debuggers and the system's tools.
 *
 * A build without WB_OS_THREAD_NAMES has no such facility and ignores the call.
 */
void
wb_os_thread_name(
	const char *name)
{
#if defined(WB_OS_THREAD_NAMES)
	char kept[OS_THREAD_NAME_MAX + 1U];
	size_t length;

	/* Ignores a call that gives no name. */
	if (name == NULL)
		return;

	/* Keeps at most the first bytes of the name, which is all the system stores. */
	length = 0;
	while (length < OS_THREAD_NAME_MAX && name[length] != '\0') {
		kept[length] = name[length];
		length++;
	}
	kept[length] = '\0';

	/* Names the calling thread; a failure leaves the thread unnamed, which is harmless. */
	(void)pthread_setname_np(pthread_self(), kept);
#else
	/* The facility is not built in, so the name is dropped. */
	UNUSED_PARAMETER(name);
#endif
}

/*
 * Loads the first library of a list that the system can load.
 *
 * Every name is checked before the first one is tried, so a bad list loads
 * nothing.
 */
int
wb_os_library_open(
	const char *const *names,
	struct wb_os_library **library)
{
	void *handle;
	size_t i;

	/* Leaves the output in its failure state until a library has loaded. */
	if (library != NULL)
		*library = NULL;

	/* Refuses a call that names no list or no place for the library. */
	if (names == NULL || library == NULL)
		return EINVAL;

	/* Refuses an empty list. */
	if (names[0] == NULL)
		return EINVAL;

	/* Refuses a list that holds an empty name. */
	for (i = 0; names[i] != NULL; i++) {
		if (names[i][0] == '\0')
			return EINVAL;
	}

	/* Tries the names in order and keeps the first library that loads. */
	for (i = 0; names[i] != NULL; i++) {
		handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		if (handle != NULL) {
			*library = (struct wb_os_library *)handle;
			return 0;
		}
	}

	/* No name in the list could be loaded. */
	return ENOENT;
}

/*
 * Looks up a function in a loaded library.
 */
wb_os_function
wb_os_library_symbol(
	struct wb_os_library *library,
	const char *name)
{
	void *address;
	wb_os_function function;

	/* Finds nothing for a call that lacks the library or the name. */
	if (library == NULL || name == NULL)
		return NULL;

	/* Looks the name up in the library. */
	address = dlsym((void *)library, name);
	if (address == NULL)
		return NULL;

	/*
	 * Turns the address into a function pointer by copying its bytes, the
	 * conversion POSIX describes for dlsym results.
	 */
	memcpy(&function, &address, sizeof(function));

	/* The function the library exports. */
	return function;
}

/*
 * Unloads a library.
 */
void
wb_os_library_close(
	struct wb_os_library *library)
{
	/* Does nothing for no library. */
	if (library == NULL)
		return;

	/* Releases the library's handle; the system unloads it with its last user. */
	dlclose((void *)library);
}

/*
 * Reports the value of navigator.platform.
 */
const char *
wb_os_platform_name(
	void)
{
	/* The static string composed from the operating system's name and the processor. */
	return os_platform_name;
}

/*
 * Reports the system part of the User-Agent.
 */
const char *
wb_os_user_agent_system(
	void)
{
	/* The static string browsers on these systems send. */
	return os_user_agent_system;
}

/*
 * Writes the directory of the bundled read-only data into buffer.
 */
int
wb_os_data_directory(
	char *buffer,
	size_t size)
{
	size_t length;

	/* Refuses a call that names no buffer or a buffer of no bytes. */
	if (buffer == NULL || size == 0)
		return EINVAL;

	/* Measures the directory the build installs the data in. */
	length = strlen(KEILAND_DATADIR);

	/* Reports a buffer too small for the directory and its NUL, leaving an empty string. */
	if (length >= size) {
		buffer[0] = '\0';
		return ERANGE;
	}

	/* Copies the directory with its NUL. */
	memcpy(buffer, KEILAND_DATADIR, length + 1U);

	/* Succeeded: the buffer holds the data directory. */
	return 0;
}

/*
 * Hands each certificate of the system's trust stores to each.
 *
 * On these systems the TLS library reads the system's certificate file
 * itself, so there is nothing to hand over.
 */
int
wb_os_trust_roots(
	wb_os_trust_each each,
	void *context)
{
	UNUSED_PARAMETER(context);

	/* Refuses a call that names no function to hand certificates to. */
	if (each == NULL)
		return EINVAL;

	/* Succeeded: every certificate (there is none to hand over) was handed over. */
	return 0;
}
