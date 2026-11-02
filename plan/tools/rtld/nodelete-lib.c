/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One of the libraries of the dynamic loader's NODELETE test
 * (rtld-nodelete.c).  build-nodelete.sh compiles it three times:
 *
 *   libnodelete-keep.so   -DNODELETE_TAG='"keep"' -DNODELETE_DEP, linked
 *                         with -z nodelete and with libnodelete-dep.so
 *   libnodelete-flag.so   -DNODELETE_TAG='"flag"', opened with
 *                         RTLD_NODELETE
 *   libnodelete-plain.so  -DNODELETE_TAG='"plain"', the control, which
 *                         dlclose() unloads
 *
 * lib_register() registers a destructor of a thread's data and an atexit()
 * function, both in this library's code, which must still be mapped when
 * they run: when a thread that called lib_set() ends, after the last
 * dlclose(), and when the process exits.  Its destructor (fini) writes a
 * line, so the script sees when the loader finalized the library.
 */

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef NODELETE_TAG
#error NODELETE_TAG names the library in its lines
#endif
#ifndef NODELETE_VALUE
#error NODELETE_VALUE is what lib_value returns
#endif

int lib_value(void);
int lib_register(void);
int lib_set(void);
int lib_thread_count(void);
void *lib_dep_address(void);
#ifdef NODELETE_DEP
int nodelete_dep_value(void);
#endif

static void say(const char *what);
static void thread_done(void *value);
static void at_exit(void);
static void library_fini(void) __attribute__((destructor));

/* The key whose destructor is this library's thread_done; set by lib_register. */
static pthread_key_t thread_key;

/* The threads whose data thread_done destroyed; written by the ending threads. */
static int thread_count;

/* What lib_set stores, so the destructor of the key is called. */
static int thread_marker;

/*
 * Returns this library's value, through the dependency for keep.
 */
int
lib_value(
	void)
{
#ifdef NODELETE_DEP
	/* The dependency must stay mapped as long as this library. */
	return nodelete_dep_value() + NODELETE_VALUE;
#else
	/* The value the test expects of this library. */
	return NODELETE_VALUE;
#endif
}

/*
 * Registers the destructor of a thread's data and the atexit() function.
 */
int
lib_register(
	void)
{
	int error;

	/* Creates the key whose destructor runs this library's code. */
	error = pthread_key_create(&thread_key, thread_done);
	if (error != 0)
		return -1;

	/* Registers a function of this library to run when the process exits. */
	error = atexit(at_exit);
	if (error != 0)
		return -1;

	/* Both are registered. */
	return 0;
}

/*
 * Gives the calling thread data under the key, so its end runs thread_done.
 */
int
lib_set(
	void)
{
	int error;

	/* Stores the marker under the key. */
	error = pthread_setspecific(thread_key, &thread_marker);

	/* Reports whether the data was stored. */
	return error;
}

/*
 * Reports how many threads' data thread_done destroyed.
 */
int
lib_thread_count(
	void)
{
	int count;

	/* Reads the count the ending threads wrote. */
	count = __atomic_load_n(&thread_count, __ATOMIC_ACQUIRE);

	/* The threads counted. */
	return count;
}

/*
 * Reports the address of the dependency's function, or NULL without one.
 */
void *
lib_dep_address(
	void)
{
#ifdef NODELETE_DEP
	/* The function in libnodelete-dep.so. */
	return (void *)nodelete_dep_value;
#else
	/* This library has no dependency of its own. */
	return NULL;
#endif
}

/* Writes one line, unbuffered, so it keeps its place among the program's. */
static void
say(
	const char *what)
{
	size_t length;

	/* Counts the text. */
	length = 0;
	while (what[length] != '\0')
		length++;

	/* Writes the line. */
	(void)write(1, what, length);
}

/* Counts a thread whose data was destroyed. */
static void
thread_done(
	void *value)
{
	/* The marker is the only data stored under the key. */
	if (value != &thread_marker)
		return;

	/* One more thread ended with its data. */
	__atomic_add_fetch(&thread_count, 1, __ATOMIC_RELEASE);
}

/* Runs when the process exits. */
static void
at_exit(
	void)
{
	/* Says that this library's code was still mapped at exit. */
	say("RTLD-NODELETE atexit " NODELETE_TAG "\n");
}

/* The library's destructor, which the loader runs when it finalizes it. */
static void
library_fini(
	void)
{
	/* Says when the loader finalized this library. */
	say("RTLD-NODELETE fini " NODELETE_TAG "\n");
}
