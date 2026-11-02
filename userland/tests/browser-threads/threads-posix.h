/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The table of threads (struct browser_threads of <browser/browser.h>)
 * over the POSIX threads, for the programs over libbrowser that have no
 * desktop of their own: the tests, the probes and the tools.  The browser
 * itself gives the engine libkeiland's threads (its shell's threads.c).
 */

#ifndef BROWSER_THREADS_POSIX_H
#define BROWSER_THREADS_POSIX_H

#include <browser/browser.h>

/*
 * Reports the table over the POSIX threads: constant, for the life of the
 * program.
 */
const struct browser_threads *browser_threads_posix(void);

/*
 * Counts the threads the table started and joined so far (the tests see
 * that every thread the engine started was given back).
 */
void browser_threads_posix_counts(unsigned long *started, unsigned long *joined);

#endif
