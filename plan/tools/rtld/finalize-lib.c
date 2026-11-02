/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The C part of a library of the dynamic loader's exit-handler test
 * (rtld-finalize.c).  build-finalize.sh compiles it, with finalize-cxx.cc,
 * twice:
 *
 *   libfinalize-plain.so  -DFINALIZE_TAG='"plain"', which dlclose()
 *                         unloads, so its exit handlers run then
 *   libfinalize-keep.so   -DFINALIZE_TAG='"keep"', linked with -z
 *                         nodelete, so its exit handlers run at exit
 *
 * Every handler and the library's destructor report to the program's
 * finalize_record() (or finalize_count()), which the program exports; the
 * code of each lies in this library, which must still be mapped when it
 * runs.  The file is compiled with -O2 so that lib_register_tail() calls
 * atexit() as a tail call; build-finalize.sh checks that it does.
 */

#include <pthread.h>
#include <stdlib.h>

#ifndef FINALIZE_TAG
#error FINALIZE_TAG names the library in its records
#endif

/* The number of threads lib_register_threads starts, and the registrations of each. */
#define FINALIZE_THREADS 4
#define FINALIZE_THREAD_REGISTRATIONS 8

int lib_register(void);
int lib_register_tail(void);
int lib_register_null(void);
int lib_register_threads(void);

/* The program's records of what ran (rtld-finalize.c). */
void finalize_record(const char *event);
void finalize_count(void);

/* The Itanium C++ ABI's registration, and this object's handle, which the linker defines. */
int __cxa_atexit(void (*destructor)(void *), void *object, void *handle);
extern char __dso_handle __attribute__((visibility("hidden")));

static void handler_atexit(void);
static void handler_object(void *object);
static void handler_tail(void);
static void handler_thread(void);
static void *thread_main(void *argument);
static void library_fini(void) __attribute__((destructor));

/* The objects the C++-form registrations pass to their destructor. */
static char object_with_handle[] = FINALIZE_TAG " cxa-handle";
static char object_without_handle[] = FINALIZE_TAG " cxa-null";

/*
 * Registers an atexit() function and a destructor in the C++ ABI's form
 * with this object's handle.
 */
int
lib_register(
	void)
{
	int error;

	/* A plain function. */
	error = atexit(handler_atexit);
	if (error != 0)
		return -1;

	/* A destructor and its object, under this object's __dso_handle. */
	error = __cxa_atexit(handler_object, object_with_handle, &__dso_handle);
	if (error != 0)
		return -1;

	/* Both are registered. */
	return 0;
}

/*
 * Registers an atexit() function through a tail call, so atexit() returns
 * straight to this function's caller, in another object.
 */
int
lib_register_tail(
	void)
{
	/* The call is the last thing this function does. */
	return atexit(handler_tail);
}

/*
 * Registers a destructor in the C++ ABI's form without a handle.
 */
int
lib_register_null(
	void)
{
	int error;

	/* The C library files it under the object of the destructor's code. */
	error = __cxa_atexit(handler_object, object_without_handle, NULL);

	/* Reports whether it was registered. */
	return error;
}

/*
 * Registers atexit() functions from several threads at once.
 */
int
lib_register_threads(
	void)
{
	pthread_t threads[FINALIZE_THREADS];
	void *result;
	int failed;
	int error;
	int i;

	/* Starts the threads, each of which registers its functions. */
	for (i = 0; i < FINALIZE_THREADS; i++) {
		error = pthread_create(&threads[i], NULL, thread_main, NULL);
		if (error != 0)
			return -1;
	}

	/* Waits for every thread and collects whether one failed. */
	failed = 0;
	for (i = 0; i < FINALIZE_THREADS; i++) {
		error = pthread_join(threads[i], &result);
		if (error != 0 || result != NULL)
			failed = 1;
	}

	/* Reports whether every registration was made. */
	if (failed)
		return -1;

	return 0;
}

/* The plain atexit() function. */
static void
handler_atexit(
	void)
{
	/* Records that it ran. */
	finalize_record(FINALIZE_TAG " atexit");
}

/* The destructor of the C++-form registrations; the object is its record. */
static void
handler_object(
	void *object)
{
	/* Records which registration ran. */
	finalize_record(object);
}

/* The atexit() function registered through a tail call. */
static void
handler_tail(
	void)
{
	/* Records that it ran. */
	finalize_record(FINALIZE_TAG " atexit-tail");
}

/* The atexit() function the threads register. */
static void
handler_thread(
	void)
{
	/* Counts one run. */
	finalize_count();
}

/* Registers a thread's functions; NULL when all of them were registered. */
static void *
thread_main(
	void *argument)
{
	int error;
	int i;

	/* Registers the same function several times, as the other threads do. */
	(void)argument;
	for (i = 0; i < FINALIZE_THREAD_REGISTRATIONS; i++) {
		error = atexit(handler_thread);
		if (error != 0)
			return (void *)1;
	}

	/* Every registration was made. */
	return NULL;
}

/* The library's destructor, which the loader runs after its exit handlers. */
static void
library_fini(
	void)
{
	/* Records when the loader finalized the library. */
	finalize_record(FINALIZE_TAG " fini");
}
