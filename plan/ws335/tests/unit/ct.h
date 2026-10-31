/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant-time checks of the unit tests (run-ct.sh): a test marks a
 * secret input with CT_SECRET before the call and the outputs that may
 * depend on it with CT_PUBLIC after it.  Under Valgrind's memcheck, a
 * secret byte is "undefined", so any branch, loop bound or memory address
 * computed from it is reported as a use of an uninitialised value and
 * run-ct.sh fails; outside Valgrind the macros do nothing.
 *
 * A test compiled for run-ct.sh defines WS335_CT; run-unit.sh does not,
 * so the same test runs both ways.
 */

#ifndef WS335_TESTS_CT_H
#define WS335_TESTS_CT_H

/*
 * Procedure referenced: Adam Langley, "ctgrind" (checking constant-time
 * code with Valgrind by marking secrets undefined), done here with
 * memcheck's client requests.
 */
#ifdef WS335_CT
#include <valgrind/memcheck.h>
#define CT_SECRET(address, size) ((void)VALGRIND_MAKE_MEM_UNDEFINED((address), (size)))
#define CT_PUBLIC(address, size) ((void)VALGRIND_MAKE_MEM_DEFINED((address), (size)))
#else
#define CT_SECRET(address, size) ((void)(address), (void)(size))
#define CT_PUBLIC(address, size) ((void)(address), (void)(size))
#endif

#endif
